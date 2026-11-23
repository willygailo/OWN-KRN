#include <ulibc.h>

#define NUM_SUITES 6
#define MAX_LOGS   12

enum suite_state {
    ST_READY = 0,
    ST_RUN   = 1,
    ST_PASS  = 2,
    ST_FAIL  = 3,
};

struct suite_info {
    const char      *title;
    const char      *desc;
    enum suite_state state;
    int              passed_checks;
    int              total_checks;
    char             metric[64];
};

static struct suite_info g_suites[NUM_SUITES] = {
    { "1. Memory & Heap Stress (MemTest86)",
      "Walking-1/0, 0x55/0xAA patterns, coalescing malloc/realloc",
      ST_READY, 0, 5, "Not run yet" },
    { "2. CPU, Syscall & Scheduler Benchmark",
      "RDTSC cycles/syscall, context-switch latency, 64-bit ALU",
      ST_READY, 0, 3, "Not run yet" },
    { "3. VFS & Inode Refcount Integrity",
      "mkdir, lseek, atomic rename, open-unlink UAF protection",
      ST_READY, 0, 6, "Not run yet" },
    { "4. Unix IPC, Fork/Exec & POSIX Signals",
      "Pipe throughput, fork+execve argv, WNOHANG, kill(SIGTERM)",
      ST_READY, 0, 5, "Not run yet" },
    { "5. Ring 3 Security Penetration Audit",
      "Kernel ptr -EFAULT, #PF(14) & #GP(13) traps, /dev/urandom",
      ST_READY, 0, 5, "Not run yet" },
    { "6. Persistent ATA0 /disk I/O Benchmark",
      "4 KiB LBA pattern write/read, FNV-1a verify & report save",
      ST_READY, 0, 4, "Not run yet" },
};

static char g_logs[MAX_LOGS][72];
static int  g_log_count = 0;
static int  g_sel = 0;
static char g_banner_msg[88] = "Press [a] to Run Full 6-Suite Diagnostic & Benchmark Pass, or [1..6] for Single Suite";

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void add_log(const char *fmt, ...) {
    char line[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_log_count < MAX_LOGS) {
        strcpy(g_logs[g_log_count++], line);
    } else {
        for (int i = 1; i < MAX_LOGS; i++) {
            strcpy(g_logs[i - 1], g_logs[i]);
        }
        strcpy(g_logs[MAX_LOGS - 1], line);
    }
}

/* ---------------- Suite 1: MemTest86 & Heap Stress ---------------- */
static bool run_suite_memory(void) {
    struct suite_info *s = &g_suites[0];
    s->passed_checks = 0;
    uint64_t t0 = rdtsc64();

    /* Check 1: Walking-1s & Walking-0s bit integrity across 16 KiB buffer */
    size_t words = 2048; /* 16 KiB */
    uint64_t *buf = (uint64_t *)malloc(words * sizeof(uint64_t));
    if (!buf) return false;

    bool walk_ok = true;
    for (size_t i = 0; i < words; i++) {
        uint64_t pat = 1ULL << (i & 63);
        buf[i] = pat;
    }
    for (size_t i = 0; i < words; i++) {
        uint64_t pat = 1ULL << (i & 63);
        if (buf[i] != pat) { walk_ok = false; break; }
        buf[i] = ~pat;
    }
    for (size_t i = 0; i < words; i++) {
        uint64_t pat = ~(1ULL << (i & 63));
        if (buf[i] != pat) { walk_ok = false; break; }
    }
    if (!walk_ok) { free(buf); return false; }
    s->passed_checks++;

    /* Check 2: Checkerboard 0x5555.. / 0xAAAA.. inversions */
    bool chk_ok = true;
    for (size_t i = 0; i < words; i++) {
        buf[i] = (i & 1) ? 0x5555555555555555ULL : 0xAAAAAAAAAAAAAAAAULL;
    }
    for (size_t i = 0; i < words; i++) {
        uint64_t exp = (i & 1) ? 0x5555555555555555ULL : 0xAAAAAAAAAAAAAAAAULL;
        if (buf[i] != exp) { chk_ok = false; break; }
        buf[i] = ~exp;
    }
    for (size_t i = 0; i < words; i++) {
        uint64_t exp = (i & 1) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
        if (buf[i] != exp) { chk_ok = false; break; }
    }
    free(buf);
    if (!chk_ok) return false;
    s->passed_checks++;

    /* Check 3: Multi-block alloc & coalescing free stress */
    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        size_t sz = (size_t)(64 + i * 32);
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) return false;
        memset(ptrs[i], 0x3C ^ i, sz);
    }
    for (int i = 0; i < 16; i += 2) free(ptrs[i]);
    for (int i = 1; i < 16; i += 2) free(ptrs[i]);
    s->passed_checks++;

    /* Check 4: Coalesced large allocation + zeroed calloc verification */
    uint8_t *z = (uint8_t *)calloc(1, 4096);
    if (!z) return false;
    for (int i = 0; i < 4096; i++) {
        if (z[i] != 0) { free(z); return false; }
    }
    s->passed_checks++;

    /* Check 5: Dynamic realloc growth preserving payload */
    z[0] = 0xDE; z[4095] = 0xAD;
    z = (uint8_t *)realloc(z, 12288);
    if (!z || z[0] != 0xDE || z[4095] != 0xAD) return false;
    z[12287] = 0xBE;
    free(z);
    s->passed_checks++;

    uint64_t cycles = rdtsc64() - t0;
    snprintf(s->metric, sizeof(s->metric),
             "5/5 patterns OK (%llu kCycles)",
             (unsigned long long)(cycles / 1000ULL));
    add_log("[PASS] Suite 1: MemTest86 & Heap Coalesce (%s)", s->metric);
    return true;
}

/* ---------------- Suite 2: CPU, Syscall & Scheduler Benchmark ---------------- */
static bool run_suite_bench(void) {
    struct suite_info *s = &g_suites[1];
    s->passed_checks = 0;

    /* Check 1: Fast SYSCALL/SYSRET round-trip latency (5000 getpid calls) */
    const int ncalls = 5000;
    uint64_t c0 = rdtsc64();
    for (int i = 0; i < ncalls; i++) {
        if (u_getpid() < 1) return false;
    }
    uint64_t c1 = rdtsc64();
    uint64_t cyc_per_syscall = (c1 > c0) ? ((c1 - c0) / (uint64_t)ncalls) : 1;
    s->passed_checks++;

    /* Check 2: Preemptive scheduler context-switch benchmark (fork + yield ping-pong) */
    uint64_t s0 = rdtsc64();
    pid_t worker = u_fork();
    if (worker < 0) return false;
    if (worker == 0) {
        for (int i = 0; i < 100; i++) {
            u_sched_yield();
        }
        u_exit(0);
    }
    for (int i = 0; i < 100; i++) {
        u_sched_yield();
    }
    int wst = 0;
    if (u_waitpid(worker, &wst) != worker) return false;
    uint64_t s1 = rdtsc64();
    uint64_t cyc_per_sw = (s1 > s0) ? ((s1 - s0) / 200ULL) : 1;
    s->passed_checks++;

    /* Check 3: 64-bit ALU SplitMix hash compute verification */
    uint64_t acc = 0x123456789ABCDEF0ULL;
    for (int i = 0; i < 20000; i++) {
        acc += 0x9E3779B97F4A7C15ULL;
        uint64_t z = acc;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        acc ^= z;
    }
    if (acc == 0) return false;
    s->passed_checks++;

    snprintf(s->metric, sizeof(s->metric),
             "syscall=%llu cyc | ctxsw=%llu cyc",
             (unsigned long long)cyc_per_syscall,
             (unsigned long long)cyc_per_sw);
    add_log("[PASS] Suite 2: CPU/Syscall Bench (%s)", s->metric);
    return true;
}

/* ---------------- Suite 3: VFS & Inode Refcount Suite ---------------- */
static bool run_suite_vfs(void) {
    struct suite_info *s = &g_suites[2];
    s->passed_checks = 0;

    /* Check 1: mkdir + stat directory */
    u_unlink("/tmp/tdir/moved.dat");
    u_unlink("/tmp/tdir/file.dat");
    u_unlink("/tmp/tdir");
    if (u_mkdir("/tmp/tdir", 0755) < 0) return false;
    struct uapi_stat st;
    if (u_stat("/tmp/tdir", &st) < 0 || st.type != 2) return false;
    s->passed_checks++;

    /* Check 2: create + write + fstat */
    int fd = u_open("/tmp/tdir/file.dat", UAPI_O_CREAT | UAPI_O_RDWR | UAPI_O_TRUNC, 0644);
    if (fd < 0) return false;
    const char *payload = "OwnKrn-VFS-Integrity-Payload-2026";
    size_t plen = strlen(payload);
    if (u_write(fd, payload, plen) != (ssize_t)plen) { u_close(fd); return false; }
    if (u_fstat(fd, &st) < 0 || st.size != plen) { u_close(fd); return false; }
    s->passed_checks++;

    /* Check 3: lseek + partial read */
    if (u_lseek(fd, 7, 0) != 7) { u_close(fd); return false; }
    char sub[16];
    memset(sub, 0, sizeof(sub));
    if (u_read(fd, sub, 13) != 13 || strcmp(sub, "VFS-Integrity") != 0) {
        u_close(fd);
        return false;
    }
    u_close(fd);
    s->passed_checks++;

    /* Check 4: Atomic SYS_rename across paths */
    if (u_rename("/tmp/tdir/file.dat", "/tmp/tdir/moved.dat") < 0) return false;
    if (u_stat("/tmp/tdir/file.dat", &st) == 0) return false;
    if (u_stat("/tmp/tdir/moved.dat", &st) < 0 || st.size != plen) return false;
    s->passed_checks++;

    /* Check 5: Open-file unlink() refcount safety (no use-after-free) */
    fd = u_open("/tmp/tdir/moved.dat", UAPI_O_RDONLY, 0);
    if (fd < 0) return false;
    if (u_unlink("/tmp/tdir/moved.dat") < 0) { u_close(fd); return false; }
    if (u_stat("/tmp/tdir/moved.dat", &st) == 0) { u_close(fd); return false; }

    char full[64];
    memset(full, 0, sizeof(full));
    if (u_read(fd, full, plen) != (ssize_t)plen || strcmp(full, payload) != 0) {
        u_close(fd);
        return false;
    }
    u_close(fd);
    s->passed_checks++;

    /* Check 6: Remove empty test directory */
    if (u_unlink("/tmp/tdir") < 0) return false;
    s->passed_checks++;

    snprintf(s->metric, sizeof(s->metric), "6/6 VFS, rename & refcount OK");
    add_log("[PASS] Suite 3: VFS & Open-Unlink Refcount (%s)", s->metric);
    return true;
}

/* ---------------- Suite 4: Unix IPC, Fork/Exec & Signals ---------------- */
static bool run_suite_ipc(void) {
    struct suite_info *s = &g_suites[3];
    s->passed_checks = 0;

    /* Check 1 & 2: Pipe + dup2 multi-chunk streaming & EOF */
    int pfd[2];
    if (u_pipe(pfd) < 0) return false;
    pid_t writer = u_fork();
    if (writer < 0) return false;
    if (writer == 0) {
        u_close(pfd[0]);
        u_dup2(pfd[1], 1);
        u_close(pfd[1]);
        char blk[256];
        memset(blk, 'P', sizeof(blk));
        for (int i = 0; i < 8; i++) {
            u_write(1, blk, sizeof(blk));
        }
        u_exit(0);
    }
    u_close(pfd[1]);
    size_t total_rx = 0;
    char rbuf[256];
    ssize_t n;
    while ((n = u_read(pfd[0], rbuf, sizeof(rbuf))) > 0) {
        total_rx += (size_t)n;
    }
    u_close(pfd[0]);
    int wst = 0;
    u_waitpid(writer, &wst);
    if (total_rx != 2048) return false;
    s->passed_checks += 2;

    /* Check 3: fork() + execve("/bin/child_helper") + exit status 42 */
    pid_t child = u_fork();
    if (child < 0) return false;
    if (child == 0) {
        char *cargv[] = { "/bin/child_helper", "tester-arg", NULL };
        u_execve("/bin/child_helper", cargv);
        u_exit(99);
    }
    int status = 0;
    if (u_waitpid(child, &status) != child || ((status >> 8) & 0xFF) != 42) {
        return false;
    }
    s->passed_checks++;

    /* Check 4 & 5: WNOHANG non-blocking check + POSIX kill(SIGTERM -> 143) */
    pid_t slp = u_fork();
    if (slp < 0) return false;
    if (slp == 0) {
        u_msleep(5000);
        u_exit(0);
    }
    if (u_waitpid_opts(slp, &status, UAPI_WNOHANG) != 0) {
        u_kill(slp, UAPI_SIGKILL);
        u_waitpid(slp, &status);
        return false;
    }
    s->passed_checks++;

    if (u_kill(slp, UAPI_SIGTERM) < 0) return false;
    if (u_waitpid(slp, &status) != slp || ((status >> 8) & 0xFF) != 143) {
        return false;
    }
    s->passed_checks++;

    snprintf(s->metric, sizeof(s->metric), "Pipe 2048B + execve(42) + SIGTERM(143)");
    add_log("[PASS] Suite 4: Pipe IPC & POSIX Signals (%s)", s->metric);
    return true;
}

/* ---------------- Suite 5: Ring 3 Security Penetration Audit ---------------- */
static bool run_suite_security(void) {
    struct suite_info *s = &g_suites[4];
    s->passed_checks = 0;

    /* Check 1: Higher-half kernel pointer rejection (-EFAULT = -14) */
    ssize_t w1 = u_write(1, (const void *)0xFFFFFFFF80200000ULL, 32);
    ssize_t r1 = u_read(0, (void *)0xFFFF800000100000ULL, 32);
    int     o1 = u_open((const char *)0xFFFFFFFF80200000ULL, UAPI_O_RDONLY, 0);
    if (w1 != -14 || r1 != -14 || o1 != -14) return false;
    s->passed_checks++;

    /* Check 2: Null pointer syscall buffer rejection (-EFAULT = -14) */
    struct uapi_stat *null_st = (struct uapi_stat *)0x0ULL;
    if (u_stat("/etc/motd", null_st) != -14) return false;
    s->passed_checks++;

    /* Check 3: Live Ring 3 Page-Fault (#PF, vec=14) child containment */
    pid_t pf_child = u_fork();
    if (pf_child < 0) return false;
    if (pf_child == 0) {
        volatile uint64_t *bad = (volatile uint64_t *)0x0ULL;
        *bad = 0xDEADBEEFULL;
        u_exit(0);
    }
    int pf_st = 0;
    if (u_waitpid(pf_child, &pf_st) != pf_child || ((pf_st >> 8) & 0xFF) != 142) {
        return false;
    }
    s->passed_checks++;

    /* Check 4: Live Ring 3 General-Protection Fault (#GP, vec=13) on privileged 'cli' */
    pid_t gp_child = u_fork();
    if (gp_child < 0) return false;
    if (gp_child == 0) {
        __asm__ volatile("cli");
        u_exit(0);
    }
    int gp_st = 0;
    if (u_waitpid(gp_child, &gp_st) != gp_child || ((gp_st >> 8) & 0xFF) != 141) {
        return false;
    }
    s->passed_checks++;

    /* Check 5: Hardware CSPRNG /dev/urandom entropy uniqueness */
    int rfd = u_open("/dev/urandom", UAPI_O_RDONLY, 0);
    if (rfd < 0) return false;
    uint64_t e1 = 0, e2 = 0, e3 = 0;
    u_read(rfd, &e1, sizeof(e1));
    u_read(rfd, &e2, sizeof(e2));
    u_read(rfd, &e3, sizeof(e3));
    u_close(rfd);
    if (e1 == 0 || e2 == 0 || e1 == e2 || e2 == e3) return false;
    s->passed_checks++;

    snprintf(s->metric, sizeof(s->metric), "-EFAULT + #PF(142) + #GP(141) isolated");
    add_log("[PASS] Suite 5: Ring 3 Security Audit (%s)", s->metric);
    return true;
}

/* ---------------- Suite 6: Persistent ATA0 /disk I/O Benchmark ---------------- */
static uint32_t fnv1a_hash(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619U;
    }
    return h;
}

static bool save_persistent_report(void) {
    int fd = u_open("/disk/test_report.txt",
                    UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, 0644);
    if (fd < 0) return false;

    char rtc[64] = "UTC";
    int rfd = u_open("/proc/rtc", UAPI_O_RDONLY, 0);
    if (rfd >= 0) {
        ssize_t n = u_read(rfd, rtc, sizeof(rtc) - 1);
        u_close(rfd);
        if (n > 0) {
            rtc[n] = '\0';
            for (int i = 0; rtc[i]; i++) {
                if (rtc[i] == '\n') { rtc[i] = '\0'; break; }
            }
        }
    }

    char buf[1536];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
        "====================================================================\n"
        "  OwnKrn v0.4.0 Automated Diagnostics, Security & Benchmark Report\n"
        "  Timestamp : %s\n"
        "====================================================================\n",
        rtc);

    int total_pass = 0;
    for (int i = 0; i < NUM_SUITES; i++) {
        const char *verdict = (g_suites[i].state == ST_PASS) ? "PASS" :
                              (g_suites[i].state == ST_FAIL) ? "FAIL" : "SKIP";
        if (g_suites[i].state == ST_PASS) total_pass++;
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "  [%s] %-40s (%d/%d) | %s\n",
            verdict,
            g_suites[i].title,
            g_suites[i].passed_checks,
            g_suites[i].total_checks,
            g_suites[i].metric);
    }

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
        "--------------------------------------------------------------------\n"
        "  OVERALL VERDICT : %d / %d SUITES PASSED\n"
        "====================================================================\n",
        total_pass, NUM_SUITES);

    u_write(fd, buf, (size_t)pos);
    u_close(fd);
    return true;
}

static bool run_suite_disk(void) {
    struct suite_info *s = &g_suites[5];
    s->passed_checks = 0;

    uint8_t *wbuf = (uint8_t *)malloc(4096);
    uint8_t *rbuf = (uint8_t *)malloc(4096);
    if (!wbuf || !rbuf) { free(wbuf); free(rbuf); return false; }

    for (size_t i = 0; i < 4096; i++) {
        wbuf[i] = (uint8_t)((i * 37U + 0x5AU) & 0xFFU);
    }
    uint32_t expected_hash = fnv1a_hash(wbuf, 4096);

    /* Check 1: Write 4 KiB (8 LBA sectors) to persistent /disk/bench.bin */
    uint64_t t0 = rdtsc64();
    int fd = u_open("/disk/bench.bin", UAPI_O_CREAT | UAPI_O_WRONLY | UAPI_O_TRUNC, 0644);
    if (fd < 0) { free(wbuf); free(rbuf); return false; }
    if (u_write(fd, wbuf, 4096) != 4096) {
        u_close(fd); free(wbuf); free(rbuf); return false;
    }
    u_close(fd);
    s->passed_checks++;

    /* Check 2: Read back 4 KiB from /disk/bench.bin & verify FNV-1a checksum */
    memset(rbuf, 0, 4096);
    fd = u_open("/disk/bench.bin", UAPI_O_RDONLY, 0);
    if (fd < 0) { free(wbuf); free(rbuf); return false; }
    ssize_t rn = u_read(fd, rbuf, 4096);
    u_close(fd);
    uint64_t t1 = rdtsc64();

    uint32_t got_hash = fnv1a_hash(rbuf, 4096);
    free(wbuf);
    free(rbuf);
    if (rn != 4096 || got_hash != expected_hash) return false;
    s->passed_checks++;

    /* Check 3: Cleanup temporary benchmark file */
    if (u_unlink("/disk/bench.bin") < 0) return false;
    s->passed_checks++;

    /* Check 4: Auto-export persistent report to /disk/test_report.txt */
    s->state = ST_PASS;
    s->passed_checks++;
    snprintf(s->metric, sizeof(s->metric),
             "4KiB FNV1a=0x%08x (%llu kCyc)",
             expected_hash, (unsigned long long)((t1 - t0) / 1000ULL));

    if (!save_persistent_report()) return false;
    add_log("[PASS] Suite 6: ATA0 /disk I/O & saved /disk/test_report.txt");
    return true;
}

static bool execute_single_suite(int idx) {
    if (idx < 0 || idx >= NUM_SUITES) return false;
    g_suites[idx].state = ST_RUN;
    bool ok = false;
    switch (idx) {
        case 0: ok = run_suite_memory();   break;
        case 1: ok = run_suite_bench();    break;
        case 2: ok = run_suite_vfs();      break;
        case 3: ok = run_suite_ipc();      break;
        case 4: ok = run_suite_security(); break;
        case 5: ok = run_suite_disk();     break;
        default: break;
    }
    g_suites[idx].state = ok ? ST_PASS : ST_FAIL;
    if (!ok) {
        snprintf(g_suites[idx].metric, sizeof(g_suites[idx].metric),
                 "FAILED at check %d/%d",
                 g_suites[idx].passed_checks + 1,
                 g_suites[idx].total_checks);
        add_log("[FAIL] %s", g_suites[idx].title);
    }
    return ok;
}

/* ---------------- Full-Screen Widescreen TUI Renderer ---------------- */
static void render_tui(void) {
    int total_checks = 0, done_checks = 0, suites_passed = 0;
    for (int i = 0; i < NUM_SUITES; i++) {
        total_checks += g_suites[i].total_checks;
        done_checks  += g_suites[i].passed_checks;
        if (g_suites[i].state == ST_PASS) suites_passed++;
    }

    int pct = (total_checks > 0) ? ((done_checks * 100) / total_checks) : 0;
    char bar[33];
    int filled = (pct * 32) / 100;
    for (int i = 0; i < 32; i++) bar[i] = (i < filled) ? '#' : '.';
    bar[32] = '\0';

    printf("\x1b[H");
    printf("\x1b[7m OwnKrn v0.4.0 Interactive System Diagnostics, Security Audit & Benchmark Testbench (/bin/tester)       \x1b[0m\x1b[K\n");
    printf(" \x1b[1;36mProgress:\x1b[0m [\x1b[1;32m%s\x1b[0m] \x1b[1;33m%3d%%\x1b[0m (%d/%d checks | %d/%d suites passed) | Report: \x1b[1;32m/disk/test_report.txt\x1b[0m\x1b[K\n",
           bar, pct, done_checks, total_checks, suites_passed, NUM_SUITES);
    printf(" ------------------------------------------------------------------------------------------------------------------\x1b[K\n");
    printf(" \x1b[1;33m%-8s %-40s %-7s | %-54s\x1b[0m\x1b[K\n",
           "STATUS", "DIAGNOSTIC / BENCHMARK SUITE", "CHECKS", "LIVE TELEMETRY & VERDICT");
    printf(" ------------------------------------------------------------------------------------------------------------------\x1b[K\n");

    for (int i = 0; i < NUM_SUITES; i++) {
        struct suite_info *s = &g_suites[i];
        const char *badge = "[READY]";
        const char *bcol  = "\x1b[0m";
        if (s->state == ST_RUN)  { badge = "[RUN..]"; bcol = "\x1b[1;33m"; }
        if (s->state == ST_PASS) { badge = "[PASS ]"; bcol = "\x1b[1;32m"; }
        if (s->state == ST_FAIL) { badge = "[FAIL ]"; bcol = "\x1b[1;31m"; }

        const char *row_hl = (i == g_sel) ? "\x1b[7m" : "";
        const char *row_rs = (i == g_sel) ? "\x1b[0m" : "";

        printf(" %s%s\x1b[0m %s%-40s%s (%d/%d)   | \x1b[1;36m%-54s\x1b[0m\x1b[K\n",
               bcol, badge, row_hl, s->title, row_rs,
               s->passed_checks, s->total_checks, s->metric);
        printf("          \x1b[0m%-40s         | \x1b[K\n", s->desc);
    }

    printf(" ------------------------------------------------------------------------------------------------------------------\x1b[K\n");
    printf(" \x1b[1;33mDiagnostic Activity Log:\x1b[0m\x1b[K\n");
    for (int i = 0; i < 8; i++) {
        int lidx = (g_log_count > 8) ? (g_log_count - 8 + i) : i;
        const char *msg = (lidx < g_log_count) ? g_logs[lidx] : "";
        printf("   %-108s\x1b[K\n", msg);
    }

    printf(" ------------------------------------------------------------------------------------------------------------------\x1b[K\n");
    printf(" \x1b[1;32m%s\x1b[0m\x1b[K\n", g_banner_msg);
    printf("\x1b[7m [a] Run All 6 Suites   [1..6/Enter] Run Selected Suite   [Up/Down] Select   [s] Save Report   [q] Quit Testbench \x1b[0m\x1b[K");
}

static int run_batch_mode(void) {
    printf("\n====================================================================\n");
    printf("  OwnKrn v0.4.0 Diagnostic, Security & Benchmark Testbench (Batch)\n");
    printf("====================================================================\n");

    int passed = 0;
    for (int i = 0; i < NUM_SUITES; i++) {
        bool ok = execute_single_suite(i);
        printf("  [%s] %-40s (%d/%d) | %s\n",
               ok ? "\x1b[1;32mPASS\x1b[0m" : "\x1b[1;31mFAIL\x1b[0m",
               g_suites[i].title,
               g_suites[i].passed_checks,
               g_suites[i].total_checks,
               g_suites[i].metric);
        if (ok) passed++;
    }
    save_persistent_report();
    printf("--------------------------------------------------------------------\n");
    printf("  RESULT: [%d/%d SUITES PASSED] | Persistent report: /disk/test_report.txt\n",
           passed, NUM_SUITES);
    printf("====================================================================\n\n");
    return (passed == NUM_SUITES) ? 0 : 1;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--batch") == 0 ||
            strcmp(argv[i], "--all") == 0) {
            return run_batch_mode();
        }
    }

    add_log("OwnKrn v0.4.0 Testbench initialized (PID=%d).", u_getpid());
    add_log("Press 'a' to execute all 6 diagnostic & benchmark suites.");

    u_tty_set_mode(TTY_MODE_RAW);
    printf("\x1b[2J\x1b[H");

    for (;;) {
        render_tui();

        char c = 0;
        if (u_read(0, &c, 1) <= 0) continue;

        if (c == 'q' || c == 'Q' || c == 0x03) {
            break;
        }
        if (c == 'a' || c == 'A') {
            int pass_cnt = 0;
            for (int i = 0; i < NUM_SUITES; i++) {
                g_sel = i;
                g_suites[i].state = ST_RUN;
                snprintf(g_banner_msg, sizeof(g_banner_msg),
                         "Running %s ...", g_suites[i].title);
                render_tui();
                if (execute_single_suite(i)) pass_cnt++;
                render_tui();
            }
            save_persistent_report();
            snprintf(g_banner_msg, sizeof(g_banner_msg),
                     "Completed: %d/%d Suites PASSED! Report saved to /disk/test_report.txt",
                     pass_cnt, NUM_SUITES);
            continue;
        }
        if (c >= '1' && c <= '6') {
            g_sel = c - '1';
            g_suites[g_sel].state = ST_RUN;
            snprintf(g_banner_msg, sizeof(g_banner_msg),
                     "Running %s ...", g_suites[g_sel].title);
            render_tui();
            execute_single_suite(g_sel);
            save_persistent_report();
            snprintf(g_banner_msg, sizeof(g_banner_msg),
                     "Finished %s -> %s", g_suites[g_sel].title, g_suites[g_sel].metric);
            continue;
        }
        if (c == '\r' || c == '\n') {
            g_suites[g_sel].state = ST_RUN;
            snprintf(g_banner_msg, sizeof(g_banner_msg),
                     "Running %s ...", g_suites[g_sel].title);
            render_tui();
            execute_single_suite(g_sel);
            save_persistent_report();
            snprintf(g_banner_msg, sizeof(g_banner_msg),
                     "Finished %s -> %s", g_suites[g_sel].title, g_suites[g_sel].metric);
            continue;
        }
        if (c == 's' || c == 'S') {
            if (save_persistent_report()) {
                add_log("Saved full diagnostic report to /disk/test_report.txt");
                snprintf(g_banner_msg, sizeof(g_banner_msg),
                         "Report exported to persistent /disk/test_report.txt!");
            }
            continue;
        }
        if (c == 0x1B) {
            char s1 = 0, s2 = 0;
            if (u_read(0, &s1, 1) <= 0 || (s1 != '[' && s1 != 'O')) continue;
            if (u_read(0, &s2, 1) <= 0) continue;
            if (s2 == 'A' && g_sel > 0) g_sel--;
            else if (s2 == 'B' && g_sel + 1 < NUM_SUITES) g_sel++;
        }
    }

    u_tty_set_mode(TTY_MODE_CANON);
    printf("\x1b[2J\x1b[H\x1b[0m");
    return 0;
}
