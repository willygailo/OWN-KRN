#include <ulibc.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sleep <ms>\n");
        return 1;
    }
    int ms = atoi(argv[1]);
    if (ms > 0) {
        u_msleep((uint64_t)ms);
    }
    return 0;
}
