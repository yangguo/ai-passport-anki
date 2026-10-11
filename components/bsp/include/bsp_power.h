#pragma once

#include "esp_err.h"

// Deep-sleep wake uses the board's active-low ADC-ladder key on GPIO0.
// On a GPIO wake, wait for release before iot_button initializes the ladder.
esp_err_t bsp_power_wait_for_wake_release(void);

// Persisting game state is the caller's responsibility. This function holds
// LVGL, sleeps the battery gauge, prepares shared pins/display and enters sleep.
esp_err_t bsp_power_enter_deep_sleep(void);
