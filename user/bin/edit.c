#include <ulibc.h>

#define EDIT_MAX_LINES 128
#define EDIT_MAX_COLS  70
#define VIEW_ROWS      20

static char g_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1];
static int  g_num_lines = 1;
static int  g_cur_row = 0;
static int  g_cur_col = 0;
static int  g_row_off = 0;
static bool g_modified = false;
static char g_filepath[128] = "/tmp/notes.txt";
static char g_status[80] = "Welcome to OwnKrn Editor! Ctrl+S=Save, Ctrl+Q=Quit";

static void load_file(const char *path) {
    memset(g_lines, 0, sizeof(g_lines));
    g_num_lines = 1;
    g_cur_row = 0;
    g_cur_col = 0;
    g_row_off = 0;
    g_modified = false;

    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "[New File] %s", path);
        return;
    }

    char buf[512];
    ssize_t n;
    int r = 0, c = 0;
    size_t total_bytes = 0;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        total_bytes += (size_t)n;
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                g_lines[r][c] = '\0';
                if (r + 1 < EDIT_MAX_LINES) {
                    r++;
                    c = 0;
                }
            } else if ((unsigned char)ch >= 0x20 || ch == '\t') {
                if (ch == '\t') ch = ' ';
                if (c < EDIT_MAX_COLS) {
                    g_lines[r][c++] = ch;
                    g_lines[r][c] = '\0';
                }
            }
        }
    }
    u_close(fd);
    g_num_lines = r + 1;
    snprintf(g_status, sizeof(g_status), "Loaded %s (%zu bytes, %d lines)",
             path, total_bytes, g_num_lines);
}

static void save_file(void) {
    int fd = u_open(g_filepath, UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: Cannot write '%s' (err=%d)",
                 g_filepath, fd);
        return;
    }

    size_t total = 0;
    for (int i = 0; i < g_num_lines; i++) {
        size_t l = strlen(g_lines[i]);
        if (l > 0) {
            u_write(fd, g_lines[i], l);
            total += l;
        }
        if (i + 1 < g_num_lines || l > 0) {
            u_write(fd, "\n", 1);
            total += 1;
        }
    }
    u_close(fd);
    g_modified = false;
    snprintf(g_status, sizeof(g_status), "Saved %zu bytes to %s", total, g_filepath);
}

static void clamp_scroll(void) {
    size_t line_len = strlen(g_lines[g_cur_row]);
    if (g_cur_col > (int)line_len) {
        g_cur_col = (int)line_len;
    }
    if (g_cur_row < g_row_off) {
        g_row_off = g_cur_row;
    }
    if (g_cur_row >= g_row_off + VIEW_ROWS) {
        g_row_off = g_cur_row - VIEW_ROWS + 1;
    }
}

static void render_editor(void) {
    clamp_scroll();
    printf("\x1b[H");
    printf("\x1b[7m OwnKrn TUI Editor (edit/nano) | File: %-26s %s \x1b[0m\x1b[K\n",
           g_filepath, g_modified ? "[Modified]" : "          ");

    for (int v = 0; v < VIEW_ROWS; v++) {
        int idx = g_row_off + v;
        if (idx < g_num_lines) {
            printf("\x1b[1;36m%3d\x1b[0m | %s\x1b[K\n", idx + 1, g_lines[idx]);
        } else {
            printf("\x1b[1;34m  ~\x1b[0m |\x1b[K\n");
        }
    }

    printf("\x1b[1;33m %-56s Ln %d/%d, Col %d \x1b[0m\x1b[K\n",
           g_status, g_cur_row + 1, g_num_lines, g_cur_col + 1);
    printf("\x1b[7m ^S Save   ^Q Quit   ^K Cut Line   Arrows/Home/End/Del Navigate & Edit    \x1b[0m\x1b[K");

    /* Position terminal cursor inside viewport */
    int screen_row = (g_cur_row - g_row_off) + 2;
    int screen_col = g_cur_col + 7;
    printf("\x1b[%d;%dH", screen_row, screen_col);
}

static void insert_char(char ch) {
    char *line = g_lines[g_cur_row];
    size_t len = strlen(line);
    if (len >= EDIT_MAX_COLS) return;

    for (int i = (int)len + 1; i > g_cur_col; i--) {
        line[i] = line[i - 1];
    }
    line[g_cur_col] = ch;
    g_cur_col++;
    g_modified = true;
}

static void insert_newline(void) {
    if (g_num_lines >= EDIT_MAX_LINES) return;

    for (int r = g_num_lines; r > g_cur_row + 1; r--) {
        strcpy(g_lines[r], g_lines[r - 1]);
    }
    strcpy(g_lines[g_cur_row + 1], g_lines[g_cur_row] + g_cur_col);
    g_lines[g_cur_row][g_cur_col] = '\0';
    g_num_lines++;
    g_cur_row++;
    g_cur_col = 0;
    g_modified = true;
}

static void delete_char_at(int row, int col) {
    char *line = g_lines[row];
    size_t len = strlen(line);
    if (col < (int)len) {
        for (int i = col; i < (int)len; i++) {
            line[i] = line[i + 1];
        }
        g_modified = true;
    } else if (row + 1 < g_num_lines) {
        /* Join next line onto current line */
        size_t next_len = strlen(g_lines[row + 1]);
        size_t space = EDIT_MAX_COLS - len;
        if (next_len > space) next_len = space;
        memcpy(line + len, g_lines[row + 1], next_len);
        line[len + next_len] = '\0';

        for (int r = row + 1; r + 1 < g_num_lines; r++) {
            strcpy(g_lines[r], g_lines[r + 1]);
        }
        g_lines[g_num_lines - 1][0] = '\0';
        g_num_lines--;
        g_modified = true;
    }
}

static void cut_current_line(void) {
    if (g_num_lines == 1) {
        g_lines[0][0] = '\0';
        g_cur_col = 0;
        g_modified = true;
        return;
    }
    for (int r = g_cur_row; r + 1 < g_num_lines; r++) {
        strcpy(g_lines[r], g_lines[r + 1]);
    }
    g_lines[g_num_lines - 1][0] = '\0';
    g_num_lines--;
    if (g_cur_row >= g_num_lines) {
        g_cur_row = g_num_lines - 1;
    }
    g_cur_col = 0;
    g_modified = true;
    snprintf(g_status, sizeof(g_status), "Cut line %d", g_cur_row + 1);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("OwnKrn TUI Editor: usage: edit <filepath>\n");
            return 0;
        }
        strncpy(g_filepath, argv[1], sizeof(g_filepath) - 1);
        g_filepath[sizeof(g_filepath) - 1] = '\0';
    }

    load_file(g_filepath);
    u_tty_set_mode(TTY_MODE_RAW);
    printf("\x1b[2J\x1b[H");

    for (;;) {
        render_editor();

        char c = 0;
        if (u_read(0, &c, 1) <= 0) continue;

        if (c == 0x11 || c == 0x03) { /* Ctrl+Q or Ctrl+C */
            break;
        }
        if (c == 0x13) { /* Ctrl+S */
            save_file();
            continue;
        }
        if (c == 0x0B) { /* Ctrl+K */
            cut_current_line();
            continue;
        }
        if (c == '\r' || c == '\n') {
            insert_newline();
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            if (g_cur_col > 0) {
                g_cur_col--;
                delete_char_at(g_cur_row, g_cur_col);
            } else if (g_cur_row > 0) {
                g_cur_row--;
                g_cur_col = (int)strlen(g_lines[g_cur_row]);
                delete_char_at(g_cur_row, g_cur_col);
            }
            continue;
        }
        if (c == 0x1B) {
            char s1 = 0, s2 = 0;
            if (u_read(0, &s1, 1) <= 0) continue;
            if (s1 != '[' && s1 != 'O') continue;
            if (u_read(0, &s2, 1) <= 0) continue;

            if (s2 == 'A') { /* Up */
                if (g_cur_row > 0) g_cur_row--;
            } else if (s2 == 'B') { /* Down */
                if (g_cur_row + 1 < g_num_lines) g_cur_row++;
            } else if (s2 == 'C') { /* Right */
                int len = (int)strlen(g_lines[g_cur_row]);
                if (g_cur_col < len) g_cur_col++;
                else if (g_cur_row + 1 < g_num_lines) {
                    g_cur_row++;
                    g_cur_col = 0;
                }
            } else if (s2 == 'D') { /* Left */
                if (g_cur_col > 0) g_cur_col--;
                else if (g_cur_row > 0) {
                    g_cur_row--;
                    g_cur_col = (int)strlen(g_lines[g_cur_row]);
                }
            } else if (s2 == 'H') {
                g_cur_col = 0;
            } else if (s2 == 'F') {
                g_cur_col = (int)strlen(g_lines[g_cur_row]);
            } else if (s2 >= '1' && s2 <= '8') {
                char tilde = 0;
                u_read(0, &tilde, 1);
                if (s2 == '3' && tilde == '~') {
                    delete_char_at(g_cur_row, g_cur_col);
                } else if ((s2 == '1' || s2 == '7') && tilde == '~') {
                    g_cur_col = 0;
                } else if ((s2 == '4' || s2 == '8') && tilde == '~') {
                    g_cur_col = (int)strlen(g_lines[g_cur_row]);
                }
            }
            continue;
        }
        if (c == '\t') {
            insert_char(' ');
            insert_char(' ');
            continue;
        }
        if ((unsigned char)c >= 0x20) {
            insert_char(c);
        }
    }

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[2J\x1b[H\x1b[0m");
    return 0;
}
