#include "plugin/capture_options.hpp"
#include "shared/protocol.hpp"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <cassert>
#include <nlohmann/json.hpp>
using namespace hyprcapture;
int main() {
    auto* L = luaL_newstate();
    assert(L);
    for (const auto* expression : {"return { save=false, clipboard=true }", "return {save=true, clipboard=false}", "return {}", "return nil"}) {
        assert(luaL_dostring(L, expression) == LUA_OK);
        CaptureOptions options;
        std::string error;
        assert(parseLuaCaptureOptions(L, -1, options, error));
        CaptureSession session;
        session.id = "per-call";
        session.filenameMetadata = FilenameMetadata{"example.app", "Title with 100%"};
        CaptureDefaults global;
        session.defaults = global;
        options.apply(session.defaults);
        const auto decoded = decodeSessionJson(encodeSessionJson(session));
        assert(decoded && decoded->filenameMetadata && decoded->filenameMetadata->windowTitle == "Title with 100%");
        assert(decoded && decoded->defaults.save == options.save.value_or(global.save));
        assert(decoded->defaults.clipboard == options.clipboard.value_or(global.clipboard));
        assert(global.save && global.clipboard);
        auto malformed = nlohmann::json::parse(encodeSessionJson(session));
        malformed["filenameMetadata"]["windowTitle"] = false;
        assert(!decodeSessionJson(malformed.dump()));
        malformed.erase("filenameMetadata");
        assert(decodeSessionJson(malformed.dump())); // Older peers have no output metadata.
        malformed["defaults"]["save"] = "false";
        assert(!decodeSessionJson(malformed.dump()));
        malformed["defaults"]["save"] = false;
        malformed["defaults"]["clipboard"] = 1;
        assert(!decodeSessionJson(malformed.dump()));
        lua_settop(L, 0);
    }
    for (const auto* expression : {"return {save=0}", "return {clipboard='true'}", "return {saving=true}", "return {[1]=true}", "return false"}) {
        assert(luaL_dostring(L, expression) == LUA_OK);
        CaptureOptions options;
        std::string error;
        const auto top = lua_gettop(L);
        assert(!parseLuaCaptureOptions(L, -1, options, error));
        assert(!error.empty() && lua_gettop(L) == top);
        lua_settop(L, 0);
    }
    CaptureOptions missing;
    std::string error;
    assert(parseLuaCaptureOptions(L, 2, missing, error) && !missing.save && !missing.clipboard);
    CaptureMode mode = CaptureMode::Window;
    CaptureOptions options;
    assert(parseCaptureArguments("region save=false clipboard=1", mode, options, error));
    assert(mode == CaptureMode::Region && options.save == false && options.clipboard == true);
    assert(parseCaptureArguments("save=1 clipboard=0", mode, options, error));
    assert(options.save == true && options.clipboard == false);
    assert(!parseCaptureArguments("region save=perhaps", mode, options, error));
    assert(!parseCaptureArguments("region clipboard=yes", mode, options, error));
    lua_close(L);
}
