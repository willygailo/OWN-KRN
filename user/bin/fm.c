#include <ulibc.h>

#define FM_MAX_ENTS 64
#define FM_VIEW_ROWS 24

struct fm_item {
    char     name[64];
    uint32_t type;
    uint64_t ino;
    uint64_t size;
};

static struct fm_item g_items[FM_MAX_ENTS];
static int            g_count = 0;
static int            g_sel = 0;
static int            g_scroll = 0;
static char           g_cwd[256] = "/";
static char           g_status[96] = "Arrows=Select, Enter=Open/Edit, [c]=Copy to /disk, [n]=New, [d]=Del, [q]=Quit";

static void join_p(const char *dir, const char *name, char *out, size_t cap) {
    if (strcmp(dir, "/") == 0) {
        snprintf(out, cap, "/%s", name);
    } else {
        snprintf(out, cap, "%s/%s", dir, name);
    }
}

static void refresh_dir(void) {
    if (u_getcwd(g_cwd, sizeof(g_cwd)) <= 0) {
        strcpy(g_cwd, "/");
    }
    g_count = 0;
    int fd = u_open(g_cwd, UAPI_O_RDONLY, 0);
    if (fd < 0) return;

    struct uapi_dirent ents[32];
    int n;
    while ((n = u_getdents(fd, ents, 32)) > 0) {
        for (int i = 0; i < n && g_count < FM_MAX_ENTS; i++) {
            if (strcmp(ents[i].name, ".") == 0) continue;
            struct fm_item *it = &g_items[g_count++];
            strncpy(it->name, ents[i].name, sizeof(it->name) - 1);
            it->name[sizeof(it->name) - 1] = '\0';
            it->type = ents[i].type;
            it->ino  = ents[i].ino;
            it->size = 0;

            char full[256];
            join_p(g_cwd, it->name, full, sizeof(full));
            struct uapi_stat st;
            if (u_stat(full, &st) == 0) {
                it->size = st.size;
            }
        }
    }
    u_close(fd);

    if (g_sel >= g_count) g_sel = (g_count > 0) ? (g_count - 1) : 0;
    if (g_sel < 0) g_sel = 0;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + FM_VIEW_ROWS) g_scroll = g_sel - FM_VIEW_ROWS + 1;
}

static void format_preview_line(int row, char *out, size_t cap) {
    out[0] = '\0';
    if (g_count == 0) return;
    struct fm_item *it = &g_items[g_sel];
    char full[256];
    join_p(g_cwd, it->name, full, sizeof(full));

    if (row == 0) {
        snprintf(out, cap, "Selected : %s", full);
        return;
    }
    if (row == 1) {
        const char *t = (it->type == 2) ? "Directory" :
                        (it->type == 3) ? "Char Device" : "Regular File";
        snprintf(out, cap, "Type     : %-14s  Inode: %llu",
                 t, (unsigned long long)it->ino);
        return;
    }
    if (row == 2) {
        snprintf(out, cap, "Size     : %llu bytes", (unsigned long long)it->size);
        return;
    }
    if (row == 3) {
        snprintf(out, cap, "------------------------------------------------------------");
        return;
    }

    if (it->type == 2) {
        if (row == 4) {
            snprintf(out, cap, "[Directory - Press Enter to open '%s']", it->name);
        }
        return;
    }
    if (it->type == 3) {
        if (row == 4) {
            snprintf(out, cap, "[Character Device Node in /dev]");
        }
        return;
    }

    /* Preview file text lines (rows 4..20) */
    int fd = u_open(full, UAPI_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[1024];
    ssize_t n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int target_line = row - 4;
    int cur_line = 0;
    const char *p = buf;
    while (*p && cur_line < target_line) {
        if (*p++ == '\n') cur_line++;
    }
    if (cur_line == target_line && *p) {
        size_t i = 0;
        while (p[i] && p[i] != '\n' && p[i] != '\r' && i + 1 < cap && i < 58) {
            char ch = p[i];
            out[i] = ((unsigned char)ch >= 0x20 && (unsigned char)ch <= 0x7E) ? ch : '.';
            i++;
        }
        out[i] = '\0';
    }
}

static void render_fm(bool batch) {
    if (!batch) {
        printf("\x1b[H");
    }
    printf("\x1b[7m OwnKrn Dual-Pane File Manager (fm) | Dir: %-44s | Items: %-3d \x1b[0m\x1b[K\n",
           g_cwd, g_count);
    printf(" \x1b[1;36m%-6s %-26s %10s\x1b[0m | \x1b[1;33mFile / Directory Inspector\x1b[0m\x1b[K\n",
           "TYPE", "NAME", "SIZE");
    printf(" ---------------------------------------------+-------------------------------------------------------------\x1b[K\n");

    for (int r = 0; r < FM_VIEW_ROWS; r++) {
        int idx = g_scroll + r;
        char left[64];
        const char *lcolor = "\x1b[0m";

        if (idx < g_count) {
            struct fm_item *it = &g_items[idx];
            const char *tag = (it->type == 2) ? "<DIR>" :
                              (it->type == 3) ? "<DEV>" : "FILE ";
            if (idx == g_sel && !batch) {
                lcolor = "\x1b[7m";
            } else if (it->type == 2) {
                lcolor = "\x1b[1;34m";
            } else if (it->type == 3) {
                lcolor = "\x1b[1;33m";
            }
            snprintf(left, sizeof(left), " %-5s %-26s %8llu B ",
                     tag, it->name, (unsigned long long)it->size);
        } else {
            snprintf(left, sizeof(left), " %-43s ", "");
        }

        char right[64];
        format_preview_line(r, right, sizeof(right));
        printf(" %s%-44s\x1b[0m | %-60s\x1b[K\n", lcolor, left, right);
    }

    if (!batch) {
        printf(" ---------------------------------------------+-------------------------------------------------------------\x1b[K\n");
        printf("\x1b[1;32m %-108s\x1b[0m\x1b[K\n", g_status);
        printf("\x1b[7m [Up/Down] Select  [Enter] Open/Edit  [c] Copy->/disk  [n] New File  [m] Mkdir  [d] Delete  [q] Quit       \x1b[0m\x1b[K");
    }
}

static void prompt_name(const char *prompt, char *out, size_t cap) {
    printf("\r\x1b[K\x1b[1;33m%s: \x1b[0m", prompt);
    size_t len = 0;
    out[0] = '\0';
    for (;;) {
        char c = 0;
        if (u_read(0, &c, 1) <= 0) continue;
        if (c == 0x1B || c == 0x03) {
            out[0] = '\0';
            return;
        }
        if (c == '\r' || c == '\n') {
            return;
        }
        if ((c == '\b' || c == 0x7F) && len > 0) {
            len--;
            out[len] = '\0';
            u_write(1, "\b \b", 3);
        } else if ((unsigned char)c >= 0x20 && len + 1 < cap) {
            out[len++] = c;
            out[len] = '\0';
            u_write(1, &c, 1);
        }
    }
}

static void copy_selected_to_disk(void) {
    if (g_count == 0) return;
    struct fm_item *it = &g_items[g_sel];
    if (it->type != 1) {
        snprintf(g_status, sizeof(g_status), "Select a regular file to copy to /disk");
        return;
    }
    char src[256], dst[256];
    join_p(g_cwd, it->name, src, sizeof(src));
    snprintf(dst, sizeof(dst), "/disk/%s", it->name);

    int sfd = u_open(src, UAPI_O_RDONLY, 0);
    if (sfd < 0) {
        snprintf(g_status, sizeof(g_status), "Cannot open %s (%d)", src, sfd);
        return;
    }
    int dfd = u_open(dst, UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, 0644);
    if (dfd < 0) {
        u_close(sfd);
        snprintf(g_status, sizeof(g_status), "Cannot write %s (%d)", dst, dfd);
        return;
    }
    char buf[512];
    ssize_t n;
    size_t copied = 0;
    while ((n = u_read(sfd, buf, sizeof(buf))) > 0) {
        u_write(dfd, buf, (size_t)n);
        copied += (size_t)n;
    }
    u_close(sfd);
    u_close(dfd);
    snprintf(g_status, sizeof(g_status),
             "Copied %zu bytes to persistent %s!", copied, dst);
}

int main(int argc, char **argv) {
    bool batch = false;
    const char *start_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--batch") == 0) {
            batch = true;
        } else {
            start_dir = argv[i];
        }
    }
    if (start_dir) {
        u_chdir(start_dir);
    }

    refresh_dir();
    if (batch) {
        render_fm(true);
        return 0;
    }

    u_tty_set_mode(TTY_MODE_RAW);
    printf("\x1b[2J\x1b[H");

    for (;;) {
        render_fm(false);

        char c = 0;
        if (u_read(0, &c, 1) <= 0) continue;

        if (c == 'q' || c == 'Q' || c == 0x03) {
            break;
        }
        if (c == 'c' || c == 'C') {
            copy_selected_to_disk();
            refresh_dir();
            continue;
        }
        if (c == 'n' || c == 'N') {
            char name[48];
            prompt_name("New filename", name, sizeof(name));
            if (name[0]) {
                char full[256];
                join_p(g_cwd, name, full, sizeof(full));
                int fd = u_open(full, UAPI_O_CREAT | UAPI_O_WRONLY, 0644);
                if (fd >= 0) {
                    u_close(fd);
                    snprintf(g_status, sizeof(g_status), "Created %s", full);
                } else {
                    snprintf(g_status, sizeof(g_status), "Create failed (%d)", fd);
                }
            }
            refresh_dir();
            continue;
        }
        if (c == 'm' || c == 'M') {
            char name[48];
            prompt_name("New directory name", name, sizeof(name));
            if (name[0]) {
                char full[256];
                join_p(g_cwd, name, full, sizeof(full));
                int rc = u_mkdir(full, 0755);
                if (rc == 0) snprintf(g_status, sizeof(g_status), "Created dir %s", full);
                else snprintf(g_status, sizeof(g_status), "mkdir failed (%d)", rc);
            }
            refresh_dir();
            continue;
        }
        if (c == 'd' || c == 'D') {
            if (g_count > 0 && strcmp(g_items[g_sel].name, "..") != 0) {
                char full[256];
                join_p(g_cwd, g_items[g_sel].name, full, sizeof(full));
                int rc = u_unlink(full);
                if (rc == 0) snprintf(g_status, sizeof(g_status), "Deleted %s", full);
                else snprintf(g_status, sizeof(g_status), "Delete failed (%d)", rc);
                refresh_dir();
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (g_count == 0) continue;
            struct fm_item *it = &g_items[g_sel];
            char full[256];
            join_p(g_cwd, it->name, full, sizeof(full));
            if (it->type == 2) {
                u_chdir(full);
                g_sel = 0;
                g_scroll = 0;
                refresh_dir();
            } else if (it->type == 1) {
                pid_t pid = u_fork();
                if (pid == 0) {
                    char *eargv[] = { "/bin/edit", full, NULL };
                    u_execve("/bin/edit", eargv);
                    u_exit(127);
                } else if (pid > 0) {
                    int st = 0;
                    u_waitpid(pid, &st);
                    u_tty_set_mode(TTY_MODE_RAW);
                    printf("\x1b[2J\x1b[H");
                    refresh_dir();
                }
            }
            continue;
        }
        if (c == 0x1B) {
            char s1 = 0, s2 = 0;
            if (u_read(0, &s1, 1) <= 0 || (s1 != '[' && s1 != 'O')) continue;
            if (u_read(0, &s2, 1) <= 0) continue;
            if (s2 == 'A' && g_sel > 0) {
                g_sel--;
                if (g_sel < g_scroll) g_scroll = g_sel;
            } else if (s2 == 'B' && g_sel + 1 < g_count) {
                g_sel++;
                if (g_sel >= g_scroll + FM_VIEW_ROWS) g_scroll = g_sel - FM_VIEW_ROWS + 1;
            }
        }
    }

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[2J\x1b[H\x1b[0m");
    return 0;
}
