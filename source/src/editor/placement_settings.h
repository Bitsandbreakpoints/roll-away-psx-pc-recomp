/* Session-local brush settings. Copy only user-facing properties, never
 * links, positions, paths, collectible indices or animation state. */
#ifndef EDITOR_PLACEMENT_SETTINGS_H
#define EDITOR_PLACEMENT_SETTINGS_H

#include "editor/level_model.h"

class PlacementSettings {
    std::map<int, Block> m_blocks;
    std::map<int, Thing> m_things;

    static void copy_block(const Block &from, Block *to)
    {
        if (from.kind == BLOCK_MOVING || from.kind == BLOCK_LASER) {
            to->prop[1] = from.prop[1];
            to->length = from.length;
            if (from.kind == BLOCK_MOVING) {
                to->prop[17] = from.prop[17];
            } else {
                to->prop[3] = from.prop[3];
                to->prop[22] = from.prop[22];
            }
        } else if (from.kind == BLOCK_FLASHING) {
            to->prop[2] = from.prop[2];
        }
    }

public:
    Block &block(int kind)
    {
        auto it = m_blocks.find(kind);
        if (it == m_blocks.end()) {
            Level defaults;
            defaults.add_block(0, kind);
            it = m_blocks.emplace(kind, *defaults.block(0)).first;
        }
        return it->second;
    }

    Thing &thing(int id)
    {
        auto it = m_things.find(id);
        if (it == m_things.end()) {
            Thing defaults;
            Level::set_thing(&defaults, id);
            const ThingInfo *info = thing_info(id);
            if (info && (info->flags & THING_ROTATES)) {
                defaults.w[1] = 0;      /* face the camera until explicitly chosen */
            }
            it = m_things.emplace(id, defaults).first;
        }
        return it->second;
    }

    void remember(const Block &from) { copy_block(from, &block(from.kind)); }

    void remember_edits(const Block &before, const Block &after)
    {
        bool changed = false;
        if (after.kind == BLOCK_MOVING || after.kind == BLOCK_LASER) {
            changed = before.prop[1] != after.prop[1] || before.length != after.length;
            if (after.kind == BLOCK_MOVING) changed |= before.prop[17] != after.prop[17];
            else changed |= before.prop[3] != after.prop[3] || before.prop[22] != after.prop[22];
        } else if (after.kind == BLOCK_FLASHING) {
            changed = before.prop[2] != after.prop[2];
        }
        if (changed) remember(after);
    }

    void remember(const Thing &from)
    {
        const ThingInfo *info = thing_info(from.id());
        if (!info) return;
        Thing &to = thing(from.id());
        if (info->flags & THING_ROTATES) to.w[1] = from.w[1];
        if (info->flags & (THING_KINDS | THING_COLOURED | THING_TIMED)) to.w[2] = from.w[2];
        if (info->flags & THING_TOGGLES) to.w[3] = from.w[3];
    }

    void remember_edits(const Thing &before, const Thing &after)
    {
        if (before.w[1] != after.w[1] || before.w[2] != after.w[2] || before.w[3] != after.w[3]) {
            remember(after);
        }
    }

    void apply(Block *to) { copy_block(block(to->kind), to); }

    Thing make_thing(int id, int camera_rotation)
    {
        Thing result;
        Level::set_thing(&result, id);
        const ThingInfo *info = thing_info(id);
        if (!info) return result;
        const Thing &settings = thing(id);
        if (info->flags & THING_ROTATES) result.w[1] = settings.w[1] ? settings.w[1] : camera_rotation;
        if (info->flags & (THING_KINDS | THING_COLOURED | THING_TIMED)) result.w[2] = settings.w[2];
        if (info->flags & THING_TOGGLES) result.w[3] = settings.w[3];
        return result;
    }
};

#endif
