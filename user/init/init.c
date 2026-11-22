#include <ulibc.h>

static void print_file(const char *path) {
    int fd = u_open(path, UAPI_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[512];
    ssize_t n;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        u_write(1, buf, (size_t)n);
    }
    u_close(fd);
}

int main(void) {
    printf("\n");
    print_file("/etc/motd");
    printf("\n");

    pid_t sh_pid = u_fork();
    if (sh_pid < 0) {
        printf("[init] fork failed (%d)\n", sh_pid);
        u_poweroff();
    }
    if (sh_pid == 0) {
        char *argv[] = { "/bin/sh", NULL };
        u_execve("/bin/sh", argv);
        printf("[init] failed to exec /bin/sh\n");
        u_exit(127);
    }

    /* Reap all reparented orphan zombies cleanly until /bin/sh exits */
    for (;;) {
        int status = 0;
        pid_t reaped = u_waitpid(-1, &status);
        if (reaped == sh_pid) {
            printf("[init] /bin/sh exited (code=%d). Shutting down.\n",
                   (status >> 8) & 0xFF);
            break;
        }
        if (reaped < 0) {
            break;
        }
    }

    u_poweroff();
    return 0;
}
