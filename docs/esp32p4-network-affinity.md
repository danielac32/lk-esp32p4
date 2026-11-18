# Red ESP32-P4: afinidad de cores y servicios

## Regla de oro

**Todo lo que espere en un `event_t` de minip debe correr en core0.**

El stack de minip (rx/tx, DHCP, ARP, timers) corre en core0. Los sockets
(`tcp_socket_t`) tienen `event_t rx_event`/`tx_event` que se señalizan desde el
hilo del stack. Medido en HW: un servidor TCP fijado a core1 hace `accept` con
éxito (ve el SYN) pero `tcp_read()` se queda colgado esperando un evento que no
despierta entre cores. Mover el servidor a core0 lo resuelve al instante.

## Mapa de afinidad

| Componente | Core | Razón |
|---|---|---|
| stack minip (netif, pktbuf, dhcp, arp, dns) | 0 | eventos de sockets se señalan aquí |
| wifi_poll (ESP32-C6 / SDIO) | 0 | `wifi_netif.c:86` |
| httpd | 0 | `tcp_accept`/`tcp_read` |
| telnetd (planeado) | 0 | `tcp_read`/`tcp_write` |
| TFTP / UDP (planeado) | 0 | `udp_listen` callback corre en el stack |
| shell local (UART) | 0 | consola interactiva |
| bootTask / LVGL / apps ELF | 1 | cómputo, sin eventos de red |
| I2C / audio / LCD (locks auditados) | cualquiera | protegidos por mutex propios |

## IP y URL de prueba

No existe `minip_get_ip()`. La IP está en `wifi_netif_get()->ipv4_addr`
(`netif_t *`, `lib/minip/include/lib/minip/netif.h`). Se obtiene por DHCP.

Al arrancar el httpd imprime:

```
============================================================
 httpd LISTO en http://192.168.1.103/  (core0)
 pagina de prueba: http://192.168.1.103/index.html
 archivos: http://192.168.1.103/api/files?path=/
============================================================
```

## Servidor httpd

Comandos: `httpd` (arranca), `httpd stop`, `httpd status`.

Rutas (todas verificadas en HW):

| Método | Ruta | Acción |
|---|---|---|
| GET | `/` | página embebida (o `/sd/index.html`) |
| GET | `/api/files?path=/dir` | listado JSON |
| GET | `/api/download?path=/f` | descarga |
| PUT | `/api/file?path=/f` | sube archivo (body crudo) |
| DELETE | `/api/file?path=/f` | borra archivo |
| POST | `/api/mkdir?path=/d` | crea directorio |
| POST | `/api/rmdir?path=/d` | borra directorio |

Detalles del port (bugs corregidos):
- `/sd` se monta RW (`sd_bio.c`).
- El body se calcula **antes** de `parse_request()` (que modifica el buffer).
- Se crea el archivo con longitud 0; `fs_write_file` hace el truncate.
- `wifi dhcp` comprueba `minip_is_configured()` para no spamear.
- `/` e `/index.html` sirven siempre la pagina embebida (571 B); el
  `index.html` de la SD puede ser enorme (~495 KB) y hacer timeout al
  navegador.  Se baja con `/api/download?path=/index.html`.
- **Bug de checksum TCP corregido** (`lib/minip/tcp.cpp:631`): `ones_sum16()`
  devuelve la suma byte-swapeada en RV32 little-endian.  Comparar
  `cksum_pheader()` con `header->checksum` daba `REJECT: failed checksum`
  intermitente.  Ahora se comprueba que la suma directa de
  (pseudoheader + segmento) sea `0xffff`, simetrico e independiente del
  endianness (igual que el check de IP en `minip.cpp`).

## Arranque automatico (lk.autorun)

`app/netcfg/autorun.c` inicializa la command line con:

```
lk.autorun=netcfg+reload;netcfg+wifi;httpd
```

El shell lo ejecuta al arrancar: carga `/sd/kernel.json`, conecta la primera
red WiFi guardada (con reintentos), **espera a DHCP de forma sincronica**
(`netcfg wait`) y arranca httpd, telnetd y tftpd.  Es el mismo orden que
`netBootTask`/`netup` de Xinu.

**Causa raíz de inestabilidad anterior (corregida):** `netif_register()` ya
arranca DHCP una vez por interfaz.  Anadir un supervisor que re-lanzaba DHCP,
mas `wifi dhcp` llamando `minip_start_dhcp`, creaba multiples threads DHCP
compitiendo y dejaban la IP en 0.0.0.0.  NO llamar `minip_start_dhcp` desde
ningun comando: solo esperar con `minip_wait_for_configured()` (20 s, como el
`dhcpClient(dev,20,...)` de Xinu).  Tampoco incluir `wifi up` en el autorun:
`wifi_join()` ya hace `attach_and_up`, y un doble attach hace que el AP
responda `reason=39`.

Verificado en HW en un solo arranque (64 s de muestreo): HTTP 200 (571 B)
sostenido de 24 s a 64 s, ping 3/3, telnet `help` y TFTP RRQ OK, cero
`REJECT: failed checksum`.

## TFTP (RFC 1350)

`app/tftpd` (UDP 69, corre en el hilo del stack = core0).  Comandos:
`tftpd`, `tftpd stop`, `tftpd status`.

Soporta WRQ (subir, se guarda en `/sd`) y RRQ (bajar, desde `/sd`), modo
`octet`, bloques de 512 B.  Un unico `udp_listen(69, tftp_cb, ...)`; cada
transferencia usa un `udp_open` efimero al puerto del cliente y una entrada
en la tabla `xfers[4]`.  Verificado en HW: WRQ + RRQ de un archivo binario
devuelven los bytes identicos.

## Telnet (servidor de shells remotos)

`app/telnetd` (puerto 23, core0). Comandos: `telnetd`, `telnetd stop`,
`telnetd status`.

Modelo de Xinu: `printf_set_putc()` (`lib/libc/printf.c`) pone un sink de
salida en `TLS_ENTRY_PUTCHAR` (nuevo entry en `kernel/thread.h`).  Cada hilo
cliente tiene su propio sink, asi el `printf` de los comandos va a su socket
y no a la UART.  Como `printf` corre bajo `print_spin_lock` con IRQs off, el
sink solo acumula en un buffer; el flush (tcp_write) se hace fuera del lock.
Los comandos se despachan con `console_get_command_handler()` + tokenizer
propio (`console_run_script` no sirve: su command loop compite con el get_line
del socket).  Verificado en HW: `help` por telnet devuelve la lista de
comandos en el socket.

**Nota de pruebas:** abrir `/dev/ttyACM0` con `rts=True` reinicia la placa
(es lo que hace el script de flash).  Para medir ping/curl despues del
arranque NO se debe abrir/cerrar el puerto serie; usar el toggle solo una vez
al flashear y despues dejar la placa corriendo.
