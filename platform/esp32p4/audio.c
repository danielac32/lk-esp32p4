/*
 * Audio PCM (anillos DMA) — portado de Xinu audioOut.c + audioIn.c.
 * CPU accede a PSRAM por 0x88 (sin cache); descriptores anotan 0x48 (motor).
 */
#include <platform/esp32p4_audio.h>
#include <platform/esp32p4_i2s.h>
#include <platform/esp32p4_ahbdma.h>
#include <platform/esp32p4_es8311.h>
#include <platform/esp32p4_gpio.h>
#include <platform/esp32p4_board.h>
#include <platform/time.h>
#include <dev/gpio.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_CPU_UNCACHED 0x40000000UL
#define MEM_BARRIER() __asm__ volatile("fence rw, rw" ::: "memory")

extern uint8_t _audio_desc_page[], _audio_buf_page[];
extern uint8_t _audio_stream_desc_page[], _audio_stream_buf_page[];
extern uint8_t _audio_in_desc_page[], _audio_in_buf_page[];

#define OUT_CHUNK_BYTES 2048U
#define OUT_CHUNKS      32U
#define OUT_RING_BYTES  (OUT_CHUNK_BYTES * OUT_CHUNKS)
#define IN_CHUNK_BYTES  2048U
#define IN_CHUNKS       32U
#define DESC_LEN_SHIFT  12U
#define DESC_SIZE_MASK  0xFFFU
#define DESC_SUC_EOF    (1U << 30)
#define DESC_OWNER_DMA  (1U << 31)

typedef struct { uint32_t dw0, buffer, next; } link_desc;

/* Serializa el subsistema de audio completo: anillos TX/RX, estado de
 * start/stop y registros I2S/DMA.  Un solo mutex porque TX y RX comparten
 * relojes (RX es esclavo del TX) y no pueden operar en paralelo. */
static mutex_t audio_lock = MUTEX_INITIAL_VALUE(audio_lock);

static bool stream_on, in_on;
static uint64_t streamWritten;
static uint32_t streamGaps;
static uint32_t streamDescBus, streamRingBus;
static volatile link_desc *streamDesc;
static uint8_t *streamRing;

/* --- PSRAM word-copy (0x88 direct path, byte-per-byte memcpy too slow) --- */
static void psram_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n && (((uintptr_t)d | (uintptr_t)s) & 3U)) { *d++ = *s++; n--; }
    for (; n >= 4U; n -= 4U, d += 4U, s += 4U) *(uint32_t *)d = *(const uint32_t *)s;
    while (n) { *d++ = *s++; n--; }
}
static void psram_zero(void *dst, uint32_t n) {
    uint8_t *d = dst;
    while (n && ((uintptr_t)d & 3U)) { *d++ = 0; n--; }
    for (; n >= 4U; n -= 4U, d += 4U) *(uint32_t *)d = 0;
    while (n) { *d++ = 0; n--; }
}

/* ---------------------- reproduccion (stream) ---------------------- */
static void stream_link_build(void) {
    for (uint32_t i = 0; i < OUT_CHUNKS; i++) {
        streamDesc[i].dw0 = OUT_CHUNK_BYTES | (OUT_CHUNK_BYTES << DESC_LEN_SHIFT);
        streamDesc[i].buffer = streamRingBus + i * OUT_CHUNK_BYTES;
        streamDesc[i].next = streamDescBus + ((i + 1U) % OUT_CHUNKS) * sizeof(link_desc);
        MEM_BARRIER();
        streamDesc[i].dw0 |= DESC_OWNER_DMA;
    }
}

bool p4_audio_stream_start(void) {
    mutex_acquire(&audio_lock);
    if (stream_on) { mutex_release(&audio_lock); return true; }
    if (!p4_es8311_present() && !p4_es8311_init()) { mutex_release(&audio_lock); return false; }

    streamDescBus = (uint32_t)(uintptr_t)_audio_stream_desc_page;
    streamRingBus = (uint32_t)(uintptr_t)_audio_stream_buf_page;
    streamDesc = (volatile link_desc *)(uintptr_t)(streamDescBus + AUDIO_CPU_UNCACHED);
    streamRing = (uint8_t *)(uintptr_t)(streamRingBus + AUDIO_CPU_UNCACHED);
    psram_zero(streamRing, OUT_RING_BYTES);
    stream_link_build();

    if (!p4_i2s_clocks_start()) { mutex_release(&audio_lock); return false; }
    p4_i2s_tx_sync_reset();
    if (!p4_ahbdma_tx_link(streamDescBus)) { p4_i2s_clocks_stop(); mutex_release(&audio_lock); return false; }
    p4_es8311_set_volume(75);
    p4_es8311_set_mute(false);
    /* El pin de potencia del amplificador (GPIO53) debe configurarse como
     * SALIDA antes de escribirlo; si no, queda en alta impedancia y el
     * NS4150B nunca enciende (silencio aunque el DMA/I2S corran). */
    p4_gpio_set_dir(P4_I2S_AMP_POWER, true);
    gpio_set(P4_I2S_AMP_POWER, 1);   /* amp lo ultimo (anti-pop) */
    streamWritten = 0; streamGaps = 0;
    stream_on = true;
    mutex_release(&audio_lock);
    return true;
}

uint32_t p4_audio_stream_write(const uint8_t *buf, uint32_t len) {
    mutex_acquire(&audio_lock);
    if (!stream_on || !buf) { mutex_release(&audio_lock); return 0; }
    uint32_t sent = 0;
    while (sent < len) {
        uint64_t emitted = (uint64_t)p4_i2s_tx_sync_count() * 2UL;
        int64_t ahead = (int64_t)streamWritten - (int64_t)emitted;
        if (ahead < 0) { streamGaps++; ahead = 0; }
        uint64_t room = OUT_RING_BYTES - 4096U - (uint64_t)ahead;
        uint32_t slot = (uint32_t)(streamWritten % OUT_RING_BYTES);
        uint32_t chunk = len - sent;
        uint32_t toend = OUT_RING_BYTES - slot;
        if (chunk > room) chunk = (uint32_t)room;
        if (chunk > toend) chunk = toend;
        chunk &= ~3U;  /* tramas enteras de 4 bytes */
        if (chunk == 0) {
            /* No hay hueco: dormir FUERA del lock para no bloquear a otros
             * hilos, y re-adquirir al despertar. */
            mutex_release(&audio_lock);
            thread_sleep(2);
            mutex_acquire(&audio_lock);
            continue;
        }
        psram_copy(streamRing + slot, buf + sent, chunk);
        streamWritten += chunk;
        sent += chunk;
    }
    mutex_release(&audio_lock);
    return sent;
}

bool p4_audio_stream_drain(void) {
    mutex_acquire(&audio_lock);
    if (!stream_on) { mutex_release(&audio_lock); return true; }
    for (int i = 0; i < 1000; i++) {
        if ((uint64_t)p4_i2s_tx_sync_count() * 2UL >= streamWritten) break;
        thread_sleep(2);
    }
    thread_sleep(5);
    mutex_release(&audio_lock);
    return true;
}

uint32_t p4_audio_stream_queued(void) {
    mutex_acquire(&audio_lock);
    if (!stream_on) { mutex_release(&audio_lock); return 0; }
    int64_t ahead = (int64_t)streamWritten - (int64_t)((uint64_t)p4_i2s_tx_sync_count() * 2UL);
    uint32_t q = ahead > 0 ? (uint32_t)ahead / 4U : 0;
    mutex_release(&audio_lock);
    return q;
}

void p4_audio_stream_stop(void) {
    mutex_acquire(&audio_lock);
    if (!stream_on) { mutex_release(&audio_lock); return; }
    p4_ahbdma_tx_stop();
    gpio_set(P4_I2S_AMP_POWER, 0);
    p4_es8311_set_mute(true);
    p4_i2s_clocks_stop();
    stream_on = false;
    mutex_release(&audio_lock);
}

/* tono simple: llena el anillo con un patron seno y reproduce ms */
bool p4_audio_play(const uint8_t *pcm, uint32_t len) {
    if (!pcm || !len) return false;
    if (!p4_audio_stream_start()) return false;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t n = p4_audio_stream_write(pcm + sent, len - sent);
        if (n == 0) { thread_sleep(2); continue; }
        sent += n;
    }
    p4_audio_stream_drain();
    p4_audio_stream_stop();
    return true;
}

/* Seno de 1/4 de onda con interpolacion, copiado de Xinu audioOut.c. */
#define TONE_AMPLITUDE 32000
static const int16_t quarter_wave[17] = {
    0, 3137, 6243, 9289, 12246, 15085, 17778, 20301, 22627,
    24736, 26607, 28221, 29564, 30622, 31385, 31846, TONE_AMPLITUDE
};
static int16_t tone_sine_at(uint32_t u, uint32_t len) {
    uint32_t quad = (u * 4U) / len;
    uint32_t in = (u * 4U) % len;
    uint32_t pos, rem, mag;
    if ((quad & 1U) != 0U) in = len - in;
    pos = (in * 16U) / len;
    if (pos >= 16U) {
        mag = (uint32_t)quarter_wave[16];
    } else {
        rem = (in * 16U) % len;
        mag = (uint32_t)((int32_t)quarter_wave[pos] +
                         (((int32_t)quarter_wave[pos+1] - (int32_t)quarter_wave[pos]) *
                          (int32_t)rem) / (int32_t)len);
    }
    return (int16_t)((quad >= 2U) ? -(int32_t)mag : (int32_t)mag);
}

/* Tono exactamente como Xinu audio_out_start(): llenar el anillo UNA vez con
 * un patron que cierre periodos completos y dejar que el DMA lo repita en
 * bucle (lista circular) mientras duerme ms.  Sin write/drain. */
bool p4_audio_tone(uint32_t hz, uint32_t ms) {
    if (hz == 0 || hz > 48000U) return false;
    if (!p4_audio_stream_start()) return false;

    /* Elegir un numero de periodos que llene exactamente el anillo:
     * frames = 48000/gcd(hz,48000) especimenes; repetir para 16384 frames. */
    uint32_t g = 48000U;
    uint32_t a = hz, b = 48000U;
    while (b) { uint32_t t = a % b; a = b; b = t; }
    uint32_t period = 48000U / a;              /* frames de un periodo */
    uint32_t frames = OUT_RING_BYTES / 4U;     /* 16384 frames en el anillo */
    uint32_t periods = frames / period;
    uint32_t step = (hz * 65536UL) / 48000U;

    uint8_t *r = streamRing;                   /* 0x88, sin cache */
    (void)step;
    for (uint32_t f = 0; f < periods * period; f++) {
        int16_t s = tone_sine_at((f * periods) % (periods * period), periods * period);
        uint8_t lo = (uint8_t)(s & 0xFF), hi = (uint8_t)((s >> 8) & 0xFF);
        uint32_t o = f * 4U;
        r[o] = lo; r[o+1] = hi;
        r[o+2] = lo; r[o+3] = hi;              /* duplicar slot (mono quirk) */
    }
    /* el anillo ya esta lleno: streamWritten debe reflejarlo para que el
     * productor no lo sobreescriba.  Aqui no hay productor: el DMA lee en bucle. */
    streamWritten = periods * period * 4U;

    thread_sleep(ms);                          /* suena mientras duerme */
    p4_audio_stream_stop();
    (void)g;
    return true;
}

/* ---------------------- captura (in) ---------------------- */
static uint32_t inDescBus, inDataBus;
static volatile link_desc *inDesc;
static uint8_t *inData;
static uint32_t inProduced, inConsumed, inDrainIdx, inDrainOff, inDropped;
static uint32_t inEofIdx, inEofStale;
static bool inEofSeen;

bool p4_audio_in_start(void) {
    mutex_acquire(&audio_lock);
    if (in_on) { mutex_release(&audio_lock); return true; }
    if (!p4_es8311_present() && !p4_es8311_init()) { mutex_release(&audio_lock); return false; }
    inDescBus = (uint32_t)(uintptr_t)_audio_in_desc_page;
    inDataBus = (uint32_t)(uintptr_t)_audio_in_buf_page;
    inDesc = (volatile link_desc *)(uintptr_t)(inDescBus + AUDIO_CPU_UNCACHED);
    inData = (uint8_t *)(uintptr_t)(inDataBus + AUDIO_CPU_UNCACHED);
    /* Xinu hace el zero dos veces seguidas (inofensivo pero lo reproducimos
     * por si el pipeline de caché/PSRAM lo necesita en el primer arranque). */
    psram_zero(inData, IN_CHUNK_BYTES * IN_CHUNKS);
    psram_zero(inData, IN_CHUNK_BYTES * IN_CHUNKS);

    for (uint32_t i = 0; i < IN_CHUNKS; i++) {
        inDesc[i].buffer = inDataBus + i * IN_CHUNK_BYTES;
        inDesc[i].next = inDescBus + ((i + 1U) % IN_CHUNKS) * sizeof(link_desc);
        inDesc[i].dw0 = DESC_SUC_EOF | DESC_OWNER_DMA | IN_CHUNK_BYTES |
                        (IN_CHUNK_BYTES << DESC_LEN_SHIFT);
    }
    MEM_BARRIER();

    if (!p4_i2s_rx_start()) { mutex_release(&audio_lock); return false; }
    p4_es8311_start_mic();
    /* Posicion de fin de recepcion previa, ANTES de enlazar el motor (quirk
     * de Xinu).  Si la direccion no es valida (tras el reset el registro vale
     * 0, menor que descBus), in_desc_index devuelve IN_CHUNKS (32), que nunca
     * coincide con un indice 0..31: asi el primer avistamiento cuenta bien.
     * Forzarlo a 0 hacia que el primer rec copiara el trozo en llenado ->
     * silencio; el segundo rec (con registro ya valido) si funcionaba. */
    uint32_t eof = p4_ahbdma_in_eof_desc();
    if (eof < inDescBus || (eof - inDescBus) % sizeof(link_desc) != 0U ||
        (eof - inDescBus) / sizeof(link_desc) >= IN_CHUNKS)
        inEofStale = IN_CHUNKS;
    else
        inEofStale = (eof - inDescBus) / sizeof(link_desc);
    inProduced = inConsumed = inDrainIdx = inDrainOff = inDropped = 0;
    inEofSeen = false; inEofIdx = 0;
    if (!p4_ahbdma_in_link(inDescBus)) { p4_i2s_rx_stop(); mutex_release(&audio_lock); return false; }
    in_on = true;
    mutex_release(&audio_lock);
    return true;
}

static void in_produce(void) {
    uint32_t eof = p4_ahbdma_in_eof_desc();
    uint32_t idx = (eof >= inDescBus) ? (eof - inDescBus) / sizeof(link_desc) : IN_CHUNKS;
    if (idx >= IN_CHUNKS) return;

    if (!inEofSeen) {
        /* Primer avistamiento: el registro IN_SUC_EOF es fiable SOLO la
         * primera vez tras reset.  En arranques posteriores conserva la
         * direccion vieja y compararla con eofStale desincroniza el contador
         * (produzia de mas y el lector se quedaba corto -> silencio).  Ignorar
         * la posicion inicial y contar desde cero es robusto siempre: se
         * descarta un unico trozo de transitorio, inofensivo. */
        inEofSeen = true;
        inEofIdx = idx;
        inProduced = 0U;
        return;
    }
    if (idx == inEofIdx) return;

    uint32_t move = (idx + IN_CHUNKS - inEofIdx) % IN_CHUNKS;
    inProduced += move;
    inEofIdx = idx;
    if (inProduced - inConsumed >= IN_CHUNKS) {
        uint32_t lost = (inProduced - inConsumed) - (IN_CHUNKS - 1U);
        inDropped += lost * (IN_CHUNK_BYTES / 4U);
        inConsumed += lost;
        inDrainIdx = inConsumed % IN_CHUNKS;
        inDrainOff = 0;
    }
}

uint32_t p4_audio_in_read(uint8_t *buf, uint32_t len) {
    mutex_acquire(&audio_lock);
    if (!in_on || !buf) { mutex_release(&audio_lock); return 0; }
    in_produce();
    uint32_t copied = 0;
    while (inConsumed < inProduced && copied < len) {
        uint32_t d = inDesc[inDrainIdx].dw0;
        uint32_t filled = (d >> DESC_LEN_SHIFT) & DESC_SIZE_MASK;
        if (filled > IN_CHUNK_BYTES) filled = IN_CHUNK_BYTES;
        uint32_t avail = filled - inDrainOff;
        uint32_t n = len - copied;
        if (n > avail) n = avail;
        if (n) {
            psram_copy(buf + copied, inData + inDrainIdx * IN_CHUNK_BYTES + inDrainOff, n);
            copied += n; inDrainOff += n;
        }
        if (inDrainOff >= filled) { inDrainIdx = (inDrainIdx + 1U) % IN_CHUNKS; inDrainOff = 0; inConsumed++; }
        else break;
    }
    mutex_release(&audio_lock);
    return copied;
}

void p4_audio_in_stop(void) {
    mutex_acquire(&audio_lock);
    if (!in_on) { mutex_release(&audio_lock); return; }
    p4_ahbdma_in_stop();
    p4_i2s_rx_stop();
    p4_es8311_stop_mic();
    in_on = false;
    mutex_release(&audio_lock);
}
uint32_t p4_audio_in_dropped(void) { return inDropped; }


/* ---------------------- estado ---------------------- */
uint32_t p4_audio_out_sync_count(void) { return p4_i2s_tx_sync_count(); }
bool p4_audio_clocks_running(void) { return p4_i2s_clocks_running(); }
void p4_audio_status(void) {
    printf("audio: TX %s RX %s | sync=%u queued=%u gaps=%u dropped=%u\n",
           p4_ahbdma_tx_running() ? "on" : "off", p4_ahbdma_in_running() ? "on" : "off",
           p4_i2s_tx_sync_count(), p4_audio_stream_queued(), streamGaps, inDropped);
    volatile uint32_t *g = (volatile uint32_t *)0x500E0000UL;
    printf("audio: AMP53 in=%d latch=%d en=%d | vol=%d dB mute=%d | I2S=%s\n",
           gpio_get(P4_I2S_AMP_POWER), (int)((g[0x10/4] >> 21) & 1),
           (int)((g[0x2C/4] >> 21) & 1),
           p4_es8311_get_volume_db(), p4_es8311_get_mute(),
           p4_i2s_clocks_running() ? "on" : "off");
}
