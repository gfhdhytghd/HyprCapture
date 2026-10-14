#pragma once

#include <hyprutils/math/Vector2D.hpp>

namespace hyprcapture {

// Mark a standalone capture as foreign without changing its geometry or the
// window's decoration list. Background-sampling plugins can then leave their
// glass in the independently captured real background instead of sampling a
// freshly cleared capture target. Native decorations still render normally.
// The state type is injected so restoration can be tested without linking a
// running compositor (Hyprland headers contain compositor-owned globals).
template <typename RenderModifData>
class OffscreenRenderScope {
  public:
    explicit OffscreenRenderScope(RenderModifData& data) : m_data(data), m_previous(data) {
        m_identity.modifs.emplace_back(RenderModifData::RMOD_TYPE_TRANSLATE, Hyprutils::Math::Vector2D{});
    }

    // Install the marker after beginFullFakeRender establishes the capture target.
    // Keep the marker through endRender, which executes the queued pass elements.
    void activate() {
        m_data = m_identity;
    }

    ~OffscreenRenderScope() noexcept {
        m_data.modifs.swap(m_previous.modifs);
        m_data.enabled = m_previous.enabled;
    }

    OffscreenRenderScope(const OffscreenRenderScope&) = delete;
    OffscreenRenderScope& operator=(const OffscreenRenderScope&) = delete;

  private:
    RenderModifData& m_data;
    RenderModifData  m_previous;
    RenderModifData  m_identity;
};

} // namespace hyprcapture
