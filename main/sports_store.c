#include "sports_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "sports_store";
static const char *NVS_NAMESPACE = "sports";
static const char *NVS_KEY = "data";

esp_err_t sports_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sports_store_load(sports_data_t *data) {
    if (!data) return ESP_ERR_INVALID_ARG;
    sports_data_init(data);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // 第一次运行还没有榜单
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t blob[SPORTS_BLOB_SIZE];
    size_t length = sizeof(blob);
    err = nvs_get_blob(handle, NVS_KEY, blob, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取榜单失败: %s", esp_err_to_name(err));
        return err;
    }
    if (!sports_decode(data, blob, length)) {
        ESP_LOGW(TAG, "榜单数据校验失败，已回到空榜");
        sports_data_init(data);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t sports_store_save(const sports_data_t *data) {
    if (!data) return ESP_ERR_INVALID_ARG;

    uint8_t blob[SPORTS_BLOB_SIZE];
    const size_t length = sports_encode(data, blob, sizeof(blob));
    if (length == 0) {
        ESP_LOGE(TAG, "榜单编码失败");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY, blob, length);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存榜单失败: %s", esp_err_to_name(err));
    }
    return err;
}
