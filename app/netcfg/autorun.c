/*
 * Inicializa la command line del kernel con un script lk.autorun por defecto.
 * El shell lo ejecuta antes de mostrar el prompt (app/shell/shell.c), así que
 * aqui arrancan los servicios de red sin intervencion manual.
 *
 * El script usa '+' = espacio y ';' para separar comandos (ver decode_pluses en
 * shell.c).  Orden: cargar kernel.json, subir el C6, conectar WiFi guardada y
 * arrancar httpd.  `netcfg` y `wifi join` son idempotentes.
 *
 * Si /sd/kernel.json trae otra configuracion se honra en netcfg; el script
 * sigue siendo el punto de entrada unico.  Para cambiar el comportamiento,
 * editar este buffer o sobreescribir lk.autorun desde otro modulo antes del
 * shell (no aplica aqui porque somos el unico productor).
 */
#include <lk/init.h>
#include <lib/cmdline.h>
#include <stdio.h>
#include <string.h>

/* Orden (copia de netBootTask de Xinu):
 *   - netcfg wifi: wifi_join hace attach_and_up Y registra wlan0; el
 *     netif_register arranca DHCP una sola vez (NO meter wifi up antes:
 *     doble attach => AP reason=39, medido en HW).
 *   - netcfg wait: ESPERA sincronica a DHCP (como dhcpClient de Xinu), asi
 *     los servicios arrancan con IP valida, no con 0.0.0.0.
 *   - luego httpd, telnetd, tftpd, udpserver.
 *   - gui autostart: lanza la interfaz grafica/launcher si esta habilitada
 *     en kernel.json (core 1 para fluidez, sin romper la red en core 0). */
/* PRUEBA A/B: 'gui+autostart' comentado para verificar que la caida de red
 * (rx_dropped, httpd/telnet/UDP muertos) viene de la GUI.  Reactivar
 * descomentando ';gui+autostart'.  Nada se borra: los ELFs siguen en /sd. */
static char autorun_buf[512] =
    "lk.autorun=netcfg+reload;netcfg+wifi;netcfg+wait;httpd;telnetd;tftpd;udpserver";

static void autorun_init(uint level) {
    (void)level;
    status_t err = cmdline_init(autorun_buf, strlen(autorun_buf));
    if (err < 0) {
        printf("autorun: cmdline_init fallo %d\n", err);
    } else {
        printf("autorun: '%s'\n", autorun_buf);
    }
}

/* cmdline debe estar listo antes de que arranque el shell (APPS).  PLATFORM
 * es suficiente (el init hook del shell corre en APPS). */
LK_INIT_HOOK(autorun, autorun_init, LK_INIT_LEVEL_PLATFORM);
