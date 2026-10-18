#pragma once
#include "shared/config.hpp"
#include <optional>
#include <sstream>
#include <string>
extern "C" {
#include <lua.h>
}

namespace hyprcapture {
struct CaptureOptions {
    std::optional<bool> save;
    std::optional<bool> clipboard;
    void apply(CaptureDefaults& defaults) const {
        if (save) defaults.save = *save;
        if (clipboard) defaults.clipboard = *clipboard;
    }
};

// Shared by the string dispatch entry and the Lua table entry. No global writes.
inline bool parseCaptureArguments(const std::string& args, CaptureMode& mode, CaptureOptions& options, std::string& error) {
    std::istringstream input(args);
    std::string token;
    bool haveMode = false;
    while (input >> token) {
        const auto split = token.find('=');
        if (split == std::string::npos && !haveMode) {
            mode = parseCaptureMode(token, mode);
            haveMode = true;
            continue;
        }
        const auto key = token.substr(0, split);
        const auto value = split == std::string::npos ? std::string{} : token.substr(split + 1);
        if ((key != "save" && key != "clipboard") || (value != "true" && value != "false" && value != "1" && value != "0")) {
            error = "hyprcapture options must be save=true|false or clipboard=true|false";
            return false;
        }
        (key == "save" ? options.save : options.clipboard) = value == "true" || value == "1";
    }
    return true;
}

// Return errors to the wrapper so no C++ objects cross a Lua longjmp.
inline bool parseLuaCaptureOptions(lua_State* L, int index, CaptureOptions& options, std::string& error) {
    if (lua_gettop(L) < index || lua_isnil(L, index)) return true;
    if (!lua_istable(L, index)) {
        error = "hyprcapture capture options must be a table";
        return false;
    }
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            error = "hyprcapture capture option keys must be strings";
            lua_pop(L, 2);
            return false;
        }
        size_t keyLength = 0;
        const char* keyData = lua_tolstring(L, -2, &keyLength);
        const std::string key(keyData, keyLength);
        if (key != "save" && key != "clipboard") {
            error = "unknown hyprcapture capture option: " + key;
            lua_pop(L, 2);
            return false;
        }
        if (!lua_isboolean(L, -1)) {
            error = "hyprcapture option '" + key + "' must be a boolean";
            lua_pop(L, 2);
            return false;
        }
        (key == "save" ? options.save : options.clipboard) = bool(lua_toboolean(L, -1));
        lua_pop(L, 1);
    }
    return true;
}
} // namespace hyprcapture
