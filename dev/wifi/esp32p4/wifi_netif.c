/*
 * Interfaz de red wlan0 sobre lib/minip para el ESP-Hosted (ESP32-C6).
 * El transporte/control (wifi_attach_and_up, wifi_join, wifi_set_mac_from_c6)
 * vive en wifi_ctrl.c; aqui solo se crea el netif, se registran los callbacks
 * RX y se bomborea el stack.  Adaptado del bring-up, con los delays de sondeo
 * cambiados a spin() (busy-wait) para igualar el delay_ms()/mdelay() de Xinu:
 * thread_sleep() es un reschedule de LK y no equivale al delay del chip.
 */
#include <dev/wifi/esp_hosted.h>
#include <dev/wifi/esp_hosted_sdio.h>
#include <dev/wifi/esp_hosted_ctrl.h>
#include <dev/wifi/wifi.h>
#include <lib/minip.h>
#include <lib/minip/netif.h>
#include <lib/pktbuf.h>
#include <lk/err.h>
#include <stdio.h>
#include <string.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <platform/time.h>

#define LOCAL_TRACE 0

static netif_t wifi_netif;
static bool wifi_netif_ready;
static thread_t *wifi_poll_thread;
static mutex_t wifi_lock = MUTEX_INITIAL_VALUE(wifi_lock);

static int wifi_tx(void *arg, pktbuf_t *p) {
    (void)arg;
    if (!esp_hosted_sdio_is_attached() || !p) {
        if (p) pktbuf_free(p, true);
        return ERR_NOT_READY;
    }
    size_t len = p->dlen;
    size_t sent = esp_hosted_lwip_tx_to_if(ESP_HOSTED_IF_STA, 0, p->data, len);
    pktbuf_free(p, true);
    return (sent == len) ? 0 : (int)ERR_IO;
}

static void wifi_rx_cb(const esp_hosted_frame_info_t *info, const uint8_t *payload, size_t len, void *ctx) {
    (void)ctx;
    if (!wifi_netif_ready || !info || !payload || len == 0U) return;
    if (info->if_type != ESP_HOSTED_IF_STA && info->if_type != ESP_HOSTED_IF_AP &&
        info->if_type != ESP_HOSTED_IF_ETH) return;
    (void)minip_rx_driver_callback_copy(&wifi_netif, payload, len);
}

static int wifi_poll_thread_fn(void *arg) {
    (void)arg;
    /* Hilo de fondo: bombea el C6 y cede el CPU con thread_sleep(1) entre
     * vueltas, igual que el hilo netRecv/espethRead de Xinu (sleep(1)).  El
     * spin() del driver (timeout de RPC) se mantiene; aqui un spin continuo en
     * el mismo core de la consola la bloquea. */
    for (;;) {
        if (esp_hosted_sdio_is_attached() && esp_hosted_is_initialized()) {
            esp_hosted_poll();
            esp_hosted_dispatch_rx();
        }
        thread_sleep(1);
    }
    return 0;
}

status_t wifi_netif_start(void) {
    mutex_acquire(&wifi_lock);
    if (!wifi_netif_ready) {
        uint8_t mac[6] = {0};
        netif_create(&wifi_netif, "wlan0");
        /* MAC temporal hasta que el control lea la real del C6; se puede
         * sobreescribir con wifi_netif_set_mac() tras wifi join. */
        gen_random_mac_address(mac);
        netif_set_eth(&wifi_netif, wifi_tx, NULL, mac);
        netif_register(&wifi_netif);
        wifi_netif_ready = true;
    }
    if (!wifi_poll_thread) {
        /* En core 0 (donde arranca el attach SDIO): moverlo al core 1 hace
         * que el poll tome el bus sdmmc justo mientras el attach hace reset
         * y el arranque se cuelga en 'sdio pins ok reset' (medido en HW).
         * La contencion con la microSD se resuelve soltando sdmmc_bus_lock()
         * entre bloques en sd_bio_write (ver ahi), no moviendo este hilo. */
        wifi_poll_thread = thread_create("wifi_poll", wifi_poll_thread_fn, NULL,
                                         DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
        if (wifi_poll_thread) {
            thread_set_pinned_cpu(wifi_poll_thread, 0);
            thread_resume(wifi_poll_thread);
        }
    }
    mutex_release(&wifi_lock);
    return NO_ERROR;
}

/* Actualiza la MAC del netif con la que reporta el C6. */
void wifi_netif_set_mac(const uint8_t mac[6]) {
    if (wifi_netif_ready && mac) {
        /* netif_set_eth() ya se llamo en wifi_netif_start() con una MAC temporal
         * y no permite una segunda llamada (DEBUG_ASSERT).  Sobrecribir la MAC
         * directamente con la que reporta el C6. */
        memcpy(wifi_netif.mac_address, mac, 6);
    }
}

bool wifi_netif_is_ready(void) { return wifi_netif_ready; }

void *wifi_netif_get(void) { return wifi_netif_ready ? (void *)&wifi_netif : NULL; }

void wifi_netif_input_cb_register(void) {
    esp_hosted_lwip_set_input_cb(wifi_rx_cb, NULL);
}
