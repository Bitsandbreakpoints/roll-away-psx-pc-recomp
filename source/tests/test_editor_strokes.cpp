/* Include both editor units to exercise their private input/history actions
 * without opening a window. Link with the editor's other objects, excluding
 * editor_main.o, editor.o and editor_view.o. */
#include "../src/editor/editor.cpp"
#include "../src/editor/editor_view.cpp"
#include <cassert>

extern "C" int disc_check_executables(char *, int) { return 0; }

static void begin_erase(int cell)
{
    g_ed.tool = TOOL_BUILD;
    s_stroke_tool = TOOL_BUILD;
    s_stroke_ctrl = true;
    s_dragging = s_erase_stroke = true;
    s_drag_changed = false;
    s_hover_cell = cell;
    erase_stroke_target();
}

int main()
{
    int a = Level::cell(10, 10, 10), b = Level::cell(12, 10, 10);
    g_ed.level.add_block(a, BLOCK_NORMAL);
    g_ed.level.add_block(b, BLOCK_NORMAL);
    begin_erase(a);
    s_hover_cell = b;
    erase_stroke_target();
    assert(g_ed.undo.size() == 1 && !g_ed.level.block(a) && !g_ed.level.block(b));

    undo();
    assert(!s_dragging && !s_erase_stroke && !s_drag_changed);
    assert(g_ed.level.block(a) && g_ed.level.block(b));
    assert(g_ed.undo.empty() && g_ed.redo.size() == 1);

    /* A new press must take a new snapshot and discard the old redo. */
    begin_erase(a);
    assert(g_ed.undo.size() == 1 && g_ed.redo.empty());
    undo();
    assert(g_ed.level.block(a));

    /* Redo must also cancel an ongoing (not-yet-modified) stroke. */
    s_dragging = s_erase_stroke = true;
    s_drag_changed = false;
    redo();
    assert(!s_dragging && !s_erase_stroke && !s_drag_changed);
    assert(!g_ed.level.block(a) && g_ed.level.block(b));
    assert(g_ed.undo.size() == 1 && g_ed.redo.empty());
    undo();
    assert(g_ed.level.block(a) && g_ed.level.block(b));
    std::puts("PASS: Undo/Redo cancel erase strokes and fresh presses keep independent history");
}
