#pragma once

namespace hyprcapture {

// A pre-surface framebuffer is only a provisional capture for a blurred target.
// Shader blend and transformed surfaces may never draw a standalone blurred BG.
inline bool usableRealBackground(bool captured, bool requiresBlur, bool blurCaptured) {
    return captured && (!requiresBlur || blurCaptured);
}

} // namespace hyprcapture
