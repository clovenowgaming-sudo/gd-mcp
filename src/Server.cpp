#include "Server.hpp"
#include "Tools.hpp"

#include <Geode/Geode.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <thread>

using namespace geode::prelude;

namespace mcp {
namespace {

    constexpr size_t MAX_HEADER = 16 * 1024;
    constexpr size_t MAX_BODY = 8 * 1024 * 1024;
    constexpr int MAX_CONNECTIONS = 16;

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    std::string trim(std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        return s.substr(b);
    }

    bool constantTimeEquals(std::string const& a, std::string const& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    bool sendAll(int fd, std::string const& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void respond(int fd, int status, char const* text, std::string const& body,
                 char const* contentType = "application/json", char const* extraHeaders = "") {
        std::string head = fmt::format(
            "HTTP/1.1 {} {}\r\n"
            "Content-Type: {}\r\n"
            "Content-Length: {}\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Authorization, Content-Type, Accept, Mcp-Session-Id, Mcp-Protocol-Version\r\n"
            "Access-Control-Expose-Headers: Mcp-Session-Id\r\n"
            "{}"
            "\r\n",
            status, text, contentType, body.size(), extraHeaders);
        sendAll(fd, head + body);
    }

    matjson::Value rpcError(matjson::Value const& id, int code, std::string const& message) {
        return matjson::makeObject({
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", matjson::makeObject({{"code", code}, {"message", message}})},
        });
    }

    matjson::Value rpcResult(matjson::Value const& id, matjson::Value const& result) {
        return matjson::makeObject({
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", result},
        });
    }

    matjson::Value toolError(std::string const& text) {
        auto content = matjson::Value::array();
        content.push(matjson::makeObject({{"type", "text"}, {"text", text}}));
        return matjson::makeObject({{"content", content}, {"isError", true}});
    }

    /// Runs a tool on the game's main thread and waits for the result.
    matjson::Value runOnMain(std::string const& name, matjson::Value const& args) {
        auto promise = std::make_shared<std::promise<matjson::Value>>();
        auto future = promise->get_future();

        Loader::get()->queueInMainThread([promise, name, args]() {
            matjson::Value result;
            try {
                result = callTool(name, args);
            }
            catch (std::exception const& ex) {
                result = toolError(std::string("Internal error: ") + ex.what());
            }
            catch (...) {
                result = toolError("Internal error.");
            }
            try {
                promise->set_value(std::move(result));
            }
            catch (...) {}
        });

        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
            return toolError("Timed out waiting for the game thread (is the game paused or in the background?).");
        }
        return future.get();
    }

    /// Handles one JSON-RPC message. Returns false if it was a notification (no response).
    bool handleMessage(matjson::Value& msg, matjson::Value& response) {
        if (!msg.isObject()) {
            response = rpcError(matjson::Value(nullptr), -32600, "Invalid request");
            return true;
        }

        bool isNotification = !msg.contains("id");
        matjson::Value id = isNotification ? matjson::Value(nullptr) : msg["id"];
        std::string method = msg.contains("method") ? msg["method"].asString().unwrapOr("") : "";

        if (isNotification) return false;

        if (method == "initialize") {
            std::string version = "2025-06-18";
            if (msg.contains("params") && msg["params"].isObject() && msg["params"].contains("protocolVersion")) {
                auto v = msg["params"]["protocolVersion"].asString();
                if (v.isOk() && !v.unwrap().empty()) version = v.unwrap();
            }
            response = rpcResult(id, matjson::makeObject({
                {"protocolVersion", version},
                {"capabilities", matjson::makeObject({
                    {"tools", matjson::makeObject({{"listChanged", false}})},
                })},
                {"serverInfo", matjson::makeObject({
                    {"name", "gd-mcp"},
                    {"version", Mod::get()->getVersion().toVString()},
                })},
                {"instructions", serverInstructions()},
            }));
            return true;
        }
        if (method == "ping") {
            response = rpcResult(id, matjson::Value::object());
            return true;
        }
        if (method == "tools/list") {
            static matjson::Value const tools = toolList();
            response = rpcResult(id, tools);
            return true;
        }
        if (method == "resources/list") {
            response = rpcResult(id, matjson::makeObject({{"resources", matjson::Value::array()}}));
            return true;
        }
        if (method == "prompts/list") {
            response = rpcResult(id, matjson::makeObject({{"prompts", matjson::Value::array()}}));
            return true;
        }
        if (method == "tools/call") {
            if (!msg.contains("params") || !msg["params"].isObject() || !msg["params"].contains("name")) {
                response = rpcError(id, -32602, "Missing params.name");
                return true;
            }
            auto& params = msg["params"];
            std::string name = params["name"].asString().unwrapOr("");
            matjson::Value args = params.contains("arguments") ? params["arguments"] : matjson::Value::object();
            log::debug("MCP tools/call {}", name);
            response = rpcResult(id, runOnMain(name, args));
            return true;
        }

        response = rpcError(id, -32601, "Method not found: " + method);
        return true;
    }

} // namespace

Server& Server::get() {
    static Server s;
    return s;
}

bool Server::start() {
    if (m_running) return true;

    auto mod = Mod::get();
    m_port = static_cast<int>(mod->getSettingValue<int64_t>("port"));
    m_token = mod->getSettingValue<std::string>("token");
    bool bindAll = mod->getSettingValue<bool>("bind-all");

    if (m_token.empty()) {
        log::error("MCP server not started: empty token");
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log::error("MCP server: socket() failed: {}", std::strerror(errno));
        return false;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(m_port));
    addr.sin_addr.s_addr = htonl(bindAll ? INADDR_ANY : INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        log::error("MCP server: bind({}) failed: {}", m_port, std::strerror(errno));
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) < 0) {
        log::error("MCP server: listen() failed: {}", std::strerror(errno));
        ::close(fd);
        return false;
    }

    m_listenFd = fd;
    m_running = true;
    std::thread([this]() { acceptLoop(); }).detach();
    log::info("MCP server listening on {}:{} (POST /mcp)", bindAll ? "0.0.0.0" : "127.0.0.1", m_port);
    return true;
}

void Server::stop() {
    if (!m_running.exchange(false)) return;
    if (m_listenFd >= 0) {
        ::shutdown(m_listenFd, SHUT_RDWR);
        ::close(m_listenFd);
        m_listenFd = -1;
    }
}

void Server::acceptLoop() {
    while (m_running) {
        int client = ::accept(m_listenFd, nullptr, nullptr);
        if (client < 0) {
            if (!m_running) break;
            if (errno == EINTR) continue;
            log::warn("MCP server: accept() failed: {}", std::strerror(errno));
            break;
        }
        if (m_active >= MAX_CONNECTIONS) {
            respond(client, 503, "Service Unavailable", R"({"error":"too many connections"})");
            ::close(client);
            continue;
        }
        ++m_active;
        std::thread([this, client]() {
            handleClient(client);
            ::close(client);
            --m_active;
        }).detach();
    }
    m_running = false;
}

void Server::handleClient(int fd) {
    timeval tv{};
    tv.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::string buf;
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > MAX_HEADER) {
            respond(fd, 431, "Request Header Fields Too Large", "{}");
            return;
        }
        auto n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return;
        buf.append(tmp, static_cast<size_t>(n));
    }

    std::string head = buf.substr(0, headerEnd);
    std::string body = buf.substr(headerEnd + 4);

    // request line
    auto lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd);
    std::map<std::string, std::string> headers;
    if (lineEnd != std::string::npos) {
        size_t pos = lineEnd + 2;
        while (pos < head.size()) {
            auto next = head.find("\r\n", pos);
            std::string line = head.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            }
            if (next == std::string::npos) break;
            pos = next + 2;
        }
    }

    auto sp1 = requestLine.find(' ');
    auto sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        respond(fd, 400, "Bad Request", R"({"error":"bad request"})");
        return;
    }
    std::string method = requestLine.substr(0, sp1);
    std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string path = target;
    std::string query;
    if (auto q = target.find('?'); q != std::string::npos) {
        path = target.substr(0, q);
        query = target.substr(q + 1);
    }

    // body
    size_t contentLength = 0;
    if (headers.count("content-length")) {
        contentLength = static_cast<size_t>(std::strtoull(headers["content-length"].c_str(), nullptr, 10));
    }
    if (contentLength > MAX_BODY) {
        respond(fd, 413, "Payload Too Large", R"({"error":"body too large"})");
        return;
    }
    if (contentLength > body.size() && headers.count("expect") && lower(headers["expect"]) == "100-continue") {
        sendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n");
    }
    while (body.size() < contentLength) {
        auto n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return;
        body.append(tmp, static_cast<size_t>(n));
    }
    if (body.size() > contentLength) body.resize(contentLength);

    // routing
    if (method == "OPTIONS") {
        respond(fd, 204, "No Content", "");
        return;
    }
    if (path == "/" || path == "/health") {
        respond(fd, 200, "OK", R"({"name":"gd-mcp","status":"ok"})");
        return;
    }
    if (path != "/mcp" && path.rfind("/mcp/", 0) != 0) {
        respond(fd, 404, "Not Found", R"({"error":"not found"})");
        return;
    }

    // auth: Bearer header, /mcp/<token>, or ?token=
    std::string supplied;
    if (headers.count("authorization")) {
        auto h = headers["authorization"];
        if (lower(h).rfind("bearer ", 0) == 0) supplied = trim(h.substr(7));
    }
    if (supplied.empty() && path.size() > 5) supplied = path.substr(5);
    if (supplied.empty()) {
        auto t = query.find("token=");
        if (t != std::string::npos) {
            auto end = query.find('&', t);
            supplied = query.substr(t + 6, end == std::string::npos ? std::string::npos : end - t - 6);
        }
    }
    if (!constantTimeEquals(supplied, m_token)) {
        respond(fd, 401, "Unauthorized", R"({"error":"invalid or missing token"})",
                "application/json", "WWW-Authenticate: Bearer\r\n");
        return;
    }

    if (method != "POST") {
        respond(fd, 405, "Method Not Allowed", R"({"error":"use POST"})", "application/json", "Allow: POST\r\n");
        return;
    }

    auto parsed = matjson::parse(body);
    if (parsed.isErr()) {
        respond(fd, 400, "Bad Request", rpcError(matjson::Value(nullptr), -32700, "Parse error").dump(0));
        return;
    }
    auto msg = parsed.unwrap();

    if (msg.isArray()) {
        auto responses = matjson::Value::array();
        auto items = msg.asArray();
        if (items.isOk()) {
            for (auto item : items.unwrap()) {
                matjson::Value r;
                if (handleMessage(item, r)) responses.push(r);
            }
        }
        if (responses.size() == 0) {
            respond(fd, 202, "Accepted", "");
        }
        else {
            respond(fd, 200, "OK", responses.dump(0));
        }
        return;
    }

    matjson::Value response;
    if (!handleMessage(msg, response)) {
        respond(fd, 202, "Accepted", "");
        return;
    }
    respond(fd, 200, "OK", response.dump(0));
}

}
