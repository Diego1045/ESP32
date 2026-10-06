#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "storage.h"

#define HMAC_SIZE 32

#define SECTOR_SECRET_KEY { 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F }
#define SECURE_SECTOR_INDEX 1
#define SECURE_SECTOR_BLOCK 4
#define SECURE_SECTOR_TRAILER 7
#define SECURE_HMAC_BLOCK_A 4
#define SECURE_HMAC_BLOCK_B 5

int security_calculate_hmac(const uint8_t *uid, size_t uid_len,
                            const uint8_t *nonce, const uint8_t *master_key,
                            uint8_t *out_hmac);

bool security_hmac_is_unprovisioned(const uint8_t *hmac);

bool security_verify_hmac(const uint8_t *expected, const uint8_t *received);

int security_generate_next_session(const uint8_t *uid, size_t uid_len,
                                   const lock_settings_t *settings,
                                   uint8_t new_nonce[NONCE_SIZE],
                                   uint8_t new_card_hmac[HMAC_SIZE]);

#endif
