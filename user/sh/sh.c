#include <ulibc.h>

#define SH_LINE_MAX   512
#define SH_MAX_ARGS   32
#define SH_MAX_STAGES 8
#define SH_HIST_MAX   32

static char g_history[SH_HIST_MAX][SH_LINE_MAX];
static int  g_hist_count = 0;
static char g_user[32] = "root";
static char g_host[32] = "ownkrn";

static void write_str_fd(int fd, const char *s) {
    u_write(fd, s, strlen(s));
}

static void history_add(const char *line) {
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    /* Avoid consecutive duplicate entries */
    if (g_hist_count > 0 &&
        strcmp(g_history[(g_hist_count - 1) % SH_HIST_MAX], line) == 0) {
        return;
    }
    strncpy(g_history[g_hist_count % SH_HIST_MAX], line, SH_LINE_MAX - 1);
    g_history[g_hist_count % SH_HIST_MAX][SH_LINE_MAX - 1] = '\0';
    g_hist_count++;
}

static void print_file_to_fd(const char *path, int out_fd) {
    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        printf("cat: cannot open '%s' (err=%d)\n", path, fd);
        return;
    }
    char buf[512];
    ssize_t n;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        u_write(out_fd, buf, (size_t)n);
    }
    u_close(fd);
}

static void copy_fd(int in_fd, int out_fd) {
    char buf[512];
    ssize_t n;
    while ((n = u_read(in_fd, buf, sizeof(buf))) > 0) {
        u_write(out_fd, buf, (size_t)n);
    }
}

static void cmd_help(int out_fd) {
    write_str_fd(out_fd,
        "\x1b[1;36mOwnKrn Interactive Shell (/bin/sh)\x1b[0m\n"
        "  \x1b[1;32mSystem & Info:\x1b[0m\n"
        "    fetch / neofetch    Display ASCII OS logo & live system telemetry\n"
        "    top / htop [-n 1]   Interactive real-time process & memory monitor\n"
        "    uname [-a]          Print kernel & machine info\n"
        "    cpuinfo / irqs      Inspect /proc/cpuinfo & /proc/interrupts\n"
        "    ps / free / uptime  Inspect /proc/tasks, meminfo, uptime\n"
        "    date                Display CMOS RTC date & UTC epoch\n"
        "    whoami / hostname   Show or set active user / hostname\n"
        "    history             Display command history (Up/Down arrows recall)\n"
        "  \x1b[1;32mFiles & Navigation:\x1b[0m\n"
        "    pwd / cd [dir]      Print or change working directory\n"
        "    ls [-l] [dir]       List directory entries (colorized + sizes)\n"
        "    tree [dir]          Render visual directory hierarchy\n"
        "    stat <path>         Inspect inode, type, mode, and byte size\n"
        "    cat / head / tail   Display full, first N, or last N lines\n"
        "    cp / mv <src> <dst> Copy or move/rename files & directories\n"
        "    edit / nano <file>  Full-screen interactive TUI text editor\n"
        "    mkdir / touch / rm  Create or remove files & directories\n"
        "    sh <script.sh>      Execute a shell script file\n"
        "  \x1b[1;32mUtilities & Games:\x1b[0m\n"
        "    snake               Playable terminal Snake game in Ring 3\n"
        "    grep / wc / hexdump Stream filter, line/word/byte count, hex viewer\n"
        "    kill <pid> [sig]    Send POSIX signal (default SIGTERM=15)\n"
        "    selftest / faulttest Run Ring 3 kernel verification suite\n"
        "    clear / halt / exit Clear screen, power off, or exit shell\n"
        "  \x1b[1;33mShortcuts:\x1b[0m Up/Down=History, Left/Right/Home/End=Edit, Tab=Autocomplete, Ctrl+L=Clear\n");
}

static void join_path(const char *dir, const char *name, char *out, size_t cap) {
    if (strcmp(dir, "/") == 0) {
        snprintf(out, cap, "/%s", name);
    } else {
        snprintf(out, cap, "%s/%s", dir, name);
    }
}

static void cmd_ls(int argc, char **argv, int out_fd) {
    bool long_fmt = false;
    const char *dir = ".";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-la") == 0 ||
            strcmp(argv[i], "-al") == 0) {
            long_fmt = true;
        } else {
            dir = argv[i];
        }
    }

    int fd = u_open(dir, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot open '%s' (err=%d)\n", dir, fd);
        return;
    }

    struct uapi_dirent ents[32];
    int n;
    while ((n = u_getdents(fd, ents, 32)) > 0) {
        for (int i = 0; i < n; i++) {
            char full[256];
            join_path(dir, ents[i].name, full, sizeof(full));
            struct uapi_stat st;
            uint64_t sz = 0;
            if (u_stat(full, &st) == 0) {
                sz = st.size;
            }

            const char *color = "\x1b[0m";
            const char *tag   = "<FILE>";
            const char *perm  = "-rw-r--r--";
            if (ents[i].type == 2) {
                color = "\x1b[1;34m";
                tag   = "<DIR> ";
                perm  = "drwxr-xr-x";
            } else if (ents[i].type == 3) {
                color = "\x1b[1;33m";
                tag   = "<DEV> ";
                perm  = "crw-rw-rw-";
            } else if (strncmp(dir, "/bin", 4) == 0 || strcmp(full, "/bin") == 0) {
                color = "\x1b[1;32m";
                perm  = "-rwxr-xr-x";
            }

            char line[256];
            if (long_fmt) {
                snprintf(line, sizeof(line), "  %s  ino=%-4llu %8llu B  %s%s%s\x1b[0m\n",
                         perm,
                         (unsigned long long)ents[i].ino,
                         (unsigned long long)sz,
                         color,
                         ents[i].name,
                         (ents[i].type == 2) ? "/" : "");
            } else {
                snprintf(line, sizeof(line), "  %s  %s%s%s\x1b[0m\n",
                         tag, color, ents[i].name,
                         (ents[i].type == 2) ? "/" : "");
            }
            write_str_fd(out_fd, line);
        }
    }
    u_close(fd);
}

static void tree_walk(const char *path, const char *prefix, int depth,
                      int *dirs_out, int *files_out, int out_fd) {
    if (depth > 4) return;
    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) return;

    struct uapi_dirent ents[32];
    int total = 0;
    struct uapi_dirent list[32];
    int n;
    while ((n = u_getdents(fd, ents, 32)) > 0) {
        for (int i = 0; i < n && total < 32; i++) {
            if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0) {
                continue;
            }
            list[total++] = ents[i];
        }
    }
    u_close(fd);

    for (int i = 0; i < total; i++) {
        bool last = (i == total - 1);
        const char *branch = last ? "`-- " : "|-- ";
        const char *color = (list[i].type == 2) ? "\x1b[1;34m" :
                            (list[i].type == 3) ? "\x1b[1;33m" : "\x1b[0m";
        char line[256];
        snprintf(line, sizeof(line), "%s%s%s%s%s\x1b[0m\n",
                 prefix, branch, color, list[i].name,
                 (list[i].type == 2) ? "/" : "");
        write_str_fd(out_fd, line);

        if (list[i].type == 2) {
            (*dirs_out)++;
            char sub[256];
            char next_pref[128];
            join_path(path, list[i].name, sub, sizeof(sub));
            snprintf(next_pref, sizeof(next_pref), "%s%s",
                     prefix, last ? "    " : "|   ");
            tree_walk(sub, next_pref, depth + 1, dirs_out, files_out, out_fd);
        } else {
            (*files_out)++;
        }
    }
}

static void cmd_tree(int argc, char **argv, int out_fd) {
    const char *root = (argc > 1) ? argv[1] : ".";
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "\x1b[1;34m%s\x1b[0m\n", root);
    write_str_fd(out_fd, hdr);

    int dirs = 0, files = 0;
    tree_walk(root, "", 0, &dirs, &files, out_fd);

    char sum[128];
    snprintf(sum, sizeof(sum), "\n%d directories, %d files\n", dirs, files);
    write_str_fd(out_fd, sum);
}

static void cmd_stat(int argc, char **argv, int out_fd) {
    if (argc < 2) {
        printf("usage: stat <path>\n");
        return;
    }
    struct uapi_stat st;
    int rc = u_stat(argv[1], &st);
    if (rc < 0) {
        printf("stat: cannot stat '%s' (err=%d)\n", argv[1], rc);
        return;
    }
    const char *tstr = (st.type == 2) ? "directory" :
                       (st.type == 3) ? "character device" : "regular file";
    char buf[256];
    snprintf(buf, sizeof(buf),
             "  File: %s\n"
             "  Size: %-10llu  Type: %s\n"
             " Inode: %-10llu  Mode: 0%x\n",
             argv[1],
             (unsigned long long)st.size,
             tstr,
             (unsigned long long)st.ino,
             st.mode);
    write_str_fd(out_fd, buf);
}

static void cmd_history(int out_fd) {
    int start = (g_hist_count > SH_HIST_MAX) ? (g_hist_count - SH_HIST_MAX) : 0;
    for (int i = start; i < g_hist_count; i++) {
        char line[SH_LINE_MAX + 32];
        snprintf(line, sizeof(line), "  %3d  %s\n",
                 i + 1, g_history[i % SH_HIST_MAX]);
        write_str_fd(out_fd, line);
    }
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: cp <src> <dst>\n");
        return;
    }
    int sfd = u_open(argv[1], UAPI_O_RDONLY, 0);
    if (sfd < 0) {
        printf("cp: cannot open '%s' (err=%d)\n", argv[1], sfd);
        return;
    }
    int dfd = u_open(argv[2], UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, 0644);
    if (dfd < 0) {
        printf("cp: cannot create '%s' (err=%d)\n", argv[2], dfd);
        u_close(sfd);
        return;
    }
    char buf[512];
    ssize_t n;
    while ((n = u_read(sfd, buf, sizeof(buf))) > 0) {
        u_write(dfd, buf, (size_t)n);
    }
    u_close(sfd);
    u_close(dfd);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: mv <src> <dst>\n");
        return;
    }
    int rc = u_rename(argv[1], argv[2]);
    if (rc < 0) {
        printf("mv: cannot move '%s' to '%s' (err=%d)\n", argv[1], argv[2], rc);
    }
}

static void cmd_head_tail(int argc, char **argv, int in_fd, int out_fd, bool is_tail) {
    int nlines = 10;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nlines = atoi(argv[++i]);
            if (nlines < 1) nlines = 1;
        } else {
            path = argv[i];
        }
    }

    int fd = in_fd;
    if (path) {
        fd = u_open(path, UAPI_O_RDONLY, 0);
        if (fd < 0) {
            printf("%s: cannot open '%s' (err=%d)\n",
                   is_tail ? "tail" : "head", path, fd);
            return;
        }
    }

    char data[2048];
    size_t total = 0;
    ssize_t r;
    while (total + 1 < sizeof(data) &&
           (r = u_read(fd, data + total, sizeof(data) - 1 - total)) > 0) {
        total += (size_t)r;
    }
    data[total] = '\0';
    if (path) u_close(fd);

    if (!is_tail) {
        int seen = 0;
        size_t end = 0;
        while (end < total && seen < nlines) {
            if (data[end++] == '\n') seen++;
        }
        if (end > 0) u_write(out_fd, data, end);
    } else {
        int seen = 0;
        size_t pos = total;
        if (pos > 0 && data[pos - 1] == '\n') pos--;
        while (pos > 0) {
            if (data[pos - 1] == '\n') {
                seen++;
                if (seen >= nlines) break;
            }
            pos--;
        }
        if (total > pos) u_write(out_fd, data + pos, total - pos);
    }
}

static void execute_line(char *line);

static int run_script_file(const char *path) {
    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        printf("sh: cannot open script '%s' (err=%d)\n", path, fd);
        return 1;
    }
    char content[2048];
    size_t total = 0;
    ssize_t n;
    while (total + 1 < sizeof(content) &&
           (n = u_read(fd, content + total, sizeof(content) - 1 - total)) > 0) {
        total += (size_t)n;
    }
    content[total] = '\0';
    u_close(fd);

    char *p = content;
    while (*p) {
        char *nl = strchr(p, '\n');
        char line[SH_LINE_MAX];
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = '\0';
        execute_line(line);
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static void cmd_uname(int argc, char **argv, int out_fd) {
    struct utsname u;
    if (u_uname(&u) < 0) {
        printf("uname: syscall failed\n");
        return;
    }
    char line[384];
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        snprintf(line, sizeof(line), "%s %s %s %s %s\n",
                 u.sysname, u.nodename, u.release, u.version, u.machine);
    } else {
        snprintf(line, sizeof(line), "%s\n", u.sysname);
    }
    write_str_fd(out_fd, line);
}

static bool is_stateful_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 ||
           strcmp(cmd, "hostname") == 0 ||
           strcmp(cmd, "source") == 0 ||
           strcmp(cmd, "exit") == 0 ||
           strcmp(cmd, "halt") == 0 ||
           strcmp(cmd, "poweroff") == 0 ||
           strcmp(cmd, "reboot") == 0;
}

static bool run_builtin(int argc, char **argv, int in_fd, int out_fd) {
    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0) {
        cmd_help(out_fd);
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname(argc, argv, out_fd);
    } else if (strcmp(cmd, "pwd") == 0) {
        char cwd[256];
        if (u_getcwd(cwd, sizeof(cwd)) > 0) {
            write_str_fd(out_fd, cwd);
            write_str_fd(out_fd, "\n");
        }
    } else if (strcmp(cmd, "cd") == 0) {
        const char *target = (argc > 1) ? argv[1] : "/";
        int rc = u_chdir(target);
        if (rc < 0) printf("cd: %s: error %d\n", target, rc);
    } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "ll") == 0) {
        if (strcmp(cmd, "ll") == 0) {
            char *ll_argv[4];
            int ll_argc = 0;
            ll_argv[ll_argc++] = "ls";
            ll_argv[ll_argc++] = "-l";
            if (argc > 1) ll_argv[ll_argc++] = argv[1];
            ll_argv[ll_argc] = NULL;
            cmd_ls(ll_argc, ll_argv, out_fd);
        } else {
            cmd_ls(argc, argv, out_fd);
        }
    } else if (strcmp(cmd, "tree") == 0) {
        cmd_tree(argc, argv, out_fd);
    } else if (strcmp(cmd, "stat") == 0) {
        cmd_stat(argc, argv, out_fd);
    } else if (strcmp(cmd, "history") == 0) {
        cmd_history(out_fd);
    } else if (strcmp(cmd, "whoami") == 0) {
        write_str_fd(out_fd, g_user);
        write_str_fd(out_fd, "\n");
    } else if (strcmp(cmd, "hostname") == 0) {
        if (argc > 1) {
            strncpy(g_host, argv[1], sizeof(g_host) - 1);
            g_host[sizeof(g_host) - 1] = '\0';
        } else {
            write_str_fd(out_fd, g_host);
            write_str_fd(out_fd, "\n");
        }
    } else if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            copy_fd(in_fd, out_fd);
        } else {
            for (int i = 1; i < argc; i++) {
                print_file_to_fd(argv[i], out_fd);
            }
        }
    } else if (strcmp(cmd, "head") == 0) {
        cmd_head_tail(argc, argv, in_fd, out_fd, false);
    } else if (strcmp(cmd, "tail") == 0) {
        cmd_head_tail(argc, argv, in_fd, out_fd, true);
    } else if (strcmp(cmd, "cp") == 0) {
        cmd_cp(argc, argv);
    } else if (strcmp(cmd, "mv") == 0) {
        cmd_mv(argc, argv);
    } else if (strcmp(cmd, "source") == 0) {
        if (argc < 2) printf("usage: source <script.sh>\n");
        else run_script_file(argv[1]);
    } else if (strcmp(cmd, "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) write_str_fd(out_fd, " ");
            write_str_fd(out_fd, argv[i]);
        }
        write_str_fd(out_fd, "\n");
    } else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 2) printf("usage: mkdir <dir>\n");
        else {
            int rc = u_mkdir(argv[1], 0755);
            if (rc < 0) printf("mkdir: %s: error %d\n", argv[1], rc);
        }
    } else if (strcmp(cmd, "touch") == 0) {
        if (argc < 2) printf("usage: touch <file>\n");
        else {
            int fd = u_open(argv[1], UAPI_O_CREAT | UAPI_O_WRONLY, 0644);
            if (fd < 0) printf("touch: %s: error %d\n", argv[1], fd);
            else u_close(fd);
        }
    } else if (strcmp(cmd, "rm") == 0) {
        if (argc < 2) printf("usage: rm <path>\n");
        else {
            int rc = u_unlink(argv[1]);
            if (rc < 0) printf("rm: %s: error %d\n", argv[1], rc);
        }
    } else if (strcmp(cmd, "ps") == 0) {
        print_file_to_fd("/proc/tasks", out_fd);
    } else if (strcmp(cmd, "free") == 0 || strcmp(cmd, "mem") == 0) {
        print_file_to_fd("/proc/meminfo", out_fd);
    } else if (strcmp(cmd, "uptime") == 0) {
        print_file_to_fd("/proc/uptime", out_fd);
    } else if (strcmp(cmd, "cpuinfo") == 0) {
        print_file_to_fd("/proc/cpuinfo", out_fd);
    } else if (strcmp(cmd, "irqs") == 0 || strcmp(cmd, "interrupts") == 0) {
        print_file_to_fd("/proc/interrupts", out_fd);
    } else if (strcmp(cmd, "kill") == 0) {
        if (argc < 2) {
            printf("usage: kill <pid> [sig]\n");
        } else {
            pid_t target = (pid_t)atoi(argv[1]);
            int sig = (argc > 2) ? atoi(argv[2]) : UAPI_SIGTERM;
            int rc = u_kill(target, sig);
            if (rc < 0) printf("kill: pid %d failed (%d)\n", target, rc);
        }
    } else if (strcmp(cmd, "clear") == 0) {
        write_str_fd(out_fd, "\x1b[2J\x1b[H");
    } else if (strcmp(cmd, "halt") == 0 || strcmp(cmd, "poweroff") == 0) {
        printf("Powering off OwnKrn...\n");
        u_poweroff();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("Rebooting OwnKrn...\n");
        u_reboot();
    } else if (strcmp(cmd, "exit") == 0) {
        u_exit((argc > 1) ? atoi(argv[1]) : 0);
    } else {
        return false;
    }
    return true;
}

struct stage {
    char *argv[SH_MAX_ARGS];
    int   argc;
    char *in_file;
    char *out_file;
    bool  out_append;
};

static int tokenize_stage(char *text, struct stage *st) {
    memset(st, 0, sizeof(*st));
    char *raw[SH_MAX_ARGS];
    int n = 0;

    char *p = text;
    while (*p && n < SH_MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            raw[n++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else if (*p == '>' || *p == '<') {
            if (*p == '>' && p[1] == '>') {
                raw[n++] = ">>";
                p += 2;
            } else if (*p == '>') {
                raw[n++] = ">";
                p++;
            } else {
                raw[n++] = "<";
                p++;
            }
        } else {
            raw[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
                   *p != '<' && *p != '>') {
                p++;
            }
            if (*p == '<' || *p == '>') {
                if (*p == '>' && p[1] == '>') {
                    *p = '\0';
                    p += 2;
                    if (n < SH_MAX_ARGS - 1) raw[n++] = ">>";
                } else {
                    char op = *p;
                    *p++ = '\0';
                    if (n < SH_MAX_ARGS - 1) raw[n++] = (op == '>') ? ">" : "<";
                }
            } else if (*p) {
                *p++ = '\0';
            }
        }
    }

    for (int i = 0; i < n; i++) {
        if (strcmp(raw[i], "<") == 0 && i + 1 < n) {
            st->in_file = raw[++i];
        } else if (strcmp(raw[i], ">") == 0 && i + 1 < n) {
            st->out_file = raw[++i];
            st->out_append = false;
        } else if (strcmp(raw[i], ">>") == 0 && i + 1 < n) {
            st->out_file = raw[++i];
            st->out_append = true;
        } else {
            st->argv[st->argc++] = raw[i];
        }
    }
    st->argv[st->argc] = NULL;
    return st->argc;
}

static const char *map_cmd_alias(const char *cmd) {
    if (strcmp(cmd, "neofetch") == 0 || strcmp(cmd, "sysinfo") == 0) {
        return "fetch";
    }
    if (strcmp(cmd, "nano") == 0) {
        return "edit";
    }
    if (strcmp(cmd, "htop") == 0) {
        return "top";
    }
    return cmd;
}

static bool resolve_exec_path(const char *raw_cmd, char *out, size_t cap) {
    const char *cmd = map_cmd_alias(raw_cmd);
    if (cmd[0] == '/' || cmd[0] == '.') {
        strncpy(out, cmd, cap - 1);
        out[cap - 1] = '\0';
    } else {
        snprintf(out, cap, "/bin/%s", cmd);
    }
    struct uapi_stat st;
    return u_stat(out, &st) == 0 && st.type == 1;
}

static bool is_known_builtin(const char *cmd) {
    static const char *const builtins[] = {
        "echo", "cat", "head", "tail", "cp", "mv", "source",
        "ls", "ll", "tree", "stat", "history",
        "whoami", "hostname", "ps", "free", "mem", "uname",
        "uptime", "cpuinfo", "irqs", "interrupts",
        "pwd", "cd", "help", "mkdir", "touch", "rm",
        "kill", "clear", "halt", "poweroff", "reboot", "exit", NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(cmd, builtins[i]) == 0) return true;
    }
    return false;
}

static void execute_line(char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
        line++;
    }
    if (!*line || *line == '#') {
        return;
    }

    /* Strip inline '#' comment outside double quotes */
    bool q = false;
    for (char *p = line; *p; p++) {
        if (*p == '"') q = !q;
        else if (*p == '#' && !q && (p == line || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            break;
        }
    }

    char *stage_strs[SH_MAX_STAGES];
    int nstages = 0;
    bool in_quote = false;
    stage_strs[nstages++] = line;

    for (char *p = line; *p; p++) {
        if (*p == '"') {
            in_quote = !in_quote;
        } else if (*p == '|' && !in_quote) {
            *p = '\0';
            if (nstages < SH_MAX_STAGES) {
                stage_strs[nstages++] = p + 1;
            }
        }
    }

    struct stage stages[SH_MAX_STAGES];
    for (int i = 0; i < nstages; i++) {
        if (tokenize_stage(stage_strs[i], &stages[i]) == 0) {
            if (nstages == 1) return;
            printf("sh: syntax error near '|'\n");
            return;
        }
    }

    if (nstages == 1 && is_stateful_builtin(stages[0].argv[0])) {
        run_builtin(stages[0].argc, stages[0].argv, 0, 1);
        return;
    }

    if (nstages == 1) {
        struct stage *s = &stages[0];
        char exec_path[256];
        int in_fd = 0, out_fd = 1;
        bool close_in = false, close_out = false;

        if (!resolve_exec_path(s->argv[0], exec_path, sizeof(exec_path)) ||
            is_known_builtin(s->argv[0])) {
            if (s->in_file) {
                in_fd = u_open(s->in_file, UAPI_O_RDONLY, 0);
                if (in_fd < 0) {
                    printf("sh: cannot open '%s' (%d)\n", s->in_file, in_fd);
                    return;
                }
                close_in = true;
            }
            if (s->out_file) {
                uint32_t fl = UAPI_O_CREAT | UAPI_O_WRONLY |
                              (s->out_append ? UAPI_O_APPEND : UAPI_O_TRUNC);
                out_fd = u_open(s->out_file, fl, 0644);
                if (out_fd < 0) {
                    printf("sh: cannot open '%s' (%d)\n", s->out_file, out_fd);
                    if (close_in) u_close(in_fd);
                    return;
                }
                close_out = true;
            }
            bool handled = run_builtin(s->argc, s->argv, in_fd, out_fd);
            if (close_in) u_close(in_fd);
            if (close_out) u_close(out_fd);
            if (handled) return;
        }
    }

    pid_t pids[SH_MAX_STAGES];
    int nspawned = 0;
    int prev_read = -1;

    for (int i = 0; i < nstages; i++) {
        struct stage *s = &stages[i];
        int pfd[2] = { -1, -1 };
        if (i + 1 < nstages) {
            if (u_pipe(pfd) < 0) {
                printf("sh: pipe failed\n");
                if (prev_read >= 0) u_close(prev_read);
                break;
            }
        }

        pid_t pid = u_fork();
        if (pid < 0) {
            printf("sh: fork failed (%d)\n", pid);
            if (prev_read >= 0) u_close(prev_read);
            if (pfd[0] >= 0) { u_close(pfd[0]); u_close(pfd[1]); }
            break;
        }

        if (pid == 0) {
            if (prev_read >= 0) {
                u_dup2(prev_read, 0);
                u_close(prev_read);
            }
            if (s->in_file) {
                int fd = u_open(s->in_file, UAPI_O_RDONLY, 0);
                if (fd < 0) {
                    printf("sh: cannot open '%s' (%d)\n", s->in_file, fd);
                    u_exit(1);
                }
                u_dup2(fd, 0);
                u_close(fd);
            }

            if (pfd[1] >= 0) {
                u_dup2(pfd[1], 1);
                u_close(pfd[0]);
                u_close(pfd[1]);
            }
            if (s->out_file) {
                uint32_t fl = UAPI_O_CREAT | UAPI_O_WRONLY |
                              (s->out_append ? UAPI_O_APPEND : UAPI_O_TRUNC);
                int fd = u_open(s->out_file, fl, 0644);
                if (fd < 0) {
                    printf("sh: cannot open '%s' (%d)\n", s->out_file, fd);
                    u_exit(1);
                }
                u_dup2(fd, 1);
                u_close(fd);
            }

            if (run_builtin(s->argc, s->argv, 0, 1)) {
                u_exit(0);
            }

            char exec_path[256];
            if (!resolve_exec_path(s->argv[0], exec_path, sizeof(exec_path))) {
                printf("sh: command not found: %s\n", s->argv[0]);
                u_exit(127);
            }
            u_execve(exec_path, s->argv);
            printf("sh: execve '%s' failed\n", exec_path);
            u_exit(127);
        }

        pids[nspawned++] = pid;
        if (prev_read >= 0) u_close(prev_read);
        if (pfd[1] >= 0) u_close(pfd[1]);
        prev_read = pfd[0];
    }

    if (prev_read >= 0) u_close(prev_read);

    for (int i = 0; i < nspawned; i++) {
        int st = 0;
        u_waitpid(pids[i], &st);
    }
    u_tty_set_mode(TTY_MODE_CANON);
}

/* ---------------- Interactive Line Editor & Tab Completion ---------------- */

static void print_prompt(const char *cwd) {
    /* Keep literal 'ownkrn:<cwd># ' contiguous for automated test suite */
    printf("\x1b[1;32m%s@\x1b[1;36m%s:%s# \x1b[0m", g_user, g_host, cwd);
}

static void redraw_input(const char *cwd, const char *buf, size_t len, size_t pos) {
    write_str_fd(1, "\r");
    print_prompt(cwd);
    if (len > 0) {
        u_write(1, buf, len);
    }
    write_str_fd(1, "\x1b[K");
    if (pos < len) {
        char mv[24];
        snprintf(mv, sizeof(mv), "\x1b[%zuD", len - pos);
        write_str_fd(1, mv);
    }
}

static void add_candidate(char cands[48][64], bool is_dir[48], int *count,
                          const char *name, bool dir) {
    if (*count >= 48) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(cands[i], name) == 0) return;
    }
    strncpy(cands[*count], name, 63);
    cands[*count][63] = '\0';
    is_dir[*count] = dir;
    (*count)++;
}

static void handle_tab_completion(const char *cwd, char *buf, size_t *len_io, size_t *pos_io) {
    size_t len = *len_io;
    size_t pos = *pos_io;

    size_t tok_start = pos;
    while (tok_start > 0 && buf[tok_start - 1] != ' ' && buf[tok_start - 1] != '|') {
        tok_start--;
    }

    char token[128];
    size_t tok_len = pos - tok_start;
    if (tok_len >= sizeof(token)) return;
    memcpy(token, buf + tok_start, tok_len);
    token[tok_len] = '\0';

    /* Check if this is the command token */
    size_t lead = 0;
    while (lead < tok_start && (buf[lead] == ' ' || buf[lead] == '\t')) lead++;
    bool is_cmd_tok = (lead == tok_start || (tok_start > 0 && buf[tok_start - 1] == '|'));
    bool has_slash = (strchr(token, '/') != NULL);

    char cands[48][64];
    bool is_dir[48];
    int  ncands = 0;
    const char *match_prefix = token;

    if (is_cmd_tok && !has_slash) {
        static const char *const cmds[] = {
            "help", "fetch", "neofetch", "top", "htop", "edit", "nano",
            "snake", "uname", "cpuinfo", "irqs", "interrupts",
            "pwd", "cd", "ls", "ll", "tree", "stat",
            "cat", "head", "tail", "cp", "mv", "source", "sh",
            "echo", "mkdir", "touch", "rm", "ps", "free", "uptime",
            "date", "kill", "history", "whoami", "hostname", "clear",
            "halt", "reboot", "exit", NULL
        };
        for (int i = 0; cmds[i]; i++) {
            if (strncmp(cmds[i], token, tok_len) == 0) {
                add_candidate(cands, is_dir, &ncands, cmds[i], false);
            }
        }
        int fd = u_open("/bin", UAPI_O_RDONLY, 0);
        if (fd >= 0) {
            struct uapi_dirent ents[32];
            int n;
            while ((n = u_getdents(fd, ents, 32)) > 0) {
                for (int i = 0; i < n; i++) {
                    if (ents[i].name[0] == '.') continue;
                    if (strncmp(ents[i].name, token, tok_len) == 0) {
                        add_candidate(cands, is_dir, &ncands, ents[i].name, false);
                    }
                }
            }
            u_close(fd);
        }
    } else {
        char search_dir[128];
        const char *last_slash = NULL;
        for (size_t i = 0; i < tok_len; i++) {
            if (token[i] == '/') last_slash = &token[i];
        }
        if (!last_slash) {
            strcpy(search_dir, ".");
            match_prefix = token;
        } else if (last_slash == token) {
            strcpy(search_dir, "/");
            match_prefix = last_slash + 1;
        } else {
            size_t dlen = (size_t)(last_slash - token);
            if (dlen >= sizeof(search_dir)) dlen = sizeof(search_dir) - 1;
            memcpy(search_dir, token, dlen);
            search_dir[dlen] = '\0';
            match_prefix = last_slash + 1;
        }

        size_t plen = strlen(match_prefix);
        int fd = u_open(search_dir, UAPI_O_RDONLY, 0);
        if (fd >= 0) {
            struct uapi_dirent ents[32];
            int n;
            while ((n = u_getdents(fd, ents, 32)) > 0) {
                for (int i = 0; i < n; i++) {
                    if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0) {
                        continue;
                    }
                    if (strncmp(ents[i].name, match_prefix, plen) == 0) {
                        add_candidate(cands, is_dir, &ncands, ents[i].name, ents[i].type == 2);
                    }
                }
            }
            u_close(fd);
        }
    }

    if (ncands == 0) return;

    size_t plen = strlen(match_prefix);
    /* Compute longest common prefix across candidates */
    size_t lcp = strlen(cands[0]);
    for (int i = 1; i < ncands; i++) {
        size_t j = 0;
        while (j < lcp && cands[0][j] && cands[0][j] == cands[i][j]) j++;
        lcp = j;
    }

    if (lcp > plen) {
        size_t add_len = lcp - plen;
        if (len + add_len + 2 < SH_LINE_MAX) {
            for (size_t i = len; i > pos; i--) {
                buf[i + add_len] = buf[i];
            }
            memcpy(buf + pos, cands[0] + plen, add_len);
            len += add_len;
            pos += add_len;
            buf[len] = '\0';

            if (ncands == 1) {
                char suffix = is_dir[0] ? '/' : ' ';
                for (size_t i = len; i > pos; i--) {
                    buf[i + 1] = buf[i];
                }
                buf[pos] = suffix;
                len++;
                pos++;
                buf[len] = '\0';
            }
            *len_io = len;
            *pos_io = pos;
            redraw_input(cwd, buf, len, pos);
        }
    } else if (ncands == 1) {
        char suffix = is_dir[0] ? '/' : ' ';
        if (len + 2 < SH_LINE_MAX) {
            for (size_t i = len; i > pos; i--) {
                buf[i + 1] = buf[i];
            }
            buf[pos] = suffix;
            len++;
            pos++;
            buf[len] = '\0';
            *len_io = len;
            *pos_io = pos;
            redraw_input(cwd, buf, len, pos);
        }
    } else {
        write_str_fd(1, "\n");
        for (int i = 0; i < ncands; i++) {
            printf("  %s%s%s\x1b[0m",
                   is_dir[i] ? "\x1b[1;34m" : "\x1b[0m",
                   cands[i],
                   is_dir[i] ? "/" : "");
        }
        write_str_fd(1, "\n");
        redraw_input(cwd, buf, len, pos);
    }
}

static ssize_t sh_readline(const char *cwd, char *buf, size_t cap) {
    size_t len = 0;
    size_t pos = 0;
    buf[0] = '\0';

    char saved_current[SH_LINE_MAX];
    saved_current[0] = '\0';
    int hist_idx = g_hist_count;
    int hist_min = (g_hist_count > SH_HIST_MAX) ? (g_hist_count - SH_HIST_MAX) : 0;

    u_tty_set_mode(TTY_MODE_RAW);

    for (;;) {
        char c = 0;
        ssize_t r = u_read(0, &c, 1);
        if (r <= 0) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            write_str_fd(1, "\n");
            buf[len] = '\0';
            u_tty_set_mode(TTY_MODE_CANON);
            return (ssize_t)len;
        }

        if (c == 0x04) { /* Ctrl-D */
            if (len == 0) {
                u_tty_set_mode(TTY_MODE_CANON);
                return 0;
            }
            if (pos < len) {
                for (size_t i = pos; i < len; i++) buf[i] = buf[i + 1];
                len--;
                redraw_input(cwd, buf, len, pos);
            }
            continue;
        }

        if (c == 0x03) { /* Ctrl-C */
            write_str_fd(1, "^C\n");
            len = 0;
            pos = 0;
            buf[0] = '\0';
            print_prompt(cwd);
            continue;
        }

        if (c == 0x0C) { /* Ctrl-L */
            write_str_fd(1, "\x1b[2J\x1b[H");
            redraw_input(cwd, buf, len, pos);
            continue;
        }

        if (c == 0x15) { /* Ctrl-U */
            len = 0;
            pos = 0;
            buf[0] = '\0';
            redraw_input(cwd, buf, len, pos);
            continue;
        }

        if (c == 0x17) { /* Ctrl-W: delete previous word */
            if (pos > 0) {
                size_t old_pos = pos;
                while (pos > 0 && buf[pos - 1] == ' ') pos--;
                while (pos > 0 && buf[pos - 1] != ' ') pos--;
                size_t diff = old_pos - pos;
                for (size_t i = pos; i + diff <= len; i++) {
                    buf[i] = buf[i + diff];
                }
                len -= diff;
                redraw_input(cwd, buf, len, pos);
            }
            continue;
        }

        if (c == '\b' || c == 0x7F) { /* Backspace */
            if (pos > 0) {
                if (pos == len) {
                    pos--;
                    len--;
                    buf[len] = '\0';
                    write_str_fd(1, "\b \b");
                } else {
                    for (size_t i = pos - 1; i < len; i++) {
                        buf[i] = buf[i + 1];
                    }
                    pos--;
                    len--;
                    redraw_input(cwd, buf, len, pos);
                }
            }
            continue;
        }

        if (c == '\t') { /* Tab auto-completion */
            handle_tab_completion(cwd, buf, &len, &pos);
            continue;
        }

        if (c == 0x1B) { /* ANSI escape sequence (Arrows, Home, End, Delete) */
            char s1 = 0, s2 = 0;
            if (u_read(0, &s1, 1) <= 0) continue;
            if (s1 != '[' && s1 != 'O') continue;
            if (u_read(0, &s2, 1) <= 0) continue;

            if (s2 == 'A') { /* Up Arrow: older history */
                if (hist_idx > hist_min) {
                    if (hist_idx == g_hist_count) {
                        strncpy(saved_current, buf, SH_LINE_MAX - 1);
                        saved_current[SH_LINE_MAX - 1] = '\0';
                    }
                    hist_idx--;
                    strncpy(buf, g_history[hist_idx % SH_HIST_MAX], cap - 1);
                    buf[cap - 1] = '\0';
                    len = strlen(buf);
                    pos = len;
                    redraw_input(cwd, buf, len, pos);
                }
            } else if (s2 == 'B') { /* Down Arrow: newer history */
                if (hist_idx < g_hist_count) {
                    hist_idx++;
                    if (hist_idx == g_hist_count) {
                        strncpy(buf, saved_current, cap - 1);
                    } else {
                        strncpy(buf, g_history[hist_idx % SH_HIST_MAX], cap - 1);
                    }
                    buf[cap - 1] = '\0';
                    len = strlen(buf);
                    pos = len;
                    redraw_input(cwd, buf, len, pos);
                }
            } else if (s2 == 'C') { /* Right Arrow */
                if (pos < len) {
                    pos++;
                    write_str_fd(1, "\x1b[C");
                }
            } else if (s2 == 'D') { /* Left Arrow */
                if (pos > 0) {
                    pos--;
                    write_str_fd(1, "\x1b[D");
                }
            } else if (s2 == 'H') { /* Home */
                if (pos > 0) {
                    pos = 0;
                    redraw_input(cwd, buf, len, pos);
                }
            } else if (s2 == 'F') { /* End */
                if (pos < len) {
                    pos = len;
                    redraw_input(cwd, buf, len, pos);
                }
            } else if (s2 >= '1' && s2 <= '8') {
                char tilde = 0;
                u_read(0, &tilde, 1);
                if (s2 == '3' && tilde == '~') { /* Delete */
                    if (pos < len) {
                        for (size_t i = pos; i < len; i++) buf[i] = buf[i + 1];
                        len--;
                        redraw_input(cwd, buf, len, pos);
                    }
                } else if ((s2 == '1' || s2 == '7') && tilde == '~') { /* Home */
                    pos = 0;
                    redraw_input(cwd, buf, len, pos);
                } else if ((s2 == '4' || s2 == '8') && tilde == '~') { /* End */
                    pos = len;
                    redraw_input(cwd, buf, len, pos);
                }
            }
            continue;
        }

        if ((unsigned char)c >= 0x20 && len + 1 < cap) {
            if (pos == len) {
                buf[len++] = c;
                pos = len;
                buf[len] = '\0';
                u_write(1, &c, 1);
            } else {
                for (size_t i = len + 1; i > pos; i--) {
                    buf[i] = buf[i - 1];
                }
                buf[pos] = c;
                len++;
                pos++;
                buf[len] = '\0';
                redraw_input(cwd, buf, len, pos);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        return run_script_file(argv[1]);
    }

    char line[SH_LINE_MAX];
    char cwd[256];

    for (;;) {
        if (u_getcwd(cwd, sizeof(cwd)) <= 0) {
            strcpy(cwd, "/");
        }
        print_prompt(cwd);

        ssize_t n = sh_readline(cwd, line, sizeof(line));
        if (n == 0) {
            printf("\n");
            u_exit(0);
        }
        if (n < 0) {
            continue;
        }
        history_add(line);
        execute_line(line);
    }
    return 0;
}
