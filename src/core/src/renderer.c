#include <raylib.h>
#undef bool
#include <stdio.h>
#include <math.h>
#include "renderer.h"
#include "gba_io.h"
#include "ppu.h"
#include "flash.h"
#include "wtime.h"
#include "wwindow.h"
#include "eeprom.h"
#include "sram.h"
#include "apu.h"

static Texture2D texture;
static u32 global_base_width, global_base_height;
static frame_time_t frame_time;
static bool should_close;
static Font font;
static Font fps_font;
static bool renderer_draw_fps = false;

static
Font load_first_font(const char* const* paths, int count) {
    for (int i = 0; i < count; i++) {
        if (!FileExists(paths[i]))
            continue;

        Font f = LoadFontEx(paths[i], 64, NULL, 0);
        SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
        return f;
    }
    return GetFontDefault();
}

void render_init(u32 width, u32 height) {
    global_base_width = width;
    global_base_height = height;

    should_close = false;

    frame_time.speed_percent = 100;
    frame_time.auto_frameskip = true;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(width * INITIAL_DISPLAY_SCALE, height * INITIAL_DISPLAY_SCALE, WINDOW_NAME);
    SetWindowMinSize(240, 160);

    Image image = {
        .data = 0,
        .width = width,
        .height = height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };

    texture = LoadTextureFromImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    const char* intro_paths[] = {
        "C:/Windows/Fonts/segoeuil.ttf",
        "C:/Windows/Fonts/segoeui.ttf"
    };

    const char* fps_paths[] = {
        "C:/Windows/Fonts/segoeuib.ttf",
        "C:/Windows/Fonts/segoeui.ttf"
    };

    font = load_first_font(intro_paths, sizeof(intro_paths) / sizeof(char*));
    fps_font = load_first_font(fps_paths, sizeof(fps_paths) / sizeof(char*));

    install_window_menu();
}

static u32 frames_last_update = 0;

static
void render_draw_fps() {
    char text[16];
    snprintf(text, sizeof(text), "%d FPS", frames_last_update);

    float fontSize = 24.0f;
    float spacing = 2.0f;
    float margin = 10.0f;

    Vector2 size = MeasureTextEx(fps_font, text, fontSize, spacing);
    Vector2 pos = { GetScreenWidth() - size.x - margin, margin };

    DrawTextEx(fps_font, text, (Vector2){ pos.x + 2.0f, pos.y + 2.0f }, fontSize, spacing, BLACK);
    DrawTextEx(fps_font, text, pos, fontSize, spacing, WHITE);
}

void render_present(u32* buffer) {
    UpdateTexture(texture, buffer);

    float scale_x = GetScreenWidth() / (float)global_base_width;
    float scale_y = GetScreenHeight() / (float)global_base_height;
    float scale = fminf(scale_x, scale_y);

    Rectangle destination = {
        .x = (GetScreenWidth()  - global_base_width * scale) / 2.0f,
        .y = (GetScreenHeight() - global_base_height * scale) / 2.0f,
        .width = global_base_width * scale,
        .height = global_base_height * scale,
    };

    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(
        texture,
        (Rectangle){ 0, 0, (float)global_base_width, (float)global_base_height },
        destination,
        (Vector2){ 0, 0 },
        0.0f,
        WHITE
    );

    if (renderer_draw_fps)
        render_draw_fps();

    EndDrawing();
}

bool render_should_close() {
    return WindowShouldClose() || should_close;
}

void render_close() {
    UnloadTexture(texture);
    UnloadFont(font);
    UnloadFont(fps_font);
    CloseWindow();
}

void render_poll_events() {
    PollInputEvents();
}

static
void render_handle_events() {
    u16 pressed = 0;
    i32 gamepad = 0;
    bool should_speed_up = false;

    if (IsWindowFocused()) {
        if (IsGamepadAvailable(gamepad)) {
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP) || IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) should_speed_up = true;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) pressed |= GBA_KEY_A;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) pressed |= GBA_KEY_B;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT)) pressed |= GBA_KEY_SELECT;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT)) pressed |= GBA_KEY_START;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) pressed |= GBA_KEY_DPAD_LEFT;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) pressed |= GBA_KEY_DPAD_RIGHT;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP)) pressed |= GBA_KEY_DPAD_UP;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) pressed |= GBA_KEY_DPAD_DOWN;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) pressed |= GBA_KEY_L;
            if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) pressed |= GBA_KEY_R;
        }

        if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_LEFT_SHIFT)) should_speed_up = true;
        if (IsKeyDown(KEY_Z)) pressed |= GBA_KEY_A;
        if (IsKeyDown(KEY_X)) pressed |= GBA_KEY_B;
        if (IsKeyDown(KEY_BACKSPACE)) pressed |= GBA_KEY_SELECT;
        if (IsKeyDown(KEY_ENTER)) pressed |= GBA_KEY_START;
        if (IsKeyDown(KEY_RIGHT)) pressed |= GBA_KEY_DPAD_RIGHT;
        if (IsKeyDown(KEY_LEFT)) pressed |= GBA_KEY_DPAD_LEFT;
        if (IsKeyDown(KEY_UP)) pressed |= GBA_KEY_DPAD_UP;
        if (IsKeyDown(KEY_DOWN)) pressed |= GBA_KEY_DPAD_DOWN;
        if (IsKeyDown(KEY_W)) pressed |= GBA_KEY_R;
        if (IsKeyDown(KEY_Q)) pressed |= GBA_KEY_L;
    }

    render_set_speed(should_speed_up ? 0 : 100);
    io_set_keys_pressed(pressed);
}

void render_update_events() {
    render_poll_events();
    render_handle_events();
}

void render_update_saves() {
    static u32 save_dirty_counter = 0;

    bool use_eeprom = eeprom_enabled();
    bool use_sram = !use_eeprom && sram_is_enabled();

    bool dirty;
    if (use_eeprom)
        dirty = eeprom_is_dirty();
    else if (use_sram)
        dirty = sram_is_dirty();
    else
        dirty = flash_is_dirty();

    if (dirty) {
        if (++save_dirty_counter >= 60u) {
            bool okay;

            if (use_eeprom) {
                okay = eeprom_save();
            } else if (use_sram) {
                okay = sram_save();
            } else {
                okay = flash_save();
                if (okay)
                    flash_mark_clean();
            }

            save_dirty_counter = 0;
        }
    } else {
        save_dirty_counter = 0;
    }
}

void render_handle_frame() {
    if (!ppu_take_frame_ready())
        return;

    render_update_events();

    static u64 last_reset = 0;
    static u32 frames = 0;

    u64 t = time_now_ns();
    if (last_reset == 0)
        last_reset = t;

    frames++;
    u64 elapsed = t - last_reset;
    if (elapsed >= 500000000ull) {
        frames_last_update = (u32)(((u64)frames * 1000000000ull) / elapsed);
        frames = 0;
        last_reset = t;
    }

    render_update_saves();

    frame_time_t* ft = &frame_time;

    bool was_rendered = ppu_render_enabled();
    if (was_rendered)
        render_present(ppu_get_framebuffer());

    u64 now = time_now_ns();
    if (ft->speed_percent != 0) {
        ft->next_deadline_ns += GBA_FRAME_NS * 100u / ft->speed_percent;

        if (now < ft->next_deadline_ns)
            sleep_until_ns(ft->next_deadline_ns);
        else if (now - ft->next_deadline_ns > GBA_FRAME_NS * 5u)
            ft->next_deadline_ns = now;
    }

    bool render;
    if (ft->auto_frameskip && ft->speed_percent != 0) {
        bool behind = time_now_ns() > ft->next_deadline_ns + GBA_FRAME_NS;
        render = !behind || ft->consecutive_skips >= 9;
    } else {
        render = (++ft->frame_counter % (ft->frameskip + 1)) == 0;
    }

    ft->consecutive_skips = render ? 0 : ft->consecutive_skips + 1;
    ppu_set_render_enabled(render);
}

void render_set_close(bool state) {
    should_close = state;
}

void render_set_frame_skip(u32 value) {
    if (value == 0) {
        frame_time.auto_frameskip = true;
        frame_time.frameskip = 1;
    } else {
        frame_time.auto_frameskip = false;
        frame_time.frameskip = value;
    }
}

void render_set_speed(u32 value) {
    if (frame_time.speed_percent == value)
        return;

    frame_time.speed_percent = value;
    apu_set_muted(value != 100, false);
}

static
float hash(float n) {
    float s = sinf(n) * 43758.5453f;
    return s - floorf(s);
}

static
v3_t v3_rotate(v3_t v, float ax, float ay) {
    float x1 =  v.x * cosf(ay) + v.z * sinf(ay);
    float z1 = -v.x * sinf(ay) + v.z * cosf(ay);
    float y2 = v.y * cosf(ax) - z1 * sinf(ax);
    float z2 = v.y * sinf(ax) + z1 * cosf(ax);
    return (v3_t){ x1, y2, z2 };
}

static
Vector2 v3_project(v3_t v, float cx, float cy, float scale) {
    const float d = 4.0f;
    float k = d / (d - v.z) * scale;
    return (Vector2){ cx + v.x * k, cy - v.y * k };
}

static
void draw_diamond(const v3_t* verts, float ax, float ay, float cx, float cy, float scale, float alpha, bool vertices) {
    v3_t r[6];
    for (int i = 0; i < 6; i++)
        r[i] = v3_rotate(verts[i], ax, ay);

    for (int i = 0; i < 6; i++) {
        for (int j = i + 1; j < 6; j++) {
            float dot = verts[i].x * verts[j].x + verts[i].y * verts[j].y + verts[i].z * verts[j].z;
            if (dot < -0.5f)
                continue;

            float z = (r[i].z + r[j].z) * 0.5f;
            float a = (0.1f + 0.9f * (z + 1.0f) * 0.5f) * alpha;
            float thickness = 1.0f + (z + 1.0f) * 0.5f;

            DrawLineEx(v3_project(r[i], cx, cy, scale), v3_project(r[j], cx, cy, scale), thickness, Fade(C_LINE, a));
        }
    }

    if (!vertices)
        return;

    for (int i = 0; i < 6; i++) {
        Vector2 p = v3_project(r[i], cx, cy, scale);
        float a = (0.1f + 0.9f * (r[i].z + 1.0f) * 0.5f) * alpha;
        int sz = 2 + (int)(r[i].z + 1.0f);
        DrawRectangle((int)p.x - sz / 2, (int)p.y - sz / 2, sz, sz, Fade(C_DARK, a));
    }
}

static
void draw_particles(float t, float cx, float cy, float scale, float alpha, bool front) {
    for (int i = 0; i < 48; i++) {
        float theta = hash(i * 1.3f) * 6.2832f;
        float phi = acosf(2.0f * hash(i * 2.9f) - 1.0f);
        float radius = 1.2f + hash(i * 4.1f) * 0.8f;
        float spin = 0.15f + hash(i * 6.7f) * 0.25f;
        int size = 2 + (int)(hash(i * 8.3f) * 2.0f);

        v3_t p = {
            radius * sinf(phi) * cosf(theta),
            radius * cosf(phi),
            radius * sinf(phi) * sinf(theta)
        };
        p = v3_rotate(p, t * spin * 0.5f, t * spin);

        if ((p.z >= 0.0f) != front)
            continue;

        Vector2 s = v3_project(p, cx, cy, scale);
        float depth = p.z / radius;
        float a = (0.2f + 0.6f * (depth + 1.0f) * 0.5f) * alpha;

        DrawRectangle((int)s.x, (int)s.y, size, size, Fade(front ? C_DARK : C_LINE, a));
    }
}

static
void render_draw_screen(const char* text, bool pulse) {
    static const v3_t octa[6] = {
        { 1, 0, 0 }, { -1, 0, 0 },
        { 0, 1, 0 }, { 0, -1, 0 },
        { 0, 0, 1 }, { 0, 0, -1 },
    };

    float t = (float)GetTime();
    int w = GetScreenWidth();
    int h = GetScreenHeight();
    float cx = w * 0.5f;
    float cy = h * 0.5f;

    float fadeIn = fminf(t / 1.2f, 1.0f);
    float ease = 1.0f - powf(1.0f - fadeIn, 3.0f);

    float scale = fminf((float)w, (float)h) * 0.32f;

    BeginDrawing();
    ClearBackground(C_BG);

    if (w > 0 && h > 0) {
        for (int i = 0; i < 40; i++) {
            float x = hash(i * 1.7f) * w;
            float speed = 6.0f + hash(i * 3.1f) * 14.0f;
            float y = h - fmodf(hash(i * 5.3f) * h + t * speed, (float)h);
            int s = 1 + (int)(hash(i * 7.7f) * 2.0f);
            DrawRectangle((int)x, (int)y, s, s, Fade(C_LINE, (0.3f + 0.5f * hash(i * 9.1f)) * ease));
        }
    }

    draw_particles(t, cx, cy, scale, ease, false);
    draw_diamond(octa, 0.35f, t * 0.15f, cx, cy, scale * 1.7f, ease * 0.35f, false);
    draw_diamond(octa, 0.35f, -t * 0.3f, cx, cy, scale, ease, true);
    draw_particles(t, cx, cy, scale, ease, true);

    float fontSize = 28.0f;
    float spacing  = 10.0f;
    Vector2 size = MeasureTextEx(font, text, fontSize, spacing);
    Vector2 pos = { cx - size.x / 2.0f, cy - size.y / 2.0f };

    float blink = 1.0f;
    if (pulse) {
        blink = 0.5f + 0.5f * sinf(t * 2.5f);
        blink = 0.2f + 0.8f * blink;
    }

    DrawTextEx(font, text, pos, fontSize, spacing, Fade(C_DARK, blink * ease));

    EndDrawing();
}

void render_draw_intro() {
    render_draw_screen("PLEASE INSERT ROM", true);
}

void render_draw_loading_rom() {
    render_draw_screen("LOADING ROM", true);
}

void render_draw_error(const char* text) {
    render_draw_screen(text, false);
}

void render_set_enable_fps(bool state) {
    renderer_draw_fps = state;
}