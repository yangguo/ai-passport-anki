#include "bsp_power.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp-power";

esp_err_t bsp_power_wait_for_wake_release(void) {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) return ESP_OK;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BSP_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    while (gpio_get_level(BSP_BTN_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t bsp_power_enter_deep_sleep(void) {
    /* ADC setup disables the pad's digital input. The C3 wakeup API only
     * selects the trigger; restore input before the sleep code holds the pad.
     * Keep the external ladder pull-up, without changing its thresholds. */
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BSP_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    if (gpio_get_level(BSP_BTN_GPIO) == 0) return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (gpio_get_level(BSP_BTN_GPIO) == 0) return ESP_ERR_INVALID_STATE;

    err = esp_deep_sleep_enable_gpio_wakeup(
        1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) return err;

    // Prevent an LVGL flush while the panel and SPI pins are being shut down.
    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    err = bsp_battery_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "CW2017 休眠校验失败，仍继续系统休眠");
    err = bsp_i2c_prepare_deep_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "I2C 低功耗准备失败，仍继续系统休眠");
    err = bsp_display_prepare_deep_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "显示低功耗准备失败，仍继续系统休眠");

    ESP_LOGI(TAG, "进入深度休眠；按任一按键唤醒");
    esp_deep_sleep_start();
    // The sleep API normally does not return; restart rather than continue with
    // the display bus and LVGL lock already shut down if it unexpectedly does.
    esp_restart();
    return ESP_FAIL;
}
