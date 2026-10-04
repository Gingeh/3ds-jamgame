#include <3ds.h>
#include <citro2d.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/unistd.h>
#include "cameraButton.h"

// TODO LIST:
// - maybe also briefly show a "spark" on the top screen ignoring the spotlight darkness
// - playtest
// - playtest
// - playtest

// lower is slower
#define SLIDER_SMOOTHING 0.05f
#define CIV_TURN_SPEED 0.05f
#define CIV_MOVE_SPEED 1.5f
#define CIVILIAN_COUNT 50
#define BOTTOM_ZOOM 1.5f
#define GENERATOR_COUNT 5
#define GENERATOR_TOP_MARGIN 25.0f
#define SABO_RANGE 40.0f
#define FLASH_DURATION 20

#define TOP_WIDTH  400
#define TOP_HEIGHT 240
#define BOTTOM_WIDTH  320
#define BOTTOM_HEIGHT 240

#define PI (float)M_PI

enum DepthLayer {
    LAYER_GROUND,
    LAYER_OBJECTS,
    LAYER_SPOTLIGHT,
    LAYER_UI,
    THIS_MANY_LAYERS,
};

float layer_depth(enum DepthLayer layer) {
    // -1.0 is the deepest, 1.0 is the highest
    // plus each layer needs room to grow upwards slightly
    return -1.0f + ((float)layer / (float)THIS_MANY_LAYERS) * 2.0f;
}

struct Generator {
    float x;
    float y;
    bool destroyed;
    float health;
};

struct TargetPos {
    float x;
    float y;
};

struct CivPosDir {
    float x;
    float y;
    float direction;
};

struct Civilian {
    struct CivPosDir posDir;
    struct TargetPos target;
    bool splattered;
};

struct Sabateur {
    struct CivPosDir posDir;
};

enum GameStatus {
    RUNNING,
    SABOTEUR_CAUGHT,
    POWER_DEPLETED,
};

// everybody loves global state!
struct GameState {
    C3D_RenderTarget* top_target;
    C3D_RenderTarget* bottom_target;
    float leftSlider;
    float rightSlider;
    bool prevCameraCovered;
    struct Civilian civilians[CIVILIAN_COUNT];
    struct Sabateur sabateur;
    float lastDesiredDirection;
    int remainingPower;
    int flashframes;
    struct Generator generators[GENERATOR_COUNT];
    enum GameStatus status;
};
struct GameState gameState;

float spotlight_radius() {
    return gameState.remainingPower / (float)GENERATOR_COUNT * 50.0f + 30.0f;
}
float spotlight_target_x() { return TOP_WIDTH * gameState.leftSlider; }
float spotlight_target_y() { return TOP_HEIGHT * gameState.rightSlider; }

struct TargetPos new_civilian_target() {
    return (struct TargetPos){
        .x = rand() % (int)TOP_WIDTH,
        .y = rand() % (int)TOP_HEIGHT,
    };
}

void get_slider_targets(float* left, float* right) {
    // top player controls need to be preprocessed because they're weird
    float leftSliderRaw = osGet3DSliderState();
    u8 rightSliderRaw; MCUHWC_GetSoundSliderLevel(&rightSliderRaw);
    // left slider is an 8bit float while volume slider is a 6bit int
    // reduce left slider to 6bit precision so they're consistent
    *left = roundf(leftSliderRaw * 63.0f) / 63.0f;
    // convert right slider to float
    *right = ((float)rightSliderRaw / 63.0f);
}

void init_global_state() {
    get_slider_targets(&gameState.leftSlider, &gameState.rightSlider);
    gameState.prevCameraCovered = false;

    gameState.sabateur = (struct Sabateur){
        .posDir = {
            .x = rand() % (int)TOP_WIDTH,
            .y = rand() % (int)TOP_HEIGHT,
            .direction = (float)(rand() % 360) * (PI / 180.0f),
        }
    };

    for (int i = 0; i < CIVILIAN_COUNT; i++) {
        gameState.civilians[i] = (struct Civilian){
            .posDir = {
                .x = rand() % (int)TOP_WIDTH,
                .y = rand() % (int)TOP_HEIGHT,
                .direction = (float)(rand() % 360) * (PI / 180.0f),
            },
            .target = new_civilian_target(),
            .splattered = false,
        };
    }

    gameState.remainingPower = GENERATOR_COUNT;
    gameState.flashframes = 0;
    gameState.status = RUNNING;

    #define GENERATOR_OVERPREPARE 4
    struct Generator generator_candidates[GENERATOR_COUNT * GENERATOR_OVERPREPARE];
    float min_distances[GENERATOR_COUNT * GENERATOR_OVERPREPARE];
    for (int i = 0; i < GENERATOR_COUNT * GENERATOR_OVERPREPARE; i++) {
        generator_candidates[i] = (struct Generator){
            .x = rand() % (int)(TOP_WIDTH),
            .y = rand() % (int)(TOP_HEIGHT - GENERATOR_TOP_MARGIN) + GENERATOR_TOP_MARGIN,
            .destroyed = false,
            .health = 1.0f,
        };
        min_distances[i] = INFINITY;
    }

    for (int i = 0; i < GENERATOR_COUNT; i++) {
        int best_candidate_index = -1;
        float best_candidate_min_distance = -1.0f;
        for (int j = 0; j < GENERATOR_COUNT * GENERATOR_OVERPREPARE; j++) {
            if (min_distances[j] > best_candidate_min_distance) {
                best_candidate_index = j;
                best_candidate_min_distance = min_distances[j];
            }
        }
        gameState.generators[i] = generator_candidates[best_candidate_index];
        generator_candidates[best_candidate_index].destroyed = true;

        for (int j = 0; j < GENERATOR_COUNT * GENERATOR_OVERPREPARE; j++) {
            float dx = generator_candidates[j].x - gameState.generators[i].x;
            float dy = generator_candidates[j].y - gameState.generators[i].y;
            float dist_sq = dx * dx + dy * dy;
            if (dist_sq < min_distances[j]) {
                min_distances[j] = dist_sq;
            }
        }
    }
}

void move_civilian_posdir(struct CivPosDir* civ, float desired_direction) {
    float diff = desired_direction - civ->direction;
    while (diff > PI)  diff -= 2.0f * PI;
    while (diff < -PI) diff += 2.0f * PI;
    civ->direction += diff * CIV_TURN_SPEED;
    while (civ->direction > PI)  civ->direction -= 2.0f * PI;
    while (civ->direction < -PI) civ->direction += 2.0f * PI;

    civ->x += cosf(civ->direction) * CIV_MOVE_SPEED;
    civ->y += sinf(civ->direction) * CIV_MOVE_SPEED;

    // reflect direction if they hit the edge of the screen
    if (civ->x < 0.0f) {
        civ->x = 0.0f;
        civ->direction = PI - civ->direction;
    } else if (civ->x > TOP_WIDTH) {
        civ->x = TOP_WIDTH;
        civ->direction = PI - civ->direction;
    }
    if (civ->y < 0.0f) {
        civ->y = 0.0f;
        civ->direction = -civ->direction;
    } else if (civ->y > TOP_HEIGHT) {
        civ->y = TOP_HEIGHT;
        civ->direction = -civ->direction;
    }
}

float make_desired_direction(struct Civilian* civ) {
    struct TargetPos* target = &civ->target;

    // if the civilian is close enough to the target, pick a new target
    float dx = target->x - civ->posDir.x;
    float dy = target->y - civ->posDir.y;
    if (dx * dx + dy * dy < 400.0f) {
        *target = new_civilian_target();
    }

    // return the angle to the target
    float new_dx = target->x - civ->posDir.x;
    float new_dy = target->y - civ->posDir.y;
    return atan2f(new_dy, new_dx);
}

enum ShouldExit {
    EXIT_NO,
    EXIT_YES,
};

enum ShouldExit tick() {
    hidScanInput();

    if (hidKeysDown() & KEY_START) return EXIT_YES;

    if (gameState.status != RUNNING) {
        gameState.flashframes = 0;
        if (hidKeysDown() & KEY_A) {
            init_global_state();
        }
        return EXIT_NO;
    }

    float leftSliderTarget, rightSliderTarget;
    get_slider_targets(&leftSliderTarget, &rightSliderTarget);

    // smooth the sliders to avoid jitter
    gameState.leftSlider = gameState.leftSlider * (1.0f - SLIDER_SMOOTHING) + leftSliderTarget * SLIDER_SMOOTHING;
    gameState.rightSlider = gameState.rightSlider * (1.0f - SLIDER_SMOOTHING) + rightSliderTarget * SLIDER_SMOOTHING;

    bool cameraNewlyCovered = false;
    switch (camera_check_covered()) {
        case NOT_COVERED:
            gameState.prevCameraCovered = false;
            break;
        case COVERED:
            cameraNewlyCovered = !gameState.prevCameraCovered;
            gameState.prevCameraCovered = true;
            break;
        case PENDING:
            break;
    }

    if (gameState.flashframes > 0) {
        gameState.flashframes--;
        return EXIT_NO; // game paused while flashing (except input reading)
    }

    if (cameraNewlyCovered) {
        CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
        // ^ shutter sound ignores volume slider!

        float target_x = spotlight_target_x();
        float target_y = spotlight_target_y();

        for (int i = 0; i < CIVILIAN_COUNT; i++) {
            struct Civilian* civ = &gameState.civilians[i];
            if (civ->splattered) continue; // don't die twice
            float dx = civ->posDir.x - target_x;
            float dy = civ->posDir.y - target_y;
            if (dx * dx + dy * dy < spotlight_radius() * spotlight_radius()) {
                civ->splattered = true;
                // direction away from target
                civ->posDir.direction = atan2f(dy, dx);
            }
        }
        
        float sabateur_dx = gameState.sabateur.posDir.x - target_x;
        float sabateur_dy = gameState.sabateur.posDir.y - target_y;
        if (sabateur_dx * sabateur_dx + sabateur_dy * sabateur_dy < spotlight_radius() * spotlight_radius()) {
            gameState.status = SABOTEUR_CAUGHT;
        } else {
            gameState.flashframes = FLASH_DURATION;
            gameState.remainingPower--;
        }
    }

    for (int i = 0; i < CIVILIAN_COUNT; i++) {
        struct Civilian* civ = &gameState.civilians[i];
        if (civ->splattered) continue; // dead civilians don't move
        float desired_direction = make_desired_direction(civ);
        move_civilian_posdir(&civ->posDir, desired_direction);
    }

    circlePosition circle_pos;
    hidCircleRead(&circle_pos);
    float sabateur_desired_direction;
    if (circle_pos.dx * circle_pos.dx + circle_pos.dy * circle_pos.dy < 10000) {
        sabateur_desired_direction = gameState.lastDesiredDirection;
    } else {
        sabateur_desired_direction = atan2f(-circle_pos.dy, circle_pos.dx);
    }
    gameState.lastDesiredDirection = sabateur_desired_direction;
    move_civilian_posdir(&gameState.sabateur.posDir, sabateur_desired_direction);

    for (int i = 0; i < GENERATOR_COUNT; i++) {
        struct Generator* gen = &gameState.generators[i];
        if (gen->destroyed) continue;
        float dx = gen->x - gameState.sabateur.posDir.x;
        float dy = gen->y - gameState.sabateur.posDir.y;
        if (dx * dx + dy * dy < SABO_RANGE * SABO_RANGE) {
            gen->health -= 0.01f;
            if (gen->health <= 0.0f) {
                gen->destroyed = true;
                gameState.remainingPower--;
                gameState.flashframes = FLASH_DURATION;
                CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_MOVIE_END);
            }
        }
    }

    if (gameState.remainingPower <= 0 && gameState.status == RUNNING) {
        gameState.status = POWER_DEPLETED;
    }

    return EXIT_NO;
}

#define WHITE C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define BLACK C2D_Color32(0x00, 0x00, 0x00, 0xFF)
#define TRANS C2D_Color32(0x00, 0x00, 0x00, 0x00)
#define YELLOW C2D_Color32(0xFF, 0xFF, 0x00, 0xFF)
#define RED   C2D_Color32(0xFF, 0x00, 0x00, 0xFF)
#define GREEN C2D_Color32(0x00, 0xFF, 0x00, 0xFF)
#define GRAY  C2D_Color32(0x80, 0x80, 0x80, 0xFF)
#define LIGHTESTGRAY C2D_Color32(0xF0, 0xF0, 0xF0, 0xFF)
#define VERYLIGHTRED C2D_Color32(0xFF, 0xA0, 0xA0, 0xFF)
#define TRANSPARENTISHBLACK C2D_Color32(0x00, 0x00, 0x00, 0x40)

void draw_splat(struct CivPosDir* civ) {
    // two rectangles, one offset by direction
    float depth = layer_depth(LAYER_GROUND) + 0.001f;
    C2D_DrawRectSolid(civ->x - 10.0f, civ->y - 10.0f, depth, 20.0f, 20.0f, RED);
    float offset_x = cosf(civ->direction) * 7.5f;
    float offset_y = sinf(civ->direction) * 7.5f;
    C2D_DrawRectSolid(civ->x - 7.0f + offset_x, civ->y - 7.0f + offset_y, depth, 14.0f, 14.0f, RED);
}

void draw_civ(struct CivPosDir* civ, bool is_sabateur) {
    float individual_depth_offset = 0.19f * ((civ->y + 7.0f) / TOP_HEIGHT); // lower on screen = closer to camera
    float depth = layer_depth(LAYER_OBJECTS) + individual_depth_offset;

    // outline (two pixels thick)
    C2D_DrawRectSolid(civ->x - 7.0f, civ->y - 7.0f, depth, 14.0f, 14.0f, BLACK);
    depth += 0.0001f; // layering body parts

    // body fill
    u32 body_colour = is_sabateur ? VERYLIGHTRED : WHITE;
    C2D_DrawRectSolid(civ->x - 5.0f, civ->y - 5.0f, depth, 10.0f, 10.0f, body_colour);
    depth += 0.0001f;

    // eyes
    float x_offset = cosf(civ->direction) * 2.0f;
    float y_offset = sinf(civ->direction) * 2.0f;
    C2D_DrawRectSolid(civ->x - 4.0f + x_offset, civ->y - 2.0f + y_offset, depth, 2.0f, 2.0f, BLACK);
    C2D_DrawRectSolid(civ->x + 2.0f + x_offset, civ->y - 2.0f + y_offset, depth, 2.0f, 2.0f, BLACK);
}

void draw_civs(bool highlight_sabateur) {
    draw_civ(&gameState.sabateur.posDir, highlight_sabateur);
    for (int i = 0; i < CIVILIAN_COUNT; i++) {
        struct Civilian* civ = &gameState.civilians[i];
        if (civ->splattered) {
            draw_splat(&civ->posDir);
        } else {
            draw_civ(&civ->posDir, false);
        }
    }
}

void draw_generators(bool show_health) {
    for (int i = 0; i < GENERATOR_COUNT; i++) {
        struct Generator* gen = &gameState.generators[i];
        float individual_depth_offset = 0.19f * (gen->y / TOP_HEIGHT); // lower on screen = closer to camera
        float depth = layer_depth(LAYER_OBJECTS) + individual_depth_offset;
        u32 colour = gen->destroyed ? GRAY : BLACK;

        // long base
        C2D_DrawRectSolid(gen->x - 10.0f, gen->y - 1.0f, depth, 20.0f, 3.0f, colour);
        // pole
        C2D_DrawRectSolid(gen->x - 2.5f, gen->y - 23.0f, depth, 5.0f, 22.0f, colour);
        // ring outlines
        C2D_DrawRectSolid(gen->x -  8.0f, gen->y - 23.0f, depth, 16.0f, 6.0f, colour);
        C2D_DrawRectSolid(gen->x - 10.5f, gen->y - 16.0f, depth, 21.0f, 6.0f, colour);
        C2D_DrawRectSolid(gen->x - 13.0f, gen->y -  9.0f, depth, 26.0f, 6.0f, colour);
        // ring fills
        if (gen->destroyed) continue;
        C2D_DrawRectSolid(gen->x -  6.0f, gen->y - 21.0f, depth, 12.0f, 2.0f, YELLOW);
        C2D_DrawRectSolid(gen->x -  8.5f, gen->y - 14.0f, depth, 17.0f, 2.0f, YELLOW);
        C2D_DrawRectSolid(gen->x - 11.0f, gen->y -  7.0f, depth, 22.0f, 2.0f, YELLOW);

        // health bar
        if (!show_health) continue;
        float health_depth = layer_depth(LAYER_UI) + individual_depth_offset;
        C2D_DrawRectSolid(gen->x - 10.0f, gen->y + 4.0f, health_depth, 20.0f, 3.0f, GRAY);
        C2D_DrawRectSolid(gen->x - 10.0f, gen->y + 4.0f, health_depth, 20.0f * gen->health, 3.0f, RED);

        // sabo range
        float range_depth = layer_depth(LAYER_GROUND);
        float diameter = SABO_RANGE * 2.0f;
        C2D_DrawEllipseSolid(gen->x - SABO_RANGE - 1.5f, gen->y - SABO_RANGE - 1.5f, range_depth, diameter + 3.0f, diameter + 3.0f, BLACK);
        C2D_DrawEllipseSolid(gen->x - SABO_RANGE, gen->y - SABO_RANGE, range_depth + 0.01f, diameter, diameter, LIGHTESTGRAY);
    }
}

void draw_spotlight(float cx, float cy, float radius, bool bottom_screen) {
    int segments = 128;
    float depth = layer_depth(LAYER_SPOTLIGHT);

    float out_radius = sqrtf(TOP_WIDTH * TOP_WIDTH + TOP_HEIGHT * TOP_HEIGHT)/2.0f;
    // large enough to cover the whole screen ^^^
    float in_radius = radius;
    float fade_radius = radius * 0.9f;

    u32 colour = bottom_screen ? TRANSPARENTISHBLACK : BLACK;

    for (int i = 0; i < segments; i++) {
        float theta1 = (float)i / segments * 2.0f * PI;
        float theta2 = (float)(i + 1) / segments * 2.0f * PI;

        float x1_out = TOP_WIDTH/2.0f + cosf(theta1) * out_radius;
        float y1_out = TOP_HEIGHT/2.0f + sinf(theta1) * out_radius;
        float x2_out = TOP_WIDTH/2.0f + cosf(theta2) * out_radius;
        float y2_out = TOP_HEIGHT/2.0f + sinf(theta2) * out_radius;

        float x1_in = cx + cosf(theta1) * in_radius;
        float y1_in = cy + sinf(theta1) * in_radius;
        float x2_in = cx + cosf(theta2) * in_radius;
        float y2_in = cy + sinf(theta2) * in_radius;

        float x1_fade = cx + cosf(theta1) * fade_radius;
        float y1_fade = cy + sinf(theta1) * fade_radius;
        float x2_fade = cx + cosf(theta2) * fade_radius;
        float y2_fade = cy + sinf(theta2) * fade_radius;

        // 1. opaque quad from outer to inner radius
        C2D_DrawTriangle(
            x1_in, y1_in, colour,
            x1_out, y1_out, colour,
            x2_out, y2_out, colour,
            depth
        );
        C2D_DrawTriangle(
            x1_in, y1_in, colour,
            x2_out, y2_out, colour,
            x2_in, y2_in, colour,
            depth
        );

        // 2. gradient quad from inner to fade radius
        C2D_DrawTriangle(
            x1_in, y1_in, colour,
            x1_fade, y1_fade, TRANS,
            x2_fade, y2_fade, TRANS,
            depth
        );
        C2D_DrawTriangle(
            x1_in, y1_in, colour,
            x2_fade, y2_fade, TRANS,
            x2_in, y2_in, colour,
            depth
        );

        // 3. white gradient triangle from corner to fade radius
        if (bottom_screen) continue;
        // skip if cross product is negative to avoid drawing over the spotlight
        float cross =  x2_in * y1_in - x1_in * y2_in;
        if (cross <= 0.0f) continue;
        // distance from origin to first point (for normalisation)
        float dist = sqrtf(x1_in * x1_in + y1_in * y1_in);
        // length of circle segment (also for normalisation)
        float dx = x2_in - x1_in;
        float dy = y2_in - y1_in;
        float seg_length = sqrtf(dx * dx + dy * dy);
        // 0 if parallel to light, 1 if facing directly towards it
        float facing_ratio = cross / (dist * seg_length);
        u8 luma = facing_ratio * 255.0f;
        u32 beam_colour = C2D_Color32(luma, luma, luma, 0xFF);
        C2D_DrawTriangle(
            x1_in, y1_in, BLACK,
            0, 0, beam_colour,
            x2_in, y2_in, BLACK,
            depth + 0.01f
        );
    }
}

void draw_power(float screen_width) {
    float depth = layer_depth(LAYER_UI);
    float bar_width = screen_width - 10.0f;
    u32 bar_colour;
    switch (gameState.remainingPower) {
        case 1: bar_colour = RED; break;
        case 2: bar_colour = YELLOW; break;
        default: bar_colour = GREEN; break;
    }
    C2D_DrawRectSolid(3.0f, 3.0f, depth, bar_width + 4.0f, 14.0f, WHITE);
    depth += 0.001f;
    C2D_DrawRectSolid(5.0f, 5.0f, depth, bar_width, 10.0f, BLACK);
    depth += 0.001f;
    C2D_DrawRectSolid(5.0f, 5.0f, depth, bar_width * ((float)gameState.remainingPower / (float)GENERATOR_COUNT), 10.0f, bar_colour);
}

void draw_top() {
    C2D_TargetClear(gameState.top_target, WHITE);
    C2D_SceneBegin(gameState.top_target);

    C2D_ViewRotateDegrees(180);
    C2D_ViewTranslate(-TOP_WIDTH, -TOP_HEIGHT);

    draw_civs(gameState.status != RUNNING);
    draw_generators(false);

    draw_spotlight(
        spotlight_target_x(),
        spotlight_target_y(),
        spotlight_radius(),
        gameState.status != RUNNING
    );

    if (gameState.flashframes > 0) C2D_TargetClear(gameState.top_target, BLACK);

    draw_power(TOP_WIDTH);

    C2D_ViewReset();
}

void draw_bottom() {
    C2D_TargetClear(gameState.bottom_target, WHITE);
    C2D_SceneBegin(gameState.bottom_target);

    C2D_ViewScale(BOTTOM_ZOOM, BOTTOM_ZOOM);

    float camtarget_x = gameState.sabateur.posDir.x + cosf(gameState.sabateur.posDir.direction) * 20.0f;
    float camtarget_y = gameState.sabateur.posDir.y + sinf(gameState.sabateur.posDir.direction) * 20.0f;

    float cam_x = camtarget_x - BOTTOM_WIDTH/BOTTOM_ZOOM/2.0f;
    float cam_y = camtarget_y - BOTTOM_HEIGHT/BOTTOM_ZOOM/2.0f;

    if (cam_x < 0.0f) cam_x = 0.0f;
    if (cam_y < 0.0f) cam_y = 0.0f;

    if (cam_x > TOP_WIDTH - BOTTOM_WIDTH/BOTTOM_ZOOM) cam_x = TOP_WIDTH - BOTTOM_WIDTH/BOTTOM_ZOOM;
    if (cam_y > TOP_HEIGHT - BOTTOM_HEIGHT/BOTTOM_ZOOM) cam_y = TOP_HEIGHT - BOTTOM_HEIGHT/BOTTOM_ZOOM;

    C2D_ViewTranslate(-cam_x, -cam_y);

    draw_civs(true);
    draw_generators(true);

    draw_spotlight(
        spotlight_target_x(),
        spotlight_target_y(),
        spotlight_radius(),
        true
    );

    if (gameState.flashframes > 0) C2D_TargetClear(gameState.bottom_target, BLACK);

    C2D_ViewReset();
    
    draw_power(BOTTOM_WIDTH);
    // ^ after view reset because it's independent of the camera
}

typedef void (*defer_fn)(void);
static inline void _execute_defer(defer_fn *func_ptr) {
    if (func_ptr && *func_ptr) {
        (*func_ptr)();
    }
}
#define DEFER_CONCAT_IMPL(x, y) x##y
#define DEFER_CONCAT(x, y) DEFER_CONCAT_IMPL(x, y)
#define DEFER_VAR_NAME DEFER_CONCAT(_defer_var_, __LINE__)
#define DEFER(func) \
    __attribute__((cleanup(_execute_defer))) \
    defer_fn DEFER_VAR_NAME = (defer_fn)(func)

#define DEFINE_CLEANUP(Type, Func, MacroName, InvalidValue) \
    static inline void _cleanup_##MacroName(Type *val) { \
        if (val && *val != (InvalidValue)) { \
            Func(*val); \
        } \
    } \
    __attribute__((unused)) /* suppresses warnings if macro isn't used */
#define CLEAN(MacroName) __attribute__((cleanup(_cleanup_##MacroName)))

//            (type,  callback, name,  invalid value)
DEFINE_CLEANUP(int,   close,    fd,    -1)
DEFINE_CLEANUP(void*, free,     alloc, NULL)

int main(int argc, char *argv[]) {
    osSetSpeedupEnable(true); // new 3ds go brrrr

    srand((unsigned int)svcGetSystemTick());

    gfxInitDefault();
    DEFER(gfxExit);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    DEFER(C3D_Fini);

    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    DEFER(C2D_Fini);

    gameState.top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    gameState.bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    mcuHwcInit(); // needed to grab volume slider position
    DEFER(mcuHwcExit);

    camera_init();
    DEFER(camera_exit);

    #define SOC_ALIGN       0x1000
    #define SOC_BUFFERSIZE  0x100000
    CLEAN(alloc) void *SOC_buffer = memalign(SOC_ALIGN, SOC_BUFFERSIZE);

	socInit(SOC_buffer, SOC_BUFFERSIZE);
    DEFER(socExit);

    CLEAN(fd) int debug_socket = link3dsStdio();

    init_global_state();

    while (aptMainLoop()) {
        if (tick() == EXIT_YES) break;
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        draw_top();
        draw_bottom();
        C3D_FrameEnd(0);
    }

    return 0;
}
