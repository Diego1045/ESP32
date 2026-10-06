#include "driver/spi_master.h"
#include "rc522_pcd_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "access.h"
#include "access_log.h"
#include "actuator.h"
#include "console_cli.h"
#include "driver/gpio.h"
#include "driver/rc522_spi.h"
#include "hardware_profile.h"
#include "rc522.h"
#include "rc522_pcd.h"
#include "rc522_picc.h"
#include "storage.h"

static const char *TAG = "MAIN";

#define FORMAT_MODE_AUTO_OFF_MS (120 * 1000)

static rc522_handle_t scanner;
static TimerHandle_t format_mode_timer;
static SemaphoreHandle_t rc522_mutex;

static void rfid_watch_task(void *arg) {
  (void)arg;
  uint32_t last_hits = 0;
  uint32_t last_select_ok = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    rc522_diag_t d;
    rc522_diag_get(&d);
    const uint32_t hits = d.poll_ok + d.poll_collision;
    if (hits <= last_hits) {
      continue;
    }
    if (d.select_ok > last_select_ok) {
      last_select_ok = d.select_ok;
      last_hits = hits;
      continue;
    }
    last_hits = hits;
    if (d.last_select_err == 0) {
      continue;
    }
    if (d.last_select_err == 0xF527 || d.last_select_err == 0xF528) {
      console_notify(
          "REQA ok pero SELECT timeout (0x%04X) — tarjeta quieta en el centro; pruebe gain 23",
          (unsigned)d.last_select_err);
    } else {
      console_notify(
          "campo RF (ok=%u col=%u) sin UID — SELECT err=0x%04X | gain 38, centre tarjeta",
          (unsigned)d.poll_ok, (unsigned)d.poll_collision, (unsigned)d.last_select_err);
    }
  }
}

static void format_mode_timer_callback(TimerHandle_t timer) {
  (void)timer;
  storage_set_format_mode(false);
  ESP_LOGI(TAG, "Modo formateo desactivado automaticamente");
  printf("\n[CLI] Formateo auto-OFF. Escribe 'format on' para provisionar otra tarjeta.\n");
  fflush(stdout);
}

#if HW_HAS_BOOT_BUTTON
static void check_boot_format_request(void) {
  gpio_reset_pin(BOOT_GPIO);
  gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY);
  vTaskDelay(pdMS_TO_TICKS(80));
  if (gpio_get_level(BOOT_GPIO) == 0) {
    storage_set_format_mode(true);
    ESP_LOGW(TAG, "BOOT: formateo ON %d s", FORMAT_MODE_AUTO_OFF_MS / 1000);
    if (format_mode_timer != NULL) {
      xTimerReset(format_mode_timer, 0);
    }
  }
}
#else
static void check_boot_format_request(void) {
  ESP_LOGI(TAG, "Sin botón BOOT: use 'format on' en el monitor serie");
}
#endif

static const char *picc_state_name(rc522_picc_state_t state) {
  switch (state) {
  case RC522_PICC_STATE_IDLE:
    return "IDLE";
  case RC522_PICC_STATE_READY:
    return "READY";
  case RC522_PICC_STATE_ACTIVE:
    return "ACTIVE";
  case RC522_PICC_STATE_HALT:
    return "HALT";
  case RC522_PICC_STATE_READY_H:
    return "READY_H";
  case RC522_PICC_STATE_ACTIVE_H:
    return "ACTIVE_H";
  default:
    return "?";
  }
}

static void rc522_handler(void *arg, esp_event_base_t base, int32_t event_id,
                          void *event_data) {
  rc522_picc_state_changed_event_t *event =
      (rc522_picc_state_changed_event_t *)event_data;
  rc522_picc_t *picc = event->picc;

  if (picc->state == RC522_PICC_STATE_IDLE) {
    if (event->old_state == RC522_PICC_STATE_READY ||
        event->old_state == RC522_PICC_STATE_READY_H) {
      static TickType_t s_last_select_note;
      const TickType_t now = xTaskGetTickCount();
      if ((now - s_last_select_note) >= pdMS_TO_TICKS(5000)) {
        s_last_select_note = now;
        rc522_diag_t d;
        rc522_diag_get(&d);
        console_notify("SELECT fallo (0x%04X) — tarjeta quieta; pruebe gain 33",
                       (unsigned)d.last_select_err);
      }
    } else if (event->old_state == RC522_PICC_STATE_ACTIVE ||
               event->old_state == RC522_PICC_STATE_ACTIVE_H) {
      console_notify("estado %s -> IDLE", picc_state_name(event->old_state));
      access_on_picc_idle();
    }
    return;
  }

  if (picc->state != RC522_PICC_STATE_ACTIVE &&
      picc->state != RC522_PICC_STATE_ACTIVE_H) {
    return;
  }

  if (picc->uid.length == 0) {
    console_notify("ACTIVE sin UID (reintento en curso)");
    return;
  }

  if (access_should_ignore_uid(picc->uid.value, picc->uid.length)) {
    access_submit_halt_only(picc);
    return;
  }

  char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX] = {0};
  rc522_picc_uid_to_str(&picc->uid, uid_str, sizeof(uid_str));
  ESP_LOGI(TAG, "Tarjeta detectada: UID=%s", uid_str);
  access_submit_card(picc);
}

void app_main(void) {
  ESP_LOGI(TAG, "Cerradura — perfil banco (RC522 + tarjeta)");
  esp_log_level_set("rc522", ESP_LOG_WARN);

  ESP_ERROR_CHECK(storage_init());
  ESP_ERROR_CHECK(storage_ensure_secrets());
  ESP_ERROR_CHECK(access_log_init());
  access_log_dump_recent(5);

  check_boot_format_request();

  actuator_init();

  format_mode_timer =
      xTimerCreate("fmt_off", pdMS_TO_TICKS(FORMAT_MODE_AUTO_OFF_MS), pdFALSE,
                   NULL, format_mode_timer_callback);
  ESP_ERROR_CHECK(format_mode_timer == NULL ? ESP_ERR_NO_MEM : ESP_OK);

  rc522_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(rc522_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);

  ESP_LOGI(TAG, "RC522 SPI: MISO=%d MOSI=%d CLK=%d CS=%d RST=%d (swap_mosi_miso=%d)",
           PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS, PIN_NUM_RST, HW_SPI_SWAP_MOSI_MISO);

  rc522_spi_config_t spi_config = {
      .host_id = SPI2_HOST,
      .dma_chan = SPI_DMA_DISABLED,
      .bus_config =
          &(spi_bus_config_t){
              .miso_io_num = PIN_NUM_MISO,
              .mosi_io_num = PIN_NUM_MOSI,
              .sclk_io_num = PIN_NUM_CLK,
              .max_transfer_sz = 128,
          },
      .dev_config =
          {
              .spics_io_num = PIN_NUM_CS,
              .clock_speed_hz = 100000,
              .mode = 0,
          },
      .rst_io_num = PIN_NUM_RST};

  rc522_driver_handle_t driver;
  ESP_ERROR_CHECK(rc522_spi_create(&spi_config, &driver));
  ESP_ERROR_CHECK(rc522_driver_install(driver));

  rc522_config_t config = {
      .driver = driver,
      .poll_interval_ms = 50,
      .task_mutex = rc522_mutex,
  };

  ESP_ERROR_CHECK(rc522_create(&config, &scanner));
  // IMPRESCINDIBLE: sin este registro el lector detecta tarjetas pero nadie
  // procesa el evento y "no pasa nada".
  ESP_ERROR_CHECK(rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                        rc522_handler, NULL));

  access_set_grant_handler(actuator_grant_access);
  access_system_init(NULL, scanner, rc522_mutex);
  console_cli_start(format_mode_timer, scanner, rc522_mutex);

  esp_err_t rc_start = rc522_start(scanner);
  if (rc_start == ESP_OK) {
    (void)rc522_pcd_set_rx_gain(scanner, RC522_PCD_48_DB_RX_GAIN);
  }
  if (rc_start != ESP_OK) {
    rc522_pcd_firmware_t fw = 0;
    (void)rc522_pcd_firmware(scanner, &fw);
    ESP_LOGE(TAG, "RC522 no inicializado: %s (version reg=0x%02X)", esp_err_to_name(rc_start),
             (unsigned)fw);
    printf("\n[RFID] ERROR SPI: version RC522=0x%02X (valido: 0x91 o 0x92).\n"
           "  Si es 0x00/0xFF: cableado CS=D23 SCK=D26 MOSI=D25 MISO=D27 RST=D22, 3.3V.\n"
           "  Si version OK pero FIFO falla: pruebe intercambiar cables MOSI y MISO.\n\n",
           (unsigned)fw);
    fflush(stdout);
  } else {
    xTaskCreate(rfid_watch_task, "rfid_watch", 2048, NULL, 1, NULL);
    ESP_LOGI(TAG, "RFID listo. Monitor 115200 — escribe 'help'");
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
