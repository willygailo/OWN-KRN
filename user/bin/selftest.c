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
    printf("[USER] Ring 3 selftest running (pid=%d, ppid=%d)\n", pid, u_getppid());
    check(pid >= 1, "valid pid");

    /* 1. brk / malloc dynamic heap test */
    char *heap_buf = (char *)malloc(8192);
    check(heap_buf != NULL, "brk malloc(8192)");
    memset(heap_buf, 0x5A, 8192);
    check(heap_buf[0] == 0x5A && heap_buf[8191] == 0x5A, "heap memory r/w");
    free(heap_buf);

    /* 2. Kernel pointer isolation (-EFAULT = -14) test */
    ssize_t bad_w = u_write(1, (const void *)0xFFFFFFFF80200000ULL, 16);
    ssize_t bad_r = u_read(0, (void *)0xFFFFFFFF80200000ULL, 16);
    printf("[USER] Kernel-pointer syscall checks: write=%ld read=%ld (expect -14)\n",
           (long)bad_w, (long)bad_r);
    check(bad_w == -14 && bad_r == -14, "copy_from/to_user rejects kernel ptrs with -EFAULT");

    /* 3. Ring 3 VFS CRUD test */
    int fd = u_open("/tmp/user_check.txt", UAPI_O_CREAT | UAPI_O_RDWR | UAPI_O_TRUNC, 0644);
    check(fd >= 0, "open /tmp/user_check.txt");
    const char *msg = "ring3-vfs-ok";
    check(u_write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg), "write user file");
    check(u_lseek(fd, 0, 0) == 0, "lseek user file");
    char rbuf[32];
    memset(rbuf, 0, sizeof(rbuf));
    check(u_read(fd, rbuf, sizeof(rbuf) - 1) == (ssize_t)strlen(msg), "read user file");
    check(strcmp(rbuf, msg) == 0, "verify user file contents");
    check(u_close(fd) == 0, "close user file");
    check(u_unlink("/tmp/user_check.txt") == 0, "unlink user file");

    /* 4. fork() + execve() + waitpid() test */
    pid_t child = u_fork();
    check(child >= 0, "fork succeeded");
    if (child == 0) {
        char *cargv[] = { "/bin/child_helper", "execve-arg-ok", NULL };
        u_execve("/bin/child_helper", cargv);
        printf("[USER-FAIL] execve returned!\n");
        u_exit(99);
    }

    int status = 0;
    pid_t waited = u_waitpid(child, &status);
    int exit_code = (status >> 8) & 0xFF;
    printf("[USER] waitpid(%d) -> pid=%d exit_code=%d\n", child, waited, exit_code);
    check(waited == child && exit_code == 42, "fork+execve+waitpid exit code 42");

    /* 5. uname syscall test */
    struct utsname u;
    check(u_uname(&u) == 0, "uname syscall");
    printf("[USER] uname: %s %s %s\n", u.sysname, u.release, u.machine);

    printf("[ OK ] Ring 3 selftest PASSED.\n");
    return 0;
}
