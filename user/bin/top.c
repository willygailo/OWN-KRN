#include <ulibc.h>

static int read_proc(const char *path, char *buf, size_t cap) {
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

static uint64_t parse_num_after(const char *text, const char *label) {
    const char *p = strstr(text, label);
    if (!p) return 0;
    p += strlen(label);
    while (*p == ' ' || *p == '\t') p++;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    return v;
}

static void strip_nl(char *s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            s[i] = '\0';
            break;
        }
    }
}

static void build_gauge(char *out, size_t width, uint64_t used, uint64_t total) {
    if (total == 0) total = 1;
    size_t fill = (size_t)((used * width) / total);
    if (fill == 0 && used > 0) fill = 1;
    if (fill > width) fill = width;
    for (size_t i = 0; i < width; i++) {
        out[i] = (i < fill) ? '|' : '.';
    }
    out[width] = '\0';
}

static void render_frame(bool batch_mode, const char *status_msg) {
    char mem_buf[512];
    char up_buf[128];
    char rtc_buf[128];
    char task_buf[1536];

    read_proc("/proc/meminfo", mem_buf, sizeof(mem_buf));
    read_proc("/proc/uptime", up_buf, sizeof(up_buf));
    read_proc("/proc/rtc", rtc_buf, sizeof(rtc_buf));
    read_proc("/proc/tasks", task_buf, sizeof(task_buf));

    strip_nl(up_buf);
    strip_nl(rtc_buf);
    char *ep = strstr(rtc_buf, " (epoch=");
    if (ep) *ep = '\0';

    uint64_t mem_total = parse_num_after(mem_buf, "MemTotal:");
    uint64_t mem_used  = parse_num_after(mem_buf, "MemUsed:");
    uint64_t mem_free  = parse_num_after(mem_buf, "MemFree:");
    uint64_t kh_mapped = parse_num_after(mem_buf, "KHeapMapped:");
    uint64_t kh_used   = parse_num_after(mem_buf, "KHeapUsed:");

    char ram_gauge[41];
    char kh_gauge[41];
    build_gauge(ram_gauge, 40, mem_used, mem_total);
    build_gauge(kh_gauge, 40, kh_used, kh_mapped * 1024ULL);

    if (!batch_mode) {
        printf("\x1b[H");
    }

    printf("\x1b[7m OwnKrn Live System Monitor (top) | %s | Up: %-36s \x1b[0m\x1b[K\n",
           rtc_buf[0] ? rtc_buf : "RTC UTC", up_buf);
    printf(" \x1b[1;36mMem  \x1b[0m[\x1b[1;32m%s\x1b[0m] %llu / %llu kB (Free: %llu kB)\x1b[K\n",
           ram_gauge,
           (unsigned long long)mem_used,
           (unsigned long long)mem_total,
           (unsigned long long)mem_free);
    printf(" \x1b[1;36mKHeap\x1b[0m[\x1b[1;33m%s\x1b[0m] %llu B / %llu kB mapped\x1b[K\n",
           kh_gauge,
           (unsigned long long)kh_used,
           (unsigned long long)kh_mapped);
    printf("\x1b[K\n");
    printf("\x1b[7m %-5s %-5s %-7s %-5s %-8s %-60s \x1b[0m\x1b[K\n",
           "PID", "PPID", "STATE", "RING", "TICKS", "NAME");

    /* Print each line of /proc/tasks after the header */
    const char *p = strchr(task_buf, '\n');
    int rows = 0;
    if (p) p++;
    while (p && *p && rows < 24) {
        const char *nl = strchr(p, '\n');
        char line[128];
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = '\0';

        const char *color = "\x1b[0m";
        if (strstr(line, " RUN ")) {
            color = "\x1b[1;32m";
        } else if (strstr(line, " U3 ")) {
            color = "\x1b[1;36m";
        }
        printf(" %s%-94s\x1b[0m\x1b[K\n", color, line);
        rows++;
        p = nl ? (nl + 1) : NULL;
    }

    if (!batch_mode) {
        while (rows < 24) {
            printf("\x1b[K\n");
            rows++;
        }
        printf("\x1b[7m [q] Quit   [k] Kill PID (SIGTERM)   [r/Space] Refresh   %-40s \x1b[0m\x1b[K",
               status_msg ? status_msg : "");
    }
}

static void prompt_kill_pid(char *status_out, size_t cap) {
    printf("\r\x1b[K\x1b[1;33mKill PID (Enter to confirm, Esc to cancel): \x1b[0m");
    u_tty_set_mode(TTY_MODE_RAW);

    char num[16];
    size_t len = 0;
    num[0] = '\0';

    for (;;) {
        char c = 0;
        if (u_read(0, &c, 1) <= 0) continue;
        if (c == 0x1B || c == 0x03) {
            snprintf(status_out, cap, "Kill cancelled");
            break;
        }
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                pid_t pid = (pid_t)atoi(num);
                int rc = u_kill(pid, UAPI_SIGTERM);
                if (rc == 0) {
                    snprintf(status_out, cap, "Sent SIGTERM to PID %d", pid);
                } else {
                    snprintf(status_out, cap, "Kill PID %d failed (%d)", pid, rc);
                }
            } else {
                snprintf(status_out, cap, "");
            }
            break;
        }
        if ((c == '\b' || c == 0x7F) && len > 0) {
            len--;
            num[len] = '\0';
            u_write(1, "\b \b", 3);
        } else if (c >= '0' && c <= '9' && len + 1 < sizeof(num)) {
            num[len++] = c;
            num[len] = '\0';
            u_write(1, &c, 1);
        }
    }
    u_tty_set_mode(TTY_MODE_RAW | TTY_MODE_NONBLOCK);
}

int main(int argc, char **argv) {
    bool batch_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "-n") == 0 ||
            strcmp(argv[i], "--batch") == 0) {
            batch_mode = true;
        }
    }

    if (batch_mode) {
        render_frame(true, "");
        return 0;
    }

    char status_msg[48] = "";
    u_tty_set_mode(TTY_MODE_RAW | TTY_MODE_NONBLOCK);
    printf("\x1b[2J\x1b[H");

    for (;;) {
        render_frame(false, status_msg);

        /* Poll keyboard every 50 ms across a 500 ms refresh window */
        bool quit = false;
        for (int slice = 0; slice < 10; slice++) {
            char c = 0;
            while (u_read(0, &c, 1) > 0) {
                if (c == 'q' || c == 'Q' || c == 0x03) {
                    quit = true;
                    break;
                }
                if (c == 'k' || c == 'K') {
                    prompt_kill_pid(status_msg, sizeof(status_msg));
                    slice = 10;
                    break;
                }
                if (c == 'r' || c == 'R' || c == ' ') {
                    snprintf(status_msg, sizeof(status_msg), "Refreshed");
                    slice = 10;
                    break;
                }
            }
            if (quit || slice >= 10) break;
            u_msleep(50);
        }
        if (quit) break;
    }

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[0m\n");
    return 0;
}
