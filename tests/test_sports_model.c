// tests/test_sports_model.c —— 「运动榜」纯逻辑的 host 测试。
// 不依赖 ESP-IDF/LVGL：只编译 main/sports_model.c，验证排序、个人最佳、
// NVS 编解码与所有显示格式。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sports_model.h"

static void expect_record(const sports_record_t *rec, int player, int32_t value,
                          int32_t aux) {
    assert(rec != NULL);
    assert(rec->valid);
    assert(rec->player == player);
    assert(rec->value == value);
    assert(rec->aux == aux);
}

static void test_table(void) {
    for (int i = 0; i < SPORTS_COUNT; i++) {
        const sports_def_t *def = sports_def(i);
        assert(def != NULL);
        assert(def->name && def->name[0]);
        assert(def->badge && def->badge[0]);
        assert(def->tag && def->tag[0]);
        assert(sports_mode_is_valid(def->mode));
        if (def->mode == SPORTS_MODE_DISTANCE) {
            assert(def->max_value > def->min_value);
            assert(def->limit_ms == 0);
        }
        if (def->mode == SPORTS_MODE_TIME) {
            assert(def->limit_ms == 0);
        }
    }

    assert(sports_def(-1) == sports_def(0));
    assert(sports_def(SPORTS_COUNT) == sports_def(SPORTS_COUNT - 1));
    assert(sports_clamp_index(-7) == 0);
    assert(sports_clamp_index(SPORTS_COUNT + 3) == SPORTS_COUNT - 1);

    assert(strcmp(sports_player_name(1), "1号选手") == 0);
    assert(strcmp(sports_player_name(4), "4号选手") == 0);
    assert(strcmp(sports_player_name(0), "1号选手") == 0);
    assert(strcmp(sports_player_short(2), "2号") == 0);
    assert(strcmp(sports_player_short(9), "1号") == 0);

    assert(!sports_mode_is_valid((sports_mode_t)99));
    assert(!sports_value_in_range(NULL, 1));

    const sports_def_t *count = sports_def(SPORTS_JUMP_ROPE);
    assert(sports_value_in_range(count, 0));
    assert(sports_value_in_range(count, SPORTS_MAX_COUNT));
    assert(!sports_value_in_range(count, -1));
    assert(!sports_value_in_range(count, SPORTS_MAX_COUNT + 1));

    const sports_def_t *time = sports_def(SPORTS_RUN_50M);
    assert(sports_value_in_range(time, 0));
    assert(sports_value_in_range(time, SPORTS_MAX_TIME_CS));
    assert(!sports_value_in_range(time, -1));
    assert(!sports_value_in_range(time, SPORTS_MAX_TIME_CS + 1));

    const sports_def_t *jump = sports_def(SPORTS_LONG_JUMP);
    assert(sports_value_in_range(jump, 0));
    assert(sports_value_in_range(jump, 300));
    assert(!sports_value_in_range(jump, -1));
    assert(!sports_value_in_range(jump, 301));

    const sports_def_t *reach = sports_def(SPORTS_SIT_REACH);
    assert(sports_value_in_range(reach, -20));
    assert(sports_value_in_range(reach, 40));
    assert(!sports_value_in_range(reach, -21));
    assert(!sports_value_in_range(reach, 41));

    assert(strcmp(jump->name, "立定跳远") == 0);
    assert(strcmp(reach->name, "坐位体前屈") == 0);
}

static void test_compare(void) {
    const sports_def_t *count = sports_def(SPORTS_JUMP_ROPE);
    const sports_record_t a = { .valid = 1, .player = 1, .value = 100, .aux = 60000 };
    const sports_record_t b = { .valid = 1, .player = 2, .value = 100, .aux = 55000 };
    const sports_record_t c = { .valid = 1, .player = 3, .value = 101, .aux = 60000 };
    assert(sports_compare_records(count, &a, &b) > 0);  // 同次数,用时短者优先
    assert(sports_compare_records(count, &b, &a) < 0);
    assert(sports_compare_records(count, &c, &a) < 0);
    assert(sports_compare_records(count, &c, &c) == 0);

    const sports_record_t invalid = { 0 };
    assert(sports_compare_records(count, &invalid, &a) > 0);
    assert(sports_compare_records(count, &a, &invalid) < 0);
    assert(sports_compare_records(count, &invalid, &invalid) == 0);

    const sports_def_t *time = sports_def(SPORTS_RUN_50M);
    const sports_record_t fast = { 1, 1, 800, 0 };
    const sports_record_t slow = { 1, 2, 900, 0 };
    assert(sports_compare_records(time, &fast, &slow) < 0);
    assert(sports_compare_records(time, &slow, &fast) > 0);

    const sports_def_t *distance = sports_def(SPORTS_LONG_JUMP);
    const sports_record_t far = { 1, 1, 200, 0 };
    const sports_record_t near = { 1, 2, 180, 0 };
    assert(sports_compare_records(distance, &far, &near) < 0);
    assert(sports_compare_records(distance, &near, &far) > 0);

    const sports_def_t *reach = sports_def(SPORTS_SIT_REACH);
    const sports_record_t neg5 = { 1, 1, -5, 0 };
    const sports_record_t neg10 = { 1, 2, -10, 0 };
    assert(sports_compare_records(reach, &neg5, &neg10) < 0);
}

static void test_add_count(void) {
    sports_data_t data;
    sports_data_init(&data);
    assert(data.selected_player == 1);
    assert(sports_top_at(&data, 0, 1) == NULL);
    assert(sports_pb_at(&data, 0, 1) == NULL);

    sports_add_result_t r = sports_add_result(&data, SPORTS_JUMP_ROPE, 1, 120, 60000);
    assert(r.ranked && r.rank == 1 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 1), 1, 120, 60000);
    expect_record(sports_pb_at(&data, SPORTS_JUMP_ROPE, 1), 1, 120, 60000);

    r = sports_add_result(&data, SPORTS_JUMP_ROPE, 2, 100, 60000);
    assert(r.ranked && r.rank == 2 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 2), 2, 100, 60000);

    r = sports_add_result(&data, SPORTS_JUMP_ROPE, 2, 130, 60000);
    assert(r.ranked && r.rank == 1 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 1), 2, 130, 60000);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 2), 1, 120, 60000);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 3), 2, 100, 60000);

    // 同次数、用时更短：排在原 130 次前面，并刷新个人最佳。
    r = sports_add_result(&data, SPORTS_JUMP_ROPE, 2, 130, 55000);
    assert(r.ranked && r.rank == 1 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 1), 2, 130, 55000);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 2), 2, 130, 60000);

    // 填满前五，再追加一个较差的成绩：上榜但不改变前五。
    for (int i = 0; i < 5; i++) {
        r = sports_add_result(&data, SPORTS_SIT_UP, 1, 200 - i * 10, 60000);
        assert(r.ranked);
    }
    r = sports_add_result(&data, SPORTS_SIT_UP, 2, 10, 60000);
    assert(!r.ranked && r.rank == 0 && r.is_pb);
    expect_record(sports_pb_at(&data, SPORTS_SIT_UP, 2), 2, 10, 60000);
    expect_record(sports_top_at(&data, SPORTS_SIT_UP, 5), 1, 160, 60000);

    // 更差的成绩不刷新个人最佳。
    r = sports_add_result(&data, SPORTS_SIT_UP, 2, 5, 60000);
    assert(!r.ranked && !r.is_pb);
    expect_record(sports_pb_at(&data, SPORTS_SIT_UP, 2), 2, 10, 60000);

    // 非法选手/项目/数值不得改动数据。
    const sports_record_t before = data.top[SPORTS_JUMP_ROPE][0];
    r = sports_add_result(&data, -1, 1, 999, 0);
    assert(!r.ranked && !r.is_pb);
    r = sports_add_result(&data, SPORTS_JUMP_ROPE, 0, 999, 0);
    assert(!r.ranked && !r.is_pb);
    r = sports_add_result(&data, SPORTS_JUMP_ROPE, 1, -5, 0);
    assert(!r.ranked && !r.is_pb);
    assert(memcmp(&before, &data.top[SPORTS_JUMP_ROPE][0], sizeof(before)) == 0);
}

static void test_add_time(void) {
    sports_data_t data;
    sports_data_init(&data);

    sports_add_result_t r = sports_add_result(&data, SPORTS_RUN_50M, 1, 900, 0);
    assert(r.ranked && r.rank == 1 && r.is_pb);

    r = sports_add_result(&data, SPORTS_RUN_50M, 2, 850, 0);
    assert(r.ranked && r.rank == 1 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_RUN_50M, 1), 2, 850, 0);
    expect_record(sports_top_at(&data, SPORTS_RUN_50M, 2), 1, 900, 0);

    r = sports_add_result(&data, SPORTS_RUN_50M, 3, 870, 0);
    assert(r.ranked && r.rank == 2 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_RUN_50M, 2), 3, 870, 0);

    // 更慢的成绩排到后面，且不刷新个人最佳。
    r = sports_add_result(&data, SPORTS_RUN_50M, 1, 950, 0);
    assert(r.ranked && r.rank == 4 && !r.is_pb);
    expect_record(sports_pb_at(&data, SPORTS_RUN_50M, 1), 1, 900, 0);

    // 完全相同的成绩保留先到者在前，新选手仍获得个人最佳。
    r = sports_add_result(&data, SPORTS_RUN_50M, 4, 850, 0);
    assert(r.ranked && r.rank == 2 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_RUN_50M, 1), 2, 850, 0);
    expect_record(sports_top_at(&data, SPORTS_RUN_50M, 2), 4, 850, 0);
}

static void test_add_distance(void) {
    sports_data_t data;
    sports_data_init(&data);

    sports_add_result_t r = sports_add_result(&data, SPORTS_LONG_JUMP, 1, 180, 0);
    assert(r.ranked && r.rank == 1 && r.is_pb);
    r = sports_add_result(&data, SPORTS_LONG_JUMP, 2, 200, 0);
    assert(r.ranked && r.rank == 1 && r.is_pb);
    r = sports_add_result(&data, SPORTS_LONG_JUMP, 3, 190, 0);
    assert(r.ranked && r.rank == 2 && r.is_pb);
    expect_record(sports_top_at(&data, SPORTS_LONG_JUMP, 1), 2, 200, 0);
    expect_record(sports_top_at(&data, SPORTS_LONG_JUMP, 2), 3, 190, 0);
    expect_record(sports_top_at(&data, SPORTS_LONG_JUMP, 3), 1, 180, 0);

    // 坐位体前屈允许负值，越大越好。
    r = sports_add_result(&data, SPORTS_SIT_REACH, 1, -5, 0);
    assert(r.ranked && r.rank == 1);
    r = sports_add_result(&data, SPORTS_SIT_REACH, 2, 10, 0);
    assert(r.ranked && r.rank == 1);
    r = sports_add_result(&data, SPORTS_SIT_REACH, 3, 0, 0);
    assert(r.ranked && r.rank == 2);
    expect_record(sports_top_at(&data, SPORTS_SIT_REACH, 1), 2, 10, 0);
    expect_record(sports_top_at(&data, SPORTS_SIT_REACH, 2), 3, 0, 0);
    expect_record(sports_top_at(&data, SPORTS_SIT_REACH, 3), 1, -5, 0);

    // 越界距离被拒绝。
    r = sports_add_result(&data, SPORTS_LONG_JUMP, 1, 301, 0);
    assert(!r.ranked && !r.is_pb);
    r = sports_add_result(&data, SPORTS_SIT_REACH, 1, -21, 0);
    assert(!r.ranked && !r.is_pb);

    // 非距离项目传入 aux 会被清零。
    r = sports_add_result(&data, SPORTS_LONG_JUMP, 1, 185, 1234);
    assert(r.ranked);
    expect_record(sports_top_at(&data, SPORTS_LONG_JUMP, 3), 1, 185, 0);
}

static void test_sanitize(void) {
    sports_data_t data;
    memset(&data, 0, sizeof(data));
    data.selected_player = 9;
    data.top[SPORTS_JUMP_ROPE][0] = (sports_record_t){ 1, 2, 100, 60000 };
    data.top[SPORTS_JUMP_ROPE][1] = (sports_record_t){ 1, 1, 120, 60000 };
    data.top[SPORTS_JUMP_ROPE][2] = (sports_record_t){ 1, 0, 999, 0 };
    data.top[SPORTS_JUMP_ROPE][3] = (sports_record_t){ 1, 3, 999999, 0 };
    data.top[SPORTS_JUMP_ROPE][4] = (sports_record_t){ 0, 4, 0, 0 };
    data.pb[SPORTS_JUMP_ROPE][0] = (sports_record_t){ 1, 1, 90, 60000 };
    data.pb[SPORTS_JUMP_ROPE][1] = (sports_record_t){ 1, 2, 100, 60000 };
    data.pb[SPORTS_JUMP_ROPE][2] = (sports_record_t){ 1, 3, 0, 0 };
    data.pb[SPORTS_JUMP_ROPE][3] = (sports_record_t){ 1, 5, 1, 0 };

    sports_data_sanitize(&data);
    assert(data.selected_player == 1);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 1), 1, 120, 60000);
    expect_record(sports_top_at(&data, SPORTS_JUMP_ROPE, 2), 2, 100, 60000);
    assert(sports_top_at(&data, SPORTS_JUMP_ROPE, 3) == NULL);

    // 个人最佳按前五修复：p1 从 90 修到 120；p2 保持 100；p3 的 0 不在前五但合法；p4 被清除。
    expect_record(sports_pb_at(&data, SPORTS_JUMP_ROPE, 1), 1, 120, 60000);
    expect_record(sports_pb_at(&data, SPORTS_JUMP_ROPE, 2), 2, 100, 60000);
    expect_record(sports_pb_at(&data, SPORTS_JUMP_ROPE, 3), 3, 0, 0);
    assert(sports_pb_at(&data, SPORTS_JUMP_ROPE, 4) == NULL);

    // 幂等：再次清理不应改变任何字段。
    sports_data_t copy = data;
    sports_data_sanitize(&copy);
    assert(memcmp(&data, &copy, sizeof(data)) == 0);

    // 合法个人最佳即使不在前五也应保留。
    sports_data_t other;
    sports_data_init(&other);
    for (int i = 0; i < 5; i++) {
        (void)sports_add_result(&other, SPORTS_PUSH_UP, 1, 100 - i, 0);
    }
    other.pb[SPORTS_PUSH_UP][1] = (sports_record_t){ 1, 2, 12, 0 };
    sports_data_sanitize(&other);
    expect_record(sports_pb_at(&other, SPORTS_PUSH_UP, 2), 2, 12, 0);
    assert(sports_top_at(&other, SPORTS_PUSH_UP, 5) != NULL);
}

static void test_serialization(void) {
    sports_data_t data;
    sports_data_init(&data);
    data.selected_player = 3;
    (void)sports_add_result(&data, SPORTS_JUMP_ROPE, 1, 128, 60000);
    (void)sports_add_result(&data, SPORTS_JUMP_ROPE, 3, 150, 60000);
    (void)sports_add_result(&data, SPORTS_RUN_50M, 2, 842, 0);
    (void)sports_add_result(&data, SPORTS_SIT_REACH, 4, -6, 0);

    uint8_t blob[SPORTS_BLOB_SIZE];
    uint8_t small[SPORTS_BLOB_SIZE - 1];
    assert(sports_encode(&data, blob, sizeof(blob)) == SPORTS_BLOB_SIZE);
    assert(sports_encode(&data, small, sizeof(small)) == 0);
    assert(sports_encode(NULL, blob, sizeof(blob)) == 0);

    sports_data_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert(sports_decode(&decoded, blob, sizeof(blob)));
    assert(decoded.selected_player == 3);
    expect_record(sports_top_at(&decoded, SPORTS_JUMP_ROPE, 1), 3, 150, 60000);
    expect_record(sports_top_at(&decoded, SPORTS_JUMP_ROPE, 2), 1, 128, 60000);
    expect_record(sports_pb_at(&decoded, SPORTS_RUN_50M, 2), 2, 842, 0);
    expect_record(sports_pb_at(&decoded, SPORTS_SIT_REACH, 4), 4, -6, 0);

    // 校验和错误必须拒绝，并且不改动输出。
    uint8_t broken[SPORTS_BLOB_SIZE];
    memcpy(broken, blob, sizeof(blob));
    broken[SPORTS_BLOB_SIZE - 1] ^= 0x5A;
    sports_data_t untouched = data;
    untouched.selected_player = 1;
    sports_data_t before = untouched;
    assert(!sports_decode(&untouched, broken, sizeof(broken)));
    assert(memcmp(&untouched, &before, sizeof(before)) == 0);

    // magic / 版本 / 长度错误都要拒绝。
    memcpy(broken, blob, sizeof(blob));
    broken[0] = 'X';
    assert(!sports_decode(&untouched, broken, sizeof(broken)));
    memcpy(broken, blob, sizeof(blob));
    broken[4] = 2;
    assert(!sports_decode(&untouched, broken, sizeof(broken)));
    assert(!sports_decode(&untouched, blob, sizeof(blob) - 1));
    assert(!sports_decode(NULL, blob, sizeof(blob)));
    assert(!sports_decode(&untouched, NULL, sizeof(blob)));

    // 编码会先 sanitize 一份副本，越界值不会写进 blob。
    sports_data_t dirty = data;
    dirty.selected_player = 200;
    dirty.top[SPORTS_JUMP_ROPE][4] = (sports_record_t){ 1, 1, 999999, 0 };
    assert(sports_encode(&dirty, blob, sizeof(blob)) == SPORTS_BLOB_SIZE);
    sports_data_t repaired;
    assert(sports_decode(&repaired, blob, sizeof(blob)));
    assert(repaired.selected_player == 1);
    expect_record(sports_top_at(&repaired, SPORTS_JUMP_ROPE, 1), 3, 150, 60000);
}

static void assert_ascii(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        assert(*p < 0x80);
    }
}

static void test_format(void) {
    const sports_def_t *count = sports_def(SPORTS_JUMP_ROPE);
    const sports_def_t *time = sports_def(SPORTS_RUN_50M);
    const sports_def_t *distance = sports_def(SPORTS_LONG_JUMP);
    const sports_def_t *reach = sports_def(SPORTS_SIT_REACH);

    char buf[48];
    assert(sports_format_result(count, 128, buf, sizeof(buf)));
    assert(strcmp(buf, "128 次") == 0);
    assert(sports_format_result(time, 842, buf, sizeof(buf)));
    assert(strcmp(buf, "8.42 秒") == 0);
    assert(sports_format_result(time, 5999, buf, sizeof(buf)));
    assert(strcmp(buf, "59.99 秒") == 0);
    assert(sports_format_result(time, 6000, buf, sizeof(buf)));
    assert(strcmp(buf, "1:00.00") == 0);
    assert(sports_format_result(time, 8345, buf, sizeof(buf)));
    assert(strcmp(buf, "1:23.45") == 0);
    assert(sports_format_result(time, -5, buf, sizeof(buf)));
    assert(strcmp(buf, "0.00 秒") == 0);
    assert(sports_format_result(distance, 185, buf, sizeof(buf)));
    assert(strcmp(buf, "185 厘米") == 0);
    assert(sports_format_result(reach, -5, buf, sizeof(buf)));
    assert(strcmp(buf, "-5 厘米") == 0);
    assert(!sports_format_result(NULL, 1, buf, sizeof(buf)));
    assert(!sports_format_result(count, 1, NULL, sizeof(buf)));
    assert(!sports_format_result(count, 1, buf, 0));
    assert(!sports_format_result(count, 1, buf, 3));

    char value[32];
    assert(sports_format_value_only(count, 128, value, sizeof(value)));
    assert(strcmp(value, "128") == 0);
    assert_ascii(value);
    assert(sports_format_value_only(time, 842, value, sizeof(value)));
    assert(strcmp(value, "8.42") == 0);
    assert_ascii(value);
    assert(sports_format_value_only(time, 8345, value, sizeof(value)));
    assert(strcmp(value, "1:23.45") == 0);
    assert_ascii(value);
    assert(sports_format_value_only(distance, 185, value, sizeof(value)));
    assert(strcmp(value, "185") == 0);
    assert_ascii(value);

    char clock[32];
    assert(sports_format_clock(0, clock, sizeof(clock)));
    assert(strcmp(clock, "0:00.00") == 0);
    assert(sports_format_clock(8420, clock, sizeof(clock)));
    assert(strcmp(clock, "0:08.42") == 0);
    assert(sports_format_clock(83450, clock, sizeof(clock)));
    assert(strcmp(clock, "1:23.45") == 0);
    assert(sports_format_clock(-1, clock, sizeof(clock)));
    assert(strcmp(clock, "0:00.00") == 0);
    assert(sports_format_clock(3600000, clock, sizeof(clock)));
    assert(strcmp(clock, "59:59.99") == 0);
    assert_ascii(clock);
    assert(!sports_format_clock(0, clock, 0));
    assert(!sports_format_clock(0, NULL, sizeof(clock)));
}

static void test_elapsed_and_remaining(void) {
    assert(sports_elapsed_ms(0, 1000000) == 0);
    assert(sports_elapsed_ms(1000000, 1000000) == 0);
    assert(sports_elapsed_ms(1000000, 1500000) == 500);
    assert(sports_elapsed_ms(2000000, 1000000) == 0);

    assert(sports_remaining_seconds(0, 1000000, 1000000) == 0);
    assert(sports_remaining_seconds(60000, 0, 1000000) == 0);
    assert(sports_remaining_seconds(60000, 1000000, 1000000) == 60);
    assert(sports_remaining_seconds(60000, 1000000, 1001000) == 60);
    assert(sports_remaining_seconds(60000, 1000000, 2000000) == 59);
    assert(sports_remaining_seconds(60000, 1000000, 60999999) == 1);
    assert(sports_remaining_seconds(60000, 1000000, 61000000) == 0);
    assert(sports_remaining_seconds(60000, 1000000, 99000000) == 0);
}

int main(void) {
    test_table();
    test_compare();
    test_add_count();
    test_add_time();
    test_add_distance();
    test_sanitize();
    test_serialization();
    test_format();
    test_elapsed_and_remaining();
    puts("sports model tests: PASS");
    return 0;
}
