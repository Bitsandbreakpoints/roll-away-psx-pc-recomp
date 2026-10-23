/*
 * level_model.cpp - the level editor's level (level_model.h).
 *
 * The game's level data (see level/level_file.h): the grid, the counts,
 * then one 128-word property per block that needs one, in grid order.
 * A property of kind 0..4 is a block with things: 6 sides x 16 words
 * (id, rotation, variant, state, index, power, target, 0, animation...),
 * the block's position (x, z, 33 - y) in its last 3 words.  The level's
 * information (kind 666) follows, then nothing.
 */
#include "editor/level_model.h"
#include "level/level_file.h"

#include <stdlib.h>
#include <string.h>
#include <algorithm>

#define FF 0xFFFF
#define INFO_KIND 666
#define FLAGS_KIND 9

/* the templates: the most common words of each thing in the game's levels */
#define T(id, rot, var, state, index, f8, f9, f13) \
    { (uint16_t) (id), rot, var, state, index, FF, FF, 0, f8, f9, FF, FF, FF, f13, FF, FF }

const ThingInfo g_things[] = {
    { THING_START, "Start", "Essentials", 0xFF40C0FF, THING_ROTATES, T(30, 3, 0, 0, FF, 0x100, 0, 0) },
    { THING_KEY, "Key", "Essentials", 0xFF30D8FF, THING_COLLECT, T(31, 0, 0, 1, 0, 0x182, 1, 0xFF9C) },
    { THING_EXIT, "Exit", "Essentials", 0xFF3040E0, 0, T(7, 0, 0, 2, FF, 0x1F4, 1, 0x1E) },
    { THING_FRUIT, "Fruit", "Essentials", 0xFF3050F0, THING_COLLECT, T(46, 0, 0, 1, 0, 0x182, 1, 0x64) },
    { THING_HIDDEN_EXIT, "Hidden exit", "Essentials", 0xFFA040C0, 0, T(26, 0, 0, 2, FF, 0x1F4, 1, 0x1E) },
    { THING_COIN, "Coin", "Treasure", 0xFF20C8F0, THING_COLLECT | THING_KINDS, T(37, 0, 0, 1, 0, 0x182, 1, 0xFF9C) },
    { THING_GEM, "Gem", "Treasure", 0xFFF0A040, THING_COLLECT | THING_KINDS, T(36, 0, 0, 1, 0, 0x182, 1, 0xFFEF) },
    { THING_SUNGLASSES, "Sunglasses", "Power-ups", 0xFF303030, THING_COLLECT, T(38, 0, 0, 1, 0, 0x182, 1, 0xFF9C) },
    { THING_BOUNCE_PILL, "Bounce pill", "Power-ups", 0xFF50E070, THING_COLLECT, T(33, 0, 0, 1, 0, 0x182, 2, 0x64) },
    { THING_INVINCIBILITY, "Invincibility pill", "Power-ups", 0xFFE070E0, THING_COLLECT, T(34, 0, 0, 1, 0, 0x182, 2, 0x64) },
    { THING_TIMER_FLIP, "Hourglass", "Power-ups", 0xFFE0C060, THING_COLLECT, T(35, 0, 0, 1, 0, 0x182, 2, 0x64) },
    { THING_LETHARGY, "Lethargy pill", "Power-ups", 0xFF808080, THING_COLLECT, T(32, 0, 0, 1, 0, 0x182, 2, 0x64) },
    { THING_TIME_STOP, "Time stop", "Power-ups", 0xFFFFC080, 0, T(8, 0, 0, 0, FF, 0x100, 0, 0) },
    { THING_SPIKES, "Spikes", "Hazards", 0xFF2020B0, 0, T(12, 0, 0, 1, FF, 0x100, 0, 0) },
    { THING_MOVING_SPIKES, "Moving spikes", "Hazards", 0xFF5050E0, THING_TIMED, T(11, 0, 0, 1, FF, 0x100, 0, 0) },
    { THING_FIRE_PATCH, "Fire patch", "Hazards", 0xFF2060FF, 0, T(1, 0, 0, 0, FF, FF, FF, FF) },
    { THING_ICE_PATCH, "Ice patch", "Hazards", 0xFFFFE0A0, 0, T(2, 0, 0, 0, FF, FF, FF, FF) },
    { THING_INVISIBLE_PATCH, "Invisible patch", "Hazards", 0xFFC0C0C0, 0, T(3, 0, 0, 0, FF, FF, FF, FF) },
    { THING_ACID_PATCH, "Acid patch", "Hazards", 0xFF40E080, 0, T(4, 0, 0, 0, FF, FF, FF, FF) },
    { THING_BOUNCE_PAD, "Bounce pad", "Gizmos", 0xFF60A0FF, 0, T(10, 0, 0, 1, FF, 0x100, 3, 0) },
    { THING_ARROW, "Arrow", "Gizmos", 0xFF40FFFF, THING_ROTATES, T(28, 4, 0, 1, FF, 0x100, 0, 0) },
    { THING_TRANSPORTER, "Transporter", "Gizmos", 0xFFFFB040,
      THING_ROTATES | THING_COLOURED | THING_TOGGLES, T(5, 1, 0, 1, FF, 0x100, 1, 0x1E) },
    { THING_BUTTON, "Button", "Gizmos", 0xFF4080FF, THING_COLOURED | THING_TOGGLES,
      T(9, 0, 0, 1, FF, 0x100, 0, 0) },
    { THING_SLOW_STAR, "Slow star", "Enemies", 0xFF40A0E0, THING_ROTATES, T(50, 4, 0, 0, FF, FF, FF, FF) },
    { THING_FAST_STAR, "Fast star", "Enemies", 0xFF2060F0, THING_ROTATES, T(52, 4, 0, 0, FF, FF, FF, FF) },
    { THING_TIRE, "Tire", "Enemies", 0xFF404040, THING_ROTATES, T(51, 4, 0, 0, FF, FF, FF, FF) },
    { THING_CAPTURE_POD, "Capture pod", "Enemies", 0xFFC08040, THING_ROTATES, T(53, 0, 0, 0, FF, FF, FF, FF) },
    { THING_CAPTIVATOR, "Captivator", "Enemies", 0xFF8040C0, THING_ROTATES, T(56, 0, 0, 0, FF, FF, FF, FF) },
    { THING_HEDGEHOG, "Hedgehog", "Enemies", 0xFF305080, THING_COLLECT, T(42, 0, 0, 1, 0, 0x1A0, 0, 0x64) },
};
const int g_thing_count = (int) (sizeof(g_things) / sizeof(g_things[0]));

const BlockKindInfo g_block_kinds[BLOCK_KIND_COUNT] = {
    { "Normal", 0xFFFFFFFF },
    { "Fire", 0xFF3070FF },
    { "Ice", 0xFFFFD8A0 },
    { "Invisible", 0xFFE0E0E0 },
    { "Acid", 0xFF50E070 },
    { "Moving", 0xFFFFB070 },
    { "Crumbling", 0xFF7090B0 },
    { "Flashing", 0xFF70F0F0 },
    { "Laser", 0xFF8070FF },
};

const ThingInfo *thing_info(int id)
{
    for (int i = 0; i < g_thing_count; i++) {
        if (g_things[i].id == id) {
            return &g_things[i];
        }
    }
    return NULL;
}

const char *thing_variant_name(int id, int variant)
{
    static const char *const coins[] = { "Gold coin", "Silver coin", "Bronze coin" };
    static const char *const gems[] = { "Blue gem", "Green gem", "Red gem" };
    if (variant >= 0 && variant < 3) {
        if (id == THING_COIN) {
            return coins[variant];
        }
        if (id == THING_GEM) {
            return gems[variant];
        }
    }
    const ThingInfo *info = thing_info(id);
    return info ? info->name : "Unknown thing";
}

static void empty_thing(Thing *t)
{
    static const uint16_t none[16] = { 0, FF, FF, 0, FF, FF, FF, 0, FF, FF, FF, FF, FF, FF, FF, FF };
    memcpy(t->w, none, sizeof(none));
    t->power = Link();
    t->target = Link();
}

bool Block::has_things() const
{
    if (kind > BLOCK_ACID) {
        return false;
    }
    for (int s = 0; s < 6; s++) {
        if (sides[s].id()) {
            return true;
        }
    }
    return false;
}

Level::Level()
{
    clear();
}

void Level::position(int cell, int *x, int *y, int *z)
{
    *x = cell / (GRID * GRID);
    *z = cell / GRID % GRID;
    *y = GRID - 1 - cell % GRID;
}

bool Level::inside(int x, int y, int z)
{
    return x >= 0 && x < GRID && y >= 0 && y < GRID && z >= 0 && z < GRID;
}

void Level::side_normal(int side, int n[3])
{
    static const int normals[6][3] = {
        { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, -1, 0 }
    };
    n[0] = normals[side][0];
    n[1] = normals[side][1];
    n[2] = normals[side][2];
}

int Level::opposite(int side)
{
    static const int other[6] = { SIDE_BOTTOM, SIDE_LEFT, SIDE_BACK, SIDE_FRONT, SIDE_RIGHT, SIDE_TOP };
    return other[side];
}

int Level::neighbour(int c, int side)
{
    int x, y, z, n[3];
    position(c, &x, &y, &z);
    side_normal(side, n);
    x += n[0];
    y += n[1];
    z += n[2];
    return inside(x, y, z) ? cell(x, y, z) : -1;
}

void Level::clear()
{
    m_blocks.clear();
    settings = LevelSettings();
    for (int i = 0; i < 128; i++) {
        m_info[i] = FF;
    }
    m_info[0] = INFO_KIND;
    m_info[4] = 0xFFFB;
    m_info[5] = 0;
    m_had_flags = false;
    memset(m_flags, 0, sizeof(m_flags));
    m_flags[0] = FLAGS_KIND;
}

void Level::make_new()
{
    clear();
    int c = cell(16, 16, 16);
    add_block(c, BLOCK_NORMAL);
    set_thing(&m_blocks[c].sides[SIDE_TOP], THING_START);
}

Block *Level::block(int c)
{
    auto it = m_blocks.find(c);
    return it == m_blocks.end() ? NULL : &it->second;
}

const Block *Level::block(int c) const
{
    auto it = m_blocks.find(c);
    return it == m_blocks.end() ? NULL : &it->second;
}

void Level::set_thing(Thing *t, int id)
{
    const ThingInfo *info = thing_info(id);
    if (!info || id == THING_NONE) {
        empty_thing(t);
        return;
    }
    memcpy(t->w, info->words, sizeof(t->w));
    t->power = Link();
    t->target = Link();
}

void Level::init_property(Block *b, int kind, int c)
{
    int x, y, z;
    position(c, &x, &y, &z);
    for (int i = 0; i < 128; i++) {
        b->prop[i] = FF;
    }
    b->prop[0] = (uint16_t) kind;
    b->laser_power = Link();
    switch (kind) {
    case BLOCK_MOVING:
        b->prop[1] = SIDE_FRONT;            /* direction (a side) */
        b->prop[3] = 1;
        b->prop[17] = 2;                    /* speed */
        b->prop[18] = 30;                   /* (as all the game's) */
        b->prop[122] = b->prop[123] = b->prop[124] = 0x100;
        b->length = 2;
        break;
    case BLOCK_CRUMBLING:
        b->prop[1] = 1;                     /* whole */
        break;
    case BLOCK_FLASHING:
        b->prop[1] = 0;
        b->prop[2] = 0;                     /* timing 0..3 */
        b->prop[17] = 1;
        break;
    case BLOCK_LASER:
        b->prop[1] = SIDE_RIGHT;
        b->prop[3] = 1;                     /* on */
        b->prop[17] = 1;
        b->prop[22] = 0;                    /* colour */
        b->length = 0;
        break;
    }
}

bool Level::add_block(int c, int kind)
{
    if (c < 0 || c >= GRID * GRID * GRID) {
        return false;
    }
    Block &b = m_blocks[c];
    b = Block();
    b.kind = kind;
    for (int s = 0; s < 6; s++) {
        empty_thing(&b.sides[s]);
    }
    init_property(&b, kind, c);
    drop_covered_things();
    update_paths();
    return true;
}

void Level::remove_block(int c)
{
    m_blocks.erase(c);
    /* references to it go */
    for (auto &kv : m_blocks) {
        Block &b = kv.second;
        for (int s = 0; s < 6; s++) {
            if (b.sides[s].power.cell == c) {
                b.sides[s].power = Link();
            }
            if (b.sides[s].target.cell == c) {
                b.sides[s].target = Link();
            }
        }
        if (b.laser_power.cell == c) {
            b.laser_power = Link();
        }
    }
    update_paths();
}

bool Level::side_open(int c, int side) const
{
    int n = neighbour(c, side);
    return n < 0 || !block(n);
}

void Level::drop_covered_things()
{
    for (auto &kv : m_blocks) {
        for (int s = 0; s < 6; s++) {
            if (kv.second.sides[s].id() && !side_open(kv.first, s)) {
                empty_thing(&kv.second.sides[s]);
            }
        }
    }
}

int Level::property_count() const
{
    int n = 0;
    for (auto &kv : m_blocks) {
        n += kv.second.needs_property();
    }
    return n;
}

int Level::count_things(int id) const
{
    int n = 0;
    for (auto &kv : m_blocks) {
        if (kv.second.kind <= BLOCK_ACID) {
            for (int s = 0; s < 6; s++) {
                n += kv.second.sides[s].id() == id;
            }
        }
    }
    return n;
}

static int axis_of(int side)
{
    /* the game's "orientation": 5 up/down, 1 left/right, 2 forward/back */
    return side == SIDE_TOP || side == SIDE_BOTTOM ? 5 : side == SIDE_RIGHT || side == SIDE_LEFT ? 1 : 2;
}

void Level::update_paths()
{
    for (auto &kv : m_blocks) {
        Block &b = kv.second;
        if (b.kind != BLOCK_MOVING && b.kind != BLOCK_LASER) {
            continue;
        }
        int side = b.prop[1] < 6 ? b.prop[1] : SIDE_RIGHT;
        int length = b.length;
        int far = kv.first, steps = 0;
        if (b.kind == BLOCK_LASER && length == 0) {
            /* the beam: up to the first block (or 8 cells) */
            for (int c = neighbour(kv.first, side); c >= 0 && steps < GRID; c = neighbour(c, side)) {
                far = c;
                steps++;
                if (block(c) || steps >= 8) {
                    break;
                }
            }
        } else {
            for (int c = neighbour(kv.first, side); c >= 0 && steps < length; c = neighbour(c, side)) {
                far = c;
                steps++;
            }
        }
        int x0, y0, z0, x1, y1, z1;
        position(kv.first, &x0, &y0, &z0);
        position(far, &x1, &y1, &z1);
        /* the game's path runs from the smaller (x, z, 33 - y) to the larger */
        int a[3] = { x0, z0, GRID - 1 - y0 }, e[3] = { x1, z1, GRID - 1 - y1 };
        bool at_from = a[0] <= e[0] && a[1] <= e[1] && a[2] <= e[2];
        const int *from = at_from ? a : e, *to = at_from ? e : a;
        for (int i = 0; i < 3; i++) {
            b.prop[4 + i] = (uint16_t) from[i];
            b.prop[7 + i] = (uint16_t) to[i];
        }
        b.prop[2] = (uint16_t) axis_of(side);
        if (b.kind == BLOCK_MOVING) {
            b.prop[16] = at_from ? 0 : 1;
        }
    }
}

std::vector<Level::Problem> Level::check() const
{
    std::vector<Problem> out;
    int starts = 0, start_cell = -1;
    for (auto &kv : m_blocks) {
        if (kv.second.kind > BLOCK_ACID) {
            continue;
        }
        for (int s = 0; s < 6; s++) {
            if (kv.second.sides[s].id() == THING_START) {
                starts++;
                start_cell = kv.first;
            }
        }
    }
    if (starts == 0) {
        out.push_back({ true, "There is no start: put the Start on a block.", -1 });
    } else if (starts > 1) {
        out.push_back({ true, "There is more than one start.", start_cell });
    }
    int props = property_count();
    if (props > MAX_PROPERTIES) {
        out.push_back({ true, "Too many special blocks and blocks with things on them (" +
                       std::to_string(props) + " of " + std::to_string(MAX_PROPERTIES) + ").", -1 });
    }
    int collect = 0;
    for (int i = 0; i < g_thing_count; i++) {
        if (g_things[i].flags & THING_COLLECT) {
            collect += count_things(g_things[i].id);
        }
    }
    if (collect > MAX_COLLECTABLES) {
        out.push_back({ true, "Too many things to pick up (" + std::to_string(collect) + " of " +
                       std::to_string(MAX_COLLECTABLES) + ").", -1 });
    }
    int exits = count_things(THING_EXIT), keys = count_things(THING_KEY);
    if (!settings.bonus) {
        if (exits == 0) {
            out.push_back({ false, "There is no exit: the level cannot be finished.", -1 });
        } else if (exits > 1) {
            out.push_back({ false, "There is more than one exit.", -1 });
        }
        if (keys == 0 && exits) {
            out.push_back({ false, "No keys: the exit is open from the start.", -1 });
        }
    } else if (keys) {
        out.push_back({ false, "A bonus level with keys: the game's bonus levels have none.", -1 });
    }
    if (count_things(THING_FRUIT) > 1) {
        out.push_back({ false, "There is more than one fruit (the game's levels have one).", -1 });
    }
    if ((int) m_blocks.size() > MANY_BLOCKS) {
        out.push_back({ false, std::to_string(m_blocks.size()) + " blocks: more than any of the "
                       "game's levels (" + std::to_string(MANY_BLOCKS) + ").", -1 });
    }
    for (auto &kv : m_blocks) {
        const Block &b = kv.second;
        if (b.kind > BLOCK_ACID) {
            continue;
        }
        for (int s = 0; s < 6; s++) {
            if (b.sides[s].id() == THING_TRANSPORTER && !b.sides[s].target.valid()) {
                out.push_back({ false, "A transporter leads nowhere (choose its destination).", kv.first });
            }
        }
    }
    return out;
}

/* ---- the game's data ---- */

static uint16_t rd(const uint8_t *d, int i)
{
    return (uint16_t) (d[2 * i] | d[2 * i + 1] << 8);
}

static Link decode(uint16_t code, const std::vector<int> &prop_cells)
{
    Link l;
    if (code == FF || (int) (code >> 4) >= (int) prop_cells.size() || (code & 15) > LINK_SELF) {
        return l;
    }
    l.cell = prop_cells[code >> 4];
    l.side = code & 15;
    return l;
}

bool Level::from_game(const uint8_t *data, int size, std::string *error)
{
    char reason[256];
    if (!level_data_check(data, size, reason, sizeof(reason))) {
        *error = reason;
        return false;
    }
    LevelSettings keep = settings;
    clear();
    settings.name = keep.name;
    settings.theme = keep.theme;
    int count = rd(data, LEVEL_CELLS + 2);
    const int base = LEVEL_CELLS + 3;
    std::vector<int> prop_cells(count, -1);
    for (int c = 0; c < LEVEL_CELLS; c++) {
        int v = rd(data, c);
        if (v != FF && v >= 5) {
            prop_cells[v - 5] = c;
        }
    }
    for (int c = 0; c < LEVEL_CELLS; c++) {
        int v = rd(data, c);
        if (v == FF) {
            continue;
        }
        Block &b = m_blocks[c];
        b = Block();
        for (int s = 0; s < 6; s++) {
            empty_thing(&b.sides[s]);
        }
        if (v < 5) {
            b.kind = v;
            init_property(&b, v, c);
            continue;
        }
        const uint8_t *p = data + 2 * (base + (v - 5) * LEVEL_PROPERTY_WORDS);
        for (int i = 0; i < 128; i++) {
            b.prop[i] = rd(p, i);
        }
        b.kind = b.prop[0] < BLOCK_KIND_COUNT ? b.prop[0] : BLOCK_NORMAL;
        if (b.kind <= BLOCK_ACID) {
            for (int s = 0; s < 6; s++) {
                Thing &t = b.sides[s];
                for (int i = 0; i < 16; i++) {
                    t.w[i] = b.prop[1 + 16 * s + i];
                }
                if (t.id() == THING_TRANSPORTER || t.id() == THING_BUTTON) {
                    t.power = decode(t.w[5], prop_cells);
                    t.target = decode(t.w[6], prop_cells);
                }
            }
        } else if (b.kind == BLOCK_LASER) {
            b.laser_power = decode(b.prop[23], prop_cells);
        }
        if (b.kind == BLOCK_MOVING || b.kind == BLOCK_LASER) {
            /* the editor's length: the path's */
            int len = 0;
            for (int i = 0; i < 3; i++) {
                len = std::max(len, abs((int) b.prop[7 + i] - (int) b.prop[4 + i]));
            }
            b.length = len;
        }
    }
    /* what follows the properties: the flags (9) and the information (666) */
    for (int at = base + count * LEVEL_PROPERTY_WORDS; at + LEVEL_PROPERTY_WORDS <= size / 2;
         at += LEVEL_PROPERTY_WORDS) {
        int kind = rd(data, at);
        uint16_t *dest = kind == INFO_KIND ? m_info : kind == FLAGS_KIND ? m_flags : NULL;
        if (!dest) {
            break;
        }
        for (int i = 0; i < 128; i++) {
            dest[i] = rd(data, at + i);
        }
    }
    /* (the flags are counted with the properties in the game's levels) */
    for (int k = 0; k < count; k++) {
        if (prop_cells[k] < 0) {
            const uint8_t *p = data + 2 * (base + k * LEVEL_PROPERTY_WORDS);
            if (rd(p, 0) == FLAGS_KIND) {
                for (int i = 0; i < 128; i++) {
                    m_flags[i] = rd(p, i);
                }
                m_had_flags = true;
            }
        }
    }
    settings.seconds = m_info[6] == FF ? 99 : m_info[6];
    settings.hidden = m_flags[1] == 1;
    settings.reverse_invisible = m_flags[2] == 1;
    return true;
}

static void put(std::vector<uint8_t> &out, int index, uint16_t value)
{
    out[2 * index] = (uint8_t) value;
    out[2 * index + 1] = (uint8_t) (value >> 8);
}

std::vector<uint8_t> Level::to_game() const
{
    /* the properties' order: the grid's */
    std::map<int, int> prop_of;
    int count = 0;
    for (auto &kv : m_blocks) {
        if (kv.second.needs_property()) {
            prop_of[kv.first] = count++;
        }
    }
    auto encode = [&](const Link &l) -> uint16_t {
        auto it = prop_of.find(l.cell);
        if (!l.valid() || it == prop_of.end()) {
            return FF;
        }
        return (uint16_t) (it->second * 16 + l.side);
    };
    bool flags = settings.hidden || settings.reverse_invisible || m_had_flags;
    int total_props = count + (flags ? 1 : 0);
    int words = LEVEL_CELLS + 3 + (total_props + 1) * LEVEL_PROPERTY_WORDS;
    std::vector<uint8_t> out((size_t) words * 2, 0);
    for (int c = 0; c < LEVEL_CELLS; c++) {
        put(out, c, FF);
    }
    put(out, LEVEL_CELLS, (uint16_t) m_blocks.size());
    put(out, LEVEL_CELLS + 1, 0);
    put(out, LEVEL_CELLS + 2, (uint16_t) total_props);
    int collect_index = 0, lowest = -1;
    int base = LEVEL_CELLS + 3;
    int cx = 0, cy = 0, cz = 0;
    for (auto &kv : m_blocks) {
        const Block &b = kv.second;
        int x, y, z;
        position(kv.first, &x, &y, &z);
        cx += x;
        cy += GRID - 1 - y;
        cz += z;
        if (lowest < 0) {
            lowest = kv.first;
        }
        if (!b.needs_property()) {
            put(out, kv.first, (uint16_t) b.kind);
            continue;
        }
        int k = prop_of[kv.first];
        put(out, kv.first, (uint16_t) (5 + k));
        uint16_t p[128];
        memcpy(p, b.prop, sizeof(p));
        p[0] = (uint16_t) b.kind;
        if (b.kind <= BLOCK_ACID) {
            for (int s = 0; s < 6; s++) {
                Thing t = b.sides[s];
                const ThingInfo *info = thing_info(t.id());
                if (info && (info->flags & THING_COLLECT)) {
                    t.w[4] = (uint16_t) collect_index++;
                }
                if (t.id() == THING_TRANSPORTER || t.id() == THING_BUTTON) {
                    t.w[5] = encode(t.power);
                    t.w[6] = encode(t.target);
                }
                for (int i = 0; i < 16; i++) {
                    p[1 + 16 * s + i] = t.w[i];
                }
            }
        } else {
            if (b.kind == BLOCK_MOVING || b.kind == BLOCK_LASER) {
                p[20] = (uint16_t) (5 + k);
            }
            if (b.kind == BLOCK_MOVING || b.kind == BLOCK_CRUMBLING) {
                int at = b.kind == BLOCK_MOVING ? 119 : 2;
                p[at] = (uint16_t) (x * 512);
                p[at + 1] = (uint16_t) (z * 512);
                p[at + 2] = (uint16_t) ((GRID - 1 - y) * 512);
            }
            if (b.kind == BLOCK_LASER) {
                p[23] = encode(b.laser_power);
            }
        }
        p[125] = (uint16_t) x;
        p[126] = (uint16_t) z;
        p[127] = (uint16_t) (GRID - 1 - y);
        for (int i = 0; i < 128; i++) {
            put(out, base + k * LEVEL_PROPERTY_WORDS + i, p[i]);
        }
    }
    int at = base + count * LEVEL_PROPERTY_WORDS;
    if (flags) {
        uint16_t f[128];
        memcpy(f, m_flags, sizeof(f));
        f[0] = FLAGS_KIND;
        f[1] = settings.hidden ? 1 : 0;
        f[2] = settings.reverse_invisible ? 1 : 0;
        for (int i = 0; i < 128; i++) {
            put(out, at + i, f[i]);
        }
        at += LEVEL_PROPERTY_WORDS;
    }
    uint16_t info[128];
    memcpy(info, m_info, sizeof(info));
    info[0] = INFO_KIND;
    if (!m_blocks.empty() && info[1] == FF) {
        /* (a point of the level: its middle) */
        int n = (int) m_blocks.size();
        info[1] = (uint16_t) (cx / n);
        info[2] = (uint16_t) (cz / n);
        info[3] = (uint16_t) (cy / n);
    }
    info[6] = (uint16_t) settings.seconds;
    for (int i = 0; i < 128; i++) {
        put(out, at + i, info[i]);
    }
    return out;
}
