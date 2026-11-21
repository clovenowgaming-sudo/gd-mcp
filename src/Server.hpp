#pragma once

#include <atomic>
#include <string>

namespace mcp {

    /// Tiny HTTP server speaking MCP "Streamable HTTP" (JSON responses, POST /mcp).
    class Server {
    public:
        static Server& get();

        bool start();
        void stop();
        bool running() const { return m_running; }
        int port() const { return m_port; }
        std::string const& token() const { return m_token; }

    private:
        Server() = default;

        void acceptLoop();
        void handleClient(int fd);

        std::atomic<bool> m_running{false};
        std::atomic<int> m_active{0};
        int m_listenFd = -1;
        int m_port = 0;
        std::string m_token;
    };

}
