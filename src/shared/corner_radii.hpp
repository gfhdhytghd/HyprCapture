#pragma once

#include <algorithm>

namespace hyprcapture {

// Clockwise from the top left, in the same coordinate space as the frame.
struct CornerRadii {
    double topLeft = 0.0;
    double topRight = 0.0;
    double bottomRight = 0.0;
    double bottomLeft = 0.0;

    bool operator==(const CornerRadii&) const = default;

    static CornerRadii uniform(double radius) {
        return {radius, radius, radius, radius};
    }

    double max() const {
        return std::max({topLeft, topRight, bottomRight, bottomLeft});
    }

    CornerRadii operator*(double scale) const {
        return {topLeft * scale, topRight * scale, bottomRight * scale, bottomLeft * scale};
    }

    CornerRadii expanded(double amount) const {
        const auto expand = [amount](double radius) { return radius > 0.0 ? std::max(0.0, radius + amount) : 0.0; };
        return {expand(topLeft), expand(topRight), expand(bottomRight), expand(bottomLeft)};
    }

    CornerRadii clamped(double width, double height) const {
        const double limit = std::max(0.0, std::min(width, height) * 0.5);
        return {std::clamp(topLeft, 0.0, limit), std::clamp(topRight, 0.0, limit),
                std::clamp(bottomRight, 0.0, limit), std::clamp(bottomLeft, 0.0, limit)};
    }

    double at(bool left, bool top) const {
        if (top)
            return left ? topLeft : topRight;
        return left ? bottomLeft : bottomRight;
    }
};

} // namespace hyprcapture
