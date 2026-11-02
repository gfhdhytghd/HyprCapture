#pragma once

#include <QColor>
#include <cmath>

namespace hyprcapture::ui {
// Linear blend from a (amount 0) to b (amount 1), used to derive palette shades.
inline QColor mixedColor(QColor a, QColor b, double amount) {
    const auto mix = [&](int av, int bv) { return static_cast<int>(std::round(av * (1.0 - amount) + bv * amount)); };
    return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()), mix(a.alpha(), b.alpha()));
}
} // namespace hyprcapture::ui
