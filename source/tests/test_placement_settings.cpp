/* Link with editor/level_model.cpp, level/level_file.c and platform/files.cpp. */
#include "editor/placement_settings.h"
#include <cassert>
#include <cstdio>

int main()
{
    PlacementSettings settings;
    Level level;
    int a = Level::cell(10, 10, 10), b = Level::cell(20, 20, 20);
    level.add_block(a, BLOCK_LASER);
    Block &source = *level.block(a);
    source.prop[1] = SIDE_LEFT;
    source.length = 7;
    source.prop[22] = 3;
    source.prop[3] = 0;
    source.prop[125] = 123;
    source.prop[20] = 456;
    source.laser_power.cell = a;
    settings.remember(source);
    level.add_block(b, BLOCK_LASER);
    Block &placed = *level.block(b);
    uint16_t old_position = placed.prop[125], old_index = placed.prop[20];
    settings.apply(&placed);
    level.update_paths();
    assert(placed.prop[1] == SIDE_LEFT && placed.length == 7);
    assert(placed.prop[22] == 3 && placed.prop[3] == 0);
    assert(!placed.laser_power.valid());
    assert(placed.prop[125] == old_position && placed.prop[20] == old_index);
    assert(placed.prop[4] != source.prop[4]);
    assert(settings.block(BLOCK_MOVING).prop[1] == SIDE_FRONT);
    settings.block(BLOCK_MOVING).prop[17] = 4;
    settings.block(BLOCK_MOVING).length = 6;
    assert(settings.block(BLOCK_LASER).prop[1] == SIDE_LEFT);
    level.add_block(a, BLOCK_MOVING);
    settings.apply(level.block(a));
    assert(level.block(a)->length == 6 && level.block(a)->prop[17] == 4);
    settings.block(BLOCK_FLASHING).prop[2] = 3;
    level.add_block(a, BLOCK_FLASHING);
    settings.apply(level.block(a));
    assert(level.block(a)->prop[2] == 3);

    Thing transporter;
    Level::set_thing(&transporter, THING_TRANSPORTER);
    transporter.w[1] = 3;
    transporter.w[2] = 2;
    transporter.w[3] = 2;
    transporter.w[4] = 19;
    transporter.w[8] = 1234;
    transporter.power.cell = a;
    transporter.target.cell = b;
    settings.remember(transporter);
    Thing copy = settings.make_thing(THING_TRANSPORTER, 1);
    assert(copy.w[1] == 3 && copy.w[2] == 2 && copy.w[3] == 2);
    assert(!copy.power.valid() && !copy.target.valid());
    assert(copy.w[4] == 0xFFFF && copy.w[8] != 1234);
    assert(copy.w[5] == 0xFFFF && copy.w[6] == 0xFFFF);
    assert(settings.thing(THING_BUTTON).w[2] == 0);
    settings.thing(THING_BUTTON).w[2] = 1;
    assert(settings.thing(THING_TRANSPORTER).w[2] == 2);
    assert(settings.make_thing(THING_START, 4).w[1] == 4);
    settings.thing(THING_COIN).w[2] = 2;
    settings.thing(THING_GEM).w[2] = 1;
    assert(settings.make_thing(THING_COIN, 1).w[2] == 2);
    assert(settings.make_thing(THING_GEM, 1).w[2] == 1);
    settings.thing(THING_MOVING_SPIKES).w[2] = 3;
    assert(settings.make_thing(THING_MOVING_SPIKES, 1).w[2] == 3);
    level.make_new();
    assert(settings.block(BLOCK_LASER).length == 7);
    assert(settings.thing(THING_TRANSPORTER).w[2] == 2);
    level.add_block(a, BLOCK_LASER);
    Block before = *level.block(a);
    settings.remember_edits(before, before);
    assert(settings.block(BLOCK_LASER).prop[1] == SIDE_LEFT);
    level.block(a)->prop[1] = SIDE_TOP;
    settings.remember_edits(before, *level.block(a));
    level.add_block(b, BLOCK_LASER);
    settings.apply(level.block(b));
    assert(level.block(b)->prop[1] == SIDE_TOP);
    Thing unedited;
    Level::set_thing(&unedited, THING_TRANSPORTER);
    settings.remember_edits(unedited, unedited);
    assert(settings.thing(THING_TRANSPORTER).w[2] == 2);
    Thing edited = unedited;
    edited.w[2] = 3;
    settings.remember_edits(unedited, edited);
    assert(settings.make_thing(THING_TRANSPORTER, 1).w[2] == 3);
    std::puts("PASS: per-type placement settings, safe copying, camera facing and level-independent memory");
}
