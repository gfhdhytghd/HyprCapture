#pragma once

#include "shared/protocol.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace hyprcapture {

struct StageCaptureTarget {
    std::string address;
    Rect selection;
    Rect clip;
    std::optional<double> selectionRounding;
};

// Optional, versioned Hymission capability. Preserve bottom-to-top preview
// order, and fail closed for absent/unsupported or malformed responses.
inline std::vector<StageCaptureTarget> parseStageCaptureTargets(std::string_view response) {
    using Json = nlohmann::json;
    const auto root = Json::parse(response, nullptr, false);
    if (!root.is_object() || !root.contains("captureVersion") || root["captureVersion"] != 1 ||
        !root.contains("enabled") || root["enabled"] != true ||
        !root.contains("captureWindows") || !root["captureWindows"].is_array() || root["captureWindows"].size() > 512)
        return {};
    const auto rect = [](const Json& item, const char* key, Rect& out) {
        if (!item.contains(key) || !item[key].is_object())
            return false;
        const auto& value = item[key];
        double* fields[] = {&out.x, &out.y, &out.width, &out.height};
        const char* names[] = {"x", "y", "width", "height"};
        for (int i = 0; i < 4; ++i) {
            if (!value.contains(names[i]) || !value[names[i]].is_number())
                return false;
            *fields[i] = value[names[i]].get<double>();
            if (!std::isfinite(*fields[i]) || std::abs(*fields[i]) > 1000000 || (i >= 2 && *fields[i] < 1))
                return false;
        }
        return true;
    };
    std::vector<StageCaptureTarget> result;
    for (const auto& item : root["captureWindows"]) {
        StageCaptureTarget target;
        if (!item.is_object() || !item.contains("address") || !item["address"].is_string())
            continue;
        target.address = item["address"].get<std::string>();
        if (!target.address.starts_with("0x") || target.address.size() < 3 || target.address.size() > 18 ||
            target.address.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos ||
            !rect(item, "selectionGeometry", target.selection) || !rect(item, "selectionClipGeometry", target.clip))
            continue;
        if (std::max(target.selection.x, target.clip.x) >= std::min(target.selection.x + target.selection.width, target.clip.x + target.clip.width) ||
            std::max(target.selection.y, target.clip.y) >= std::min(target.selection.y + target.selection.height, target.clip.y + target.clip.height))
            continue;
        if (item.contains("selectionRounding")) {
            if (!item["selectionRounding"].is_number())
                continue;
            const double radius = item["selectionRounding"].get<double>();
            if (!std::isfinite(radius) || radius < 0 || radius > 1000000)
                continue;
            target.selectionRounding = radius;
        }
        result.push_back(std::move(target));
    }
    return result;
}

} // namespace hyprcapture
