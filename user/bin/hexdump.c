#include <ulibc.h>

static void dump_fd(int fd) {
    uint8_t row[16];
    uint64_t off = 0;
    size_t have = 0;

    for (;;) {
        ssize_t n = u_read(fd, row + have, sizeof(row) - have);
        if (n > 0) {
            have += (size_t)n;
        }
        if (have == 16 || (n <= 0 && have > 0)) {
            printf("%08llx  ", (unsigned long long)off);
            for (size_t i = 0; i < 16; i++) {
                if (i < have) printf("%02x ", row[i]);
                else printf("   ");
                if (i == 7) printf(" ");
            }
            printf(" |");
            for (size_t i = 0; i < have; i++) {
                char c = (row[i] >= 0x20 && row[i] <= 0x7E) ? (char)row[i] : '.';
                putchar(c);
            }
            printf("|\n");
            off += have;
            have = 0;
        }
        if (n <= 0) break;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        dump_fd(0);
        return 0;
    }
    int fd = u_open(argv[1], UAPI_O_RDONLY, 0);
    if (fd < 0) {
        printf("hexdump: cannot open '%s' (%d)\n", argv[1], fd);
        return 1;
    }
    dump_fd(fd);
    u_close(fd);
    return 0;
}
