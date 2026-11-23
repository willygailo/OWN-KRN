#include <ulibc.h>

#define EDIT_MAX_LINES 512
#define EDIT_MAX_COLS  120
#define VIEW_ROWS      28
#define CLIP_BUF_MAX   8192

static char g_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1];
static int  g_num_lines = 1;
static int  g_cur_row = 0;
static int  g_cur_col = 0;
static int  g_row_off = 0;
static bool g_modified = false;
static bool g_select_all = false;
static bool g_should_quit = false;
static char g_filepath[128] = "/tmp/notes.txt";
static char g_status[96] = "Shortcuts: Ctrl+Shift+V/Ctrl+V=Paste | Ctrl+C=Copy | Ctrl+S=Save | Ctrl+Q=Quit";
static char g_clip[CLIP_BUF_MAX];

/* 1-level Undo Snapshot for Ctrl+Z */
static char g_undo_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1];
static int  g_undo_num_lines = 0;
static int  g_undo_row = 0;
static int  g_undo_col = 0;
static bool g_has_undo = false;

static int  g_esc_state = 0; /* 0=normal, 1=ESC, 2=CSI */
static int  g_csi_val   = 0;

static void save_undo_snapshot(void) {
    memcpy(g_undo_lines, g_lines, sizeof(g_lines));
    g_undo_num_lines = g_num_lines;
    g_undo_row = g_cur_row;
    g_undo_col = g_cur_col;
    g_has_undo = true;
}

static void perform_undo(void) {
    if (!g_has_undo) {
        snprintf(g_status, sizeof(g_status), "Nothing to undo (Ctrl+Z)");
        return;
    }
    memcpy(g_lines, g_undo_lines, sizeof(g_lines));
    g_num_lines = g_undo_num_lines;
    g_cur_row = g_undo_row;
    g_cur_col = g_undo_col;
    g_has_undo = false;
    g_select_all = false;
    g_modified = true;
    snprintf(g_status, sizeof(g_status), "Undid last change (Ctrl+Z)");
}

static void sync_clip_to_dev(void) {
    int fd = u_open("/dev/clipboard", UAPI_O_WRONLY, 0);
    if (fd >= 0) {
        size_t len = strlen(g_clip);
        if (len > 0) {
            u_write(fd, g_clip, len);
        }
        u_close(fd);
    }
}

static size_t sync_clip_from_dev(void) {
    int fd = u_open("/dev/clipboard", UAPI_O_RDONLY, 0);
    if (fd < 0) {
        return strlen(g_clip);
    }
    char tmp[CLIP_BUF_MAX];
    size_t total = 0;
    while (total + 1 < sizeof(tmp)) {
        ssize_t n = u_read(fd, tmp + total, sizeof(tmp) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    u_close(fd);
    if (total > 0) {
        tmp[total] = '\0';
        memcpy(g_clip, tmp, total + 1);
    }
    return strlen(g_clip);
}

static bool has_exec_extension(const char *path) {
    size_t len = strlen(path);
    if (len >= 3 && strcmp(path + len - 3, ".sh") == 0) return true;
    if (len >= 3 && strcmp(path + len - 3, ".py") == 0) return true;
    return false;
}

static void load_file(const char *path) {
    memset(g_lines, 0, sizeof(g_lines));
    g_num_lines = 1;
    g_cur_row = 0;
    g_cur_col = 0;
    g_row_off = 0;
    g_modified = false;
    g_select_all = false;
    g_has_undo = false;

    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status),
                 "[New File] %s | Ctrl+Shift+V / Ctrl+V = Paste, Ctrl+S = Save", path);
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
                if (ch == '\t') {
                    for (int s = 0; s < 4 && c < EDIT_MAX_COLS; s++) {
                        g_lines[r][c++] = ' ';
                    }
                    g_lines[r][c] = '\0';
                } else if (c < EDIT_MAX_COLS) {
                    g_lines[r][c++] = ch;
                    g_lines[r][c] = '\0';
                }
            }
        }
    }
    u_close(fd);
    g_num_lines = r + 1;
    snprintf(g_status, sizeof(g_status),
             "Loaded %s (%zu B, %d lines) | Ctrl+Shift+V=Paste, Ctrl+S=Save",
             path, total_bytes, g_num_lines);
}

static void save_file(void) {
    uint32_t mode = has_exec_extension(g_filepath) ? 0755U : 0644U;
    int fd = u_open(g_filepath, UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, mode);
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
    if (has_exec_extension(g_filepath)) {
        u_chmod(g_filepath, 0755U);
    }
    g_modified = false;
    g_select_all = false;
    snprintf(g_status, sizeof(g_status), "Saved %zu bytes to %s (Ctrl+Q to Quit)",
             total, g_filepath);
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
    printf("\x1b[7m OwnKrn Editor (nano/edit) | File: %-28s %s \x1b[0m\x1b[K\n",
           g_filepath,
           g_select_all ? "[ALL SELECTED]" : (g_modified ? "[Modified]    " : "              "));

    for (int v = 0; v < VIEW_ROWS; v++) {
        int idx = g_row_off + v;
        if (idx < g_num_lines) {
            if (g_select_all) {
                printf("\x1b[1;36m%3d\x1b[0m | \x1b[7m%s\x1b[0m\x1b[K\n", idx + 1, g_lines[idx]);
            } else {
                printf("\x1b[1;36m%3d\x1b[0m | %s\x1b[K\n", idx + 1, g_lines[idx]);
            }
        } else {
            printf("\x1b[1;34m  ~\x1b[0m |\x1b[K\n");
        }
    }

    printf("\x1b[1;33m %-64s Ln %d/%d, Col %d \x1b[0m\x1b[K\n",
           g_status, g_cur_row + 1, g_num_lines, g_cur_col + 1);
    printf("\x1b[7m Ctrl+Shift+V/Ctrl+V Paste | Ctrl+C Copy | Ctrl+K Cut | Ctrl+A All | Ctrl+Z Undo | Ctrl+S Save | Ctrl+Q Quit \x1b[0m\x1b[K");

    /* Position terminal cursor inside viewport */
    int screen_row = (g_cur_row - g_row_off) + 2;
    int screen_col = g_cur_col + 7;
    printf("\x1b[%d;%dH", screen_row, screen_col);
}

static void clear_all_lines(void) {
    memset(g_lines, 0, sizeof(g_lines));
    g_num_lines = 1;
    g_cur_row = 0;
    g_cur_col = 0;
    g_row_off = 0;
    g_select_all = false;
    g_modified = true;
}

static void insert_char(char ch) {
    if (g_select_all) {
        save_undo_snapshot();
        clear_all_lines();
    }
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
    if (g_select_all) {
        save_undo_snapshot();
        clear_all_lines();
        return;
    }
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

static void copy_all_to_clip(void) {
    size_t pos = 0;
    for (int i = 0; i < g_num_lines && pos + 2 < sizeof(g_clip); i++) {
        size_t l = strlen(g_lines[i]);
        if (pos + l + 2 >= sizeof(g_clip)) {
            l = sizeof(g_clip) - 2 - pos;
        }
        memcpy(g_clip + pos, g_lines[i], l);
        pos += l;
        if (i + 1 < g_num_lines) {
            g_clip[pos++] = '\n';
        }
    }
    g_clip[pos] = '\0';
    sync_clip_to_dev();
}

static void copy_selection_or_line(void) {
    if (g_select_all) {
        copy_all_to_clip();
        g_select_all = false;
        snprintf(g_status, sizeof(g_status),
                 "Copied all %d lines (%zu B) to clipboard (Ctrl+V to paste)",
                 g_num_lines, strlen(g_clip));
        return;
    }
    snprintf(g_clip, sizeof(g_clip), "%s\n", g_lines[g_cur_row]);
    sync_clip_to_dev();
    snprintf(g_status, sizeof(g_status),
             "Copied line %d (%zu B) to clipboard (Ctrl+V to paste)",
             g_cur_row + 1, strlen(g_lines[g_cur_row]));
}

static void cut_selection_or_line(void) {
    save_undo_snapshot();
    if (g_select_all) {
        int cut_cnt = g_num_lines;
        copy_all_to_clip();
        clear_all_lines();
        snprintf(g_status, sizeof(g_status),
                 "Cut all %d lines to clipboard (Ctrl+V to paste, Ctrl+Z to undo)",
                 cut_cnt);
        return;
    }

    int cut_row = g_cur_row + 1;
    snprintf(g_clip, sizeof(g_clip), "%s\n", g_lines[g_cur_row]);
    sync_clip_to_dev();

    if (g_num_lines == 1) {
        g_lines[0][0] = '\0';
        g_cur_col = 0;
        g_modified = true;
        snprintf(g_status, sizeof(g_status), "Cut line 1 to clipboard (Ctrl+V to paste)");
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
    snprintf(g_status, sizeof(g_status), "Cut line %d to clipboard (Ctrl+V to paste)", cut_row);
}

static void paste_clipboard(void) {
    size_t len = sync_clip_from_dev();
    if (len == 0) {
        snprintf(g_status, sizeof(g_status),
                 "Clipboard is empty! Copy text on Host OS or press Ctrl+C first");
        return;
    }

    save_undo_snapshot();
    if (g_select_all) {
        clear_all_lines();
    }

    int lines_added = 0;
    size_t chars_added = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = g_clip[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            insert_newline();
            lines_added++;
            chars_added++;
        } else if (ch == '\t') {
            for (int s = 0; s < 4; s++) {
                insert_char(' ');
                chars_added++;
            }
        } else if ((unsigned char)ch >= 0x20) {
            insert_char(ch);
            chars_added++;
        }
    }
    snprintf(g_status, sizeof(g_status),
             "Pasted %zu chars (%d lines) from clipboard | Ctrl+S to Save",
             chars_added, lines_added + 1);
}

static void handle_csi_final(char final_ch, int param) {
    g_select_all = false;
    if (final_ch == 'A') { /* Up */
        if (g_cur_row > 0) g_cur_row--;
    } else if (final_ch == 'B') { /* Down */
        if (g_cur_row + 1 < g_num_lines) g_cur_row++;
    } else if (final_ch == 'C') { /* Right */
        int len = (int)strlen(g_lines[g_cur_row]);
        if (g_cur_col < len) g_cur_col++;
        else if (g_cur_row + 1 < g_num_lines) {
            g_cur_row++;
            g_cur_col = 0;
        }
    } else if (final_ch == 'D') { /* Left */
        if (g_cur_col > 0) g_cur_col--;
        else if (g_cur_row > 0) {
            g_cur_row--;
            g_cur_col = (int)strlen(g_lines[g_cur_row]);
        }
    } else if (final_ch == 'H') { /* Home */
        g_cur_col = 0;
    } else if (final_ch == 'F') { /* End */
        g_cur_col = (int)strlen(g_lines[g_cur_row]);
    } else if (final_ch == '~') {
        if (param == 2) { /* Insert key -> Paste */
            paste_clipboard();
        } else if (param == 3) { /* Delete */
            delete_char_at(g_cur_row, g_cur_col);
        } else if (param == 1 || param == 7) { /* Home */
            g_cur_col = 0;
        } else if (param == 4 || param == 8) { /* End */
            g_cur_col = (int)strlen(g_lines[g_cur_row]);
        } else if (param == 5) { /* Page Up */
            g_cur_row -= VIEW_ROWS;
            if (g_cur_row < 0) g_cur_row = 0;
        } else if (param == 6) { /* Page Down */
            g_cur_row += VIEW_ROWS;
            if (g_cur_row >= g_num_lines) g_cur_row = g_num_lines - 1;
        }
        /* param == 200 and param == 201 (Bracketed Paste start/end) are cleanly ignored */
    }
}

static void process_input_byte(char c) {
    if (g_esc_state == 1) {
        if (c == '[' || c == 'O') {
            g_esc_state = 2;
            g_csi_val = 0;
            return;
        }
        g_esc_state = 0;
    } else if (g_esc_state == 2) {
        if (c >= '0' && c <= '9') {
            g_csi_val = g_csi_val * 10 + (c - '0');
            return;
        }
        if (c == ';' || c == '?') {
            return;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
            handle_csi_final(c, g_csi_val);
            g_esc_state = 0;
            return;
        }
        g_esc_state = 0;
        return;
    }

    if (c == 0x1B) {
        g_esc_state = 1;
        return;
    }
    if (c == 0x11 || c == 0x18) { /* Ctrl+Q or Ctrl+X: Quit */
        g_should_quit = true;
        return;
    }
    if (c == 0x13 || c == 0x0F) { /* Ctrl+S or Ctrl+O: Save */
        save_file();
        return;
    }
    if (c == 0x01) { /* Ctrl+A: Select All */
        g_select_all = !g_select_all;
        if (g_select_all) {
            snprintf(g_status, sizeof(g_status),
                     "Selected all %d lines (Ctrl+C=Copy All, Ctrl+K=Cut All, Backspace=Clear)",
                     g_num_lines);
        } else {
            snprintf(g_status, sizeof(g_status), "Selection cleared");
        }
        return;
    }
    if (c == 0x03 || c == 0x19) { /* Ctrl+C / Ctrl+Shift+C / Ctrl+Y: Copy */
        copy_selection_or_line();
        return;
    }
    if (c == 0x0B) { /* Ctrl+K: Cut */
        cut_selection_or_line();
        return;
    }
    if (c == 0x16 || c == 0x15) { /* Ctrl+V / Ctrl+Shift+V / Ctrl+U: Paste */
        paste_clipboard();
        return;
    }
    if (c == 0x1A) { /* Ctrl+Z: Undo */
        perform_undo();
        return;
    }
    if (c == '\r' || c == '\n') {
        insert_newline();
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (g_select_all) {
            save_undo_snapshot();
            clear_all_lines();
            snprintf(g_status, sizeof(g_status), "Cleared all lines (Ctrl+Z to undo)");
            return;
        }
        if (g_cur_col > 0) {
            g_cur_col--;
            delete_char_at(g_cur_row, g_cur_col);
        } else if (g_cur_row > 0) {
            g_cur_row--;
            g_cur_col = (int)strlen(g_lines[g_cur_row]);
            delete_char_at(g_cur_row, g_cur_col);
        }
        return;
    }
    if (c == '\t') {
        for (int s = 0; s < 4; s++) {
            insert_char(' ');
        }
        return;
    }
    if ((unsigned char)c >= 0x20) {
        insert_char(c);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("OwnKrn TUI Editor: usage: edit <filepath>\n");
            printf("  Shortcuts: Ctrl+Shift+V/Ctrl+V=Paste, Ctrl+C=Copy, Ctrl+A=Select All,\n");
            printf("             Ctrl+K=Cut, Ctrl+Z=Undo, Ctrl+S=Save, Ctrl+Q/Ctrl+X=Quit\n");
            return 0;
        }
        strncpy(g_filepath, argv[1], sizeof(g_filepath) - 1);
        g_filepath[sizeof(g_filepath) - 1] = '\0';
    }

    g_clip[0] = '\0';
    load_file(g_filepath);
    u_tty_set_mode(TTY_MODE_RAW);
    printf("\x1b[2J\x1b[H");

    while (!g_should_quit) {
        render_editor();

        char first = 0;
        if (u_read(0, &first, 1) <= 0) {
            continue;
        }
        process_input_byte(first);

        /* Drain any remaining queued bytes (e.g. multi-line terminal paste) before redrawing */
        u_tty_set_mode(TTY_MODE_RAW | TTY_MODE_NONBLOCK);
        char batch[256];
        while (!g_should_quit) {
            ssize_t n = u_read(0, batch, sizeof(batch));
            if (n <= 0) break;
            for (ssize_t i = 0; i < n && !g_should_quit; i++) {
                process_input_byte(batch[i]);
            }
        }
        u_tty_set_mode(TTY_MODE_RAW);
    }

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[2J\x1b[H\x1b[0m");
    return 0;
}
