#include "console_cli.h"
#include "access.h"
#include "access_log.h"
#include "actuator.h"
#include "hardware_profile.h"
#include "rc522_diag.h"
#include "rc522_pcd.h"
#include "rc522_pcd_internal.h"
#include "rc522_picc_internal.h"
#include "storage.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONSOLE_NOTIFY_Q_LEN 8
#define CONSOLE_NOTIFY_MSG_LEN 120

static QueueHandle_t s_notify_queue;

static void console_notify_task(void *arg) {
    (void)arg;
    char msg[CONSOLE_NOTIFY_MSG_LEN];
    for (;;) {
        if (xQueueReceive(s_notify_queue, msg, portMAX_DELAY) == pdTRUE) {
            fputs("\n[RFID] ", stdout);
            fputs(msg, stdout);
            fputs("\n", stdout);
            fflush(stdout);
        }
    }
}

void console_notify(const char *fmt, ...) {
    if (s_notify_queue == NULL) {
        va_list ap;
        va_start(ap, fmt);
        fputs("\n[RFID] ", stdout);
        vprintf(fmt, ap);
        va_end(ap);
        fputs("\n", stdout);
        fflush(stdout);
        return;
    }
    char msg[CONSOLE_NOTIFY_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (void)xQueueSend(s_notify_queue, msg, 0);
}

static const char *TAG = "CLI";
static TimerHandle_t s_format_timer;
static rc522_handle_t s_scanner;
static SemaphoreHandle_t s_rc522_mutex;

static void trim(char *s) {
    if (s == NULL) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool parse_uid_hex(const char *hex, uint8_t *out, size_t *out_len) {
    if (hex == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    size_t len = strlen(hex);
    if (len == 0 || (len % 2) != 0 || len > REVOKED_UID_MAX * 2) {
        return false;
    }
    size_t bytes = len / 2;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = bytes;
    return true;
}

static void cmd_help(void) {
    printf(
        "\nComandos (banco: tarjeta + RC522, sin relé fisico):\n"
        "  help              Esta ayuda\n"
        "  status            Estado formateo / perfil hardware\n"
        "  fw                Lee registro de version del RC522 (prueba SPI)\n"
        "  diag [reset]      Contadores del lector (REQA/SELECT) para diagnostico\n"
        "  gain <dB>         Ganancia RX: 18|23|33|38|43|48 (prueba en vivo)\n"
        "  scan              Una lectura REQA+SELECT (prueba manual)\n"
        "  selftest          SPI: version + FIFO + antena TX\n"
        "  tap <UIDhex>      Emula tarjeta (sin RF) para probar access/NVS\n"
        "\nLas lecturas del lector se muestran como lineas [RFID] en este monitor.\n"
        "  format on         Provisionar tarjetas (sector + HMAC), 120 s\n"
        "  format off        Modo produccion\n"
        "  logs [N]          Ultimas N entradas NVS (default 10)\n"
        "  revoke <UIDhex>   Revocar UID (ej. A1B2C3D4)\n"
        "  unlock            Simula pulso de relé (solo consola)\n"
        "\nFlujo: format on -> acerca tarjeta -> format off -> acerca tarjeta\n\n");
}

static void cmd_status(void) {
    printf("Perfil: RELAY=%s BOOT=%s consola=%s\n",
           HW_HAS_RELAY ? "si" : "no",
           HW_HAS_BOOT_BUTTON ? "si" : "no",
           HW_ACTUATOR_CONSOLE ? "si" : "no");
    printf("Modo formateo NVS: %s\n", storage_get_format_mode() ? "ON" : "OFF");
}

static void cmd_fw(void) {
    if (xSemaphoreTake(s_rc522_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        printf("Lector ocupado.\n");
        return;
    }
    rc522_pcd_firmware_t fw = 0;
    esp_err_t err = rc522_pcd_firmware(s_scanner, &fw);
    xSemaphoreGive(s_rc522_mutex);
    if (err != ESP_OK) {
        printf("No se pudo leer version (SPI roto o modulo apagado).\n");
        return;
    }
    printf("RC522 version reg = 0x%02X (%s)\n", (unsigned)fw,
           (fw == 0x91 || fw == 0x92) ? "MFRC522 OK" : "revision rara o clone");
}

static void cmd_diag(const char *arg) {
    if (arg != NULL && strcmp(arg, "reset") == 0) {
        rc522_diag_reset();
        printf("Contadores a cero.\n");
        return;
    }

    rc522_diag_t d;
    rc522_diag_get(&d);
    printf("Sondeo REQA/WUPA : total=%u ok=%u colision=%u timeout=%u paridad=%u atqa_inv=%u otros=%u\n",
           (unsigned)d.poll_total, (unsigned)d.poll_ok, (unsigned)d.poll_collision,
           (unsigned)d.poll_timeout, (unsigned)d.poll_parity, (unsigned)d.poll_invalid_atqa,
           (unsigned)d.poll_other);
    printf("SELECT           : ok=%u fallo=%u   heartbeat_fallo=%u\n", (unsigned)d.select_ok,
           (unsigned)d.select_fail, (unsigned)d.heartbeat_fail);
    printf("Ultimo error sondeo=0x%04X  ultimo error select=0x%04X\n", (unsigned)d.last_poll_err,
           (unsigned)d.last_select_err);
    printf("Lectura: sin tarjeta casi todo es 'timeout'. Con la tarjeta puesta deben subir\n"
           "'ok' o 'colision' y luego 'SELECT ok'. Si solo sube 'timeout', el lector no la ve\n"
           "(3,3 V, cableado SPI o antena).\n");
}

static void cmd_selftest(void) {
    (void)rc522_pause(s_scanner);
    if (xSemaphoreTake(s_rc522_mutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
        printf("Lector ocupado.\n");
        (void)rc522_start(s_scanner);
        return;
    }

    rc522_pcd_firmware_t fw = 0;
    esp_err_t fw_err = rc522_pcd_firmware(s_scanner, &fw);
    esp_err_t rw_err = rc522_pcd_rw_test(s_scanner);
    uint8_t txc = 0;
    (void)rc522_pcd_read(s_scanner, RC522_PCD_TX_CONTROL_REG, &txc);
    xSemaphoreGive(s_rc522_mutex);
    (void)rc522_start(s_scanner);

    printf("selftest: version=0x%02X (%s)\n", (unsigned)fw,
           fw_err == ESP_OK ? "leida" : "fallo");
    printf("selftest: fifo_rw=%s\n", rw_err == ESP_OK ? "OK" : esp_err_to_name(rw_err));
    printf("selftest: TxControl=0x%02X antena_RF=%s\n", (unsigned)txc,
           (txc & (RC522_PCD_TX1_RF_EN_BIT | RC522_PCD_TX2_RF_EN_BIT)) ? "ON" : "OFF");
    printf("selftest: swap_mosi_miso=%d  pines MISO=%d MOSI=%d CS=%d CLK=%d RST=%d\n",
           HW_SPI_SWAP_MOSI_MISO, PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CS, PIN_NUM_CLK, PIN_NUM_RST);
}

static void cmd_tap(const char *hex) {
    uint8_t uid[RC522_PICC_UID_SIZE_MAX];
    size_t uid_len = 0;
    if (hex == NULL || !parse_uid_hex(hex, uid, &uid_len)) {
        printf("Uso: tap 44CBC871  (UID hex, sin espacios)\n");
        return;
    }
    rc522_picc_t picc = {0};
    memcpy(picc.uid.value, uid, uid_len);
    picc.uid.length = uid_len;
    picc.sak = 0x08;
    picc.type = RC522_PICC_TYPE_MIFARE_1K;

    char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX] = {0};
    rc522_picc_uid_to_str(&picc.uid, uid_str, sizeof(uid_str));
    access_forget_last_uid();
    access_submit_card(&picc);
    printf("tap encolado: UID=%s (sin RF)\n", uid_str);
}

static void cmd_scan(void) {
    (void)rc522_pause(s_scanner);
    if (xSemaphoreTake(s_rc522_mutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
        (void)rc522_start(s_scanner);
        printf("Lector ocupado; espere y reintente.\n");
        return;
    }

    (void)rc522_pcd_stop_crypto1(s_scanner);
    (void)rc522_pcd_stop_active_command(s_scanner);
    (void)rc522_pcd_fifo_flush(s_scanner);

    rc522_picc_atqa_desc_t atqa;
    esp_err_t err = rc522_picc_reqa(s_scanner, &atqa);
    if (err != ESP_OK) {
        err = rc522_picc_wupa(s_scanner, &atqa);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_rc522_mutex);
        (void)rc522_start(s_scanner);
        printf("REQA/WUPA fallo (0x%04X). Ponga la tarjeta en el centro de la antena.\n", (unsigned)err);
        return;
    }

    rc522_picc_uid_t uid;
    uint8_t sak = 0;
    memset(&uid, 0, sizeof(uid));
    err = rc522_picc_select(s_scanner, &uid, &sak, false);
    if (err != ESP_OK) {
        (void)rc522_picc_wupa(s_scanner, &atqa);
        err = rc522_picc_select(s_scanner, &uid, &sak, false);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_rc522_mutex);
        (void)rc522_start(s_scanner);
        printf("SELECT fallo (0x%04X). Centre la tarjeta sobre la antena.\n", (unsigned)err);
        return;
    }

    rc522_picc_t picc = {.uid = uid, .sak = sak};
    picc.type = rc522_picc_get_type(&picc);
    char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX] = {0};
    rc522_picc_uid_to_str(&uid, uid_str, sizeof(uid_str));
    xSemaphoreGive(s_rc522_mutex);
    (void)rc522_start(s_scanner);

    console_notify("scan manual: UID=%s SAK=0x%02X tipo=%d", uid_str, sak, (int)picc.type);
}

static void cmd_gain(const char *arg) {
    static const struct {
        int db;
        rc522_pcd_rx_gain_t value;
    } gains[] = {
        {18, RC522_PCD_18_DB_RX_GAIN}, {23, RC522_PCD_23_DB_RX_GAIN}, {33, RC522_PCD_33_DB_RX_GAIN},
        {38, RC522_PCD_38_DB_RX_GAIN}, {43, RC522_PCD_43_DB_RX_GAIN}, {48, RC522_PCD_48_DB_RX_GAIN},
    };

    int db = (arg != NULL) ? atoi(arg) : 0;
    for (size_t i = 0; i < sizeof(gains) / sizeof(gains[0]); i++) {
        if (gains[i].db != db) {
            continue;
        }
        if (xSemaphoreTake(s_rc522_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            printf("Lector ocupado; reintente.\n");
            return;
        }
        esp_err_t err = rc522_pcd_set_rx_gain(s_scanner, gains[i].value);
        xSemaphoreGive(s_rc522_mutex);
        if (err == ESP_OK) {
            printf("Ganancia RX = %d dB (no persiste tras reinicio).\n", db);
        } else {
            printf("Error ajustando ganancia (0x%04X)\n", (unsigned)err);
        }
        return;
    }
    printf("Uso: gain 18|23|33|38|43|48  (33 = valor por defecto)\n");
}

static void cmd_format(const char *arg) {
    if (arg == NULL) {
        return;
    }
    access_forget_last_uid();
    if (strcmp(arg, "on") == 0) {
        storage_set_format_mode(true);
        if (s_format_timer != NULL) {
            xTimerReset(s_format_timer, 0);
        }
        printf("Formateo ON. Pase la tarjeta. Auto-OFF en 120 s o 'format off'.\n");
    } else if (strcmp(arg, "off") == 0) {
        storage_set_format_mode(false);
        printf("Formateo OFF (produccion).\n");
    } else {
        printf("Uso: format on | format off\n");
    }
}

static void cmd_logs(const char *arg) {
    unsigned n = 10;
    if (arg != NULL && arg[0] != '\0') {
        n = (unsigned)strtoul(arg, NULL, 10);
    }
    access_log_dump_recent(n);
}

static void cmd_revoke(const char *hex) {
    uint8_t uid[REVOKED_UID_MAX];
    size_t uid_len = 0;
    if (!parse_uid_hex(hex, uid, &uid_len)) {
        printf("UID invalido. Ejemplo: revoke DEADBEEF\n");
        return;
    }
    if (storage_revoke_uid(uid, uid_len) == ESP_OK) {
        printf("UID revocado.\n");
    } else {
        printf("Error al revocar (lista llena?).\n");
    }
}

static void dispatch_line(char *line) {
    trim(line);
    if (line[0] == '\0') {
        return;
    }

    char *cmd = strtok(line, " \t");
    char *arg = strtok(NULL, " \t");

    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status();
    } else if (strcmp(cmd, "fw") == 0) {
        cmd_fw();
    } else if (strcmp(cmd, "diag") == 0) {
        cmd_diag(arg);
    } else if (strcmp(cmd, "gain") == 0) {
        cmd_gain(arg);
    } else if (strcmp(cmd, "scan") == 0) {
        cmd_scan();
    } else if (strcmp(cmd, "selftest") == 0) {
        cmd_selftest();
    } else if (strcmp(cmd, "tap") == 0) {
        cmd_tap(arg);
    } else if (strcmp(cmd, "format") == 0) {
        cmd_format(arg);
    } else if (strcmp(cmd, "logs") == 0) {
        cmd_logs(arg);
    } else if (strcmp(cmd, "revoke") == 0) {
        if (arg == NULL) {
            printf("Uso: revoke <UIDhex>\n");
        } else {
            cmd_revoke(arg);
        }
    } else if (strcmp(cmd, "unlock") == 0) {
        bench_console_unlock();
    } else {
        printf("Comando desconocido: %s (escribe help)\n", cmd);
    }
}

#define CLI_UART_NUM ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)

static void console_task(void *arg) {
    (void)arg;

    // fgets(stdin) sin driver UART devuelve EOF de forma continua; se lee el
    // UART directamente. Si el driver ya estaba instalado, el error se ignora.
    esp_err_t drv = uart_driver_install(CLI_UART_NUM, 512, 0, 0, NULL, 0);
    if (drv != ESP_OK && drv != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "uart_driver_install: 0x%04X", (unsigned)drv);
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    cmd_help();
    printf("lock> ");
    fflush(stdout);

    char line[128];
    size_t len = 0;
    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(CLI_UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            printf("\n");
            line[len] = '\0';
            if (len > 0) {
                dispatch_line(line);
            }
            len = 0;
            printf("lock> ");
            fflush(stdout);
        } else if (ch == 0x08 || ch == 0x7F) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (ch >= 0x20 && ch < 0x7F && len < sizeof(line) - 1) {
            line[len++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

void console_cli_start(TimerHandle_t format_mode_auto_off_timer, rc522_handle_t scanner,
                       SemaphoreHandle_t rc522_mutex) {
    s_format_timer = format_mode_auto_off_timer;
    s_scanner = scanner;
    s_rc522_mutex = rc522_mutex;
    s_notify_queue = xQueueCreate(CONSOLE_NOTIFY_Q_LEN, CONSOLE_NOTIFY_MSG_LEN);
    if (s_notify_queue != NULL) {
        xTaskCreate(console_notify_task, "rfid_notify", 2048, NULL, 2, NULL);
    }
    xTaskCreate(console_task, "console_cli", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "CLI serie activa (escribe help en el monitor)");
}
