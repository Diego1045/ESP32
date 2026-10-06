#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "STORAGE";
static const char *NVS_NAMESPACE = "lock_data";
static const char *KEY_SETTINGS = "settings";

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
            ESP_LOGI(TAG, "Settings not found, using defaults");
            // Default Master Key (Should be changed in production!)
            memset(settings->master_key, 0xAA, MASTER_KEY_SIZE);
            memset(settings->current_nonce, 0x00, NONCE_SIZE);
            return ESP_OK;
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
    if (err != ESP_OK) return err;

    err = nvs_set_blob(my_handle, KEY_SETTINGS, settings, sizeof(lock_settings_t));
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }

    nvs_close(my_handle);
    return err;
}

esp_err_t storage_update_nonce(const uint8_t *new_nonce) {
    lock_settings_t settings;
    esp_err_t err = storage_load_settings(&settings);
    if (err != ESP_OK) return err;

    memcpy(settings.current_nonce, new_nonce, NONCE_SIZE);
    return storage_save_settings(&settings);
}
