#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include "storage.h"

#define HMAC_SIZE 32

// Configuraciones para Autenticación de Sectores (MIFARE)
#define SECTOR_SECRET_KEY { 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F } // Contraseña segura (Key A) de 6 bytes
#define SECURE_SECTOR_INDEX 1
#define SECURE_SECTOR_BLOCK 4 // El bloque 4 es el primer bloque de datos del Sector 1
#define SECURE_SECTOR_TRAILER 7 // El bloque 7 guarda las contraseñas del Sector 1

/**
 * @brief Calculate HMAC-SHA256(UID + Nonce) using MasterKey
 * 
 * @param uid Card UID (usually 4 or 7 bytes)
 * @param uid_len Length of UID
 * @param nonce Current nonce (16 bytes)
 * @param master_key Master key (32 bytes)
 * @param out_hmac Buffer to store resulting 32-byte HMAC
 * @return 0 on success, negative on error
 */
int security_calculate_hmac(const uint8_t *uid, size_t uid_len, 
                             const uint8_t *nonce, 
                             const uint8_t *master_key, 
                             uint8_t *out_hmac);

/**
 * @brief Verify if the received hash matches the calculated one
 */
bool security_verify_hash(const uint8_t *calculated, const uint8_t *received);

/**
 * @brief Full validation flow:
 * 1. Calculates HMAC(UID + Current Nonce)
 * 2. Compares with received_hash
 * 3. If valid, generates NEW nonce and saves it to NVS
 * 
 * @return 0 if valid and updated, -1 if invalid, other for NVS/Crypto errors
 */
int security_validate_and_update_card(const uint8_t *uid, size_t uid_len,
                                       const uint8_t *received_hash,
                                       const lock_settings_t *settings);

#endif // SECURITY_H
