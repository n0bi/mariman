#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MM_SCREEN_W 128
#define MM_SCREEN_H 64
#define MM_TILE_SIZE 8
#define MM_WORLD_ROWS 8
#define MM_GROUND_ROW 7

#define MM_PLAYER_W 10
#define MM_PLAYER_H 12
#define MM_ENEMY_W 8
#define MM_ENEMY_H 8
#define MM_PICKUP_W 7
#define MM_PICKUP_H 7

#define MM_FRAME_MS 33U
#define MM_QUEUE_DEPTH 16U
#define MM_STACK_SIZE (8 * 1024)

#define MM_PLAYER_START_X 16
#define MM_PLAYER_START_Y 44
#define MM_CAMERA_LEAD_X 44

#define MM_ACCEL_Q8 96
#define MM_FRICTION_Q8 80
#define MM_MAX_RUN_Q8 448
#define MM_JUMP_Q8 (-1312)
#define MM_GRAVITY_Q8 104
#define MM_MAX_FALL_Q8 1536
#define MM_STOMP_BOUNCE_Q8 (-864)

#define MM_COLLECTIBLE_SLOTS 96
#define MM_ENEMY_SLOTS 96
#define MM_FLOATING_LABELS 4

#define MM_SEGMENT_COLUMNS 16
#define MM_SEGMENT_PIXEL_W (MM_SEGMENT_COLUMNS * MM_TILE_SIZE)

typedef enum {
    MerryManModeTitle = 0,
    MerryManModePlaying,
    MerryManModeGameOver,
} MerryManMode;

typedef enum {
    MerryManSegmentFlat = 0,
    MerryManSegmentBush,
    MerryManSegmentPillar,
    MerryManSegmentSteps,
    MerryManSegmentFloating,
    MerryManSegmentGapSmall,
    MerryManSegmentGapLarge,
    MerryManSegmentBridge,
    MerryManSegmentSpikes,
    MerryManSegmentTower,
} MerryManSegmentFeature;

typedef enum {
    MerryManTileEmpty = 0,
    MerryManTileGround,
    MerryManTileBrick,
    MerryManTilePillar,
    MerryManTileBridge,
} MerryManTile;

typedef enum {
    MerryManDecoNone = 0,
    MerryManDecoBush,
    MerryManDecoSign,
    MerryManDecoRock,
    MerryManDecoMound,
} MerryManDeco;

typedef enum {
    MerryManEnemyTypeNone = 0,
    MerryManEnemyTypeWalker,
    MerryManEnemyTypeHopper,
} MerryManEnemyType;

typedef struct {
    uint32_t distance;
    const char* title;
} MerryManRankGoal;

typedef struct {
    MerryManEnemyType type;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t segment;
    bool face_left;
} MerryManEnemy;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;

    bool running;
    bool left_held;
    bool right_held;
    bool jump_pressed;
    bool grounded;
    bool facing_left;

    MerryManMode mode;

    int32_t player_x_q8;
    int32_t player_y_q8;
    int32_t player_vx_q8;
    int32_t player_vy_q8;

    uint32_t frame;
    uint32_t best_distance;
    uint32_t farthest_distance;
    uint32_t collectible_score;
    uint32_t enemy_score;

    int32_t collectible_segments[MM_COLLECTIBLE_SLOTS];
    bool collectible_collected[MM_COLLECTIBLE_SLOTS];

    int32_t enemy_segments[MM_ENEMY_SLOTS];
    bool enemy_defeated[MM_ENEMY_SLOTS];

    int16_t label_x[MM_FLOATING_LABELS];
    int16_t label_y[MM_FLOATING_LABELS];
    int16_t label_value[MM_FLOATING_LABELS];
    int16_t label_timer[MM_FLOATING_LABELS];
} MerryManApp;

static const MerryManRankGoal merry_man_rank_goals[] = {
    {0U, "Rookie"},
    {20U, "Scout"},
    {50U, "Ranger"},
    {90U, "Trailblazer"},
    {140U, "Hero"},
    {220U, "Legend"},
};

static const uint16_t merry_man_player_idle_rows[MM_PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07C, 0x0DE, 0x0D2, 0x186, 0x102,
};

static const uint16_t merry_man_player_run_a_rows[MM_PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07E, 0x0DA, 0x0D0, 0x188, 0x104,
};

static const uint16_t merry_man_player_run_b_rows[MM_PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07C, 0x0DE, 0x08A, 0x106, 0x080,
};

static const uint16_t merry_man_player_jump_rows[MM_PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07C, 0x0DE, 0x192, 0x102, 0x144,
};

static const uint16_t merry_man_walker_rows[MM_ENEMY_H] = {
    0x03C, 0x07E, 0x0DB, 0x0FF, 0x07E, 0x024, 0x066, 0x042,
};

static const uint16_t merry_man_hopper_rows[MM_ENEMY_H] = {
    0x018, 0x03C, 0x07E, 0x0DB, 0x0FF, 0x07E, 0x024, 0x042,
};

static const uint16_t merry_man_pickup_rows[MM_PICKUP_H] = {
    0x008, 0x01C, 0x03E, 0x02A, 0x03E, 0x01C, 0x008,
};

static uint32_t merry_man_hash32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352DU;
    value ^= value >> 15;
    value *= 0x846CA68BU;
    value ^= value >> 16;
    return value;
}

static int32_t merry_man_abs_i32(int32_t value) {
    return (value < 0) ? -value : value;
}

static int32_t merry_man_sign_i32(int32_t value) {
    if(value < 0) return -1;
    if(value > 0) return 1;
    return 0;
}

static int32_t merry_man_clamp_i32(int32_t value, int32_t minimum, int32_t maximum) {
    if(value < minimum) return minimum;
    if(value > maximum) return maximum;
    return value;
}

static uint32_t merry_man_ping_pong_u32(uint32_t value, uint32_t span) {
    uint32_t period;
    uint32_t phase;

    if(span == 0U) return 0U;

    period = span * 2U;
    phase = value % period;
    if(phase > span) phase = period - phase;
    return phase;
}

static bool merry_man_rects_overlap(
    int32_t ax,
    int32_t ay,
    int32_t aw,
    int32_t ah,
    int32_t bx,
    int32_t by,
    int32_t bw,
    int32_t bh) {
    return (ax < (bx + bw)) && ((ax + aw) > bx) && (ay < (by + bh)) && ((ay + ah) > by);
}

static uint32_t merry_man_distance_from_world_x(int32_t world_x) {
    if(world_x <= 0) return 0U;
    return (uint32_t)(world_x / 16);
}

static const char* merry_man_rank_title(uint32_t distance) {
    size_t index;
    const char* title = merry_man_rank_goals[0].title;

    for(index = 0U; index < (sizeof(merry_man_rank_goals) / sizeof(merry_man_rank_goals[0])); index++) {
        if(distance >= merry_man_rank_goals[index].distance) {
            title = merry_man_rank_goals[index].title;
        } else {
            break;
        }
    }

    return title;
}

static uint32_t merry_man_next_goal(uint32_t distance) {
    size_t index;

    for(index = 0U; index < (sizeof(merry_man_rank_goals) / sizeof(merry_man_rank_goals[0])); index++) {
        if(merry_man_rank_goals[index].distance > distance) {
            return merry_man_rank_goals[index].distance;
        }
    }

    return merry_man_rank_goals[(sizeof(merry_man_rank_goals) / sizeof(merry_man_rank_goals[0])) - 1U].distance + 80U;
}

static uint32_t merry_man_total_score(const MerryManApp* app) {
    return app->farthest_distance + (app->collectible_score * 6U) + (app->enemy_score * 10U);
}

static MerryManSegmentFeature merry_man_segment_feature(int32_t segment) {
    uint32_t roll;

    if(segment < 2) return MerryManSegmentFlat;
    if(segment == 2) return MerryManSegmentBush;

    roll = merry_man_hash32((uint32_t)segment + 0x4D455252U) % 10U;

    switch(roll) {
    case 0U:
        return MerryManSegmentFlat;
    case 1U:
        return MerryManSegmentBush;
    case 2U:
        return MerryManSegmentPillar;
    case 3U:
        return MerryManSegmentSteps;
    case 4U:
        return MerryManSegmentFloating;
    case 5U:
        return MerryManSegmentGapSmall;
    case 6U:
        return MerryManSegmentGapLarge;
    case 7U:
        return MerryManSegmentBridge;
    case 8U:
        return MerryManSegmentSpikes;
    default:
        return MerryManSegmentTower;
    }
}

static bool merry_man_ground_exists(int32_t column) {
    const int32_t segment = column / MM_SEGMENT_COLUMNS;
    const int32_t local = column % MM_SEGMENT_COLUMNS;
    const MerryManSegmentFeature feature = merry_man_segment_feature(segment);

    switch(feature) {
    case MerryManSegmentGapSmall:
        if((local >= 7) && (local <= 8)) return false;
        break;
    case MerryManSegmentGapLarge:
        if((local >= 6) && (local <= 8)) return false;
        break;
    case MerryManSegmentBridge:
        if((local >= 5) && (local <= 10)) return false;
        break;
    default:
        break;
    }

    return true;
}

static MerryManTile merry_man_tile_at(int32_t column, int32_t row) {
    const int32_t segment = column / MM_SEGMENT_COLUMNS;
    const int32_t local = column % MM_SEGMENT_COLUMNS;
    const MerryManSegmentFeature feature = merry_man_segment_feature(segment);

    if((row == MM_GROUND_ROW) && merry_man_ground_exists(column)) {
        return MerryManTileGround;
    }

    switch(feature) {
    case MerryManSegmentPillar:
        if((local >= 10) && (local <= 11) && (row >= 5) && (row <= 6)) return MerryManTilePillar;
        break;
    case MerryManSegmentSteps:
        if((local == 9) && (row == 6)) return MerryManTileBrick;
        if((local == 10) && (row >= 5) && (row <= 6)) return MerryManTileBrick;
        if((local == 11) && (row >= 4) && (row <= 6)) return MerryManTileBrick;
        if((local == 12) && (row >= 3) && (row <= 6)) return MerryManTileBrick;
        break;
    case MerryManSegmentFloating:
        if((local >= 5) && (local <= 7) && (row == 4)) return MerryManTileBrick;
        if((local >= 11) && (local <= 12) && (row == 5)) return MerryManTileBrick;
        break;
    case MerryManSegmentBridge:
        if((local >= 3) && (local <= 4) && (row == 5)) return MerryManTileBridge;
        if((local >= 10) && (local <= 11) && (row == 4)) return MerryManTileBridge;
        if((local >= 13) && (local <= 14) && (row == 5)) return MerryManTileBridge;
        break;
    case MerryManSegmentTower:
        if((local >= 9) && (local <= 11) && (row >= 4) && (row <= 6)) return MerryManTilePillar;
        if((local >= 7) && (local <= 13) && (row == 3)) return MerryManTileBrick;
        break;
    default:
        break;
    }

    return MerryManTileEmpty;
}

static MerryManDeco merry_man_deco_for_segment(int32_t segment) {
    const MerryManSegmentFeature feature = merry_man_segment_feature(segment);
    const uint32_t roll = (merry_man_hash32((uint32_t)segment + 0xD3C01234U) >> 3) % 4U;

    if(feature == MerryManSegmentBush) return MerryManDecoBush;
    if(feature == MerryManSegmentSpikes) return MerryManDecoRock;
    if((feature == MerryManSegmentFlat) && (roll == 0U)) return MerryManDecoSign;
    if((feature == MerryManSegmentFlat) && (roll == 1U)) return MerryManDecoMound;
    if((feature == MerryManSegmentPillar) && (roll == 2U)) return MerryManDecoRock;

    return MerryManDecoNone;
}

static bool merry_man_world_solid_pixel(int32_t world_x, int32_t world_y) {
    int32_t column;
    int32_t row;

    if((world_x < 0) || (world_y < 0) || (world_y >= MM_SCREEN_H)) return false;

    column = world_x / MM_TILE_SIZE;
    row = world_y / MM_TILE_SIZE;
    return merry_man_tile_at(column, row) != MerryManTileEmpty;
}

static bool merry_man_world_hazard_pixel(int32_t world_x, int32_t world_y) {
    const int32_t column = world_x / MM_TILE_SIZE;
    const int32_t segment = column / MM_SEGMENT_COLUMNS;
    const int32_t local = column % MM_SEGMENT_COLUMNS;
    const int32_t local_x = world_x % MM_TILE_SIZE;
    const int32_t local_y = world_y - 52;

    if((world_x < 0) || (world_y < 0) || (world_y >= MM_SCREEN_H)) return false;
    if(merry_man_segment_feature(segment) != MerryManSegmentSpikes) return false;
    if((local < 8) || (local > 10)) return false;
    if((world_y < 52) || (world_y > 55)) return false;

    return local_y >= (2 - (merry_man_abs_i32(local_x - 3) / 2));
}

static bool merry_man_world_rect_collides(int32_t world_x, int32_t world_y, int32_t width, int32_t height) {
    const int32_t left = world_x;
    const int32_t right = world_x + width - 1;
    const int32_t top = world_y;
    const int32_t bottom = world_y + height - 1;
    const int32_t middle_x = world_x + (width / 2);
    const int32_t middle_y = world_y + (height / 2);

    return merry_man_world_solid_pixel(left, top) || merry_man_world_solid_pixel(right, top) ||
           merry_man_world_solid_pixel(left, bottom) || merry_man_world_solid_pixel(right, bottom) ||
           merry_man_world_solid_pixel(middle_x, top) || merry_man_world_solid_pixel(middle_x, bottom) ||
           merry_man_world_solid_pixel(left, middle_y) || merry_man_world_solid_pixel(right, middle_y);
}

static bool merry_man_world_rect_hits_hazard(int32_t world_x, int32_t world_y, int32_t width, int32_t height) {
    const int32_t left = world_x;
    const int32_t right = world_x + width - 1;
    const int32_t top = world_y;
    const int32_t bottom = world_y + height - 1;
    const int32_t middle_x = world_x + (width / 2);
    const int32_t middle_y = world_y + (height / 2);

    return merry_man_world_hazard_pixel(left, top) || merry_man_world_hazard_pixel(right, top) ||
           merry_man_world_hazard_pixel(left, bottom) || merry_man_world_hazard_pixel(right, bottom) ||
           merry_man_world_hazard_pixel(middle_x, top) || merry_man_world_hazard_pixel(middle_x, bottom) ||
           merry_man_world_hazard_pixel(left, middle_y) || merry_man_world_hazard_pixel(right, middle_y);
}

static int32_t merry_man_find_floor_y(int32_t world_x, int32_t width) {
    int32_t y;
    int32_t offset;

    for(y = 0; y < MM_SCREEN_H; y++) {
        for(offset = 0; offset < width; offset++) {
            if(merry_man_world_solid_pixel(world_x + offset, y)) {
                return y;
            }
        }
    }

    return -1;
}

static uint32_t merry_man_collectible_slot_index(int32_t segment) {
    return ((uint32_t)segment) % MM_COLLECTIBLE_SLOTS;
}

static uint32_t merry_man_enemy_slot_index(int32_t segment) {
    return ((uint32_t)segment) % MM_ENEMY_SLOTS;
}

static bool merry_man_collectible_lookup(const MerryManApp* app, int32_t segment) {
    const uint32_t slot = merry_man_collectible_slot_index(segment);
    return (app->collectible_segments[slot] == segment) && app->collectible_collected[slot];
}

static bool merry_man_collectible_is_collected(MerryManApp* app, int32_t segment) {
    const uint32_t slot = merry_man_collectible_slot_index(segment);

    if(app->collectible_segments[slot] != segment) {
        app->collectible_segments[slot] = segment;
        app->collectible_collected[slot] = false;
    }

    return app->collectible_collected[slot];
}

static void merry_man_mark_collectible(MerryManApp* app, int32_t segment) {
    const uint32_t slot = merry_man_collectible_slot_index(segment);
    app->collectible_segments[slot] = segment;
    app->collectible_collected[slot] = true;
    app->collectible_score += 1U;
}

static bool merry_man_enemy_lookup(const MerryManApp* app, int32_t segment) {
    const uint32_t slot = merry_man_enemy_slot_index(segment);
    return (app->enemy_segments[slot] == segment) && app->enemy_defeated[slot];
}

static void merry_man_mark_enemy_defeated(MerryManApp* app, int32_t segment) {
    const uint32_t slot = merry_man_enemy_slot_index(segment);
    app->enemy_segments[slot] = segment;
    app->enemy_defeated[slot] = true;
    app->enemy_score += 1U;
}

static void merry_man_add_label(MerryManApp* app, int32_t world_x, int32_t world_y, int16_t value) {
    uint32_t index;
    uint32_t slot = 0U;

    for(index = 0U; index < MM_FLOATING_LABELS; index++) {
        if(app->label_timer[index] <= 0) {
            slot = index;
            break;
        }
        if(app->label_timer[index] < app->label_timer[slot]) {
            slot = index;
        }
    }

    app->label_x[slot] = (int16_t)world_x;
    app->label_y[slot] = (int16_t)world_y;
    app->label_value[slot] = value;
    app->label_timer[slot] = 20;
}

static bool merry_man_segment_has_collectible(int32_t segment) {
    const MerryManSegmentFeature feature = merry_man_segment_feature(segment);
    const uint32_t roll = merry_man_hash32((uint32_t)segment + 0xA11CE123U) % 4U;

    if(segment < 1) return false;
    if(feature == MerryManSegmentGapLarge) return roll != 0U;
    if((feature == MerryManSegmentBridge) || (feature == MerryManSegmentFloating) || (feature == MerryManSegmentTower)) return true;
    return roll == 0U;
}

static void merry_man_collectible_position(int32_t segment, int32_t* out_x, int32_t* out_y) {
    const MerryManSegmentFeature feature = merry_man_segment_feature(segment);
    const int32_t base_x = segment * MM_SEGMENT_PIXEL_W;

    switch(feature) {
    case MerryManSegmentFloating:
        *out_x = base_x + 48;
        *out_y = 24;
        break;
    case MerryManSegmentBridge:
        *out_x = base_x + 84;
        *out_y = 20;
        break;
    case MerryManSegmentTower:
        *out_x = base_x + 78;
        *out_y = 14;
        break;
    case MerryManSegmentGapSmall:
        *out_x = base_x + 60;
        *out_y = 28;
        break;
    case MerryManSegmentGapLarge:
        *out_x = base_x + 64;
        *out_y = 20;
        break;
    default:
        *out_x = base_x + 80;
        *out_y = 30;
        break;
    }
}

static MerryManEnemyType merry_man_segment_enemy_type(int32_t segment) {
    const MerryManSegmentFeature feature = merry_man_segment_feature(segment);
    const uint32_t roll = merry_man_hash32((uint32_t)segment + 0xEE771122U) % 5U;

    if(segment < 3) return MerryManEnemyTypeNone;
    if((feature == MerryManSegmentGapLarge) || (feature == MerryManSegmentBridge)) return MerryManEnemyTypeNone;
    if(roll >= 3U) return MerryManEnemyTypeNone;
    if((feature == MerryManSegmentFloating) || (feature == MerryManSegmentTower)) return MerryManEnemyTypeHopper;
    if((feature == MerryManSegmentSteps) && (roll == 1U)) return MerryManEnemyTypeHopper;
    return (roll == 0U) ? MerryManEnemyTypeWalker : MerryManEnemyTypeHopper;
}

static bool merry_man_generated_enemy(
    const MerryManApp* app,
    int32_t segment,
    uint32_t frame,
    MerryManEnemy* out_enemy) {
    const MerryManEnemyType type = merry_man_segment_enemy_type(segment);
    const int32_t base_x = segment * MM_SEGMENT_PIXEL_W;
    const uint32_t seed = merry_man_hash32((uint32_t)segment + 0xBAD5EEDU);
    const int32_t anchor_x = base_x + 40 + (int32_t)((seed >> 8) % 40U);
    const uint32_t phase = (seed >> 16) & 63U;
    const uint32_t range = 8U + ((seed >> 22) % 10U);
    const uint32_t speed = (type == MerryManEnemyTypeWalker) ? 2U : 3U;
    const uint32_t swing = merry_man_ping_pong_u32((frame / speed) + phase, range);
    const int32_t base_enemy_x = anchor_x + (int32_t)swing - (int32_t)(range / 2U);
    const int32_t floor_y = merry_man_find_floor_y(base_enemy_x + 1, MM_ENEMY_W - 2);
    int32_t enemy_x;
    int32_t enemy_y;

    if(type == MerryManEnemyTypeNone) return false;
    if(merry_man_enemy_lookup(app, segment)) return false;
    if(floor_y < 0) return false;

    enemy_x = base_enemy_x;
    enemy_y = floor_y - MM_ENEMY_H;

    if(type == MerryManEnemyTypeHopper) {
        const uint32_t hop_cycle = (frame + (phase * 3U)) % 44U;
        int32_t hop_height = 0;
        int32_t drift = 0;

        if(hop_cycle < 10U) {
            hop_height = (int32_t)hop_cycle;
        } else if(hop_cycle < 20U) {
            hop_height = (int32_t)(20U - hop_cycle);
        } else if((hop_cycle >= 28U) && (hop_cycle < 36U)) {
            hop_height = (int32_t)((hop_cycle - 28U) / 2U);
        } else if(hop_cycle >= 36U) {
            hop_height = (int32_t)((44U - hop_cycle) / 2U);
        }

        if(((seed >> 5) & 1U) != 0U) {
            if(hop_cycle < 22U) {
                drift = (int32_t)(hop_cycle / 6U);
            } else {
                drift = -((int32_t)((44U - hop_cycle) / 8U));
            }
        }

        enemy_x += drift;
        enemy_y -= hop_height;
    }

    out_enemy->type = type;
    out_enemy->x = enemy_x;
    out_enemy->y = enemy_y;
    out_enemy->w = MM_ENEMY_W;
    out_enemy->h = MM_ENEMY_H;
    out_enemy->segment = segment;
    out_enemy->face_left = ((seed & 1U) == 0U);

    return true;
}

static void merry_man_draw_sprite(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    uint8_t width,
    uint8_t height,
    const uint16_t* rows,
    bool mirror) {
    uint8_t row;

    for(row = 0U; row < height; row++) {
        const uint16_t row_bits = rows[row];
        uint8_t col;

        for(col = 0U; col < width; col++) {
            const uint8_t bit_index = (uint8_t)(width - 1U - col);
            const bool on = (row_bits & ((uint16_t)1U << bit_index)) != 0U;

            if(on) {
                const int32_t draw_x = x + (mirror ? ((int32_t)width - 1 - (int32_t)col) : (int32_t)col);
                canvas_draw_dot(canvas, draw_x, y + (int32_t)row);
            }
        }
    }
}

static void merry_man_draw_player(Canvas* canvas, const MerryManApp* app, int32_t x, int32_t y) {
    const bool running = app->grounded && (merry_man_abs_i32(app->player_vx_q8) > 80);
    const bool jumping = !app->grounded;
    const uint16_t* rows = merry_man_player_idle_rows;

    if(jumping) {
        rows = merry_man_player_jump_rows;
    } else if(running) {
        rows = (((app->frame / 5U) & 1U) != 0U) ? merry_man_player_run_a_rows : merry_man_player_run_b_rows;
    }

    merry_man_draw_sprite(canvas, x, y, MM_PLAYER_W, MM_PLAYER_H, rows, app->facing_left);
}

static void merry_man_draw_enemy(Canvas* canvas, const MerryManEnemy* enemy, int32_t camera_x) {
    const uint16_t* rows = (enemy->type == MerryManEnemyTypeWalker) ? merry_man_walker_rows : merry_man_hopper_rows;
    merry_man_draw_sprite(canvas, enemy->x - camera_x, enemy->y, MM_ENEMY_W, MM_ENEMY_H, rows, enemy->face_left);
}

static void merry_man_draw_collectible(Canvas* canvas, int32_t x, int32_t y, uint32_t frame) {
    const int32_t bob = (int32_t)((frame / 5U) & 1U);
    merry_man_draw_sprite(canvas, x, y + bob, MM_PICKUP_W, MM_PICKUP_H, merry_man_pickup_rows, false);
}

static void merry_man_draw_ground_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x, y, MM_TILE_SIZE, MM_TILE_SIZE);
    canvas_draw_line(canvas, x + 1, y + 2, x + 6, y + 2);
    canvas_draw_line(canvas, x + 2, y + 4, x + 5, y + 4);
    canvas_draw_dot(canvas, x + 2, y + 6);
    canvas_draw_dot(canvas, x + 5, y + 5);
}

static void merry_man_draw_brick_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x, y, MM_TILE_SIZE, MM_TILE_SIZE);
    canvas_draw_line(canvas, x + 1, y + 3, x + 6, y + 3);
    canvas_draw_line(canvas, x + 3, y + 1, x + 3, y + 2);
    canvas_draw_line(canvas, x + 4, y + 4, x + 4, y + 6);
}

static void merry_man_draw_pillar_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_box(canvas, x + 1, y, 6U, MM_TILE_SIZE);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, x + 2, y + 1, x + 2, y + 6);
    canvas_draw_line(canvas, x + 5, y + 1, x + 5, y + 6);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, x + 1, y, 6U, MM_TILE_SIZE);
}

static void merry_man_draw_bridge_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 4, x + 7, y + 4);
    canvas_draw_line(canvas, x + 1, y + 3, x + 1, y + 5);
    canvas_draw_line(canvas, x + 4, y + 3, x + 4, y + 5);
    canvas_draw_line(canvas, x + 6, y + 3, x + 6, y + 5);
}

static void merry_man_draw_spikes(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 3, x + 3, y);
    canvas_draw_line(canvas, x + 3, y, x + 6, y + 3);
    canvas_draw_line(canvas, x + 2, y + 3, x + 5, y);
    canvas_draw_line(canvas, x + 5, y, x + 8, y + 3);
    canvas_draw_line(canvas, x + 4, y + 3, x + 7, y);
    canvas_draw_line(canvas, x + 7, y, x + 10, y + 3);
}

static void merry_man_draw_bush(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x + 2, y + 2, 8U, 4U);
    canvas_draw_frame(canvas, x, y + 4, 6U, 3U);
    canvas_draw_frame(canvas, x + 6, y + 4, 7U, 3U);
}

static void merry_man_draw_sign(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x, y, 8U, 5U);
    canvas_draw_line(canvas, x + 4, y + 5, x + 4, y + 10);
    canvas_draw_line(canvas, x + 3, y + 10, x + 5, y + 10);
}

static void merry_man_draw_rock(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x + 1, y + 1, 6U, 4U);
    canvas_draw_dot(canvas, x + 3, y + 2);
    canvas_draw_dot(canvas, x + 5, y + 3);
}

static void merry_man_draw_mound(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 4, x + 4, y + 1);
    canvas_draw_line(canvas, x + 4, y + 1, x + 8, y + 4);
}

static void merry_man_draw_cloud(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x + 2, y + 1, 9U, 4U);
    canvas_draw_frame(canvas, x, y + 3, 6U, 3U);
    canvas_draw_frame(canvas, x + 7, y + 3, 7U, 3U);
}

static void merry_man_draw_hill(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 8, x + 6, y + 2);
    canvas_draw_line(canvas, x + 6, y + 2, x + 12, y + 8);
    canvas_draw_line(canvas, x + 2, y + 8, x + 10, y + 8);
}

static void merry_man_draw_background(Canvas* canvas, int32_t camera_x) {
    const int32_t first_segment = (camera_x / MM_SEGMENT_PIXEL_W) - 1;
    const int32_t last_segment = ((camera_x + MM_SCREEN_W) / MM_SEGMENT_PIXEL_W) + 1;
    int32_t segment;

    for(segment = first_segment; segment <= last_segment; segment++) {
        const uint32_t sky = merry_man_hash32((uint32_t)segment + 0x5151AA11U);
        const int32_t cloud_x = (segment * MM_SEGMENT_PIXEL_W) + 8 + (int32_t)(sky % 40U) - camera_x;
        const int32_t cloud_y = 5 + (int32_t)((sky >> 8) % 10U);
        const int32_t hill_x = (segment * MM_SEGMENT_PIXEL_W) + 24 + (int32_t)((sky >> 14) % 48U) - camera_x;

        if(((sky & 1U) == 0U) && (cloud_x > -16) && (cloud_x < (MM_SCREEN_W + 8))) {
            merry_man_draw_cloud(canvas, cloud_x, cloud_y);
        }
        if((((sky >> 3) & 1U) == 0U) && (hill_x > -20) && (hill_x < (MM_SCREEN_W + 12))) {
            merry_man_draw_hill(canvas, hill_x, 42);
        }
    }
}

static void merry_man_draw_world(Canvas* canvas, const MerryManApp* app, int32_t camera_x) {
    const int32_t world_column_start = camera_x / MM_TILE_SIZE;
    const int32_t visible_segments_first = (camera_x / MM_SEGMENT_PIXEL_W) - 1;
    const int32_t visible_segments_last = ((camera_x + MM_SCREEN_W) / MM_SEGMENT_PIXEL_W) + 1;
    int32_t screen_column;
    int32_t segment;

    merry_man_draw_background(canvas, camera_x);

    for(screen_column = -1; screen_column <= ((MM_SCREEN_W / MM_TILE_SIZE) + 1); screen_column++) {
        const int32_t world_column = world_column_start + screen_column;
        const int32_t screen_x = (world_column * MM_TILE_SIZE) - camera_x;
        int32_t row;

        for(row = 0; row < MM_WORLD_ROWS; row++) {
            const MerryManTile tile = merry_man_tile_at(world_column, row);
            const int32_t screen_y = row * MM_TILE_SIZE;

            switch(tile) {
            case MerryManTileGround:
                merry_man_draw_ground_tile(canvas, screen_x, screen_y);
                break;
            case MerryManTileBrick:
                merry_man_draw_brick_tile(canvas, screen_x, screen_y);
                break;
            case MerryManTilePillar:
                merry_man_draw_pillar_tile(canvas, screen_x, screen_y);
                break;
            case MerryManTileBridge:
                merry_man_draw_bridge_tile(canvas, screen_x, screen_y);
                break;
            default:
                break;
            }
        }
    }

    for(segment = visible_segments_first; segment <= visible_segments_last; segment++) {
        const int32_t base_x = (segment * MM_SEGMENT_PIXEL_W) - camera_x;
        const MerryManDeco deco = merry_man_deco_for_segment(segment);

        switch(deco) {
        case MerryManDecoBush:
            merry_man_draw_bush(canvas, base_x + 28, 49);
            break;
        case MerryManDecoSign:
            merry_man_draw_sign(canvas, base_x + 36, 46);
            break;
        case MerryManDecoRock:
            merry_man_draw_rock(canvas, base_x + 60, 51);
            break;
        case MerryManDecoMound:
            merry_man_draw_mound(canvas, base_x + 48, 48);
            break;
        default:
            break;
        }

        if(merry_man_segment_feature(segment) == MerryManSegmentSpikes) {
            merry_man_draw_spikes(canvas, base_x + 64, 52);
        }
    }

    for(segment = visible_segments_first; segment <= visible_segments_last; segment++) {
        int32_t item_x;
        int32_t item_y;

        if(!merry_man_segment_has_collectible(segment)) continue;
        if(merry_man_collectible_lookup(app, segment)) continue;

        merry_man_collectible_position(segment, &item_x, &item_y);
        item_x -= camera_x;
        if((item_x > -12) && (item_x < (MM_SCREEN_W + 8))) {
            merry_man_draw_collectible(canvas, item_x, item_y, app->frame);
        }
    }

    for(segment = visible_segments_first; segment <= visible_segments_last; segment++) {
        MerryManEnemy enemy;

        if(!merry_man_generated_enemy(app, segment, app->frame, &enemy)) continue;
        if((enemy.x - camera_x > -12) && (enemy.x - camera_x < (MM_SCREEN_W + 8))) {
            merry_man_draw_enemy(canvas, &enemy, camera_x);
        }
    }
}

static void merry_man_reset_slots(MerryManApp* app) {
    uint32_t index;

    for(index = 0U; index < MM_COLLECTIBLE_SLOTS; index++) {
        app->collectible_segments[index] = INT32_MIN;
        app->collectible_collected[index] = false;
    }

    for(index = 0U; index < MM_ENEMY_SLOTS; index++) {
        app->enemy_segments[index] = INT32_MIN;
        app->enemy_defeated[index] = false;
    }

    for(index = 0U; index < MM_FLOATING_LABELS; index++) {
        app->label_timer[index] = 0;
        app->label_value[index] = 0;
        app->label_x[index] = 0;
        app->label_y[index] = 0;
    }
}

static void merry_man_start_run(MerryManApp* app) {
    app->left_held = false;
    app->right_held = false;
    app->jump_pressed = false;
    app->grounded = true;
    app->facing_left = false;

    app->mode = MerryManModePlaying;
    app->player_x_q8 = MM_PLAYER_START_X << 8;
    app->player_y_q8 = MM_PLAYER_START_Y << 8;
    app->player_vx_q8 = 0;
    app->player_vy_q8 = 0;
    app->frame = 0U;
    app->farthest_distance = 0U;
    app->collectible_score = 0U;
    app->enemy_score = 0U;

    merry_man_reset_slots(app);
}

static void merry_man_finish_run(MerryManApp* app) {
    app->mode = MerryManModeGameOver;
    if(app->farthest_distance > app->best_distance) {
        app->best_distance = app->farthest_distance;
    }
}

static void merry_man_collectibles_step(MerryManApp* app) {
    const int32_t player_x = app->player_x_q8 >> 8;
    const int32_t player_y = app->player_y_q8 >> 8;
    const int32_t current_segment = player_x / MM_SEGMENT_PIXEL_W;
    int32_t segment;

    for(segment = current_segment - 1; segment <= current_segment + 2; segment++) {
        int32_t item_x;
        int32_t item_y;

        if(segment < 0) continue;
        if(!merry_man_segment_has_collectible(segment)) continue;
        if(merry_man_collectible_is_collected(app, segment)) continue;

        merry_man_collectible_position(segment, &item_x, &item_y);
        if(merry_man_rects_overlap(player_x, player_y, MM_PLAYER_W, MM_PLAYER_H, item_x, item_y, MM_PICKUP_W, MM_PICKUP_H)) {
            merry_man_mark_collectible(app, segment);
            merry_man_add_label(app, item_x, item_y, 5);
        }
    }
}

static void merry_man_enemies_step(MerryManApp* app) {
    const int32_t player_x = app->player_x_q8 >> 8;
    const int32_t player_y = app->player_y_q8 >> 8;
    const int32_t player_bottom = player_y + MM_PLAYER_H;
    const int32_t current_segment = player_x / MM_SEGMENT_PIXEL_W;
    int32_t segment;

    for(segment = current_segment - 1; segment <= current_segment + 2; segment++) {
        MerryManEnemy enemy;

        if(segment < 0) continue;
        if(!merry_man_generated_enemy(app, segment, app->frame, &enemy)) continue;

        if(merry_man_rects_overlap(player_x, player_y, MM_PLAYER_W, MM_PLAYER_H, enemy.x, enemy.y, enemy.w, enemy.h)) {
            const bool stomp = (app->player_vy_q8 > 0) && (player_bottom <= (enemy.y + 4));
            if(stomp) {
                merry_man_mark_enemy_defeated(app, segment);
                app->player_vy_q8 = MM_STOMP_BOUNCE_Q8;
                app->grounded = false;
                merry_man_add_label(app, enemy.x, enemy.y - 2, 10);
            } else {
                merry_man_finish_run(app);
            }
            return;
        }
    }
}

static void merry_man_move_player(MerryManApp* app) {
    int32_t old_x;
    int32_t new_x;
    int32_t step;

    if(app->left_held && !app->right_held) {
        app->player_vx_q8 -= MM_ACCEL_Q8;
    } else if(app->right_held && !app->left_held) {
        app->player_vx_q8 += MM_ACCEL_Q8;
    } else if(app->player_vx_q8 != 0) {
        const int32_t drag = merry_man_sign_i32(app->player_vx_q8) * MM_FRICTION_Q8;
        if(merry_man_abs_i32(app->player_vx_q8) <= MM_FRICTION_Q8) {
            app->player_vx_q8 = 0;
        } else {
            app->player_vx_q8 -= drag;
        }
    }

    app->player_vx_q8 = merry_man_clamp_i32(app->player_vx_q8, -MM_MAX_RUN_Q8, MM_MAX_RUN_Q8);

    if(app->player_vx_q8 < -32) app->facing_left = true;
    if(app->player_vx_q8 > 32) app->facing_left = false;

    if(app->jump_pressed && app->grounded) {
        app->player_vy_q8 = MM_JUMP_Q8;
        app->grounded = false;
    }
    app->jump_pressed = false;

    old_x = app->player_x_q8 >> 8;
    app->player_x_q8 += app->player_vx_q8;
    if(app->player_x_q8 < 0) {
        app->player_x_q8 = 0;
        app->player_vx_q8 = 0;
    }
    new_x = app->player_x_q8 >> 8;

    if(new_x != old_x) {
        const int32_t direction = (new_x > old_x) ? 1 : -1;
        int32_t test_x;
        for(test_x = old_x + direction; test_x != (new_x + direction); test_x += direction) {
            if(merry_man_world_rect_collides(test_x, app->player_y_q8 >> 8, MM_PLAYER_W, MM_PLAYER_H)) {
                app->player_x_q8 = (test_x - direction) << 8;
                app->player_vx_q8 = 0;
                break;
            }
        }
    }

    app->player_vy_q8 += MM_GRAVITY_Q8;
    app->player_vy_q8 = merry_man_clamp_i32(app->player_vy_q8, -2048, MM_MAX_FALL_Q8);

    old_x = app->player_y_q8 >> 8;
    app->player_y_q8 += app->player_vy_q8;
    new_x = app->player_y_q8 >> 8;
    app->grounded = false;

    if(new_x > old_x) {
        for(step = old_x + 1; step <= new_x; step++) {
            if(merry_man_world_rect_collides(app->player_x_q8 >> 8, step, MM_PLAYER_W, MM_PLAYER_H)) {
                app->player_y_q8 = (step - 1) << 8;
                app->player_vy_q8 = 0;
                app->grounded = true;
                break;
            }
        }
    } else if(new_x < old_x) {
        for(step = old_x - 1; step >= new_x; step--) {
            if(merry_man_world_rect_collides(app->player_x_q8 >> 8, step, MM_PLAYER_W, MM_PLAYER_H)) {
                app->player_y_q8 = (step + 1) << 8;
                app->player_vy_q8 = 0;
                break;
            }
        }
    } else if(merry_man_world_rect_collides(app->player_x_q8 >> 8, new_x + 1, MM_PLAYER_W, MM_PLAYER_H)) {
        app->grounded = true;
    }
}

static void merry_man_tick_labels(MerryManApp* app) {
    uint32_t index;
    for(index = 0U; index < MM_FLOATING_LABELS; index++) {
        if(app->label_timer[index] > 0) {
            app->label_timer[index]--;
        }
    }
}

static void merry_man_step_locked(MerryManApp* app) {
    const int32_t player_x_before = app->player_x_q8 >> 8;

    app->frame++;
    merry_man_tick_labels(app);

    if(app->mode != MerryManModePlaying) return;

    merry_man_move_player(app);

    if(merry_man_world_rect_hits_hazard(app->player_x_q8 >> 8, app->player_y_q8 >> 8, MM_PLAYER_W, MM_PLAYER_H)) {
        merry_man_finish_run(app);
        return;
    }

    merry_man_collectibles_step(app);
    merry_man_enemies_step(app);
    if(app->mode != MerryManModePlaying) return;

    if((app->player_y_q8 >> 8) > (MM_SCREEN_H + 10)) {
        merry_man_finish_run(app);
        return;
    }

    if(merry_man_distance_from_world_x(player_x_before) > app->farthest_distance) {
        app->farthest_distance = merry_man_distance_from_world_x(player_x_before);
    }
    if(merry_man_distance_from_world_x(app->player_x_q8 >> 8) > app->farthest_distance) {
        app->farthest_distance = merry_man_distance_from_world_x(app->player_x_q8 >> 8);
    }
}

static void merry_man_draw_labels(Canvas* canvas, const MerryManApp* app, int32_t camera_x) {
    uint32_t index;
    char buffer[8];

    canvas_set_font(canvas, FontSecondary);
    for(index = 0U; index < MM_FLOATING_LABELS; index++) {
        if(app->label_timer[index] <= 0) continue;
        snprintf(buffer, sizeof(buffer), "+%d", app->label_value[index]);
        canvas_draw_str(
            canvas,
            app->label_x[index] - camera_x,
            app->label_y[index] - ((20 - app->label_timer[index]) / 2),
            buffer);
    }
}

static void merry_man_draw_callback(Canvas* canvas, void* context) {
    MerryManApp* app = (MerryManApp*)context;
    int32_t player_x;
    int32_t player_y;
    int32_t camera_x;
    uint32_t next_goal;
    char line_left[24];
    char line_right[24];
    char line_bottom[32];

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    if(app->mode == MerryManModeTitle) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 8, AlignCenter, AlignTop, "MERRY MAN");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 21, AlignCenter, AlignTop, "1-bit platform runner");
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 31, AlignCenter, AlignTop, "LEFT/RIGHT move");
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 39, AlignCenter, AlignTop, "UP/OK jump");
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 47, AlignCenter, AlignTop, "BACK exit  OK start");
        merry_man_draw_player(canvas, app, 18, 42);
        merry_man_draw_bush(canvas, 30, 49);
        merry_man_draw_collectible(canvas, 70, 28, app->frame);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 57, AlignCenter, AlignBottom, "20 / 50 / 90 / 140+");
        furi_mutex_release(app->mutex);
        return;
    }

    player_x = app->player_x_q8 >> 8;
    player_y = app->player_y_q8 >> 8;
    camera_x = merry_man_clamp_i32(player_x - MM_CAMERA_LEAD_X, 0, INT_MAX);
    next_goal = merry_man_next_goal(app->farthest_distance);

    merry_man_draw_world(canvas, app, camera_x);
    merry_man_draw_player(canvas, app, player_x - camera_x, player_y);
    merry_man_draw_labels(canvas, app, camera_x);

    canvas_draw_line(canvas, 0, 10, MM_SCREEN_W - 1, 10);
    canvas_set_font(canvas, FontSecondary);
    snprintf(line_left, sizeof(line_left), "DST %lu", (unsigned long)app->farthest_distance);
    snprintf(line_right, sizeof(line_right), "OBJ %lu", (unsigned long)app->collectible_score);
    snprintf(line_bottom, sizeof(line_bottom), "%s %lu", merry_man_rank_title(app->farthest_distance), (unsigned long)next_goal);
    canvas_draw_str(canvas, 2, 8, line_left);
    canvas_draw_str_aligned(canvas, MM_SCREEN_W - 2, 8, AlignRight, AlignBottom, line_right);
    canvas_draw_str(canvas, 2, 63, line_bottom);

    if(app->mode == MerryManModeGameOver) {
        char best_buffer[24];
        char score_buffer[24];
        char rank_buffer[24];

        canvas_draw_rframe(canvas, 16, 17, 96U, 31U, 3U);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 22, AlignCenter, AlignTop, "RUN OVER");
        snprintf(best_buffer, sizeof(best_buffer), "BEST %lu", (unsigned long)app->best_distance);
        snprintf(score_buffer, sizeof(score_buffer), "SCORE %lu", (unsigned long)merry_man_total_score(app));
        snprintf(rank_buffer, sizeof(rank_buffer), "%s", merry_man_rank_title(app->farthest_distance));
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 31, AlignCenter, AlignTop, best_buffer);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 38, AlignCenter, AlignTop, score_buffer);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 45, AlignCenter, AlignTop, rank_buffer);
        canvas_draw_str_aligned(canvas, MM_SCREEN_W / 2, 54, AlignCenter, AlignBottom, "OK retry");
    }

    furi_mutex_release(app->mutex);
}

static void merry_man_input_callback(InputEvent* event, void* context) {
    MerryManApp* app = (MerryManApp*)context;
    furi_message_queue_put(app->input_queue, event, 0U);
}

static void merry_man_handle_input_locked(MerryManApp* app, const InputEvent* event) {
    if((event->key == InputKeyBack) && (event->type == InputTypeShort)) {
        app->running = false;
        return;
    }

    if(app->mode == MerryManModeTitle) {
        if((event->key == InputKeyOk) &&
           ((event->type == InputTypeShort) || (event->type == InputTypePress))) {
            merry_man_start_run(app);
        }
        return;
    }

    if(app->mode == MerryManModeGameOver) {
        if((event->key == InputKeyOk) &&
           ((event->type == InputTypeShort) || (event->type == InputTypePress))) {
            merry_man_start_run(app);
        }
        return;
    }

    if(event->key == InputKeyLeft) {
        if(event->type == InputTypePress) app->left_held = true;
        if(event->type == InputTypeRelease) app->left_held = false;
    } else if(event->key == InputKeyRight) {
        if(event->type == InputTypePress) app->right_held = true;
        if(event->type == InputTypeRelease) app->right_held = false;
    } else if((event->key == InputKeyUp) || (event->key == InputKeyOk)) {
        if((event->type == InputTypePress) || (event->type == InputTypeShort)) {
            app->jump_pressed = true;
        }
    }
}

int32_t merry_man_app(void* p) {
    MerryManApp app;
    InputEvent event;

    UNUSED(p);

    memset(&app, 0, sizeof(app));

    app.gui = furi_record_open(RECORD_GUI);
    app.view_port = view_port_alloc();
    app.input_queue = furi_message_queue_alloc(MM_QUEUE_DEPTH, sizeof(InputEvent));
    app.mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    if((app.gui == NULL) || (app.view_port == NULL) || (app.input_queue == NULL) || (app.mutex == NULL)) {
        if(app.mutex != NULL) furi_mutex_free(app.mutex);
        if(app.input_queue != NULL) furi_message_queue_free(app.input_queue);
        if(app.view_port != NULL) view_port_free(app.view_port);
        if(app.gui != NULL) furi_record_close(RECORD_GUI);
        return -1;
    }

    app.running = true;
    app.mode = MerryManModeTitle;
    app.grounded = true;
    app.player_x_q8 = MM_PLAYER_START_X << 8;
    app.player_y_q8 = MM_PLAYER_START_Y << 8;
    merry_man_reset_slots(&app);

    view_port_draw_callback_set(app.view_port, merry_man_draw_callback, &app);
    view_port_input_callback_set(app.view_port, merry_man_input_callback, &app);
    gui_add_view_port(app.gui, app.view_port, GuiLayerFullscreen);

    while(app.running) {
        while(furi_message_queue_get(app.input_queue, &event, 0U) == FuriStatusOk) {
            furi_mutex_acquire(app.mutex, FuriWaitForever);
            merry_man_handle_input_locked(&app, &event);
            furi_mutex_release(app.mutex);
        }

        furi_mutex_acquire(app.mutex, FuriWaitForever);
        merry_man_step_locked(&app);
        furi_mutex_release(app.mutex);

        view_port_update(app.view_port);
        furi_delay_ms(MM_FRAME_MS);
    }

    view_port_enabled_set(app.view_port, false);
    gui_remove_view_port(app.gui, app.view_port);
    view_port_free(app.view_port);
    furi_message_queue_free(app.input_queue);
    furi_mutex_free(app.mutex);
    furi_record_close(RECORD_GUI);

    return 0;
}
