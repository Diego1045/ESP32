#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ACCESS_LOG_GRANTED = 0,
    ACCESS_LOG_DENIED_MIFARE = 1,
    ACCESS_LOG_DENIED_HMAC = 2,
    ACCESS_LOG_DENIED_REVOKED = 3,
    ACCESS_LOG_DENIED_TYPE = 4,
    ACCESS_LOG_DENIED_UNPROVISIONED = 5,
    ACCESS_LOG_FORMAT_OK = 6,
    ACCESS_LOG_FORMAT_FAIL = 7,
} access_log_result_t;

esp_err_t access_log_init(void);

esp_err_t access_log_append(const uint8_t *uid, size_t uid_len, access_log_result_t result);

void access_log_dump_recent(unsigned count);

#endif
