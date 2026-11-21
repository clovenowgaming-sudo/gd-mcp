#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <random>

#include "Server.hpp"

using namespace geode::prelude;

static std::string randomToken() {
    static char const* hex = "0123456789abcdef";
    std::random_device rd;
    std::string t;
    for (int i = 0; i < 32; ++i) t += hex[rd() % 16];
    return t;
}

$on_mod(Loaded) {
    auto mod = Mod::get();

    auto token = mod->getSettingValue<std::string>("token");
    if (token.empty()) {
        mod->setSettingValue<std::string>("token", randomToken());
    }

    if (mod->getSettingValue<bool>("auto-start")) {
        if (!mcp::Server::get().start()) {
            log::error("GD MCP: could not start the server (check the port setting)");
        }
    }
}

class $modify(GdMcpMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        static bool shown = false;
        if (!shown) {
            shown = true;
            auto& server = mcp::Server::get();
            if (server.running()) {
                Notification::create(
                    fmt::format("GD MCP: listening on port {} (token in mod settings)", server.port()),
                    NotificationIcon::Success
                )->show();
            }
            else {
                Notification::create("GD MCP: server is not running", NotificationIcon::Error)->show();
            }
        }
        return true;
    }
};
