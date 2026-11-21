#include "Tools.hpp"

#include <Geode/utils/base64.hpp>
#include <Geode/utils/cocos.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

namespace mcp {
namespace {

    constexpr size_t MAX_OBJECTS = 5000;
    constexpr size_t MAX_LISTED_UIDS = 200;
    constexpr size_t MAX_LISTED_BUTTONS = 300;

    struct Fail {
        std::string msg;
    };

    struct Out {
        matjson::Value data;
        std::string imageB64;
    };

    // ---------------------------------------------------------------- json helpers

    bool has(matjson::Value& o, char const* k) {
        return o.isObject() && o.contains(k);
    }

    double num(matjson::Value& o, char const* k, double def) {
        if (has(o, k)) {
            auto r = o[k].asDouble();
            if (r.isOk()) return r.unwrap();
        }
        return def;
    }

    bool flag(matjson::Value& o, char const* k, bool def) {
        if (has(o, k)) {
            auto r = o[k].asBool();
            if (r.isOk()) return r.unwrap();
        }
        return def;
    }

    std::string str(matjson::Value& o, char const* k) {
        if (has(o, k)) {
            auto r = o[k].asString();
            if (r.isOk()) return r.unwrap();
        }
        return "";
    }

    std::vector<matjson::Value> arr(matjson::Value& o, char const* k) {
        std::vector<matjson::Value> out;
        if (has(o, k)) {
            auto r = o[k].asArray();
            if (r.isOk()) {
                for (auto const& e : r.unwrap()) out.push_back(e);
            }
        }
        return out;
    }

    std::vector<int> ints(matjson::Value& o, char const* k) {
        std::vector<int> out;
        for (auto& v : arr(o, k)) {
            auto r = v.asDouble();
            if (r.isOk()) out.push_back(static_cast<int>(r.unwrap()));
        }
        return out;
    }

    matjson::Value makeArray() {
        return matjson::Value::array();
    }

    std::string lowerStr(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    // ---------------------------------------------------------------- editor helpers

    LevelEditorLayer* editorOrFail() {
        auto e = LevelEditorLayer::get();
        if (!e || !e->m_editorUI) {
            throw Fail{"The level editor is not open. Use list_levels + open_level first."};
        }
        return e;
    }

    std::string handleOf(GameObject* o) {
        return fmt::format("{:x}", reinterpret_cast<uintptr_t>(o));
    }

    std::vector<GameObject*> allObjects(LevelEditorLayer* e) {
        std::vector<GameObject*> v;
        if (e->m_objects) {
            for (auto o : CCArrayExt<GameObject*>(e->m_objects)) v.push_back(o);
        }
        return v;
    }

    std::vector<GameObject*> selectedObjects(LevelEditorLayer* e) {
        std::vector<GameObject*> v;
        auto sel = e->m_editorUI->getSelectedObjects();
        if (sel) {
            for (auto o : CCArrayExt<GameObject*>(sel)) v.push_back(o);
        }
        return v;
    }

    bool inGroup(GameObject* o, int g) {
        if (!o->m_groups) return false;
        for (auto id : *o->m_groups) {
            if (id == g) return true;
        }
        return false;
    }

    matjson::Value objectJson(GameObject* o) {
        auto groups = makeArray();
        if (o->m_groups) {
            for (auto id : *o->m_groups) {
                if (id > 0) groups.push(static_cast<int>(id));
            }
        }
        return matjson::makeObject({
            {"uid", handleOf(o)},
            {"id", static_cast<int>(o->m_objectID)},
            {"x", static_cast<double>(o->getPositionX())},
            {"y", static_cast<double>(o->getPositionY())},
            {"rotation", static_cast<double>(o->getRotation())},
            {"scaleX", static_cast<double>(o->m_scaleX)},
            {"scaleY", static_cast<double>(o->m_scaleY)},
            {"flipX", static_cast<bool>(o->m_isFlipX)},
            {"flipY", static_cast<bool>(o->m_isFlipY)},
            {"zOrder", static_cast<int>(o->m_zOrder)},
            {"editorLayer", static_cast<int>(o->m_editorLayer)},
            {"groups", groups},
        });
    }

    /// Applies uids / selected / id / group / rect filters. `hadFilter` tells if any filter was given.
    std::vector<GameObject*> filtered(LevelEditorLayer* e, matjson::Value& a, bool& hadFilter) {
        hadFilter = has(a, "uids") || has(a, "selected") || has(a, "id") || has(a, "group") || has(a, "rect");

        std::vector<GameObject*> list = flag(a, "selected", false) ? selectedObjects(e) : allObjects(e);

        if (has(a, "uids")) {
            std::unordered_set<std::string> want;
            for (auto& v : arr(a, "uids")) {
                auto s = v.asString();
                if (s.isOk()) want.insert(s.unwrap());
            }
            std::vector<GameObject*> out;
            for (auto o : list) {
                if (want.count(handleOf(o))) out.push_back(o);
            }
            list = std::move(out);
        }
        if (has(a, "id")) {
            int id = static_cast<int>(num(a, "id", 0));
            std::vector<GameObject*> out;
            for (auto o : list) {
                if (o->m_objectID == id) out.push_back(o);
            }
            list = std::move(out);
        }
        if (has(a, "group")) {
            int g = static_cast<int>(num(a, "group", 0));
            std::vector<GameObject*> out;
            for (auto o : list) {
                if (inGroup(o, g)) out.push_back(o);
            }
            list = std::move(out);
        }
        if (has(a, "rect")) {
            auto& r = a["rect"];
            double x1 = num(r, "x1", 0), y1 = num(r, "y1", 0), x2 = num(r, "x2", 0), y2 = num(r, "y2", 0);
            if (x1 > x2) std::swap(x1, x2);
            if (y1 > y2) std::swap(y1, y2);
            std::vector<GameObject*> out;
            for (auto o : list) {
                double px = o->getPositionX(), py = o->getPositionY();
                if (px >= x1 && px <= x2 && py >= y1 && py <= y2) out.push_back(o);
            }
            list = std::move(out);
        }
        return list;
    }

    // ---------------------------------------------------------------- level-string building

    std::string objectString(int id, double x, double y, matjson::Value& p) {
        if (id < 1 || id > 5000) throw Fail{fmt::format("Invalid object id {} (allowed 1-5000).", id)};
        if (!std::isfinite(x) || !std::isfinite(y)) throw Fail{"x and y must be finite numbers."};

        std::string s = fmt::format("1,{},2,{},3,{}", id, x, y);
        if (has(p, "rotation")) s += fmt::format(",6,{}", num(p, "rotation", 0));
        if (flag(p, "flipX", false)) s += ",4,1";
        if (flag(p, "flipY", false)) s += ",5,1";
        if (has(p, "scale")) s += fmt::format(",32,{}", num(p, "scale", 1));
        if (has(p, "scaleX")) s += fmt::format(",128,{}", num(p, "scaleX", 1));
        if (has(p, "scaleY")) s += fmt::format(",129,{}", num(p, "scaleY", 1));
        if (has(p, "editorLayer")) s += fmt::format(",20,{}", static_cast<int>(num(p, "editorLayer", 0)));
        if (has(p, "color")) s += fmt::format(",21,{}", static_cast<int>(num(p, "color", 0)));
        if (has(p, "detailColor")) s += fmt::format(",22,{}", static_cast<int>(num(p, "detailColor", 0)));
        if (has(p, "zLayer")) s += fmt::format(",24,{}", static_cast<int>(num(p, "zLayer", 0)));
        if (has(p, "zOrder")) s += fmt::format(",25,{}", static_cast<int>(num(p, "zOrder", 0)));

        auto groups = ints(p, "groups");
        if (!groups.empty()) {
            std::string g;
            for (size_t i = 0; i < groups.size(); ++i) {
                if (i) g += ".";
                g += std::to_string(groups[i]);
            }
            s += ",57," + g;
        }

        if (has(p, "extra") && p["extra"].isObject()) {
            for (auto const& v : p["extra"]) {
                std::string k = v.getKey().value_or("");
                if (k.empty() || !std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    throw Fail{"extra keys must be numeric level-string keys, got '" + k + "'."};
                }
                std::string val;
                if (v.isBool()) {
                    val = v.asBool().unwrapOr(false) ? "1" : "0";
                }
                else if (v.isNumber()) {
                    val = fmt::format("{}", v.asDouble().unwrapOr(0));
                }
                else if (v.isString()) {
                    val = v.asString().unwrapOr("");
                    if (val.find_first_of(",;") != std::string::npos) {
                        throw Fail{"extra string values must not contain ',' or ';'."};
                    }
                }
                else {
                    throw Fail{"extra values must be numbers, booleans or strings."};
                }
                s += "," + k + "," + val;
            }
        }
        return s;
    }

    /// Validates one raw object segment ("1,1,2,15,...").
    void validateSegment(std::string const& seg, size_t index) {
        std::vector<std::string> tok;
        size_t start = 0;
        while (true) {
            auto pos = seg.find(',', start);
            tok.push_back(seg.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        if (tok.size() < 2 || tok.size() % 2 != 0) {
            throw Fail{fmt::format("Object #{} is malformed (need key,value pairs).", index)};
        }
        for (size_t i = 0; i < tok.size(); i += 2) {
            auto const& k = tok[i];
            if (k.empty() || !std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                throw Fail{fmt::format("Object #{} has a non-numeric key '{}'.", index, k)};
            }
        }
        if (tok[0] != "1") {
            throw Fail{fmt::format("Object #{} must start with key 1 (object id).", index)};
        }
        int id = std::atoi(tok[1].c_str());
        if (id < 1 || id > 5000) {
            throw Fail{fmt::format("Object #{} has invalid id {}.", index, id)};
        }
    }

    /// Strips the level header, validates and normalises a raw string. Returns object count.
    std::string cleanLevelString(std::string const& raw, size_t& count) {
        std::string out;
        count = 0;
        size_t start = 0;
        size_t index = 0;
        while (start <= raw.size()) {
            auto pos = raw.find(';', start);
            std::string seg = raw.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
            start = pos == std::string::npos ? raw.size() + 1 : pos + 1;

            // trim whitespace
            while (!seg.empty() && std::isspace(static_cast<unsigned char>(seg.back()))) seg.pop_back();
            size_t b = 0;
            while (b < seg.size() && std::isspace(static_cast<unsigned char>(seg[b]))) ++b;
            seg = seg.substr(b);

            if (seg.empty()) continue;
            if (seg.rfind("kA", 0) == 0 || seg.rfind("kS", 0) == 0) continue; // level header
            validateSegment(seg, index++);
            out += seg + ";";
            ++count;
            if (count > MAX_OBJECTS * 20) throw Fail{"Too many objects in one import."};
        }
        return out;
    }

    Out createdOut(LevelEditorLayer* e, CCArray* made, bool select) {
        auto uids = makeArray();
        size_t n = 0;
        if (made) {
            for (auto o : CCArrayExt<GameObject*>(made)) {
                if (n < MAX_LISTED_UIDS) uids.push(handleOf(o));
                ++n;
            }
            if (select && n > 0) {
                e->m_editorUI->deselectAll();
                e->m_editorUI->selectObjects(made, true);
            }
        }
        auto obj = matjson::makeObject({
            {"created", static_cast<int>(n)},
            {"uids", uids},
        });
        if (n > MAX_LISTED_UIDS) obj["uidsTruncated"] = true;
        return {obj, ""};
    }

    Out createFromString(LevelEditorLayer* e, std::string const& objects, bool select) {
        auto made = e->createObjectsFromString(objects, false, true);
        return createdOut(e, made, select);
    }

    // ---------------------------------------------------------------- backups

    std::filesystem::path backupDir() {
        auto dir = Mod::get()->getSaveDir() / "backups";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::string makeBackup(LevelEditorLayer* e) {
        auto now = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, "backup-%Y%m%d-%H%M%S", std::localtime(&now));
        std::string name = buf;
        std::string data = std::string(e->getLevelString().c_str());
        auto res = utils::file::writeString(backupDir() / (name + ".txt"), data);
        if (res.isErr()) throw Fail{"Could not write backup file."};
        return name;
    }

    // ---------------------------------------------------------------- scene / UI control helpers

    std::string sceneClassName() {
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene || scene->getChildrenCount() == 0) return scene ? "CCScene" : "none";
        // most GD scenes put the "real" layer as the first child of CCScene
        auto first = static_cast<CCNode*>(scene->getChildren()->objectAtIndex(0));
        return first ? typeid(*first).name() : "CCScene";
    }

    CCNode* runningRoot() {
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) throw Fail{"No running scene."};
        return scene;
    }

    void collectMenuItems(CCNode* node, std::vector<CCMenuItem*>& out) {
        if (!node) return;
        auto children = node->getChildren();
        if (!children) return;
        for (auto child : CCArrayExt<CCNode*>(children)) {
            if (auto item = typeinfo_cast<CCMenuItem*>(child)) {
                out.push_back(item);
            }
            collectMenuItems(child, out);
        }
    }

    matjson::Value menuItemJson(CCMenuItem* item) {
        auto world = item->getParent() ? item->getParent()->convertToWorldSpace(item->getPosition()) : item->getPosition();
        std::string id;
        if (auto geodeId = item->getID(); !geodeId.empty()) id = geodeId;
        return matjson::makeObject({
            {"tag", item->getTag()},
            {"id", id},
            {"enabled", item->isEnabled()},
            {"visible", item->isVisible()},
            {"x", static_cast<double>(world.x)},
            {"y", static_cast<double>(world.y)},
            {"class", typeid(*item).name()},
        });
    }

    CCMenuItem* findByTag(CCNode* root, int tag) {
        std::vector<CCMenuItem*> all;
        collectMenuItems(root, all);
        for (auto it : all) {
            if (it->getTag() == tag) return it;
        }
        return nullptr;
    }

    GameManager* gm() {
        return GameManager::sharedState();
    }

    int currentIconID(GameManager* g, std::string const& form) {
        auto f = lowerStr(form);
        if (f == "cube") return g->m_playerFrame;
        if (f == "ship") return g->m_playerShip;
        if (f == "ball" || f == "roll") return g->m_playerBall;
        if (f == "ufo" || f == "bird") return g->m_playerBird;
        if (f == "wave" || f == "dart") return g->m_playerDart;
        if (f == "robot") return g->m_playerRobot;
        if (f == "spider") return g->m_playerSpider;
        if (f == "swing") return g->m_playerSwing;
        return 0;
    }

    void setIconID(GameManager* g, std::string const& form, int id) {
        auto f = lowerStr(form);
        if (f == "cube") g->setPlayerFrame(id);
        else if (f == "ship") g->setPlayerShip(id);
        else if (f == "ball" || f == "roll") g->setPlayerBall(id);
        else if (f == "ufo" || f == "bird") g->setPlayerBird(id);
        else if (f == "wave" || f == "dart") g->setPlayerDart(id);
        else if (f == "robot") g->setPlayerRobot(id);
        else if (f == "spider") g->setPlayerSpider(id);
        else if (f == "swing") g->setPlayerSwing(id);
        else throw Fail{"form must be one of: cube, ship, ball, ufo, wave, robot, spider, swing."};
    }

    // ---------------------------------------------------------------- tools: editor

    Out tEditorStatus(matjson::Value&) {
        auto e = LevelEditorLayer::get();
        if (!e || !e->m_editorUI) {
            return {matjson::makeObject({
                {"inEditor", false},
                {"hint", "Open a level with open_level (see list_levels)."},
            }), ""};
        }
        auto win = CCDirector::get()->getWinSize();
        auto ol = e->m_objectLayer;
        float z = ol->getScale();
        if (z <= 0.f) z = 1.f;
        return {matjson::makeObject({
            {"inEditor", true},
            {"levelName", std::string(e->m_level->m_levelName.c_str())},
            {"objectCount", static_cast<int>(e->m_objects ? e->m_objects->count() : 0)},
            {"selectedCount", static_cast<int>(selectedObjects(e).size())},
            {"editorLayer", static_cast<int>(e->m_currentLayer)},
            {"playtesting", e->m_playbackMode != PlaybackMode::Not},
            {"zoom", static_cast<double>(z)},
            {"cameraCenter", matjson::makeObject({
                {"x", static_cast<double>((win.width / 2 - ol->getPositionX()) / z)},
                {"y", static_cast<double>((win.height / 2 - ol->getPositionY()) / z)},
            })},
        }), ""};
    }

    Out tListLevels(matjson::Value&) {
        auto list = makeArray();
        auto lm = LocalLevelManager::sharedState();
        if (lm && lm->m_localLevels) {
            int i = 0;
            for (auto lvl : CCArrayExt<GJGameLevel*>(lm->m_localLevels)) {
                list.push(matjson::makeObject({
                    {"index", i},
                    {"name", std::string(lvl->m_levelName.c_str())},
                }));
                ++i;
            }
        }
        return {matjson::makeObject({{"levels", list}}), ""};
    }

    GJGameLevel* resolveLocalLevel(matjson::Value& a) {
        auto lm = LocalLevelManager::sharedState();
        if (!lm || !lm->m_localLevels) throw Fail{"No local levels available."};

        GJGameLevel* target = nullptr;
        if (has(a, "index")) {
            int idx = static_cast<int>(num(a, "index", -1));
            if (idx < 0 || idx >= static_cast<int>(lm->m_localLevels->count())) throw Fail{"Index out of range."};
            target = static_cast<GJGameLevel*>(lm->m_localLevels->objectAtIndex(idx));
        }
        else {
            auto name = str(a, "name");
            if (name.empty()) throw Fail{"Give 'name' or 'index'."};
            auto want = lowerStr(name);
            GJGameLevel* partial = nullptr;
            for (auto lvl : CCArrayExt<GJGameLevel*>(lm->m_localLevels)) {
                auto n = lowerStr(std::string(lvl->m_levelName.c_str()));
                if (n == want) { target = lvl; break; }
                if (!partial && n.find(want) != std::string::npos) partial = lvl;
            }
            if (!target) target = partial;
        }
        if (!target) throw Fail{"Level not found. Use list_levels."};
        return target;
    }

    Out tOpenLevel(matjson::Value& a) {
        auto target = resolveLocalLevel(a);
        auto scene = LevelEditorLayer::scene(target, false);
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
        return {matjson::makeObject({
            {"opening", std::string(target->m_levelName.c_str())},
            {"hint", "Wait ~1-2 seconds then call editor_status."},
        }), ""};
    }

    Out tPlayLevel(matjson::Value& a) {
        auto target = resolveLocalLevel(a);
        auto layer = PlayLayer::create(target, false, false);
        auto scene = CCScene::create();
        scene->addChild(layer);
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
        return {matjson::makeObject({
            {"playing", std::string(target->m_levelName.c_str())},
            {"hint", "Wait ~1-2 seconds then call game_status. Use tap_screen or click_tag to jump/click in-game."},
        }), ""};
    }

    Out tReturnToMenu(matjson::Value&) {
        auto scene = MenuLayer::scene(false);
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
        return {matjson::makeObject({{"ok", true}}), ""};
    }

    Out tListObjects(matjson::Value& a) {
        auto e = editorOrFail();
        bool hadFilter = false;
        auto list = filtered(e, a, hadFilter);

        int limit = std::clamp(static_cast<int>(num(a, "limit", 200)), 1, 2000);
        int offset = std::max(0, static_cast<int>(num(a, "offset", 0)));

        auto objs = makeArray();
        int returned = 0;
        for (size_t i = static_cast<size_t>(offset); i < list.size() && returned < limit; ++i, ++returned) {
            objs.push(objectJson(list[i]));
        }
        return {matjson::makeObject({
            {"matched", static_cast<int>(list.size())},
            {"returned", returned},
            {"offset", offset},
            {"objects", objs},
        }), ""};
    }

    Out tPlaceObject(matjson::Value& a) {
        auto e = editorOrFail();
        if (!has(a, "id") || !has(a, "x") || !has(a, "y")) throw Fail{"id, x and y are required."};
        auto s = objectString(static_cast<int>(num(a, "id", 0)), num(a, "x", 0), num(a, "y", 0), a) + ";";
        return createFromString(e, s, flag(a, "select", false));
    }

    Out tPlaceObjects(matjson::Value& a) {
        auto e = editorOrFail();
        auto items = arr(a, "objects");
        if (items.empty()) throw Fail{"'objects' must be a non-empty array."};
        if (items.size() > MAX_OBJECTS) throw Fail{"Too many objects (max 5000 per call)."};
        std::string s;
        size_t i = 0;
        for (auto& o : items) {
            if (!has(o, "id") || !has(o, "x") || !has(o, "y")) {
                throw Fail{fmt::format("objects[{}] needs id, x and y.", i)};
            }
            s += objectString(static_cast<int>(num(o, "id", 0)), num(o, "x", 0), num(o, "y", 0), o) + ";";
            ++i;
        }
        return createFromString(e, s, flag(a, "select", false));
    }

    Out tFillRect(matjson::Value& a) {
        auto e = editorOrFail();
        if (!has(a, "id")) throw Fail{"id is required."};
        double x1 = num(a, "x1", 0), y1 = num(a, "y1", 0), x2 = num(a, "x2", 0), y2 = num(a, "y2", 0);
        if (x1 > x2) std::swap(x1, x2);
        if (y1 > y2) std::swap(y1, y2);
        double sx = std::max(0.5, num(a, "stepX", 30));
        double sy = std::max(0.5, num(a, "stepY", 30));
        double nx = std::floor((x2 - x1) / sx + 1e-6) + 1;
        double ny = std::floor((y2 - y1) / sy + 1e-6) + 1;
        if (nx * ny > static_cast<double>(MAX_OBJECTS)) {
            throw Fail{fmt::format("That would create {} objects (max {}). Use a smaller area or bigger step.", nx * ny, MAX_OBJECTS)};
        }
        int id = static_cast<int>(num(a, "id", 0));
        std::string s;
        for (int iy = 0; iy < static_cast<int>(ny); ++iy) {
            for (int ix = 0; ix < static_cast<int>(nx); ++ix) {
                s += objectString(id, x1 + ix * sx, y1 + iy * sy, a) + ";";
            }
        }
        return createFromString(e, s, flag(a, "select", false));
    }

    Out tPlaceLine(matjson::Value& a) {
        auto e = editorOrFail();
        if (!has(a, "id")) throw Fail{"id is required."};
        double x1 = num(a, "x1", 0), y1 = num(a, "y1", 0), x2 = num(a, "x2", 0), y2 = num(a, "y2", 0);
        double step = std::max(0.5, num(a, "step", 30));
        double len = std::hypot(x2 - x1, y2 - y1);
        double n = std::floor(len / step + 1e-6) + 1;
        if (n > static_cast<double>(MAX_OBJECTS)) {
            throw Fail{fmt::format("That would create {} objects (max {}).", n, MAX_OBJECTS)};
        }
        int id = static_cast<int>(num(a, "id", 0));
        std::string s;
        for (int i = 0; i < static_cast<int>(n); ++i) {
            double t = len > 0 ? (i * step) / len : 0;
            s += objectString(id, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, a) + ";";
        }
        return createFromString(e, s, flag(a, "select", false));
    }

    Out tDeleteObjects(matjson::Value& a) {
        auto e = editorOrFail();
        bool hadFilter = false;
        auto list = filtered(e, a, hadFilter);
        if (!hadFilter) throw Fail{"Give at least one filter (uids, selected, id, group, rect). Use clear_level to delete everything."};
        for (auto o : list) {
            if (o->m_isSelected) e->m_editorUI->deselectObject(o);
            e->removeObject(o, false);
        }
        return {matjson::makeObject({{"deleted", static_cast<int>(list.size())}}), ""};
    }

    Out tEditObjects(matjson::Value& a) {
        auto e = editorOrFail();
        bool hadFilter = false;
        auto list = filtered(e, a, hadFilter);
        if (!hadFilter) throw Fail{"Give at least one filter (uids, selected, id, group, rect)."};

        bool anyOp = has(a, "moveBy") || has(a, "setPosition") || has(a, "rotateBy") || has(a, "setRotation") ||
                     has(a, "setScale") || has(a, "setScaleX") || has(a, "setScaleY") || has(a, "flipX") ||
                     has(a, "flipY") || has(a, "addGroups") || has(a, "removeGroups");
        if (!anyOp) throw Fail{"No edit operation given."};

        auto addG = ints(a, "addGroups");
        auto remG = ints(a, "removeGroups");

        for (auto o : list) {
            if (has(a, "moveBy")) {
                auto& m = a["moveBy"];
                e->m_editorUI->moveObject(o, CCPoint{static_cast<float>(num(m, "x", 0)), static_cast<float>(num(m, "y", 0))});
            }
            if (has(a, "setPosition")) {
                auto& p = a["setPosition"];
                float dx = has(p, "x") ? static_cast<float>(num(p, "x", 0)) - o->getPositionX() : 0.f;
                float dy = has(p, "y") ? static_cast<float>(num(p, "y", 0)) - o->getPositionY() : 0.f;
                e->m_editorUI->moveObject(o, CCPoint{dx, dy});
            }
            if (has(a, "setRotation")) o->setRotation(static_cast<float>(num(a, "setRotation", 0)));
            if (has(a, "rotateBy")) o->setRotation(o->getRotation() + static_cast<float>(num(a, "rotateBy", 0)));
            if (has(a, "setScale")) {
                float s = static_cast<float>(num(a, "setScale", 1));
                o->updateCustomScaleX(s);
                o->updateCustomScaleY(s);
            }
            if (has(a, "setScaleX")) o->updateCustomScaleX(static_cast<float>(num(a, "setScaleX", 1)));
            if (has(a, "setScaleY")) o->updateCustomScaleY(static_cast<float>(num(a, "setScaleY", 1)));
            if (has(a, "flipX")) o->setFlipX(flag(a, "flipX", false));
            if (has(a, "flipY")) o->setFlipY(flag(a, "flipY", false));
            for (int g : addG) {
                if (g > 0 && !inGroup(o, g)) {
                    o->addToGroup(g);
                    e->addToGroup(o, g, false);
                }
            }
            for (int g : remG) {
                if (inGroup(o, g)) {
                    o->removeFromGroup(g);
                    e->removeFromGroup(o, g);
                }
            }
        }
        e->m_editorUI->updateButtons();
        return {matjson::makeObject({{"edited", static_cast<int>(list.size())}}), ""};
    }

    Out tSelectObjects(matjson::Value& a) {
        auto e = editorOrFail();
        auto ui = e->m_editorUI;
        if (flag(a, "clear", false)) {
            ui->deselectAll();
            return {matjson::makeObject({{"selected", 0}}), ""};
        }
        if (flag(a, "all", false)) {
            ui->selectAll();
            return {matjson::makeObject({{"selected", static_cast<int>(selectedObjects(e).size())}}), ""};
        }
        bool hadFilter = false;
        auto list = filtered(e, a, hadFilter);
        if (!hadFilter) throw Fail{"Give a filter, all=true or clear=true."};
        ui->deselectAll();
        auto sel = CCArray::create();
        for (auto o : list) sel->addObject(o);
        if (sel->count() > 0) ui->selectObjects(sel, true);
        return {matjson::makeObject({{"selected", static_cast<int>(list.size())}}), ""};
    }

    Out importString(LevelEditorLayer* e, std::string const& raw, bool replace, bool select) {
        size_t count = 0;
        auto clean = cleanLevelString(raw, count);
        if (count == 0) throw Fail{"No objects found in the string."};
        std::string backup;
        if (replace) {
            backup = makeBackup(e);
            e->m_editorUI->deselectAll();
            e->removeAllObjects();
        }
        auto out = createFromString(e, clean, select);
        if (!backup.empty()) out.data["backup"] = backup;
        return out;
    }

    Out tImportObjects(matjson::Value& a) {
        auto e = editorOrFail();
        auto s = str(a, "levelString");
        if (s.empty()) throw Fail{"levelString is required."};
        return importString(e, s, flag(a, "replace", false), flag(a, "select", false));
    }

    Out tGetLevelString(matjson::Value& a) {
        auto e = editorOrFail();
        std::string s = std::string(e->getLevelString().c_str());
        size_t offset = static_cast<size_t>(std::max(0.0, num(a, "offset", 0)));
        size_t limit = static_cast<size_t>(std::clamp(num(a, "limit", 20000), 1.0, 200000.0));
        std::string part = offset < s.size() ? s.substr(offset, limit) : "";
        return {matjson::makeObject({
            {"totalLength", static_cast<int>(s.size())},
            {"offset", static_cast<int>(offset)},
            {"returnedLength", static_cast<int>(part.size())},
            {"data", part},
        }), ""};
    }

    Out tHistory(matjson::Value& a) {
        auto e = editorOrFail();
        auto action = str(a, "action");
        bool redo = action == "redo";
        if (!redo && action != "undo") throw Fail{"action must be 'undo' or 'redo'."};
        int times = std::clamp(static_cast<int>(num(a, "times", 1)), 1, 100);
        int done = 0;
        for (; done < times; ++done) {
            if (!e->hasAction(redo)) break;
            if (redo) e->redoLastAction();
            else e->undoLastAction();
        }
        return {matjson::makeObject({{"done", done}}), ""};
    }

    Out tPlaytest(matjson::Value& a) {
        auto e = editorOrFail();
        auto action = str(a, "action");
        bool playing = e->m_playbackMode != PlaybackMode::Not;
        if (action == "start") {
            if (!playing) e->m_editorUI->onPlaytest(nullptr);
        }
        else if (action == "stop") {
            if (playing) e->onStopPlaytest();
        }
        else {
            throw Fail{"action must be 'start' or 'stop'."};
        }
        return {matjson::makeObject({{"requested", action}}), ""};
    }

    Out tSaveLevel(matjson::Value&) {
        auto e = editorOrFail();
        EditorPauseLayer::create(e)->saveLevel();
        return {matjson::makeObject({{"saved", true}}), ""};
    }

    Out tScrollTo(matjson::Value& a) {
        auto e = editorOrFail();
        if (!has(a, "x") || !has(a, "y")) throw Fail{"x and y are required."};
        if (has(a, "zoom")) {
            e->m_editorUI->updateZoom(static_cast<float>(std::clamp(num(a, "zoom", 1), 0.1, 4.0)));
        }
        auto win = CCDirector::get()->getWinSize();
        auto ol = e->m_objectLayer;
        float z = ol->getScale();
        if (z <= 0.f) z = 1.f;
        ol->setPosition(CCPoint{
            win.width / 2 - static_cast<float>(num(a, "x", 0)) * z,
            win.height / 2 - static_cast<float>(num(a, "y", 0)) * z,
        });
        return {matjson::makeObject({{"ok", true}}), ""};
    }

    Out tScreenshot(matjson::Value&) {
        auto dir = CCDirector::get();
        auto scene = dir->getRunningScene();
        if (!scene) throw Fail{"No running scene."};
        auto win = dir->getWinSize();

        auto rt = CCRenderTexture::create(static_cast<int>(win.width), static_cast<int>(win.height));
        if (!rt) throw Fail{"Could not create render texture."};
        rt->begin();
        scene->visit();
        rt->end();

        auto img = rt->newCCImage(true);
        if (!img) throw Fail{"Could not capture the screen."};
        auto path = Mod::get()->getSaveDir() / "screenshot.png";
        bool ok = img->saveToFile(path.string().c_str(), true);
        img->release();
        if (!ok) throw Fail{"Could not encode the screenshot."};

        auto bytes = utils::file::readBinary(path);
        if (bytes.isErr()) throw Fail{"Could not read the screenshot file."};
        auto b64 = utils::base64::encode(bytes.unwrap(), utils::base64::Base64Variant::Normal);
        return {matjson::makeObject({{"captured", true}}), b64};
    }

    Out tBackupLevel(matjson::Value&) {
        auto e = editorOrFail();
        auto name = makeBackup(e);
        return {matjson::makeObject({{"backup", name}}), ""};
    }

    Out tRestoreBackup(matjson::Value& a) {
        auto e = editorOrFail();
        auto name = str(a, "name");
        if (name.empty()) {
            std::vector<std::string> names;
            std::error_code ec;
            for (auto& ent : std::filesystem::directory_iterator(backupDir(), ec)) {
                if (ent.path().extension() == ".txt") names.push_back(ent.path().stem().string());
            }
            std::sort(names.rbegin(), names.rend());
            auto list = makeArray();
            for (auto& n : names) list.push(n);
            return {matjson::makeObject({{"backups", list}}), ""};
        }
        if (name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos) {
            throw Fail{"Invalid backup name."};
        }
        auto data = utils::file::readString(backupDir() / (name + ".txt"));
        if (data.isErr()) throw Fail{"Backup not found. Call restore_backup without a name to list them."};
        return importString(e, data.unwrap(), true, false);
    }

    Out tClearLevel(matjson::Value& a) {
        auto e = editorOrFail();
        if (!flag(a, "confirm", false)) throw Fail{"Pass confirm=true to delete every object."};
        auto backup = makeBackup(e);
        int count = static_cast<int>(e->m_objects ? e->m_objects->count() : 0);
        e->m_editorUI->deselectAll();
        e->removeAllObjects();
        return {matjson::makeObject({{"deleted", count}, {"backup", backup}}), ""};
    }

    // ---------------------------------------------------------------- tools: full game control

    Out tGameStatus(matjson::Value&) {
        auto e = LevelEditorLayer::get();
        auto p = PlayLayer::get();
        return {matjson::makeObject({
            {"scene", sceneClassName()},
            {"inEditor", e != nullptr},
            {"inPlayLayer", p != nullptr},
            {"paused", p ? p->getChildByType<PauseLayer>(0) != nullptr : false},
        }), ""};
    }

    Out tListButtons(matjson::Value& a) {
        auto root = runningRoot();
        std::vector<CCMenuItem*> items;
        collectMenuItems(root, items);
        int limit = std::clamp(static_cast<int>(num(a, "limit", 150)), 1, static_cast<int>(MAX_LISTED_BUTTONS));
        auto list = makeArray();
        int n = 0;
        for (auto it : items) {
            if (n >= limit) break;
            list.push(menuItemJson(it));
            ++n;
        }
        return {matjson::makeObject({
            {"scene", sceneClassName()},
            {"matched", static_cast<int>(items.size())},
            {"returned", n},
            {"buttons", list},
        }), ""};
    }

    Out tClickTag(matjson::Value& a) {
        if (!has(a, "tag")) throw Fail{"tag is required (see list_buttons)."};
        auto root = runningRoot();
        auto item = findByTag(root, static_cast<int>(num(a, "tag", 0)));
        if (!item) throw Fail{"No menu item with that tag in the current scene. Call list_buttons first."};
        item->activate();
        return {matjson::makeObject({{"clicked", true}, {"tag", item->getTag()}}), ""};
    }

    Out tClickId(matjson::Value& a) {
        auto id = str(a, "id");
        if (id.empty()) throw Fail{"id is required (the Geode node ID from list_buttons)."};
        auto root = runningRoot();
        auto node = root->getChildByIDRecursive(id);
        auto item = typeinfo_cast<CCMenuItem*>(node);
        if (!item) throw Fail{"No clickable menu item with that ID in the current scene."};
        item->activate();
        return {matjson::makeObject({{"clicked", true}, {"id", id}}), ""};
    }

    Out tTapScreen(matjson::Value& a) {
        if (!has(a, "x") || !has(a, "y")) throw Fail{"x and y are required (screen coordinates)."};
        auto point = CCPoint{static_cast<float>(num(a, "x", 0)), static_cast<float>(num(a, "y", 0))};
        auto root = runningRoot();
        std::vector<CCMenuItem*> items;
        collectMenuItems(root, items);
        for (auto it : items) {
            if (!it->isVisible() || !it->isEnabled()) continue;
            auto box = it->boundingBox();
            auto transform = it->nodeToWorldTransform();
            auto worldBox = CCRectApplyAffineTransform(box, transform);
            if (worldBox.containsPoint(point)) {
                it->activate();
                return {matjson::makeObject({{"clicked", true}, {"hitButtonTag", it->getTag()}}), ""};
            }
        }
        return {matjson::makeObject({{"clicked", false}, {"hint", "No button at that point. Use list_buttons to find exact positions, or for in-level jumps use play_level + click_tag on the player's PlayerObject-bound controls."}}), ""};
    }

    Out tGetIcons(matjson::Value&) {
        auto g = gm();
        auto icons = matjson::makeObject({});
        for (auto f : {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"}) {
            icons[f] = currentIconID(g, f);
        }
        return {matjson::makeObject({
            {"icons", icons},
            {"color1", static_cast<int>(g->m_playerColor)},
            {"color2", static_cast<int>(g->m_playerColor2)},
            {"glow", static_cast<bool>(g->m_playerGlow)},
            {"trail", static_cast<int>(g->m_playerStreak)},
        }), ""};
    }

    Out tSetIcon(matjson::Value& a) {
        auto form = str(a, "form");
        if (form.empty()) throw Fail{"form is required (cube, ship, ball, ufo, wave, robot, spider, swing)."};
        if (!has(a, "id")) throw Fail{"id is required (icon id, see the in-game icon kit for valid ranges)."};
        int id = static_cast<int>(num(a, "id", 1));
        if (id < 1 || id > 5000) throw Fail{"id out of range."};
        auto g = gm();
        setIconID(g, form, id);
        return {matjson::makeObject({{"form", form}, {"id", id}}), ""};
    }

    Out tSetColors(matjson::Value& a) {
        auto g = gm();
        bool any = false;
        if (has(a, "color1")) { g->setPlayerColor(static_cast<int>(num(a, "color1", 0))); any = true; }
        if (has(a, "color2")) { g->setPlayerColor2(static_cast<int>(num(a, "color2", 0))); any = true; }
        if (has(a, "glow")) { g->setPlayerGlow(flag(a, "glow", false)); any = true; }
        if (has(a, "trail")) { g->setPlayerStreak(static_cast<int>(num(a, "trail", 0))); any = true; }
        if (!any) throw Fail{"Give at least one of color1, color2, glow, trail."};
        return {matjson::makeObject({{"ok", true}}), ""};
    }

    using ToolFn = Out (*)(matjson::Value&);

    std::unordered_map<std::string, ToolFn> const& registry() {
        static std::unordered_map<std::string, ToolFn> const r = {
            {"game_status", tGameStatus},
            {"editor_status", tEditorStatus},
            {"list_levels", tListLevels},
            {"open_level", tOpenLevel},
            {"play_level", tPlayLevel},
            {"return_to_menu", tReturnToMenu},
            {"list_buttons", tListButtons},
            {"click_tag", tClickTag},
            {"click_id", tClickId},
            {"tap_screen", tTapScreen},
            {"get_icons", tGetIcons},
            {"set_icon", tSetIcon},
            {"set_colors", tSetColors},
            {"list_objects", tListObjects},
            {"place_object", tPlaceObject},
            {"place_objects", tPlaceObjects},
            {"fill_rect", tFillRect},
            {"place_line", tPlaceLine},
            {"delete_objects", tDeleteObjects},
            {"edit_objects", tEditObjects},
            {"select_objects", tSelectObjects},
            {"import_objects", tImportObjects},
            {"get_level_string", tGetLevelString},
            {"history", tHistory},
            {"playtest", tPlaytest},
            {"save_level", tSaveLevel},
            {"scroll_to", tScrollTo},
            {"screenshot", tScreenshot},
            {"backup_level", tBackupLevel},
            {"restore_backup", tRestoreBackup},
            {"clear_level", tClearLevel},
        };
        return r;
    }

    matjson::Value textResult(std::string const& text, bool isError, std::string const& imageB64 = "") {
        auto content = makeArray();
        content.push(matjson::makeObject({{"type", "text"}, {"text", text}}));
        if (!imageB64.empty()) {
            content.push(matjson::makeObject({
                {"type", "image"},
                {"data", imageB64},
                {"mimeType", "image/png"},
            }));
        }
        return matjson::makeObject({{"content", content}, {"isError", isError}});
    }

} // namespace

matjson::Value callTool(std::string const& name, matjson::Value args) {
    auto it = registry().find(name);
    if (it == registry().end()) return textResult("Unknown tool: " + name, true);
    if (!args.isObject()) args = matjson::Value::object();
    try {
        auto out = it->second(args);
        return textResult(out.data.dump(0), false, out.imageB64);
    }
    catch (Fail const& f) {
        return textResult(f.msg, true);
    }
    catch (std::exception const& ex) {
        return textResult(std::string("Internal error: ") + ex.what(), true);
    }
}

}
