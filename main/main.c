#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#include "driver/rc522_spi.h"
#include "picc/rc522_mifare.h"
#include "rc522.h"
#include "rc522_picc.h"
#include "security.h"
#include "storage.h"
// Acceso a la funcion interna halta() para resetear la tarjeta tras cada operacion
#include "rc522_picc_internal.h"

static const char *TAG = "MAIN";

#define RELAY_GPIO 2
#define PIN_NUM_MISO 26 // D26 - movido de D12 (strapping pin)
#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK 14
#define PIN_NUM_CS 27  // D27 - lado derecho del ESP32, junto a D12/13/14
#define PIN_NUM_RST -1 // VN/GPIO39 es solo entrada; usamos soft-reset

static rc522_handle_t scanner;
static lock_settings_t settings;

// Variables para control de intentos y tiempos
static TickType_t last_scan_time = 0;
static int failed_attempts = 0;
static TickType_t block_until = 0;

// CAMBIA ESTO A false DESPUES DE HABER FORMATEADO TUS TARJETAS
static bool format_mode = false;

static void rc522_handler(void *arg, esp_event_base_t base, int32_t event_id,
                          void *event_data) {
  rc522_picc_state_changed_event_t *event =
      (rc522_picc_state_changed_event_t *)event_data;
  rc522_picc_t *picc = event->picc;

  if (picc->state == RC522_PICC_STATE_ACTIVE) {
    TickType_t now = xTaskGetTickCount();

    // Verificar si estamos bloqueados por 2 minutos
    if (block_until != 0 && now < block_until) {
      ESP_LOGW(TAG, "SISTEMA BLOQUEADO. Intento ignorado. Faltan %d segundos.",
               (block_until - now) * portTICK_PERIOD_MS / 1000);
      return;
    }

    // Cooldown de 1.5 segundos entre CADA lectura
    if (now - last_scan_time < pdMS_TO_TICKS(1500)) {
      return; // Ignorar si fue muy rápido
    }
    last_scan_time = now;

    ESP_LOGI(TAG, "Tarjeta detectada UID!");

// Soporte para UIDs de hasta 10 bytes (el maximo estandar ISO 14443A)
#define MAX_UID_LEN 10
    uint8_t uid[MAX_UID_LEN] = {0};
    size_t uid_len =
        (picc->uid.length <= MAX_UID_LEN) ? picc->uid.length : MAX_UID_LEN;

    if (uid_len > 0) {
      memcpy(uid, picc->uid.value, uid_len);
    }

    // -----------------------------------------------------------
    // LÓGICA DE AUTENTICACIÓN MIFARE (CRYPTO-1)
    // -----------------------------------------------------------
    if (rc522_mifare_type_is_classic_compatible(picc->type)) {
      rc522_mifare_key_t secret_key = {.type = RC522_MIFARE_KEY_A,
                                       .value = SECTOR_SECRET_KEY};
      rc522_mifare_key_t factory_key = {
          .type = RC522_MIFARE_KEY_A,
          .value = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

      if (format_mode) {
        ESP_LOGI(TAG, "ESTADO: MODO FORMATEO ACTIVO");
        // En modo formateo, intentamos entrar con la clave de fábrica
        if (rc522_mifare_auth(scanner, picc, SECURE_SECTOR_TRAILER,
                              &factory_key) == ESP_OK) {
          ESP_LOGI(TAG, "Clave de fabrica detectada. Formateando tarjeta con "
                        "nueva clave segura...");

          // Para cambiar la Key A, escribimos en el Sector Trailer.
          // Estructura del Trailer (16 bytes): [Key A (6 bytes)] [Access Bits
          // (4 bytes)] [Key B (6 bytes)]
          uint8_t new_trailer[RC522_MIFARE_BLOCK_SIZE] = {
              0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, // Tu SECTOR_SECRET_KEY
              0xFF, 0x07, 0x80, 0x69,             // Bits de acceso estandar
              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF  // Key B (por defecto)
          };

          if (rc522_mifare_write(scanner, picc, SECURE_SECTOR_TRAILER,
                                 new_trailer) == ESP_OK) {
            ESP_LOGI(TAG,
                     ">> EXITO: TARJETA FORMATEADA. AHORA ESTA ASEGURADA.");
          } else {
            ESP_LOGE(TAG, ">> ERROR AL ESCRIBIR EN LA TARJETA.");
          }
        } else {
          ESP_LOGW(TAG, "No se pudo acceder con la clave de fabrica. O ya esta "
                        "formateada, o es otra clave.");
        }
        rc522_mifare_deauth(scanner, picc); // Detener crypto en el lector (PCD)
        rc522_picc_halta(scanner, picc);    // Enviar HALT a la tarjeta (PICC) - CRITICO
        return; // En modo formateo no abrimos la puerta
      } else {
        // MODO SEGURO (PRODUCCIÓN)
        ESP_LOGI(TAG, "Intentando abrir Sector con clave secreta...");
        if (rc522_mifare_auth(scanner, picc, SECURE_SECTOR_BLOCK,
                              &secret_key) == ESP_OK) {
          // La autenticacion con la clave secreta es suficiente prueba.
          // El simple hecho de que la tarjeta conozca la clave de 281 billones
          // de combinaciones DEMUESTRA que no es un clon de UID.
          ESP_LOGI(TAG, "Autenticacion de Sector: OK. Clave secreta verificada.");

          uint8_t received_hash[HMAC_SIZE];
          security_calculate_hmac(uid, uid_len, settings.current_nonce,
                                  settings.master_key, received_hash);

          if (security_validate_and_update_card(uid, uid_len, received_hash,
                                                &settings) == 0) {
            ESP_LOGI(TAG, "ACCESO CONCEDIDO");
            failed_attempts = 0;
            block_until = 0;

            gpio_set_level(RELAY_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            gpio_set_level(RELAY_GPIO, 0);
            storage_load_settings(&settings);
          }
        } else {
          failed_attempts++;
          ESP_LOGE(TAG,
                   "ACCESO DENEGADO (Clave incorrecta / Posible Clon). Intento "
                   "%d de 5",
                   failed_attempts);
          if (failed_attempts >= 5) {
            ESP_LOGE(
                TAG,
                "¡5 INTENTOS FALLIDOS! Bloqueando sistema por 2 minutos...");
            block_until = now + pdMS_TO_TICKS(120000);
          }
          // Forzar cooldown extra tras fallo de auth para que el RC522 se
          // recupere del estado criptografico. Sin esto, la tarjeta queda
          // "trabada".
          last_scan_time = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        }
        rc522_mifare_deauth(scanner, picc); // Detener crypto en el lector (PCD)
        rc522_picc_halta(scanner, picc);    // Enviar HALT a la tarjeta (PICC) - CRITICO
      }
      // Cooldown adicional post-operacion MIFARE
      last_scan_time = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    } else {
      ESP_LOGW(TAG,
               "La tarjeta no es compatible con seguridad MIFARE Classic.");
    }
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "Iniciando Cerradura de Alta Seguridad...");

  // Mostrar logs de advertencias y errores de la libreria rc522
  esp_log_level_set("rc522", ESP_LOG_WARN);

  // 1. Inicializar Almacenamiento
  ESP_ERROR_CHECK(storage_init());
  ESP_ERROR_CHECK(storage_load_settings(&settings));

  // 2. Configurar Relé
  gpio_reset_pin(RELAY_GPIO);
  gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(RELAY_GPIO, 0);

  // 3. Configurar e Inicializar RC522 (Lector RFID real)
  rc522_spi_config_t spi_config = {.host_id = SPI2_HOST,
                                   .bus_config =
                                       &(spi_bus_config_t){
                                           .miso_io_num = PIN_NUM_MISO,
                                           .mosi_io_num = PIN_NUM_MOSI,
                                           .sclk_io_num = PIN_NUM_CLK,
                                       },
                                   .dev_config =
                                       {
                                           .spics_io_num = PIN_NUM_CS,
                                       },
                                   .rst_io_num = PIN_NUM_RST};

  rc522_driver_handle_t driver;
  ESP_ERROR_CHECK(rc522_spi_create(&spi_config, &driver));
  ESP_ERROR_CHECK(rc522_driver_install(driver));

  rc522_config_t config = {
      .driver = driver,
  };

  ESP_ERROR_CHECK(rc522_create(&config, &scanner));
  ESP_ERROR_CHECK(rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                        rc522_handler, NULL));
  ESP_ERROR_CHECK(rc522_start(scanner));

  ESP_LOGI(TAG, "Sistema listo. Esperando tarjeta real...");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
