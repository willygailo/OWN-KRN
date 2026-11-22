#include <ulibc.h>

int main(int argc, char **argv) {
    const char *who = (argc > 1) ? argv[1] : "OwnKrn";
    printf("Hello, %s! (from Ring 3 ELF, pid=%d, ppid=%d)\n",
           who, u_getpid(), u_getppid());
    return 0;
}
