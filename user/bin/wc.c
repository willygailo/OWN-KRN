#include <ulibc.h>

static void wc_fd(int fd, const char *label) {
    char buf[512];
    uint64_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    ssize_t n;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
        }
    }

    if (label) {
        printf("%6llu %6llu %6llu %s\n",
               (unsigned long long)lines,
               (unsigned long long)words,
               (unsigned long long)bytes,
               label);
    } else {
        printf("%6llu %6llu %6llu\n",
               (unsigned long long)lines,
               (unsigned long long)words,
               (unsigned long long)bytes);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        wc_fd(0, NULL);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = u_open(argv[i], UAPI_O_RDONLY, 0);
        if (fd < 0) {
            printf("wc: cannot open '%s' (%d)\n", argv[i], fd);
            continue;
        }
        wc_fd(fd, argv[i]);
        u_close(fd);
    }
    return 0;
}
