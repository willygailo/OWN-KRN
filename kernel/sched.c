#include <arch/gdt.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/sync.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/vmm.h>

#define MSR_GS_BASE        0xC0000101U
#define MSR_KERNEL_GS_BASE 0xC0000102U

extern char kernel_boot_stack_bottom[];
extern char kernel_boot_stack_top[];

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);
extern void kthread_trampoline(void);

typedef void (*task_close_fds_hook_t)(struct task *t);
static task_close_fds_hook_t g_close_fds_hook = NULL;

void sched_set_close_fds_hook(task_close_fds_hook_t hook) {
    g_close_fds_hook = hook;
}

static struct cpu_local g_cpu_local ALIGNED(16);
static struct task      g_tasks[MAX_TASKS];
static struct task     *g_current = NULL;
static pid_t            g_next_pid = 1;
static size_t           g_rr_cursor = 0;
static spinlock_t       g_sched_lock = SPINLOCK_INIT;
static bool             g_sched_ready = false;

struct cpu_local *arch_cpu_local(void) {
    return &g_cpu_local;
}

void sched_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));
    memset(&g_cpu_local, 0, sizeof(g_cpu_local));

    g_cpu_local.kernel_rsp = (uint64_t)(uintptr_t)kernel_boot_stack_top;
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)&g_cpu_local);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    struct task *t0 = &g_tasks[0];
    t0->pid         = 0;
    t0->ppid        = 0;
    strcpy(t0->name, "kidle");
    t0->state       = TASK_RUNNING;
    t0->kstack_base = kernel_boot_stack_bottom;
    t0->kstack_top  = (uint64_t)(uintptr_t)kernel_boot_stack_top;
    t0->cr3         = vmm_kernel_pml4();
    t0->is_user     = false;
    t0->slice_rem   = SCHED_QUANTUM_TICKS;
    strcpy(t0->cwd, "/");

    g_current = t0;
    g_next_pid = 1;
    g_rr_cursor = 0;
    g_sched_ready = true;

    pit_set_tick_hook(sched_tick);
    pr_ok("Scheduler initialized (preemptive RR, quantum=%u ticks, GS base configured)\n",
          SCHED_QUANTUM_TICKS);
}

struct task *sched_current(void) {
    return g_current;
}

struct task *sched_get_task(pid_t pid) {
    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state != TASK_UNUSED && g_tasks[i].pid == pid) {
            return &g_tasks[i];
        }
    }
    return NULL;
}

struct task *sched_task_table(size_t *out_max) {
    if (out_max) {
        *out_max = MAX_TASKS;
    }
    return g_tasks;
}

struct task *sched_alloc_task(const char *name, pid_t ppid, uint64_t cr3, bool is_user) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    struct task *slot = NULL;
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) {
            slot = &g_tasks[i];
            break;
        }
    }
    if (!slot) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->pid       = g_next_pid++;
    slot->ppid      = ppid;
    slot->state     = TASK_BLOCKED;
    slot->cr3       = cr3 ? cr3 : vmm_kernel_pml4();
    slot->is_user   = is_user;
    slot->slice_rem = SCHED_QUANTUM_TICKS;
    strncpy(slot->name, name ? name : "task", sizeof(slot->name) - 1);
    strcpy(slot->cwd, "/");
    spin_unlock_irqrestore(&g_sched_lock, flags);

    void *stack = kmalloc(TASK_KSTACK_SIZE);
    if (!stack) {
        uint64_t f2 = spin_lock_irqsave(&g_sched_lock);
        slot->state = TASK_UNUSED;
        spin_unlock_irqrestore(&g_sched_lock, f2);
        return NULL;
    }
    memset(stack, 0, TASK_KSTACK_SIZE);
    slot->kstack_base = stack;
    slot->kstack_top  = ALIGN_DOWN((uint64_t)(uintptr_t)stack + TASK_KSTACK_SIZE, 16);
    return slot;
}

void sched_make_ready(struct task *t) {
    if (!t) return;
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    if (t->state != TASK_UNUSED && t->state != TASK_ZOMBIE) {
        t->state = TASK_READY;
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

pid_t kthread_create(const char *name, kthread_fn_t fn, void *arg) {
    struct task *t = sched_alloc_task(name, g_current ? g_current->pid : 0,
                                      vmm_kernel_pml4(), false);
    if (!t) {
        return -1;
    }

    uint64_t *sp = (uint64_t *)(uintptr_t)t->kstack_top;
    *--sp = (uint64_t)(uintptr_t)kthread_trampoline;
    *--sp = 0; /* rbp */
    *--sp = 0; /* rbx */
    *--sp = (uint64_t)(uintptr_t)fn;  /* r12 */
    *--sp = (uint64_t)(uintptr_t)arg; /* r13 */
    *--sp = 0; /* r14 */
    *--sp = 0; /* r15 */
    t->rsp = (uint64_t)(uintptr_t)sp;

    sched_make_ready(t);
    return t->pid;
}

void sched_post_switch(void) {
    spin_unlock(&g_sched_lock);
    irq_enable();
}

static void wake_sleepers_unlocked(uint64_t now) {
    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_SLEEPING &&
            (now >= g_tasks[i].wake_tick || g_tasks[i].pending_sigs != 0)) {
            g_tasks[i].state = TASK_READY;
        }
    }
}

static struct task *pick_next_unlocked(void) {
    for (size_t step = 1; step <= MAX_TASKS - 1; step++) {
        size_t idx = ((g_rr_cursor + step - 1) % (MAX_TASKS - 1)) + 1;
        if (g_tasks[idx].state == TASK_READY) {
            g_rr_cursor = idx;
            return &g_tasks[idx];
        }
    }
    if (g_current && g_current->pid != 0 && g_current->state == TASK_READY) {
        return g_current;
    }
    if (g_tasks[0].state == TASK_READY || g_tasks[0].state == TASK_RUNNING) {
        return &g_tasks[0];
    }
    return g_current;
}

static void schedule_unlocked(void) {
    wake_sleepers_unlocked(pit_ticks());

    struct task *prev = g_current;
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }

    struct task *next = pick_next_unlocked();
    next->state = TASK_RUNNING;
    next->slice_rem = SCHED_QUANTUM_TICKS;

    if (next == prev) {
        return;
    }

    g_current = next;
    tss_set_rsp0(next->kstack_top);
    g_cpu_local.kernel_rsp = next->kstack_top;

    switch_context(&prev->rsp, next->rsp, next->cr3);
}

void sched_yield(void) {
    if (!g_sched_ready) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    schedule_unlocked();
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sleep_ms(uint64_t ms) {
    uint64_t ticks = (ms * PIT_HZ + 999) / 1000;
    if (ticks == 0) {
        ticks = 1;
    }
    uint64_t target = pit_ticks() + ticks;

    if (!g_sched_ready || g_current->pid == 0) {
        while (pit_ticks() < target) {
            cpu_halt();
        }
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    if (g_current->pending_sigs == 0) {
        g_current->wake_tick = target;
        g_current->state = TASK_SLEEPING;
        schedule_unlocked();
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sched_tick(void) {
    if (!g_sched_ready || !g_current) {
        return;
    }
    if (g_sched_lock.locked) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    g_current->ticks_used++;
    wake_sleepers_unlocked(pit_ticks());

    if (g_current->slice_rem > 0) {
        g_current->slice_rem--;
    }
    if (g_current->slice_rem == 0 || g_current->pid == 0) {
        schedule_unlocked();
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sched_wake_parent(pid_t ppid) {
    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_BLOCKED && g_tasks[i].pid == ppid) {
            g_tasks[i].state = TASK_READY;
        }
    }
}

NORETURN void task_exit(int code) {
    struct task *cur = g_current;
    KASSERT(cur != NULL && cur->pid != 0);

    if (g_close_fds_hook) {
        g_close_fds_hook(cur);
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    cur->exit_code = code;
    cur->state = TASK_ZOMBIE;

    /* Reparent any children (including zombies) to PID 1 (init) or PID 0 */
    bool wake_init = false;
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state != TASK_UNUSED && g_tasks[i].ppid == cur->pid) {
            g_tasks[i].ppid = (cur->pid == 1) ? 0 : 1;
            if (g_tasks[i].state == TASK_ZOMBIE && g_tasks[i].ppid == 1) {
                wake_init = true;
            }
        }
    }
    sched_wake_parent(cur->ppid);
    if (wake_init) {
        sched_wake_parent(1);
    }

    schedule_unlocked();
    spin_unlock_irqrestore(&g_sched_lock, flags);

    kpanic("task_exit returned unexpectedly for pid=%d", cur->pid);
}

int sched_waitpid(pid_t target_pid, int *status_out, int options) {
    struct task *cur = g_current;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_sched_lock);
        bool has_matching_child = false;
        struct task *zombie = NULL;

        for (size_t i = 1; i < MAX_TASKS; i++) {
            struct task *t = &g_tasks[i];
            if (t->state == TASK_UNUSED) continue;
            if (t->ppid != cur->pid) continue;
            if (target_pid > 0 && t->pid != target_pid) continue;

            has_matching_child = true;
            if (t->state == TASK_ZOMBIE) {
                zombie = t;
                break;
            }
        }

        if (zombie) {
            pid_t zpid    = zombie->pid;
            int   zcode   = zombie->exit_code;
            uint64_t zcr3 = zombie->cr3;
            void *zkstack = zombie->kstack_base;
            bool  zuser   = zombie->is_user;

            zombie->state       = TASK_UNUSED;
            zombie->kstack_base = NULL;
            spin_unlock_irqrestore(&g_sched_lock, flags);

            if (zuser && zcr3 && zcr3 != vmm_kernel_pml4()) {
                vmm_destroy_address_space(zcr3);
            }
            if (zkstack) {
                kfree(zkstack);
            }
            if (status_out) {
                *status_out = zcode;
            }
            return zpid;
        }

        if (!has_matching_child) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return -ECHILD;
        }

        if (options & WNOHANG) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return 0;
        }

        if (cur->pending_sigs != 0) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return -EINTR;
        }

        if (cur->pid == 0) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            irq_enable();
            cpu_halt();
        } else {
            cur->wait_target = target_pid;
            cur->state = TASK_BLOCKED;
            schedule_unlocked();
            spin_unlock_irqrestore(&g_sched_lock, flags);
        }
    }
}

int sched_send_signal(pid_t pid, int sig) {
    if (sig <= 0 || sig >= 32) {
        return -EINVAL;
    }
    if (pid <= 1) {
        return -EPERM; /* Protect kidle (0) and init (1) */
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    struct task *target = NULL;
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state != TASK_UNUSED && g_tasks[i].pid == pid) {
            target = &g_tasks[i];
            break;
        }
    }
    if (!target || target->state == TASK_ZOMBIE) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return -ESRCH;
    }

    target->pending_sigs |= (1U << (uint32_t)sig);
    if (target->state == TASK_SLEEPING || target->state == TASK_BLOCKED) {
        target->state = TASK_READY;
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return 0;
}

void sched_signal_foreground(int sig) {
    if (sig <= 0 || sig >= 32) return;
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    /* Deliver Ctrl+C to the highest-PID active user child above init & sh (pid > 2) */
    struct task *victim = NULL;
    for (size_t i = 1; i < MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];
        if (!t->is_user || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
        if (t->pid <= 2) continue; /* Keep init (1) and shell (2) alive on Ctrl+C */
        if (!victim || t->pid > victim->pid) {
            victim = t;
        }
    }
    if (victim) {
        victim->pending_sigs |= (1U << (uint32_t)sig);
        if (victim->state == TASK_SLEEPING || victim->state == TASK_BLOCKED) {
            victim->state = TASK_READY;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sched_check_signals(void) {
    struct task *cur = g_current;
    if (!cur || !cur->is_user || cur->pending_sigs == 0) {
        return;
    }
    uint32_t sigs = cur->pending_sigs;
    cur->pending_sigs = 0;

    for (int s = 1; s < 32; s++) {
        if (sigs & (1U << (uint32_t)s)) {
            if (s == SIGKILL || s == SIGTERM || s == SIGINT || s == SIGPIPE) {
                task_exit(128 + s);
            }
        }
    }
}
