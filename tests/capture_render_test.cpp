#undef NDEBUG
#include <cassert>
#include <chrono>
#include <memory>
#include <string>

// Compile the production entry point against a renderer that models the fork's
// default live-resource selection. The real plugin also compiles this same body
// against the isolated fork headers, including the resource constructor contract.
template <typename T> using SP = std::shared_ptr<T>;
template <typename T, typename... Args> auto makeShared(Args&&... args) { return std::make_shared<T>(std::forward<Args>(args)...); }
namespace Time { using steady_tp = std::chrono::steady_clock::time_point; }
struct CRegion {};
struct CFramebuffer { int m_drmFormat = 1; };
struct Monitor { int workBufferImageDescription() const { return 17; } };
using PHLMONITOR = SP<Monitor>;
constexpr int RENDER_MODE_FULL_FAKE = 1;
namespace Render {
struct SSceneBufferDescription { int format; int imageDescription; };
struct CSceneResources {
    SP<CFramebuffer> blur;
    SSceneBufferDescription description;
    CSceneResources(SP<CFramebuffer> fb, SSceneBufferDescription desc) : blur(fb), description(desc) {}
};
struct SRenderOptions { SP<CSceneResources> sceneResources; };
}
struct Context { Time::steady_tp m_frameTime; bool m_blockSurfaceFeedback = false; bool m_renderingCapture = false; } context;
Context& renderContext() { return context; }
struct Renderer {
    bool fail = false;
    int liveClock = 0, liveDirty = 1, liveQueued = 1;
    bool lastSimple = false;
    SP<Render::CSceneResources> selected;
    SP<CFramebuffer> createFB(const std::string&) { return makeShared<CFramebuffer>(); }
    bool beginRender(PHLMONITOR, CRegion&, int mode, std::nullptr_t, SP<CFramebuffer>, bool simple, const Render::SRenderOptions& options) {
        assert(mode == RENDER_MODE_FULL_FAKE);
        if (fail) return false;
        context = {};
        lastSimple = simple;
        selected = options.sceneResources;
        if (!selected) { ++liveClock; liveDirty = liveQueued = 0; }
        return true;
    }
} renderer;
auto* g_pHyprRenderer = &renderer;
#include "plugin/capture_render.inc"

int main() {
    auto monitor = makeShared<Monitor>();
    auto fb = makeShared<CFramebuffer>();
    fb->m_drmFormat = 42;
    const auto frozen = Time::steady_tp(std::chrono::seconds(9));
    CRegion damage;
    for (bool simple : {false, true}) {
        assert(beginCaptureRender(monitor, damage, fb, frozen, simple));
        assert(renderer.selected && renderer.selected->blur);
        assert(renderer.selected->description.format == 42);
        assert(renderer.selected->description.imageDescription == 17);
        assert(renderer.lastSimple == simple);
        assert(renderer.liveClock == 0 && renderer.liveDirty == 1 && renderer.liveQueued == 1);
        assert(context.m_blockSurfaceFeedback && context.m_frameTime == frozen);
        assert(!context.m_renderingCapture); // no new privacy policy as a side effect
    }
    renderer.fail = true;
    context.m_blockSurfaceFeedback = false;
    context.m_frameTime = {};
    assert(!beginCaptureRender(monitor, damage, fb, frozen));
    assert(!context.m_blockSurfaceFeedback && context.m_frameTime == Time::steady_tp{});
}
