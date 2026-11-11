// main/main.c —— 「运动榜」应用入口：初始化 BSP、启动界面、挂接三键。
//
// 应用自己的页面、任务与存储都在 sports_app.c；本文件只负责硬件初始化顺序
// 和把按键回调转成非阻塞的入队操作。
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "key_guard.h"
#include "sports_app.h"

static const char *TAG = "main";
static volatile bool s_input_ready;

// 深睡按键唤醒后，唤醒键会在重启时仍被按住；守卫在它松手前吞掉所有按键动作。
// 本应用不主动进入深睡，但保留该契约，避免从深睡唤醒时误触首页长按。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 按键回调运行在 button 组件共享的 esp_timer 任务里：只入队，不访问 LVGL。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready) return;
    (void)sports_app_post_key(btn, ev);
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport 运动榜启动");

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    (void)bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是唯一输出；初始化失败时保留串口日志，不做无屏降级。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    esp_err_t err = sports_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(err));
        sports_app_show_error("应用启动失败");
        return;
    }

    // 应用队列先就绪，再把按键接上，避免启动窗口里的按键事件丢失。
    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s —— 检查 GPIO%d 的 ADC 分压与外部上拉",
                 esp_err_to_name(err), BSP_BTN_GPIO);
        sports_app_show_error("按键初始化失败");
        return;
    }

    if (s_key_guard.armed) {
        // 唤醒键在按键驱动启动前就松手时不会产生 release 事件:按实际电压同步一次。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }

    s_input_ready = true;
    ESP_LOGI(TAG, "就绪");
}
