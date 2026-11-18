#include <dev/wifi/sdio.h>
#include <dev/wifi/esp_hosted_sdio.h>
#include <dev/wifi/esp_hosted.h>
#include <dev/wifi/esp_hosted_ctrl.h>
#include <dev/wifi/wifi.h>
#include <lib/minip.h>
#include <lib/minip/netif.h>
#include <lk/console_cmd.h>
#include <stdio.h>
#include <string.h>
#include <kernel/thread.h>
#include <platform/time.h>

/* --- Parseo protobuf minimo para el scan (adaptado de Xinu xsh_wifi_scan.c) --- */
static bool scan_read_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *out) {
    uint64_t result = 0U;
    unsigned shift = 0U;
    while (*off < len && shift < 64U) {
        uint8_t b = buf[(*off)++];
        result |= ((uint64_t)(b & 0x7FU)) << shift;
        if ((b & 0x80U) == 0U) { *out = result; return true; }
        shift += 7U;
    }
    return false;
}

/* Busca el campo `field` (wire type 2, longitud) dentro de un mensaje protobuf. */
static const uint8_t *scan_find_bytes(const uint8_t *buf, size_t len, uint32_t field,
                                      size_t *out_len) {
    size_t off = 0U;
    while (off < len) {
        uint64_t key, v;
        if (!scan_read_varint(buf, len, &off, &key)) return NULL;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 2U) {
            if (!scan_read_varint(buf, len, &off, &v) || v > (uint64_t)(len - off)) return NULL;
            if (f == field) { *out_len = (size_t)v; return buf + off; }
            off += (size_t)v;
        } else if (wt == 0U) {
            if (!scan_read_varint(buf, len, &off, &v)) return NULL;
        } else if (wt == 5U) { off += 4U; }
        else if (wt == 1U) { off += 8U; }
        else return NULL;
        if (off > len) return NULL;
    }
    return NULL;
}

/* raw_request ya devuelve el protobuf sin el TLV (request_common copia
 * rpc_payload).  Buscar directamente el campo de bytes con numero == resp_id. */
static const uint8_t *scan_rpc_payload(const uint8_t *resp, size_t len, uint32_t resp_id,
                                       size_t *out_len) {
    return scan_find_bytes(resp, len, resp_id, out_len);
}

static uint32_t scan_varint(const uint8_t *buf, size_t len, uint32_t field) {
    size_t off = 0U;
    while (off < len) {
        uint64_t key, v;
        if (!scan_read_varint(buf, len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 2U) {
            if (!scan_read_varint(buf, len, &off, &v) || v > (uint64_t)(len - off)) break;
            off += (size_t)v;
        } else if (wt == 0U) {
            if (!scan_read_varint(buf, len, &off, &v)) break;
            if (f == field) return (uint32_t)v;
        } else break;
    }
    return 0U;
}

static void scan_print_record(uint32_t idx, const uint8_t *rec, size_t len) {
    size_t off = 0U;
    uint64_t key, v;
    char ssid[33] = {0};
    uint8_t bssid[6] = {0};
    uint32_t channel = 0U, authmode = 0U;
    int32_t rssi = 0;
    bool have_ssid = false, have_bssid = false;
    while (off < len) {
        if (!scan_read_varint(rec, len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 0U) {
            if (!scan_read_varint(rec, len, &off, &v)) break;
            if (f == 3U) channel = (uint32_t)v;
            else if (f == 4U) authmode = (uint32_t)v;
            else if (f == 5U) rssi = (int32_t)(uint32_t)v;
        } else if (wt == 2U) {
            if (!scan_read_varint(rec, len, &off, &v) || v > (uint64_t)(len - off)) break;
            size_t n = (size_t)v;
            if (n == 6U && !have_bssid) { memcpy(bssid, rec + off, 6U); have_bssid = true; }
            else if (f == 2U && !have_ssid && n > 0U) {
                size_t slen = n;
                while (slen > 0U && rec[off + slen - 1U] == 0U) slen--;
                if (slen > 0U && slen <= 32U) {
                    memcpy(ssid, rec + off, slen); ssid[slen] = '\0'; have_ssid = true;
                }
            }
            off += n;
        } else break;
    }
    printf("  [%u] %-32s ch=%u auth=%u rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
           (unsigned)idx, have_ssid ? ssid : "?", (unsigned)channel, (unsigned)authmode,
           (int)rssi, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

/* --- senal del AP asociado (port de Xinu wifiSignalRead, RPC 294) ---
 * Contesta el AP asociado cuando hay enlace y 0x300f cuando no.  Se usa para
 * NO reiniciar la WiFi del C6 si ya esta asociada (repetir 278/260/280 con
 * enlace tira la conexion y el 286 deja de contestar: medido en Xinu). */
static bool wifi_signal_read(int32_t *rssi, uint32_t *channel) {
    static uint8_t resp[300];
    size_t resp_len = 0U, body_len = 0U, off = 0U;
    const uint8_t *body;
    uint64_t key, v;
    if (!esp_hosted_sdio_is_attached()) return false;
    if (esp_hosted_ctrl_raw_request(294U, NULL, 0U, resp, sizeof(resp), &resp_len, 1000U) != ESP_HOSTED_CTRL_OK)
        return false;
    body = scan_rpc_payload(resp, resp_len, 550U, &body_len);
    if (!body) return false;
    /* campo 2 (bytes) = registro del AP asociado; dentro: f3=canal, f5=rssi.
     * Solo hay enlace si existe el registro y trae f5 (rssi), como Xinu. */
    const uint8_t *rec = NULL; size_t rec_len = 0U;
    off = 0U;
    while (off < body_len) {
        if (!scan_read_varint(body, body_len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 2U) {
            if (!scan_read_varint(body, body_len, &off, &v) || v > (uint64_t)(body_len - off)) break;
            if (f == 2U) { rec = body + off; rec_len = (size_t)v; }
            off += (size_t)v;
        } else if (wt == 0U) {
            if (!scan_read_varint(body, body_len, &off, &v)) break;
        } else if (wt == 5U) { off += 4U; }
        else if (wt == 1U) { off += 8U; }
        else break;
    }
    if (!rec) return false;
    bool have_rssi = false;
    off = 0U;
    while (off < rec_len) {
        if (!scan_read_varint(rec, rec_len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt != 0U) { if (wt == 2U) { if (!scan_read_varint(rec, rec_len, &off, &v) || v > (uint64_t)(rec_len-off)) break; off += (size_t)v; continue; } else break; }
        if (!scan_read_varint(rec, rec_len, &off, &v)) break;
        if (f == 3U) *channel = (uint32_t)v;
        else if (f == 5U) { *rssi = (int32_t)v; have_rssi = true; }
    }
    return have_rssi;
}

/* Sacar el bloque protobuf del sobre TLV de una trama del esclavo (Xinu tlv_find_data). */
static const uint8_t *scan_tlv_data(const uint8_t *buf, size_t len, size_t *out_len) {
    size_t off = 0U;
    while (off + 3U <= len) {
        uint8_t type = buf[off];
        size_t blen = (size_t)buf[off + 1U] | ((size_t)buf[off + 2U] << 8U);
        off += 3U;
        if (off + blen > len) return NULL;
        if (type == 3U) { *out_len = blen; return &buf[off]; }  /* HOSTED_TLV_DATA = 3 */
        off += blen;
    }
    return NULL;
}

/* Cabecera RpcPacket: type, msg_id, uid (Xinu rpc_read_header). */
static bool scan_rpc_header(const uint8_t *pb, size_t len, uint32_t *type, uint32_t *msg_id) {
    size_t off = 0U;
    uint64_t key, v;
    while (off < len) {
        if (!scan_read_varint(pb, len, &off, &key)) return false;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt != 0U) return false;
        if (!scan_read_varint(pb, len, &off, &v)) return false;
        if (f == 1U) *type = (uint32_t)v;
        else if (f == 2U) *msg_id = (uint32_t)v;
        else if (f == 3U) break;  /* uid: oneof empieza despues */
    }
    return true;
}

static int wifi_scan_cmd(void) {
    static uint8_t resp[1700];
    size_t resp_len = 0U;
    uint8_t arg[8];
    size_t arg_len;
    const uint8_t *body;
    size_t body_len;
    int32_t rssi = 0;
    uint32_t channel = 0U;

    if (!esp_hosted_sdio_is_attached()) { printf("wifi: no adjunto; usa 'wifi up'\n"); return -1; }
    esp_hosted_ctrl_init();

    /* Encender la WiFi del NCP (278 init + 260 STA + 280 start) SOLO si aun no
     * hay enlace (port de Xinu wifiScanList).  El flag del controlador lo pone
     * el evento StaConnected (775) y es mas confiable que el RPC 294. */
    bool linked = esp_hosted_wifi_sta_is_connected() || wifi_signal_read(&rssi, &channel);
    if (!linked) {
        printf("wifi scan: encendiendo WiFi del NCP (278/260/280)...\n");
        esp_hosted_wifi_init_default(3000U);
        esp_hosted_wifi_set_mode(ESP_HOSTED_WIFI_MODE_STA, 3000U);
        esp_hosted_wifi_start(3000U);
    } else {
        printf("wifi scan: el NCP ya tiene enlace; no se reinicia\n");
    }

    printf("wifi scan: arrancando barrido (id=286)...\n");
    if (esp_hosted_ctrl_raw_request(286U, NULL, 0U, resp, sizeof(resp), &resp_len, 3000U) != ESP_HOSTED_CTRL_OK) {
        printf("wifi scan: el NCP no contesto al arranque del barrido\n");
        return -1;
    }

    /* Barrido asincrono: evento 774 al terminar.  Bombear hasta 15 s y salir
     * 3 s despues del ultimo evento (patron quiet de Xinu). */
    {
        esp_hosted_packet_t pkt;
        uint32_t quiet = 0U, scans = 0U;
        for (int i = 0; i < 1500U; i++) {
            uint32_t got = 0U;
            esp_hosted_poll();
            while (esp_hosted_rx_pop(&pkt)) {
                size_t pl = 0U;
                const uint8_t *pb;
                uint32_t type = 0U, msg_id = 0U;
                if (pkt.info.if_type != ESP_HOSTED_IF_SERIAL) continue;
                pb = scan_tlv_data(pkt.payload, pkt.len, &pl);
                if (!pb || !scan_rpc_header(pb, pl, &type, &msg_id)) continue;
                if (type == 3U && msg_id == 774U) { got = 1U; scans++; }
            }
            spin(10000U);  /* 10 ms */
            quiet = got ? 0U : quiet + 10U;
            if (scans != 0U && quiet >= 1500U) break;
        }
        printf("wifi scan: %u eventos 774 del barrido\n", (unsigned)scans);
    }

    if (esp_hosted_ctrl_raw_request(288U, NULL, 0U, resp, sizeof(resp), &resp_len, 3000U) != ESP_HOSTED_CTRL_OK) {
        printf("wifi scan: no llego el recuento (id=288)\n");
        return -1;
    }
    body = scan_rpc_payload(resp, resp_len, 544U, &body_len);
    if (!body) { printf("wifi scan: respuesta de recuento inesperada\n"); return -1; }
    uint32_t count = scan_varint(body, body_len, 2U);
    printf("wifi scan: %u redes vistas por el C6\n", (unsigned)count);

    uint32_t idx = 0U;
    uint32_t batch = 6U;
    while (idx < count) {
        uint32_t ask = (count - idx < batch) ? (count - idx) : batch;
        arg_len = 0U;
        arg[arg_len++] = 0x08U;
        uint64_t av = ask;
        do { arg[arg_len++] = (uint8_t)((av & 0x7FU) | (av > 0x7FU ? 0x80U : 0U)); av >>= 7U; } while (av);
        if (esp_hosted_ctrl_raw_request(289U, arg, arg_len, resp, sizeof(resp), &resp_len, 3000U) != ESP_HOSTED_CTRL_OK) {
            printf("  [%u] sin respuesta\n", (unsigned)idx);
            break;
        }
        body = scan_rpc_payload(resp, resp_len, 545U, &body_len);
        if (!body) { printf("wifi scan: respuesta de registros inesperada\n"); break; }
        uint32_t got = scan_varint(body, body_len, 2U);
        (void)got;
        uint32_t before = idx;
        size_t off = 0U;
        while (off < body_len) {
            uint64_t key, v;
            if (!scan_read_varint(body, body_len, &off, &key)) break;
            uint32_t f = (uint32_t)(key >> 3U);
            uint8_t wt = (uint8_t)(key & 7U);
            if (wt == 0U) { if (!scan_read_varint(body, body_len, &off, &v)) break; continue; }
            if (wt != 2U) break;
            if (!scan_read_varint(body, body_len, &off, &v) || v > (uint64_t)(body_len - off)) break;
            if (f == 3U) { scan_print_record(idx, body + off, (size_t)v); idx++; }
            off += (size_t)v;
        }
        if (idx == before) {
            if (batch > 1U) { batch /= 2U; continue; }
            printf("wifi scan: sin registros nuevos\n");
            break;
        }
    }
    printf("wifi scan: %u APs listados\n", (unsigned)idx);
    return 0;
}

static int cmd_wifi(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("usage: wifi <probe|up|down|ver|mac|join|scan|dhcp|status>\n");
        printf("  wifi probe\n");
        printf("  wifi up\n");
        printf("  wifi ver\n");
        printf("  wifi mac\n");
        printf("  wifi join <ssid> [password]\n");
        printf("  wifi scan\n");
        printf("  wifi dhcp\n");
        printf("  wifi status\n");
        return 0;
    }
    if (!strcmp(argv[1].str, "probe")) {
        struct sdio_info info;
        status_t st = sdio_enumerate(&info);
        sdio_probe_report(&info);
        if (st < 0) printf("wifi probe: error %d\n", (int)st);
        return (int)st;
    }
    if (!strcmp(argv[1].str, "up")) {
        status_t st = wifi_attach_and_up();
        if (st < 0) { printf("wifi up failed: %d\n", (int)st); return (int)st; }
        for (int i = 0; i < 100; i++) { esp_hosted_poll(); esp_hosted_dispatch_rx(); spin(1000U); }
        printf("wifi up: ok (attached=%d ready=%d)\n", esp_hosted_sdio_is_attached(), esp_hosted_ctrl_is_ready());
        return 0;
    }
    if (!strcmp(argv[1].str, "down")) {
        esp_hosted_ctrl_deinit();
        esp_hosted_sdio_detach();
        printf("wifi down\n");
        return 0;
    }
    if (!strcmp(argv[1].str, "ver")) {
        uint32_t maj=0,min=0,patch=0;
        if (!esp_hosted_is_initialized()) { printf("wifi not up\n"); return -1; }
        int rc = esp_hosted_ctrl_get_coprocessor_fw_version(3000, &maj, &min, &patch);
        if (rc == ESP_HOSTED_CTRL_OK) printf("C6 fw version: %u.%u.%u\n", (unsigned)maj,(unsigned)min,(unsigned)patch);
        else printf("wifi ver failed: %d\n", rc);
        return rc;
    }
    if (!strcmp(argv[1].str, "mac")) {
        uint8_t mac[6]={0};
        int rc = esp_hosted_wifi_get_mac(ESP_HOSTED_WIFI_IF_STA, mac, 3000);
        if (rc == ESP_HOSTED_CTRL_OK) printf("STA MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        else printf("wifi mac failed: %d\n", rc);
        return rc;
    }
    if (!strcmp(argv[1].str, "init")) {
        int rc = esp_hosted_wifi_init_default(5000);
        if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi init_default failed %d\n", rc); return rc; }
        rc = esp_hosted_wifi_set_mode(ESP_HOSTED_WIFI_MODE_STA, 3000);
        if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi set_mode failed %d\n", rc); return rc; }
        rc = esp_hosted_wifi_start(3000);
        if (rc != ESP_HOSTED_CTRL_OK) { printf("wifi start failed %d\n", rc); return rc; }
        printf("wifi init: ok\n");
        return 0;
    }
    if (!strcmp(argv[1].str, "join")) {
        if (argc < 3) { printf("usage: wifi join <ssid> [password]\n"); return -1; }
        const char *ssid = argv[2].str;
        const char *pass = (argc >= 4) ? argv[3].str : "";
        status_t st = wifi_join(ssid, pass);
        if (st < 0) { printf("wifi join failed: %d\n", (int)st); return (int)st; }
        printf("wifi join: requested (connected=%d)\n", esp_hosted_wifi_sta_is_connected());
        return 0;
    }
    if (!strcmp(argv[1].str, "scan")) {
        return wifi_scan_cmd();
    }
    if (!strcmp(argv[1].str, "dhcp")) {
        void *netif = wifi_netif_get();
        if (!netif) { printf("wifi dhcp: wlan0 no listo (usa 'wifi up' primero)\n"); return -1; }
        /* netif_register() ya arranca DHCP una vez al registrar wlan0; llamar
         * minip_start_dhcp aqui crearia un segundo thread compitiendo. */
        status_t st = minip_wait_for_configured(20000);
        printf("wifi dhcp: wait=%d configured=%d\n", st, minip_is_configured());
        return st;
    }
    if (!strcmp(argv[1].str, "status")) {
        printf("attached=%d initialized=%d ctrl_ready=%d sta_connected=%d\n",
               esp_hosted_sdio_is_attached(), esp_hosted_is_initialized(), esp_hosted_ctrl_is_ready(),
               esp_hosted_wifi_sta_is_connected());
        printf("rx_pending=%u rx_dropped=%u\n",
               (unsigned)esp_hosted_rx_pending(), (unsigned)esp_hosted_rx_dropped());
        return 0;
    }
    if (!strcmp(argv[1].str, "ep")) {
        extern const char *esp_hosted_ctrl_ep_name;
        if (argc >= 3) esp_hosted_ctrl_ep_name = argv[2].str;
        printf("endpoint H2S = %s\n", esp_hosted_ctrl_ep_name);
        return 0;
    }
    if (!strcmp(argv[1].str, "dbg")) {
        extern void esp_hosted_sdio_debug_dump(void);
        esp_hosted_sdio_debug_dump();
        return 0;
    }
    printf("unknown subcommand\n");
    return -1;
}

STATIC_COMMAND_START
STATIC_COMMAND("wifi", "WiFi/SDIO (ESP32-C6) commands", &cmd_wifi)
STATIC_COMMAND_END(wifi);
