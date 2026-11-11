// main/sports_store.h —— 运动榜的 NVS 持久化边界。
//
// 只负责初始化 NVS、读写一个固定长度 blob；成绩语义与解码校验在
// sports_model.c，屏幕与任务在 sports_app.c。
#pragma once

#include "esp_err.h"
#include "sports_model.h"

// 幂等初始化。仅在 NVS 无空闲页或版本不兼容时擦除并重建整个 NVS 分区。
esp_err_t sports_store_init(void);

// 读取失败时 data 保持默认值并返回错误，调用方可继续以空榜运行。
esp_err_t sports_store_load(sports_data_t *data);

// 写入前会再次 sanitize；失败不影响内存中的榜单。
esp_err_t sports_store_save(const sports_data_t *data);
