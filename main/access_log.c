#include "access_log.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ACCESS_LOG";
static const char *NVS_NAMESPACE = "lock_data";
static const char *KEY_LOG = "access_log";

#define ACCESS_LOG_MAX_ENTRIES 32
#define ACCESS_LOG_UID_MAX 10

typedef struct {
    uint32_t timestamp_ms;
    uint8_t uid_len;
    uint8_t uid[ACCESS_LOG_UID_MAX];
    uint8_t result;
} access_log_entry_t;

typedef struct {
    uint16_t head;
    uint16_t count;
    access_log_entry_t entries[ACCESS_LOG_MAX_ENTRIES];
} access_log_store_t;

static access_log_store_t s_log;

static esp_err_t persist_log(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_LOG, &s_log, sizeof(s_log));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t access_log_init(void) {
    memset(&s_log, 0, sizeof(s_log));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t sz = sizeof(s_log);
        err = nvs_get_blob(h, KEY_LOG, &s_log, &sz);
        nvs_close(h);
        if (err == ESP_OK && s_log.count > ACCESS_LOG_MAX_ENTRIES) {
            memset(&s_log, 0, sizeof(s_log));
        }
    }
    ESP_LOGI(TAG, "Log: %u entradas en NVS", (unsigned)s_log.count);
    return ESP_OK;
}

esp_err_t access_log_append(const uint8_t *uid, size_t uid_len, access_log_result_t result) {
    if (uid == NULL || uid_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uid_len > ACCESS_LOG_UID_MAX) {
        uid_len = ACCESS_LOG_UID_MAX;
    }

    access_log_entry_t *e = &s_log.entries[s_log.head];
    e->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->uid_len = (uint8_t)uid_len;
    memset(e->uid, 0, sizeof(e->uid));
    memcpy(e->uid, uid, uid_len);
    e->result = (uint8_t)result;

    s_log.head = (s_log.head + 1) % ACCESS_LOG_MAX_ENTRIES;
    if (s_log.count < ACCESS_LOG_MAX_ENTRIES) {
        s_log.count++;
    }

    esp_err_t err = persist_log();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo guardar log: %s", esp_err_to_name(err));
    }
    return err;
}

static const char *result_str(uint8_t r) {
    switch ((access_log_result_t)r) {
    case ACCESS_LOG_GRANTED: return "GRANTED";
    case ACCESS_LOG_DENIED_MIFARE: return "DENIED_MIFARE";
    case ACCESS_LOG_DENIED_HMAC: return "DENIED_HMAC";
    case ACCESS_LOG_DENIED_REVOKED: return "DENIED_REVOKED";
    case ACCESS_LOG_DENIED_TYPE: return "DENIED_TYPE";
    case ACCESS_LOG_DENIED_UNPROVISIONED: return "DENIED_UNPROV";
    case ACCESS_LOG_FORMAT_OK: return "FORMAT_OK";
    case ACCESS_LOG_FORMAT_FAIL: return "FORMAT_FAIL";
    default: return "?";
    }
}

void access_log_dump_recent(unsigned count) {
    if (count == 0 || count > ACCESS_LOG_MAX_ENTRIES) {
        count = 5;
    }
    if (s_log.count == 0) {
        ESP_LOGI(TAG, "(sin entradas)");
        return;
    }
    unsigned n = s_log.count < count ? s_log.count : count;
    ESP_LOGI(TAG, "--- Ultimas %u entradas ---", n);
    for (unsigned i = 0; i < n; i++) {
        int idx = (int)s_log.head - 1 - (int)i;
        while (idx < 0) {
            idx += ACCESS_LOG_MAX_ENTRIES;
        }
        const access_log_entry_t *e = &s_log.entries[idx];
        char hex[ACCESS_LOG_UID_MAX * 2 + 1];
        for (uint8_t u = 0; u < e->uid_len; u++) {
            snprintf(hex + u * 2, 3, "%02X", e->uid[u]);
        }
        ESP_LOGI(TAG, "  t=%ums uid=%s %s", (unsigned)e->timestamp_ms, hex, result_str(e->result));
    }
}
