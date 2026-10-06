#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include "esp_err.h"

#define MASTER_KEY_SIZE 32
#define NONCE_SIZE 16

typedef struct {
    uint8_t master_key[MASTER_KEY_SIZE];
    uint8_t current_nonce[NONCE_SIZE];
} lock_settings_t;

/**
 * @brief Initialize NVS storage
 */
esp_err_t storage_init(void);

/**
 * @brief Load settings from NVS. If not found, initialize with defaults.
 */
esp_err_t storage_load_settings(lock_settings_t *settings);

/**
 * @brief Save settings to NVS.
 */
esp_err_t storage_save_settings(const lock_settings_t *settings);

/**
 * @brief Update only the nonce in NVS.
 */
esp_err_t storage_update_nonce(const uint8_t *new_nonce);

#endif // STORAGE_H
