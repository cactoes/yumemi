#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
#include <timeapi.h>
#include <raylib.h>
#undef bool
#include <stdio.h>
#include "renderer.h"
#include "gba_io.h"
#include "ppu.h"
#include "flash.h"

static
u64 time_now_ns() {
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);

    QueryPerformanceCounter(&counter);

    u64 secs = (u64)(counter.QuadPart / freq.QuadPart);
    u64 rem  = (u64)(counter.QuadPart % freq.QuadPart);
    return secs * 1000000000ull + rem * 1000000000ull / (u64)freq.QuadPart;
}

static
void sleep_until_ns(u64 deadline) {
    static bool timer_init = false;
    if (!timer_init) {
        timeBeginPeriod(1);
        timer_init = true;
    }

    const u64 spin_margin = 1500000ull;

    u64 now = time_now_ns();
    if (now >= deadline)
        return;

    if (deadline - now > spin_margin) {
        u64 sleep_ms = (deadline - now - spin_margin) / 1000000ull;
        if (sleep_ms > 0)
            Sleep((DWORD)sleep_ms);
    }

    while (time_now_ns() < deadline)
        YieldProcessor();
}

static Texture2D texture;
static u32 global_width, global_height;
static frame_time_t frame_time;

void render_init(u32 width, u32 height) {
    global_width = width;
    global_height = height;

    frame_time.speed_percent = 100;
    frame_time.auto_frameskip = true;
    // frame_time.speed_percent = 0;
    // frame_time.frameskip = 4;

    InitWindow(width * DISPLAY_SCALE, height * DISPLAY_SCALE, WINDOW_NAME);

    Image image = {
        .data = 0,
        .width = width,
        .height = height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };

    texture = LoadTextureFromImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
}

void render_present(u32* buffer) {
    UpdateTexture(texture, buffer);

    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(
        texture,
        (Rectangle){ 0, 0, (float)global_width, (float)global_height },
        (Rectangle){ 0, 0, (float)global_width * DISPLAY_SCALE, (float)global_height * DISPLAY_SCALE },
        (Vector2){ 0, 0 },
        0.0f,
        WHITE
    );
    EndDrawing();
}

bool render_should_close() {
    return WindowShouldClose();
}

void render_close() {
    UnloadTexture(texture);
    CloseWindow();
}

void render_poll_events() {
    PollInputEvents();
}

static
void render_handle_events() {
    u16 pressed = 0;

    if (IsWindowFocused()) {
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

    io_set_keys_pressed(pressed);
}

void render_update_events() {
    render_poll_events();
    render_handle_events();
}

void render_handle_frame() {
    if (!ppu_take_frame_ready())
        return;

    // static u64 last_print = 0;
    // static u32 frames = 0;
    // frames++;
    // u64 t = time_now_ns();
    // if (t - last_print >= 1000000000) {
    //     printf("emulated fps: %u\n", frames);
    //     frames = 0;
    //     last_print = t;
    // }

    render_update_events();

    static u32 flash_dirty_counter = 0;
    if (flash_is_dirty()) {
        if (flash_dirty_counter++ > 60) {
            flash_save();
            flash_mark_clean();
            flash_dirty_counter = 0;
        }
    } else {
        flash_dirty_counter = 0;
    }

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