#include <ulibc.h>

static int read_proc_file(const char *path, char *buf, size_t cap) {
    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        buf[0] = '\0';
        return -1;
    }
    ssize_t n = u_read(fd, buf, cap - 1);
    u_close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

static uint64_t parse_after_label(const char *text, const char *label) {
    const char *p = strstr(text, label);
    if (!p) return 0;
    p += strlen(label);
    while (*p == ' ' || *p == '\t') p++;
    uint64_t val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (uint64_t)(*p - '0');
        p++;
    }
    return val;
}

static void trim_newline(char *s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            s[i] = '\0';
            break;
        }
    }
}

static void make_bar(char *out, size_t width, uint64_t used, uint64_t total) {
    if (total == 0) total = 1;
    size_t filled = (size_t)((used * width) / total);
    if (filled == 0 && used > 0) filled = 1;
    if (filled > width) filled = width;
    for (size_t i = 0; i < width; i++) {
        out[i] = (i < filled) ? '#' : '.';
    }
    out[width] = '\0';
}

int main(void) {
    struct utsname u;
    memset(&u, 0, sizeof(u));
    u_uname(&u);

    char mem_buf[512];
    char up_buf[128];
    char rtc_buf[128];
    char task_buf[1024];

    read_proc_file("/proc/meminfo", mem_buf, sizeof(mem_buf));
    read_proc_file("/proc/uptime", up_buf, sizeof(up_buf));
    read_proc_file("/proc/rtc", rtc_buf, sizeof(rtc_buf));
    read_proc_file("/proc/tasks", task_buf, sizeof(task_buf));

    trim_newline(up_buf);
    trim_newline(rtc_buf);

    uint64_t mem_total = parse_after_label(mem_buf, "MemTotal:");
    uint64_t mem_used  = parse_after_label(mem_buf, "MemUsed:");
    uint64_t kh_mapped = parse_after_label(mem_buf, "KHeapMapped:");
    uint64_t kh_used   = parse_after_label(mem_buf, "KHeapUsed:");

    int k0_tasks = 0, u3_tasks = 0;
    for (const char *p = task_buf; *p; p++) {
        if (p[0] == 'K' && p[1] == '0' && p[2] == ' ') k0_tasks++;
        if (p[0] == 'U' && p[1] == '3' && p[2] == ' ') u3_tasks++;
    }

    char ram_bar[17];
    make_bar(ram_bar, 16, mem_used, mem_total);

    printf("\n");
    printf("  \x1b[1;36m      .---.        \x1b[1;32mroot\x1b[0m@\x1b[1;36m%s\x1b[0m\n",
           u.nodename[0] ? u.nodename : "ownkrn");
    printf("  \x1b[1;36m     /     \\       \x1b[0m---------------------------\n");
    printf("  \x1b[1;36m    | \x1b[1;33m(O) (O)\x1b[1;36m |      \x1b[1;33mOS:\x1b[0m       %s %s (%s)\n",
           u.sysname, u.release, u.machine);
    printf("  \x1b[1;36m    |   \x1b[1;37m > \x1b[1;36m   |      \x1b[1;33mKernel:\x1b[0m   %s\n",
           u.version);
    printf("  \x1b[1;36m     \\ \x1b[1;32m\\___/\x1b[1;36m /       \x1b[1;33mSecurity:\x1b[0m NXE=on, WP=on, SMEP=on (W^X)\n");
    printf("  \x1b[1;36m    / `-----' \\      \x1b[1;33mUptime:\x1b[0m   %s\n",
           up_buf[0] ? up_buf : "0.000 s");
    printf("  \x1b[1;36m   /  \x1b[1;32mOWN KRN\x1b[1;36m  \\     \x1b[1;33mRTC UTC:\x1b[0m  %s\n",
           rtc_buf[0] ? rtc_buf : "N/A");
    printf("  \x1b[1;36m  /   \x1b[1;37mx86_64\x1b[1;36m    \\    \x1b[1;33mTasks:\x1b[0m    %d total (%d K0, %d U3)\n",
           k0_tasks + u3_tasks, k0_tasks, u3_tasks);
    printf("  \x1b[1;36m '-------------'   \x1b[1;33mShell:\x1b[0m    /bin/sh (history + tab-complete)\n");
    printf("                     \x1b[1;33mMemory:\x1b[0m   %llu / %llu kB [\x1b[1;32m%s\x1b[0m]\n",
           (unsigned long long)mem_used,
           (unsigned long long)mem_total,
           ram_bar);
    printf("                     \x1b[1;33mKHeap:\x1b[0m    %llu B / %llu kB mapped\n",
           (unsigned long long)kh_used,
           (unsigned long long)kh_mapped);
    printf("                     \x1b[41m   \x1b[42m   \x1b[43m   \x1b[44m   \x1b[45m   \x1b[46m   \x1b[47m   \x1b[0m\n\n");
    return 0;
}
