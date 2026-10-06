#ifndef ACCESS_H
#define ACCESS_H

#include "rc522.h"
#include "rc522_picc.h"
#include "freertos/semphr.h"

typedef struct {
    rc522_handle_t scanner;
    SemaphoreHandle_t rc522_mutex;
} access_context_t;

void access_system_init(access_context_t *ctx, rc522_handle_t scanner, SemaphoreHandle_t rc522_mutex);

void access_on_picc_idle(void);

/** Olvida el ultimo UID para poder reprocesar la misma tarjeta sin retirarla. */
void access_forget_last_uid(void);

bool access_should_ignore_uid(const uint8_t *uid, size_t uid_len);

void access_mark_uid_handled(const uint8_t *uid, size_t uid_len);

void access_submit_card(const rc522_picc_t *picc);

void access_submit_halt_only(const rc522_picc_t *picc);

void access_set_grant_handler(void (*grant_relay)(void));

void access_task(void *arg);

#endif
