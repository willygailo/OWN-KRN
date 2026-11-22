#include <ulibc.h>

#define SH_LINE_MAX   512
#define SH_MAX_ARGS   32
#define SH_MAX_STAGES 8

static void write_str_fd(int fd, const char *s) {
    u_write(fd, s, strlen(s));
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
        "OwnKrn Shell (/bin/sh) commands:\n"
        "  help                Show this help\n"
        "  uname [-a]          Print kernel & machine info\n"
        "  pwd / cd [dir]      Print or change working directory\n"
        "  ls [dir]            List directory entries\n"
        "  cat [file...]       Display file(s) or stdin\n"
        "  echo [args...]      Print text\n"
        "  mkdir / touch / rm  Create/remove files & directories\n"
        "  ps / free / uptime  Inspect /proc/tasks, meminfo, uptime\n"
        "  date                Display CMOS RTC date & epoch\n"
        "  kill <pid> [sig]    Send POSIX signal (default SIGTERM=15)\n"
        "  clear               Clear terminal\n"
        "  halt | reboot | exit Power management / exit shell\n"
        "  Pipelines & Redir:  cmd1 | cmd2, < in, > out, >> append\n"
        "  /bin ELF64 apps:    hello, grep, wc, hexdump, date, sleep, selftest, faulttest\n");
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

static bool is_stateful_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 ||
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
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(argc, argv, out_fd);
    } else if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            copy_fd(in_fd, out_fd);
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
                /* Split glued redirect without losing next char by shifting */
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

static bool resolve_exec_path(const char *cmd, char *out, size_t cap) {
    if (cmd[0] == '/' || cmd[0] == '.') {
        strncpy(out, cmd, cap - 1);
        out[cap - 1] = '\0';
    } else {
        snprintf(out, cap, "/bin/%s", cmd);
    }
    struct uapi_stat st;
    return u_stat(out, &st) == 0 && st.type == 1;
}

static void execute_line(char *line) {
    /* Split into pipeline stages on '|' outside double quotes */
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

    /* Fast path: single stateful built-in (cd/exit/halt/reboot) must run in parent */
    if (nstages == 1 && is_stateful_builtin(stages[0].argv[0])) {
        run_builtin(stages[0].argc, stages[0].argv, 0, 1);
        return;
    }

    /* Fast path: single non-stateful built-in with optional redirection */
    if (nstages == 1) {
        struct stage *s = &stages[0];
        char exec_path[256];
        /* Prefer built-in if it matches; otherwise fall through to fork+execve */
        int in_fd = 0, out_fd = 1;
        bool close_in = false, close_out = false;

        if (!resolve_exec_path(s->argv[0], exec_path, sizeof(exec_path)) ||
            strcmp(s->argv[0], "echo") == 0 || strcmp(s->argv[0], "cat") == 0 ||
            strcmp(s->argv[0], "ls") == 0 || strcmp(s->argv[0], "ps") == 0 ||
            strcmp(s->argv[0], "free") == 0 || strcmp(s->argv[0], "uname") == 0 ||
            strcmp(s->argv[0], "uptime") == 0 || strcmp(s->argv[0], "pwd") == 0 ||
            strcmp(s->argv[0], "help") == 0 || strcmp(s->argv[0], "mkdir") == 0 ||
            strcmp(s->argv[0], "touch") == 0 || strcmp(s->argv[0], "rm") == 0 ||
            strcmp(s->argv[0], "kill") == 0 || strcmp(s->argv[0], "clear") == 0) {
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

    /* General pipeline / external ELF64 execution path using pipe() + fork() + dup2() */
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
            /* Wire stdin from previous pipe or '<' file */
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

            /* Wire stdout to next pipe or '>' / '>>' file */
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

            /* Try built-in inside pipeline stage first (e.g. ps | grep U3) */
            if (run_builtin(s->argc, s->argv, 0, 1)) {
                u_exit(0);
            }

            /* Otherwise exec external Ring 3 ELF64 binary */
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
        if (n == 0) {
            /* Ctrl-D EOF */
            printf("\n");
            u_exit(0);
        }
        if (n < 0) {
            continue;
        }
        line[n] = '\0';
        execute_line(line);
    }
    return 0;
}
