/*
 * level_model.h - the level editor's level: the blocks of the 34 x 34 x 34
 * grid, the things on their sides and the special blocks, made from and
 * turned back into the game's own level data (level/level_file.h).
 *
 * Everything the game keeps is kept: a block's property words are held
 * as they were read (or as a template from the game's levels), and only
 * the fields the editor knows are changed.  References between things
 * (a transporter's destination, what a button powers) are held as
 * (block, side) and turned into the game's (property * 16 + side) when
 * the level is written.
 */
#ifndef EDITOR_LEVEL_MODEL_H
#define EDITOR_LEVEL_MODEL_H

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

#define GRID                    34
#define LINK_SELF               6       /* a reference to a block, not a side of it */
#define MAX_PROPERTIES          76      /* the most a level of the game has (MARS, 126) */
#define MAX_COLLECTABLES        32      /* things that can be picked up (their index) */
#define MANY_BLOCKS             200     /* more than any level of the game has */

/* block kinds (the game's numbers; 0..4 can carry things on their sides) */
enum BlockKind {
    BLOCK_NORMAL = 0,
    BLOCK_FIRE = 1,
    BLOCK_ICE = 2,
    BLOCK_INVISIBLE = 3,
    BLOCK_ACID = 4,
    BLOCK_MOVING = 5,
    BLOCK_CRUMBLING = 6,
    BLOCK_FLASHING = 7,
    BLOCK_LASER = 8,
    BLOCK_KIND_COUNT = 9
};

/* the sides, in the game's order */
enum Side {
    SIDE_TOP = 0,                       /* +Y (up) */
    SIDE_RIGHT = 1,                     /* +X */
    SIDE_FRONT = 2,                     /* +Z */
    SIDE_BACK = 3,                      /* -Z */
    SIDE_LEFT = 4,                      /* -X */
    SIDE_BOTTOM = 5                     /* -Y */
};

/* things on a side (the game's numbers) */
enum ThingId {
    THING_NONE = 0,
    THING_FIRE_PATCH = 1,
    THING_ICE_PATCH = 2,
    THING_INVISIBLE_PATCH = 3,
    THING_ACID_PATCH = 4,
    THING_TRANSPORTER = 5,
    THING_EXIT = 7,
    THING_TIME_STOP = 8,
    THING_BUTTON = 9,
    THING_BOUNCE_PAD = 10,
    THING_MOVING_SPIKES = 11,
    THING_SPIKES = 12,
    THING_HIDDEN_EXIT = 26,
    THING_ARROW = 28,
    THING_START = 30,
    THING_KEY = 31,
    THING_LETHARGY = 32,
    THING_BOUNCE_PILL = 33,
    THING_INVINCIBILITY = 34,
    THING_TIMER_FLIP = 35,
    THING_GEM = 36,
    THING_COIN = 37,
    THING_SUNGLASSES = 38,
    THING_HEDGEHOG = 42,
    THING_FRUIT = 46,
    THING_SLOW_STAR = 50,
    THING_TIRE = 51,
    THING_FAST_STAR = 52,
    THING_CAPTURE_POD = 53,
    THING_CAPTIVATOR = 56
};

/* thing flags */
enum {
    THING_ROTATES = 1,                  /* faces one of 4 directions */
    THING_COLLECT = 2,                  /* picked up: has an index of its own */
    THING_COLOURED = 4,                 /* variant = colour (transporter, button) */
    THING_TIMED = 8,                    /* variant = timing 0..3 (moving spikes) */
    THING_KINDS = 16,                   /* variant = kind (coins, gems) */
    THING_TOGGLES = 32                  /* state 1 on / 2 off */
};

struct ThingInfo {
    int id;
    const char *name;
    const char *group;
    uint32_t colour;                    /* IM_COL32 (0xAABBGGRR) */
    int flags;
    uint16_t words[16];                 /* the template of its 16 words */
};

/* the things the editor offers, in palette order */
extern const ThingInfo g_things[];
extern const int g_thing_count;
const ThingInfo *thing_info(int id);

/* "Gold coin" etc. for a thing with kinds */
const char *thing_variant_name(int id, int variant);

struct BlockKindInfo {
    const char *name;
    uint32_t colour;                    /* tint of the world's texture */
};
extern const BlockKindInfo g_block_kinds[BLOCK_KIND_COUNT];

/* a reference to a side of a block (or the block, LINK_SELF) */
struct Link {
    int cell = -1;                      /* grid index, -1 none */
    int side = 0;
    bool valid() const { return cell >= 0; }
    bool operator==(const Link &o) const { return cell == o.cell && side == o.side; }
};

struct Thing {
    uint16_t w[16];                     /* the game's words (w[0] the id) */
    Link power;                         /* w[5]: the next in the power chain */
    Link target;                        /* w[6]: a transporter's destination, a button's 2nd */
    int id() const { return w[0]; }
};

struct Block {
    int kind = BLOCK_NORMAL;
    Thing sides[6];
    uint16_t prop[128];                 /* moving, crumbling, flashing, laser: the game's words */
    Link laser_power;                   /* a laser's prop[23] */
    int length = 0;                     /* moving: distance; laser: beam, 0 up to a block */
    bool has_things() const;
    bool needs_property() const { return kind > BLOCK_ACID || has_things(); }
};

struct LevelSettings {
    std::string name;
    int theme = 0;                      /* the world: level/level_file.h */
    int seconds = 99;                   /* the starting time */
    bool bonus = false;                 /* a bonus level (else a normal one) */
    std::string next_level;             /* the level after the exit (a full path, "" none) */
    std::string next_hidden;            /* ... after the hidden exit */
    bool hidden = false;                /* the level flags (property 9) */
    bool reverse_invisible = false;
};

class Level {
public:
    Level();

    /* cells: index (x * 34 + z) * 34 + (33 - y) as the game's */
    static int cell(int x, int y, int z) { return (x * GRID + z) * GRID + (GRID - 1 - y); }
    static void position(int cell, int *x, int *y, int *z);
    static bool inside(int x, int y, int z);
    /* the cell next to `cell` across `side`, -1 outside the grid */
    static int neighbour(int cell, int side);
    static void side_normal(int side, int n[3]);
    static int opposite(int side);

    void clear();
    /* a new level: one block with the start on it */
    void make_new();

    Block *block(int cell);
    const Block *block(int cell) const;
    bool add_block(int cell, int kind);
    void remove_block(int cell);
    /* a side is free if no block covers it */
    bool side_open(int cell, int side) const;
    /* things on sides a new block covers are taken away */
    void drop_covered_things();

    const std::map<int, Block> &blocks() const { return m_blocks; }
    std::map<int, Block> &blocks() { return m_blocks; }

    LevelSettings settings;

    /* reads the game's level data; FALSE with a reason */
    bool from_game(const uint8_t *data, int size, std::string *error);
    /* the game's level data */
    std::vector<uint8_t> to_game() const;

    /* things to fix before the level can be played (errors) or that are
     * probably not meant (warnings) */
    struct Problem {
        bool error;
        std::string text;
        int cell;                       /* where, -1 for the level */
    };
    std::vector<Problem> check() const;

    int property_count() const;
    int count_things(int id) const;

    /* sets a thing with its template (rotation etc. kept if `keep`) */
    static void set_thing(Thing *t, int id);
    /* the template of a special block */
    static void init_property(Block *b, int kind, int cell);

    /* a moving block's or laser's path, updated from its cell, direction
     * and length; the laser's beam stops at the first block */
    void update_paths();

private:
    std::map<int, Block> m_blocks;
    uint16_t m_info[128];               /* property 666 as read (or the template) */
    bool m_had_flags = false;
    uint16_t m_flags[128];
};

#endif /* EDITOR_LEVEL_MODEL_H */
