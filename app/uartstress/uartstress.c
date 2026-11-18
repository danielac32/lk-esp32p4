/*
 * LK app: prueba de estres productor-consumidor SMP sobre UART.
 * Unico dispositivo: consola/UART (printf). Primitivas LK: hilos, semaforos,
 * mutex, spinlock, malloc/free. printf de LK serializa con print_spin_lock.
 * No autostart: ejecutar desde shell con "app start uartstress" o "start uartstress".
 */
#include <app.h>
#include <kernel/mutex.h>
#include <kernel/semaphore.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_CPUS        2
#define NUM_PRODUCERS   2
#define NUM_CONSUMERS   2
#define QUEUE_CAP       8
#define ITEMS_PER_PROD  20
#define TOTAL_ITEMS     (NUM_PRODUCERS * ITEMS_PER_PROD)

typedef struct item {
    int producer;
    int seq;
} item_t;

static item_t *qbuf[QUEUE_CAP];
static int qh, qt;

static semaphore_t sem_empty;
static semaphore_t sem_full;
static mutex_t qlock;

static spin_lock_t slock = SPIN_LOCK_INITIAL_VALUE;
static volatile int produced;
static volatile int consumed;

static void q_put(item_t *it) {
    sem_wait(&sem_empty);
    mutex_acquire(&qlock);
    qbuf[qt] = it;
    qt = (qt + 1) % QUEUE_CAP;
    mutex_release(&qlock);
    sem_post(&sem_full, true);
}

static item_t *q_get(void) {
    item_t *it;
    sem_wait(&sem_full);
    mutex_acquire(&qlock);
    it = qbuf[qh];
    qbuf[qh] = NULL;
    qh = (qh + 1) % QUEUE_CAP;
    mutex_release(&qlock);
    sem_post(&sem_empty, true);
    return it;
}

static int producer_fn(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < ITEMS_PER_PROD; i++) {
        item_t *it = (item_t *)malloc(sizeof(item_t));
        if (!it) {
            printf("[prod%d cpu%u] malloc fail i=%d\n", id, arch_curr_cpu_num(), i);
            continue;
        }
        it->producer = id;
        it->seq = i;
        q_put(it);

        arch_interrupt_saved_state_t st = spin_lock_irqsave(&slock);
        produced++;
        int p = produced;
        int c = consumed;
        spin_unlock_irqrestore(&slock, st);

        if ((i % 5) == 0) {
            printf("[prod%d cpu%u] seq=%d produced=%d consumed=%d\n",
                   id, arch_curr_cpu_num(), i, p, c);
        }
        thread_yield();
    }
    /* un marcador de fin por consumidor */
    item_t *end = (item_t *)malloc(sizeof(item_t));
    if (end) {
        end->producer = id;
        end->seq = -1;
        q_put(end);
    }
    printf("[prod%d cpu%u] done\n", id, arch_curr_cpu_num());
    return 0;
}

static int consumer_fn(void *arg) {
    int id = (int)(long)arg;
    int local = 0;
    for (;;) {
        item_t *it = q_get();
        if (!it) continue;
        if (it->seq < 0) {
            free(it);
            break;
        }
        local++;
        arch_interrupt_saved_state_t st = spin_lock_irqsave(&slock);
        consumed++;
        int c = consumed;
        spin_unlock_irqrestore(&slock, st);
        if ((local % 10) == 0) {
            printf("[cons%d cpu%u] got p%d:%d local=%d total=%d\n",
                   id, arch_curr_cpu_num(), it->producer, it->seq, local, c);
        }
        free(it);
        thread_yield();
    }
    printf("[cons%d cpu%u] done local=%d\n", id, arch_curr_cpu_num(), local);
    return 0;
}

static void uartstress_entry(const struct app_descriptor *app, void *args) {
    (void)app; (void)args;

    printf("\n[uartstress] app LK: productor-consumidor SMP (UART)\n");
    printf("[uartstress] printf serializado por print_spin_lock del kernel\n");
    printf("[uartstress] 2 productores + 2 consumidores, 1 por CPU, cola=%d, items=%d\n",
           QUEUE_CAP, TOTAL_ITEMS);

    qh = qt = 0;
    produced = 0;
    consumed = 0;
    sem_init(&sem_empty, QUEUE_CAP);
    sem_init(&sem_full, 0);
    mutex_init(&qlock);

    thread_t *tp0 = thread_create("prod0", producer_fn, (void *)0, DEFAULT_PRIORITY, 8192);
    thread_t *tp1 = thread_create("prod1", producer_fn, (void *)1, DEFAULT_PRIORITY, 8192);
    thread_t *tc0 = thread_create("cons0", consumer_fn, (void *)0, DEFAULT_PRIORITY, 8192);
    thread_t *tc1 = thread_create("cons1", consumer_fn, (void *)1, DEFAULT_PRIORITY, 8192);

    if (tp0) { thread_set_pinned_cpu(tp0, 0); thread_resume(tp0); }
    if (tp1) { thread_set_pinned_cpu(tp1, 1); thread_resume(tp1); }
    if (tc0) { thread_set_pinned_cpu(tc0, 0); thread_resume(tc0); }
    if (tc1) { thread_set_pinned_cpu(tc1, 1); thread_resume(tc1); }

    printf("[uartstress] hilos creados: prod0@cpu0 prod1@cpu1 cons0@cpu0 cons1@cpu1\n");

    if (tp0) thread_join(tp0, NULL, INFINITE_TIME);
    if (tp1) thread_join(tp1, NULL, INFINITE_TIME);
    if (tc0) thread_join(tc0, NULL, INFINITE_TIME);
    if (tc1) thread_join(tc1, NULL, INFINITE_TIME);

    printf("[uartstress] resumen: producidos=%d consumidos=%d esperados=%d\n",
           produced, consumed, TOTAL_ITEMS);
    if (produced == TOTAL_ITEMS && consumed == TOTAL_ITEMS) {
        printf("[uartstress] RESULTADO: OK\n");
    } else {
        printf("[uartstress] RESULTADO: ERROR\n");
    }
    printf("[uartstress] complete\n");
}

APP_START(uartstress)
.entry = uartstress_entry,
.flags = APP_FLAG_CUSTOM_STACK_SIZE | APP_FLAG_NO_AUTOSTART,
.stack_size = 8192,
APP_END
