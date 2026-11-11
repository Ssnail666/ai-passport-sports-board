// main/sports_app.c —— 「运动榜」应用：重新设计的运动计数与排行榜界面。
//
// 线程模型：
//   * BSP 按键回调只调用 sports_app_post_key() 入队；
//   * 应用任务独占屏幕切换、NVS 保存和会话状态；
//   * 所有 LVGL 访问都在 bsp_lvgl_lock() 内完成，活动页的 100ms 刷新定时器
//     运行在 LVGL 任务中，只读取 s_live 快照。
//
// 交互（三键）：
//   首页      上/下选择项目  OK 进入准备页  长按OK 排行榜  长按上键 换选手
//   准备页    上/下换选手    OK 开始        长按OK 返回
//   计数页    OK 计数        上键 结束
//   计时页    OK 开始/停止   上键 结束/返回
//   距离页    上/下调整       OK 保存        长按OK 取消
//   结果页    OK 再来一次    上键 排行榜    长按OK 首页
//   排行榜    上/下切换项目   OK 去挑战      长按OK 返回
#include "sports_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sports_model.h"
#include "sports_store.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(sports_font_16);
LV_FONT_DECLARE(sports_font_20);
LV_FONT_DECLARE(sports_font_28);

static const char *TAG = "sports_app";

// 深色田径场配色：荧光绿为主色，橙/蓝/粉区分选手，金银铜只用于名次。
#define C_BG 0x0B1220u
#define C_DARK 0x0B1220u
#define C_SURFACE 0x141F33u
#define C_SURFACE2 0x1B2942u
#define C_LINE 0x2A3A55u
#define C_TEXT 0xF2F6EAu
#define C_MUTED 0x8797AFu
#define C_LIME 0xC8F45Au
#define C_ORANGE 0xFF7A3Cu
#define C_SKY 0x4CC9F0u
#define C_PINK 0xFF7AD9u
#define C_GOLD 0xFFD166u
#define C_SILVER 0xC9D6E3u
#define C_BRONZE 0xE0A16Bu

#define IDLE_DIM_US (60LL * 1000 * 1000)
#define BATTERY_REFRESH_US (30LL * 1000 * 1000)
#define INPUT_REPEAT_FIRST_US 350000
#define INPUT_REPEAT_US 80000

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_READY,
    SCREEN_ACTIVE,
    SCREEN_INPUT,
    SCREEN_RESULT,
    SCREEN_BOARD,
    SCREEN_ERROR,
} app_screen_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_event_t;

// 活动页与 LVGL 定时器共享的快照。只包含 32 位字段，临界区里整体复制。
typedef struct {
    bool running;
    int32_t count;
    int64_t start_us;
    uint16_t limit_ms;
    sports_mode_t mode;
} live_t;

// 同一时刻只有一页存在；切换时先删定时器、建新页、再删旧页，避免悬空指针。
typedef struct {
    lv_obj_t *scr;
    lv_timer_t *tick;
    lv_obj_t *battery_label;
    lv_obj_t *battery_fill;
    lv_obj_t *player_chip;
    lv_obj_t *player_chip_dot;
    lv_obj_t *player_chip_label;
    lv_obj_t *list;
    lv_obj_t *rows[SPORTS_COUNT];
    lv_obj_t *row_badges[SPORTS_COUNT];
    lv_obj_t *row_badge_labels[SPORTS_COUNT];
    lv_obj_t *row_names[SPORTS_COUNT];
    lv_obj_t *row_tags[SPORTS_COUNT];
    lv_obj_t *row_arrows[SPORTS_COUNT];
    lv_obj_t *big_value;
    lv_obj_t *unit;
    lv_obj_t *status;
    lv_obj_t *bar;
    lv_obj_t *hint;
    lv_obj_t *input_value;
    lv_obj_t *input_bar;
} ui_refs_t;

static sports_data_t s_data;
static app_screen_t s_screen = SCREEN_HOME;
static int s_sport;
static int s_player = 1;
static int s_input_value;
static int s_input_hold = -1;  // -1 未按住；0 上键；1 下键
static int64_t s_input_next_repeat_us;
static int64_t s_last_activity_us;
static int64_t s_last_battery_us;
static int s_battery_soc = -1;
static bool s_dimmed;
static const char *s_error_message = "初始化失败";

static live_t s_live;
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t s_queue;
static volatile bool s_ready;

static sports_record_t s_last_rec;
static sports_add_result_t s_last_add;
static int s_last_sport;

static char s_tick_text[48];
static int s_tick_status = -1;
static bool s_bar_low;

static ui_refs_t s_ui;

static const uint32_t SPORT_COLORS[SPORTS_COUNT] = {
    C_LIME, C_ORANGE, C_SKY, C_PINK, C_GOLD, C_SKY, C_LIME, C_ORANGE, C_PINK,
};

static uint32_t player_color(int player) {
    switch (player) {
        case 2: return C_SKY;
        case 3: return C_ORANGE;
        case 4: return C_PINK;
        default: return C_LIME;
    }
}

static uint32_t rank_color(int rank) {
    switch (rank) {
        case 1: return C_GOLD;
        case 2: return C_SILVER;
        case 3: return C_BRONZE;
        default: return C_SURFACE2;
    }
}

static uint32_t rank_text_color(int rank) {
    return rank >= 1 && rank <= 3 ? C_DARK : C_TEXT;
}

static lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                              uint32_t color, int radius) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font,
                              uint32_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static void battery_refresh_locked(void) {
    if (!s_ui.battery_label || !s_ui.battery_fill) return;

    if (s_battery_soc < 0) {
        lv_label_set_text(s_ui.battery_label, "—");
        lv_obj_set_style_text_color(s_ui.battery_label, lv_color_hex(C_MUTED), 0);
        lv_obj_set_width(s_ui.battery_fill, 0);
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", s_battery_soc);
    lv_label_set_text(s_ui.battery_label, buf);
    const bool low = s_battery_soc <= 20;
    lv_obj_set_style_text_color(s_ui.battery_label,
                                lv_color_hex(low ? C_ORANGE : C_MUTED), 0);
    int width = (18 * s_battery_soc) / 100;
    if (s_battery_soc > 0 && width < 1) width = 1;
    lv_obj_set_width(s_ui.battery_fill, width);
    lv_obj_set_style_bg_color(s_ui.battery_fill,
                              lv_color_hex(low ? C_ORANGE : C_LIME), 0);
}

static void add_battery(lv_obj_t *scr) {
    lv_obj_t *body = panel_create(scr, 190, 10, 22, 11, C_BG, 3);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(C_MUTED), 0);

    s_ui.battery_fill = panel_create(body, 2, 2, 18, 7, C_LIME, 2);
    panel_create(scr, 213, 13, 2, 5, C_MUTED, 1);

    s_ui.battery_label = label_create(scr, &sports_font_16, C_MUTED, "");
    lv_obj_set_pos(s_ui.battery_label, 136, 4);
    lv_obj_set_width(s_ui.battery_label, 48);
    lv_obj_set_style_text_align(s_ui.battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    battery_refresh_locked();
}

static void player_chip_refresh_locked(void) {
    if (!s_ui.player_chip || !s_ui.player_chip_label) return;
    lv_label_set_text(s_ui.player_chip_label, sports_player_name(s_player));
    const uint32_t color = player_color(s_player);
    lv_obj_set_style_border_color(s_ui.player_chip, lv_color_hex(color), 0);
    if (s_ui.player_chip_dot) {
        lv_obj_set_style_bg_color(s_ui.player_chip_dot, lv_color_hex(color), 0);
    }
}

static void add_player_chip(lv_obj_t *scr, int x, int y) {
    lv_obj_t *chip = panel_create(scr, x, y, 92, 26, C_SURFACE2, 13);
    lv_obj_set_style_border_width(chip, 1, 0);
    s_ui.player_chip = chip;
    s_ui.player_chip_dot = panel_create(chip, 10, 9, 8, 8,
                                        player_color(s_player), 4);
    s_ui.player_chip_label = label_create(chip, &sports_font_16, C_TEXT,
                                          sports_player_name(s_player));
    lv_obj_set_pos(s_ui.player_chip_label, 24, 3);
    player_chip_refresh_locked();
}

static lv_obj_t *screen_start(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    s_ui.scr = scr;

    // 顶部 3px 荧光条是整套界面的识别元素，也避开圆角遮罩。
    panel_create(scr, 0, 0, 240, 3, C_LIME, 0);
    add_battery(scr);
    return scr;
}

static void home_apply_selection(bool animate) {
    for (int i = 0; i < SPORTS_COUNT; i++) {
        if (!s_ui.rows[i]) continue;
        const bool selected = i == s_sport;
        lv_obj_set_style_bg_color(s_ui.rows[i],
                                  lv_color_hex(selected ? C_LIME : C_SURFACE), 0);
        if (s_ui.row_names[i]) {
            lv_obj_set_style_text_color(s_ui.row_names[i],
                                        lv_color_hex(selected ? C_DARK : C_TEXT), 0);
        }
        if (s_ui.row_tags[i]) {
            lv_obj_set_style_text_color(s_ui.row_tags[i],
                                        lv_color_hex(selected ? 0x3C4A1Fu : C_MUTED), 0);
        }
        if (s_ui.row_badges[i]) {
            lv_obj_set_style_bg_color(
                s_ui.row_badges[i],
                lv_color_hex(selected ? C_DARK : SPORT_COLORS[i]), 0);
        }
        if (s_ui.row_badge_labels[i]) {
            lv_obj_set_style_text_color(
                s_ui.row_badge_labels[i],
                lv_color_hex(selected ? C_LIME : C_DARK), 0);
        }
        if (s_ui.row_arrows[i]) {
            lv_obj_set_style_text_color(s_ui.row_arrows[i],
                                        lv_color_hex(selected ? C_DARK : C_MUTED), 0);
        }
    }
    if (s_ui.list && s_ui.rows[s_sport]) {
        lv_obj_scroll_to_view(s_ui.rows[s_sport],
                              animate ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

static void build_home(void) {
    lv_obj_t *scr = screen_start();

    lv_obj_t *title = label_create(scr, &sports_font_28, C_TEXT, "运动榜");
    lv_obj_set_pos(title, 14, 5);

    lv_obj_t *brand = label_create(scr, &lv_font_montserrat_14, C_LIME, "SPORTS BOARD");
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_set_pos(brand, 14, 43);

    add_player_chip(scr, 132, 32);

    lv_obj_t *list = panel_create(scr, 12, 66, 216, 198, C_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    s_ui.list = list;

    for (int i = 0; i < SPORTS_COUNT; i++) {
        const sports_def_t *def = sports_def(i);
        lv_obj_t *row = panel_create(list, 0, i * 48, 216, 44, C_SURFACE, 12);
        s_ui.rows[i] = row;

        lv_obj_t *badge = panel_create(row, 8, 8, 28, 28, SPORT_COLORS[i], 9);
        s_ui.row_badges[i] = badge;
        lv_obj_t *badge_label = label_create(badge, &sports_font_16, C_DARK, def->badge);
        lv_obj_center(badge_label);
        s_ui.row_badge_labels[i] = badge_label;

        s_ui.row_names[i] = label_create(row, &sports_font_20, C_TEXT, def->name);
        lv_obj_set_pos(s_ui.row_names[i], 44, 0);

        s_ui.row_tags[i] = label_create(row, &sports_font_16, C_MUTED, def->tag);
        lv_obj_set_pos(s_ui.row_tags[i], 44, 22);

        s_ui.row_arrows[i] = label_create(row, &sports_font_20, C_MUTED, ">");
        lv_obj_set_pos(s_ui.row_arrows[i], 198, 8);
    }

    lv_obj_t *hint1 = label_create(scr, &sports_font_16, C_MUTED,
                                   "上/下 选择 · OK 开始");
    lv_obj_set_pos(hint1, 12, 266);
    lv_obj_t *hint2 = label_create(scr, &sports_font_16, C_MUTED,
                                   "长按OK 排行 · 长按上键换人");
    lv_obj_set_pos(hint2, 12, 288);

    lv_screen_load(scr);
    lv_obj_update_layout(scr);
    home_apply_selection(false);
}

static void build_ready(void) {
    lv_obj_t *scr = screen_start();
    const sports_def_t *def = sports_def(s_sport);

    lv_obj_t *back = label_create(scr, &sports_font_16, C_MUTED, "长按OK 返回");
    lv_obj_set_pos(back, 14, 8);

    lv_obj_t *title = label_create(scr, &sports_font_28, C_TEXT, def->name);
    lv_obj_set_pos(title, 14, 30);

    lv_obj_t *tag = label_create(scr, &sports_font_16, C_LIME, def->tag);
    lv_obj_set_pos(tag, 14, 72);

    lv_obj_t *player_card = panel_create(scr, 14, 96, 212, 56, C_SURFACE, 16);
    lv_obj_t *circle = panel_create(player_card, 10, 9, 38, 38,
                                    player_color(s_player), 19);
    char number[12];
    snprintf(number, sizeof(number), "%d", s_player);
    lv_obj_t *number_label = label_create(circle, &lv_font_montserrat_20,
                                          C_DARK, number);
    lv_obj_center(number_label);

    lv_obj_t *player_name = label_create(player_card, &sports_font_20, C_TEXT,
                                         sports_player_name(s_player));
    lv_obj_set_pos(player_name, 58, 6);

    char pb_text[48];
    const sports_record_t *pb = sports_pb_at(&s_data, s_sport, s_player);
    if (pb) {
        char value[32];
        sports_format_result(def, pb->value, value, sizeof(value));
        snprintf(pb_text, sizeof(pb_text), "个人最佳 %s", value);
    } else {
        snprintf(pb_text, sizeof(pb_text), "个人最佳 —");
    }
    lv_obj_t *pb_label = label_create(player_card, &sports_font_16, C_MUTED, pb_text);
    lv_obj_set_pos(pb_label, 58, 30);

    lv_obj_t *record_card = panel_create(scr, 14, 158, 212, 46, C_SURFACE, 14);
    lv_obj_t *record_caption = label_create(record_card, &sports_font_16,
                                            C_MUTED, "本机纪录");
    lv_obj_set_pos(record_caption, 10, 2);

    char record_text[64];
    const sports_record_t *top = sports_top_at(&s_data, s_sport, 1);
    if (top) {
        char value[32];
        sports_format_result(def, top->value, value, sizeof(value));
        snprintf(record_text, sizeof(record_text), "%s · %s", value,
                 sports_player_short(top->player));
    } else {
        snprintf(record_text, sizeof(record_text), "暂无纪录");
    }
    lv_obj_t *record_value = label_create(record_card, &sports_font_20, C_TEXT,
                                          record_text);
    lv_obj_set_pos(record_value, 10, 21);

    const char *rules;
    if (def->mode == SPORTS_MODE_DISTANCE) {
        rules = "上/下 调整成绩\n按 OK 保存";
    } else if (def->mode == SPORTS_MODE_TIME) {
        rules = "按 OK 开始计时\n再按 OK 停止";
    } else if (def->limit_ms) {
        rules = "60秒内按 OK 计数\n时间到自动结算";
    } else {
        rules = "按 OK 计数\n按上键结束并结算";
    }
    lv_obj_t *rules_label = label_create(scr, &sports_font_16, C_MUTED, rules);
    lv_obj_set_pos(rules_label, 14, 208);
    lv_obj_set_size(rules_label, 212, 44);
    lv_label_set_long_mode(rules_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(rules_label, 0, 0);

    lv_obj_t *action = panel_create(scr, 14, 256, 212, 38, C_LIME, 19);
    lv_obj_t *action_label = label_create(
        action, &sports_font_20, C_DARK,
        def->mode == SPORTS_MODE_DISTANCE ? "按 OK 录入" : "按 OK 开始");
    lv_obj_center(action_label);

    lv_obj_t *footer = label_create(scr, &sports_font_16, C_MUTED,
                                    "上/下 换选手 · 长按OK 返回");
    lv_obj_set_pos(footer, 14, 298);

    lv_screen_load(scr);
}

static void active_tick(lv_timer_t *timer) {
    (void)timer;
    live_t snap;
    portENTER_CRITICAL(&s_live_mux);
    snap = s_live;
    portEXIT_CRITICAL(&s_live_mux);

    const sports_def_t *def = sports_def(s_sport);
    const int64_t now = esp_timer_get_time();
    const int32_t elapsed = snap.running ? sports_elapsed_ms(snap.start_us, now) : 0;
    char buf[40];

    if (def->mode == SPORTS_MODE_TIME) {
        sports_format_clock(elapsed, buf, sizeof(buf));
        if (strcmp(buf, s_tick_text) != 0) {
            snprintf(s_tick_text, sizeof(s_tick_text), "%s", buf);
            if (s_ui.big_value) lv_label_set_text(s_ui.big_value, buf);
        }
        const int running = snap.running ? 1 : 0;
        if (running != s_tick_status) {
            s_tick_status = running;
            if (s_ui.status) {
                lv_label_set_text(s_ui.status, running ? "计时中" : "按 OK 开始");
            }
        }
        return;
    }

    if (!s_ui.status) return;

    if (snap.limit_ms) {
        const int seconds = snap.running
            ? sports_remaining_seconds(snap.limit_ms, snap.start_us, now)
            : (snap.limit_ms + 999) / 1000;
        if (!snap.running && snap.count == 0) {
            snprintf(buf, sizeof(buf), "按 OK 开始计数");
        } else {
            snprintf(buf, sizeof(buf), "剩余 %d 秒", seconds);
        }
        if (strcmp(buf, s_tick_text) != 0) {
            snprintf(s_tick_text, sizeof(s_tick_text), "%s", buf);
            lv_label_set_text(s_ui.status, buf);
        }

        int32_t remaining = snap.limit_ms - elapsed;
        if (!snap.running) remaining = snap.limit_ms;
        if (remaining < 0) remaining = 0;
        if (s_ui.bar) lv_bar_set_value(s_ui.bar, remaining, LV_ANIM_OFF);

        const bool low = seconds <= 10;
        if (low != s_bar_low) {
            s_bar_low = low;
            if (s_ui.bar) {
                lv_obj_set_style_bg_color(s_ui.bar,
                    lv_color_hex(low ? C_ORANGE : C_LIME), LV_PART_INDICATOR);
            }
            lv_obj_set_style_text_color(s_ui.status,
                lv_color_hex(low ? C_ORANGE : C_MUTED), 0);
        }
    } else {
        char clock[24];
        sports_format_clock(elapsed, clock, sizeof(clock));
        snprintf(buf, sizeof(buf), "用时 %s", clock);
        if (strcmp(buf, s_tick_text) != 0) {
            snprintf(s_tick_text, sizeof(s_tick_text), "%s", buf);
            lv_label_set_text(s_ui.status, buf);
        }
    }
}

static void build_active(void) {
    lv_obj_t *scr = screen_start();
    const sports_def_t *def = sports_def(s_sport);

    lv_obj_t *event = label_create(scr, &sports_font_20, C_TEXT, def->name);
    lv_obj_set_pos(event, 14, 6);

    lv_obj_t *who = label_create(scr, &sports_font_16, C_MUTED,
                                 sports_player_name(s_player));
    lv_obj_set_pos(who, 14, 30);

    lv_obj_t *card = panel_create(scr, 12, 60, 216, 176, C_SURFACE, 20);

    s_ui.big_value = label_create(card, &lv_font_montserrat_40, C_TEXT,
                                  def->mode == SPORTS_MODE_TIME ? "0:00.00" : "0");
    lv_obj_set_pos(s_ui.big_value, 0, 20);
    lv_obj_set_size(s_ui.big_value, 216, 56);
    lv_obj_set_style_text_align(s_ui.big_value, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.unit = label_create(card, &sports_font_20, C_MUTED,
                             def->mode == SPORTS_MODE_TIME ? "秒" : "次");
    lv_obj_set_pos(s_ui.unit, 0, 80);
    lv_obj_set_size(s_ui.unit, 216, 26);
    lv_obj_set_style_text_align(s_ui.unit, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.status = label_create(card, &sports_font_16, C_MUTED,
                               def->mode == SPORTS_MODE_TIME
                                   ? "按 OK 开始"
                                   : "按 OK 开始计数");
    lv_obj_set_pos(s_ui.status, 0, 108);
    lv_obj_set_size(s_ui.status, 216, 24);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER, 0);

    if (def->mode == SPORTS_MODE_COUNT && def->limit_ms) {
        s_ui.bar = lv_bar_create(card);
        lv_obj_set_pos(s_ui.bar, 20, 142);
        lv_obj_set_size(s_ui.bar, 176, 8);
        lv_bar_set_range(s_ui.bar, 0, def->limit_ms);
        lv_bar_set_value(s_ui.bar, def->limit_ms, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_ui.bar, lv_color_hex(C_SURFACE2), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_ui.bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_ui.bar, 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_ui.bar, 0, 0);
        lv_obj_set_style_bg_color(s_ui.bar, lv_color_hex(C_LIME), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_ui.bar, 4, LV_PART_INDICATOR);
    }

    const char *hint_text;
    if (def->mode == SPORTS_MODE_TIME) {
        hint_text = "OK 开始 · 上键 结束";
    } else {
        hint_text = "OK 计数 · 上键 结束";
    }
    s_ui.hint = label_create(scr, &sports_font_16, C_MUTED, hint_text);
    lv_obj_set_pos(s_ui.hint, 0, 266);
    lv_obj_set_size(s_ui.hint, 240, 24);
    lv_obj_set_style_text_align(s_ui.hint, LV_TEXT_ALIGN_CENTER, 0);

    s_tick_text[0] = '\0';
    s_tick_status = -1;
    s_bar_low = false;
    s_ui.tick = lv_timer_create(active_tick, 100, NULL);
    lv_screen_load(scr);
}

static void input_refresh_locked(void) {
    if (!s_ui.input_value) return;
    lv_label_set_text_fmt(s_ui.input_value, "%d", s_input_value);
    if (s_ui.input_bar) {
        const sports_def_t *def = sports_def(s_sport);
        const int span = def->max_value - def->min_value;
        if (span > 0) {
            lv_bar_set_value(s_ui.input_bar, s_input_value - def->min_value,
                             LV_ANIM_OFF);
        }
    }
}

static void build_input(void) {
    lv_obj_t *scr = screen_start();
    const sports_def_t *def = sports_def(s_sport);

    lv_obj_t *back = label_create(scr, &sports_font_16, C_MUTED, "长按OK 取消");
    lv_obj_set_pos(back, 14, 8);

    lv_obj_t *event = label_create(scr, &sports_font_20, C_TEXT, def->name);
    lv_obj_set_pos(event, 14, 32);

    lv_obj_t *card = panel_create(scr, 12, 76, 216, 150, C_SURFACE, 20);

    s_ui.input_value = label_create(card, &lv_font_montserrat_40, C_TEXT, "0");
    lv_obj_set_pos(s_ui.input_value, 0, 16);
    lv_obj_set_size(s_ui.input_value, 216, 52);
    lv_obj_set_style_text_align(s_ui.input_value, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *unit = label_create(card, &sports_font_20, C_MUTED, "厘米");
    lv_obj_set_pos(unit, 0, 74);
    lv_obj_set_size(unit, 216, 26);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.input_bar = lv_bar_create(card);
    lv_obj_set_pos(s_ui.input_bar, 20, 102);
    lv_obj_set_size(s_ui.input_bar, 176, 8);
    lv_bar_set_range(s_ui.input_bar, 0, def->max_value - def->min_value);
    lv_obj_set_style_bg_color(s_ui.input_bar, lv_color_hex(C_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.input_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.input_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.input_bar, 0, 0);
    lv_obj_set_style_bg_color(s_ui.input_bar, lv_color_hex(C_LIME), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.input_bar, 4, LV_PART_INDICATOR);

    char range[16];
    snprintf(range, sizeof(range), "%d", def->min_value);
    lv_obj_t *min_label = label_create(card, &sports_font_16, C_MUTED, range);
    lv_obj_set_pos(min_label, 20, 114);
    snprintf(range, sizeof(range), "%d", def->max_value);
    lv_obj_t *max_label = label_create(card, &sports_font_16, C_MUTED, range);
    lv_obj_set_pos(max_label, 140, 114);
    lv_obj_set_size(max_label, 56, 22);
    lv_obj_set_style_text_align(max_label, LV_TEXT_ALIGN_RIGHT, 0);

    char pb_text[48];
    const sports_record_t *pb = sports_pb_at(&s_data, s_sport, s_player);
    if (pb) {
        char value[32];
        sports_format_result(def, pb->value, value, sizeof(value));
        snprintf(pb_text, sizeof(pb_text), "个人最佳 %s", value);
    } else {
        snprintf(pb_text, sizeof(pb_text), "个人最佳 —");
    }
    lv_obj_t *pb_card = panel_create(scr, 14, 234, 212, 36, C_SURFACE, 12);
    lv_obj_t *pb_label = label_create(pb_card, &sports_font_16, C_MUTED, pb_text);
    lv_obj_set_pos(pb_label, 10, 7);

    lv_obj_t *hint = label_create(scr, &sports_font_16, C_MUTED,
                                  "上/下 调整 · OK 保存");
    lv_obj_set_pos(hint, 0, 288);
    lv_obj_set_size(hint, 240, 24);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    input_refresh_locked();
    lv_screen_load(scr);
}

static void build_result(void) {
    lv_obj_t *scr = screen_start();
    const sports_def_t *def = sports_def(s_last_sport);

    lv_obj_t *micro = label_create(scr, &lv_font_montserrat_14, C_LIME, "RESULT");
    lv_obj_set_style_text_letter_space(micro, 2, 0);
    lv_obj_set_pos(micro, 14, 8);

    lv_obj_t *event = label_create(scr, &sports_font_20, C_TEXT, def->name);
    lv_obj_set_pos(event, 14, 28);

    char value[32];
    sports_format_value_only(def, s_last_rec.value, value, sizeof(value));
    lv_obj_t *big = label_create(scr, &lv_font_montserrat_40, C_TEXT, value);
    lv_obj_set_pos(big, 0, 52);
    lv_obj_set_size(big, 240, 52);
    lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);

    const char *unit_text = "次";
    if (def->mode == SPORTS_MODE_TIME) unit_text = "秒";
    if (def->mode == SPORTS_MODE_DISTANCE) unit_text = "厘米";
    lv_obj_t *unit = label_create(scr, &sports_font_20, C_MUTED, unit_text);
    lv_obj_set_pos(unit, 0, 104);
    lv_obj_set_size(unit, 240, 26);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);

    char rank_text[24];
    uint32_t pill_color = C_SURFACE2;
    uint32_t pill_text_color = C_TEXT;
    if (s_last_add.ranked) {
        snprintf(rank_text, sizeof(rank_text), "第%d名", s_last_add.rank);
        pill_color = rank_color(s_last_add.rank);
        pill_text_color = rank_text_color(s_last_add.rank);
    } else {
        snprintf(rank_text, sizeof(rank_text), "未上榜");
    }
    lv_obj_t *rank_pill = panel_create(scr, 14, 132, 100, 28, pill_color, 14);
    lv_obj_t *rank_label = label_create(rank_pill, &sports_font_16,
                                        pill_text_color, rank_text);
    lv_obj_center(rank_label);

    if (s_last_add.is_pb) {
        lv_obj_t *pb_pill = panel_create(scr, 122, 132, 104, 28, C_LIME, 14);
        lv_obj_t *pb_label = label_create(
            pb_pill, &sports_font_16, C_DARK,
            s_last_add.rank == 1 ? "新纪录" : "个人最佳");
        lv_obj_center(pb_label);
    }

    lv_obj_t *caption = label_create(scr, &sports_font_16, C_MUTED, "本机前三");
    lv_obj_set_pos(caption, 14, 168);

    for (int i = 0; i < 3; i++) {
        const sports_record_t *rec = sports_top_at(&s_data, s_last_sport, i + 1);
        if (!rec) break;
        const bool highlight = s_last_add.ranked && (i + 1 == s_last_add.rank);
        lv_obj_t *row = panel_create(scr, 14, 192 + i * 26, 212, 24,
                                     highlight ? C_SURFACE2 : C_SURFACE, 8);
        if (highlight) {
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(C_LIME), 0);
        }
        char rank_buf[12];
        snprintf(rank_buf, sizeof(rank_buf), "%d", i + 1);
        lv_obj_t *rank = label_create(row, &sports_font_16,
                                      rank_text_color(i + 1), rank_buf);
        lv_obj_set_pos(rank, 8, 1);
        lv_obj_t *who = label_create(row, &sports_font_16, C_TEXT,
                                     sports_player_short(rec->player));
        lv_obj_set_pos(who, 30, 1);
        char result_buf[32];
        sports_format_result(def, rec->value, result_buf, sizeof(result_buf));
        lv_obj_t *result = label_create(row, &sports_font_16, C_TEXT, result_buf);
        lv_obj_set_pos(result, 94, 1);
        lv_obj_set_size(result, 110, 22);
        lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *hint1 = label_create(scr, &sports_font_16, C_MUTED,
                                   "OK 再来一次 · 上键 排行榜");
    lv_obj_set_pos(hint1, 14, 276);
    lv_obj_t *hint2 = label_create(scr, &sports_font_16, C_MUTED,
                                   "长按OK 返回首页");
    lv_obj_set_pos(hint2, 14, 298);

    lv_screen_load(scr);
}

static void build_board(void) {
    lv_obj_t *scr = screen_start();

    lv_obj_t *title = label_create(scr, &sports_font_28, C_TEXT, "排行榜");
    lv_obj_set_pos(title, 14, 4);

    lv_obj_t *micro = label_create(scr, &lv_font_montserrat_14, C_LIME, "TOP 5");
    lv_obj_set_style_text_letter_space(micro, 2, 0);
    lv_obj_set_pos(micro, 14, 42);

    lv_obj_t *selector = panel_create(scr, 12, 62, 216, 34, C_SURFACE, 12);
    lv_obj_t *event = label_create(selector, &sports_font_20, C_TEXT,
                                   sports_def(s_sport)->name);
    lv_obj_center(event);

    bool empty = sports_top_at(&s_data, s_sport, 1) == NULL;
    if (empty) {
        lv_obj_t *none = label_create(scr, &sports_font_20, C_TEXT, "还没有成绩");
        lv_obj_set_pos(none, 0, 140);
        lv_obj_set_size(none, 240, 28);
        lv_obj_set_style_text_align(none, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *sub = label_create(scr, &sports_font_16, C_MUTED,
                                     "第一个纪录等你创造");
        lv_obj_set_pos(sub, 0, 172);
        lv_obj_set_size(sub, 240, 24);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *action = panel_create(scr, 14, 210, 212, 38, C_LIME, 19);
        lv_obj_t *action_label = label_create(action, &sports_font_20, C_DARK,
                                              "OK 去挑战");
        lv_obj_center(action_label);
    } else {
        for (int i = 0; i < SPORTS_TOP_N; i++) {
            const sports_record_t *rec = sports_top_at(&s_data, s_sport, i + 1);
            if (!rec) break;
            const bool mine = rec->player == (uint8_t)s_player;
            lv_obj_t *row = panel_create(scr, 12, 102 + i * 34, 216, 30,
                                         mine ? C_SURFACE2 : C_SURFACE, 9);
            lv_obj_t *medal = panel_create(row, 5, 4, 22, 22,
                                           rank_color(i + 1), 7);
            char rank_buf[12];
            snprintf(rank_buf, sizeof(rank_buf), "%d", i + 1);
            lv_obj_t *rank = label_create(medal, &lv_font_montserrat_14,
                                          rank_text_color(i + 1), rank_buf);
            lv_obj_center(rank);

            panel_create(row, 34, 12, 6, 6, player_color(rec->player), 3);
            lv_obj_t *who = label_create(row, &sports_font_16, C_TEXT,
                                         sports_player_short(rec->player));
            lv_obj_set_pos(who, 44, 3);

            char result_buf[32];
            sports_format_result(sports_def(s_sport), rec->value,
                                 result_buf, sizeof(result_buf));
            lv_obj_t *result = label_create(row, &sports_font_16, C_TEXT,
                                            result_buf);
            lv_obj_set_pos(result, 100, 3);
            lv_obj_set_size(result, 106, 22);
            lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    lv_obj_t *hint1 = label_create(scr, &sports_font_16, C_MUTED,
                                   "上/下 切换项目 · OK 去挑战");
    lv_obj_set_pos(hint1, 14, 276);
    lv_obj_t *hint2 = label_create(scr, &sports_font_16, C_MUTED,
                                   "长按OK 返回");
    lv_obj_set_pos(hint2, 14, 298);

    lv_screen_load(scr);
}

static void build_error(void) {
    lv_obj_t *scr = screen_start();

    lv_obj_t *title = label_create(scr, &sports_font_28, C_ORANGE, "初始化失败");
    lv_obj_set_pos(title, 14, 56);

    lv_obj_t *panel = panel_create(scr, 14, 112, 212, 76, C_SURFACE, 14);
    lv_obj_t *message = label_create(panel, &sports_font_16, C_TEXT,
                                     s_error_message);
    lv_obj_set_pos(message, 12, 10);
    lv_obj_set_size(message, 188, 56);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = label_create(scr, &sports_font_16, C_MUTED,
                                  "请重启设备");
    lv_obj_set_pos(hint, 14, 208);

    lv_screen_load(scr);
}

static bool app_goto(app_screen_t next) {
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 锁定超时，保留当前页面");
        return false;
    }

    if (s_ui.tick) {
        lv_timer_delete(s_ui.tick);
        s_ui.tick = NULL;
    }
    lv_obj_t *old = s_ui.scr;
    memset(&s_ui, 0, sizeof(s_ui));
    s_screen = next;
    s_input_hold = -1;

    switch (next) {
        case SCREEN_HOME: build_home(); break;
        case SCREEN_READY: build_ready(); break;
        case SCREEN_ACTIVE: build_active(); break;
        case SCREEN_INPUT: build_input(); break;
        case SCREEN_RESULT: build_result(); break;
        case SCREEN_BOARD: build_board(); break;
        case SCREEN_ERROR: build_error(); break;
    }

    if (old) lv_obj_delete(old);
    bsp_lvgl_unlock();

    s_last_activity_us = esp_timer_get_time();
    if (next == SCREEN_ACTIVE || next == SCREEN_INPUT) {
        if (s_dimmed) {
            bsp_display_backlight(100);
            s_dimmed = false;
        }
    }
    return true;
}

static void cycle_player(int delta) {
    s_player += delta;
    if (s_player < 1) s_player = SPORTS_PLAYER_COUNT;
    if (s_player > SPORTS_PLAYER_COUNT) s_player = 1;
    s_data.selected_player = (uint8_t)s_player;
    const esp_err_t err = sports_store_save(&s_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "保存选手选择失败: %s", esp_err_to_name(err));
    }
}

static void record_result(int sport, int player, int32_t value, int32_t aux) {
    const sports_def_t *def = sports_def(sport);
    if (!sports_value_in_range(def, value)) {
        ESP_LOGW(TAG, "拒绝越界成绩: %ld", (long)value);
        app_goto(SCREEN_READY);
        return;
    }

    s_last_add = sports_add_result(&s_data, sport, player, value, aux);
    const esp_err_t err = sports_store_save(&s_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "成绩已显示但保存失败: %s", esp_err_to_name(err));
    }

    s_last_sport = sport;
    s_sport = sport;
    s_player = player;
    s_last_rec = (sports_record_t){
        .valid = 1,
        .player = (uint8_t)player,
        .value = value,
        .aux = aux,
    };
    app_goto(SCREEN_RESULT);
}

static void start_active(void) {
    const sports_def_t *def = sports_def(s_sport);
    portENTER_CRITICAL(&s_live_mux);
    s_live.running = false;
    s_live.count = 0;
    s_live.start_us = 0;
    s_live.limit_ms = def->limit_ms;
    s_live.mode = def->mode;
    portEXIT_CRITICAL(&s_live_mux);
    app_goto(SCREEN_ACTIVE);
}

static void start_input(void) {
    const sports_def_t *def = sports_def(s_sport);
    const sports_record_t *pb = sports_pb_at(&s_data, s_sport, s_player);
    int value = pb ? pb->value : (s_sport == SPORTS_LONG_JUMP ? 150 : 0);
    if (value < def->min_value) value = def->min_value;
    if (value > def->max_value) value = def->max_value;
    s_input_value = value;
    s_input_hold = -1;
    app_goto(SCREEN_INPUT);
}

static void start_selected(void) {
    if (sports_def(s_sport)->mode == SPORTS_MODE_DISTANCE) {
        start_input();
    } else {
        start_active();
    }
}

static void count_press(void) {
    int32_t count;
    portENTER_CRITICAL(&s_live_mux);
    if (!s_live.running) {
        s_live.running = true;
        s_live.start_us = esp_timer_get_time();
    }
    if (s_live.count < SPORTS_MAX_COUNT) s_live.count++;
    count = s_live.count;
    portEXIT_CRITICAL(&s_live_mux);

    if (s_ui.big_value && bsp_lvgl_lock(300)) {
        lv_label_set_text_fmt(s_ui.big_value, "%ld", (long)count);
        bsp_lvgl_unlock();
    }
}

static void finish_count(void) {
    live_t snap;
    portENTER_CRITICAL(&s_live_mux);
    snap = s_live;
    s_live.running = false;
    portEXIT_CRITICAL(&s_live_mux);

    if (snap.count <= 0) {
        app_goto(SCREEN_READY);
        return;
    }
    int32_t elapsed = snap.running
        ? sports_elapsed_ms(snap.start_us, esp_timer_get_time())
        : 0;
    if (snap.limit_ms && elapsed > (int32_t)snap.limit_ms) {
        elapsed = (int32_t)snap.limit_ms;
    }
    record_result(s_sport, s_player, snap.count, elapsed);
}

static void start_time(void) {
    portENTER_CRITICAL(&s_live_mux);
    s_live.running = true;
    s_live.start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_live_mux);

    if (s_ui.hint && bsp_lvgl_lock(300)) {
        lv_label_set_text(s_ui.hint, "OK 停止 · 上键 结束");
        bsp_lvgl_unlock();
    }
}

static void stop_time(void) {
    live_t snap;
    portENTER_CRITICAL(&s_live_mux);
    snap = s_live;
    portEXIT_CRITICAL(&s_live_mux);
    if (!snap.running) return;

    const int32_t elapsed = sports_elapsed_ms(snap.start_us, esp_timer_get_time());
    if (elapsed < 300) return;  // 防抖：太短的一次点击不产生无效成绩

    portENTER_CRITICAL(&s_live_mux);
    s_live.running = false;
    portEXIT_CRITICAL(&s_live_mux);

    const int32_t centis = elapsed / 10;
    record_result(s_sport, s_player, centis, 0);
}

static void adjust_input(int delta) {
    const sports_def_t *def = sports_def(s_sport);
    int value = s_input_value + delta;
    if (value < def->min_value) value = def->min_value;
    if (value > def->max_value) value = def->max_value;
    if (value == s_input_value) return;
    s_input_value = value;

    if (bsp_lvgl_lock(300)) {
        input_refresh_locked();
        bsp_lvgl_unlock();
    }
}

static void handle_home(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_sport += btn == BSP_BTN_UP ? -1 : 1;
            if (s_sport < 0) s_sport = SPORTS_COUNT - 1;
            if (s_sport >= SPORTS_COUNT) s_sport = 0;
            if (bsp_lvgl_lock(500)) {
                home_apply_selection(true);
                bsp_lvgl_unlock();
            }
        } else if (btn == BSP_BTN_OK) {
            start_selected();
        }
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) {
            app_goto(SCREEN_BOARD);
        } else if (btn == BSP_BTN_UP) {
            cycle_player(1);
            if (bsp_lvgl_lock(300)) {
                player_chip_refresh_locked();
                bsp_lvgl_unlock();
            }
        }
    }
}

static void handle_ready(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) {
            cycle_player(-1);
            app_goto(SCREEN_READY);
        } else if (btn == BSP_BTN_DOWN) {
            cycle_player(1);
            app_goto(SCREEN_READY);
        } else if (btn == BSP_BTN_OK) {
            start_selected();
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        app_goto(SCREEN_HOME);
    }
}

static void handle_active(bsp_btn_t btn, bsp_btn_ev_t ev) {
    const sports_def_t *def = sports_def(s_sport);
    if (def->mode == SPORTS_MODE_COUNT) {
        if (ev == BSP_BTN_PRESS && btn == BSP_BTN_OK) {
            count_press();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            finish_count();
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        live_t snap;
        portENTER_CRITICAL(&s_live_mux);
        snap = s_live;
        portEXIT_CRITICAL(&s_live_mux);
        if (snap.running) {
            stop_time();
        } else {
            start_time();
        }
    } else if (btn == BSP_BTN_UP) {
        live_t snap;
        portENTER_CRITICAL(&s_live_mux);
        snap = s_live;
        portEXIT_CRITICAL(&s_live_mux);
        if (snap.running) {
            stop_time();
        } else {
            app_goto(SCREEN_READY);
        }
    }
}

static void handle_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_PRESS && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        adjust_input(btn == BSP_BTN_UP ? 1 : -1);
        s_input_hold = btn == BSP_BTN_UP ? 0 : 1;
        s_input_next_repeat_us = esp_timer_get_time() + INPUT_REPEAT_FIRST_US;
    } else if (ev == BSP_BTN_RELEASE) {
        if ((btn == BSP_BTN_UP && s_input_hold == 0) ||
            (btn == BSP_BTN_DOWN && s_input_hold == 1)) {
            s_input_hold = -1;
        }
    } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        s_input_hold = -1;
        record_result(s_sport, s_player, s_input_value, 0);
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_input_hold = -1;
        app_goto(SCREEN_HOME);
    }
}

static void handle_result(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_OK) {
            app_goto(SCREEN_READY);
        } else if (btn == BSP_BTN_UP) {
            app_goto(SCREEN_BOARD);
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        app_goto(SCREEN_HOME);
    }
}

static void handle_board(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_sport += btn == BSP_BTN_UP ? -1 : 1;
            if (s_sport < 0) s_sport = SPORTS_COUNT - 1;
            if (s_sport >= SPORTS_COUNT) s_sport = 0;
            app_goto(SCREEN_BOARD);
        } else if (btn == BSP_BTN_OK) {
            app_goto(SCREEN_READY);
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        app_goto(SCREEN_HOME);
    }
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    switch (s_screen) {
        case SCREEN_HOME: handle_home(btn, ev); break;
        case SCREEN_READY: handle_ready(btn, ev); break;
        case SCREEN_ACTIVE: handle_active(btn, ev); break;
        case SCREEN_INPUT: handle_input(btn, ev); break;
        case SCREEN_RESULT: handle_result(btn, ev); break;
        case SCREEN_BOARD: handle_board(btn, ev); break;
        case SCREEN_ERROR: break;
    }
}

static void battery_check(int64_t now) {
    if (s_screen == SCREEN_ERROR) return;
    if (now - s_last_battery_us < BATTERY_REFRESH_US) return;
    s_last_battery_us = now;

    int soc = bsp_battery_soc();
    if (soc > 100) soc = 100;
    if (soc < -1) soc = -1;
    if (soc == s_battery_soc) return;
    s_battery_soc = soc;

    if (bsp_lvgl_lock(200)) {
        battery_refresh_locked();
        bsp_lvgl_unlock();
    }
}

static void idle_check(int64_t now) {
    if (s_dimmed) return;
    if (s_screen != SCREEN_HOME && s_screen != SCREEN_READY &&
        s_screen != SCREEN_RESULT && s_screen != SCREEN_BOARD) {
        return;
    }
    if (now - s_last_activity_us < IDLE_DIM_US) return;
    bsp_display_backlight(15);
    s_dimmed = true;
}

static void app_tick(int64_t now) {
    if (s_screen == SCREEN_ACTIVE) {
        const sports_def_t *def = sports_def(s_sport);
        if (def->mode == SPORTS_MODE_COUNT && def->limit_ms) {
            live_t snap;
            portENTER_CRITICAL(&s_live_mux);
            snap = s_live;
            portEXIT_CRITICAL(&s_live_mux);
            if (snap.running &&
                sports_elapsed_ms(snap.start_us, now) >= (int32_t)def->limit_ms) {
                finish_count();
                return;
            }
        }
    }

    if (s_screen == SCREEN_INPUT && s_input_hold >= 0 &&
        now >= s_input_next_repeat_us) {
        adjust_input(s_input_hold == 0 ? 1 : -1);
        s_input_next_repeat_us = now + INPUT_REPEAT_US;
    }

    battery_check(now);
    idle_check(now);
}

static void app_task(void *arg) {
    (void)arg;
    for (;;) {
        key_event_t event;
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            const int64_t now = esp_timer_get_time();
            s_last_activity_us = now;
            if (s_dimmed) {
                bsp_display_backlight(100);
                s_dimmed = false;
            }
            handle_key(event.btn, event.ev);
        }
        app_tick(esp_timer_get_time());
    }
}

bool sports_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_ready || !s_queue || ev == BSP_BTN_DOUBLE) return false;
    const key_event_t event = { .btn = btn, .ev = ev };
    return xQueueSend(s_queue, &event, 0) == pdTRUE;
}

esp_err_t sports_app_start(void) {
    esp_err_t err = sports_store_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用，本次成绩只在内存中: %s", esp_err_to_name(err));
    }

    sports_data_init(&s_data);
    err = sports_store_load(&s_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取榜单失败，使用空榜: %s", esp_err_to_name(err));
    }
    sports_data_sanitize(&s_data);
    s_player = s_data.selected_player;
    s_sport = 0;

    if (bsp_battery_init() == ESP_OK) {
        s_battery_soc = bsp_battery_soc();
    } else {
        ESP_LOGW(TAG, "电量计不可用，界面显示 —");
        s_battery_soc = -1;
    }

    s_last_activity_us = esp_timer_get_time();
    s_last_battery_us = s_last_activity_us;

    s_queue = xQueueCreate(32, sizeof(key_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (!app_goto(SCREEN_HOME)) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_TIMEOUT;
    }

    if (xTaskCreate(app_task, "sports_app", 6144, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "运动榜就绪，共 %d 个项目", SPORTS_COUNT);
    return ESP_OK;
}

void sports_app_show_error(const char *message) {
    if (message && message[0]) s_error_message = message;
    (void)app_goto(SCREEN_ERROR);
}
