#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_HOSTED_CTRL_FEATURE_NONE = 0,
    ESP_HOSTED_CTRL_FEATURE_BLUETOOTH = 1,
} esp_hosted_ctrl_feature_t;

typedef enum {
    ESP_HOSTED_CTRL_FEATURE_COMMAND_NONE = 0,
    ESP_HOSTED_CTRL_FEATURE_COMMAND_BT_INIT = 1,
    ESP_HOSTED_CTRL_FEATURE_COMMAND_BT_DEINIT = 2,
    ESP_HOSTED_CTRL_FEATURE_COMMAND_BT_ENABLE = 3,
    ESP_HOSTED_CTRL_FEATURE_COMMAND_BT_DISABLE = 4,
} esp_hosted_ctrl_feature_command_t;

typedef enum {
    ESP_HOSTED_CTRL_FEATURE_OPTION_NONE = 0,
    ESP_HOSTED_CTRL_FEATURE_OPTION_BT_DEINIT_RELEASE_MEMORY = 1,
} esp_hosted_ctrl_feature_option_t;

typedef enum {
    ESP_HOSTED_WIFI_MODE_NULL = 0,
    ESP_HOSTED_WIFI_MODE_STA = 1,
    ESP_HOSTED_WIFI_MODE_AP = 2,
    ESP_HOSTED_WIFI_MODE_APSTA = 3,
} esp_hosted_wifi_mode_t;

typedef enum {
    ESP_HOSTED_WIFI_IF_STA = 0,
    ESP_HOSTED_WIFI_IF_AP = 1,
} esp_hosted_wifi_interface_t;

typedef enum {
    ESP_HOSTED_WIFI_EVENT_STA_CONNECTED = 1,
    ESP_HOSTED_WIFI_EVENT_STA_DISCONNECTED = 2,
} esp_hosted_wifi_event_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint32_t channel;
    int32_t authmode;
    uint32_t aid;
} esp_hosted_wifi_sta_connected_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint32_t reason;
    int32_t rssi;
} esp_hosted_wifi_sta_disconnected_t;

typedef void (*esp_hosted_wifi_event_cb_t)(esp_hosted_wifi_event_t event,
                                           const void *event_data,
                                           void *ctx);

enum {
    ESP_HOSTED_CTRL_OK = 0,
    ESP_HOSTED_CTRL_ERR_STATE = -1000,
    ESP_HOSTED_CTRL_ERR_PROTO = -1001,
    ESP_HOSTED_CTRL_ERR_TX = -1002,
    ESP_HOSTED_CTRL_ERR_TIMEOUT = -1003,
    ESP_HOSTED_CTRL_ERR_MISMATCH = -1004,
    ESP_HOSTED_CTRL_ERR_BUSY = -1005,
};

extern const char *esp_hosted_ctrl_ep_name; /* endpoint H2S: "RPCReq" o "RPCRsp" */

bool esp_hosted_ctrl_init(void);
void esp_hosted_ctrl_deinit(void);
bool esp_hosted_ctrl_is_ready(void);
int esp_hosted_ctrl_raw_request(uint32_t req_id, const uint8_t *args, size_t args_len,
                                uint8_t *resp, size_t resp_cap, size_t *resp_len, uint32_t timeout_ms);
int esp_hosted_ctrl_feature_control(esp_hosted_ctrl_feature_t feature,
                                    esp_hosted_ctrl_feature_command_t command,
                                    esp_hosted_ctrl_feature_option_t option,
                                    uint32_t timeout_ms);
int esp_hosted_ctrl_get_coprocessor_fw_version(uint32_t timeout_ms,
                                               uint32_t *major, uint32_t *minor, uint32_t *patch);
int esp_hosted_ctrl_bt_init(uint32_t timeout_ms);
int esp_hosted_ctrl_bt_enable(uint32_t timeout_ms);
void esp_hosted_wifi_set_event_cb(esp_hosted_wifi_event_cb_t cb, void *ctx);
bool esp_hosted_wifi_sta_is_connected(void);
int esp_hosted_wifi_init_default(uint32_t timeout_ms);
int esp_hosted_wifi_set_mode(esp_hosted_wifi_mode_t mode, uint32_t timeout_ms);
int esp_hosted_wifi_set_sta_config(const char *ssid, const char *password, uint32_t timeout_ms);
int esp_hosted_wifi_start(uint32_t timeout_ms);
int esp_hosted_wifi_connect(uint32_t timeout_ms);
int esp_hosted_wifi_disconnect(uint32_t timeout_ms);
int esp_hosted_wifi_get_mac(esp_hosted_wifi_interface_t iface, uint8_t mac[6], uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
