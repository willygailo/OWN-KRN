#include <ulibc.h>

#define BOARD_W   38
#define BOARD_H   16
#define MAX_SNAKE 256

struct pt {
    int x;
    int y;
};

static struct pt g_snake[MAX_SNAKE];
static int       g_len = 4;
static int       g_dx = 1;
static int       g_dy = 0;
static struct pt g_food;
static int       g_score = 0;
static int       g_best = 0;
static bool      g_paused = false;
static bool      g_game_over = false;
static uint32_t  g_rng = 0x13579BDFU;

static uint32_t rand_next(void) {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x ? x : 0xCAFEBABEU;
    return g_rng;
}

static bool snake_contains(int x, int y) {
    for (int i = 0; i < g_len; i++) {
        if (g_snake[i].x == x && g_snake[i].y == y) return true;
    }
    return false;
}

static void spawn_food(void) {
    for (int tries = 0; tries < 200; tries++) {
        int fx = (int)(rand_next() % BOARD_W);
        int fy = (int)(rand_next() % BOARD_H);
        if (!snake_contains(fx, fy)) {
            g_food.x = fx;
            g_food.y = fy;
            return;
        }
    }
    g_food.x = 2;
    g_food.y = 2;
}

static void reset_game(void) {
    g_len = 4;
    int sx = BOARD_W / 2;
    int sy = BOARD_H / 2;
    for (int i = 0; i < g_len; i++) {
        g_snake[i].x = sx - i;
        g_snake[i].y = sy;
    }
    g_dx = 1;
    g_dy = 0;
    g_score = 0;
    g_paused = false;
    g_game_over = false;
    spawn_food();
}

static void draw_board(void) {
    printf("\x1b[H");
    printf("  \x1b[7m OwnKrn Arcade: SNAKE | Score: %-4d | High Score: %-4d \x1b[0m\x1b[K\n",
           g_score, g_best);

    printf("  \x1b[1;36m+");
    for (int x = 0; x < BOARD_W; x++) putchar('-');
    printf("+\x1b[0m\x1b[K\n");

    for (int y = 0; y < BOARD_H; y++) {
        char row[BOARD_W + 1];
        for (int x = 0; x < BOARD_W; x++) row[x] = ' ';
        row[BOARD_W] = '\0';

        row[g_food.x] = (g_food.y == y) ? '@' : row[g_food.x];
        for (int i = g_len - 1; i >= 0; i--) {
            if (g_snake[i].y == y && g_snake[i].x >= 0 && g_snake[i].x < BOARD_W) {
                row[g_snake[i].x] = (i == 0) ? 'O' : 'o';
            }
        }

        printf("  \x1b[1;36m|\x1b[0m");
        for (int x = 0; x < BOARD_W; x++) {
            char ch = row[x];
            if (ch == 'O')      printf("\x1b[1;33mO\x1b[0m");
            else if (ch == 'o') printf("\x1b[1;32mo\x1b[0m");
            else if (ch == '@') printf("\x1b[1;31m@\x1b[0m");
            else                putchar(' ');
        }
        printf("\x1b[1;36m|\x1b[0m\x1b[K\n");
    }

    printf("  \x1b[1;36m+");
    for (int x = 0; x < BOARD_W; x++) putchar('-');
    printf("+\x1b[0m\x1b[K\n");

    if (g_game_over) {
        printf("  \x1b[1;31mGAME OVER!\x1b[0m Press \x1b[1;32m[R]\x1b[0m to Restart or \x1b[1;33m[Q]\x1b[0m to Quit.\x1b[K\n");
    } else if (g_paused) {
        printf("  \x1b[1;33mPAUSED\x1b[0m - Press \x1b[1;32m[P]\x1b[0m to Resume or \x1b[1;33m[Q]\x1b[0m to Quit.\x1b[K\n");
    } else {
        printf("  Controls: \x1b[1;32mArrows / WASD\x1b[0m Move | \x1b[1;33m[P]\x1b[0m Pause | \x1b[1;31m[Q]\x1b[0m Quit\x1b[K\n");
    }
}

static void step_game(void) {
    if (g_paused || g_game_over) return;

    int nx = g_snake[0].x + g_dx;
    int ny = g_snake[0].y + g_dy;

    if (nx < 0 || nx >= BOARD_W || ny < 0 || ny >= BOARD_H || snake_contains(nx, ny)) {
        g_game_over = true;
        return;
    }

    bool ate = (nx == g_food.x && ny == g_food.y);
    if (ate && g_len < MAX_SNAKE) {
        g_len++;
        g_score += 10;
        if (g_score > g_best) g_best = g_score;
    }

    for (int i = g_len - 1; i > 0; i--) {
        g_snake[i] = g_snake[i - 1];
    }
    g_snake[0].x = nx;
    g_snake[0].y = ny;

    if (ate) {
        spawn_food();
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("OwnKrn Snake: controls: Arrow keys / WASD, P=Pause, R=Restart, Q=Quit\n");
        return 0;
    }

    struct timespec ts;
    if (u_clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        g_rng ^= (uint32_t)ts.tv_nsec ^ ((uint32_t)u_getpid() << 16);
    }

    reset_game();
    u_tty_set_mode(TTY_MODE_RAW | TTY_MODE_NONBLOCK);
    printf("\x1b[2J\x1b[H");

    bool quit = false;
    while (!quit) {
        draw_board();
        u_msleep(95);

        int next_dx = g_dx;
        int next_dy = g_dy;
        char c = 0;
        while (u_read(0, &c, 1) > 0) {
            if (c == 'q' || c == 'Q' || c == 0x03) {
                quit = true;
                break;
            }
            if (c == 'p' || c == 'P') {
                g_paused = !g_paused;
            } else if ((c == 'r' || c == 'R') && g_game_over) {
                reset_game();
                next_dx = g_dx;
                next_dy = g_dy;
            } else if ((c == 'w' || c == 'W') && g_dy == 0) {
                next_dx = 0; next_dy = -1;
            } else if ((c == 's' || c == 'S') && g_dy == 0) {
                next_dx = 0; next_dy = 1;
            } else if ((c == 'a' || c == 'A') && g_dx == 0) {
                next_dx = -1; next_dy = 0;
            } else if ((c == 'd' || c == 'D') && g_dx == 0) {
                next_dx = 1; next_dy = 0;
            } else if (c == 0x1B) {
                char s1 = 0, s2 = 0;
                if (u_read(0, &s1, 1) > 0 && (s1 == '[' || s1 == 'O') &&
                    u_read(0, &s2, 1) > 0) {
                    if (s2 == 'A' && g_dy == 0)      { next_dx = 0;  next_dy = -1; }
                    else if (s2 == 'B' && g_dy == 0) { next_dx = 0;  next_dy = 1;  }
                    else if (s2 == 'C' && g_dx == 0) { next_dx = 1;  next_dy = 0;  }
                    else if (s2 == 'D' && g_dx == 0) { next_dx = -1; next_dy = 0;  }
                }
            }
        }
        g_dx = next_dx;
        g_dy = next_dy;
        step_game();
    }

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[0m\nFinal Snake Score: %d (Best: %d)\n", g_score, g_best);
    return 0;
}
