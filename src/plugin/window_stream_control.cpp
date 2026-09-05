#include "plugin/window_stream_control.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace hyprcapture {
namespace {

using Json = nlohmann::ordered_json;

bool validId(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; });
}

bool validAddress(const std::string& address) {
    return address.size() > 2 && address.size() <= 18 && address.starts_with("0x") &&
        std::all_of(address.begin() + 2, address.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::optional<Json> parseWithoutDuplicateKeys(std::string_view raw) {
    std::unordered_map<int, std::unordered_set<std::string>> keysAtDepth;
    bool duplicate = false;
    const auto rejectDuplicateKeys = [&keysAtDepth, &duplicate](int depth, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::key)
            duplicate = duplicate || !value.is_string() || !keysAtDepth[depth].insert(value.get<std::string>()).second;
        if (event == Json::parse_event_t::object_end)
            keysAtDepth.erase(depth);
        return true;
    };
    const auto parsed = Json::parse(raw, rejectDuplicateKeys, false);
    return parsed.is_discarded() || duplicate ? std::nullopt : std::optional<Json>{std::move(parsed)};
}

std::string stringField(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
}

bool onlyKeys(const Json& object, std::initializer_list<const char*> allowed) {
    for (const auto& [key, _] : object.items()) {
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* allowedKey) { return key == allowedKey; }))
            return false;
    }
    return true;
}

} // namespace

std::optional<WindowStreamStartControl> decodeWindowStreamStartControl(std::string_view raw) {
    const auto root = parseWithoutDuplicateKeys(raw);
    if (!root || !root->is_object() || (root->size() != 5 && root->size() != 6) || !onlyKeys(*root, {"id", "mode", "windowAddress", "socketPath", "fps", "defaults"}) ||
        (stringField(*root, "mode") != "window" && stringField(*root, "mode") != "window-gpu"))
        return std::nullopt;

    WindowStreamStartControl result{
        .id = stringField(*root, "id"),
        .windowAddress = stringField(*root, "windowAddress"),
        .socketPath = stringField(*root, "socketPath"),
        .transport = stringField(*root, "mode") == "window-gpu" ? WindowStreamTransport::Gpu : WindowStreamTransport::Cpu,
    };
    if (root->contains("fps")) {
        const auto& value = (*root)["fps"];
        if (value.is_number_unsigned()) {
            const auto fps = value.get<std::uint64_t>();
            if (fps < 1 || fps > 1000)
                return std::nullopt;
            result.fps = static_cast<int>(fps);
        } else if (value.is_number_integer()) {
            const auto fps = value.get<std::int64_t>();
            if (fps < 1 || fps > 1000)
                return std::nullopt;
            result.fps = static_cast<int>(fps);
        } else {
            return std::nullopt;
        }
    }

    const auto defaults = root->find("defaults");
    if (defaults == root->end() || !defaults->is_object() || defaults->size() != 4 ||
        !onlyKeys(*defaults, {"mode", "windowBackground", "windowBorder", "windowShadow"}) || stringField(*defaults, "mode") != "window" ||
        stringField(*defaults, "windowBackground") != "transparent" || stringField(*defaults, "windowBorder") != "keep" ||
        stringField(*defaults, "windowShadow") != "keep" || !validId(result.id) || !validAddress(result.windowAddress) || result.socketPath.empty() ||
        result.socketPath.front() != '/' || result.socketPath.size() >= 108 || result.socketPath.find('\0') != std::string::npos)
        return std::nullopt;
    return result;
}

std::optional<std::string> decodeWindowStreamStopControl(std::string_view raw) {
    const auto root = parseWithoutDuplicateKeys(raw);
    if (!root || !root->is_object() || root->size() != 1 || !onlyKeys(*root, {"streamId"}))
        return std::nullopt;
    const auto id = stringField(*root, "streamId");
    return validId(id) ? std::optional<std::string>{id} : std::nullopt;
}

} // namespace hyprcapture
