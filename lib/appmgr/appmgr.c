#include <lib/appmgr.h>
#include <kernel/spinlock.h>
#include <kernel/event.h>
#include <kernel/thread.h>
#include <platform/psram_heap.h>
#include <string.h>
#include <stdlib.h>
#include <arch/interrupts.h>

static appmgr_entry_t table[APPMGR_MAX];
static event_t resume_ev[APPMGR_MAX];
static bool ev_init[APPMGR_MAX];
static spin_lock_t appmgr_lock = SPIN_LOCK_INITIAL_VALUE;
static bool initialized;

void appmgr_init(void) {
    if (initialized) return;
    memset(table, 0, sizeof(table));
    memset(ev_init, 0, sizeof(ev_init));
    for (int i = 0; i < APPMGR_MAX; i++) {
        table[i].state = APP_FREE;
    }
    initialized = true;
}

appmgr_entry_t *appmgr_table(void) { return table; }
int appmgr_count(void) { return APPMGR_MAX; }

int appmgr_find_path(const char *path) {
    if (!path) return -1;
    int found = -1;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    for (int i = 0; i < APPMGR_MAX; i++) {
        if (table[i].state != APP_FREE && table[i].state != APP_EXITED &&
            strcmp(table[i].path, path) == 0) {
            found = i;
            break;
        }
    }
    spin_unlock_irqrestore(&appmgr_lock, state);
    return found;
}

int appmgr_find_thread(thread_t *t) {
    if (!t) return -1;
    int found = -1;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    for (int i = 0; i < APPMGR_MAX; i++) {
        if (table[i].state != APP_FREE && table[i].state != APP_EXITED && table[i].thread == t) {
            found = i;
            break;
        }
    }
    spin_unlock_irqrestore(&appmgr_lock, state);
    return found;
}

static void slot_clear_unlocked(int slot) {
    if (table[slot].thumb) {
        psram_free(table[slot].thumb);
        table[slot].thumb = NULL;
    }
    if (ev_init[slot]) {
        event_destroy(&resume_ev[slot]);
        ev_init[slot] = false;
    }
    memset(&table[slot], 0, sizeof(table[slot]));
    table[slot].state = APP_FREE;
}

int appmgr_register(const char *name, const char *path, uint32_t color, thread_t *t) {
    if (!initialized) appmgr_init();
    int slot = -1;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);

    if (path) {
        for (int i = 0; i < APPMGR_MAX; i++) {
            if (table[i].state != APP_FREE && strcmp(table[i].path, path) == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        for (int i = 0; i < APPMGR_MAX; i++) {
            if (table[i].state == APP_FREE || table[i].state == APP_EXITED) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&appmgr_lock, state);
        return -1;
    }

    slot_clear_unlocked(slot);

    if (name) strlcpy(table[slot].name, name, sizeof(table[slot].name));
    if (path) strlcpy(table[slot].path, path, sizeof(table[slot].path));
    table[slot].color = color;
    table[slot].thread = t;
    table[slot].state = APP_RUNNING;
    table[slot].info = 0;
    event_init(&resume_ev[slot], false, 0);
    ev_init[slot] = true;

    spin_unlock_irqrestore(&appmgr_lock, state);
    return slot;
}

int appmgr_unregister(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return -1;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    slot_clear_unlocked(slot);
    spin_unlock_irqrestore(&appmgr_lock, state);
    return 0;
}

int appmgr_get_state(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return APP_FREE;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    int s = table[slot].state;
    spin_unlock_irqrestore(&appmgr_lock, state);
    return s;
}

void appmgr_set_state(int slot, int st) {
    if (slot < 0 || slot >= APPMGR_MAX) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    table[slot].state = st;
    spin_unlock_irqrestore(&appmgr_lock, state);
}

void appmgr_request_close(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    table[slot].state = APP_CLOSING;
    bool ev = ev_init[slot];
    spin_unlock_irqrestore(&appmgr_lock, state);
    if (ev) event_signal(&resume_ev[slot], true);
}

int appmgr_take_foreground(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return -1;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    for (int i = 0; i < APPMGR_MAX; i++) {
        if (table[i].state == APP_RUNNING) table[i].state = APP_BACKGROUND;
    }
    table[slot].state = APP_RUNNING;
    bool ev = ev_init[slot];
    spin_unlock_irqrestore(&appmgr_lock, state);
    if (ev) event_signal(&resume_ev[slot], true);
    return 0;
}

void appmgr_set_info(int slot, int info) {
    if (slot < 0 || slot >= APPMGR_MAX) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    table[slot].info = info;
    spin_unlock_irqrestore(&appmgr_lock, state);
}

int appmgr_get_info(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return 0;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    int info = table[slot].info;
    spin_unlock_irqrestore(&appmgr_lock, state);
    return info;
}

void appmgr_clear_thumbnail(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    if (table[slot].thumb) {
        psram_free(table[slot].thumb);
        table[slot].thumb = NULL;
    }
    table[slot].thumb_w = 0;
    table[slot].thumb_h = 0;
    spin_unlock_irqrestore(&appmgr_lock, state);
}

int appmgr_set_thumbnail(int slot, const uint16_t *src, int src_w, int src_h, int tw, int th) {
    if (slot < 0 || slot >= APPMGR_MAX || !src || src_w <= 0 || src_h <= 0 || tw <= 0 || th <= 0)
        return -1;
    size_t bytes = (size_t)tw * (size_t)th * 2u;
    uint16_t *buf = (uint16_t *)psram_malloc(bytes);
    if (!buf) return -1;

    for (int y = 0; y < th; y++) {
        int sy = y * src_h / th;
        if (sy >= src_h) sy = src_h - 1;
        for (int x = 0; x < tw; x++) {
            int sx = x * src_w / tw;
            if (sx >= src_w) sx = src_w - 1;
            buf[y * tw + x] = src[sy * src_w + sx];
        }
    }

    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    if (table[slot].thumb) psram_free(table[slot].thumb);
    table[slot].thumb = buf;
    table[slot].thumb_w = tw;
    table[slot].thumb_h = th;
    spin_unlock_irqrestore(&appmgr_lock, state);
    return 0;
}

int appmgr_suspend_background(int slot, const uint16_t *fb, int fb_w, int fb_h, int tw, int th) {
    if (slot < 0 || slot >= APPMGR_MAX) return -1;
    if (fb) appmgr_set_thumbnail(slot, fb, fb_w, fb_h, tw, th);
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    if (table[slot].state != APP_CLOSING) {
        table[slot].state = APP_BACKGROUND;
    }
    if (ev_init[slot]) {
        event_unsignal(&resume_ev[slot]);
    }
    spin_unlock_irqrestore(&appmgr_lock, state);
    return 0;
}

int appmgr_wait_resume(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return APP_FREE;
    for (;;) {
        arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
        int s = table[slot].state;
        bool ev = ev_init[slot];
        spin_unlock_irqrestore(&appmgr_lock, state);
        if (s == APP_RUNNING || s == APP_CLOSING || s == APP_FREE || s == APP_EXITED)
            return s;
        if (ev) event_wait(&resume_ev[slot]);
        else thread_sleep(50);
    }
}

int appmgr_resume_app(int slot) {
    return appmgr_take_foreground(slot);
}

void appmgr_mark_exited(int slot) {
    if (slot < 0 || slot >= APPMGR_MAX) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&appmgr_lock);
    table[slot].state = APP_EXITED;
    table[slot].thread = NULL;
    spin_unlock_irqrestore(&appmgr_lock, state);
}
