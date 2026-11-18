/*
 * Acceso a la configuracion de red/GUI cargada desde /sd/kernel.json.
 */
#pragma once

#include <stdbool.h>

bool netcfg_gui_autostart(void);
const char *netcfg_apps_dir(void);
const char *netcfg_launcher_path(void);
