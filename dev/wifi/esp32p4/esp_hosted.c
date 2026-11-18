/* ESP-Hosted core (RX queue/dispatch/tx), adaptado de Xinu a LK. */
#include <dev/wifi/esp_hosted.h>
#include <string.h>
#include <stdlib.h>
#include <arch/ops.h>
#include <kernel/spinlock.h>
#include <platform/psram_heap.h>

typedef struct {
    esp_hosted_packet_t items[ESP_HOSTED_RX_QUEUE_DEPTH];
    uint8_t head, tail, count;
    size_t dropped;
} esp_hosted_rx_queue_t;

typedef struct { esp_hosted_rx_cb_t cb; void *ctx; } esp_hosted_cb_slot_t;

static esp_hosted_transport_t s_transport;
static esp_hosted_rx_queue_t *s_rx_queue;
static uint16_t s_tx_seq_num;
static bool s_initialized;
static spin_lock_t s_rx_lock = SPIN_LOCK_INITIAL_VALUE;

static esp_hosted_cb_slot_t s_lwip_cb, s_ctrl_cb, s_ble_evt_cb, s_ble_acl_cb, s_priv_cb;

static bool esp_hosted_ensure_queue(void) {
    if (s_rx_queue) return true;
    s_rx_queue = (esp_hosted_rx_queue_t *)psram_calloc(1, sizeof(esp_hosted_rx_queue_t));
    if (!s_rx_queue) {
        s_rx_queue = (esp_hosted_rx_queue_t *)calloc(1, sizeof(esp_hosted_rx_queue_t));
    }
    return s_rx_queue != NULL;
}

static void esp_hosted_reset_state(void) {
    memset(&s_transport, 0, sizeof(s_transport));
    if (s_rx_queue) memset(s_rx_queue, 0, sizeof(*s_rx_queue));
    memset(&s_lwip_cb, 0, sizeof(s_lwip_cb));
    memset(&s_ctrl_cb, 0, sizeof(s_ctrl_cb));
    memset(&s_ble_evt_cb, 0, sizeof(s_ble_evt_cb));
    memset(&s_ble_acl_cb, 0, sizeof(s_ble_acl_cb));
    memset(&s_priv_cb, 0, sizeof(s_priv_cb));
    s_tx_seq_num = 0;
    s_initialized = false;
}

static bool esp_hosted_queue_push(const esp_hosted_frame_info_t *info, const void *payload, size_t len) {
    esp_hosted_packet_t *slot;
    if (!s_rx_queue && !esp_hosted_ensure_queue()) return false;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&s_rx_lock);
    if (s_rx_queue->count >= ESP_HOSTED_RX_QUEUE_DEPTH) {
        s_rx_queue->dropped++;
        spin_unlock_irqrestore(&s_rx_lock, state);
        return false;
    }
    slot = &s_rx_queue->items[s_rx_queue->head];
    memset(slot, 0, sizeof(*slot));
    slot->info = *info;
    slot->len = len;
    if (len) memcpy(slot->payload, payload, len);
    s_rx_queue->head = (uint8_t)((s_rx_queue->head + 1U) % ESP_HOSTED_RX_QUEUE_DEPTH);
    s_rx_queue->count++;
    spin_unlock_irqrestore(&s_rx_lock, state);
    return true;
}

static bool esp_hosted_queue_pop(esp_hosted_packet_t *out_packet) {
    if (!out_packet || !s_rx_queue) return false;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&s_rx_lock);
    if (s_rx_queue->count == 0U) { spin_unlock_irqrestore(&s_rx_lock, state); return false; }
    *out_packet = s_rx_queue->items[s_rx_queue->tail];
    s_rx_queue->tail = (uint8_t)((s_rx_queue->tail + 1U) % ESP_HOSTED_RX_QUEUE_DEPTH);
    s_rx_queue->count--;
    spin_unlock_irqrestore(&s_rx_lock, state);
    return true;
}

static void esp_hosted_dispatch_one(const esp_hosted_packet_t *packet) {
    esp_hosted_cb_slot_t *slot = NULL;
    const uint8_t *payload = packet->payload;
    size_t len = packet->len;
    esp_hosted_frame_info_t info = packet->info;
    switch (packet->info.if_type) {
        case ESP_HOSTED_IF_STA: case ESP_HOSTED_IF_AP: case ESP_HOSTED_IF_ETH: slot = &s_lwip_cb; break;
        case ESP_HOSTED_IF_SERIAL: slot = &s_ctrl_cb; break;
        case ESP_HOSTED_IF_PRIV: slot = &s_priv_cb; break;
        case ESP_HOSTED_IF_HCI:
            if (info.hci_pkt_type == ESP_HOSTED_HCI_PKT_EVT) slot = &s_ble_evt_cb;
            else if (info.hci_pkt_type == ESP_HOSTED_HCI_PKT_ACL) slot = &s_ble_acl_cb;
            else if (len > 1U) {
                if (payload[0] == ESP_HOSTED_HCI_PKT_EVT) { info.hci_pkt_type = ESP_HOSTED_HCI_PKT_EVT; slot = &s_ble_evt_cb; payload++; len--; }
                else if (payload[0] == ESP_HOSTED_HCI_PKT_ACL) { info.hci_pkt_type = ESP_HOSTED_HCI_PKT_ACL; slot = &s_ble_acl_cb; payload++; len--; }
            }
            break;
        default: break;
    }
    if (slot && slot->cb) slot->cb(&info, payload, len, slot->ctx);
}

static size_t esp_hosted_tx_common(esp_hosted_if_t if_type, uint8_t if_num, uint8_t hci_pkt_type,
                                   uint8_t priv_pkt_type, const void *payload, size_t len) {
    esp_hosted_frame_info_t info = {0};
    if (!s_initialized || !s_transport.tx || (payload == NULL && len != 0U) || len > UINT16_MAX) return 0U;
    info.if_type = if_type; info.if_num = if_num; info.payload_len = (uint16_t)len;
    info.seq_num = ++s_tx_seq_num; info.hci_pkt_type = hci_pkt_type; info.priv_pkt_type = priv_pkt_type;
    return s_transport.tx(&info, payload, len, s_transport.ctx);
}

bool esp_hosted_init(const esp_hosted_transport_t *transport) {
    esp_hosted_reset_state(); s_initialized = true; return esp_hosted_set_transport(transport);
}
void esp_hosted_deinit(void) { esp_hosted_reset_state(); }
bool esp_hosted_is_initialized(void) { return s_initialized; }
bool esp_hosted_set_transport(const esp_hosted_transport_t *transport) {
    if (!s_initialized) return false;
    if (!transport) { memset(&s_transport, 0, sizeof(s_transport)); return true; }
    s_transport = *transport; return true;
}
void esp_hosted_poll(void) { if (s_initialized && s_transport.poll) s_transport.poll(s_transport.ctx); }
bool esp_hosted_rx_submit(const esp_hosted_frame_info_t *info, const void *payload, size_t len) {
    if (!s_initialized || !info || len > ESP_HOSTED_RX_PAYLOAD_MAX || (!payload && len)) return false;
    return esp_hosted_queue_push(info, payload, len);
}
bool esp_hosted_rx_pop(esp_hosted_packet_t *out_packet) { return esp_hosted_queue_pop(out_packet); }
size_t esp_hosted_rx_pending(void) { return s_rx_queue ? s_rx_queue->count : 0; }
size_t esp_hosted_rx_dropped(void) { return s_rx_queue ? s_rx_queue->dropped : 0; }
void esp_hosted_lwip_set_input_cb(esp_hosted_rx_cb_t cb, void *ctx) { s_lwip_cb.cb = cb; s_lwip_cb.ctx = ctx; }
void esp_hosted_ctrl_set_rx_cb(esp_hosted_rx_cb_t cb, void *ctx) { s_ctrl_cb.cb = cb; s_ctrl_cb.ctx = ctx; }
void esp_hosted_ble_set_evt_cb(esp_hosted_rx_cb_t cb, void *ctx) { s_ble_evt_cb.cb = cb; s_ble_evt_cb.ctx = ctx; }
void esp_hosted_ble_set_acl_cb(esp_hosted_rx_cb_t cb, void *ctx) { s_ble_acl_cb.cb = cb; s_ble_acl_cb.ctx = ctx; }
void esp_hosted_priv_set_rx_cb(esp_hosted_rx_cb_t cb, void *ctx) { s_priv_cb.cb = cb; s_priv_cb.ctx = ctx; }
void esp_hosted_dispatch_rx(void) {
    esp_hosted_packet_t packet;
    if (!s_initialized) return;
    while (esp_hosted_queue_pop(&packet)) esp_hosted_dispatch_one(&packet);
}
size_t esp_hosted_lwip_tx(const void *frame, size_t len) { return esp_hosted_lwip_tx_to_if(ESP_HOSTED_IF_STA, 0U, frame, len); }
size_t esp_hosted_lwip_tx_to_if(esp_hosted_if_t if_type, uint8_t if_num, const void *frame, size_t len) {
    if (if_type != ESP_HOSTED_IF_STA && if_type != ESP_HOSTED_IF_AP && if_type != ESP_HOSTED_IF_ETH) return 0U;
    return esp_hosted_tx_common(if_type, if_num, ESP_HOSTED_HCI_PKT_NONE, 0U, frame, len);
}
size_t esp_hosted_ctrl_tx(const void *buf, size_t len) { return esp_hosted_tx_common(ESP_HOSTED_IF_SERIAL, 0U, ESP_HOSTED_HCI_PKT_NONE, 0U, buf, len); }
size_t esp_hosted_ble_send_hci_cmd(const void *buf, size_t len) { return esp_hosted_tx_common(ESP_HOSTED_IF_HCI, 0U, ESP_HOSTED_HCI_PKT_CMD, 0U, buf, len); }
size_t esp_hosted_ble_send_acl(const void *buf, size_t len) { return esp_hosted_tx_common(ESP_HOSTED_IF_HCI, 0U, ESP_HOSTED_HCI_PKT_ACL, 0U, buf, len); }
size_t esp_hosted_priv_tx(uint8_t priv_pkt_type, const void *buf, size_t len) { return esp_hosted_tx_common(ESP_HOSTED_IF_PRIV, 0U, ESP_HOSTED_HCI_PKT_NONE, priv_pkt_type, buf, len); }
