#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SCREEN_W 128
#define SCREEN_H 64
#define TILE 8

#define PLAYER_W 10
#define PLAYER_H 12
#define ENEMY_W 8
#define ENEMY_H 8
#define PICKUP_W 7
#define PICKUP_H 7

#define FRAME_MS 33

#define PLAYER_START_X 16
#define PLAYER_START_Y 44
#define CAMERA_LEAD_X 40

#define MOVE_ACCEL_Q8 96
#define MOVE_FRICTION_Q8 80
#define MAX_RUN_Q8 448
#define JUMP_SPEED_Q8 -1312
#define GRAVITY_Q8 104
#define MAX_FALL_Q8 1536
#define STOMP_BOUNCE_Q8 -864

#define MAX_PICKUP_SLOTS 128
#define MAX_ENEMY_SLOTS 128
#define MAX_SCORE_POPS 4

typedef enum {
    GameModeTitle,
    GameModePlaying,
    GameModeGameOver,
} GameMode;

typedef enum {
    ChunkFlat = 0,
    ChunkBush,
    ChunkPillar,
    ChunkStairs,
    ChunkFloating,
    ChunkGap2,
    ChunkGap3,
    ChunkBridgeGap,
    ChunkSpikes,
    ChunkTower,
} ChunkFeature;

typedef enum {
    TileNone = 0,
    TileGround,
    TileBrick,
    TileColumn,
    TileStep,
    TileBridge,
} TileKind;

typedef enum {
    DecoNone = 0,
    DecoBush,
    DecoSign,
    DecoRock,
    DecoMound,
} DecoKind;

typedef enum {
    EnemyNone = 0,
    EnemyWalker,
    EnemyHopper,
} EnemyType;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;

    bool running;
    bool left_held;
    bool right_held;
    bool jump_queued;
    bool grounded;
    bool facing_left;

    GameMode mode;

    int32_t player_x_q8;
    int32_t player_y_q8;
    int32_t player_vx_q8;
    int32_t player_vy_q8;

    uint32_t frame;
    uint32_t pickup_score;
    uint32_t stomp_score;
    uint32_t farthest_distance;
    uint32_t best_distance;

    int32_t pickup_slot_chunk[MAX_PICKUP_SLOTS];
    bool pickup_slot_collected[MAX_PICKUP_SLOTS];

    int32_t enemy_slot_chunk[MAX_ENEMY_SLOTS];
    bool enemy_slot_defeated[MAX_ENEMY_SLOTS];

    int16_t score_pop_x[MAX_SCORE_POPS];
    int16_t score_pop_y[MAX_SCORE_POPS];
    int16_t score_pop_timer[MAX_SCORE_POPS];
    int16_t score_pop_value[MAX_SCORE_POPS];
} MerryManApp;

typedef struct {
    EnemyType type;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t chunk;
} EnemyState;

typedef struct {
    uint32_t distance;
    const char* title;
} RankGoal;

static const RankGoal rank_goals[] = {
    {0, "Rookie"},
    {20, "Scout"},
    {50, "Ranger"},
    {90, "Trailblazer"},
    {140, "Hero"},
    {220, "Legend"},
};

static const uint16_t player_idle_rows[PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07C, 0x0DE, 0x0D2, 0x186, 0x102,
};

static const uint16_t player_run_a_rows[PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07E, 0x0DA, 0x0D0, 0x188, 0x104,
};

static const uint16_t player_run_b_rows[PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07C, 0x0DE, 0x08A, 0x106, 0x080,
};

static const uint16_t player_jump_rows[PLAYER_H] = {
    0x078, 0x0FC, 0x1FE, 0x1B6, 0x1FE, 0x0F8, 0x06C, 0x07C, 0x0DE, 0x192, 0x102, 0x144,
};

static const uint16_t walker_rows[ENEMY_H] = {
    0x3C, 0x7E, 0xDB, 0xFF, 0x7E, 0x24, 0x66, 0x42,
};

static const uint16_t hopper_rows[ENEMY_H] = {
    0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x7E, 0x24, 0x42,
};

static const uint16_t pickup_rows[PICKUP_H] = {
    0x08, 0x1C, 0x3E, 0x2A, 0x3E, 0x1C, 0x08,
};

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static int32_t abs_i32(int32_t v) {
    return (v < 0) ? -v : v;
}

static int32_t clamp_i32(int32_t value, int32_t min_value, int32_t max_value) {
    if(value < min_value) return min_value;
    if(value > max_value) return max_value;
    return value;
}

static int32_t sign_i32(int32_t value) {
    if(value < 0) return -1;
    if(value > 0) return 1;
    return 0;
}

static uint32_t ping_pong_u32(uint32_t value, uint32_t span) {
    const uint32_t cycle = span * 2U;
    uint32_t t;

    if(cycle == 0U) return 0U;
    t = value % cycle;
    if(t > span) t = cycle - t;
    return t;
}

static bool rects_overlap(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t bx, int32_t by, int32_t bw, int32_t bh) {
    return (ax < (bx + bw)) && ((ax + aw) > bx) && (ay < (by + bh)) && ((ay + ah) > by);
}

static uint32_t distance_from_x(int32_t world_x) {
    if(world_x <= 0) return 0;
    return (uint32_t)(world_x / 16);
}

static const char* get_rank_title(uint32_t distance) {
    size_t i;
    const char* title = rank_goals[0].title;

    for(i = 0; i < (sizeof(rank_goals) / sizeof(rank_goals[0])); i++) {
        if(distance >= rank_goals[i].distance) {
            title = rank_goals[i].title;
        } else {
            break;
        }
    }

    return title;
}

static uint32_t get_next_goal(uint32_t distance) {
    size_t i;

    for(i = 0; i < (sizeof(rank_goals) / sizeof(rank_goals[0])); i++) {
        if(rank_goals[i].distance > distance) return rank_goals[i].distance;
    }

    return rank_goals[(sizeof(rank_goals) / sizeof(rank_goals[0])) - 1].distance + 80U;
}

static uint32_t merry_total_score(const MerryManApp* app) {
    return app->farthest_distance + (app->pickup_score * 6U) + (app->stomp_score * 10U);
}

static ChunkFeature world_get_chunk_feature(int32_t chunk) {
    const uint32_t roll = hash32((uint32_t)chunk + 0x4D455252U) % 10U;

    if(chunk < 2) return ChunkFlat;
    if(chunk == 2) return ChunkBush;

    switch(roll) {
    case 0:
        return ChunkFlat;
    case 1:
        return ChunkBush;
    case 2:
        return ChunkPillar;
    case 3:
        return ChunkStairs;
    case 4:
        return ChunkFloating;
    case 5:
        return ChunkGap2;
    case 6:
        return ChunkGap3;
    case 7:
        return ChunkBridgeGap;
    case 8:
        return ChunkSpikes;
    default:
        return ChunkTower;
    }
}

static bool world_ground_exists(int32_t column) {
    const int32_t chunk = column / 16;
    const int32_t local = column % 16;
    const ChunkFeature feature = world_get_chunk_feature(chunk);

    switch(feature) {
    case ChunkGap2:
        if((local >= 7) && (local <= 8)) return false;
        break;
    case ChunkGap3:
        if((local >= 6) && (local <= 8)) return false;
        break;
    case ChunkBridgeGap:
        if((local >= 5) && (local <= 10)) return false;
        break;
    default:
        break;
    }

    return true;
}

static TileKind world_tile_kind(int32_t column, int32_t row) {
    const int32_t chunk = column / 16;
    const int32_t local = column % 16;
    const ChunkFeature feature = world_get_chunk_feature(chunk);

    if((row == 7) && world_ground_exists(column)) return TileGround;

    switch(feature) {
    case ChunkPillar:
        if((local >= 10) && (local <= 11) && (row >= 5) && (row <= 6)) return TileColumn;
        break;
    case ChunkStairs:
        if((local == 9) && (row == 6)) return TileStep;
        if((local == 10) && (row >= 5) && (row <= 6)) return TileStep;
        if((local == 11) && (row >= 4) && (row <= 6)) return TileStep;
        if((local == 12) && (row >= 3) && (row <= 6)) return TileStep;
        break;
    case ChunkFloating:
        if((local >= 5) && (local <= 7) && (row == 4)) return TileBrick;
        if((local >= 11) && (local <= 12) && (row == 5)) return TileBrick;
        break;
    case ChunkBridgeGap:
        if((local >= 3) && (local <= 4) && (row == 5)) return TileBridge;
        if((local >= 10) && (local <= 11) && (row == 4)) return TileBridge;
        if((local >= 13) && (local <= 14) && (row == 5)) return TileBridge;
        break;
    case ChunkTower:
        if((local >= 9) && (local <= 11) && (row >= 4) && (row <= 6)) return TileColumn;
        if((local >= 7) && (local <= 13) && (row == 3)) return TileBrick;
        break;
    default:
        break;
    }

    return TileNone;
}

static DecoKind world_get_deco_kind(int32_t chunk) {
    const ChunkFeature feature = world_get_chunk_feature(chunk);
    const uint32_t deco = (hash32((uint32_t)chunk + 0xD3C01234U) >> 3) % 4U;

    if(feature == ChunkBush) return DecoBush;
    if(feature == ChunkSpikes) return DecoRock;
    if(feature == ChunkFlat && deco == 0U) return DecoSign;
    if(feature == ChunkFlat && deco == 1U) return DecoMound;
    if(feature == ChunkPillar && deco == 2U) return DecoRock;

    return DecoNone;
}

static bool world_solid_pixel(int32_t world_x, int32_t world_y) {
    int32_t column;
    int32_t row;

    if((world_x < 0) || (world_y < 0) || (world_y >= SCREEN_H)) return false;

    column = world_x / TILE;
    row = world_y / TILE;

    return world_tile_kind(column, row) != TileNone;
}

static bool world_hazard_pixel(int32_t world_x, int32_t world_y) {
    const int32_t column = world_x / TILE;
    const int32_t chunk = column / 16;
    const int32_t local = column % 16;
    const int32_t px = world_x % TILE;
    const int32_t py = world_y - 52;

    if((world_x < 0) || (world_y < 0) || (world_y >= SCREEN_H)) return false;

    if(world_get_chunk_feature(chunk) != ChunkSpikes) return false;
    if((local < 8) || (local > 10)) return false;
    if((world_y < 52) || (world_y > 55)) return false;

    return py >= (2 - abs_i32(px - 3) / 2);
}

static bool world_rect_collides(int32_t world_x, int32_t world_y, int32_t w, int32_t h) {
    const int32_t left = world_x;
    const int32_t right = world_x + w - 1;
    const int32_t top = world_y;
    const int32_t bottom = world_y + h - 1;
    const int32_t mid_x = world_x + (w / 2);
    const int32_t mid_y = world_y + (h / 2);

    return world_solid_pixel(left, top) || world_solid_pixel(right, top) ||
           world_solid_pixel(left, bottom) || world_solid_pixel(right, bottom) ||
           world_solid_pixel(mid_x, top) || world_solid_pixel(mid_x, bottom) ||
           world_solid_pixel(left, mid_y) || world_solid_pixel(right, mid_y);
}

static bool world_rect_hits_hazard(int32_t world_x, int32_t world_y, int32_t w, int32_t h) {
    const int32_t left = world_x;
    const int32_t right = world_x + w - 1;
    const int32_t top = world_y;
    const int32_t bottom = world_y + h - 1;
    const int32_t mid_x = world_x + (w / 2);
    const int32_t mid_y = world_y + (h / 2);

    return world_hazard_pixel(left, top) || world_hazard_pixel(right, top) ||
           world_hazard_pixel(left, bottom) || world_hazard_pixel(right, bottom) ||
           world_hazard_pixel(mid_x, top) || world_hazard_pixel(mid_x, bottom) ||
           world_hazard_pixel(left, mid_y) || world_hazard_pixel(right, mid_y);
}

static int32_t world_find_surface_y(int32_t world_x, int32_t width) {
    int32_t y;
    int32_t dx;

    for(y = 0; y < SCREEN_H; y++) {
        for(dx = 0; dx < width; dx++) {
            if(world_solid_pixel(world_x + dx, y)) return y;
        }
    }

    return -1;
}

static uint32_t pickup_slot_index(int32_t chunk) {
    return (uint32_t)chunk % MAX_PICKUP_SLOTS;
}

static uint32_t enemy_slot_index(int32_t chunk) {
    return (uint32_t)chunk % MAX_ENEMY_SLOTS;
}

static bool merry_pickup_lookup(const MerryManApp* app, int32_t chunk) {
    const uint32_t slot = pickup_slot_index(chunk);
    return (app->pickup_slot_chunk[slot] == chunk) && app->pickup_slot_collected[slot];
}

static bool merry_pickup_is_collected(MerryManApp* app, int32_t chunk) {
    const uint32_t slot = pickup_slot_index(chunk);

    if(app->pickup_slot_chunk[slot] != chunk) {
        app->pickup_slot_chunk[slot] = chunk;
        app->pickup_slot_collected[slot] = false;
    }

    return app->pickup_slot_collected[slot];
}

static void merry_collect_pickup(MerryManApp* app, int32_t chunk) {
    const uint32_t slot = pickup_slot_index(chunk);
    app->pickup_slot_chunk[slot] = chunk;
    app->pickup_slot_collected[slot] = true;
    app->pickup_score += 1U;
}

static bool merry_enemy_lookup(const MerryManApp* app, int32_t chunk) {
    const uint32_t slot = enemy_slot_index(chunk);
    return (app->enemy_slot_chunk[slot] == chunk) && app->enemy_slot_defeated[slot];
}

static bool merry_enemy_is_defeated(MerryManApp* app, int32_t chunk) {
    const uint32_t slot = enemy_slot_index(chunk);

    if(app->enemy_slot_chunk[slot] != chunk) {
        app->enemy_slot_chunk[slot] = chunk;
        app->enemy_slot_defeated[slot] = false;
    }

    return app->enemy_slot_defeated[slot];
}

static void merry_defeat_enemy(MerryManApp* app, int32_t chunk) {
    const uint32_t slot = enemy_slot_index(chunk);
    app->enemy_slot_chunk[slot] = chunk;
    app->enemy_slot_defeated[slot] = true;
    app->stomp_score += 1U;
}

static void merry_add_score_pop(MerryManApp* app, int32_t world_x, int32_t world_y, int16_t value) {
    uint32_t i;
    uint32_t best = 0;

    for(i = 0; i < MAX_SCORE_POPS; i++) {
        if(app->score_pop_timer[i] <= 0) {
            best = i;
            break;
        }
        if(app->score_pop_timer[i] < app->score_pop_timer[best]) best = i;
    }

    app->score_pop_x[best] = (int16_t)world_x;
    app->score_pop_y[best] = (int16_t)world_y;
    app->score_pop_timer[best] = 20;
    app->score_pop_value[best] = value;
}

static bool world_chunk_has_pickup(int32_t chunk) {
    const ChunkFeature feature = world_get_chunk_feature(chunk);
    const uint32_t roll = hash32((uint32_t)chunk + 0xA11CE123U) % 4U;

    if(chunk < 1) return false;
    if(feature == ChunkGap3) return roll != 0U;
    if(feature == ChunkBridgeGap || feature == ChunkFloating || feature == ChunkTower) return true;
    return roll == 0U;
}

static void world_pickup_position(int32_t chunk, int32_t* x, int32_t* y) {
    const ChunkFeature feature = world_get_chunk_feature(chunk);
    const int32_t base_x = chunk * 16 * TILE;

    switch(feature) {
    case ChunkFloating:
        *x = base_x + 48;
        *y = 24;
        break;
    case ChunkBridgeGap:
        *x = base_x + 84;
        *y = 20;
        break;
    case ChunkTower:
        *x = base_x + 78;
        *y = 14;
        break;
    case ChunkGap2:
        *x = base_x + 60;
        *y = 28;
        break;
    case ChunkGap3:
        *x = base_x + 64;
        *y = 20;
        break;
    default:
        *x = base_x + 80;
        *y = 30;
        break;
    }
}

static EnemyType world_chunk_enemy_type(int32_t chunk) {
    const ChunkFeature feature = world_get_chunk_feature(chunk);
    const uint32_t roll = hash32((uint32_t)chunk + 0xEE771122U) % 5U;

    if(chunk < 3) return EnemyNone;
    if(feature == ChunkGap3 || feature == ChunkBridgeGap) return EnemyNone;
    if(roll >= 3U) return EnemyNone;
    if(feature == ChunkFloating || feature == ChunkTower) return EnemyHopper;
    if(feature == ChunkStairs && roll == 1U) return EnemyHopper;
    return (roll == 0U) ? EnemyWalker : EnemyHopper;
}

static bool world_get_enemy_state(const MerryManApp* app, int32_t chunk, uint32_t frame, EnemyState* enemy) {
    EnemyType type;
    const int32_t base_x = chunk * 16 * TILE;
    const uint32_t seed = hash32((uint32_t)chunk + 0xBAD5EEDU);
    int32_t anchor_x;
    uint32_t phase;
    uint32_t range;
    uint32_t speed;
    uint32_t t;
    int32_t floor_y;

    if(merry_enemy_lookup(app, chunk)) return false;

    type = world_chunk_enemy_type(chunk);
    if(type == EnemyNone) return false;

    anchor_x = base_x + 40 + (int32_t)((seed >> 8) % 40U);
    phase = (seed >> 16) & 63U;
    range = 8U + ((seed >> 22) % 10U);
    speed = (type == EnemyWalker) ? 2U : 3U;
    t = ping_pong_u32((frame / speed) + phase, range);

    enemy->type = type;
    enemy->w = ENEMY_W;
    enemy->h = ENEMY_H;
    enemy->chunk = chunk;
    enemy->x = anchor_x + (int32_t)t - (int32_t)(range / 2U);

    floor_y = world_find_surface_y(enemy->x + 1, ENEMY_W - 2);
    if(floor_y < 0) return false;

    enemy->y = floor_y - ENEMY_H;

    if(type == EnemyHopper) {
        const uint32_t hop_cycle = (frame + phase * 3U) % 44U;
        int32_t hop_height = 0;

        if(hop_cycle < 10U) {
            hop_height = (int32_t)hop_cycle;
        } else if(hop_cycle < 20U) {
            hop_height = (int32_t)(20U - hop_cycle);
        } else if(hop_cycle >= 28U && hop_cycle < 36U) {
            hop_height = (int32_t)(hop_cycle - 28U) / 2;
        } else if(hop_cycle >= 36U && hop_cycle < 44U) {
            hop_height = (int32_t)(44U - hop_cycle) / 2;
        }

        enemy->y -= hop_height;
        if(((seed >> 5) & 1U) != 0U) {
            if(hop_cycle < 22U) {
                enemy->x += (int32_t)(hop_cycle / 6U);
            } else {
                enemy->x -= (int32_t)((44U - hop_cycle) / 8U);
            }
        }
    }

    return true;
}

static void draw_sprite_rows(Canvas* canvas, int32_t x, int32_t y, uint8_t w, uint8_t h, const uint16_t* rows, bool mirror) {
    uint8_t iy;

    for(iy = 0; iy < h; iy++) {
        uint16_t row_bits = rows[iy];
        uint8_t ix;
        for(ix = 0; ix < w; ix++) {
            const uint8_t src_bit = (uint8_t)(w - 1U - ix);
            const bool on = (row_bits & (1U << src_bit)) != 0U;
            if(on) {
                const int32_t draw_x = x + (mirror ? (w - 1 - ix) : ix);
                canvas_draw_dot(canvas, draw_x, y + iy);
            }
        }
    }
}

static void draw_player(Canvas* canvas, const MerryManApp* app, int32_t x, int32_t y) {
    const uint16_t* rows = player_idle_rows;
    const bool jumping = !app->grounded;
    const bool running = app->grounded && (abs_i32(app->player_vx_q8) > 80);

    if(jumping) {
        rows = player_jump_rows;
    } else if(running) {
        rows = (((app->frame / 5U) & 1U) != 0U) ? player_run_a_rows : player_run_b_rows;
    }

    draw_sprite_rows(canvas, x, y, PLAYER_W, PLAYER_H, rows, app->facing_left);
}

static void draw_enemy(Canvas* canvas, const EnemyState* enemy, int32_t camera_x) {
    const uint16_t* rows = (enemy->type == EnemyWalker) ? walker_rows : hopper_rows;
    draw_sprite_rows(canvas, enemy->x - camera_x, enemy->y, ENEMY_W, ENEMY_H, rows, (enemy->chunk & 1) != 0);
}

static void draw_pickup(Canvas* canvas, int32_t x, int32_t y, uint32_t frame) {
    int32_t bob = (int32_t)((frame / 5U) & 1U);
    draw_sprite_rows(canvas, x, y + bob, PICKUP_W, PICKUP_H, pickup_rows, false);
}

static void draw_ground_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x, y, TILE, TILE);
    canvas_draw_line(canvas, x + 1, y + 2, x + 6, y + 2);
    canvas_draw_line(canvas, x + 2, y + 4, x + 5, y + 4);
    canvas_draw_dot(canvas, x + 2, y + 6);
    canvas_draw_dot(canvas, x + 5, y + 5);
}

static void draw_brick_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x, y, TILE, TILE);
    canvas_draw_line(canvas, x + 1, y + 3, x + 6, y + 3);
    canvas_draw_line(canvas, x + 3, y + 1, x + 3, y + 2);
    canvas_draw_line(canvas, x + 4, y + 4, x + 4, y + 6);
}

static void draw_column_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_box(canvas, x + 1, y, 6, TILE);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, x + 2, y + 1, x + 2, y + 6);
    canvas_draw_line(canvas, x + 5, y + 1, x + 5, y + 6);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, x + 1, y, 6, TILE);
}

static void draw_bridge_tile(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 4, x + 7, y + 4);
    canvas_draw_line(canvas, x + 1, y + 3, x + 1, y + 5);
    canvas_draw_line(canvas, x + 4, y + 3, x + 4, y + 5);
    canvas_draw_line(canvas, x + 6, y + 3, x + 6, y + 5);
}

static void draw_spike_group(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 3, x + 3, y);
    canvas_draw_line(canvas, x + 3, y, x + 6, y + 3);
    canvas_draw_line(canvas, x + 2, y + 3, x + 5, y);
    canvas_draw_line(canvas, x + 5, y, x + 8, y + 3);
    canvas_draw_line(canvas, x + 4, y + 3, x + 7, y);
    canvas_draw_line(canvas, x + 7, y, x + 10, y + 3);
}

static void draw_bush(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x + 2, y + 2, 8, 4);
    canvas_draw_frame(canvas, x, y + 4, 6, 3);
    canvas_draw_frame(canvas, x + 6, y + 4, 7, 3);
}

static void draw_sign(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x, y, 8, 5);
    canvas_draw_line(canvas, x + 4, y + 5, x + 4, y + 10);
    canvas_draw_line(canvas, x + 3, y + 10, x + 5, y + 10);
}

static void draw_rock(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x + 1, y + 1, 6, 4);
    canvas_draw_dot(canvas, x + 3, y + 2);
    canvas_draw_dot(canvas, x + 5, y + 3);
}

static void draw_mound(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 4, x + 4, y + 1);
    canvas_draw_line(canvas, x + 4, y + 1, x + 8, y + 4);
}

static void draw_cloud(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_frame(canvas, x + 2, y + 1, 9, 4);
    canvas_draw_frame(canvas, x, y + 3, 6, 3);
    canvas_draw_frame(canvas, x + 7, y + 3, 7, 3);
}

static void draw_hill(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 8, x + 6, y + 2);
    canvas_draw_line(canvas, x + 6, y + 2, x + 12, y + 8);
    canvas_draw_line(canvas, x + 2, y + 8, x + 10, y + 8);
}

static void merry_draw_background(Canvas* canvas, int32_t camera_x) {
    int32_t chunk;
    int32_t start_chunk = (camera_x / 128) - 1;
    int32_t end_chunk = ((camera_x + SCREEN_W) / 128) + 1;

    for(chunk = start_chunk; chunk <= end_chunk; chunk++) {
        const uint32_t sky = hash32((uint32_t)chunk + 0x5151AA11U);
        const int32_t cloud_x = (chunk * 128) + 8 + (int32_t)(sky % 40U) - camera_x;
        const int32_t cloud_y = 5 + (int32_t)((sky >> 8) % 10U);
        const int32_t hill_x = (chunk * 128) + 24 + (int32_t)((sky >> 14) % 48U) - camera_x;

        if((sky & 1U) == 0U && cloud_x > -16 && cloud_x < SCREEN_W + 8) {
            draw_cloud(canvas, cloud_x, cloud_y);
        }
        if(((sky >> 3) & 1U) == 0U && hill_x > -20 && hill_x < SCREEN_W + 12) {
            draw_hill(canvas, hill_x, 42);
        }
    }
}

static void merry_draw_world(Canvas* canvas, const MerryManApp* app, int32_t camera_x) {
    int32_t screen_col;
    int32_t chunk;

    merry_draw_background(canvas, camera_x);

    for(screen_col = -1; screen_col <= (SCREEN_W / TILE) + 1; screen_col++) {
        const int32_t world_col = (camera_x / TILE) + screen_col;
        const int32_t x = world_col * TILE - camera_x;
        int32_t row;

        for(row = 0; row < 8; row++) {
            const TileKind tile = world_tile_kind(world_col, row);
            switch(tile) {
            case TileGround:
                draw_ground_tile(canvas, x, row * TILE);
                break;
            case TileBrick:
            case TileStep:
                draw_brick_tile(canvas, x, row * TILE);
                break;
            case TileColumn:
                draw_column_tile(canvas, x, row * TILE);
                break;
            case TileBridge:
                draw_bridge_tile(canvas, x, row * TILE);
                break;
            default:
                break;
            }
        }
    }

    for(chunk = (camera_x / 128) - 1; chunk <= ((camera_x + SCREEN_W) / 128) + 1; chunk++) {
        const DecoKind deco = world_get_deco_kind(chunk);
        const int32_t base_x = chunk * 128 - camera_x;

        switch(deco) {
        case DecoBush:
            draw_bush(canvas, base_x + 28, 49);
            break;
        case DecoSign:
            draw_sign(canvas, base_x + 36, 46);
            break;
        case DecoRock:
            draw_rock(canvas, base_x + 60, 51);
            break;
        case DecoMound:
            draw_mound(canvas, base_x + 48, 48);
            break;
        default:
            break;
        }

        if(world_get_chunk_feature(chunk) == ChunkSpikes) {
            draw_spike_group(canvas, base_x + 64, 52);
        }
    }

    for(chunk = (camera_x / 128) - 1; chunk <= ((camera_x + SCREEN_W) / 128) + 1; chunk++) {
        int32_t pickup_x;
        int32_t pickup_y;

        if(!world_chunk_has_pickup(chunk)) continue;
        if(merry_pickup_lookup(app, chunk)) continue;

        world_pickup_position(chunk, &pickup_x, &pickup_y);
        pickup_x -= camera_x;
        if((pickup_x > -12) && (pickup_x < SCREEN_W + 8)) {
            draw_pickup(canvas, pickup_x, pickup_y, app->frame);
        }
    }

    for(chunk = (camera_x / 128) - 1; chunk <= ((camera_x + SCREEN_W) / 128) + 1; chunk++) {
        EnemyState enemy;
        if(world_get_enemy_state(app, chunk, app->frame, &enemy)) {
            const int32_t screen_x = enemy.x - camera_x;
            if((screen_x > -12) && (screen_x < SCREEN_W + 8)) {
                draw_enemy(canvas, &enemy, camera_x);
            }
        }
    }
}

static void merry_reset_run(MerryManApp* app) {
    uint32_t i;

    app->left_held = false;
    app->right_held = false;
    app->jump_queued = false;
    app->grounded = true;
    app->facing_left = false;

    app->mode = GameModePlaying;
    app->player_x_q8 = PLAYER_START_X << 8;
    app->player_y_q8 = PLAYER_START_Y << 8;
    app->player_vx_q8 = 0;
    app->player_vy_q8 = 0;
    app->frame = 0;
    app->pickup_score = 0;
    app->stomp_score = 0;
    app->farthest_distance = 0;

    for(i = 0; i < MAX_PICKUP_SLOTS; i++) {
        app->pickup_slot_chunk[i] = INT32_MIN;
        app->pickup_slot_collected[i] = false;
    }

    for(i = 0; i < MAX_ENEMY_SLOTS; i++) {
        app->enemy_slot_chunk[i] = INT32_MIN;
        app->enemy_slot_defeated[i] = false;
    }

    for(i = 0; i < MAX_SCORE_POPS; i++) {
        app->score_pop_timer[i] = 0;
        app->score_pop_value[i] = 0;
    }
}

static void merry_kill_player(MerryManApp* app) {
    app->mode = GameModeGameOver;
    if(app->farthest_distance > app->best_distance) app->best_distance = app->farthest_distance;
}

static void merry_handle_pickups(MerryManApp* app) {
    const int32_t player_x = app->player_x_q8 >> 8;
    const int32_t player_y = app->player_y_q8 >> 8;
    const int32_t chunk_now = player_x / 128;
    int32_t chunk;

    for(chunk = chunk_now - 1; chunk <= chunk_now + 2; chunk++) {
        int32_t pickup_x;
        int32_t pickup_y;

        if(chunk < 0) continue;
        if(!world_chunk_has_pickup(chunk)) continue;
        if(merry_pickup_is_collected(app, chunk)) continue;

        world_pickup_position(chunk, &pickup_x, &pickup_y);
        if(rects_overlap(player_x, player_y, PLAYER_W, PLAYER_H, pickup_x, pickup_y, PICKUP_W, PICKUP_H)) {
            merry_collect_pickup(app, chunk);
            merry_add_score_pop(app, pickup_x, pickup_y, 5);
        }
    }
}

static void merry_handle_enemies(MerryManApp* app) {
    const int32_t player_x = app->player_x_q8 >> 8;
    const int32_t player_y = app->player_y_q8 >> 8;
    const int32_t player_bottom = player_y + PLAYER_H;
    const int32_t chunk_now = player_x / 128;
    int32_t chunk;

    for(chunk = chunk_now - 1; chunk <= chunk_now + 2; chunk++) {
        EnemyState enemy;

        if(chunk < 0) continue;
        if(!world_get_enemy_state(app, chunk, app->frame, &enemy)) continue;

        if(rects_overlap(player_x, player_y, PLAYER_W, PLAYER_H, enemy.x, enemy.y, enemy.w, enemy.h)) {
            const bool stomp = (app->player_vy_q8 > 0) && (player_bottom <= enemy.y + 4);
            if(stomp) {
                merry_defeat_enemy(app, chunk);
                app->player_vy_q8 = STOMP_BOUNCE_Q8;
                app->grounded = false;
                merry_add_score_pop(app, enemy.x, enemy.y - 2, 10);
            } else {
                merry_kill_player(app);
            }
            return;
        }
    }
}

static void merry_step_player(MerryManApp* app) {
    int32_t old_px;
    int32_t new_px;
    int32_t step;

    if(app->left_held && !app->right_held) {
        app->player_vx_q8 -= MOVE_ACCEL_Q8;
    } else if(app->right_held && !app->left_held) {
        app->player_vx_q8 += MOVE_ACCEL_Q8;
    } else if(app->player_vx_q8 != 0) {
        const int32_t drag = sign_i32(app->player_vx_q8) * MOVE_FRICTION_Q8;
        if(abs_i32(app->player_vx_q8) <= MOVE_FRICTION_Q8) {
            app->player_vx_q8 = 0;
        } else {
            app->player_vx_q8 -= drag;
        }
    }

    app->player_vx_q8 = clamp_i32(app->player_vx_q8, -MAX_RUN_Q8, MAX_RUN_Q8);

    if(app->player_vx_q8 < -32) app->facing_left = true;
    if(app->player_vx_q8 > 32) app->facing_left = false;

    if(app->jump_queued && app->grounded) {
        app->player_vy_q8 = JUMP_SPEED_Q8;
        app->grounded = false;
    }
    app->jump_queued = false;

    old_px = app->player_x_q8 >> 8;
    app->player_x_q8 += app->player_vx_q8;
    if(app->player_x_q8 < 0) {
        app->player_x_q8 = 0;
        app->player_vx_q8 = 0;
    }
    new_px = app->player_x_q8 >> 8;

    if(new_px != old_px) {
        const int32_t dir = (new_px > old_px) ? 1 : -1;
        int32_t test_x;
        for(test_x = old_px + dir; test_x != new_px + dir; test_x += dir) {
            if(world_rect_collides(test_x, app->player_y_q8 >> 8, PLAYER_W, PLAYER_H)) {
                app->player_x_q8 = (test_x - dir) << 8;
                app->player_vx_q8 = 0;
                break;
            }
        }
    }

    app->player_vy_q8 += GRAVITY_Q8;
    app->player_vy_q8 = clamp_i32(app->player_vy_q8, -2048, MAX_FALL_Q8);

    old_px = app->player_y_q8 >> 8;
    app->player_y_q8 += app->player_vy_q8;
    new_px = app->player_y_q8 >> 8;
    app->grounded = false;

    if(new_px > old_px) {
        for(step = old_px + 1; step <= new_px; step++) {
            if(world_rect_collides(app->player_x_q8 >> 8, step, PLAYER_W, PLAYER_H)) {
                app->player_y_q8 = (step - 1) << 8;
                app->player_vy_q8 = 0;
                app->grounded = true;
                break;
            }
        }
    } else if(new_px < old_px) {
        for(step = old_px - 1; step >= new_px; step--) {
            if(world_rect_collides(app->player_x_q8 >> 8, step, PLAYER_W, PLAYER_H)) {
                app->player_y_q8 = (step + 1) << 8;
                app->player_vy_q8 = 0;
                break;
            }
        }
    } else if(world_rect_collides(app->player_x_q8 >> 8, new_px + 1, PLAYER_W, PLAYER_H)) {
        app->grounded = true;
    }
}

static void merry_tick_score_pops(MerryManApp* app) {
    uint32_t i;
    for(i = 0; i < MAX_SCORE_POPS; i++) {
        if(app->score_pop_timer[i] > 0) app->score_pop_timer[i]--;
    }
}

static void merry_step_locked(MerryManApp* app) {
    const int32_t player_x = app->player_x_q8 >> 8;

    app->frame++;
    merry_tick_score_pops(app);

    if(app->mode != GameModePlaying) return;

    merry_step_player(app);

    if(world_rect_hits_hazard(app->player_x_q8 >> 8, app->player_y_q8 >> 8, PLAYER_W, PLAYER_H)) {
        merry_kill_player(app);
        return;
    }

    merry_handle_pickups(app);
    merry_handle_enemies(app);
    if(app->mode != GameModePlaying) return;

    if((app->player_y_q8 >> 8) > (SCREEN_H + 10)) {
        merry_kill_player(app);
        return;
    }

    if(distance_from_x(player_x) > app->farthest_distance) {
        app->farthest_distance = distance_from_x(player_x);
    }
}

static void merry_draw_score_pops(Canvas* canvas, const MerryManApp* app, int32_t camera_x) {
    uint32_t i;
    char buf[8];

    canvas_set_font(canvas, FontSecondary);
    for(i = 0; i < MAX_SCORE_POPS; i++) {
        if(app->score_pop_timer[i] <= 0) continue;
        snprintf(buf, sizeof(buf), "+%d", app->score_pop_value[i]);
        canvas_draw_str(canvas,
                        app->score_pop_x[i] - camera_x,
                        app->score_pop_y[i] - (20 - app->score_pop_timer[i]) / 2,
                        buf);
    }
}

static void merry_draw_callback(Canvas* canvas, void* context) {
    MerryManApp* app = context;
    int32_t player_x;
    int32_t player_y;
    int32_t camera_x;
    uint32_t next_goal;
    char top_left[24];
    char top_right[24];
    char bottom[32];

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    if(app->mode == GameModeTitle) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 8, AlignCenter, AlignTop, "MERRY MAN");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 21, AlignCenter, AlignTop, "Game Boy style scroller");
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 31, AlignCenter, AlignTop, "LEFT/RIGHT move");
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 39, AlignCenter, AlignTop, "UP/OK jump");
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 47, AlignCenter, AlignTop, "BACK exit  OK start");
        draw_player(canvas, app, 18, 42);
        draw_bush(canvas, 30, 49);
        draw_pickup(canvas, 70, 28, app->frame);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 57, AlignCenter, AlignBottom, "Hit 20 / 50 / 90 / 140+");
        furi_mutex_release(app->mutex);
        return;
    }

    player_x = app->player_x_q8 >> 8;
    player_y = app->player_y_q8 >> 8;
    camera_x = clamp_i32(player_x - CAMERA_LEAD_X, 0, 0x7FFFFFFF);
    next_goal = get_next_goal(app->farthest_distance);

    merry_draw_world(canvas, app, camera_x);
    draw_player(canvas, app, player_x - camera_x, player_y);
    merry_draw_score_pops(canvas, app, camera_x);

    canvas_draw_line(canvas, 0, 10, SCREEN_W - 1, 10);
    canvas_set_font(canvas, FontSecondary);
    snprintf(top_left, sizeof(top_left), "DST %lu", (unsigned long)app->farthest_distance);
    snprintf(top_right, sizeof(top_right), "OBJ %lu", (unsigned long)app->pickup_score);
    snprintf(bottom,
             sizeof(bottom),
             "%s %lu",
             get_rank_title(app->farthest_distance),
             (unsigned long)next_goal);
    canvas_draw_str(canvas, 2, 8, top_left);
    canvas_draw_str_aligned(canvas, SCREEN_W - 2, 8, AlignRight, AlignBottom, top_right);
    canvas_draw_str(canvas, 2, 63, bottom);

    if(app->mode == GameModeGameOver) {
        char line1[24];
        char line2[24];
        char line3[24];

        canvas_draw_rframe(canvas, 16, 17, 96, 31, 3);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 22, AlignCenter, AlignTop, "RUN OVER");
        snprintf(line1, sizeof(line1), "BEST %lu", (unsigned long)app->best_distance);
        snprintf(line2, sizeof(line2), "SCORE %lu", (unsigned long)merry_total_score(app));
        snprintf(line3, sizeof(line3), "%s", get_rank_title(app->farthest_distance));
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 31, AlignCenter, AlignTop, line1);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 38, AlignCenter, AlignTop, line2);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 45, AlignCenter, AlignTop, line3);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 54, AlignCenter, AlignBottom, "OK retry");
    }

    furi_mutex_release(app->mutex);
}

static void merry_input_callback(InputEvent* event, void* context) {
    MerryManApp* app = context;
    furi_message_queue_put(app->input_queue, event, 0);
}

static void merry_handle_event_locked(MerryManApp* app, const InputEvent* event) {
    if((event->key == InputKeyBack) && (event->type == InputTypeShort)) {
        app->running = false;
        return;
    }

    if(app->mode == GameModeTitle) {
        if((event->key == InputKeyOk) && (event->type == InputTypeShort || event->type == InputTypePress)) {
            merry_reset_run(app);
        }
        return;
    }

    if(app->mode == GameModeGameOver) {
        if((event->key == InputKeyOk) && (event->type == InputTypeShort || event->type == InputTypePress)) {
            merry_reset_run(app);
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
        if(event->type == InputTypePress || event->type == InputTypeShort) app->jump_queued = true;
    }
}

int32_t merry_man_app(void* p) {
    MerryManApp* app;
    InputEvent event;
    uint32_t i;

    UNUSED(p);

    app = malloc(sizeof(MerryManApp));
    if(!app) return -1;

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    app->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    if(!app->gui || !app->view_port || !app->input_queue || !app->mutex) {
        if(app->mutex) furi_mutex_free(app->mutex);
        if(app->input_queue) furi_message_queue_free(app->input_queue);
        if(app->view_port) view_port_free(app->view_port);
        if(app->gui) furi_record_close(RECORD_GUI);
        free(app);
        return -1;
    }

    app->running = true;
    app->left_held = false;
    app->right_held = false;
    app->jump_queued = false;
    app->grounded = true;
    app->facing_left = false;
    app->mode = GameModeTitle;
    app->player_x_q8 = PLAYER_START_X << 8;
    app->player_y_q8 = PLAYER_START_Y << 8;
    app->player_vx_q8 = 0;
    app->player_vy_q8 = 0;
    app->frame = 0;
    app->pickup_score = 0;
    app->stomp_score = 0;
    app->farthest_distance = 0;
    app->best_distance = 0;

    for(i = 0; i < MAX_PICKUP_SLOTS; i++) {
        app->pickup_slot_chunk[i] = INT32_MIN;
        app->pickup_slot_collected[i] = false;
    }
    for(i = 0; i < MAX_ENEMY_SLOTS; i++) {
        app->enemy_slot_chunk[i] = INT32_MIN;
        app->enemy_slot_defeated[i] = false;
    }
    for(i = 0; i < MAX_SCORE_POPS; i++) {
        app->score_pop_timer[i] = 0;
        app->score_pop_value[i] = 0;
    }

    view_port_draw_callback_set(app->view_port, merry_draw_callback, app);
    view_port_input_callback_set(app->view_port, merry_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    while(app->running) {
        while(furi_message_queue_get(app->input_queue, &event, 0) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            merry_handle_event_locked(app, &event);
            furi_mutex_release(app->mutex);
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        merry_step_locked(app);
        furi_mutex_release(app->mutex);

        view_port_update(app->view_port);
        furi_delay_ms(FRAME_MS);
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_enabled_set(app->view_port, false);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
