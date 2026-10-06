#ifndef ACTUATOR_H
#define ACTUATOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

void actuator_init(void);

/** Llamado tras acceso concedido (relé real o animación en consola). */
void actuator_grant_access(void);

/** Animación de rechazo en el monitor. */
void actuator_show_denied(const char *reason);

/** Comando manual `unlock` en consola. */
void bench_console_unlock(void);

#endif
