#include <ulibc.h>

#define EDIT_MAX_LINES 512
#define EDIT_MAX_COLS  120
#define VIEW_ROWS      28
#define CLIP_BUF_MAX   8192

enum file_type {
    FTYPE_TEXT   = 0,
    FTYPE_PYTHON = 1,
    FTYPE_SHELL  = 2,
    FTYPE_C      = 3,
};

static char g_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1];
static int  g_num_lines = 1;
static int  g_cur_row = 0;
static int  g_cur_col = 0;
static int  g_row_off = 0;
static bool g_modified = false;
static bool g_select_all = false;
static bool g_should_quit = false;
static enum file_type g_ftype = FTYPE_TEXT;
static char g_filepath[128] = "/tmp/notes.txt";
static char g_status[96] = "Ctrl+Shift+V/Ctrl+V=Paste | Ctrl+C=Copy | Ctrl+S=Save | Ctrl+R=Run | Ctrl+T=Template";
static char g_clip[CLIP_BUF_MAX];

/* 1-level Undo Snapshot for Ctrl+Z */
static char g_undo_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1];
static int  g_undo_num_lines = 0;
static int  g_undo_row = 0;
static int  g_undo_col = 0;
static bool g_has_undo = false;

static int  g_esc_state = 0; /* 0=normal, 1=ESC, 2=CSI */
static int  g_csi_val   = 0;

static enum file_type detect_file_type(const char *path) {
    size_t len = strlen(path);
    if (len >= 3 && strcmp(path + len - 3, ".py") == 0) return FTYPE_PYTHON;
    if (len >= 3 && strcmp(path + len - 3, ".sh") == 0) return FTYPE_SHELL;
    if (len >= 2 && (strcmp(path + len - 2, ".c") == 0 || strcmp(path + len - 2, ".h") == 0)) {
        return FTYPE_C;
    }
    return FTYPE_TEXT;
}

static const char *file_type_badge(enum file_type ft) {
    switch (ft) {
        case FTYPE_PYTHON: return "[PYTHON .py]";
        case FTYPE_SHELL:  return "[SHELL  .sh]";
        case FTYPE_C:      return "[C/C++ .c/h]";
        default:           return "[TEXT  .txt]";
    }
}

static bool has_exec_extension(const char *path) {
    size_t len = strlen(path);
    if (len >= 3 && strcmp(path + len - 3, ".sh") == 0) return true;
    if (len >= 3 && strcmp(path + len - 3, ".py") == 0) return true;
    return false;
}

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

static void load_file(const char *path) {
    memset(g_lines, 0, sizeof(g_lines));
    g_num_lines = 1;
    g_cur_row = 0;
    g_cur_col = 0;
    g_row_off = 0;
    g_modified = false;
    g_select_all = false;
    g_has_undo = false;
    g_ftype = detect_file_type(path);

    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status),
                 "[New %s] Ctrl+Shift+V=Paste | Ctrl+T=Starter Template | Ctrl+S=Save | Ctrl+R=Run",
                 file_type_badge(g_ftype));
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
             "Loaded %s (%zu B, %d lines) | Ctrl+Shift+V=Paste, Ctrl+S=Save, Ctrl+R=Run",
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
    snprintf(g_status, sizeof(g_status), "Saved %zu bytes to %s (Ctrl+R=Run, Ctrl+Q=Quit)",
             total, g_filepath);
}

/* ---------------- Live Multi-Language Syntax Highlighting ---------------- */

static bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool match_word_list(const char *w, size_t len, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if (strlen(list[i]) == len && strncmp(w, list[i], len) == 0) {
            return true;
        }
    }
    return false;
}

static void print_highlighted_line(const char *line) {
    static const char *const py_kw[] = {
        "def", "class", "import", "from", "as", "if", "elif", "else",
        "for", "while", "in", "return", "True", "False", "None",
        "and", "or", "not", "break", "continue", "pass", "try", "except",
        "with", "global", "lambda", NULL
    };
    static const char *const py_bi[] = {
        "print", "input", "range", "len", "int", "str", "list", "dict",
        "randint", "sleep", "random", "time", "sys", "os", NULL
    };
    static const char *const sh_kw[] = {
        "if", "then", "else", "elif", "fi", "for", "in", "do", "done",
        "while", "case", "esac", "function", "return", "exit", "export", NULL
    };
    static const char *const sh_bi[] = {
        "echo", "cat", "ls", "cd", "pwd", "mkdir", "rm", "cp", "mv",
        "touch", "chmod", "grep", "wc", "find", "sort", "uniq", "head",
        "tail", "sh", "python", "python3", "clip", "paste", "dmesg",
        "lspci", "lsblk", "tester", "fetch", "top", NULL
    };
    static const char *const c_kw[] = {
        "int", "char", "void", "bool", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "size_t", "ssize_t", "struct", "enum", "static",
        "const", "volatile", "if", "else", "for", "while", "return",
        "switch", "case", "break", "continue", "true", "false", "NULL", NULL
    };

    if (g_ftype == FTYPE_TEXT) {
        const char *trim = line;
        while (*trim == ' ') trim++;
        if (trim[0] == '#' || strncmp(trim, "===", 3) == 0 || strncmp(trim, "---", 3) == 0) {
            printf("\x1b[1;36m%s\x1b[0m", line);
        } else if (strncmp(trim, "- ", 2) == 0 || strncmp(trim, "* ", 2) == 0) {
            printf("\x1b[1;33m%s\x1b[0m", line);
        } else {
            printf("%s", line);
        }
        return;
    }

    size_t i = 0;
    while (line[i]) {
        char c = line[i];

        /* Comments: '#' in Python/Shell/C-preprocessor, '//' in C */
        if ((g_ftype == FTYPE_PYTHON || g_ftype == FTYPE_SHELL) && c == '#') {
            printf("\x1b[1;30m%s\x1b[0m", line + i);
            return;
        }
        if (g_ftype == FTYPE_C && c == '/' && line[i + 1] == '/') {
            printf("\x1b[1;30m%s\x1b[0m", line + i);
            return;
        }

        /* Strings: "..." or '...' */
        if (c == '"' || c == '\'') {
            char q = c;
            printf("\x1b[1;32m%c", c);
            i++;
            while (line[i]) {
                char sc = line[i++];
                printf("%c", sc);
                if (sc == q) break;
            }
            printf("\x1b[0m");
            continue;
        }

        /* Shell $VAR expansion */
        if (g_ftype == FTYPE_SHELL && c == '$') {
            printf("\x1b[1;33m$");
            i++;
            if (line[i] == '?' || line[i] == '$') {
                printf("%c\x1b[0m", line[i++]);
                continue;
            }
            while (is_ident_char(line[i])) {
                printf("%c", line[i++]);
            }
            printf("\x1b[0m");
            continue;
        }

        /* Numbers */
        if (c >= '0' && c <= '9' && (i == 0 || !is_ident_char(line[i - 1]))) {
            printf("\x1b[1;33m");
            while (is_ident_char(line[i])) {
                printf("%c", line[i++]);
            }
            printf("\x1b[0m");
            continue;
        }

        /* Identifiers & Keywords */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            size_t start = i;
            while (is_ident_char(line[i])) i++;
            size_t wlen = i - start;

            bool is_kw = false;
            bool is_bi = false;
            if (g_ftype == FTYPE_PYTHON) {
                is_kw = match_word_list(line + start, wlen, py_kw);
                is_bi = !is_kw && match_word_list(line + start, wlen, py_bi);
            } else if (g_ftype == FTYPE_SHELL) {
                is_kw = match_word_list(line + start, wlen, sh_kw);
                is_bi = !is_kw && match_word_list(line + start, wlen, sh_bi);
            } else if (g_ftype == FTYPE_C) {
                is_kw = match_word_list(line + start, wlen, c_kw);
            }

            if (is_kw) {
                printf("\x1b[1;35m");
            } else if (is_bi) {
                printf("\x1b[1;36m");
            }
            for (size_t k = 0; k < wlen; k++) {
                printf("%c", line[start + k]);
            }
            if (is_kw || is_bi) {
                printf("\x1b[0m");
            }
            continue;
        }

        printf("%c", c);
        i++;
    }
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
    printf("\x1b[7m OwnKrn Code Studio | %-12s | File: %-24s %s \x1b[0m\x1b[K\n",
           file_type_badge(g_ftype),
           g_filepath,
           g_select_all ? "[ALL SELECTED]" : (g_modified ? "[Modified]    " : "              "));

    for (int v = 0; v < VIEW_ROWS; v++) {
        int idx = g_row_off + v;
        if (idx < g_num_lines) {
            if (g_select_all) {
                printf("\x1b[1;36m%3d\x1b[0m | \x1b[7m%s\x1b[0m\x1b[K\n", idx + 1, g_lines[idx]);
            } else {
                printf("\x1b[1;36m%3d\x1b[0m | ", idx + 1);
                print_highlighted_line(g_lines[idx]);
                printf("\x1b[K\n");
            }
        } else {
            printf("\x1b[1;34m  ~\x1b[0m |\x1b[K\n");
        }
    }

    printf("\x1b[1;33m %-64s Ln %d/%d, Col %d \x1b[0m\x1b[K\n",
           g_status, g_cur_row + 1, g_num_lines, g_cur_col + 1);
    printf("\x1b[7m ^V Paste | ^C Copy | ^A All | ^Z Undo | ^S Save | ^R Run(.py/.sh) | ^T Template | ^F Find | ^Q Quit \x1b[0m\x1b[K");

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

static void insert_newline_raw(void) {
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

static void insert_newline_smart(void) {
    if (g_select_all) {
        save_undo_snapshot();
        clear_all_lines();
        return;
    }
    if (g_num_lines >= EDIT_MAX_LINES) return;

    /* Compute current line's leading spaces + smart block indent for .py / .sh / .c */
    const char *cur = g_lines[g_cur_row];
    int indent = 0;
    while (cur[indent] == ' ' && indent < g_cur_col && indent < 40) {
        indent++;
    }
    if (g_ftype == FTYPE_PYTHON || g_ftype == FTYPE_SHELL || g_ftype == FTYPE_C) {
        int check = g_cur_col - 1;
        while (check >= 0 && cur[check] == ' ') check--;
        if (check >= 0 && (cur[check] == ':' || cur[check] == '{')) {
            indent += 4;
        }
    }

    insert_newline_raw();
    for (int s = 0; s < indent && g_cur_col < EDIT_MAX_COLS; s++) {
        insert_char(' ');
    }
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

static void duplicate_current_line(void) {
    if (g_num_lines >= EDIT_MAX_LINES) return;
    save_undo_snapshot();
    for (int r = g_num_lines; r > g_cur_row + 1; r--) {
        strcpy(g_lines[r], g_lines[r - 1]);
    }
    strcpy(g_lines[g_cur_row + 1], g_lines[g_cur_row]);
    g_num_lines++;
    g_cur_row++;
    g_modified = true;
    snprintf(g_status, sizeof(g_status), "Duplicated line %d (Ctrl+D)", g_cur_row);
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

static void insert_raw_text_block(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            insert_newline_raw();
        } else if (ch == '\t') {
            for (int s = 0; s < 4; s++) {
                insert_char(' ');
            }
        } else if ((unsigned char)ch >= 0x20) {
            insert_char(ch);
        }
    }
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

    int before_lines = g_num_lines;
    insert_raw_text_block(g_clip, len);
    int added_lines = g_num_lines - before_lines + 1;
    snprintf(g_status, sizeof(g_status),
             "Pasted %zu chars (%d lines) from clipboard | Ctrl+S=Save, Ctrl+R=Run",
             len, added_lines);
}

static void insert_starter_template(void) {
    save_undo_snapshot();
    const char *tpl = NULL;
    if (g_ftype == FTYPE_PYTHON) {
        tpl = "# OwnKrn Python Script (.py)\n"
              "import random\n\n"
              "player = \"Willy\"\n"
              "score = random.randint(50, 100)\n"
              "print(f\"Welcome to OwnKrn Python, {player}!\")\n"
              "for level in range(1, 4):\n"
              "    print(f\"  Level {level} cleared! Score = {score}\")\n"
              "    score = score + 25\n"
              "print(f\"Final Score: {score}\")\n";
    } else if (g_ftype == FTYPE_SHELL) {
        tpl = "# OwnKrn Shell Script (.sh)\n"
              "echo \"=== OwnKrn System Check ===\"\n"
              "uname -a\n"
              "id\n"
              "df\n"
              "echo \"Done!\"\n";
    } else {
        tpl = "# OwnKrn Notes (.txt)\n"
              "=====================\n"
              "- Press Ctrl+Shift+V or Ctrl+V to paste from Host OS\n"
              "- Press Ctrl+S to save file\n"
              "- Press Ctrl+Q to exit editor\n";
    }
    clear_all_lines();
    insert_raw_text_block(tpl, strlen(tpl));
    snprintf(g_status, sizeof(g_status),
             "Inserted %s starter template! Press Ctrl+S to Save or Ctrl+R to Run",
             file_type_badge(g_ftype));
}

static void find_in_editor(void) {
    printf("\x1b[%d;1H\x1b[7m Find text (Enter to search, Esc to cancel): \x1b[0m\x1b[K",
           VIEW_ROWS + 2);
    char query[48];
    size_t qlen = 0;
    query[0] = '\0';

    for (;;) {
        char c = 0;
        if (u_read(0, &c, 1) <= 0) continue;
        if (c == 0x1B || c == 0x03) {
            snprintf(g_status, sizeof(g_status), "Search cancelled");
            return;
        }
        if (c == '\r' || c == '\n') break;
        if (c == '\b' || c == 0x7F) {
            if (qlen > 0) {
                query[--qlen] = '\0';
                printf("\b \b");
            }
            continue;
        }
        if ((unsigned char)c >= 0x20 && qlen + 1 < sizeof(query)) {
            query[qlen++] = c;
            query[qlen] = '\0';
            u_write(1, &c, 1);
        }
    }

    if (qlen == 0) {
        snprintf(g_status, sizeof(g_status), "Search cancelled");
        return;
    }

    for (int step = 0; step < g_num_lines; step++) {
        int r = (g_cur_row + step) % g_num_lines;
        int start_c = (step == 0) ? (g_cur_col + 1) : 0;
        if (start_c > (int)strlen(g_lines[r])) start_c = (int)strlen(g_lines[r]);
        const char *hit = strstr(g_lines[r] + start_c, query);
        if (hit) {
            g_cur_row = r;
            g_cur_col = (int)(hit - g_lines[r]);
            snprintf(g_status, sizeof(g_status), "Found '%s' at Ln %d, Col %d",
                     query, r + 1, g_cur_col + 1);
            return;
        }
    }
    snprintf(g_status, sizeof(g_status), "Not found: '%s'", query);
}

static void run_current_file(void) {
    save_file();

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[2J\x1b[H\x1b[0m");
    printf("\x1b[1;36m====================================================================\x1b[0m\n");
    printf("\x1b[1;32m  OwnKrn Code Studio Runner: %s %s\x1b[0m\n",
           file_type_badge(g_ftype), g_filepath);
    printf("\x1b[1;36m====================================================================\x1b[0m\n");

    if (g_ftype == FTYPE_PYTHON || g_ftype == FTYPE_SHELL) {
        pid_t pid = u_fork();
        if (pid == 0) {
            char *sh_argv[3] = { "/bin/sh", g_filepath, NULL };
            u_execve("/bin/sh", sh_argv);
            u_exit(127);
        } else if (pid > 0) {
            int st = 0;
            u_waitpid(pid, &st);
        }
    } else {
        size_t chars = 0;
        for (int i = 0; i < g_num_lines; i++) {
            chars += strlen(g_lines[i]);
            printf("%s\n", g_lines[i]);
        }
        printf("\n\x1b[1;33m[Document Stats: %d lines, %zu characters]\x1b[0m\n",
               g_num_lines, chars);
    }

    printf("\x1b[1;36m--------------------------------------------------------------------\x1b[0m\n");
    printf("\x1b[7m Execution finished. Press ENTER or SPACE to return to Code Studio \x1b[0m");
    u_tty_set_mode(TTY_MODE_RAW);
    for (;;) {
        char c = 0;
        if (u_read(0, &c, 1) > 0) {
            if (c == '\r' || c == '\n' || c == ' ' || c == 'q' || c == 0x1B) {
                break;
            }
        }
    }
    printf("\x1b[2J\x1b[H");
    snprintf(g_status, sizeof(g_status), "Returned from running %s", g_filepath);
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
    if (c == 0x12) { /* Ctrl+R: Save & Run (.py / .sh / .txt) */
        run_current_file();
        return;
    }
    if (c == 0x14) { /* Ctrl+T: Insert Starter Template */
        insert_starter_template();
        return;
    }
    if (c == 0x06 || c == 0x17) { /* Ctrl+F or Ctrl+W: Find Text */
        find_in_editor();
        return;
    }
    if (c == 0x04) { /* Ctrl+D: Duplicate Current Line */
        duplicate_current_line();
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
        insert_newline_smart();
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
            printf("OwnKrn Code Studio (code / nano / edit / micro): usage: code <filepath>\n");
            printf("  Supports: .py (Python), .sh (Shell), .c/.h (C), .txt (Text)\n");
            printf("  Shortcuts: Ctrl+Shift+V/Ctrl+V=Paste, Ctrl+C=Copy, Ctrl+A=Select All,\n");
            printf("             Ctrl+K=Cut, Ctrl+Z=Undo, Ctrl+S=Save, Ctrl+R=Save & Run,\n");
            printf("             Ctrl+T=Starter Template, Ctrl+F=Find, Ctrl+D=Duplicate, Ctrl+Q=Quit\n");
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
