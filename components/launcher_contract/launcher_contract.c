#include "launcher_contract.h"

#include <stddef.h>

#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "launcher_contract";

esp_err_t launcher_contract_return_to_factory(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
    esp_partition_pos_t position;
    esp_image_metadata_t metadata;
    esp_err_t error;

    if (factory == NULL) {
        ESP_LOGE(TAG, "no app partition labeled \"factory\"");
        return ESP_ERR_NOT_FOUND;
    }
    if (running == factory) {
        ESP_LOGE(TAG, "already running from factory");
        return ESP_ERR_INVALID_STATE;
    }

    position.offset = factory->address;
    position.size = factory->size;
    error = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "factory image failed verification: %s", esp_err_to_name(error));
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    error = esp_ota_set_boot_partition(factory);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "cannot select factory as boot partition: %s", esp_err_to_name(error));
        return error;
    }

    ESP_LOGI(TAG, "restarting into factory Launcher");
    esp_restart();
}
