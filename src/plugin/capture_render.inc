// Included after the native framebuffer helpers. All standalone passes must
// select isolated scene resources; FULL_FAKE alone still mutates live effects.
bool beginCaptureRender(const PHLMONITOR& monitor, CRegion& damage, const SP<CFramebuffer>& framebuffer,
                        const Time::steady_tp& frozenTime, bool simple = false) {
    Render::SRenderOptions options;
    options.sceneResources = makeShared<Render::CSceneResources>(g_pHyprRenderer->createFB("hyprcapture-blur"),
        Render::SSceneBufferDescription{.format = framebuffer->m_drmFormat, .imageDescription = monitor->workBufferImageDescription()});
    if (!g_pHyprRenderer->beginRender(monitor, damage, RENDER_MODE_FULL_FAKE, nullptr, framebuffer, simple, options))
        return false;
    // beginRender resets context flags and samples wall time. Capture passes
    // share the caller's frozen instant, while preserving monitor privacy policy.
    renderContext().m_frameTime = frozenTime;
    renderContext().m_blockSurfaceFeedback = true;
    return true;
}
