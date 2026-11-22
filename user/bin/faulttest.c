#include <ulibc.h>

/* Negative security test: intentionally triggers a Ring 3 #PF on NULL (0x0).
 * Kernel must contain the fault, kill only this process (exit 142), and keep /bin/sh alive. */
int main(void) {
    printf("[FAULTTEST] Deliberately dereferencing NULL in Ring 3 (pid=%d)...\n", u_getpid());
    volatile uint64_t *null_ptr = (volatile uint64_t *)0;
    *null_ptr = 0xDEADBEEFULL;
    printf("[FAULTTEST] ERROR: should have faulted!\n");
    return 1;
}
