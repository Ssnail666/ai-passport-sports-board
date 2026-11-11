#include "sports_model.h"

#include <stdio.h>
#include <string.h>

#define SPORTS_MAGIC 0x54525053u  // "SPRT" in little-endian byte order
#define SPORTS_VERSION 1u
#define SPORTS_RECORD_BYTES 12u
#define SPORTS_HEADER_BYTES 8u

_Static_assert(SPORTS_BLOB_SIZE == 984u,
               "blob layout changed; update sports_encode/sports_decode together");

// 项目顺序即首页列表顺序。tag 会显示在首页每一行，保持短小。
static const sports_def_t DEFS[SPORTS_COUNT] = {
    [SPORTS_JUMP_ROPE] = {
        .name = "跳绳", .badge = "绳", .tag = "60秒计数",
        .mode = SPORTS_MODE_COUNT, .limit_ms = 60000,
    },
    [SPORTS_SIT_UP] = {
        .name = "仰卧起坐", .badge = "仰", .tag = "60秒计数",
        .mode = SPORTS_MODE_COUNT, .limit_ms = 60000,
    },
    [SPORTS_PUSH_UP] = {
        .name = "俯卧撑", .badge = "俯", .tag = "自由计数",
        .mode = SPORTS_MODE_COUNT,
    },
    [SPORTS_PULL_UP] = {
        .name = "引体向上", .badge = "引", .tag = "自由计数",
        .mode = SPORTS_MODE_COUNT,
    },
    [SPORTS_LONG_JUMP] = {
        .name = "立定跳远", .badge = "跳", .tag = "距离成绩",
        .mode = SPORTS_MODE_DISTANCE, .min_value = 0, .max_value = 300,
    },
    [SPORTS_SIT_REACH] = {
        .name = "坐位体前屈", .badge = "屈", .tag = "距离成绩",
        .mode = SPORTS_MODE_DISTANCE, .min_value = -20, .max_value = 40,
    },
    [SPORTS_RUN_50M] = {
        .name = "50米跑", .badge = "50", .tag = "计时",
        .mode = SPORTS_MODE_TIME,
    },
    [SPORTS_RUN_800M] = {
        .name = "800米跑", .badge = "800", .tag = "计时",
        .mode = SPORTS_MODE_TIME,
    },
    [SPORTS_RUN_1000M] = {
        .name = "1000米跑", .badge = "1K", .tag = "计时",
        .mode = SPORTS_MODE_TIME,
    },
};

static const char *const PLAYER_NAMES[SPORTS_PLAYER_COUNT] = {
    "1号选手", "2号选手", "3号选手", "4号选手",
};
static const char *const PLAYER_SHORT[SPORTS_PLAYER_COUNT] = {
    "1号", "2号", "3号", "4号",
};

const sports_def_t *sports_def(int index) {
    return &DEFS[sports_clamp_index(index)];
}

int sports_clamp_index(int index) {
    if (index < 0) return 0;
    if (index >= SPORTS_COUNT) return SPORTS_COUNT - 1;
    return index;
}

const char *sports_player_name(int player) {
    if (player < 1 || player > SPORTS_PLAYER_COUNT) return PLAYER_NAMES[0];
    return PLAYER_NAMES[player - 1];
}

const char *sports_player_short(int player) {
    if (player < 1 || player > SPORTS_PLAYER_COUNT) return PLAYER_SHORT[0];
    return PLAYER_SHORT[player - 1];
}

bool sports_mode_is_valid(sports_mode_t mode) {
    switch (mode) {
        case SPORTS_MODE_COUNT:
        case SPORTS_MODE_TIME:
        case SPORTS_MODE_DISTANCE:
            return true;
    }
    return false;
}

bool sports_value_in_range(const sports_def_t *def, int32_t value) {
    if (!def) return false;
    switch (def->mode) {
        case SPORTS_MODE_COUNT:
            return value >= 0 && value <= SPORTS_MAX_COUNT;
        case SPORTS_MODE_TIME:
            return value >= 0 && value <= SPORTS_MAX_TIME_CS;
        case SPORTS_MODE_DISTANCE:
            return value >= def->min_value && value <= def->max_value;
    }
    return false;
}

int sports_compare_records(const sports_def_t *def, const sports_record_t *a,
                           const sports_record_t *b) {
    if (!def || !a || !b) return 0;
    if (!a->valid && !b->valid) return 0;
    if (!a->valid) return 1;
    if (!b->valid) return -1;
    switch (def->mode) {
        case SPORTS_MODE_COUNT:
            if (a->value != b->value) return a->value > b->value ? -1 : 1;
            if (a->aux != b->aux) return a->aux < b->aux ? -1 : 1;
            return 0;
        case SPORTS_MODE_TIME:
            if (a->value != b->value) return a->value < b->value ? -1 : 1;
            return 0;
        case SPORTS_MODE_DISTANCE:
            if (a->value != b->value) return a->value > b->value ? -1 : 1;
            return 0;
    }
    return 0;
}

void sports_data_init(sports_data_t *data) {
    if (!data) return;
    memset(data, 0, sizeof(*data));
    data->selected_player = 1;
}

static bool record_is_valid(const sports_def_t *def, const sports_record_t *rec) {
    if (!def || !rec || !rec->valid) return false;
    if (rec->player < 1 || rec->player > SPORTS_PLAYER_COUNT) return false;
    if (!sports_value_in_range(def, rec->value)) return false;
    if (def->mode == SPORTS_MODE_COUNT) {
        if (rec->aux < 0 || rec->aux > SPORTS_MAX_AUX_MS) return false;
    }
    return true;
}

void sports_data_sanitize(sports_data_t *data) {
    if (!data) return;
    if (data->selected_player < 1 || data->selected_player > SPORTS_PLAYER_COUNT) {
        data->selected_player = 1;
    }

    for (int sport = 0; sport < SPORTS_COUNT; sport++) {
        const sports_def_t *def = &DEFS[sport];
        sports_record_t compact[SPORTS_TOP_N];
        int count = 0;

        for (int i = 0; i < SPORTS_TOP_N; i++) {
            const sports_record_t *rec = &data->top[sport][i];
            if (!record_is_valid(def, rec)) continue;
            compact[count] = *rec;
            if (def->mode != SPORTS_MODE_COUNT) compact[count].aux = 0;
            count++;
        }

        // 插入排序：比较函数把更好的记录排在前面。
        for (int i = 1; i < count; i++) {
            sports_record_t key = compact[i];
            int j = i - 1;
            while (j >= 0 && sports_compare_records(def, &compact[j], &key) > 0) {
                compact[j + 1] = compact[j];
                j--;
            }
            compact[j + 1] = key;
        }

        memset(data->top[sport], 0, sizeof(data->top[sport]));
        for (int i = 0; i < count; i++) data->top[sport][i] = compact[i];

        for (int player = 1; player <= SPORTS_PLAYER_COUNT; player++) {
            sports_record_t *pb = &data->pb[sport][player - 1];
            if (!record_is_valid(def, pb) || pb->player != (uint8_t)player) {
                memset(pb, 0, sizeof(*pb));
            } else if (def->mode != SPORTS_MODE_COUNT) {
                pb->aux = 0;
            }

            // 前 5 名里若有同一选手的更好成绩，个人最佳至少应与之一致。
            const sports_record_t *best = NULL;
            for (int i = 0; i < count; i++) {
                const sports_record_t *rec = &data->top[sport][i];
                if (rec->player != (uint8_t)player) continue;
                if (!best || sports_compare_records(def, rec, best) < 0) best = rec;
            }
            if (best && (!pb->valid || sports_compare_records(def, best, pb) < 0)) {
                *pb = *best;
            }
        }
    }
}

const sports_record_t *sports_top_at(const sports_data_t *data, int sport, int rank) {
    if (!data || sport < 0 || sport >= SPORTS_COUNT || rank < 1 || rank > SPORTS_TOP_N) {
        return NULL;
    }
    const sports_record_t *rec = &data->top[sport][rank - 1];
    return rec->valid ? rec : NULL;
}

const sports_record_t *sports_pb_at(const sports_data_t *data, int sport, int player) {
    if (!data || sport < 0 || sport >= SPORTS_COUNT ||
        player < 1 || player > SPORTS_PLAYER_COUNT) {
        return NULL;
    }
    const sports_record_t *rec = &data->pb[sport][player - 1];
    return rec->valid ? rec : NULL;
}

sports_add_result_t sports_add_result(sports_data_t *data, int sport, int player,
                                      int32_t value, int32_t aux) {
    sports_add_result_t out = { false, false, 0 };
    if (!data || sport < 0 || sport >= SPORTS_COUNT ||
        player < 1 || player > SPORTS_PLAYER_COUNT) {
        return out;
    }

    const sports_def_t *def = &DEFS[sport];
    if (!sports_value_in_range(def, value)) return out;
    if (def->mode == SPORTS_MODE_COUNT) {
        if (aux < 0) aux = 0;
        if (aux > SPORTS_MAX_AUX_MS) aux = SPORTS_MAX_AUX_MS;
    } else {
        aux = 0;
    }

    // 先修复外部可能留下的空洞，保证下面的移位不会覆盖有效记录。
    sports_data_sanitize(data);

    const sports_record_t rec = {
        .valid = 1,
        .player = (uint8_t)player,
        .value = value,
        .aux = aux,
    };

    sports_record_t *pb = &data->pb[sport][player - 1];
    if (!pb->valid || sports_compare_records(def, &rec, pb) < 0) {
        *pb = rec;
        out.is_pb = true;
    }

    int pos = 0;
    while (pos < SPORTS_TOP_N && data->top[sport][pos].valid &&
           sports_compare_records(def, &rec, &data->top[sport][pos]) >= 0) {
        pos++;
    }
    if (pos < SPORTS_TOP_N) {
        for (int i = SPORTS_TOP_N - 1; i > pos; i--) {
            data->top[sport][i] = data->top[sport][i - 1];
        }
        data->top[sport][pos] = rec;
        out.ranked = true;
        out.rank = (uint8_t)(pos + 1);
    }
    return out;
}

static void put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_record(uint8_t *p, const sports_record_t *rec) {
    p[0] = rec->valid ? 1u : 0u;
    p[1] = rec->player;
    p[2] = 0;
    p[3] = 0;
    put_u32(p + 4, (uint32_t)rec->value);
    put_u32(p + 8, (uint32_t)rec->aux);
}

static void get_record(const uint8_t *p, sports_record_t *rec) {
    rec->valid = p[0] ? 1u : 0u;
    rec->player = p[1];
    rec->value = (int32_t)get_u32(p + 4);
    rec->aux = (int32_t)get_u32(p + 8);
}

static uint32_t fnv1a(const uint8_t *p, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

size_t sports_encode(const sports_data_t *data, uint8_t *buf, size_t cap) {
    if (!data || !buf || cap < SPORTS_BLOB_SIZE) return 0;

    sports_data_t copy = *data;
    sports_data_sanitize(&copy);
    memset(buf, 0, SPORTS_BLOB_SIZE);

    size_t off = 0;
    put_u32(buf + off, SPORTS_MAGIC);
    off += 4;
    put_u16(buf + off, SPORTS_VERSION);
    off += 2;
    buf[off++] = copy.selected_player;
    buf[off++] = 0;
    for (int sport = 0; sport < SPORTS_COUNT; sport++) {
        for (int rank = 0; rank < SPORTS_TOP_N; rank++) {
            put_record(buf + off, &copy.top[sport][rank]);
            off += SPORTS_RECORD_BYTES;
        }
    }
    for (int sport = 0; sport < SPORTS_COUNT; sport++) {
        for (int player = 0; player < SPORTS_PLAYER_COUNT; player++) {
            put_record(buf + off, &copy.pb[sport][player]);
            off += SPORTS_RECORD_BYTES;
        }
    }
    put_u32(buf + off, fnv1a(buf, off));
    off += 4;
    return off == SPORTS_BLOB_SIZE ? off : 0;
}

bool sports_decode(sports_data_t *data, const uint8_t *buf, size_t len) {
    if (!data || !buf || len != SPORTS_BLOB_SIZE) return false;
    if (get_u32(buf) != SPORTS_MAGIC) return false;
    if (get_u16(buf + 4) != SPORTS_VERSION) return false;
    if (get_u32(buf + SPORTS_BLOB_SIZE - 4) != fnv1a(buf, SPORTS_BLOB_SIZE - 4)) {
        return false;
    }

    sports_data_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.selected_player = buf[6];

    size_t off = SPORTS_HEADER_BYTES;
    for (int sport = 0; sport < SPORTS_COUNT; sport++) {
        for (int rank = 0; rank < SPORTS_TOP_N; rank++) {
            get_record(buf + off, &tmp.top[sport][rank]);
            off += SPORTS_RECORD_BYTES;
        }
    }
    for (int sport = 0; sport < SPORTS_COUNT; sport++) {
        for (int player = 0; player < SPORTS_PLAYER_COUNT; player++) {
            get_record(buf + off, &tmp.pb[sport][player]);
            off += SPORTS_RECORD_BYTES;
        }
    }
    if (off != SPORTS_BLOB_SIZE - 4) return false;

    sports_data_sanitize(&tmp);
    *data = tmp;
    return true;
}

bool sports_format_result(const sports_def_t *def, int32_t value, char *buf, size_t cap) {
    if (!def || !buf || cap == 0) return false;
    int written;
    switch (def->mode) {
        case SPORTS_MODE_COUNT:
            written = snprintf(buf, cap, "%ld 次", (long)value);
            break;
        case SPORTS_MODE_TIME:
            if (value < 0) value = 0;
            if (value < 6000) {
                written = snprintf(buf, cap, "%ld.%02ld 秒",
                                   (long)(value / 100), (long)(value % 100));
            } else {
                written = snprintf(buf, cap, "%ld:%02ld.%02ld",
                                   (long)(value / 6000),
                                   (long)((value / 100) % 60),
                                   (long)(value % 100));
            }
            break;
        case SPORTS_MODE_DISTANCE:
            written = snprintf(buf, cap, "%ld 厘米", (long)value);
            break;
        default:
            return false;
    }
    return written >= 0 && (size_t)written < cap;
}

bool sports_format_value_only(const sports_def_t *def, int32_t value, char *buf, size_t cap) {
    if (!def || !buf || cap == 0) return false;
    int written;
    switch (def->mode) {
        case SPORTS_MODE_COUNT:
        case SPORTS_MODE_DISTANCE:
            written = snprintf(buf, cap, "%ld", (long)value);
            break;
        case SPORTS_MODE_TIME:
            if (value < 0) value = 0;
            if (value < 6000) {
                written = snprintf(buf, cap, "%ld.%02ld",
                                   (long)(value / 100), (long)(value % 100));
            } else {
                written = snprintf(buf, cap, "%ld:%02ld.%02ld",
                                   (long)(value / 6000),
                                   (long)((value / 100) % 60),
                                   (long)(value % 100));
            }
            break;
        default:
            return false;
    }
    return written >= 0 && (size_t)written < cap;
}

bool sports_format_clock(int64_t elapsed_ms, char *buf, size_t cap) {
    if (!buf || cap == 0) return false;
    int64_t centis = elapsed_ms / 10;
    if (centis < 0) centis = 0;
    if (centis > 359999) centis = 359999;  // 59:59.99
    const long minutes = (long)(centis / 6000);
    const long seconds = (long)((centis / 100) % 60);
    const long hundredths = (long)(centis % 100);
    const int written = snprintf(buf, cap, "%ld:%02ld.%02ld",
                                 minutes, seconds, hundredths);
    return written >= 0 && (size_t)written < cap;
}

int32_t sports_elapsed_ms(int64_t start_us, int64_t now_us) {
    if (start_us <= 0 || now_us <= start_us) return 0;
    const int64_t diff_us = now_us - start_us;
    if (diff_us > (int64_t)INT32_MAX * 1000) return INT32_MAX;
    return (int32_t)(diff_us / 1000);
}

int sports_remaining_seconds(uint16_t limit_ms, int64_t start_us, int64_t now_us) {
    if (limit_ms == 0 || start_us <= 0) return 0;
    const int32_t elapsed = sports_elapsed_ms(start_us, now_us);
    if (elapsed >= (int32_t)limit_ms) return 0;
    return (int)((limit_ms - (uint32_t)elapsed + 999u) / 1000u);
}
