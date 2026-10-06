#ifndef CONSOLE_CLI_H
#define CONSOLE_CLI_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "rc522.h"

/** Tarea que lee comandos por UART (monitor serie). */
void console_cli_start(TimerHandle_t format_mode_auto_off_timer, rc522_handle_t scanner,
                       SemaphoreHandle_t rc522_mutex);

/** Mensaje visible en el monitor serie (prefijo [RFID], salto de linea). */
void console_notify(const char *fmt, ...);

#endif
