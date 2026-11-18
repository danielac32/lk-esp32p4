#include <dev/wifi/esp_hosted.h>
#include <dev/wifi/esp_hosted_sdio.h>
#include <dev/wifi/esp_hosted_ctrl.h>
#include <dev/wifi/wifi.h>
#include <lk/err.h>
#include <stdio.h>
#include <string.h>
#include <kernel/thread.h>
#include <platform/time.h>

/* Handshake PRIV (igual que Xinu xsh_wifi.c): el C6 sube su EVENT_INIT por el
 * canal PRIV y espera que el host conteste con sus capacidades antes de
 * atender cualquier RPC.  Sin esta respuesta `wifi ver` muere en silencio. */
#define HOSTED_PRIV_PKT_EVENT        0x33U
#define HOSTED_PRIV_EVENT_INIT       0x22U
#define HOSTED_PRIV_CAPABILITY       0x11U
#define HOSTED_PRIV_FIRMWARE_CHIP_ID 0x12U
#define HOSTED_PRIV_CAP_EXT          0x16U
#define HOSTED_PRIV_TRANS_SDIO_MODE  0x18U
#define HOSTED_HOST_CAPABILITIES     0x44U
#define HOSTED_HOST_SLAVE_CHIP_ID    0x45U
#define HOSTED_HOST_RAW_TP           0x46U
#define HOSTED_HOST_THROTTLE_HIGH    0x47U
#define HOSTED_HOST_THROTTLE_LOW     0x48U

static bool s_hosted_init_seen;
static bool s_hosted_init_replied;
static uint8_t s_hosted_slave_chip_id;

static bool hosted_send_init_response(uint8_t chip_id) {
    uint8_t buf[17];
    memset(buf, 0, sizeof(buf));
    buf[0] = HOSTED_PRIV_EVENT_INIT; buf[1] = 15U;
    buf[2] = HOSTED_HOST_CAPABILITIES;     buf[3] = 1U; buf[4] = 0U;
    buf[5] = HOSTED_HOST_SLAVE_CHIP_ID;    buf[6] = 1U; buf[7] = chip_id;
    buf[8] = HOSTED_HOST_RAW_TP;           buf[9] = 1U; buf[10] = 0U;
    buf[11] = HOSTED_HOST_THROTTLE_HIGH;   buf[12] = 1U; buf[13] = 0U;
    buf[14] = HOSTED_HOST_THROTTLE_LOW;    buf[15] = 1U; buf[16] = 0U;
    size_t written = esp_hosted_priv_tx(HOSTED_PRIV_PKT_EVENT, buf, sizeof(buf));
    if (written != sizeof(buf)) {
        printf("wifi: init response short (%u/%u)\n", (unsigned)written, (unsigned)sizeof(buf));
        return false;
    }
    printf("wifi: init response sent (chip=0x%02x)\n", (unsigned)chip_id);
    return true;
}

static void wifi_priv_cb(const esp_hosted_frame_info_t *info, const uint8_t *payload,
                         size_t len, void *ctx) {
    (void)info; (void)ctx;
    if (!payload || len < 2U || payload[0] != HOSTED_PRIV_EVENT_INIT) return;
    if (len < (size_t)(2U + payload[1])) return;
    s_hosted_init_seen = true;
    const uint8_t *pos = payload + 2U;
    size_t remaining = payload[1];
    while (remaining >= 2U) {
        uint8_t tag = pos[0], tag_len = pos[1];
        if ((size_t)(tag_len + 2U) > remaining) break;
        if (tag == HOSTED_PRIV_FIRMWARE_CHIP_ID && tag_len >= 1U) s_hosted_slave_chip_id = pos[2];
        pos += (size_t)tag_len + 2U;
        remaining -= (size_t)tag_len + 2U;
    }
    printf("wifi: PRIV EVENT_INIT seen (chip=0x%02x)\n", (unsigned)s_hosted_slave_chip_id);
    if (!s_hosted_init_replied) s_hosted_init_replied = hosted_send_init_response(s_hosted_slave_chip_id);
}

status_t wifi_attach_and_up(void) {
    esp_hosted_sdio_config_t cfg;
    esp_hosted_sdio_get_default_config(&cfg);
    printf("wifi: attach_and_up start\n");

    if (!esp_hosted_sdio_is_attached()) {
        printf("wifi: calling sdio attach...\n");
        if (!esp_hosted_sdio_attach(&cfg)) {
            printf("wifi: sdio attach failed\n");
            return ERR_IO;
        }
        printf("wifi: sdio attach ok\n");
    }
    if (!esp_hosted_is_initialized()) {
        printf("wifi: esp_hosted_init...\n");
        esp_hosted_init(NULL); /* transport set by attach path */
    }
    printf("wifi: ctrl init...\n");
    if (!esp_hosted_ctrl_init()) {
        printf("wifi: ctrl init failed\n");
        return ERR_IO;
    }
    /* Registrar callback PRIV para el handshake y bombear hasta que el C6
     * suba su EVENT_INIT, sea respondido y el canal de control este listo. */
    esp_hosted_priv_set_rx_cb(wifi_priv_cb, NULL);
    for (int i = 0; i < 100; i++) {
        esp_hosted_poll();
        esp_hosted_dispatch_rx();
        if (s_hosted_init_replied && esp_hosted_ctrl_is_ready()) break;
        spin(1000U);  /* delay busy-wait como Xinu */
    }
    printf("wifi: handshake seen=%d replied=%d ready=%d\n",
           (int)s_hosted_init_seen, (int)s_hosted_init_replied,
           (int)esp_hosted_ctrl_is_ready());

    /* Crear wlan0 y registrar el callback de datos (STA/AP/ETH) hacia minip. */
    wifi_netif_input_cb_register();
    wifi_netif_start();
    printf("wifi: attach_and_up done\n");
    return NO_ERROR;
}

status_t wifi_set_mac_from_c6(void) {
    uint8_t mac[6] = {0};
    int rc = esp_hosted_wifi_get_mac(ESP_HOSTED_WIFI_IF_STA, mac, 3000);
    if (rc == ESP_HOSTED_CTRL_OK) {
        printf("wifi: C6 STA MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        wifi_netif_set_mac(mac);
        return NO_ERROR;
    }
    printf("wifi: get mac failed (%d)\n", rc);
    return ERR_IO;
}

/* Estado del ultimo evento wifi del C6 (callback, como Xinu xsh_wifi.c):
 * el motivo de desasociacion distingue "SSID no visto" de "clave mala". */
static volatile int s_wifi_evt_seen; /* 0 ninguno, 1 conectado, 2 desconectado */
static volatile uint32_t s_wifi_evt_reason;

static void wifi_event_cb(esp_hosted_wifi_event_t event, const void *data, void *ctx) {
    (void)ctx;
    if (event == ESP_HOSTED_WIFI_EVENT_STA_CONNECTED) {
        const esp_hosted_wifi_sta_connected_t *e = data;
        s_wifi_evt_seen = 1;
        if (e) printf("wifi: evento StaConnected ssid=\"%.*s\" ch=%u\n",
                      (int)e->ssid_len, (const char *)e->ssid, (unsigned)e->channel);
    } else if (event == ESP_HOSTED_WIFI_EVENT_STA_DISCONNECTED) {
        const esp_hosted_wifi_sta_disconnected_t *e = data;
        s_wifi_evt_seen = 2;
        if (e) {
            s_wifi_evt_reason = e->reason;
            printf("wifi: evento StaDisconnected reason=%u\n", (unsigned)e->reason);
        }
    }
}

/* Un intento de asociacion: config + start + connect + espera del evento 775.
 * Devuelve true si el C6 confirmo StaConnected.  Bombeo con spin() (delay
 * busy-wait como delay_ms de Xinu): thread_sleep retrasa los eventos. */
static bool wifi_join_attempt(const char *ssid, const char *pass) {
    int rc = esp_hosted_wifi_set_sta_config(ssid, pass ? pass : "", 3000);
    if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi: set_sta_config failed %d\n", rc); return false; }
    rc = esp_hosted_wifi_start(3000);
    if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi: start failed %d\n", rc); return false; }
    rc = esp_hosted_wifi_connect(15000);
    if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi: connect failed %d\n", rc); return false; }

    /* 24 x 500ms de espera por el evento, como Xinu (WIFI_JOIN_EVENT_TRIES). */
    for (int i = 0; i < 24; i++) {
        esp_hosted_poll();
        esp_hosted_dispatch_rx();
        if (esp_hosted_wifi_sta_is_connected() || s_wifi_evt_seen == 1)
            return true;
        if (s_wifi_evt_seen == 2 && i >= 10) break;  // motivo terminal
        spin(500000U);  /* 500 ms busy-wait */
    }
    return false;
}

#define WIFI_JOIN_ATTEMPTS 3

status_t wifi_join(const char *ssid, const char *pass) {
    if (!ssid) return ERR_INVALID_ARGS;
    status_t st = wifi_attach_and_up();
    if (st < 0) return st;

    int rc = esp_hosted_wifi_init_default(5000);
    if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi: init_default failed %d\n", rc); return ERR_IO; }
    rc = esp_hosted_wifi_set_mode(ESP_HOSTED_WIFI_MODE_STA, 3000);
    if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi: set_mode failed %d\n", rc); return ERR_IO; }

    /* Registrar callback de eventos: sin el, el motivo de rechazo se descarta. */
    s_wifi_evt_seen = 0;
    s_wifi_evt_reason = 0;
    esp_hosted_wifi_set_event_cb(wifi_event_cb, NULL);
    (void)wifi_set_mac_from_c6();

    /* La AP a veces rechaza con motivo 39 y acepta en el siguiente intento,
     * con el mismo RSSI y configuracion (medido en Xinu).  Desasociar entre
     * intentos deja el supplicant del C6 en limpio. */
    for (int attempt = 1; attempt <= WIFI_JOIN_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            printf("wifi: intento %d de %d\n", attempt, WIFI_JOIN_ATTEMPTS);
            esp_hosted_wifi_disconnect(3000);
            spin(1000000U);
            s_wifi_evt_seen = 0;
        }
        if (wifi_join_attempt(ssid, pass)) {
            printf("wifi: STA connected a \"%s\"\n", ssid);
            return NO_ERROR;
        }
    }
    printf("wifi: no asocio tras %d intentos (rx_pending=%u dropped=%u reason=%u)\n",
           WIFI_JOIN_ATTEMPTS, (unsigned)esp_hosted_rx_pending(),
           (unsigned)esp_hosted_rx_dropped(), (unsigned)s_wifi_evt_reason);
    return ERR_IO;
}

bool wifi_is_attached(void) { return esp_hosted_sdio_is_attached(); }
