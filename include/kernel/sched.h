#ifndef OWNKRN_KERNEL_SCHED_H
#define OWNKRN_KERNEL_SCHED_H

#include <kernel/types.h>

#define MAX_TASKS           64
#define TASK_MAX_FDS        32
#define TASK_KSTACK_SIZE    16384
#define SCHED_QUANTUM_TICKS 2

/* waitpid options */
#define WNOHANG             1

/* Basic POSIX signals */
#define SIGINT              2
#define SIGKILL             9
#define SIGPIPE             13
#define SIGTERM             15

struct file_desc;

enum task_state {
    TASK_UNUSED   = 0,
    TASK_READY    = 1,
    TASK_RUNNING  = 2,
    TASK_SLEEPING = 3,
    TASK_BLOCKED  = 4,
    TASK_ZOMBIE   = 5,
};

/* Per-CPU structure anchored at IA32_GS_BASE (Ring 0) / IA32_KERNEL_GS_BASE (Ring 3).
 * Offsets are hardcoded in arch/x86_64/syscall.asm:
 *   offset 0: kernel_rsp (current task's kernel stack top)
 *   offset 8: user_rsp   (scratch slot for saving user RSP on SYSCALL entry)
 */
struct cpu_local {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
};

struct task {
    pid_t           pid;
    pid_t           ppid;
    char            name[32];
    enum task_state state;

    uint64_t        rsp;
    void           *kstack_base;
    uint64_t        kstack_top;
    uint64_t        cr3;

    bool            is_user;
    uint32_t        uid;
    uint32_t        gid;
    uint64_t        user_brk_start;
    uint64_t        user_brk_curr;
    uint64_t        mmap_next;

    uint64_t        wake_tick;
    uint64_t        ticks_used;
    uint32_t        slice_rem;

    int             exit_code;
    pid_t           wait_target;
    uint32_t        pending_sigs;

    char            cwd[256];
    struct file_desc *fds[TASK_MAX_FDS];
};

typedef void (*kthread_fn_t)(void *arg);

void          sched_init(void);
struct task  *sched_current(void);
struct task  *sched_get_task(pid_t pid);
struct task  *sched_task_table(size_t *out_max);
struct task  *sched_alloc_task(const char *name, pid_t ppid, uint64_t cr3, bool is_user);
void          sched_make_ready(struct task *t);

pid_t         kthread_create(const char *name, kthread_fn_t fn, void *arg);
void          sched_yield(void);
void          sleep_ms(uint64_t ms);
void          sched_tick(void);
NORETURN void task_exit(int code);
int           sched_waitpid(pid_t pid, int *status_out, int options);
void          sched_wake_parent(pid_t ppid);
void          sched_post_switch(void);

int           sched_send_signal(pid_t pid, int sig);
void          sched_signal_foreground(int sig);
void          sched_check_signals(void);

struct cpu_local *arch_cpu_local(void);

#endif /* OWNKRN_KERNEL_SCHED_H */
