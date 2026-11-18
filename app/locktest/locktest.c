/*
 * LK app: banco de estres para los candados SMP anadidos al port ESP32-P4.
 * Verifica bajo concurrencia real (2 cores) los recursos que antes no tenian
 * mutex: bus I2C0 (compartido por ES8311/PMIC/touch) y heap PSRAM.
 *
 * Sin el mutex de I2C, dos cores intercalando la programacion COMD/FIFO del
 * periferico producen NACKs o leen el registro equivocado.  Aqui ambos hilos
 * leen un registro con valor conocido (ChipID AXP2101 = 0x4A) millions de veces
 * y cualquier desviacion cuenta como fallo.
 *
 * No autostart: ejecutar con "app start locktest" o "start locktest".
 */
#include <app.h>
#include <platform/esp32p4_i2c.h>
#include <platform/esp32p4_board.h>
#include <platform/psram_heap.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <arch/ops.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2C_ITERS       300U
#define PSRAM_ITERS     200U
#define PSRAM_BLOCK_SZ  256U

#define AXP_ADDR        0x34U
#define AXP_CHIPID_REG  0x03U
#define AXP_CHIPID_VAL  0x4AU

static spin_lock_t stat_lock = SPIN_LOCK_INITIAL_VALUE;
static volatile uint32_t i2c_ok, i2c_bad;
static volatile uint32_t psram_ok, psram_bad;

/* hilo que golpea el I2C leyendo el ChipID del PMIC una y otra vez */
static int i2c_stress_thread(void *arg) {
    uint32_t cpu = arch_curr_cpu_num();
    uint32_t local_ok = 0, local_bad = 0;
    uint8_t buf[4];

    /* Primera lectura de calentamiento: el primer acceso tras la init del
     * periferico puede devolver un valor transitorio; se descarta. */
    p4_i2c_read_regs(AXP_ADDR, AXP_CHIPID_REG, buf, 1);

    for (uint32_t i = 0; i < I2C_ITERS; i++) {
        bool got = p4_i2c_read_regs(AXP_ADDR, AXP_CHIPID_REG, buf, 1);
        if (got && buf[0] == AXP_CHIPID_VAL) local_ok++;
        else local_bad++;
        if ((i % 1000U) == 0U) thread_yield();
    }

    arch_interrupt_saved_state_t st = spin_lock_irqsave(&stat_lock);
    i2c_ok += local_ok;
    i2c_bad += local_bad;
    spin_unlock_irqrestore(&stat_lock, st);

    printf("[locktest] i2c cpu%u: ok=%u bad=%u\n", cpu, local_ok, local_bad);
    return 0;
}

/* hilo que estresa el heap PSRAM: malloc, llenar con patron, verificar, free.
 * Dos hilos concurrentes validan que el spinlock de psram_heap no corrompe
 * el mspace ni las cabeceras de los bloques. */
static int psram_stress_thread(void *arg) {
    uint32_t cpu = arch_curr_cpu_num();
    uint32_t local_ok = 0, local_bad = 0;
    uint8_t *blocks[8];

    for (uint32_t i = 0; i < PSRAM_ITERS; i++) {
        for (int b = 0; b < 8; b++) {
            blocks[b] = psram_malloc(PSRAM_BLOCK_SZ);
            if (!blocks[b]) { local_bad++; continue; }
            memset(blocks[b], (int)(i + b), PSRAM_BLOCK_SZ);
        }
        for (int b = 0; b < 8; b++) {
            if (!blocks[b]) continue;
            uint8_t pat = (uint8_t)(i + b);
            bool good = true;
            for (uint32_t k = 0; k < PSRAM_BLOCK_SZ; k++) {
                if (blocks[b][k] != pat) { good = false; break; }
            }
            if (good) local_ok++; else local_bad++;
            psram_free(blocks[b]);
        }
        if ((i % 500U) == 0U) thread_yield();
    }

    arch_interrupt_saved_state_t st = spin_lock_irqsave(&stat_lock);
    psram_ok += local_ok;
    psram_bad += local_bad;
    spin_unlock_irqrestore(&stat_lock, st);

    printf("[locktest] psram cpu%u: ok=%u bad=%u\n", cpu, local_ok, local_bad);
    return 0;
}

static void locktest_entry(const struct app_descriptor *app, void *args) {
    (void)app; (void)args;

    printf("\n[locktest] banco de estres SMP de candados (I2C + PSRAM)\n");
    printf("[locktest] 2 hilos I2C (%u iters c/u) + 2 hilos PSRAM (%u iters c/u)\n",
           I2C_ITERS, PSRAM_ITERS);

    psram_heap_init();   /* idempotente */
    p4_i2c_init();       /* inicializa el bus antes de que los hilos lo usen */

    i2c_ok = i2c_bad = psram_ok = psram_bad = 0;

    thread_t *ti0 = thread_create("i2c0", i2c_stress_thread, NULL, DEFAULT_PRIORITY, 4096);
    thread_t *ti1 = thread_create("i2c1", i2c_stress_thread, NULL, DEFAULT_PRIORITY, 4096);
    thread_t *tp0 = thread_create("ps0", psram_stress_thread, NULL, DEFAULT_PRIORITY, 4096);
    thread_t *tp1 = thread_create("ps1", psram_stress_thread, NULL, DEFAULT_PRIORITY, 4096);

    /* Sin pinning fijo: el scheduler reparte los hilos entre cores.  Así el
     * test ejerce la concurrencia real que queremos validar sin depender de
     * que un core concreto esté disponible para esa prioridad. */
    if (ti0) thread_resume(ti0);
    if (ti1) thread_resume(ti1);
    if (tp0) thread_resume(tp0);
    if (tp1) thread_resume(tp1);

    if (ti0) thread_join(ti0, NULL, 30000);
    if (ti1) thread_join(ti1, NULL, 30000);
    if (tp0) thread_join(tp0, NULL, 30000);
    if (tp1) thread_join(tp1, NULL, 30000);

    uint32_t tot_i2c_ok = i2c_ok, tot_i2c_bad = i2c_bad;
    uint32_t tot_ps_ok = psram_ok, tot_ps_bad = psram_bad;
    printf("[locktest] RESUMEN I2C: ok=%u bad=%u (esperados %u)\n",
           tot_i2c_ok, tot_i2c_bad, 2U * I2C_ITERS);
    printf("[locktest] RESUMEN PSRAM: ok=%u bad=%u (esperados %u)\n",
           tot_ps_ok, tot_ps_bad, 2U * PSRAM_ITERS * 8U);

    if (tot_i2c_bad == 0 && tot_ps_bad == 0 &&
        tot_i2c_ok == 2U * I2C_ITERS && tot_ps_ok == 2U * PSRAM_ITERS * 8U) {
        printf("[locktest] RESULTADO: OK\n");
    } else {
        printf("[locktest] RESULTADO: ERROR\n");
    }
    printf("[locktest] complete\n");
}

APP_START(locktest)
.entry = locktest_entry,
.flags = APP_FLAG_CUSTOM_STACK_SIZE | APP_FLAG_NO_AUTOSTART,
.stack_size = 8192,
APP_END
