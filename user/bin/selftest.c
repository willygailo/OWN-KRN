#include <ulibc.h>

static void check(bool ok, const char *msg) {
    if (!ok) {
        printf("[USER-FAIL] %s\n", msg);
        u_exit(1);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    pid_t pid = u_getpid();
    printf("[USER] Ring 3 selftest v0.2 running (pid=%d, ppid=%d)\n", pid, u_getppid());
    check(pid >= 1, "valid pid");

    /* 1. Coalescing malloc / calloc / realloc dynamic brk heap test */
    char *a = (char *)malloc(128);
    char *b = (char *)malloc(256);
    char *c = (char *)calloc(1, 512);
    check(a && b && c && c[0] == 0 && c[511] == 0, "malloc + calloc");
    memset(a, 0x5A, 128);
    free(a);
    free(b); /* Coalesces with a */
    char *merged = (char *)malloc(300);
    check(merged != NULL, "coalesced malloc(300)");
    merged = (char *)realloc(merged, 8192);
    check(merged != NULL, "realloc(8192)");
    merged[0] = 0x42; merged[8191] = 0x42;
    check(merged[0] == 0x42 && merged[8191] == 0x42, "heap memory r/w");
    free(merged);
    free(c);

    /* 2. Kernel pointer isolation (-EFAULT = -14) */
    ssize_t bad_w = u_write(1, (const void *)0xFFFFFFFF80200000ULL, 16);
    ssize_t bad_r = u_read(0, (void *)0xFFFFFFFF80200000ULL, 16);
    printf("[USER] Kernel-pointer syscall checks: write=%ld read=%ld (expect -14)\n",
           (long)bad_w, (long)bad_r);
    check(bad_w == -14 && bad_r == -14, "copy_from/to_user rejects kernel ptrs with -EFAULT");

    /* 3. F1: Open-file unlink() refcount safety (no use-after-free!) */
    int fd = u_open("/tmp/open_unlink.txt", UAPI_O_CREAT | UAPI_O_RDWR | UAPI_O_TRUNC, 0644);
    check(fd >= 0, "open /tmp/open_unlink.txt");
    const char *msg = "inode-refcount-safe!";
    check(u_write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg), "write before unlink");
    check(u_unlink("/tmp/open_unlink.txt") == 0, "unlink while still open");

    struct uapi_stat st;
    check(u_stat("/tmp/open_unlink.txt", &st) < 0, "unlinked path gone from dir");
    check(u_lseek(fd, 0, 0) == 0, "lseek on unlinked open fd");
    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    check(u_read(fd, rbuf, sizeof(rbuf) - 1) == (ssize_t)strlen(msg), "read unlinked open fd");
    check(strcmp(rbuf, msg) == 0, "data intact on unlinked open fd");
    check(u_close(fd) == 0, "close releases last inode ref");

    /* 4. Pipe + dup2 IPC test */
    int pfd[2];
    check(u_pipe(pfd) == 0, "pipe()");
    pid_t writer = u_fork();
    check(writer >= 0, "fork pipe writer");
    if (writer == 0) {
        u_close(pfd[0]);
        u_dup2(pfd[1], 1);
        u_close(pfd[1]);
        printf("pipe-ipc-ok");
        u_exit(0);
    }
    u_close(pfd[1]);
    memset(rbuf, 0, sizeof(rbuf));
    ssize_t pn = u_read(pfd[0], rbuf, sizeof(rbuf) - 1);
    check(pn == 11 && strcmp(rbuf, "pipe-ipc-ok") == 0, "pipe+dup2 transfer");
    check(u_read(pfd[0], rbuf, 16) == 0, "pipe EOF after writer exit");
    u_close(pfd[0]);
    int wst = 0;
    check(u_waitpid(writer, &wst) == writer && ((wst >> 8) & 0xFF) == 0, "reap pipe writer");

    /* 5. fork() + execve() + waitpid() test */
    pid_t child = u_fork();
    check(child >= 0, "fork succeeded");
    if (child == 0) {
        char *cargv[] = { "/bin/child_helper", "execve-arg-ok", NULL };
        u_execve("/bin/child_helper", cargv);
        u_exit(99);
    }
    int status = 0;
    pid_t waited = u_waitpid(child, &status);
    int exit_code = (status >> 8) & 0xFF;
    printf("[USER] waitpid(%d) -> pid=%d exit_code=%d\n", child, waited, exit_code);
    check(waited == child && exit_code == 42, "fork+execve+waitpid exit code 42");

    /* 6. POSIX kill(SIGTERM) + WNOHANG test */
    pid_t sleeper = u_fork();
    check(sleeper >= 0, "fork sleeper");
    if (sleeper == 0) {
        u_msleep(5000);
        u_exit(0);
    }
    check(u_waitpid_opts(sleeper, &status, UAPI_WNOHANG) == 0, "WNOHANG returns 0 while running");
    check(u_kill(sleeper, UAPI_SIGTERM) == 0, "kill(SIGTERM)");
    waited = u_waitpid(sleeper, &status);
    int sig_exit = (status >> 8) & 0xFF;
    printf("[USER] kill(SIGTERM) -> pid=%d exit_code=%d (expect 143)\n", waited, sig_exit);
    check(waited == sleeper && sig_exit == 128 + UAPI_SIGTERM, "child killed with 128+SIGTERM");

    /* 7. clock_gettime + uname + SYS_ioctl TTY mode round-trip */
    struct timespec rt, mono;
    check(u_clock_gettime(CLOCK_REALTIME, &rt) == 0 && rt.tv_sec > 1577836800LL, "CLOCK_REALTIME");
    check(u_clock_gettime(CLOCK_MONOTONIC, &mono) == 0, "CLOCK_MONOTONIC");

    uint32_t orig_mode = u_tty_get_mode();
    check(u_tty_set_mode(TTY_MODE_RAW | TTY_MODE_NONBLOCK) == 0, "ioctl TIOCSETM raw+nonblock");
    check(u_tty_get_mode() == (TTY_MODE_RAW | TTY_MODE_NONBLOCK), "ioctl TIOCGETM matches");
    check(u_tty_set_mode(orig_mode) == 0, "ioctl TIOCSETM restore");

    struct utsname u;
    check(u_uname(&u) == 0, "uname syscall");
    printf("[USER] uname: %s %s %s (epoch=%lld)\n",
           u.sysname, u.release, u.machine, (long long)rt.tv_sec);

    printf("[ OK ] Ring 3 selftest PASSED.\n");
    return 0;
}
