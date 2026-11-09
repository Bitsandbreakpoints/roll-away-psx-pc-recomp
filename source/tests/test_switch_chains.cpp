/* Native regression checks for Level::check() switch-chain validation.
 * Link with editor/level_model.cpp, level/level_file.c and platform/files.cpp.
 */
#include "editor/level_model.h"
#include <cassert>
#include <cstdio>

static int cycles(const Level &level)
{
    int count = 0;
    for (const Level::Problem &p : level.check()) {
        if (p.text.find("Circular switch chain") != std::string::npos) {
            assert(p.error && level.block(p.cell));
            count++;
        }
    }
    return count;
}

static Link link(int cell, int side = SIDE_TOP)
{
    Link result;
    result.cell = cell;
    result.side = side;
    return result;
}

int main()
{
    Level level;
    int a = Level::cell(10, 10, 10), b = Level::cell(12, 10, 10);
    int laser = Level::cell(14, 10, 10), transporter = Level::cell(16, 10, 10);
    level.add_block(a, BLOCK_NORMAL);
    level.add_block(b, BLOCK_NORMAL);
    level.add_block(laser, BLOCK_LASER);
    level.add_block(transporter, BLOCK_NORMAL);
    Thing &first = level.block(a)->sides[SIDE_TOP];
    Thing &second = level.block(b)->sides[SIDE_TOP];
    Thing &teleport = level.block(transporter)->sides[SIDE_TOP];
    Level::set_thing(&first, THING_BUTTON);
    Level::set_thing(&second, THING_BUTTON);
    Level::set_thing(&teleport, THING_TRANSPORTER);
    assert(cycles(level) == 0);

    /* Valid self-starts and teleport loops are not switching loops. */
    first.target = link(a);
    second.target = link(laser, LINK_SELF);
    teleport.target = link(transporter);
    assert(cycles(level) == 0);

    /* Shared chain: laser -> first button -> second button -> transporter. */
    level.block(laser)->laser_power = link(a);
    first.power = link(b);
    second.power = link(transporter);
    assert(cycles(level) == 0);
    teleport.power = link(laser, LINK_SELF);
    assert(cycles(level) == 1);
    teleport.power = Link();
    assert(cycles(level) == 0);

    /* Two incoming branches report a shared cycle only once. */
    second.power = link(a);
    teleport.power = link(a);
    assert(cycles(level) == 1);
    first.power = Link();
    second.power = Link();
    teleport.power = Link();

    /* Independent self-cycles are each diagnosed, even without a start link. */
    first.target = Link();
    second.target = Link();
    first.power = link(a);
    level.block(laser)->laser_power = link(laser, LINK_SELF);
    assert(cycles(level) == 2);
    first.power = Link();
    level.block(laser)->laser_power = Link();

    /* Missing targets and out-of-range sides must terminate validation. */
    first.power = link(Level::cell(20, 10, 10));
    second.power = link(a, 99);
    assert(cycles(level) == 0);
    std::puts("PASS: circular switch chains, shared tails, valid self-starts and teleport links");
}
