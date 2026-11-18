#include <dev/wifi/sdio.h>
#include <lk/console_cmd.h>
#include <stdio.h>

static int cmd_wifi_probe(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
    struct sdio_info info;
    status_t st = sdio_enumerate(&info);
    sdio_probe_report(&info);
    if (st < 0) printf("wifi probe: error %d\n", (int)st);
    return (int)st;
}

STATIC_COMMAND_START
STATIC_COMMAND("wifi", "WiFi/SDIO probe (ESP32-C6)", &cmd_wifi_probe)
STATIC_COMMAND_END(wifi_probe);
