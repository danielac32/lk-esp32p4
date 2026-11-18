#include <dev/wifi/scan.h>
#include <dev/wifi/sdio.h>
#include <dev/wifi/esp_hosted_sdio.h>
#include <dev/wifi/esp_hosted.h>
#include <dev/wifi/esp_hosted_ctrl.h>
#include <dev/wifi/wifi.h>
#include <stdio.h>
#include <string.h>
#include <kernel/thread.h>
#include <platform/time.h>

static bool sc_read_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *out) {
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

static const uint8_t *sc_find_bytes(const uint8_t *buf, size_t len, uint32_t field,
                                    size_t *out_len) {
    size_t off = 0U;
    while (off < len) {
        uint64_t key, v;
        if (!sc_read_varint(buf, len, &off, &key)) return NULL;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 2U) {
            if (!sc_read_varint(buf, len, &off, &v) || v > (uint64_t)(len - off)) return NULL;
            if (f == field) { *out_len = (size_t)v; return buf + off; }
            off += (size_t)v;
        } else if (wt == 0U) {
            if (!sc_read_varint(buf, len, &off, &v)) return NULL;
        } else if (wt == 5U) { off += 4U; }
        else if (wt == 1U) { off += 8U; }
        else return NULL;
        if (off > len) return NULL;
    }
    return NULL;
}

static const uint8_t *sc_rpc_payload(const uint8_t *resp, size_t len, uint32_t resp_id,
                                     size_t *out_len) {
    return sc_find_bytes(resp, len, resp_id, out_len);
}

static uint32_t sc_varint(const uint8_t *buf, size_t len, uint32_t field) {
    size_t off = 0U;
    while (off < len) {
        uint64_t key, v;
        if (!sc_read_varint(buf, len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 2U) {
            if (!sc_read_varint(buf, len, &off, &v) || v > (uint64_t)(len - off)) break;
            off += (size_t)v;
        } else if (wt == 0U) {
            if (!sc_read_varint(buf, len, &off, &v)) break;
            if (f == field) return (uint32_t)v;
        } else break;
    }
    return 0U;
}

static void sc_parse_record(const uint8_t *rec, size_t len, wifi_scan_result_t *out) {
    size_t off = 0U;
    uint64_t key, v;
    memset(out, 0, sizeof(*out));
    bool have_ssid = false, have_bssid = false;
    while (off < len) {
        if (!sc_read_varint(rec, len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 0U) {
            if (!sc_read_varint(rec, len, &off, &v)) break;
            if (f == 3U) out->channel = (uint32_t)v;
            else if (f == 4U) out->authmode = (uint32_t)v;
            else if (f == 5U) out->rssi = (int32_t)(uint32_t)v;
        } else if (wt == 2U) {
            if (!sc_read_varint(rec, len, &off, &v) || v > (uint64_t)(len - off)) break;
            size_t n = (size_t)v;
            if (n == 6U && !have_bssid) {
                memcpy(out->bssid, rec + off, 6U); have_bssid = true;
            } else if (f == 2U && !have_ssid && n > 0U) {
                size_t slen = n;
                while (slen > 0U && rec[off + slen - 1U] == 0U) slen--;
                if (slen > 32U) slen = 32U;
                if (slen > 0U) {
                    memcpy(out->ssid, rec + off, slen);
                    out->ssid[slen] = '\0';
                    have_ssid = true;
                }
            }
            off += n;
        } else break;
    }
    if (!have_ssid) strlcpy(out->ssid, "?", sizeof(out->ssid));
}

static bool sc_signal_read(int32_t *rssi, uint32_t *channel) {
    static uint8_t resp[300];
    size_t resp_len = 0U, body_len = 0U, off = 0U;
    const uint8_t *body;
    uint64_t key, v;
    if (!esp_hosted_sdio_is_attached()) return false;
    if (esp_hosted_ctrl_raw_request(294U, NULL, 0U, resp, sizeof(resp), &resp_len, 1000U) != ESP_HOSTED_CTRL_OK)
        return false;
    body = sc_rpc_payload(resp, resp_len, 550U, &body_len);
    if (!body) return false;
    const uint8_t *rec = NULL; size_t rec_len = 0U;
    off = 0U;
    while (off < body_len) {
        if (!sc_read_varint(body, body_len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt == 2U) {
            if (!sc_read_varint(body, body_len, &off, &v) || v > (uint64_t)(body_len - off)) break;
            if (f == 2U) { rec = body + off; rec_len = (size_t)v; }
            off += (size_t)v;
        } else if (wt == 0U) {
            if (!sc_read_varint(body, body_len, &off, &v)) break;
        } else if (wt == 5U) { off += 4U; }
        else if (wt == 1U) { off += 8U; }
        else break;
    }
    if (!rec) return false;
    bool have_rssi = false;
    off = 0U;
    while (off < rec_len) {
        if (!sc_read_varint(rec, rec_len, &off, &key)) break;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt != 0U) {
            if (wt == 2U) {
                if (!sc_read_varint(rec, rec_len, &off, &v) || v > (uint64_t)(rec_len-off)) break;
                off += (size_t)v; continue;
            } else break;
        }
        if (!sc_read_varint(rec, rec_len, &off, &v)) break;
        if (f == 3U) *channel = (uint32_t)v;
        else if (f == 5U) { *rssi = (int32_t)v; have_rssi = true; }
    }
    return have_rssi;
}

static const uint8_t *sc_tlv_data(const uint8_t *buf, size_t len, size_t *out_len) {
    size_t off = 0U;
    while (off + 3U <= len) {
        uint8_t type = buf[off];
        size_t blen = (size_t)buf[off + 1U] | ((size_t)buf[off + 2U] << 8U);
        off += 3U;
        if (off + blen > len) return NULL;
        if (type == 3U) { *out_len = blen; return &buf[off]; }
        off += blen;
    }
    return NULL;
}

static bool sc_rpc_header(const uint8_t *pb, size_t len, uint32_t *type, uint32_t *msg_id) {
    size_t off = 0U;
    uint64_t key, v;
    while (off < len) {
        if (!sc_read_varint(pb, len, &off, &key)) return false;
        uint32_t f = (uint32_t)(key >> 3U);
        uint8_t wt = (uint8_t)(key & 7U);
        if (wt != 0U) return false;
        if (!sc_read_varint(pb, len, &off, &v)) return false;
        if (f == 1U) *type = (uint32_t)v;
        else if (f == 2U) *msg_id = (uint32_t)v;
        else if (f == 3U) break;
    }
    return true;
}

int wifi_scan(wifi_scan_result_t *out, int max, int *out_count) {
    static uint8_t resp[1700];
    size_t resp_len = 0U;
    uint8_t arg[8];
    size_t arg_len;
    const uint8_t *body;
    size_t body_len;
    int32_t rssi = 0;
    uint32_t channel = 0U;
    int found = 0;

    if (out_count) *out_count = 0;
    if (!out || max <= 0) return -1;

    if (!esp_hosted_sdio_is_attached()) return -1;
    esp_hosted_ctrl_init();

    bool linked = esp_hosted_wifi_sta_is_connected() || sc_signal_read(&rssi, &channel);
    if (!linked) {
        esp_hosted_wifi_init_default(3000U);
        esp_hosted_wifi_set_mode(ESP_HOSTED_WIFI_MODE_STA, 3000U);
        esp_hosted_wifi_start(3000U);
    }

    if (esp_hosted_ctrl_raw_request(286U, NULL, 0U, resp, sizeof(resp), &resp_len, 3000U) != ESP_HOSTED_CTRL_OK) {
        return -2;
    }

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
                pb = sc_tlv_data(pkt.payload, pkt.len, &pl);
                if (!pb || !sc_rpc_header(pb, pl, &type, &msg_id)) continue;
                if (type == 3U && msg_id == 774U) { got = 1U; scans++; }
            }
            spin(10000U);
            quiet = got ? 0U : quiet + 10U;
            if (scans != 0U && quiet >= 1500U) break;
        }
    }

    if (esp_hosted_ctrl_raw_request(288U, NULL, 0U, resp, sizeof(resp), &resp_len, 3000U) != ESP_HOSTED_CTRL_OK) {
        return -3;
    }
    body = sc_rpc_payload(resp, resp_len, 544U, &body_len);
    if (!body) return -3;
    uint32_t count = sc_varint(body, body_len, 2U);

    uint32_t idx = 0U;
    uint32_t batch = 6U;
    while (idx < count) {
        uint32_t ask = (count - idx < batch) ? (count - idx) : batch;
        arg_len = 0U;
        arg[arg_len++] = 0x08U;
        uint64_t av = ask;
        do { arg[arg_len++] = (uint8_t)((av & 0x7FU) | (av > 0x7FU ? 0x80U : 0U)); av >>= 7U; } while (av);
        if (esp_hosted_ctrl_raw_request(289U, arg, arg_len, resp, sizeof(resp), &resp_len, 3000U) != ESP_HOSTED_CTRL_OK) {
            break;
        }
        body = sc_rpc_payload(resp, resp_len, 545U, &body_len);
        if (!body) break;
        uint32_t before = idx;
        size_t off = 0U;
        while (off < body_len) {
            uint64_t key, v;
            if (!sc_read_varint(body, body_len, &off, &key)) break;
            uint32_t f = (uint32_t)(key >> 3U);
            uint8_t wt = (uint8_t)(key & 7U);
            if (wt == 0U) { if (!sc_read_varint(body, body_len, &off, &v)) break; continue; }
            if (wt != 2U) break;
            if (!sc_read_varint(body, body_len, &off, &v) || v > (uint64_t)(body_len - off)) break;
            if (f == 3U) {
                if (found < max) sc_parse_record(body + off, (size_t)v, &out[found]);
                found++;
                idx++;
            }
            off += (size_t)v;
        }
        if (idx == before) {
            if (batch > 1U) { batch /= 2U; continue; }
            break;
        }
    }
    if (out_count) *out_count = found;
    return 0;
}
