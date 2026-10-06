#include "actuator.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/timers.h"
#include "hardware_profile.h"
#include <stdio.h>

static const char *TAG = "ACTUATOR";
static TimerHandle_t s_relay_timer;

#define RELAY_ON_TIME_MS 2000

static void relay_off_timer_cb(TimerHandle_t t) {
    (void)t;
#if HW_HAS_RELAY
    gpio_set_level(RELAY_GPIO, 0);
    ESP_LOGI(TAG, "Relé OFF");
#else
    printf("\n*** [SIM] RELÉ OFF ***\n");
    fflush(stdout);
#endif
}

void actuator_init(void) {
    s_relay_timer = xTimerCreate("relay_off", pdMS_TO_TICKS(RELAY_ON_TIME_MS), pdFALSE, NULL,
                                 relay_off_timer_cb);
#if HW_HAS_RELAY
    gpio_reset_pin(RELAY_GPIO);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_GPIO, 0);
    ESP_LOGI(TAG, "Relé en GPIO %d", RELAY_GPIO);
#else
    ESP_LOGI(TAG, "Sin relé fisico: acceso se anuncia en consola");
#endif
}

void actuator_grant_access(void) {
#if HW_HAS_RELAY
    gpio_set_level(RELAY_GPIO, 1);
    if (s_relay_timer != NULL && xTimerReset(s_relay_timer, 0) != pdPASS) {
        gpio_set_level(RELAY_GPIO, 0);
        ESP_LOGE(TAG, "Timer relé fallo");
        return;
    }
    ESP_LOGI(TAG, "Relé ON (%d ms)", RELAY_ON_TIME_MS);
#else
    printf("\n*** [SIM] RELÉ ON %d ms — ACCESO CONCEDIDO ***\n", RELAY_ON_TIME_MS);
    fflush(stdout);
    if (s_relay_timer != NULL) {
        xTimerReset(s_relay_timer, 0);
    }
#endif
}

void bench_console_unlock(void) {
    ESP_LOGI(TAG, "unlock manual (consola)");
    actuator_grant_access();
}
