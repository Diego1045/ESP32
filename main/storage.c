#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "STORAGE";
static const char *NVS_NAMESPACE = "lock_data";
static const char *KEY_SETTINGS = "settings";
static const char *KEY_FORMAT = "format_mode";
static const char *KEY_REVOKED = "revoked";

typedef struct {
    uint8_t count;
    uint8_t lengths[REVOKED_LIST_MAX];
    uint8_t uids[REVOKED_LIST_MAX][REVOKED_UID_MAX];
} revoked_store_t;

static bool uid_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

esp_err_t storage_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t storage_load_settings(lock_settings_t *settings) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            memset(settings->master_key, 0, MASTER_KEY_SIZE);
            memset(settings->current_nonce, 0, NONCE_SIZE);
            return ESP_ERR_NVS_NOT_FOUND;
        }
        return err;
    }

    size_t required_size = sizeof(lock_settings_t);
    err = nvs_get_blob(my_handle, KEY_SETTINGS, settings, &required_size);
    nvs_close(my_handle);
    return err;
}

esp_err_t storage_save_settings(const lock_settings_t *settings) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(my_handle, KEY_SETTINGS, settings, sizeof(lock_settings_t));
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }

    nvs_close(my_handle);
    return err;
}

esp_err_t storage_ensure_secrets(void) {
    lock_settings_t settings;
    esp_err_t err = storage_load_settings(&settings);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    ESP_LOGI(TAG, "Primera ejecucion: generando master key y nonce aleatorios");
    esp_fill_random(settings.master_key, MASTER_KEY_SIZE);
    esp_fill_random(settings.current_nonce, NONCE_SIZE);
    return storage_save_settings(&settings);
}

esp_err_t storage_update_nonce(const uint8_t *new_nonce) {
    lock_settings_t settings;
    esp_err_t err = storage_load_settings(&settings);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(settings.current_nonce, new_nonce, NONCE_SIZE);
    return storage_save_settings(&settings);
}

bool storage_get_format_mode(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, KEY_FORMAT, &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

esp_err_t storage_set_format_mode(bool enabled) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_FORMAT, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Modo formateo NVS: %s", enabled ? "ON" : "OFF");
    }
    return err;
}

static esp_err_t load_revoked(revoked_store_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(*out);
    err = nvs_get_blob(h, KEY_REVOKED, out, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK && out->count > REVOKED_LIST_MAX) {
        memset(out, 0, sizeof(*out));
    }
    return err == ESP_OK ? ESP_OK : err;
}

static esp_err_t save_revoked(const revoked_store_t *store) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_REVOKED, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool storage_is_uid_revoked(const uint8_t *uid, size_t uid_len) {
    if (uid == NULL || uid_len == 0 || uid_len > REVOKED_UID_MAX) {
        return false;
    }
    revoked_store_t store;
    if (load_revoked(&store) != ESP_OK) {
        return false;
    }
    for (uint8_t i = 0; i < store.count; i++) {
        if (uid_equal(uid, uid_len, store.uids[i], store.lengths[i])) {
            return true;
        }
    }
    return false;
}

esp_err_t storage_revoke_uid(const uint8_t *uid, size_t uid_len) {
    if (uid == NULL || uid_len == 0 || uid_len > REVOKED_UID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    revoked_store_t store;
    load_revoked(&store);

    for (uint8_t i = 0; i < store.count; i++) {
        if (uid_equal(uid, uid_len, store.uids[i], store.lengths[i])) {
            return ESP_OK;
        }
    }
    if (store.count >= REVOKED_LIST_MAX) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(store.uids[store.count], uid, uid_len);
    store.lengths[store.count] = (uint8_t)uid_len;
    store.count++;
    return save_revoked(&store);
}
