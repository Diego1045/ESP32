#pragma once

#include "rc522_types.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(RC522_EVENTS);

esp_err_t rc522_create(const rc522_config_t *config, rc522_handle_t *out_rc522);

esp_err_t rc522_register_events(
    const rc522_handle_t rc522, rc522_event_t event, esp_event_handler_t event_handler, void *event_handler_arg);

esp_err_t rc522_unregister_events(const rc522_handle_t rc522, rc522_event_t event, esp_event_handler_t event_handler);

esp_err_t rc522_start(rc522_handle_t rc522);

esp_err_t rc522_pause(rc522_handle_t rc522);

esp_err_t rc522_destroy(rc522_handle_t rc522);

typedef struct
{
    uint32_t poll_total;
    uint32_t poll_ok;
    uint32_t poll_collision;
    uint32_t poll_timeout;
    uint32_t poll_parity;
    uint32_t poll_invalid_atqa;
    uint32_t poll_other;
    uint32_t select_ok;
    uint32_t select_fail;
    uint32_t heartbeat_fail;
    uint16_t last_poll_err;
    uint16_t last_select_err;
} rc522_diag_t;

void rc522_diag_get(rc522_diag_t *out);

void rc522_diag_reset(void);

#ifdef __cplusplus
}
#endif
