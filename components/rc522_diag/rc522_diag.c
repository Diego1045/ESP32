#include "rc522_diag.h"
#include <string.h>

#define RC522_ERR_BASE                     (0xF522)
#define RC522_ERR_COLLISION                (RC522_ERR_BASE + 1)
#define RC522_ERR_COLLISION_UNSOLVABLE     (RC522_ERR_BASE + 2)
#define RC522_ERR_INVALID_ATQA             (RC522_ERR_BASE + 4)
#define RC522_ERR_RX_TIMEOUT               (RC522_ERR_BASE + 5)
#define RC522_ERR_RX_TIMER_TIMEOUT         (RC522_ERR_BASE + 6)
#define RC522_ERR_PCD_PARITY_CHECK_FAILED  (RC522_ERR_BASE + 10)

static rc522_diag_t s_diag;

void rc522_diag_reset(void) {
    memset(&s_diag, 0, sizeof(s_diag));
}

void rc522_diag_get(rc522_diag_t *out) {
    if (out != NULL) {
        *out = s_diag;
    }
}

void rc522_diag_on_poll_result(esp_err_t err) {
    s_diag.poll_total++;
    if (err == ESP_OK) {
        s_diag.poll_ok++;
        return;
    }
    s_diag.last_poll_err = (uint16_t)err;
    if (err == RC522_ERR_RX_TIMEOUT || err == RC522_ERR_RX_TIMER_TIMEOUT) {
        s_diag.poll_timeout++;
    } else if (err == RC522_ERR_COLLISION || err == RC522_ERR_COLLISION_UNSOLVABLE) {
        s_diag.poll_collision++;
    } else if (err == RC522_ERR_PCD_PARITY_CHECK_FAILED) {
        s_diag.poll_parity++;
    } else if (err == RC522_ERR_INVALID_ATQA) {
        s_diag.poll_invalid_atqa++;
    } else {
        s_diag.poll_other++;
    }
}

void rc522_diag_on_select_result(esp_err_t err) {
    if (err == ESP_OK) {
        s_diag.select_ok++;
    } else {
        s_diag.select_fail++;
        s_diag.last_select_err = (uint16_t)err;
    }
}

void rc522_diag_on_heartbeat_fail(void) {
    s_diag.heartbeat_fail++;
}
