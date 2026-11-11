// main/sports_app.h —— 「运动榜」应用层入口。
//
// main.c 只负责 BSP 初始化与按键回调；界面、会话计时、榜单保存和任务都封装
// 在这里。按键回调只能调用 sports_app_post_key()（仅入队）。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t sports_app_start(void);

// 从按键回调调用：仅把事件放进应用队列，队列满或应用未就绪时返回 false。
bool sports_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 硬件初始化失败时替换为可读的错误页；不需要应用任务先启动。
void sports_app_show_error(const char *message);
