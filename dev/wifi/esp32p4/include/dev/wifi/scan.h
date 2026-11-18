#pragma once
#include <stdint.h>
#include <stdbool.h>

#define WIFI_SCAN_SSID_MAX 33
#define WIFI_SCAN_MAX_RESULTS 32

typedef struct {
    char ssid[WIFI_SCAN_SSID_MAX];
    uint8_t bssid[6];
    int32_t rssi;
    uint32_t channel;
    uint32_t authmode;
} wifi_scan_result_t;

/* Realiza escaneo y rellena hasta max resultados. Devuelve 0 si ok, <0 en error.
 * *out_count recibe el numero de redes encontradas (puede ser > max, se trunca).
 * Requiere que el stack WiFi este adjunto/inicializado (wifi_attach_and_up). */
int wifi_scan(wifi_scan_result_t *out, int max, int *out_count);
