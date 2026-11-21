#pragma once

#include <Geode/Geode.hpp>
#include <matjson.hpp>
#include <string>

namespace mcp {

    /// Result object for MCP "tools/list".
    matjson::Value toolList();

    /// Instructions string sent to the client in "initialize".
    std::string serverInstructions();

    /// Runs a tool. MUST be called on the game's main thread.
    /// Returns an MCP CallToolResult ({content: [...], isError: bool}).
    matjson::Value callTool(std::string const& name, matjson::Value args);

}
