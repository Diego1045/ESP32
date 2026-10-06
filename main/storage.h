#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MASTER_KEY_SIZE 32
#define NONCE_SIZE 16
#define REVOKED_UID_MAX 10
#define REVOKED_LIST_MAX 16

typedef struct {
    uint8_t master_key[MASTER_KEY_SIZE];
    uint8_t current_nonce[NONCE_SIZE];
} lock_settings_t;

esp_err_t storage_init(void);

esp_err_t storage_load_settings(lock_settings_t *settings);

esp_err_t storage_save_settings(const lock_settings_t *settings);

esp_err_t storage_update_nonce(const uint8_t *new_nonce);

/** Crea master key y nonce aleatorios si no existen en NVS. */
esp_err_t storage_ensure_secrets(void);

bool storage_get_format_mode(void);

esp_err_t storage_set_format_mode(bool enabled);

bool storage_is_uid_revoked(const uint8_t *uid, size_t uid_len);

esp_err_t storage_revoke_uid(const uint8_t *uid, size_t uid_len);

#endif
