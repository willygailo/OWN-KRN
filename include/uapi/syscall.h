#ifndef OWNKRN_UAPI_SYSCALL_H
#define OWNKRN_UAPI_SYSCALL_H

#include <stdint.h>

/* Linux x86_64 compatible syscall numbers */
#define SYS_read          0
#define SYS_write         1
#define SYS_open          2
#define SYS_close         3
#define SYS_stat          4
#define SYS_fstat         5
#define SYS_lseek         8
#define SYS_brk           12
#define SYS_ioctl         16
#define SYS_pipe          22
#define SYS_sched_yield   24
#define SYS_dup           32
#define SYS_dup2          33
#define SYS_nanosleep     35
#define SYS_getpid        39
#define SYS_fork          57
#define SYS_execve        59
#define SYS_exit          60
#define SYS_wait4         61
#define SYS_kill          62
#define SYS_uname         63
#define SYS_getdents      78
#define SYS_getcwd        79
#define SYS_chdir         80
#define SYS_mkdir         83
#define SYS_unlink        87
#define SYS_getppid       110
#define SYS_reboot        169
#define SYS_clock_gettime 228

/* Open flags */
#define UAPI_O_RDONLY   0x0000U
#define UAPI_O_WRONLY   0x0001U
#define UAPI_O_RDWR     0x0002U
#define UAPI_O_CREAT    0x0040U
#define UAPI_O_TRUNC    0x0200U
#define UAPI_O_APPEND   0x0400U

/* waitpid options */
#define UAPI_WNOHANG    1

/* Signals */
#define UAPI_SIGINT     2
#define UAPI_SIGKILL    9
#define UAPI_SIGPIPE    13
#define UAPI_SIGTERM    15

/* Clocks */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* Reboot commands */
#define OWNKRN_CMD_POWEROFF 0x4321FEDCULL
#define OWNKRN_CMD_REBOOT   0x01234567ULL

/* TTY ioctl commands & mode flags */
#define OWNKRN_TIOCGETM     0x5401U
#define OWNKRN_TIOCSETM     0x5402U

#define TTY_MODE_CANON      0x00U
#define TTY_MODE_RAW        0x01U
#define TTY_MODE_NONBLOCK   0x02U

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct uapi_dirent {
    uint64_t ino;
    uint32_t type;
    char     name[64];
};

struct uapi_stat {
    uint64_t ino;
    uint32_t type;
    uint32_t mode;
    uint64_t size;
};

#endif /* OWNKRN_UAPI_SYSCALL_H */
