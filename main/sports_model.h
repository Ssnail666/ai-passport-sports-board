// main/sports_model.h —— 「运动榜」的纯逻辑层。
//
// 这里只放项目定义、成绩排序/个人最佳、持久化编解码与显示格式化，不依赖
// ESP-IDF 或 LVGL，可在主机上直接编译测试。屏幕布局、按键分发和 NVS 访问
// 分别属于 sports_app.c 与 sports_store.c。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 项目顺序即首页列表顺序。新增项目时同时补齐 sports_def_table 与 UI 颜色表。
typedef enum {
    SPORTS_JUMP_ROPE = 0,
    SPORTS_SIT_UP,
    SPORTS_PUSH_UP,
    SPORTS_PULL_UP,
    SPORTS_LONG_JUMP,
    SPORTS_SIT_REACH,
    SPORTS_RUN_50M,
    SPORTS_RUN_800M,
    SPORTS_RUN_1000M,
    SPORTS_COUNT,
} sports_id_t;

#define SPORTS_PLAYER_COUNT 4
#define SPORTS_TOP_N 5

// 计数上限与时长上限用于丢弃损坏的 NVS 数据，也避免 40px 数字把屏幕撑破。
#define SPORTS_MAX_COUNT 99999
#define SPORTS_MAX_TIME_CS 360000  // 1 小时，单位 10ms
#define SPORTS_MAX_AUX_MS 3600000  // 计数项目的用时，单位 ms

// 持久化 blob 固定长度：头 8 + 前 5 名 9*5*12 + 个人最佳 9*4*12 + 校验 4。
#define SPORTS_BLOB_SIZE (8u + (SPORTS_COUNT * SPORTS_TOP_N * 12u) + \
                          (SPORTS_COUNT * SPORTS_PLAYER_COUNT * 12u) + 4u)

typedef enum {
    SPORTS_MODE_COUNT = 0,  // 手动计数，可选 60 秒限时
    SPORTS_MODE_TIME,       // 只计时，成绩越小越好
    SPORTS_MODE_DISTANCE,   // 手动录入厘米数，成绩越大越好
} sports_mode_t;

typedef struct {
    const char *name;      // 完整项目名，例如「跳绳」
    const char *badge;     // 列表左侧短标记，例如「绳」「800」
    const char *tag;       // 模式说明，例如「60秒计数」
    sports_mode_t mode;
    uint16_t limit_ms;     // 计数项目自动结算时长；0 表示不限时
    int16_t min_value;     // 距离项目可录入的最小厘米数
    int16_t max_value;     // 距离项目可录入的最大厘米数
} sports_def_t;

// valid=0 的记录不参与排序；player 使用 1..4，显示时由 sports_player_name 转换。
typedef struct {
    uint8_t valid;
    uint8_t player;
    int32_t value;  // 计数=次数；计时=10ms；距离=厘米
    int32_t aux;    // 计数项目的用时(ms)，用于同次数时短者优先；其余项目为 0
} sports_record_t;

typedef struct {
    uint8_t selected_player;
    sports_record_t top[SPORTS_COUNT][SPORTS_TOP_N];
    sports_record_t pb[SPORTS_COUNT][SPORTS_PLAYER_COUNT];
} sports_data_t;

typedef struct {
    bool ranked;   // 是否进入前 SPORTS_TOP_N
    bool is_pb;    // 是否刷新该选手的个人最佳
    uint8_t rank;  // 1..SPORTS_TOP_N；未上榜为 0
} sports_add_result_t;

const sports_def_t *sports_def(int index);
int sports_clamp_index(int index);

const char *sports_player_name(int player);  // 「1号选手」
const char *sports_player_short(int player); // 「1号」

bool sports_mode_is_valid(sports_mode_t mode);
bool sports_value_in_range(const sports_def_t *def, int32_t value);

// 返回 <0 表示 a 名次更靠前，0 表示完全并列，>0 表示 a 更靠后。
int sports_compare_records(const sports_def_t *def, const sports_record_t *a,
                           const sports_record_t *b);

void sports_data_init(sports_data_t *data);
// 修复排序、丢弃越界记录，并从排行榜反推缺失的个人最佳。幂等，可安全重复调用。
void sports_data_sanitize(sports_data_t *data);

const sports_record_t *sports_top_at(const sports_data_t *data, int sport, int rank);
const sports_record_t *sports_pb_at(const sports_data_t *data, int sport, int player);

// 写入一条成绩。无效输入返回全 false 且不改动 data；并列成绩保留先到者在前。
sports_add_result_t sports_add_result(sports_data_t *data, int sport, int player,
                                      int32_t value, int32_t aux);

// 固定长度、显式小端编码，带 FNV-1a 校验。失败返回 0 或 false。
size_t sports_encode(const sports_data_t *data, uint8_t *buf, size_t cap);
bool sports_decode(sports_data_t *data, const uint8_t *buf, size_t len);

// 成绩的完整显示文本，例如「128 次」「8.42 秒」「1:23.45」「185 厘米」。
bool sports_format_result(const sports_def_t *def, int32_t value, char *buf, size_t cap);
// 只含数字/冒号/小数点，供 40px Montserrat 大字号使用。
bool sports_format_value_only(const sports_def_t *def, int32_t value, char *buf, size_t cap);
// 运行中的计时，固定「分:秒.百分秒」，例如「0:08.42」「1:23.45」。
bool sports_format_clock(int64_t elapsed_ms, char *buf, size_t cap);

int32_t sports_elapsed_ms(int64_t start_us, int64_t now_us);
// 限时计数剩余秒数，向上取整；limit_ms 为 0 或尚未开始时返回 0。
int sports_remaining_seconds(uint16_t limit_ms, int64_t start_us, int64_t now_us);
