#ifndef LAUNCHER_CONTRACT_H
#define LAUNCHER_CONTRACT_H

#include "esp_err.h"

/**
 * Reverify the factory Launcher, select it as the boot target, and restart.
 *
 * The function returns only on failure. It never selects an unverified image
 * and never hardcodes a factory address. Call it only from task context after
 * the play confirms it is on its cover. Do not call
 * esp_ota_mark_app_valid_cancel_rollback() on this path: a one-shot slot
 * should stay invalid so a reset still returns to the Launcher.
 *
 * ESP_ERR_NOT_FOUND           no app partition labeled "factory"
 * ESP_ERR_INVALID_STATE       already running from factory
 * ESP_ERR_OTA_VALIDATE_FAILED factory image failed verification
 * other                       esp_ota_set_boot_partition failed
 */
esp_err_t launcher_contract_return_to_factory(void);

#endif
