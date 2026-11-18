#pragma once
#include <lk/compiler.h>
#include <sys/types.h>
#include <stdbool.h>
__BEGIN_CDECLS
status_t wifi_netif_start(void);
void wifi_netif_set_mac(const uint8_t mac[6]);
void wifi_netif_input_cb_register(void);
bool wifi_netif_is_ready(void);
void *wifi_netif_get(void);
status_t wifi_attach_and_up(void);
status_t wifi_set_mac_from_c6(void);
status_t wifi_join(const char *ssid, const char *pass);
bool wifi_is_attached(void);
__END_CDECLS

#include <dev/wifi/scan.h>
