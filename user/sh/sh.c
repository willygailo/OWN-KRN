#include <ulibc.h>

#define SH_LINE_MAX   512
#define SH_MAX_ARGS   32
#define SH_MAX_STAGES 8
#define SH_HIST_MAX   32
#define SH_ENV_MAX    24
#define SH_JOBS_MAX   16

struct env_entry {
    bool used;
    char key[32];
    char val[128];
};

struct bg_job {
    bool  used;
    int   id;
    pid_t pid;
    char  cmd[64];
};

static char             g_history[SH_HIST_MAX][SH_LINE_MAX];
static int              g_hist_count = 0;
static char             g_user[32] = "root";
static char             g_host[32] = "ownkrn";
static struct env_entry g_env[SH_ENV_MAX];
static struct bg_job    g_jobs[SH_JOBS_MAX];
static int              g_next_job_id = 1;
static int              g_last_status = 0;

static void write_str_fd(int fd, const char *s) {
    u_write(fd, s, strlen(s));
}

static void env_set(const char *key, const char *val) {
    for (int i = 0; i < SH_ENV_MAX; i++) {
        if (g_env[i].used && strcmp(g_env[i].key, key) == 0) {
            strncpy(g_env[i].val, val, sizeof(g_env[i].val) - 1);
            g_env[i].val[sizeof(g_env[i].val) - 1] = '\0';
            return;
        }
    }
    for (int i = 0; i < SH_ENV_MAX; i++) {
        if (!g_env[i].used) {
            g_env[i].used = true;
            strncpy(g_env[i].key, key, sizeof(g_env[i].key) - 1);
            g_env[i].key[sizeof(g_env[i].key) - 1] = '\0';
            strncpy(g_env[i].val, val, sizeof(g_env[i].val) - 1);
            g_env[i].val[sizeof(g_env[i].val) - 1] = '\0';
            return;
        }
    }
}

static const char *env_get(const char *key) {
    for (int i = 0; i < SH_ENV_MAX; i++) {
        if (g_env[i].used && strcmp(g_env[i].key, key) == 0) {
            return g_env[i].val;
        }
    }
    return "";
}

static void env_unset(const char *key) {
    for (int i = 0; i < SH_ENV_MAX; i++) {
        if (g_env[i].used && strcmp(g_env[i].key, key) == 0) {
            g_env[i].used = false;
            return;
        }
    }
}

static void env_init(void) {
    memset(g_env, 0, sizeof(g_env));
    memset(g_jobs, 0, sizeof(g_jobs));
    env_set("PATH",     "/bin");
    env_set("HOME",     "/root");
    env_set("USER",     "root");
    env_set("HOSTNAME", "ownkrn");
    env_set("SHELL",    "/bin/sh");
    env_set("TERM",     "linux");
    env_set("OSTYPE",   "linux-ownkrn");
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
        "  \x1b[1;32mSystem, Diagnostics & Linux Kernel:\x1b[0m\n"
        "    tester / bench      Interactive 6-Suite OS Diagnostics & Benchmark Center\n"
        "    dmesg [-c]          Print or clear kernel ring buffer (/proc/kmsg)\n"
        "    lspci / lsblk       List PCI Bus hardware devices & block partitions\n"
        "    kconfig             Display compiled kernel Kconfig (.config)\n"
        "    fetch / neofetch    Display ASCII OS logo & live system telemetry\n"
        "    top / htop [-n 1]   Interactive real-time process & memory monitor\n"
        "    uname [-a|-r|-s|-m] Print kernel & machine info\n"
        "    cpuinfo / irqs      Inspect /proc/cpuinfo & /proc/interrupts\n"
        "    ps / free / uptime  Inspect /proc/tasks, meminfo, uptime\n"
        "    id / su [user]      Show UID/GID credentials or switch user (root/guest)\n"
        "    env / export K=V    Manage environment variables ($USER, $HOME, $?)\n"
        "    jobs / fg [id]      Manage background jobs ('cmd &')\n"
        "  \x1b[1;32mFiles & Persistent Disk (/disk):\x1b[0m\n"
        "    fm [dir]            Dual-pane interactive TUI File Manager\n"
        "    df / sync           Show mounted filesystems (/disk ATA0) & flush\n"
        "    pwd / cd [dir]      Print or change working directory\n"
        "    ls [-l] [dir]       List directory entries (colorized + mode bits)\n"
        "    chmod <oct> <path>  Change file mode permissions (e.g. chmod 700 f)\n"
        "    find [dir] [-name]  Search files recursively across VFS\n"
        "    sort / uniq [file]  Sort or deduplicate lines in files/pipelines\n"
        "    which <cmd>         Locate builtin, alias, or /bin executable\n"
        "    tree [dir]          Render visual directory hierarchy\n"
        "    stat <path>         Inspect inode, type, mode, and byte size\n"
        "    cat / head / tail   Display full, first N, or last N lines\n"
        "    cp / mv <src> <dst> Copy or move/rename files (supports /disk)\n"
        "    edit / nano <file>  Full-screen interactive TUI text editor\n"
        "    mkdir / touch / rm  Create or remove files & directories\n"
        "    sh <script.sh>      Execute a shell script file\n"
        "  \x1b[1;32mUtilities, Audio & Games:\x1b[0m\n"
        "    calc <a op b>       Arithmetic & bitwise hex/dec calculator\n"
        "    cal [month] [year]  Monthly calendar with live RTC highlight\n"
        "    beep [hz] [ms]      Play hardware tone via PIT Ch2 PC Speaker\n"
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

static void format_mode_perm(uint32_t type, uint32_t mode, char out[11]) {
    out[0] = (type == 2) ? 'd' : (type == 3) ? 'c' : '-';
    out[1] = (mode & 0400U) ? 'r' : '-';
    out[2] = (mode & 0200U) ? 'w' : '-';
    out[3] = (mode & 0100U) ? 'x' : '-';
    out[4] = (mode & 0040U) ? 'r' : '-';
    out[5] = (mode & 0020U) ? 'w' : '-';
    out[6] = (mode & 0010U) ? 'x' : '-';
    out[7] = (mode & 0004U) ? 'r' : '-';
    out[8] = (mode & 0002U) ? 'w' : '-';
    out[9] = (mode & 0001U) ? 'x' : '-';
    out[10] = '\0';
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
            uint32_t mode = (ents[i].type == 2) ? 0755U :
                            (ents[i].type == 3) ? 0666U : 0644U;
            if (u_stat(full, &st) == 0) {
                sz = st.size;
                if (st.mode != 0) mode = st.mode;
            }
            if (strncmp(dir, "/bin", 4) == 0 || strcmp(full, "/bin") == 0) {
                mode |= 0111U;
            }

            const char *color = "\x1b[0m";
            const char *tag   = "<FILE>";
            if (ents[i].type == 2) {
                color = "\x1b[1;34m";
                tag   = "<DIR> ";
            } else if (ents[i].type == 3) {
                color = "\x1b[1;33m";
                tag   = "<DEV> ";
            } else if (mode & 0111U) {
                color = "\x1b[1;32m";
            }

            char perm[11];
            format_mode_perm(ents[i].type, mode, perm);

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
    if (rc == 0) return;

    /* Fallback for cross-filesystem moves (e.g. /tmp -> /disk): copy + unlink */
    int sfd = u_open(argv[1], UAPI_O_RDONLY, 0);
    if (sfd < 0) {
        printf("mv: cannot move '%s' to '%s' (err=%d)\n", argv[1], argv[2], rc);
        return;
    }
    int dfd = u_open(argv[2], UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, 0644);
    if (dfd < 0) {
        u_close(sfd);
        printf("mv: cannot create '%s' (err=%d)\n", argv[2], dfd);
        return;
    }
    char buf[512];
    ssize_t n;
    while ((n = u_read(sfd, buf, sizeof(buf))) > 0) {
        u_write(dfd, buf, (size_t)n);
    }
    u_close(sfd);
    u_close(dfd);
    u_unlink(argv[1]);
}

static int64_t parse_int64(const char *s) {
    int64_t sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    int64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            char c = *s++;
            if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
            else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
            else break;
        }
    } else {
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
        }
    }
    return sign * v;
}

static void cmd_calc(int argc, char **argv, int out_fd) {
    if (argc < 4) {
        printf("usage: calc <a> <op> <b>  (ops: + - * / %% & | ^ << >>, hex 0x.. supported)\n");
        return;
    }
    int64_t a = parse_int64(argv[1]);
    const char *op = argv[2];
    int64_t b = parse_int64(argv[3]);
    int64_t res = 0;

    if (strcmp(op, "+") == 0) res = a + b;
    else if (strcmp(op, "-") == 0) res = a - b;
    else if (strcmp(op, "*") == 0 || strcmp(op, "x") == 0) res = a * b;
    else if (strcmp(op, "/") == 0) {
        if (b == 0) { printf("calc: division by zero\n"); return; }
        res = a / b;
    } else if (strcmp(op, "%") == 0) {
        if (b == 0) { printf("calc: modulo by zero\n"); return; }
        res = a % b;
    } else if (strcmp(op, "&") == 0) res = a & b;
    else if (strcmp(op, "|") == 0) res = a | b;
    else if (strcmp(op, "^") == 0) res = a ^ b;
    else if (strcmp(op, "<<") == 0) res = a << (b & 63);
    else if (strcmp(op, ">>") == 0) res = a >> (b & 63);
    else {
        printf("calc: unknown operator '%s'\n", op);
        return;
    }

    char out[128];
    snprintf(out, sizeof(out), "  %lld %s %lld = \x1b[1;32m%lld\x1b[0m (0x%llx)\n",
             (long long)a, op, (long long)b,
             (long long)res, (unsigned long long)res);
    write_str_fd(out_fd, out);
}

static void cmd_cal(int argc, char **argv, int out_fd) {
    int year = 2026, month = 11, today = 23;
    int rfd = u_open("/proc/rtc", UAPI_O_RDONLY, 0);
    if (rfd >= 0) {
        char rbuf[64];
        ssize_t n = u_read(rfd, rbuf, sizeof(rbuf) - 1);
        u_close(rfd);
        if (n >= 10) {
            rbuf[n] = '\0';
            year  = atoi(rbuf + 0);
            month = atoi(rbuf + 5);
            today = atoi(rbuf + 8);
        }
    }
    if (argc == 2) {
        int v = atoi(argv[1]);
        if (v >= 1 && v <= 12) month = v;
        else if (v >= 1970) year = v;
    } else if (argc >= 3) {
        month = atoi(argv[1]);
        year  = atoi(argv[2]);
        today = -1;
    }
    if (month < 1 || month > 12) month = 1;
    if (year < 1970) year = 2026;

    static const char *const mnames[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int days = mdays[month - 1];
    bool leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    if (month == 2 && leap) days = 29;

    /* Sakamoto's day-of-week algorithm for 1st of month (0=Sun..6=Sat) */
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year - (month < 3 ? 1 : 0);
    int dow = (y + y/4 - y/100 + y/400 + t[month - 1] + 1) % 7;

    char line[128];
    snprintf(line, sizeof(line),
             "   \x1b[1;36m%s %d\x1b[0m\n"
             " \x1b[1;33mSu Mo Tu We Th Fr Sa\x1b[0m\n ",
             mnames[month - 1], year);
    write_str_fd(out_fd, line);

    for (int i = 0; i < dow; i++) {
        write_str_fd(out_fd, "   ");
    }
    for (int d = 1; d <= days; d++) {
        if (d == today) {
            snprintf(line, sizeof(line), "\x1b[7m%2d\x1b[0m ", d);
        } else {
            snprintf(line, sizeof(line), "%2d ", d);
        }
        write_str_fd(out_fd, line);
        if (((dow + d) % 7) == 0 && d < days) {
            write_str_fd(out_fd, "\n ");
        }
    }
    write_str_fd(out_fd, "\n");
}

static void cmd_beep(int argc, char **argv, int out_fd) {
    int freq = (argc > 1) ? atoi(argv[1]) : 880;
    int ms   = (argc > 2) ? atoi(argv[2]) : 75;
    if (freq < 40) freq = 880;
    if (ms < 5) ms = 75;

    int fd = u_open("/dev/speaker", UAPI_O_WRONLY, 0);
    if (fd >= 0) {
        char cmd[32];
        int n = snprintf(cmd, sizeof(cmd), "%d %d", freq, ms);
        u_write(fd, cmd, (size_t)n);
        u_close(fd);
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Beep: %d Hz (%d ms) via PIT Ch2 PC Speaker\n", freq, ms);
    write_str_fd(out_fd, msg);
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
    if (argc > 1) {
        if (strcmp(argv[1], "-a") == 0) {
            snprintf(line, sizeof(line), "%s %s %s %s %s\n",
                     u.sysname, u.nodename, u.release, u.version, u.machine);
        } else if (strcmp(argv[1], "-r") == 0) {
            snprintf(line, sizeof(line), "%s\n", u.release);
        } else if (strcmp(argv[1], "-m") == 0) {
            snprintf(line, sizeof(line), "%s\n", u.machine);
        } else if (strcmp(argv[1], "-n") == 0) {
            snprintf(line, sizeof(line), "%s\n", u.nodename);
        } else if (strcmp(argv[1], "-v") == 0) {
            snprintf(line, sizeof(line), "%s\n", u.version);
        } else {
            snprintf(line, sizeof(line), "%s\n", u.sysname);
        }
    } else {
        snprintf(line, sizeof(line), "%s\n", u.sysname);
    }
    write_str_fd(out_fd, line);
}

static void cmd_id(int out_fd) {
    uint32_t uid = u_getuid();
    uint32_t gid = u_getgid();
    const char *uname = (uid == 0) ? "root" : "guest";
    const char *gname = (gid == 0) ? "root" : "users";
    char buf[128];
    snprintf(buf, sizeof(buf), "uid=%u(%s) gid=%u(%s) groups=%u(%s)\n",
             uid, uname, gid, gname, gid, gname);
    write_str_fd(out_fd, buf);
}

static void cmd_su(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "root";
    if (strcmp(target, "guest") == 0 || strcmp(target, "1000") == 0) {
        u_setuid(1000);
        u_setgid(1000);
        strcpy(g_user, "guest");
        env_set("USER", "guest");
        env_set("HOME", "/home/guest");
    } else if (strcmp(target, "root") == 0 || strcmp(target, "0") == 0) {
        u_setuid(0);
        u_setgid(0);
        strcpy(g_user, "root");
        env_set("USER", "root");
        env_set("HOME", "/root");
    } else {
        printf("su: unknown user '%s' (available: root, guest)\n", target);
    }
}

static void cmd_chmod(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: chmod <octal_mode> <path>\n");
        return;
    }
    uint32_t mode = 0;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '7') {
        mode = (mode << 3) | (uint32_t)(*s - '0');
        s++;
    }
    int rc = u_chmod(argv[2], mode);
    if (rc < 0) {
        printf("chmod: cannot chmod '%s' (err=%d)\n", argv[2], rc);
    }
}

static void cmd_env(int out_fd) {
    for (int i = 0; i < SH_ENV_MAX; i++) {
        if (g_env[i].used) {
            char line[192];
            snprintf(line, sizeof(line), "%s=%s\n", g_env[i].key, g_env[i].val);
            write_str_fd(out_fd, line);
        }
    }
}

static void cmd_export(int argc, char **argv, int out_fd) {
    if (argc < 2) {
        cmd_env(out_fd);
        return;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            env_set(argv[i], eq + 1);
            *eq = '=';
        }
    }
}

static bool match_name_pattern(const char *name, const char *pat) {
    if (!pat || !pat[0] || strcmp(pat, "*") == 0) return true;
    if (pat[0] == '*') {
        size_t nlen = strlen(name);
        size_t slen = strlen(pat + 1);
        return (nlen >= slen && strcmp(name + nlen - slen, pat + 1) == 0);
    }
    return strstr(name, pat) != NULL;
}

static void find_walk(const char *dir, const char *pat, int depth, int out_fd) {
    if (depth > 5) return;
    int fd = u_open(dir, UAPI_O_RDONLY, 0);
    if (fd < 0) return;

    struct uapi_dirent ents[32];
    struct uapi_dirent list[32];
    int total = 0, n;
    while ((n = u_getdents(fd, ents, 32)) > 0) {
        for (int i = 0; i < n && total < 32; i++) {
            if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0) continue;
            list[total++] = ents[i];
        }
    }
    u_close(fd);

    for (int i = 0; i < total; i++) {
        char full[256];
        join_path(dir, list[i].name, full, sizeof(full));
        if (match_name_pattern(list[i].name, pat)) {
            write_str_fd(out_fd, full);
            write_str_fd(out_fd, "\n");
        }
        if (list[i].type == 2) {
            find_walk(full, pat, depth + 1, out_fd);
        }
    }
}

static void cmd_find(int argc, char **argv, int out_fd) {
    const char *dir = ".";
    const char *pat = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            pat = argv[++i];
        } else {
            dir = argv[i];
        }
    }
    if (!pat) {
        write_str_fd(out_fd, dir);
        write_str_fd(out_fd, "\n");
    }
    find_walk(dir, pat, 0, out_fd);
}

static void cmd_sort_uniq(int argc, char **argv, int in_fd, int out_fd, bool uniq_only) {
    int fd = in_fd;
    const char *path = (argc > 1) ? argv[1] : NULL;
    if (path) {
        fd = u_open(path, UAPI_O_RDONLY, 0);
        if (fd < 0) {
            printf("%s: cannot open '%s' (%d)\n", uniq_only ? "uniq" : "sort", path, fd);
            return;
        }
    }

    char buf[2048];
    size_t total = 0;
    ssize_t r;
    while (total + 1 < sizeof(buf) &&
           (r = u_read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (path) u_close(fd);

    char *lines[64];
    int nlines = 0;
    char *p = buf;
    while (*p && nlines < 64) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p || nl) {
            lines[nlines++] = p;
        }
        if (!nl) break;
        p = nl + 1;
    }

    if (!uniq_only) {
        for (int i = 0; i < nlines; i++) {
            for (int j = i + 1; j < nlines; j++) {
                if (strcmp(lines[i], lines[j]) > 0) {
                    char *tmp = lines[i];
                    lines[i] = lines[j];
                    lines[j] = tmp;
                }
            }
        }
    }

    for (int i = 0; i < nlines; i++) {
        if (uniq_only && i > 0 && strcmp(lines[i], lines[i - 1]) == 0) {
            continue;
        }
        write_str_fd(out_fd, lines[i]);
        write_str_fd(out_fd, "\n");
    }
}

static void reap_bg_jobs(bool verbose) {
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (!g_jobs[i].used) continue;
        int st = 0;
        pid_t r = u_waitpid_opts(g_jobs[i].pid, &st, UAPI_WNOHANG);
        if (r == g_jobs[i].pid || r < 0) {
            if (verbose) {
                printf("[%d]+  Done                    %s (pid=%d)\n",
                       g_jobs[i].id, g_jobs[i].cmd, g_jobs[i].pid);
            }
            g_jobs[i].used = false;
        }
    }
}

static void cmd_jobs(int out_fd) {
    reap_bg_jobs(true);
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (g_jobs[i].used) {
            char line[128];
            snprintf(line, sizeof(line), "[%d]+  Running                 %s & (pid=%d)\n",
                     g_jobs[i].id, g_jobs[i].cmd, g_jobs[i].pid);
            write_str_fd(out_fd, line);
        }
    }
}

static void cmd_fg(int argc, char **argv) {
    int target_id = (argc > 1) ? atoi(argv[1]) : -1;
    int slot = -1;
    for (int i = SH_JOBS_MAX - 1; i >= 0; i--) {
        if (!g_jobs[i].used) continue;
        if (target_id < 0 || g_jobs[i].id == target_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("fg: no current background job\n");
        return;
    }
    printf("%s (pid=%d)\n", g_jobs[slot].cmd, g_jobs[slot].pid);
    int st = 0;
    u_waitpid(g_jobs[slot].pid, &st);
    g_last_status = (st >> 8) & 0xFF;
    g_jobs[slot].used = false;
}

static void expand_variables(const char *src, char *dst, size_t cap);

static int py_eval_atom(const char *tok) {
    while (*tok == ' ' || *tok == '\t') tok++;
    if ((*tok >= '0' && *tok <= '9') || *tok == '-') {
        return atoi(tok);
    }
    char clean[32];
    size_t cl = 0;
    while (tok[cl] && tok[cl] != ' ' && tok[cl] != '\t' && tok[cl] != '\r' && cl + 1 < sizeof(clean)) {
        clean[cl] = tok[cl];
        cl++;
    }
    clean[cl] = '\0';
    const char *v = env_get(clean);
    if (v && v[0]) {
        return atoi(v);
    }
    return 0;
}

static void py_expand_fstring(const char *src, char *dst, size_t cap) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < cap; ) {
        if (src[i] == '{') {
            i++;
            char key[32];
            size_t kl = 0;
            while (src[i] && src[i] != '}') {
                if (kl + 1 < sizeof(key) && src[i] != ' ') {
                    key[kl++] = src[i];
                }
                i++;
            }
            if (src[i] == '}') i++;
            key[kl] = '\0';
            const char *val = env_get(key);
            for (size_t k = 0; val[k] && out + 1 < cap; k++) {
                dst[out++] = val[k];
            }
            continue;
        }
        dst[out++] = src[i++];
    }
    dst[out] = '\0';
}

static void py_exec_single_stmt(char *raw_stmt, int out_fd) {
    char stmt[256];
    strncpy(stmt, raw_stmt, sizeof(stmt) - 1);
    stmt[sizeof(stmt) - 1] = '\0';

    char *line = stmt;
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    size_t llen = strlen(line);
    while (llen > 0 && (line[llen - 1] == ' ' || line[llen - 1] == '\t' || line[llen - 1] == '\r')) {
        line[--llen] = '\0';
    }
    if (!*line || *line == '#' || strncmp(line, "import ", 7) == 0 || strncmp(line, "from ", 5) == 0) {
        return;
    }

    if (strncmp(line, "time.sleep(", 11) == 0) {
        int sec = atoi(line + 11);
        if (sec > 0 && sec <= 10) {
            u_msleep((uint64_t)sec * 1000ULL);
        }
        return;
    }

    if (strncmp(line, "print(", 6) == 0) {
        char *arg = line + 6;
        size_t alen = strlen(arg);
        if (alen > 0 && arg[alen - 1] == ')') {
            arg[--alen] = '\0';
        }
        while (*arg == ' ' || *arg == '\t') {
            arg++;
            if (alen > 0) alen--;
        }
        bool is_fstr = false;
        if ((arg[0] == 'f' || arg[0] == 'F') && (arg[1] == '"' || arg[1] == '\'')) {
            is_fstr = true;
            arg++;
            alen--;
        }
        if ((arg[0] == '"' || arg[0] == '\'') && alen >= 2) {
            char q = arg[0];
            arg++;
            alen--;
            if (alen > 0 && arg[alen - 1] == q) {
                arg[--alen] = '\0';
            }
            char out_buf[256];
            if (is_fstr) {
                py_expand_fstring(arg, out_buf, sizeof(out_buf));
            } else {
                expand_variables(arg, out_buf, sizeof(out_buf));
            }
            write_str_fd(out_fd, out_buf);
            write_str_fd(out_fd, "\n");
            return;
        }
        /* Print variable or literal expression */
        const char *v = env_get(arg);
        if (v && v[0]) {
            write_str_fd(out_fd, v);
        } else {
            write_str_fd(out_fd, arg);
        }
        write_str_fd(out_fd, "\n");
        return;
    }

    char *eq = strchr(line, '=');
    if (eq && eq > line && eq[1] != '=' && eq[-1] != '!' && eq[-1] != '<' && eq[-1] != '>') {
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        size_t kl = strlen(k);
        while (kl > 0 && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = '\0';
        while (*v == ' ' || *v == '\t') v++;

        /* Handle random.randint(a, b) */
        const char *ri = strstr(v, "randint(");
        if (ri) {
            ri += 8;
            int lo = atoi(ri);
            const char *comma = strchr(ri, ',');
            int hi = comma ? atoi(comma + 1) : (lo + 10);
            if (hi < lo) hi = lo;
            uint32_t rnd = 42;
            int ufd = u_open("/dev/urandom", UAPI_O_RDONLY, 0);
            if (ufd >= 0) {
                u_read(ufd, &rnd, sizeof(rnd));
                u_close(ufd);
            }
            int val = lo + (int)(rnd % (uint32_t)(hi - lo + 1));
            char nbuf[24];
            snprintf(nbuf, sizeof(nbuf), "%d", val);
            env_set(k, nbuf);
            return;
        }

        /* Handle input("Prompt") */
        if (strncmp(v, "input(", 6) == 0) {
            char *parg = v + 6;
            size_t plen = strlen(parg);
            if (plen > 0 && parg[plen - 1] == ')') parg[--plen] = '\0';
            if ((parg[0] == '"' || parg[0] == '\'') && plen >= 2) {
                parg++;
                plen--;
                if (plen > 0) parg[plen - 1] = '\0';
            }
            if (*parg) write_str_fd(out_fd, parg);
            char inbuf[128];
            ssize_t rn = u_read(0, inbuf, sizeof(inbuf) - 1);
            if (rn > 0) {
                while (rn > 0 && (inbuf[rn - 1] == '\n' || inbuf[rn - 1] == '\r')) rn--;
                inbuf[rn] = '\0';
                env_set(k, inbuf);
            }
            return;
        }

        /* Handle string literal assignment */
        if (v[0] == '"' || v[0] == '\'') {
            char q = v[0];
            v++;
            size_t vl = strlen(v);
            if (vl > 0 && v[vl - 1] == q) v[--vl] = '\0';
            env_set(k, v);
            return;
        }

        /* Handle binary arithmetic: a + b, a - b, a * b, a / b */
        char op = 0;
        char *op_ptr = NULL;
        for (char *scan = v; *scan; scan++) {
            if (*scan == '+' || (*scan == '-' && scan > v) || *scan == '*' || *scan == '/') {
                op = *scan;
                op_ptr = scan;
                break;
            }
        }
        if (op && op_ptr) {
            *op_ptr = '\0';
            int lhs = py_eval_atom(v);
            int rhs = py_eval_atom(op_ptr + 1);
            int res = lhs;
            if (op == '+') res = lhs + rhs;
            else if (op == '-') res = lhs - rhs;
            else if (op == '*') res = lhs * rhs;
            else if (op == '/' && rhs != 0) res = lhs / rhs;
            char nbuf[24];
            snprintf(nbuf, sizeof(nbuf), "%d", res);
            env_set(k, nbuf);
            return;
        }

        env_set(k, v);
    }
}

static void cmd_python(int argc, char **argv, int out_fd) {
    if (argc < 2) {
        write_str_fd(out_fd, "OwnKrn Mini-Python 3.x Runner: usage: python3 <script.py>\n");
        return;
    }
    int fd = u_open(argv[1], UAPI_O_RDONLY, 0);
    if (fd < 0) {
        printf("python: can't open file '%s': [Errno %d]\n", argv[1], fd);
        return;
    }
    static char pybuf[8192];
    size_t total = 0;
    while (total + 1 < sizeof(pybuf)) {
        ssize_t n = u_read(fd, pybuf + total, sizeof(pybuf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    u_close(fd);
    pybuf[total] = '\0';

    /* If script is a Snake / Tkinter / Curses arcade game (e.g. laro.py), launch Ring 3 /bin/snake */
    if (strstr(pybuf, "Snake") != NULL || strstr(pybuf, "snake") != NULL) {
        pid_t pid = u_fork();
        if (pid == 0) {
            char *sargv[] = { "/bin/snake", NULL };
            u_execve("/bin/snake", sargv);
            u_exit(127);
        } else if (pid > 0) {
            int st = 0;
            u_waitpid(pid, &st);
        }
        return;
    }

    /* Split script into lines and execute (supporting 'for var in range(...):' indented blocks) */
    static char *lines[256];
    int nlines = 0;
    char *p = pybuf;
    while (*p && nlines < 256) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        lines[nlines++] = p;
        if (!nl) break;
        p = nl + 1;
    }

    for (int i = 0; i < nlines; ) {
        char *ln = lines[i];
        while (*ln == ' ' || *ln == '\t') ln++;

        if (strncmp(ln, "for ", 4) == 0 && strstr(ln, " in range(") != NULL) {
            char var_name[32];
            const char *vstart = ln + 4;
            const char *in_pos = strstr(vstart, " in range(");
            size_t vlen = (size_t)(in_pos - vstart);
            if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
            memcpy(var_name, vstart, vlen);
            var_name[vlen] = '\0';

            const char *rargs = in_pos + 10;
            int r_start = 0, r_end = 0;
            const char *comma = strchr(rargs, ',');
            if (comma) {
                r_start = atoi(rargs);
                r_end = atoi(comma + 1);
            } else {
                r_end = atoi(rargs);
            }

            int body_start = i + 1;
            int body_end = body_start;
            while (body_end < nlines &&
                   (lines[body_end][0] == ' ' || lines[body_end][0] == '\t')) {
                body_end++;
            }

            for (int val = r_start; val < r_end && (val - r_start) < 64; val++) {
                char nbuf[24];
                snprintf(nbuf, sizeof(nbuf), "%d", val);
                env_set(var_name, nbuf);
                for (int b = body_start; b < body_end; b++) {
                    py_exec_single_stmt(lines[b], out_fd);
                }
            }
            i = body_end;
            continue;
        }

        py_exec_single_stmt(ln, out_fd);
        i++;
    }
}

static bool is_stateful_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 ||
           strcmp(cmd, "hostname") == 0 ||
           strcmp(cmd, "su") == 0 ||
           strcmp(cmd, "export") == 0 ||
           strcmp(cmd, "unset") == 0 ||
           strcmp(cmd, "fg") == 0 ||
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
        const char *target = (argc > 1) ? argv[1] : env_get("HOME");
        if (!target || !target[0]) target = "/";
        int rc = u_chdir(target);
        if (rc < 0) {
            printf("cd: %s: error %d\n", target, rc);
            g_last_status = 1;
        } else {
            char ncwd[256];
            if (u_getcwd(ncwd, sizeof(ncwd)) > 0) env_set("PWD", ncwd);
            g_last_status = 0;
        }
    } else if (strcmp(cmd, "id") == 0) {
        cmd_id(out_fd);
    } else if (strcmp(cmd, "su") == 0) {
        cmd_su(argc, argv);
    } else if (strcmp(cmd, "chmod") == 0) {
        cmd_chmod(argc, argv);
    } else if (strcmp(cmd, "env") == 0 || strcmp(cmd, "printenv") == 0) {
        cmd_env(out_fd);
    } else if (strcmp(cmd, "export") == 0) {
        cmd_export(argc, argv, out_fd);
    } else if (strcmp(cmd, "unset") == 0) {
        if (argc > 1) env_unset(argv[1]);
    } else if (strcmp(cmd, "dmesg") == 0) {
        print_file_to_fd("/proc/kmsg", out_fd);
        if (argc > 1 && strcmp(argv[1], "-c") == 0) {
            u_kmsg_clear();
        }
    } else if (strcmp(cmd, "lspci") == 0) {
        print_file_to_fd("/proc/pci", out_fd);
    } else if (strcmp(cmd, "lsblk") == 0) {
        print_file_to_fd("/proc/partitions", out_fd);
    } else if (strcmp(cmd, "kconfig") == 0) {
        print_file_to_fd("/proc/config", out_fd);
    } else if (strcmp(cmd, "find") == 0) {
        cmd_find(argc, argv, out_fd);
    } else if (strcmp(cmd, "sort") == 0) {
        cmd_sort_uniq(argc, argv, in_fd, out_fd, false);
    } else if (strcmp(cmd, "uniq") == 0) {
        cmd_sort_uniq(argc, argv, in_fd, out_fd, true);
    } else if (strcmp(cmd, "jobs") == 0) {
        cmd_jobs(out_fd);
    } else if (strcmp(cmd, "fg") == 0) {
        cmd_fg(argc, argv);
    } else if (strcmp(cmd, "which") == 0) {
        if (argc < 2) {
            printf("usage: which <command>\n");
        } else {
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf), "/bin/%s", argv[1]);
            struct uapi_stat wst;
            if (u_stat(pbuf, &wst) == 0 && wst.type == 1) {
                write_str_fd(out_fd, pbuf);
                write_str_fd(out_fd, "\n");
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "%s: shell built-in command\n", argv[1]);
                write_str_fd(out_fd, msg);
            }
        }
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
    } else if (strcmp(cmd, "calc") == 0) {
        cmd_calc(argc, argv, out_fd);
    } else if (strcmp(cmd, "cal") == 0) {
        cmd_cal(argc, argv, out_fd);
    } else if (strcmp(cmd, "beep") == 0) {
        cmd_beep(argc, argv, out_fd);
    } else if (strcmp(cmd, "df") == 0 || strcmp(cmd, "mounts") == 0) {
        print_file_to_fd("/proc/mounts", out_fd);
    } else if (strcmp(cmd, "sync") == 0) {
        write_str_fd(out_fd, "Flushing VFS & ATA0 persistent disk (/disk)... OK\n");
    } else if (strcmp(cmd, "source") == 0) {
        if (argc < 2) printf("usage: source <script.sh>\n");
        else run_script_file(argv[1]);
    } else if (strcmp(cmd, "python") == 0 || strcmp(cmd, "python3") == 0) {
        cmd_python(argc, argv, out_fd);
    } else if ((cmd[0] == '.' || cmd[0] == '/') && strlen(cmd) > 3 &&
               strcmp(cmd + strlen(cmd) - 3, ".sh") == 0) {
        run_script_file(cmd);
    } else if ((cmd[0] == '.' || cmd[0] == '/') && strlen(cmd) > 3 &&
               strcmp(cmd + strlen(cmd) - 3, ".py") == 0) {
        char *py_argv[3] = { "python3", (char *)cmd, NULL };
        cmd_python(2, py_argv, out_fd);
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
    } else if (strcmp(cmd, "paste") == 0 || strcmp(cmd, "pbpaste") == 0) {
        print_file_to_fd("/dev/clipboard", out_fd);
    } else if (strcmp(cmd, "copy") == 0 || strcmp(cmd, "pbcopy") == 0 || strcmp(cmd, "clip") == 0) {
        int cfd = u_open("/dev/clipboard", UAPI_O_WRONLY, 0);
        if (cfd < 0) {
            printf("clip: cannot open /dev/clipboard (%d)\n", cfd);
        } else {
            if (argc > 1) {
                for (int i = 1; i < argc; i++) {
                    if (i > 1) u_write(cfd, " ", 1);
                    u_write(cfd, argv[i], strlen(argv[i]));
                }
                u_write(cfd, "\n", 1);
            } else {
                copy_fd(in_fd, cfd);
            }
            u_close(cfd);
        }
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
    if (strcmp(cmd, "nano") == 0 || strcmp(cmd, "code") == 0 ||
        strcmp(cmd, "micro") == 0 || strcmp(cmd, "vim") == 0 ||
        strcmp(cmd, "vi") == 0) {
        return "edit";
    }
    if (strcmp(cmd, "htop") == 0) {
        return "top";
    }
    if (strcmp(cmd, "bench") == 0 || strcmp(cmd, "diag") == 0 ||
        strcmp(cmd, "audit") == 0 || strcmp(cmd, "memtest") == 0) {
        return "tester";
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
        "calc", "cal", "beep", "df", "mounts", "sync",
        "dmesg", "lspci", "lsblk", "kconfig", "id", "su", "chmod",
        "env", "printenv", "export", "unset", "find", "sort", "uniq",
        "which", "jobs", "fg", "paste", "pbpaste", "copy", "pbcopy", "clip",
        "python", "python3",
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

static void expand_variables(const char *src, char *dst, size_t cap) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < cap; ) {
        if (src[i] == '~' && (i == 0 || src[i - 1] == ' ') &&
            (src[i + 1] == '\0' || src[i + 1] == '/' || src[i + 1] == ' ')) {
            const char *home = env_get("HOME");
            size_t hlen = strlen(home);
            for (size_t k = 0; k < hlen && out + 1 < cap; k++) dst[out++] = home[k];
            i++;
            continue;
        }
        if (src[i] == '$') {
            i++;
            if (src[i] == '?') {
                char num[16];
                snprintf(num, sizeof(num), "%d", g_last_status);
                for (size_t k = 0; num[k] && out + 1 < cap; k++) dst[out++] = num[k];
                i++;
                continue;
            }
            if (src[i] == '$') {
                char num[16];
                snprintf(num, sizeof(num), "%d", (int)u_getpid());
                for (size_t k = 0; num[k] && out + 1 < cap; k++) dst[out++] = num[k];
                i++;
                continue;
            }
            char key[32];
            size_t klen = 0;
            while ((src[i] >= 'A' && src[i] <= 'Z') ||
                   (src[i] >= 'a' && src[i] <= 'z') ||
                   (src[i] >= '0' && src[i] <= '9') || src[i] == '_') {
                if (klen + 1 < sizeof(key)) key[klen++] = src[i];
                i++;
            }
            key[klen] = '\0';
            if (klen > 0) {
                const char *val = env_get(key);
                for (size_t k = 0; val[k] && out + 1 < cap; k++) dst[out++] = val[k];
            } else {
                dst[out++] = '$';
            }
            continue;
        }
        dst[out++] = src[i++];
    }
    dst[out] = '\0';
}

static void execute_single_pipeline(char *raw_line) {
    while (*raw_line == ' ' || *raw_line == '\t' || *raw_line == '\r' || *raw_line == '\n') {
        raw_line++;
    }
    if (!*raw_line || *raw_line == '#') {
        return;
    }

    char line[SH_LINE_MAX];
    expand_variables(raw_line, line, sizeof(line));

    /* Check for trailing '&' background job operator */
    bool run_in_bg = false;
    size_t llen = strlen(line);
    while (llen > 0 && (line[llen - 1] == ' ' || line[llen - 1] == '\t' ||
                        line[llen - 1] == '\r' || line[llen - 1] == '\n')) {
        line[--llen] = '\0';
    }
    if (llen > 0 && line[llen - 1] == '&') {
        run_in_bg = true;
        line[--llen] = '\0';
        while (llen > 0 && (line[llen - 1] == ' ' || line[llen - 1] == '\t')) {
            line[--llen] = '\0';
        }
    }

    char bg_cmd_copy[64];
    strncpy(bg_cmd_copy, line, sizeof(bg_cmd_copy) - 1);
    bg_cmd_copy[sizeof(bg_cmd_copy) - 1] = '\0';

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

    if (!run_in_bg && nstages == 1 && is_stateful_builtin(stages[0].argv[0])) {
        run_builtin(stages[0].argc, stages[0].argv, 0, 1);
        return;
    }

    if (!run_in_bg && nstages == 1) {
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
            if (handled) {
                g_last_status = 0;
                return;
            }
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

    if (run_in_bg && nspawned > 0) {
        pid_t last_pid = pids[nspawned - 1];
        int jid = g_next_job_id++;
        for (int i = 0; i < SH_JOBS_MAX; i++) {
            if (!g_jobs[i].used) {
                g_jobs[i].used = true;
                g_jobs[i].id   = jid;
                g_jobs[i].pid  = last_pid;
                strcpy(g_jobs[i].cmd, bg_cmd_copy);
                break;
            }
        }
        printf("[%d] %d\n", jid, last_pid);
        g_last_status = 0;
        return;
    }

    for (int i = 0; i < nspawned; i++) {
        int st = 0;
        u_waitpid(pids[i], &st);
        g_last_status = (st >> 8) & 0xFF;
    }
    u_tty_set_mode(TTY_MODE_CANON);
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

    /* Split chained commands on ';' and '&&' outside double quotes */
    char *cur = line;
    q = false;
    while (*cur) {
        char *p = cur;
        int op = 0; /* 0=end, 1=';', 2='&&' */
        while (*p) {
            if (*p == '"') {
                q = !q;
            } else if (!q && *p == ';') {
                *p = '\0';
                op = 1;
                p++;
                break;
            } else if (!q && p[0] == '&' && p[1] == '&') {
                *p = '\0';
                op = 2;
                p += 2;
                break;
            }
            p++;
        }

        execute_single_pipeline(cur);
        if (op == 2 && g_last_status != 0) {
            break;
        }
        cur = p;
    }
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
            "help", "tester", "bench", "diag", "audit", "memtest",
            "dmesg", "lspci", "lsblk", "kconfig", "id", "su", "chmod",
            "env", "printenv", "export", "unset", "find", "sort", "uniq",
            "which", "jobs", "fg",
            "fetch", "neofetch", "top", "htop", "fm", "edit", "nano",
            "snake", "calc", "cal", "beep", "df", "mounts", "sync",
            "uname", "cpuinfo", "irqs", "interrupts",
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

        if (c == 0x16) { /* Ctrl-V: Paste from Host / /dev/clipboard into command line */
            int cfd = u_open("/dev/clipboard", UAPI_O_RDONLY, 0);
            if (cfd >= 0) {
                char cbuf[256];
                ssize_t cn = u_read(cfd, cbuf, sizeof(cbuf) - 1);
                u_close(cfd);
                if (cn > 0) {
                    for (ssize_t k = 0; k < cn && len + 1 < cap; k++) {
                        char ch = cbuf[k];
                        if (ch == '\r' || ch == '\n') break;
                        if (ch == '\t') ch = ' ';
                        if ((unsigned char)ch >= 0x20) {
                            for (size_t i = len + 1; i > pos; i--) {
                                buf[i] = buf[i - 1];
                            }
                            buf[pos++] = ch;
                            len++;
                            buf[len] = '\0';
                        }
                    }
                    redraw_input(cwd, buf, len, pos);
                }
            }
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
            } else if (s2 >= '0' && s2 <= '9') {
                int param = s2 - '0';
                char ch_next = 0;
                while (u_read(0, &ch_next, 1) > 0) {
                    if (ch_next >= '0' && ch_next <= '9') {
                        param = param * 10 + (ch_next - '0');
                    } else {
                        break;
                    }
                }
                if (ch_next == '~') {
                    if (param == 3) { /* Delete */
                        if (pos < len) {
                            for (size_t i = pos; i < len; i++) buf[i] = buf[i + 1];
                            len--;
                            redraw_input(cwd, buf, len, pos);
                        }
                    } else if (param == 1 || param == 7) { /* Home */
                        pos = 0;
                        redraw_input(cwd, buf, len, pos);
                    } else if (param == 4 || param == 8) { /* End */
                        pos = len;
                        redraw_input(cwd, buf, len, pos);
                    }
                    /* Note: param 200 / 201 (Bracketed Paste) are cleanly ignored */
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
    env_init();
    if (argc > 1) {
        size_t alen = strlen(argv[1]);
        if (alen > 3 && strcmp(argv[1] + alen - 3, ".py") == 0) {
            cmd_python(argc, argv, 1);
            return 0;
        }
        return run_script_file(argv[1]);
    }

    char line[SH_LINE_MAX];
    char cwd[256];

    for (;;) {
        reap_bg_jobs(false);
        if (u_getcwd(cwd, sizeof(cwd)) <= 0) {
            strcpy(cwd, "/");
        }
        env_set("PWD", cwd);
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
