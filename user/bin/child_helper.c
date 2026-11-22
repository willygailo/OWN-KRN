#include <ulibc.h>

int main(int argc, char **argv) {
    printf("[USER-CHILD] pid=%d ppid=%d argc=%d arg1=%s\n",
           u_getpid(), u_getppid(), argc, (argc > 1) ? argv[1] : "(none)");
    return 42;
}
