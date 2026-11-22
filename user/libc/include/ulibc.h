#ifndef OWNKRN_USER_ULIBC_H
#define OWNKRN_USER_ULIBC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/syscall.h>

typedef int64_t ssize_t;
typedef int64_t off_t;
typedef int32_t pid_t;

int64_t u_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6);

/* Syscall wrappers */
ssize_t u_read(int fd, void *buf, size_t len);
ssize_t u_write(int fd, const void *buf, size_t len);
int     u_open(const char *path, uint32_t flags, uint32_t mode);
int     u_close(int fd);
int     u_stat(const char *path, struct uapi_stat *st);
int     u_fstat(int fd, struct uapi_stat *st);
off_t   u_lseek(int fd, off_t off, int whence);
void   *u_sbrk(intptr_t inc);
pid_t   u_getpid(void);
pid_t   u_getppid(void);
pid_t   u_fork(void);
int     u_execve(const char *path, char *const argv[]);
void    u_exit(int code) __attribute__((noreturn));
pid_t   u_waitpid(pid_t pid, int *status);
int     u_uname(struct utsname *u);
int     u_getdents(int fd, struct uapi_dirent *ents, size_t max_ents);
int     u_getcwd(char *buf, size_t size);
int     u_chdir(const char *path);
int     u_mkdir(const char *path, uint32_t mode);
int     u_unlink(const char *path);
int     u_msleep(uint64_t ms);
void    u_poweroff(void);
void    u_reboot(void);

/* Memory & string utilities */
void   *malloc(size_t size);
void    free(void *ptr);
void   *memset(void *dst, int c, size_t n);
void   *memcpy(void *dst, const void *src, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strchr(const char *s, int c);

/* Formatted I/O */
int     vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int     snprintf(char *buf, size_t cap, const char *fmt, ...);
int     printf(const char *fmt, ...);
int     puts(const char *s);
void    putchar(char c);

#endif /* OWNKRN_USER_ULIBC_H */
