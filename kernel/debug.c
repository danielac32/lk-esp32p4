/*
 * Copyright (c) 2008-2014 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

/**
 * @defgroup debug  Debug
 * @{
 */

/**
 * @file
 * @brief  Debug console functions.
 */

#include <kernel/debug.h>

#include <kernel/mp.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <lk/console_cmd.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/list.h>
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmd_threads(int argc, const console_cmd_args *argv);
static int cmd_threadstats(int argc, const console_cmd_args *argv);
static int cmd_threadload(int argc, const console_cmd_args *argv);
static int cmd_ps(int argc, const console_cmd_args *argv);
static int cmd_kevlog(int argc, const console_cmd_args *argv);

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 1
STATIC_COMMAND_MASKED("threads", "list kernel threads", &cmd_threads, CMD_AVAIL_ALWAYS)
#endif
#if THREAD_STATS
STATIC_COMMAND_MASKED("threadstats", "thread level statistics", &cmd_threadstats,
                      CMD_AVAIL_ALWAYS)
STATIC_COMMAND("threadload", "toggle thread load display", &cmd_threadload)
#endif
STATIC_COMMAND_MASKED("ps", "list threads (Xinu-like)", &cmd_ps, CMD_AVAIL_ALWAYS)
#if WITH_KERNEL_EVLOG
STATIC_COMMAND_MASKED("kevlog", "dump kernel event log", &cmd_kevlog, CMD_AVAIL_ALWAYS)
#endif
STATIC_COMMAND_END(kernel);

#if LK_DEBUGLEVEL > 1
static int cmd_threads(int argc, const console_cmd_args *argv) {
    printf("thread list:\n");
    /* the crashed thread may hold the thread lock */
    if (console_in_panic_shell()) {
        dump_all_threads_unlocked();
    } else {
        dump_all_threads();
    }

    return 0;
}
#endif

static const char *ps_state_str(enum thread_state st) {
    switch (st) {
        case THREAD_SUSPENDED: return "susp";
        case THREAD_READY:     return "ready";
        case THREAD_RUNNING:   return "curr";
        case THREAD_BLOCKED:   return "wait";
        case THREAD_SLEEPING:  return "sleep";
        case THREAD_DEATH:     return "death";
        default:               return "????";
    }
}

struct ps_info {
    thread_t *t;
    char name[32];
    enum thread_state state;
    int priority;
    int curr_cpu;
    int pinned_cpu;
    size_t stack_size;
    size_t stack_used;
#if THREAD_STATS
    lk_bigtime_t runtime;
#endif
};

static size_t ps_stack_used(const thread_t *t) {
#ifdef THREAD_STACK_HIGHWATER
    if (!t || !t->stack) return 0;
    uint8_t *sb = (uint8_t *)t->stack;
    size_t i = 0;
    for (i = 0; i < t->stack_size; i++) {
        if (sb[i] != 0x99) break;
    }
    return t->stack_size - i;
#else
    (void)t;
    return 0;
#endif
}

static void ps_fill_info(struct ps_info *out, thread_t *t) {
    memset(out, 0, sizeof(*out));
    out->t = t;
    strlcpy(out->name, t->name, sizeof(out->name));
    out->state = t->state;
    out->priority = t->priority;
#if WITH_SMP
    out->curr_cpu = t->curr_cpu;
    out->pinned_cpu = t->pinned_cpu;
#else
    out->curr_cpu = -1;
    out->pinned_cpu = -1;
#endif
    out->stack_size = t->stack_size;
    out->stack_used = ps_stack_used(t);
#if THREAD_STATS
    out->runtime = t->stats.total_run_time;
#endif
}

static int cmd_ps(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
#if LK_DEBUGLEVEL > 1
    const int MAX_PS = 128;
    struct ps_info *s0 = calloc(MAX_PS, sizeof(*s0));
    struct ps_info *s1 = calloc(MAX_PS, sizeof(*s1));
    uint *pct = calloc(MAX_PS, sizeof(*pct));
    if (!s0 || !s1 || !pct) {
        free(s0); free(s1); free(pct);
        printf("ps: no memory\n");
        return -1;
    }

    if (console_in_panic_shell()) {
        /* volcado simple sin sleep */
        int n = 0;
        thread_t *t;
        list_for_every_entry(&thread_list, t, thread_t, thread_list_node) {
            if (!t || t->magic != THREAD_MAGIC || n >= MAX_PS) continue;
            ps_fill_info(&s1[n], t);
            n++;
        }
        printf("%3s %-16s %-5s %4s %4s %4s %12s\n",
               "ID", "NAME", "STATE", "PRIO", "CPU", "PIN", "STK used/tot");
        printf("%3s %-16s %-5s %4s %4s %4s %12s\n",
               "---", "----------------", "-----", "----", "----", "----", "------------");
        for (int i = 0; i < n; i++) {
            printf("%3d %-16s %-5s %4d %4d %4d %5zu/%-5zu\n",
                   i, s1[i].name, ps_state_str(s1[i].state), s1[i].priority,
                   s1[i].curr_cpu, s1[i].pinned_cpu,
                   s1[i].stack_used, s1[i].stack_size);
        }
        free(s0); free(s1); free(pct);
        return 0;
    }

    /* snapshot 0 */
    int n0 = 0;
    lk_bigtime_t t0 = current_time_hires();
    {
        THREAD_LOCK(state0);
        thread_t *t;
        list_for_every_entry(&thread_list, t, thread_t, thread_list_node) {
            if (!t || t->magic != THREAD_MAGIC || n0 >= MAX_PS) continue;
            ps_fill_info(&s0[n0], t);
            n0++;
        }
        THREAD_UNLOCK(state0);
    }

    thread_sleep(1000);

    /* snapshot 1 */
    int n1 = 0;
    lk_bigtime_t t1 = current_time_hires();
    {
        THREAD_LOCK(state1);
        thread_t *t;
        list_for_every_entry(&thread_list, t, thread_t, thread_list_node) {
            if (!t || t->magic != THREAD_MAGIC || n1 >= MAX_PS) continue;
            ps_fill_info(&s1[n1], t);
            n1++;
        }
        THREAD_UNLOCK(state1);
    }

    lk_bigtime_t elapsed = t1 - t0;
    if (elapsed <= 0) elapsed = 1;
#if THREAD_STATS
    for (int i = 0; i < n1; i++) {
        lk_bigtime_t r0 = 0;
        for (int j = 0; j < n0; j++) {
            if (s0[j].t == s1[i].t) { r0 = s0[j].runtime; break; }
        }
        lk_bigtime_t delta = 0;
        if (s1[i].runtime >= r0) delta = s1[i].runtime - r0;
        if (delta > elapsed) delta = elapsed;
        pct[i] = (uint)((delta * 10000ULL) / (uint64_t)elapsed);
        if (pct[i] > 10000) pct[i] = 10000;
    }
#endif

    printf("%3s %-16s %-5s %4s %4s %4s %7s %12s\n",
           "ID", "NAME", "STATE", "PRIO", "CPU", "PIN", "CPU%", "STK used/tot");
    printf("%3s %-16s %-5s %4s %4s %4s %7s %12s\n",
           "---", "----------------", "-----", "----", "----", "----", "-------", "------------");
    for (int i = 0; i < n1; i++) {
#if THREAD_STATS
        printf("%3d %-16s %-5s %4d %4d %4d %3u.%02u%% %5zu/%-5zu\n",
               i, s1[i].name, ps_state_str(s1[i].state), s1[i].priority,
               s1[i].curr_cpu, s1[i].pinned_cpu,
               pct[i] / 100, pct[i] % 100,
               s1[i].stack_used, s1[i].stack_size);
#else
        printf("%3d %-16s %-5s %4d %4d %4d %7s %5zu/%-5zu\n",
               i, s1[i].name, ps_state_str(s1[i].state), s1[i].priority,
               s1[i].curr_cpu, s1[i].pinned_cpu, "n/a",
               s1[i].stack_used, s1[i].stack_size);
#endif
    }

    free(s0); free(s1); free(pct);
    return 0;
#else
    printf("ps: requiere LK_DEBUGLEVEL > 1\n");
    return -1;
#endif
}

#if THREAD_STATS
static int cmd_threadstats(int argc, const console_cmd_args *argv) {
    for (uint i = 0; i < SMP_MAX_CPUS; i++) {
        if (!mp_is_cpu_active(i))
            continue;

        printf("thread stats (cpu %d):\n", i);
        printf("\ttotal idle time: %lld\n", thread_stats[i].idle_time);
        printf("\ttotal busy time: %lld\n", current_time_hires() - thread_stats[i].idle_time);
        printf("\treschedules: %lu\n", thread_stats[i].reschedules);
#if WITH_SMP
        printf("\treschedule_ipis: %lu\n", thread_stats[i].reschedule_ipis);
#endif
        printf("\tcontext_switches: %lu\n", thread_stats[i].context_switches);
        printf("\tpreempts: %lu\n", thread_stats[i].preempts);
        printf("\tyields: %lu\n", thread_stats[i].yields);
        printf("\tinterrupts: %lu\n", thread_stats[i].interrupts);
        printf("\ttimer interrupts: %lu\n", thread_stats[i].timer_ints);
        printf("\ttimers: %lu\n", thread_stats[i].timers);
    }

    /* the crashed thread may hold the thread lock */
    if (console_in_panic_shell()) {
        dump_threads_stats_unlocked();
    } else {
        dump_threads_stats();
    }
    return 0;
}

static enum handler_return threadload(struct timer *t, lk_time_t now, void *arg) {
    static struct thread_stats old_stats[SMP_MAX_CPUS];
    static lk_bigtime_t last_idle_time[SMP_MAX_CPUS];

    for (uint i = 0; i < SMP_MAX_CPUS; i++) {
        /* dont display time for inactiv cpus */
        if (!mp_is_cpu_active(i))
            continue;

        lk_bigtime_t idle_time = thread_stats[i].idle_time;

        /* if the cpu is currently idle, add the time since it went idle up until now to the idle counter */
        bool is_idle = !!mp_is_cpu_idle(i);
        if (is_idle) {
            idle_time += current_time_hires() - thread_stats[i].last_idle_timestamp;
        }

        lk_bigtime_t delta_time = idle_time - last_idle_time[i];
        lk_bigtime_t busy_time = 1000000ULL - (delta_time > 1000000ULL ? 1000000ULL : delta_time);
        uint busypercent = (busy_time * 10000) / (1000000);

        printf("cpu %u LOAD: "
               "%u.%02u%%, "
               "cs %lu, "
               "pmpts %lu, "
#if WITH_SMP
               "rs_ipis %lu, "
#endif
               "ints %lu, "
               "tmr ints %lu, "
               "tmrs %lu\n",
               i,
               busypercent / 100, busypercent % 100,
               thread_stats[i].context_switches - old_stats[i].context_switches,
               thread_stats[i].preempts - old_stats[i].preempts,
#if WITH_SMP
               thread_stats[i].reschedule_ipis - old_stats[i].reschedule_ipis,
#endif
               thread_stats[i].interrupts - old_stats[i].interrupts,
               thread_stats[i].timer_ints - old_stats[i].timer_ints,
               thread_stats[i].timers - old_stats[i].timers);

        old_stats[i] = thread_stats[i];
        last_idle_time[i] = idle_time;
    }

    return INT_NO_RESCHEDULE;
}

static int cmd_threadload(int argc, const console_cmd_args *argv) {
    static bool showthreadload = false;
    static timer_t tltimer;

    if (showthreadload == false) {
        // start the display
        timer_initialize(&tltimer);
        timer_set_periodic(&tltimer, 1000, &threadload, NULL);
        showthreadload = true;
    } else {
        timer_cancel(&tltimer);
        showthreadload = false;
    }

    return 0;
}

#endif // THREAD_STATS

#if WITH_KERNEL_EVLOG

#include <lib/evlog.h>

static evlog_t kernel_evlog;
volatile bool kernel_evlog_enable;

void kernel_evlog_init(void) {
    evlog_init(&kernel_evlog, KERNEL_EVLOG_LEN, 4);

    kernel_evlog_enable = true;
}

void kernel_evlog_add(uintptr_t id, uintptr_t arg0, uintptr_t arg1) {
    if (kernel_evlog_enable) {
        uint index = evlog_bump_head(&kernel_evlog);

        kernel_evlog.items[index] = (uintptr_t)current_time_hires();
        kernel_evlog.items[index+1] = (arch_curr_cpu_num() << 16) | id;
        kernel_evlog.items[index+2] = arg0;
        kernel_evlog.items[index+3] = arg1;
    }
}

static void kevdump_cb(const uintptr_t *i) {
    switch (i[1] & 0xffff) {
        case KERNEL_EVLOG_CONTEXT_SWITCH:
            printf("%lu.%lu: context switch from %p to %p\n", i[0], i[1] >> 16, (void *)i[2], (void *)i[3]);
            break;
        case KERNEL_EVLOG_PREEMPT:
            printf("%lu.%lu: preempt on thread %p\n", i[0], i[1] >> 16, (void *)i[2]);
            break;
        case KERNEL_EVLOG_TIMER_TICK:
            printf("%lu.%lu: timer tick\n", i[0], i[1] >> 16);
            break;
        case KERNEL_EVLOG_TIMER_CALL:
            printf("%lu.%lu: timer call %p, arg %p\n", i[0], i[1] >> 16, (void *)i[2], (void *)i[3]);
            break;
        case KERNEL_EVLOG_IRQ_ENTER:
            printf("%lu.%lu: irq entry %lu\n", i[0], i[1] >> 16, i[2]);
            break;
        case KERNEL_EVLOG_IRQ_EXIT:
            printf("%lu.%lu: irq exit  %lu\n", i[0], i[1] >> 16, i[2]);
            break;
        default:
            printf("%lu: unknown id 0x%lx 0x%lx 0x%lx\n", i[0], i[1], i[2], i[3]);
    }
}

void kernel_evlog_dump(void) {
    kernel_evlog_enable = false;
    evlog_dump(&kernel_evlog, &kevdump_cb);
    kernel_evlog_enable = true;
}

static int cmd_kevlog(int argc, const console_cmd_args *argv) {
    printf("kernel event log:\n");
    kernel_evlog_dump();

    return NO_ERROR;
}

#endif // WITH_KERNEL_EVLOG
