#include <raylib.h>
#undef bool
#include "renderer.h"
#include "gba_io.h"

static Texture2D texture;
static u32 global_width, global_height;

void render_init(u32 width, u32 height) {
    global_width = width;
    global_height = height;

    InitWindow(width * DISPLAY_SCALE, height * DISPLAY_SCALE, WINDOW_NAME);
    SetTargetFPS(TARGET_FPS);

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

static
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
    static u32 poll_counter = 0;

    if ((poll_counter++ & 0x0fff) == 0) {
        render_poll_events();
        render_handle_events();
    }
}