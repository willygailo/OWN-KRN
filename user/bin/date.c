#include <ulibc.h>

int main(void) {
    int fd = u_open("/proc/rtc", UAPI_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[128];
        ssize_t n = u_read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            u_write(1, buf, (size_t)n);
        }
        u_close(fd);
        return 0;
    }
    struct timespec ts;
    if (u_clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        printf("epoch=%lld\n", (long long)ts.tv_sec);
        return 0;
    }
    printf("date: failed to read RTC\n");
    return 1;
}
