#include <string.h>
#include "ppu.h"
#include "memory_bus.h"
#include "gba_io.h"
#include "dma.h"

static ppu_t global_ppu;

static
u16 read_le16(const u8* memory, u32 offset) {
    return (u16)((u16)memory[offset] | ((u16)memory[offset + 1] << 8));
}

static
u16 register16(u32 offset) {
    u8* io = mbus_get_region(MEMORY_BUS_REGION_IO);
    return read_le16(io, offset);
}

static
u32 register32(u32 offset) {
    return (u32)register16(offset) | ((u32)register16(offset + 2) << 16);
}

static
u8 video8(u32 offset) {
    u8* vram = mbus_get_region(MEMORY_BUS_REGION_VRAM);
    return offset < MEMORY_BUS_VRAM_SIZE ? vram[offset] : 0;
}

static
u16 video16(u32 offset) {
    u8* vram = mbus_get_region(MEMORY_BUS_REGION_VRAM);
    return offset < MEMORY_BUS_VRAM_SIZE - 1u ? read_le16(vram, offset) : 0;
}

static
u16 palette_color(u32 index) {
    u8* palette = mbus_get_region(MEMORY_BUS_REGION_PALETTE);
    return read_le16(palette, index * 2u) & 0x7FFFu;
}

static
int signed16(u16 value) {
    return value & 0x8000u ? (int)value - 0x10000 : (int)value;
}

static
int signed28(u32 value) {
    value &= 0x0FFFFFFFu;
    return value & 0x08000000u ? (int)value - 0x10000000 : (int)value;
}

static
int fixed_floor(int value) {
    return value >= 0 ? value / 256 : -((-value + 255) / 256);
}

static
u32 wrap_coordinate(int value, u32 size) {
    int result = value % (int)size;
    return (u32)(result < 0 ? result + (int)size : result);
}

static
u32 coefficient(u32 value) {
    value &= 31u;
    return value > 16u ? 16u : value;
}

static
u32 framebuffer_color(u16 color) {
    u32 r = color & 31u;
    u32 g = (color >> 5) & 31u;
    u32 b = (color >> 10) & 31u;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);

    return 0xff000000u | (b << 16) | (g << 8) | r;
}

static
void snapshot_registers() {
    ppu_registers_t* regs = &global_ppu.registers;

    regs->display = register16(PREG_DISPCNT);

    for (u32 bg = 0; bg < PPU_BG_COUNT; bg++) {
        regs->bg_control[bg] = register16(PREG_BG0CNT + bg * 2u);
        regs->bg_hofs[bg] = register16(PREG_BG0HOFS + bg * 4u) & 511u;
        regs->bg_vofs[bg] = register16(PREG_BG0HOFS + bg * 4u + 2u) & 511u;
    }

    for (u32 index = 0; index < 2; index++) {
        u32 base = PREG_BG2PA + index * 0x10u;
        regs->pa[index] = signed16(register16(base));
        regs->pc[index] = signed16(register16(base + 4u));
        regs->window_h[index] = register16(PREG_WIN0H + index * 2u);
        regs->window_v[index] = register16(PREG_WIN0V + index * 2u);
    }

    regs->window_in = register16(PREG_WININ);
    regs->window_out = register16(PREG_WINOUT);
    regs->blend = register16(PREG_BLDCNT);

    u16 alpha = register16(PREG_BLDALPHA);
    regs->eva = coefficient(alpha);
    regs->evb = coefficient(alpha >> 8);
    regs->evy = coefficient(register16(PREG_BLDY));
}

static
void reload_affine() {
    for (u32 index = 0; index < 2; index++) {
        u32 base = PREG_BG2PA + index * 0x10u + 8u;
        global_ppu.reference[index].x = signed28(register32(base));
        global_ppu.reference[index].y = signed28(register32(base + 4u));
    }
}

static
void advance_affine() {
    for (u32 index = 0; index < 2; index++) {
        u32 base = PREG_BG2PA + index * 0x10u;
        int pb = signed16(register16(base + 2u));
        int pd = signed16(register16(base + 6u));

        global_ppu.reference[index].x = signed28((u32)global_ppu.reference[index].x + (u32)pb);
        global_ppu.reference[index].y = signed28((u32)global_ppu.reference[index].y + (u32)pd);
    }
}

void ppu_on_io_write(u32 offset) {
    for (u32 index = 0; index < 2; index++) {
        u32 base = PREG_BG2PA + index * 0x10u + 8u;

        if (offset >= base && offset < base + 4u)
            global_ppu.reference[index].x = signed28(register32(base));
        else if (offset >= base + 4u && offset < base + 8u)
            global_ppu.reference[index].y = signed28(register32(base + 4u));
    }
}

static
u32 text_palette_index(u16 control, u32 sx, u32 sy) {
    u32 tx = sx >> 3;
    u32 ty = sy >> 3;
    u32 size = BGCNT_SIZE(control);
    u32 block = 0;

    if (size == 1)
        block = tx >> 5;
    else if (size == 2)
        block = ty >> 5;
    else if (size == 3)
        block = (tx >> 5) + (ty >> 5) * 2u;

    u32 map = BGCNT_MAP_BASE(control) + block * PPU_SCREEN_BLOCK_SIZE;
    u16 entry = video16(map + ((ty & 31u) * 32u + (tx & 31u)) * 2u);
    u32 px = sx & 7u;
    u32 py = sy & 7u;

    if (TILE_ENTRY_HFLIP(entry))
        px = 7u - px;

    if (TILE_ENTRY_VFLIP(entry))
        py = 7u - py;

    u32 character = BGCNT_CHAR_BASE(control);
    u32 tile = TILE_ENTRY_TILE(entry);

    if (BGCNT_COLOR256(control))
        return video8(character + tile * 64u + py * 8u + px);

    u8 packed = video8(character + tile * 32u + py * 4u + (px >> 1));
    u32 color = px & 1u ? packed >> 4 : packed & 15u;
    return color ? TILE_ENTRY_PALBANK(entry) * 16u + color : 0;
}

static
void render_text(u32 bg) {
    const ppu_registers_t* regs = &global_ppu.registers;
    u16 control = regs->bg_control[bg];
    u32 size = BGCNT_SIZE(control);
    u32 width = size & 1u ? 512u : 256u;
    u32 height = size & 2u ? 512u : 256u;
    u32 sy = (global_ppu.vcount + regs->bg_vofs[bg]) & (height - 1u);

    for (u32 x = 0; x < PPU_WIDTH; x++) {
        u32 sx = (x + regs->bg_hofs[bg]) & (width - 1u);
        u32 color = text_palette_index(control, sx, sy);
        if (color)
            global_ppu.line.bg_color[bg][x] = palette_color(color);
    }
}

static
bool affine_coordinates(u32 index, u32 x, u32 width, u32 height, bool wrap, u32* sx, u32* sy) {
    int source_x = fixed_floor(global_ppu.reference[index].x + global_ppu.registers.pa[index] * (int)x);
    int source_y = fixed_floor(global_ppu.reference[index].y + global_ppu.registers.pc[index] * (int)x);

    if (wrap) {
        *sx = wrap_coordinate(source_x, width);
        *sy = wrap_coordinate(source_y, height);
        return true;
    }

    if (source_x < 0 || source_y < 0 || source_x >= (int)width || source_y >= (int)height)
        return false;

    *sx = (u32)source_x;
    *sy = (u32)source_y;
    return true;
}

static
void render_affine(u32 bg) {
    u16 control = global_ppu.registers.bg_control[bg];
    u32 size = 128u << BGCNT_SIZE(control);
    u32 map = BGCNT_MAP_BASE(control);
    u32 character = BGCNT_CHAR_BASE(control);
    bool wrap = (control & 0x2000u) != 0;

    for (u32 x = 0; x < PPU_WIDTH; x++) {
        u32 sx, sy;
        if (!affine_coordinates(bg - 2u, x, size, size, wrap, &sx, &sy))
            continue;

        u32 tile = video8(map + (sy >> 3) * (size >> 3) + (sx >> 3));
        u32 color = video8(character + tile * 64u + (sy & 7u) * 8u + (sx & 7u));
        if (color)
            global_ppu.line.bg_color[bg][x] = palette_color(color);
    }
}

static
void render_bitmap(u32 mode) {
    u32 width = mode == 5 ? PPU_MODE5_WIDTH : PPU_WIDTH;
    u32 height = mode == 5 ? PPU_MODE5_HEIGHT : PPU_HEIGHT;
    u32 frame = mode != 3 && (global_ppu.registers.display & DISPCNT_FRAME_SELECT) ? PPU_BITMAP_FRAME_SIZE : 0;

    for (u32 x = 0; x < PPU_WIDTH; x++) {
        u32 sx, sy;
        if (!affine_coordinates(0, x, width, height, false, &sx, &sy))
            continue;

        u32 pixel = sy * width + sx;
        global_ppu.line.bg_color[2][x] = mode == 4
            ? palette_color(video8(frame + pixel))
            : video16(frame + pixel * 2u) & 0x7FFFu;
    }
}

static void render_backgrounds() {
    u32 mode = PPU_DISPCNT_MODE(global_ppu.registers.display);
    u32 text_count = mode == 0 ? 4u : mode == 1 ? 2u : 0u;

    for (u32 bg = 0; bg < PPU_BG_COUNT; bg++) {
        global_ppu.line.bg_priority[bg] = (u8)BGCNT_PRIORITY(global_ppu.registers.bg_control[bg]);

        if (!(global_ppu.registers.display & PPU_DISPCNT_BG_ENABLE(bg)))
            continue;

        if (bg < text_count)
            render_text(bg);
        else if ((mode == 1 && bg == 2) || (mode == 2 && bg >= 2))
            render_affine(bg);
        else if (mode >= 3 && mode <= 5 && bg == 2)
            render_bitmap(mode);
    }
}

static
u32 object_palette_index(u16 attr0, u16 attr2, u32 width, u32 px, u32 py) {
    bool color256 = (attr0 & 0x2000u) != 0;
    u32 units = color256 ? 2u : 1u;
    u32 tile = attr2 & 1023u;
    u32 stride = global_ppu.registers.display & DISPLAY_OBJ_1D ? (width >> 3) * units : 32u;

    if (color256)
        tile &= ~1u;

    tile = (tile + (py >> 3) * stride + (px >> 3) * units) & 1023u;
    u32 mode = PPU_DISPCNT_MODE(global_ppu.registers.display);
    if (mode >= 3 && mode <= 5 && tile < 512u)
        return 0;

    u32 address = 0x10000u + tile * 32u;
    if (color256) {
        u32 color = video8(address + (py & 7u) * 8u + (px & 7u));
        return color ? 256u + color : 0;
    }

    u8 packed = video8(address + (py & 7u) * 4u + ((px & 7u) >> 1));
    u32 color = px & 1u ? packed >> 4 : packed & 15u;
    return color ? 256u + ((attr2 >> 12) & 15u) * 16u + color : 0;
}

static
void render_objects() {
    static const u8 dimensions[3][4][2] = {
        { {8,8}, {16,16}, {32,32}, {64,64} },
        { {16,8}, {32,8}, {32,16}, {64,32} },
        { {8,16}, {8,32}, {16,32}, {32,64} }
    };

    u8* oam = mbus_get_region(MEMORY_BUS_REGION_OAM);

    for (u32 object = 0; object < OBJ_COUNT; object++) {
        u32 base = object * 8u;
        u16 attr0 = read_le16(oam, base);
        u16 attr1 = read_le16(oam, base + 2u);
        u16 attr2 = read_le16(oam, base + 4u);
        bool affine = (attr0 & 0x0100u) != 0;
        u32 mode = (attr0 >> 10) & 3u;
        u32 shape = (attr0 >> 14) & 3u;
        u32 size = attr1 >> 14;
        u8 priority = (u8)((attr2 >> 10) & 3u);

        if (shape == 3 || mode == 3 || (!affine && (attr0 & 0x0200u)))
            continue;
        if (mode == 2 && !(global_ppu.registers.display & DISPLAY_OBJWIN))
            continue;

        u32 width = dimensions[shape][size][0];
        u32 height = dimensions[shape][size][1];
        bool double_size = affine && (attr0 & 0x0200u);
        u32 box_width = double_size ? width * 2u : width;
        u32 box_height = double_size ? height * 2u : height;
        u32 local_y = (global_ppu.vcount - (attr0 & 255u)) & 255u;
        if (local_y >= box_height)
            continue;

        int screen_x = attr1 & 511u;
        if (screen_x >= 256)
            screen_x -= 512;

        int pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            u32 matrix = ((attr1 >> 9) & 31u) * 32u;
            pa = signed16(read_le16(oam, matrix + 6u));
            pb = signed16(read_le16(oam, matrix + 14u));
            pc = signed16(read_le16(oam, matrix + 22u));
            pd = signed16(read_le16(oam, matrix + 30u));
        }

        for (u32 local_x = 0; local_x < box_width; local_x++) {
            int x = screen_x + (int)local_x;
            if (x < 0 || x >= PPU_WIDTH)
                continue;

            if (mode != 2 && priority >= global_ppu.line.obj_priority[x])
                continue;

            int px, py;
            if (affine) {
                int dx = (int)local_x - (int)(box_width / 2u);
                int dy = (int)local_y - (int)(box_height / 2u);
                px = fixed_floor(pa * dx + pb * dy) + (int)(width / 2u);
                py = fixed_floor(pc * dx + pd * dy) + (int)(height / 2u);
            } else {
                px = (int)local_x;
                py = (int)local_y;
                if (attr1 & 0x1000u) px = (int)width - 1 - px;
                if (attr1 & 0x2000u) py = (int)height - 1 - py;
            }

            if (px < 0 || py < 0 || px >= (int)width || py >= (int)height)
                continue;

            u32 color = object_palette_index(attr0, attr2, width, (u32)px, (u32)py);
            if (!color)
                continue;

            if (mode == 2) {
                global_ppu.line.obj_window[x] = true;
            } else {
                global_ppu.line.obj_color[x] = palette_color(color);
                global_ppu.line.obj_priority[x] = priority;
                global_ppu.line.obj_semi[x] = mode == 1;
            }
        }
    }
}

static
bool window_contains(u32 position, u16 range) {
    u32 start = range >> 8;
    u32 end = range & 255u;
    if (start == end)
        return false;
    return start < end ? position >= start && position < end : position >= start || position < end;
}

static
u8 pixel_window_mask(u32 x) {
    const ppu_registers_t* regs = &global_ppu.registers;
    if (!(regs->display & DISPLAY_WINDOWS))
        return 0x3fu;

    for (u32 index = 0; index < 2; index++) {
        if ((regs->display & (DISPLAY_WIN0 << index))
            && window_contains(global_ppu.vcount, regs->window_v[index])
            && window_contains(x, regs->window_h[index]))
            return (u8)((regs->window_in >> (index * 8u)) & 0x3fu);
    }

    if ((regs->display & DISPLAY_OBJWIN) && global_ppu.line.obj_window[x])
        return (u8)((regs->window_out >> 8) & 0x3fu);
    return (u8)(regs->window_out & 0x3fu);
}

static
void insert_candidate(ppu_candidate_t pixel, ppu_candidate_t* top, ppu_candidate_t* second) {
    if (pixel.order < top->order) {
        *second = *top;
        *top = pixel;
    } else if (pixel.order < second->order) {
        *second = pixel;
    }
}

static
u16 alpha_blend(u16 first, u16 second, u32 eva, u32 evb) {
    u32 result = 0;
    for (u32 shift = 0; shift <= 10; shift += 5) {
        u32 a = (first >> shift) & 31u;
        u32 b = (second >> shift) & 31u;
        u32 color = (a * eva + b * evb) >> 4;
        if (color > 31u) color = 31u;
        result |= color << shift;
    }
    return (u16)result;
}

static
u16 brightness(u16 color, u32 amount, bool increase) {
    u32 result = 0;
    for (u32 shift = 0; shift <= 10; shift += 5) {
        u32 component = (color >> shift) & 31u;
        if (increase)
            component += ((31u - component) * amount) >> 4;
        else
            component -= (component * amount) >> 4;
        result |= component << shift;
    }
    return (u16)result;
}

static
u16 apply_effect(ppu_candidate_t top, ppu_candidate_t second, u8 mask) {
    const ppu_registers_t* regs = &global_ppu.registers;
    if (!(mask & WINDOW_EFFECTS))
        return top.color;

    u32 effect = (regs->blend >> 6) & 3u;
    bool first_target = (regs->blend & (1u << top.layer)) != 0;
    bool second_target = second.order != INVALID_ORDER && (regs->blend & (1u << (8u + second.layer))) != 0;

    if (second_target && (top.semi || (effect == 1 && first_target)))
        return alpha_blend(top.color, second.color, regs->eva, regs->evb);
    if (first_target && (effect == 2 || effect == 3))
        return brightness(top.color, regs->evy, effect == 2);
    return top.color;
}

static
void compose_scanline(u32* row) {
    u16 backdrop = palette_color(0);

    for (u32 x = 0; x < PPU_WIDTH; x++) {
        u8 mask = pixel_window_mask(x);
        ppu_candidate_t top = { backdrop, LAYER_BACKDROP, 100u, false };
        ppu_candidate_t second = { 0, LAYER_BACKDROP, INVALID_ORDER, false };

        for (u32 bg = 0; bg < PPU_BG_COUNT; bg++) {
            if (!(mask & (1u << bg)) || global_ppu.line.bg_color[bg][x] == PIXEL_NONE)
                continue;

            ppu_candidate_t pixel = {
                global_ppu.line.bg_color[bg][x], (u8)bg,
                (u32)global_ppu.line.bg_priority[bg] * 8u + 1u + bg, false
            };
            insert_candidate(pixel, &top, &second);
        }

        if ((mask & WINDOW_OBJ) && global_ppu.line.obj_color[x] != PIXEL_NONE) {
            ppu_candidate_t pixel = {
                global_ppu.line.obj_color[x], LAYER_OBJ,
                (u32)global_ppu.line.obj_priority[x] * 8u, global_ppu.line.obj_semi[x]
            };
            insert_candidate(pixel, &top, &second);
        }

        row[x] = framebuffer_color(apply_effect(top, second, mask));
    }
}

static
void clear_scanline() {
    memset(global_ppu.line.bg_color, 0xff, sizeof(global_ppu.line.bg_color));
    memset(global_ppu.line.obj_color, 0xff, sizeof(global_ppu.line.obj_color));
    memset(global_ppu.line.obj_priority, 4, sizeof(global_ppu.line.obj_priority));
    memset(global_ppu.line.obj_semi, 0, sizeof(global_ppu.line.obj_semi));
    memset(global_ppu.line.obj_window, 0, sizeof(global_ppu.line.obj_window));
}

static
void render_scanline() {
    snapshot_registers();
    u32* row = &global_ppu.framebuffer[global_ppu.vcount * PPU_WIDTH];

    if (global_ppu.registers.display & DISPCNT_FORCED_BLANK) {
        for (u32 x = 0; x < PPU_WIDTH; x++)
            row[x] = 0xffffffffu;
        return;
    }

    clear_scanline();
    render_backgrounds();
    if (global_ppu.registers.display & DISPLAY_OBJ)
        render_objects();
    compose_scanline(row);
}

static
void enter_hblank() {
    global_ppu.in_hblank = true;

    if (global_ppu.vcount < VISIBLE_LINES) {
        render_scanline();
        advance_affine();
    }

    u16 status = register16(PREG_DISPSTAT) | DISPSTAT_HBLANK_FLAG;
    io_write_raw16(PREG_DISPSTAT, status);
    if (status & DISPSTAT_HBLANK_IRQ)
        io_request_irq(IRQ_HBLANK);
    if (global_ppu.vcount < VISIBLE_LINES)
        dma_on_hblank();
}

static
void end_scanline() {
    global_ppu.line_cycles = 0;
    global_ppu.in_hblank = false;
    global_ppu.vcount = (global_ppu.vcount + 1u) % TOTAL_LINES;

    if (global_ppu.vcount == 0)
        reload_affine();

    u16 status = (u16)(register16(PREG_DISPSTAT) & ~DISPSTAT_HBLANK_FLAG);
    bool entering_vblank = global_ppu.vcount == VISIBLE_LINES;
    bool vcount_match = global_ppu.vcount == (u32)(status >> 8);

    if (entering_vblank) {
        status |= DISPSTAT_VBLANK_FLAG;
        global_ppu.frame_ready = true;
    } else if (global_ppu.vcount == TOTAL_LINES - 1u) {
        status = (u16)(status & ~DISPSTAT_VCOUNT_FLAG);
    }

    if (vcount_match)
        status |= DISPSTAT_VCOUNT_FLAG;
    else
        status = (u16)(status & ~DISPSTAT_VCOUNT_FLAG);

    io_write_raw16(PREG_DISPSTAT, status);
    io_write_raw16(PREG_VCOUNT, (u16)global_ppu.vcount);

    if (entering_vblank && (status & DISPSTAT_VBLANK_IRQ))
        io_request_irq(IRQ_VBLANK);
    if (vcount_match && (status & DISPSTAT_VCOUNT_IRQ))
        io_request_irq(IRQ_VCOUNT);
    if (entering_vblank)
        dma_on_vblank();
}

void ppu_reset() {
    memset(&global_ppu, 0, sizeof(global_ppu));
    reload_affine();

    u16 status = register16(PREG_DISPSTAT);
    status = (u16)(status & ~(DISPSTAT_VBLANK_FLAG | DISPSTAT_HBLANK_FLAG | DISPSTAT_VCOUNT_FLAG));
    if ((status >> 8) == 0)
        status |= DISPSTAT_VCOUNT_FLAG;
    io_write_raw16(PREG_DISPSTAT, status);
    io_write_raw16(PREG_VCOUNT, 0);
}

void ppu_step(u32 cycles) {
    while (cycles) {
        u32 boundary = global_ppu.in_hblank ? SCANLINE_CYCLES : HDRAW_CYCLES;
        u32 remaining = boundary - global_ppu.line_cycles;
        u32 elapsed = cycles < remaining ? cycles : remaining;

        global_ppu.line_cycles += elapsed;
        cycles -= elapsed;
        if (global_ppu.line_cycles < boundary)
            break;

        if (global_ppu.in_hblank)
            end_scanline();
        else
            enter_hblank();
    }
}

bool ppu_take_frame_ready() {
    bool ready = global_ppu.frame_ready;
    global_ppu.frame_ready = false;
    return ready;
}

u32* ppu_get_framebuffer() {
    return global_ppu.framebuffer;
}