#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
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

void rc522_diag_reset(void);
void rc522_diag_get(rc522_diag_t *out);

void rc522_diag_on_poll_result(esp_err_t err);
void rc522_diag_on_select_result(esp_err_t err);
void rc522_diag_on_heartbeat_fail(void);
