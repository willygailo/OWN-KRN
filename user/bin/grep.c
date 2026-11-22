#include <ulibc.h>

static void grep_fd(int fd, const char *pattern) {
    char buf[512];
    char line[512];
    size_t lpos = 0;
    ssize_t n;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || lpos + 1 >= sizeof(line)) {
                line[lpos] = '\0';
                if (strstr(line, pattern) != NULL) {
                    printf("%s\n", line);
                }
                lpos = 0;
            } else if (c != '\r') {
                line[lpos++] = c;
            }
        }
    }
    if (lpos > 0) {
        line[lpos] = '\0';
        if (strstr(line, pattern) != NULL) {
            printf("%s\n", line);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: grep <pattern> [file...]\n");
        return 1;
    }
    if (argc == 2) {
        grep_fd(0, argv[1]);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        int fd = u_open(argv[i], UAPI_O_RDONLY, 0);
        if (fd < 0) {
            printf("grep: cannot open '%s' (%d)\n", argv[i], fd);
            continue;
        }
        grep_fd(fd, argv[1]);
        u_close(fd);
    }
    return 0;
}
