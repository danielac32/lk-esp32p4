/*
 * Comandos de consola "ver" para ESP32-P4 (LK).
 */
#include <lk/console_cmd.h>
#include <lk/debug.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/thread.h>
#include <kernel/mp.h>
#include <arch/ops.h>
#include <platform.h>
#include <platform/psram_heap.h>
#include <platform/esp32p4_board.h>
#include <platform/esp32p4_power.h>
#include <platform/esp32p4_touch.h>
#include <platform/esp32p4_es8311.h>
#include <platform/esp32p4_lcd.h>
#include <platform/esp32p4_audio.h>
#include <lib/dlmalloc.h>

void p4_fbcon_show(void);
void p4_lcd_set_rot(uint8_t rot);
#include <stdint.h>

extern uint8_t __data_start[], __data_end[];
extern uint8_t __bss_start[], __bss_end[];
extern uint8_t _end[], _end_of_ram[];
extern uint8_t _psram_free_start[], _psram_free_end[];
extern uint8_t _sd_desc_page[];

static void print_mem_info(void) {
    uintptr_t data_sz = (uintptr_t)__data_end - (uintptr_t)__data_start;
    uintptr_t bss_sz = (uintptr_t)__bss_end - (uintptr_t)__bss_start;
    uintptr_t ram_total = (uintptr_t)_end_of_ram - 0x4FF00000UL;
    uintptr_t img_end = (uintptr_t)_end;
    uintptr_t novm_free_start = (uintptr_t)_end;
    uintptr_t novm_free_end = (uintptr_t)_end_of_ram; /* NOVM usa MEMBASE+MEMSIZE; revisar límite superior como Xinu */
    uintptr_t novm_free_sz = (novm_free_end > novm_free_start) ? (novm_free_end - novm_free_start) : 0;
    uintptr_t psram_start = (uintptr_t)_psram_free_start;
    uintptr_t psram_end = (uintptr_t)_psram_free_end;
    uintptr_t psram_sz = (psram_end > psram_start) ? (psram_end - psram_start) : 0;

    printf("== memoria (ESP32-P4) ==\n");
    printf("RAM interna (HP_L2MEM): base=0x4FF00000 total=%u KB (%u bytes)\n",
           (unsigned)(ram_total / 1024), (unsigned)ram_total);
    printf("  .l2reserve: primeros 128KB de L2MEM reservados para caché L2; .data/.bss/heap "
           "empiezan en 0x4FF20000 (por encima de la caché).\n");
    printf("  .data: 0x%08x..0x%08x (%u bytes)\n",
           (unsigned)(uintptr_t)__data_start, (unsigned)(uintptr_t)__data_end, (unsigned)data_sz);
    printf("  .bss : 0x%08x..0x%08x (%u bytes)\n",
           (unsigned)(uintptr_t)__bss_start, (unsigned)(uintptr_t)__bss_end, (unsigned)bss_sz);
    printf("  _end imagen: 0x%08x  (_end_of_ram=0x%08x)\n",
           (unsigned)img_end, (unsigned)(uintptr_t)_end_of_ram);
    printf("  Rango libre tras imagen (NOVM/heap L2, aprox): 0x%08x..0x%08x (%u KB)\n",
           (unsigned)novm_free_start, (unsigned)novm_free_end, (unsigned)(novm_free_sz / 1024));

    printf("\nPSRAM: region compartida SD (.sdshare) en 0x%08x, heap PSRAM 0x%08x..0x%08x (%u KB, %u MB)\n",
           (unsigned)(uintptr_t)_sd_desc_page, (unsigned)psram_start, (unsigned)psram_end,
           (unsigned)(psram_sz / 1024), (unsigned)(psram_sz / (1024 * 1024)));

#if !NO_MALLINFO
    /* dlmalloc principal (heap L2MEM/kernel). Evita 'heap info' si congela; esto solo consulta mallinfo. */
    struct mallinfo mi = dlmallinfo_unlocked();
    printf("\nHeap L2 (dlmalloc, mallinfo):\n");
    printf("  arena=%u  ordblks=%u  uordblks=%u  fordblks=%u  keepcost=%u\n",
           (unsigned)mi.arena, (unsigned)mi.ordblks, (unsigned)mi.uordblks,
           (unsigned)mi.fordblks, (unsigned)mi.keepcost);
    printf("  usado~%u bytes, libre~%u bytes (aprox)\n",
           (unsigned)mi.uordblks, (unsigned)mi.fordblks);

    struct mallinfo pmi = psram_mallinfo();
    printf("Heap PSRAM (mspace, mallinfo):\n");
    printf("  arena=%u  ordblks=%u  uordblks=%u  fordblks=%u  keepcost=%u\n",
           (unsigned)pmi.arena, (unsigned)pmi.ordblks, (unsigned)pmi.uordblks,
           (unsigned)pmi.fordblks, (unsigned)pmi.keepcost);
#endif

    printf("\nComandos útiles: 'novm info', 'heap info' (puede congelar), 'threads'\n");
}

static int cmd_ver(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("uso: ver <threads|mem|cpu|all>\n");
        printf("  (tambien: threads, threadstats, heap, help)\n");
        return 0;
    }

    const char *sub = argv[1].str;

    if (!strcmp(sub, "threads") || !strcmp(sub, "hilos")) {
        printf("== hilos ==\n");
        dump_all_threads();
        return 0;
    }

    if (!strcmp(sub, "mem") || !strcmp(sub, "heap") || !strcmp(sub, "memoria") || !strcmp(sub, "ram")) {
        print_mem_info();
        return 0;
    }

    if (!strcmp(sub, "cpu") || !strcmp(sub, "cpus") || !strcmp(sub, "smp")) {
        printf("== cpu/smp ==\n");
        printf("cpu actual: %u\n", arch_curr_cpu_num());
        printf("smp_max_cpus: %u\n", SMP_MAX_CPUS);
#if WITH_SMP
        for (uint i = 0; i < SMP_MAX_CPUS; i++) {
            printf("  cpu%u: active=%d idle=%d\n", i, mp_is_cpu_active(i), mp_is_cpu_idle(i));
        }
#endif
        return 0;
    }

    if (!strcmp(sub, "all") || !strcmp(sub, "todo")) {
        const console_cmd_args a1[] = {argv[0], {.str = "cpu"}};
        const console_cmd_args a2[] = {argv[0], {.str = "threads"}};
        cmd_ver(2, a1);
        printf("\n");
        cmd_ver(2, a2);
        printf("\n");
        print_mem_info();
        return 0;
    }

    printf("subcomando desconocido: %s\n", sub);
    printf("uso: ver <threads|mem|cpu|all>\n");
    return 0;
}

static int cmd_power(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
    if (!p4_power_init()) {
        printf("power: AXP2101 no respondio (ChipID != 0x4a)\n");
        return -1;
    }
    p4_power_info_t info;
    if (!p4_power_info(&info)) {
        printf("power: fallo al leer el PMIC\n");
        return -1;
    }
    static const char *chg[] = {"trickle", "?", "fast", "absorb", "done", "none"};
    static const char *cur[] = {"standby", "charge", "discharge", "?"};
    printf("AXP2101: bateria %u%% (vbat=%u mV)\n", (unsigned)info.percent, (unsigned)info.vbat_mv);
    printf("  VBUS %s | bateria %s | sistema %s\n",
           info.vbus_good ? "OK" : "no", info.battery ? "presente" : "no",
           info.system_on ? "ON" : "OFF");
    printf("  carga=%s | corriente=%s\n",
           info.charge < 6 ? chg[info.charge] : "?", info.current < 4 ? cur[info.current] : "?");
    return 0;
}

static int cmd_touch(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
    if (!p4_touch_init()) {
        printf("touch: FT6336 no responde en 0x38\n");
        return -1;
    }
    printf("touch: FT6336 detectado. Toca la pantalla (3 lecturas):\n");
    for (int k = 0; k < 3; k++) {
        p4_touch_point_t pts[P4_TOUCH_MAX_POINTS];
        int n = p4_touch_read(pts, P4_TOUCH_MAX_POINTS);
        printf("  [%d] %d punto(s)", k, n);
        for (int i = 0; i < n; i++) printf(" | id=%u x=%u y=%u", pts[i].id, pts[i].x, pts[i].y);
        printf("\n");
        spin(300000U);  /* 300 ms */
    }
    return 0;
}

static int cmd_audio(int argc, const console_cmd_args *argv) {
    if (argc >= 2 && !strcmp(argv[1].str, "init")) {
        printf("audio: ES8311 init... ");
        bool ok = p4_es8311_init();
        printf("%s\n", ok ? "OK" : "FAIL (codec no responde)");
        if (!ok) return -1;
        printf("  vol=%d%% (%d dB) mute=%d\n",
               p4_es8311_get_volume(), p4_es8311_get_volume_db(),
               p4_es8311_get_mute());
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1].str, "vol")) {
        int v = atoi(argv[2].str);
        printf("audio: vol %d -> %d%% (leido: %d dB)\n", v,
               p4_es8311_set_volume((uint32_t)(v < 0 ? 0 : v)), p4_es8311_get_volume_db());
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1].str, "mute")) {
        bool m = !strcmp(argv[2].str, "on");
        p4_es8311_set_mute(m);
        printf("audio: mute %s (leido=%d)\n", m ? "ON" : "OFF", p4_es8311_get_mute());
        return 0;
    }
    printf("uso: audio init | audio vol <0-100> | audio mute <on|off>\n");
    return -1;
}

static int cmd_lcd(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
    printf("lcd: ST7796 init... ");
    if (!p4_lcd_init()) { printf("FAIL (SPI timeout?)\n"); return -1; }
    printf("OK (320x480)\n");
    printf("lcd: barras de colores... ");
    printf("%s\n", p4_lcd_test() ? "OK" : "FAIL");
    return 0;
}

/* rec MS [noloop]: graba MS ms del micro a PSRAM, duplica el slot mono y
 * reproduce lo grabado (por defecto volumen 75%).  Patron de Xinu xsh_audio.c. */
#define REC_BYTES_PER_MS 192U
#define REC_MAX_MS       20000U
#define REC_CHUNK        4096U
static uint8_t *rec_base;   /* forma 0x48 (bus/psram_malloc) */
static uint8_t *rec_direct; /* forma 0x88 (acceso CPU directo, sin cache) */
static uint32_t rec_cap;

static int cmd_rec(int argc, const console_cmd_args *argv) {
    uint32_t ms = 4000;
    bool loop = true;
    if (argc >= 2) {
        unsigned long v = strtoul(argv[1].str, NULL, 0);
        if (v == 0 || v > REC_MAX_MS) { printf("rec: MS debe estar entre 1 y %u\n", REC_MAX_MS); return -1; }
        ms = (uint32_t)v;
    }
    if (argc >= 3 && !strcmp(argv[2].str, "noloop")) loop = false;

    /* El mspace PSRAM no se inicializa en el arranque de este kernel:
     * inicializarlo aqui bajo demanda (idempotente). */
    psram_heap_init();

    uint32_t wanted = ms * REC_BYTES_PER_MS;
    if (!rec_base || wanted > rec_cap) {
        if (rec_base) psram_free(rec_base);
        rec_cap = wanted;
        rec_base = psram_malloc(rec_cap);
        if (!rec_base) { printf("rec: sin hueco de %u bytes en PSRAM\n", rec_cap); return -1; }
        rec_direct = rec_base + 0x40000000UL;  /* 0x48 -> 0x88 (sin cache) */
        printf("rec: buffer en 0x%08x (%u bytes)\n", (unsigned)(uintptr_t)rec_direct, rec_cap);
    }

    printf("rec: grabando %u ms (%u bytes)...\n", (unsigned)ms, (unsigned)wanted);
    if (!p4_audio_in_start()) { printf("rec: fallo al arrancar captura\n"); return -1; }
    uint32_t total = 0, peak = 0, silent = 0;
    while (total < wanted) {
        uint32_t chunk = wanted - total;
        if (chunk > REC_CHUNK) chunk = REC_CHUNK;
        uint32_t got = p4_audio_in_read(rec_direct + total, chunk);
        if (got == 0) {
            if (++silent > 300) { printf("rec: la captura no entrega tramas\n"); p4_audio_in_stop(); return -1; }
            thread_sleep(2);
            continue;
        }
        silent = 0;
        /* quirk mono ES8311: ADC vuelca solo en slot0, DAC reproduce el
         * contrario -> duplicar slot0 sobre slot1 trama a trama */
        for (uint32_t k = 0; k + 3U < got; k += 4U) {
            rec_direct[total + k + 2U] = rec_direct[total + k];
            rec_direct[total + k + 3U] = rec_direct[total + k + 1U];
        }
        for (uint32_t k = 0; k < got; k++) {
            uint8_t v = rec_direct[total + k];
            uint32_t a = v > 128 ? v - 128 : 128 - v;
            if (a > peak) peak = a;
        }
        total += got;
    }
    p4_audio_in_stop();
    printf("rec: %u bytes, pico=%u, dropped=%u\n", total, peak, p4_audio_in_dropped());

    if (loop) {
        p4_es8311_set_volume(75);
        printf("rec: reproduciendo a 75%%... ");
        bool ok = p4_audio_play(rec_direct, total);
        printf("%s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : -1;
    }
    return 0;
}

static int cmd_arec(int argc, const console_cmd_args *argv) {
    uint32_t ms = 500;
    if (argc >= 2) ms = (uint32_t)atoi(argv[1].str);
    static uint8_t buf[4096];
    printf("arec: capturando %u ms (I2S RX + DMA)...\n", (unsigned)ms);
    if (!p4_audio_in_start()) { printf("arec: FAIL start\n"); return -1; }
    uint32_t total = 0;
    for (uint32_t t = 0; t < ms; t += 25U) {
        uint32_t n = p4_audio_in_read(buf, sizeof(buf));
        total += n;
        thread_sleep(25);
    }
    uint32_t peak = 0;
    for (uint32_t i = 0; i < total && i < sizeof(buf); i++) {
        uint32_t v = buf[i] > 127 ? buf[i] - 128 : 128 - buf[i];
        if (v > peak) peak = v;
    }
    p4_audio_in_stop();
    printf("arec: %u bytes, pico=%u, dropped=%u\n", total, peak, p4_audio_in_dropped());
    return 0;
}

static int cmd_atest(int argc, const console_cmd_args *argv) {
    if (argc >= 2 && !strcmp(argv[1].str, "status")) { p4_audio_status(); return 0; }
    uint32_t hz = 440, ms = 500;
    if (argc >= 2) hz = (uint32_t)atoi(argv[1].str);
    if (argc >= 3) ms = (uint32_t)atoi(argv[2].str);
    printf("atone: %u Hz %u ms (I2S+DMA)...\n", (unsigned)hz, (unsigned)ms);
    bool ok = p4_audio_tone(hz, ms);
    printf("atone: %s\n", ok ? "OK" : "FAIL");
    p4_audio_status();
    return ok ? 0 : -1;
}

static int cmd_lcdrot(int argc, const console_cmd_args *argv) {
    if (argc < 2) { printf("uso: lcdrot <0|2|4|6>\n"); return -1; }
    uint8_t r = (uint8_t)atoi(argv[1].str);
    if ((r & 1) || r > 6) { printf("lcdrot:valor invalido\n"); return -1; }
    printf("lcdrot: MADCTL rot=%u -> 0x%02x\n", r, (unsigned)(r==0?0x08:r==2?0xC8:r==4?0x48:0x88));
    p4_lcd_set_rot(r);
    p4_fbcon_show();   /* redibuja el splash con la nueva orientacion */
    return 0;
}

static int cmd_fbshow(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
    p4_fbcon_show();
    printf("fbshow: log de arranque volcado al LCD\n");
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("lcdrot", "cambiar orientacion LCD [0|2|4|6]", &cmd_lcdrot)
STATIC_COMMAND("fbshow", "volcar log de arranque al LCD", &cmd_fbshow)
STATIC_COMMAND("ver", "ver estado del sistema (threads|mem|cpu|all)", &cmd_ver)
STATIC_COMMAND("lcd", "ST7796: init + test de colores", &cmd_lcd)
STATIC_COMMAND("atone", "audio: tono I2S+DMA [hz] [ms] | status", &cmd_atest)
STATIC_COMMAND("arec", "audio: captura I2S RX [ms]", &cmd_arec)
STATIC_COMMAND("rec", "audio: grabar [ms] y reproducir (noloop=no reproduce)", &cmd_rec)
STATIC_COMMAND("audio", "ES8311: codec de audio (init/vol/mute)", &cmd_audio)
STATIC_COMMAND("power", "AXP2101: estado de bateria/alimentacion", &cmd_power)
STATIC_COMMAND("touch", "FT6336: leer panel tactil", &cmd_touch)
STATIC_COMMAND_END(esp32p4_shell_cmds);
