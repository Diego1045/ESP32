#include "access.h"
#include "access_log.h"
#include "console_cli.h"
#include "picc/rc522_mifare.h"
#include "rc522_picc_internal.h"
#include "rc522_pcd.h"
#include "security.h"
#include "storage.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ACCESS";

#define ACCESS_QUEUE_LEN 4
#define ACCESS_MUTEX_MS 8000
#define MAX_UID_LEN 10
#define RESELECT_TRIES 3
#define CARD_OP_TRIES 3

typedef enum {
    ACCESS_MSG_PROCESS = 0,
    ACCESS_MSG_HALT_ONLY = 1,
} access_msg_type_t;

typedef struct {
    access_msg_type_t type;
    rc522_picc_t picc;
} access_msg_t;

typedef void (*access_grant_fn_t)(void);

static access_context_t s_ctx;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static access_grant_fn_t s_grant_fn;

static uint8_t s_last_uid[RC522_PICC_UID_SIZE_MAX];
static size_t s_last_uid_len;
static bool s_last_uid_valid;

void access_set_grant_handler(void (*fn)(void)) { s_grant_fn = fn; }

void access_system_init(access_context_t *ctx, rc522_handle_t scanner, SemaphoreHandle_t rc522_mutex) {
    s_ctx.scanner = scanner;
    s_ctx.rc522_mutex = rc522_mutex;
    if (ctx != NULL) {
        *ctx = s_ctx;
    }
    s_queue = xQueueCreate(ACCESS_QUEUE_LEN, sizeof(access_msg_t));
    configASSERT(s_queue != NULL);
    xTaskCreate(access_task, "access_task", 6144, NULL, 5, &s_task);
}

void access_on_picc_idle(void) {
    if (s_last_uid_valid) {
        console_notify("tarjeta retirada — listo para otra lectura");
        ESP_LOGI(TAG, "Tarjeta retirada; lector listo para la siguiente");
    }
    s_last_uid_valid = false;
    s_last_uid_len = 0;
}

void access_forget_last_uid(void) {
    s_last_uid_valid = false;
    s_last_uid_len = 0;
}

bool access_should_ignore_uid(const uint8_t *uid, size_t uid_len) {
    return s_last_uid_valid && s_last_uid_len == uid_len && memcmp(s_last_uid, uid, uid_len) == 0;
}

void access_mark_uid_handled(const uint8_t *uid, size_t uid_len) {
    if (uid_len > RC522_PICC_UID_SIZE_MAX) {
        uid_len = RC522_PICC_UID_SIZE_MAX;
    }
    memcpy(s_last_uid, uid, uid_len);
    s_last_uid_len = uid_len;
    s_last_uid_valid = true;
}

static void enqueue_msg(access_msg_type_t type, const rc522_picc_t *picc) {
    access_msg_t msg = {.type = type};
    memcpy(&msg.picc, picc, sizeof(msg.picc));
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola de acceso llena; evento descartado");
    }
}

void access_submit_card(const rc522_picc_t *picc) { enqueue_msg(ACCESS_MSG_PROCESS, picc); }

void access_submit_halt_only(const rc522_picc_t *picc) { enqueue_msg(ACCESS_MSG_HALT_ONLY, picc); }

static void secret_key_fill(rc522_mifare_key_t *key) {
    static const uint8_t material[] = SECTOR_SECRET_KEY;
    key->type = RC522_MIFARE_KEY_A;
    memcpy(key->value, material, sizeof(material));
}

static bool take_rc522_lock(void) {
    if (s_ctx.rc522_mutex == NULL) {
        return true;
    }
    return xSemaphoreTake(s_ctx.rc522_mutex, pdMS_TO_TICKS(ACCESS_MUTEX_MS)) == pdTRUE;
}

static void give_rc522_lock(void) {
    if (s_ctx.rc522_mutex != NULL) {
        xSemaphoreGive(s_ctx.rc522_mutex);
    }
}

static void end_picc_session(rc522_picc_t *picc) {
    (void)rc522_mifare_deauth(s_ctx.scanner, picc);
    (void)rc522_picc_halta(s_ctx.scanner, picc);
}

/**
 * Tras un fallo de autenticacion MIFARE (o si la tarjeta se desincroniza) la
 * tarjeta vuelve a IDLE/HALT. Hay que despertarla y volver a seleccionarla
 * antes de reintentar cualquier operacion.
 */
static bool wake_and_select_picc(rc522_picc_t *picc) {
    (void)rc522_mifare_deauth(s_ctx.scanner, picc);

    for (int attempt = 0; attempt < RESELECT_TRIES; attempt++) {
        rc522_picc_atqa_desc_t atqa;
        esp_err_t err = rc522_picc_wupa(s_ctx.scanner, &atqa);
        if (err != ESP_OK) {
            err = rc522_picc_reqa(s_ctx.scanner, &atqa);
        }
        if (err == ESP_OK || err == RC522_ERR_COLLISION) {
            rc522_picc_uid_t uid;
            memcpy(&uid, &picc->uid, sizeof(uid));
            uint8_t sak = picc->sak;
            if (rc522_picc_select(s_ctx.scanner, &uid, &sak, true) == ESP_OK) {
                memcpy(&picc->uid, &uid, sizeof(uid));
                picc->sak = sak;
                picc->type = rc522_picc_get_type(picc);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/** Autentica; si falla, re-selecciona la tarjeta y reintenta una vez. */
static esp_err_t auth_with_retry(rc522_picc_t *picc, uint8_t block, const rc522_mifare_key_t *key) {
    esp_err_t err = rc522_mifare_auth(s_ctx.scanner, picc, block, key);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGD(TAG, "auth bloque %u fallo (0x%04X); re-seleccionando", block, (unsigned)err);
    if (!wake_and_select_picc(picc)) {
        return err;
    }
    return rc522_mifare_auth(s_ctx.scanner, picc, block, key);
}

typedef esp_err_t (*card_op_fn_t)(const rc522_picc_t *picc, void *arg);

/**
 * Ejecuta una operacion de bloque con reintentos ante fallos de RF puntuales.
 * Cualquier error (auth, NAK, CRC) deja la tarjeta en IDLE, asi que cada
 * reintento re-selecciona y re-autentica antes de repetir la operacion.
 * Si authenticated es true, el primer intento reutiliza la sesion Crypto-1.
 */
static esp_err_t card_op_with_retry(rc522_picc_t *picc, const rc522_mifare_key_t *key, bool authenticated,
                                    card_op_fn_t op, void *arg, bool *out_auth_ok) {
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < CARD_OP_TRIES; attempt++) {
        if (attempt > 0) {
            authenticated = false;
            if (!wake_and_select_picc(picc)) {
                continue;
            }
        }
        if (!authenticated) {
            err = rc522_mifare_auth(s_ctx.scanner, picc, SECURE_SECTOR_BLOCK, key);
            if (err != ESP_OK) {
                ESP_LOGD(TAG, "auth intento %d fallo (0x%04X)", attempt + 1, (unsigned)err);
                continue;
            }
            if (out_auth_ok != NULL) {
                *out_auth_ok = true;
            }
        }
        err = op(picc, arg);
        if (err == ESP_OK) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "Operacion OK al intento %d (RF marginal: centre la tarjeta)", attempt + 1);
            }
            return ESP_OK;
        }
        ESP_LOGD(TAG, "operacion intento %d fallo (0x%04X)", attempt + 1, (unsigned)err);
    }
    return err;
}

static esp_err_t card_read_hmac(const rc522_picc_t *picc, uint8_t hmac[HMAC_SIZE]) {
    uint8_t block_a[RC522_MIFARE_BLOCK_SIZE];
    uint8_t block_b[RC522_MIFARE_BLOCK_SIZE];
    esp_err_t err = rc522_mifare_read(s_ctx.scanner, picc, SECURE_HMAC_BLOCK_A, block_a);
    if (err != ESP_OK) {
        return err;
    }
    err = rc522_mifare_read(s_ctx.scanner, picc, SECURE_HMAC_BLOCK_B, block_b);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(hmac, block_a, 16);
    memcpy(hmac + 16, block_b, 16);
    return ESP_OK;
}

static esp_err_t card_write_hmac(const rc522_picc_t *picc, const uint8_t hmac[HMAC_SIZE]) {
    esp_err_t err = rc522_mifare_write(s_ctx.scanner, picc, SECURE_HMAC_BLOCK_A, hmac);
    if (err != ESP_OK) {
        return err;
    }
    return rc522_mifare_write(s_ctx.scanner, picc, SECURE_HMAC_BLOCK_B, hmac + 16);
}

static esp_err_t op_read_hmac(const rc522_picc_t *picc, void *arg) { return card_read_hmac(picc, arg); }

static esp_err_t op_write_hmac(const rc522_picc_t *picc, void *arg) { return card_write_hmac(picc, arg); }

static esp_err_t card_write_trailer(const rc522_picc_t *picc) {
    // Key A secreta | access bits estandar (datos: A/B lee/escribe) | Key B por defecto
    uint8_t new_trailer[RC522_MIFARE_BLOCK_SIZE] = {
        0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F,
        0xFF, 0x07, 0x80, 0x69,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    return rc522_mifare_write(s_ctx.scanner, picc, SECURE_SECTOR_TRAILER, new_trailer);
}

static esp_err_t card_provision_hmac(rc522_picc_t *picc, const lock_settings_t *settings,
                                     const rc522_mifare_key_t *key) {
    uint8_t hmac[HMAC_SIZE];
    int ret = security_calculate_hmac(picc->uid.value, picc->uid.length, settings->current_nonce,
                                      settings->master_key, hmac);
    if (ret != 0) {
        return ESP_FAIL;
    }
    return card_op_with_retry(picc, key, true, op_write_hmac, hmac, NULL);
}

static void process_format_mode(rc522_picc_t *picc, const uint8_t *uid, size_t uid_len) {
    rc522_mifare_key_t factory_key = {
        .type = RC522_MIFARE_KEY_A,
        .value = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    };
    rc522_mifare_key_t secret_key;
    secret_key_fill(&secret_key);

    ESP_LOGI(TAG, "Modo formateo: provisionando tarjeta");
    console_notify("formateo: provisionando sector 1...");

    // 1) Tarjeta de fabrica: clave FF..FF sobre el trailer del sector 1.
    if (rc522_mifare_auth(s_ctx.scanner, picc, SECURE_SECTOR_TRAILER, &factory_key) == ESP_OK) {
        ESP_LOGI(TAG, "Clave de fabrica OK; escribiendo trailer seguro");
        if (card_write_trailer(picc) != ESP_OK) {
            console_notify("formateo FALLIDO: no se pudo escribir trailer");
            ESP_LOGE(TAG, "Error escribiendo trailer");
            access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_FAIL);
            return;
        }
        // El trailer nuevo invalida la sesion Crypto-1: autenticar otra vez.
        if (auth_with_retry(picc, SECURE_SECTOR_BLOCK, &secret_key) != ESP_OK) {
            ESP_LOGE(TAG, "Auth bloque 4 tras escribir trailer fallo");
            access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_FAIL);
            return;
        }
    } else {
        // 2) Posiblemente ya formateada: reselect (el fallo la dejo en IDLE) + clave secreta.
        if (!wake_and_select_picc(picc)) {
            ESP_LOGW(TAG, "Formateo: no se pudo re-seleccionar la tarjeta");
            access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_FAIL);
            return;
        }
        if (auth_with_retry(picc, SECURE_SECTOR_BLOCK, &secret_key) != ESP_OK) {
            ESP_LOGW(TAG, "Formateo: ni clave de fabrica ni clave secreta (tarjeta ajena?)");
            access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_FAIL);
            return;
        }
        ESP_LOGI(TAG, "Tarjeta ya formateada; reescribiendo HMAC en bloques 4-5");
    }

    lock_settings_t settings;
    if (storage_load_settings(&settings) != ESP_OK) {
        access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_FAIL);
        return;
    }
    if (card_provision_hmac(picc, &settings, &secret_key) != ESP_OK) {
        ESP_LOGE(TAG, "Error escribiendo HMAC inicial");
        access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_FAIL);
        return;
    }

    ESP_LOGI(TAG, ">> Tarjeta provisionada (sector + HMAC). Escriba 'format off' y vuelva a acercarla.");
    console_notify("OK formateo — escriba 'format off' y vuelva a pasar la tarjeta");
    access_log_append(uid, uid_len, ACCESS_LOG_FORMAT_OK);
}

static void process_production(rc522_picc_t *picc, const uint8_t *uid, size_t uid_len) {
    if (storage_is_uid_revoked(uid, uid_len)) {
        console_notify("DENEGADO: UID revocado");
        ESP_LOGW(TAG, "DENEGADO: UID revocado");
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_REVOKED);
        return;
    }

    rc522_mifare_key_t secret_key;
    secret_key_fill(&secret_key);

    uint8_t card_hmac[HMAC_SIZE];
    bool auth_ok = false;
    if (card_op_with_retry(picc, &secret_key, false, op_read_hmac, card_hmac, &auth_ok) != ESP_OK) {
        if (!auth_ok) {
            console_notify("DENEGADO: auth MIFARE (use 'format on' si es tarjeta nueva)");
            ESP_LOGE(TAG, "DENEGADO: auth MIFARE (tarjeta sin formatear? use 'format on')");
        } else {
            console_notify("DENEGADO: fallo lectura HMAC (centre la tarjeta en la antena)");
            ESP_LOGE(TAG, "DENEGADO: no se pudo leer HMAC (RF inestable: centre la tarjeta)");
        }
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_MIFARE);
        return;
    }

    console_notify("lectura bloques 4-5 OK (HMAC %02X%02X%02X%02X...)", card_hmac[0], card_hmac[1],
                   card_hmac[2], card_hmac[3]);

    if (security_hmac_is_unprovisioned(card_hmac)) {
        console_notify("DENEGADO: HMAC vacio — 'format on'");
        ESP_LOGW(TAG, "DENEGADO: tarjeta sin HMAC; use 'format on' y pase la tarjeta");
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_UNPROVISIONED);
        return;
    }

    lock_settings_t settings;
    if (storage_load_settings(&settings) != ESP_OK) {
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_HMAC);
        return;
    }

    uint8_t expected[HMAC_SIZE];
    if (security_calculate_hmac(uid, uid_len, settings.current_nonce, settings.master_key, expected) != 0) {
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_HMAC);
        return;
    }

    if (!security_verify_hmac(expected, card_hmac)) {
        console_notify("DENEGADO: HMAC no coincide (re-formatee o tarjeta de otro sistema)");
        ESP_LOGW(TAG, "DENEGADO: HMAC invalido (replay o tarjeta desincronizada)");
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_HMAC);
        return;
    }

    uint8_t new_nonce[NONCE_SIZE];
    uint8_t new_hmac[HMAC_SIZE];
    if (security_generate_next_session(uid, uid_len, &settings, new_nonce, new_hmac) != 0) {
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_HMAC);
        return;
    }

    // Orden: primero la tarjeta, luego NVS. Si falla la tarjeta, nada cambia.
    if (card_op_with_retry(picc, &secret_key, true, op_write_hmac, new_hmac, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo escribir el nuevo HMAC; nonce NVS sin cambiar");
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_HMAC);
        return;
    }

    if (storage_update_nonce(new_nonce) != ESP_OK) {
        ESP_LOGE(TAG, "HMAC en tarjeta actualizado pero NVS fallo: re-formatee la tarjeta");
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_HMAC);
        return;
    }

    console_notify("ACCESO CONCEDIDO — UID valido, sesion actualizada");
    ESP_LOGI(TAG, "ACCESO CONCEDIDO");
    access_log_append(uid, uid_len, ACCESS_LOG_GRANTED);
    if (s_grant_fn != NULL) {
        s_grant_fn();
    }
}

static void process_card(rc522_picc_t *picc) {
    size_t uid_len = picc->uid.length;
    if (uid_len == 0 || uid_len > MAX_UID_LEN) {
        return;
    }

    uint8_t uid[MAX_UID_LEN];
    memcpy(uid, picc->uid.value, uid_len);

    char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX] = {0};
    rc522_picc_uid_to_str(&picc->uid, uid_str, sizeof(uid_str));
    ESP_LOGI(TAG, "Procesando UID=%s tipo=%d (formateo=%s)", uid_str, picc->type,
             storage_get_format_mode() ? "ON" : "OFF");
    console_notify("procesando UID=%s (formateo %s)", uid_str,
                   storage_get_format_mode() ? "ON" : "OFF");

    if (!rc522_mifare_type_is_classic_compatible(picc->type)) {
        console_notify("DENEGADO: no es MIFARE Classic");
        ESP_LOGW(TAG, "DENEGADO: tipo de tarjeta no es MIFARE Classic");
        access_log_append(uid, uid_len, ACCESS_LOG_DENIED_TYPE);
        end_picc_session(picc);
        return;
    }

    access_mark_uid_handled(uid, uid_len);

    if (storage_get_format_mode()) {
        process_format_mode(picc, uid, uid_len);
    } else {
        process_production(picc, uid, uid_len);
    }

    end_picc_session(picc);
}

void access_task(void *arg) {
    (void)arg;
    access_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!take_rc522_lock()) {
            ESP_LOGW(TAG, "Timeout esperando mutex RC522");
            continue;
        }

        if (msg.type == ACCESS_MSG_HALT_ONLY) {
            end_picc_session(&msg.picc);
        } else {
            process_card(&msg.picc);
        }

        give_rc522_lock();
    }
}
