#include "Tools.hpp"

namespace mcp {

    static char const* FILTER_PROPS = R"JSON(
        "uids":     {"type":"array","items":{"type":"string"},"description":"Object handles returned by list_objects / place_*."},
        "selected": {"type":"boolean","description":"Only the currently selected objects."},
        "id":       {"type":"integer","description":"Only objects with this object ID."},
        "group":    {"type":"integer","description":"Only objects in this group."},
        "rect":     {"type":"object","description":"Only objects whose position is inside this rectangle (level units).",
                     "properties":{"x1":{"type":"number"},"y1":{"type":"number"},"x2":{"type":"number"},"y2":{"type":"number"}}}
    )JSON";

    static char const* OBJECT_PROPS = R"JSON(
        "id":          {"type":"integer","description":"Object ID (e.g. 1 = basic block, 8 = spike)."},
        "x":           {"type":"number","description":"X position in level units (30 = one block)."},
        "y":           {"type":"number","description":"Y position in level units (a block resting on the ground has y=15)."},
        "rotation":    {"type":"number","description":"Rotation in degrees."},
        "scale":       {"type":"number","description":"Uniform scale (1 = normal)."},
        "scaleX":      {"type":"number"},
        "scaleY":      {"type":"number"},
        "flipX":       {"type":"boolean"},
        "flipY":       {"type":"boolean"},
        "groups":      {"type":"array","items":{"type":"integer"},"description":"Group IDs."},
        "zOrder":      {"type":"integer"},
        "zLayer":      {"type":"integer","description":"Raw GD z-layer value (key 24)."},
        "editorLayer": {"type":"integer","description":"Editor layer number (key 20)."},
        "color":       {"type":"integer","description":"Main color channel ID (key 21)."},
        "detailColor": {"type":"integer","description":"Detail color channel ID (key 22)."},
        "extra":       {"type":"object","description":"Raw level-string keys -> values for anything else, e.g. trigger settings {\"51\":5,\"28\":30,\"10\":0.5}."}
    )JSON";

    static std::string schema(std::string const& props, std::string const& required = "") {
        std::string s = R"({"type":"object","properties":{)" + props + "}";
        if (!required.empty()) s += R"(,"required":[)" + required + "]";
        s += "}";
        return s;
    }

    static std::string tool(std::string const& name, std::string const& desc, std::string const& inputSchema) {
        matjson::Value d(desc);
        return R"({"name":")" + name + R"(","description":)" + d.dump(0) + R"(,"inputSchema":)" + inputSchema + "}";
    }

    matjson::Value toolList() {
        std::string filter = FILTER_PROPS;
        std::string objp = OBJECT_PROPS;
        std::vector<std::string> t;

        // ---- full game control ----

        t.push_back(tool("game_status",
            "Info about what's currently on screen: scene class name, whether the level editor or a PlayLayer is open, whether paused. Call this first to orient yourself.",
            schema("")));

        t.push_back(tool("list_levels",
            "List the user's local levels (both for open_level/editing and play_level/playing).",
            schema("")));

        t.push_back(tool("open_level",
            "Open a local level in the EDITOR by name (exact or partial) or by index from list_levels. Takes ~1s; poll editor_status afterwards.",
            schema(R"("name":{"type":"string"},"index":{"type":"integer"})")));

        t.push_back(tool("play_level",
            "Start actually PLAYING a local level (normal gameplay, not the editor) by name or index from list_levels. Use tap_screen or click_tag/click_id afterwards to interact with the pause/result menus; the game itself (jumping) is driven by the player's own taps, not this tool.",
            schema(R"("name":{"type":"string"},"index":{"type":"integer"})")));

        t.push_back(tool("return_to_menu",
            "Go back to the main menu scene from wherever you are.",
            schema("")));

        t.push_back(tool("list_buttons",
            "List every clickable menu item (button) in the CURRENT scene: tag, Geode node id (if any), screen position, enabled/visible, class name. Use this to discover what you can click_tag / click_id before clicking blindly.",
            schema(R"("limit":{"type":"integer","description":"Max buttons to return, default 150."})")));

        t.push_back(tool("click_tag",
            "Click (activate) a menu item in the current scene by its cocos2d tag, as returned by list_buttons.",
            schema(R"("tag":{"type":"integer"})", R"("tag")")));

        t.push_back(tool("click_id",
            "Click (activate) a menu item in the current scene by its Geode node ID (a string), as returned by list_buttons.",
            schema(R"("id":{"type":"string"})", R"("id")")));

        t.push_back(tool("tap_screen",
            "Simulate a tap at a screen coordinate (origin bottom-left, same units as list_buttons positions). If a visible enabled button is at that point it is activated. Use list_buttons first when you need precision.",
            schema(R"("x":{"type":"number"},"y":{"type":"number"})", R"("x","y")")));

        t.push_back(tool("get_icons",
            "Get the player's currently equipped icon ID for every form (cube, ship, ball, ufo, wave, robot, spider, swing) plus color1, color2, glow and trail.",
            schema("")));

        t.push_back(tool("set_icon",
            "Change the player's equipped icon for one form. form: cube|ship|ball|ufo|wave|robot|spider|swing. id is the icon's number as shown in the in-game icon kit (not validated against ownership - unlocked icons work immediately, locked ones may need to be unlocked first in-game).",
            schema(R"("form":{"type":"string","enum":["cube","ship","ball","ufo","wave","robot","spider","swing"]},"id":{"type":"integer"})", R"("form","id")")));

        t.push_back(tool("set_colors",
            "Change the player's color1 / color2 (channel IDs, 0-based palette index), glow (bool) and/or trail (streak id). Give at least one.",
            schema(R"("color1":{"type":"integer"},"color2":{"type":"integer"},"glow":{"type":"boolean"},"trail":{"type":"integer"})")));

        // ---- level editor ----

        t.push_back(tool("editor_status",
            "Info about the open editor: level name, object count, selection count, current editor layer and camera. Call this before editor tools.",
            schema("")));

        t.push_back(tool("list_objects",
            "List objects in the level with position, rotation, scale, groups. Filter by uids/selected/id/group/rect. Supports paging with limit (default 200, max 2000) and offset.",
            schema(filter + R"(,"limit":{"type":"integer"},"offset":{"type":"integer"})")));

        t.push_back(tool("place_object",
            "Place one object. Supports every level-string property via the typed fields plus 'extra' raw keys (triggers, colors, etc.). Undoable with history.",
            schema(objp + R"(,"select":{"type":"boolean","description":"Select the new object afterwards."})", R"("id","x","y")")));

        t.push_back(tool("place_objects",
            "Place many objects at once (max 5000). Each entry takes the same fields as place_object. Prefer this (or fill_rect/place_line) over many single calls.",
            schema(R"("objects":{"type":"array","items":{"type":"object","properties":{)" + objp + R"(},"required":["id","x","y"]}},"select":{"type":"boolean"})", R"("objects")")));

        t.push_back(tool("fill_rect",
            "Fill a rectangle with copies of one object on a grid (inclusive corners). Great for walls, floors, blocks of deco. Max 5000 objects. Other object fields (rotation, groups, color, extra...) apply to every copy.",
            schema(objp + R"(,"x1":{"type":"number"},"y1":{"type":"number"},"x2":{"type":"number"},"y2":{"type":"number"},"stepX":{"type":"number","description":"Default 30."},"stepY":{"type":"number","description":"Default 30."},"select":{"type":"boolean"})", R"("id","x1","y1","x2","y2")")));

        t.push_back(tool("place_line",
            "Place copies of one object along a line from (x1,y1) to (x2,y2) every 'step' units (default 30). Max 5000 objects. Other fields apply to every copy.",
            schema(objp + R"(,"x1":{"type":"number"},"y1":{"type":"number"},"x2":{"type":"number"},"y2":{"type":"number"},"step":{"type":"number"},"select":{"type":"boolean"})", R"("id","x1","y1","x2","y2")")));

        t.push_back(tool("delete_objects",
            "Delete objects matching the filter. At least one filter is required (use clear_level to wipe everything). Undoable with history.",
            schema(filter)));

        t.push_back(tool("edit_objects",
            "Modify objects matching the filter (at least one filter required). Ops: moveBy{x,y}, setPosition{x,y}, rotateBy, setRotation, setScale/setScaleX/setScaleY, flipX, flipY, addGroups[], removeGroups[]. NOT undoable - call backup_level first for big edits.",
            schema(filter + R"(,
                "moveBy":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}}},
                "setPosition":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}}},
                "rotateBy":{"type":"number"},"setRotation":{"type":"number"},
                "setScale":{"type":"number"},"setScaleX":{"type":"number"},"setScaleY":{"type":"number"},
                "flipX":{"type":"boolean"},"flipY":{"type":"boolean"},
                "addGroups":{"type":"array","items":{"type":"integer"}},
                "removeGroups":{"type":"array","items":{"type":"integer"}})")));

        t.push_back(tool("select_objects",
            "Select objects in the editor (matching the filter), select all with all=true, or clear the selection with clear=true.",
            schema(filter + R"(,"all":{"type":"boolean"},"clear":{"type":"boolean"})")));

        t.push_back(tool("import_objects",
            "Add objects from a raw GD level/object string ('1,1,2,15,3,15;1,8,2,45,3,15;'). With replace=true the whole level is wiped first (auto backup). Use this for copy/paste of big pieces, triggers or anything the typed tools cannot express.",
            schema(R"("levelString":{"type":"string"},"replace":{"type":"boolean"},"select":{"type":"boolean"})", R"("levelString")")));

        t.push_back(tool("get_level_string",
            "Return the raw (uncompressed) level string of the open editor, paged by characters (limit default 20000).",
            schema(R"("offset":{"type":"integer"},"limit":{"type":"integer"})")));

        t.push_back(tool("history",
            "Undo or redo editor actions.",
            schema(R"("action":{"type":"string","enum":["undo","redo"]},"times":{"type":"integer"})", R"("action")")));

        t.push_back(tool("playtest",
            "Start or stop playtesting the level FROM WITHIN THE EDITOR.",
            schema(R"("action":{"type":"string","enum":["start","stop"]})", R"("action")")));

        t.push_back(tool("save_level",
            "Save the level (same as Save in the editor pause menu).",
            schema("")));

        t.push_back(tool("scroll_to",
            "Move the editor camera so (x,y) is at the center of the screen; optional zoom (0.1 - 4).",
            schema(R"("x":{"type":"number"},"y":{"type":"number"},"zoom":{"type":"number"})", R"("x","y")")));

        t.push_back(tool("screenshot",
            "Take a screenshot of the current screen (whatever is on screen right now - menu, gameplay, or editor) and return it as an image, so you can SEE it and check your work.",
            schema("")));

        t.push_back(tool("backup_level",
            "Save the current object data to a backup file on the device. Returns the backup name for restore_backup.",
            schema("")));

        t.push_back(tool("restore_backup",
            "Replace the level's objects with a backup made by backup_level (auto backup of the current state first). Without 'name' lists available backups.",
            schema(R"("name":{"type":"string"})")));

        t.push_back(tool("clear_level",
            "Delete ALL objects in the level (an automatic backup is made first). Requires confirm=true.",
            schema(R"("confirm":{"type":"boolean"})", R"("confirm")")));

        std::string all = R"({"tools":[)";
        for (size_t i = 0; i < t.size(); ++i) {
            if (i) all += ",";
            all += t[i];
        }
        all += "]}";

        auto parsed = matjson::parse(all);
        if (parsed.isErr()) {
            geode::log::error("MCP tool schema failed to parse");
            return matjson::makeObject({{"tools", matjson::Value::array()}});
        }
        return parsed.unwrap();
    }

    std::string serverInstructions() {
        return
            "You have full remote control of Geometry Dash on the user's device: menus, icons, playing levels, and the level editor.\n"
            "Always call game_status first to see what's on screen. Use list_buttons to discover clickable things in the current scene, "
            "then click_tag / click_id to press them, or tap_screen for a raw coordinate tap.\n"
            "To change how the player looks: get_icons / set_icon (cube/ship/ball/ufo/wave/robot/spider/swing) / set_colors.\n"
            "To play: list_levels then play_level (real gameplay) or open_level (level editor). Gameplay jumping itself is driven by the "
            "player's own screen taps, not by this server - use tap_screen for taps timed to the level if asked to 'play' a level live.\n"
            "For building/editing levels, open_level then use editor_status first; coordinates are level units: 30 units = 1 block, "
            "a block on the ground sits at y=15. Use place_objects / fill_rect / place_line for bulk work, screenshot to verify visually, "
            "backup_level before big edits.\n"
            "Common object IDs (2.2): 1 block, 8 spike, 35 yellow pad, 36 yellow orb, 67 blue pad, 84 blue orb, 140 pink pad, 141 pink orb, "
            "12 cube portal, 13 ship portal, 47 ball portal, 111 UFO portal, 660 wave portal, 745 robot portal, 1331 spider portal, "
            "10/11 gravity portals, 200-203 speed portals, 899 color trigger, 901 move trigger, 1006 pulse, 1007 alpha, 1049 toggle, 1268 spawn.\n"
            "Raw level-string keys usable in 'extra': 51 target group, 10 duration, 28 move X, 29 move Y, 30 easing, 11 touch triggered, 62 spawn triggered, "
            "7/8/9 color R/G/B, 23 target color channel, 35 opacity, 56 activate group (toggle), 21/22 main/detail color channel, 24 z layer, 25 z order.";
    }

}
