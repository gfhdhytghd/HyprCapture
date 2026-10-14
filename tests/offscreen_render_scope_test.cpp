#undef NDEBUG
#include "plugin/offscreen_render_scope.hpp"

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Region.hpp>
#include <any>
#include <cassert>
#include <stdexcept>
#include <type_traits>
#include <vector>

// Only the state fields touched by the scope; production instantiates it with
// Hyprland's actual SRenderModifData in artifact_capture.cpp.
struct SRenderModifData {
    enum eRenderModifType { RMOD_TYPE_SCALE, RMOD_TYPE_TRANSLATE, RMOD_TYPE_ROTATE };
    std::vector<std::pair<eRenderModifType, std::any>> modifs;
    bool enabled = true;
};
using Hyprutils::Math::Vector2D;
using OffscreenRenderScope = hyprcapture::OffscreenRenderScope<SRenderModifData>;

// Match the modifier arm of hyprglass's foreign-render guard. A full fake
// render can have currentFB == mainFB, so framebuffer identity is insufficient.
static bool foreign(const SRenderModifData& data) {
    return data.enabled && !data.modifs.empty();
}

static void checkIdentity(const SRenderModifData& data) {
    assert(foreign(data));
    assert(data.modifs.size() == 1);
    assert(data.modifs.front().first == SRenderModifData::RMOD_TYPE_TRANSLATE);
    const auto translation = std::any_cast<Vector2D>(data.modifs.front().second);
    assert(translation.x == 0 && translation.y == 0);
    // These are the primitives used by 0.56.2's applyToBox/applyToRegion.
    // Translation does not participate in combinedScale (which stays at 1).
    Hyprutils::Math::CBox box{-9.25, 7.75, 101.5, 42.25};
    box.rot = 0.3;
    box.translate(translation);
    assert(box.x == -9.25 && box.y == 7.75 && box.w == 101.5 && box.h == 42.25 && box.rot == 0.3);
    Hyprutils::Math::CRegion damage{-9, 7, 102, 43};
    damage.translate(translation);
    const auto extents = damage.getExtents();
    assert(extents.x == -9 && extents.y == 7 && extents.w == 102 && extents.h == 43);
}

static SRenderModifData priorState(bool enabled) {
    SRenderModifData data;
    data.enabled = enabled;
    data.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, 1.25F);
    data.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{-17.5, 23.25});
    data.modifs.emplace_back(SRenderModifData::RMOD_TYPE_ROTATE, 0.2F);
    return data;
}

static void checkPrior(const SRenderModifData& data, bool enabled) {
    assert(data.enabled == enabled && data.modifs.size() == 3);
    assert(data.modifs[0].first == SRenderModifData::RMOD_TYPE_SCALE);
    assert(std::any_cast<float>(data.modifs[0].second) == 1.25F);
    assert(data.modifs[1].first == SRenderModifData::RMOD_TYPE_TRANSLATE);
    const auto translation = std::any_cast<Vector2D>(data.modifs[1].second);
    assert(translation.x == -17.5 && translation.y == 23.25);
    assert(data.modifs[2].first == SRenderModifData::RMOD_TYPE_ROTATE);
    assert(std::any_cast<float>(data.modifs[2].second) == 0.2F);
}

int main() {
    static_assert(!std::is_copy_constructible_v<OffscreenRenderScope>);
    static_assert(!std::is_move_constructible_v<OffscreenRenderScope>);
    static_assert(std::is_nothrow_destructible_v<OffscreenRenderScope>);
    SRenderModifData data;
    {
        OffscreenRenderScope capture(data);
        assert(!foreign(data));
        capture.activate();
        checkIdentity(data);
        {
            OffscreenRenderScope nested(data);
            data = {}; // Simulate render setup replacing the prior state.
            nested.activate();
            checkIdentity(data);
        }
        checkIdentity(data);
    }
    assert(data.enabled && data.modifs.empty());

    for (bool enabled : {false, true}) {
        data = priorState(enabled);
        {
            OffscreenRenderScope capture(data);
            checkPrior(data, enabled); // Construction does not mutate the state.
            data = {};
            capture.activate();
            checkIdentity(data);
            data.enabled = false; // A pass element may change renderer hints.
            data.modifs.clear();
        }
        checkPrior(data, enabled);
        try {
            OffscreenRenderScope capture(data);
            data = {};
            capture.activate();
            throw std::runtime_error("render failed");
        } catch (const std::runtime_error&) {}
        checkPrior(data, enabled);
        // Failed beginFullFakeRender / early return, before activation.
        [&] {
            OffscreenRenderScope capture(data);
            data = {};
            return;
        }();
        checkPrior(data, enabled);
    }
}
