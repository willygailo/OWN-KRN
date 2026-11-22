#include <ulibc.h>

#define SH_LINE_MAX 512
#define SH_MAX_ARGS 32

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

static void write_str_fd(int fd, const char *s) {
    u_write(fd, s, strlen(s));
}

static void cmd_help(int out_fd) {
    write_str_fd(out_fd,
        "OwnKrn Shell (/bin/sh) commands:\n"
        "  help              Show this help\n"
        "  uname [-a]        Print kernel & machine info\n"
        "  pwd               Print working directory\n"
        "  cd [dir]          Change directory\n"
        "  ls [dir]          List directory entries\n"
        "  cat <file...>     Display file contents\n"
        "  echo [args...]    Print text (supports > and >> redirection)\n"
        "  mkdir <dir>       Create directory\n"
        "  touch <file>      Create empty file\n"
        "  rm <path>         Remove file or empty directory\n"
        "  ps                Show running tasks (/proc/tasks)\n"
        "  free | mem        Show memory statistics (/proc/meminfo)\n"
        "  uptime            Show system uptime (/proc/uptime)\n"
        "  clear             Clear terminal\n"
        "  halt | poweroff   Shut down machine cleanly\n"
        "  reboot            Reboot machine\n"
        "  exit              Exit shell\n"
        "  <prog> [args...]  Execute ELF64 binary from /bin or path (e.g. hello, selftest)\n");
}

static void cmd_ls(int argc, char **argv, int out_fd) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    int fd = u_open(dir, UAPI_O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot open '%s' (err=%d)\n", dir, fd);
        return;
    }

    struct uapi_dirent ents[32];
    int n;
    while ((n = u_getdents(fd, ents, 32)) > 0) {
        for (int i = 0; i < n; i++) {
            char line[128];
            const char *tag = (ents[i].type == 2) ? "<DIR> " :
                              (ents[i].type == 3) ? "<DEV> " : "<FILE>";
            snprintf(line, sizeof(line), "  %s  %s%s\n",
                     tag, ents[i].name, (ents[i].type == 2) ? "/" : "");
            write_str_fd(out_fd, line);
        }
    }
    u_close(fd);
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

static void run_external(int argc, char **argv) {
    (void)argc;
    char exec_path[256];
    if (argv[0][0] == '/' || argv[0][0] == '.') {
        strncpy(exec_path, argv[0], sizeof(exec_path) - 1);
        exec_path[sizeof(exec_path) - 1] = '\0';
    } else {
        snprintf(exec_path, sizeof(exec_path), "/bin/%s", argv[0]);
    }

    struct uapi_stat st;
    if (u_stat(exec_path, &st) < 0 || st.type != 1) {
        printf("sh: command not found: %s\n", argv[0]);
        return;
    }

    pid_t pid = u_fork();
    if (pid < 0) {
        printf("sh: fork failed (%d)\n", pid);
        return;
    }
    if (pid == 0) {
        int rc = u_execve(exec_path, argv);
        printf("sh: execve '%s' failed (%d)\n", exec_path, rc);
        u_exit(127);
    }

    int status = 0;
    u_waitpid(pid, &status);
}

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }
    }
    argv[argc] = NULL;
    return argc;
}

static void execute_line(char *line) {
    char *argv[SH_MAX_ARGS];
    int argc = tokenize(line, argv, SH_MAX_ARGS);
    if (argc == 0) return;

    /* Parse simple stdout redirection (> file or >> file) */
    int out_fd = 1;
    bool close_out = false;
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) && i + 1 < argc) {
            uint32_t flags = UAPI_O_CREAT | UAPI_O_WRONLY |
                             ((argv[i][1] == '>') ? UAPI_O_APPEND : UAPI_O_TRUNC);
            int fd = u_open(argv[i + 1], flags, 0644);
            if (fd < 0) {
                printf("sh: cannot open '%s' for write (err=%d)\n", argv[i + 1], fd);
                return;
            }
            out_fd = fd;
            close_out = true;
            argc = i;
            argv[argc] = NULL;
            break;
        }
    }
    if (argc == 0) {
        if (close_out) u_close(out_fd);
        return;
    }

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
        if (rc < 0) {
            printf("cd: %s: error %d\n", target, rc);
        }
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(argc, argv, out_fd);
    } else if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            printf("usage: cat <file...>\n");
        } else {
            for (int i = 1; i < argc; i++) {
                print_file_to_fd(argv[i], out_fd);
            }
        }
    } else if (strcmp(cmd, "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) write_str_fd(out_fd, " ");
            write_str_fd(out_fd, argv[i]);
        }
        write_str_fd(out_fd, "\n");
    } else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 2) {
            printf("usage: mkdir <dir>\n");
        } else {
            int rc = u_mkdir(argv[1], 0755);
            if (rc < 0) printf("mkdir: %s: error %d\n", argv[1], rc);
        }
    } else if (strcmp(cmd, "touch") == 0) {
        if (argc < 2) {
            printf("usage: touch <file>\n");
        } else {
            int fd = u_open(argv[1], UAPI_O_CREAT | UAPI_O_WRONLY, 0644);
            if (fd < 0) printf("touch: %s: error %d\n", argv[1], fd);
            else u_close(fd);
        }
    } else if (strcmp(cmd, "rm") == 0) {
        if (argc < 2) {
            printf("usage: rm <path>\n");
        } else {
            int rc = u_unlink(argv[1]);
            if (rc < 0) printf("rm: %s: error %d\n", argv[1], rc);
        }
    } else if (strcmp(cmd, "ps") == 0) {
        print_file_to_fd("/proc/tasks", out_fd);
    } else if (strcmp(cmd, "free") == 0 || strcmp(cmd, "mem") == 0) {
        print_file_to_fd("/proc/meminfo", out_fd);
    } else if (strcmp(cmd, "uptime") == 0) {
        print_file_to_fd("/proc/uptime", out_fd);
    } else if (strcmp(cmd, "clear") == 0) {
        write_str_fd(out_fd, "\x1b[2J\x1b[H");
    } else if (strcmp(cmd, "halt") == 0 || strcmp(cmd, "poweroff") == 0) {
        printf("Powering off OwnKrn...\n");
        u_poweroff();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("Rebooting OwnKrn...\n");
        u_reboot();
    } else if (strcmp(cmd, "exit") == 0) {
        if (close_out) u_close(out_fd);
        u_exit(0);
    } else {
        run_external(argc, argv);
    }

    if (close_out) {
        u_close(out_fd);
    }
}

int main(void) {
    char line[SH_LINE_MAX];
    char cwd[256];

    for (;;) {
        if (u_getcwd(cwd, sizeof(cwd)) <= 0) {
            strcpy(cwd, "/");
        }
        printf("ownkrn:%s# ", cwd);

        ssize_t n = u_read(0, line, sizeof(line) - 1);
        if (n <= 0) {
            continue;
        }
        line[n] = '\0';
        execute_line(line);
    }
    return 0;
}
