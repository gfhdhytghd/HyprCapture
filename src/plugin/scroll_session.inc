// Included in artifact_capture.cpp to share its native framebuffer/PBO helpers.
// No Qt or image matching executes on the compositor thread.
namespace {
bool scrollContains(const Rect &a, const Rect &b) {
  return b.x >= a.x && b.y >= a.y && b.x + b.width <= a.x + a.width &&
         b.y + b.height <= a.y + a.height;
}
struct ScrollInput {
  IPointer::SAxisEvent event;
  Vector2D position;
  bool native = false;
  SP<IPointer> pointer;
};
struct ScrollSession {
  std::string id, windowAddress;
  Rect target;
  PHLMONITOR monitor;
  PHLWINDOW window;
  SP<CWLSurfaceResource> surface;
  WP<CWLSurfaceResource> inputRoot;
  Rect windowGeometry;
  double scale = 1;
  int transform = 0, control = -1;
  pid_t owner = 0;
  wl_event_source *controlSource = nullptr;
  std::string input;
  std::vector<Rect> excluded;
  std::vector<ScrollInput> pending;
  std::unique_ptr<WindowStreamSender> sender;
  WindowStreamPboReadbackState pbo;
  SP<CEventLoopTimer> timer;
  CHyprSignalListener axis, button, unmap;
  std::uint64_t sequence = 0;
  bool active = false, baseline = false, released = false, replaying = false,
       failed = false, paused = false;
  Time::steady_tp started = Time::steadyNow();
};
std::unique_ptr<ScrollSession> g_scrollSession;
CFunctionHook *g_scrollLayerHook = nullptr;
using ScrollLayerFn = void (*)(void *, PHLLS, PHLMONITOR,
                               const Time::steady_tp &, bool, bool);
ScrollLayerFn g_scrollLayerOriginal = nullptr;
bool g_scrollRendering = false;
void hkScrollLayer(void *renderer, PHLLS layer, PHLMONITOR monitor,
                   const Time::steady_tp &time, bool popups, bool lockscreen) {
  if (g_scrollRendering && g_scrollSession && layer &&
      layer->getPID() == g_scrollSession->owner &&
      layer->m_namespace.starts_with("hyprcapture-"))
    return;
  if (g_scrollLayerOriginal)
    g_scrollLayerOriginal(renderer, layer, monitor, time, popups, lockscreen);
}
bool ensureScrollLayerHook() {
  if (g_scrollLayerHook)
    return true;
  auto source = findFunctionByDemangledName("renderLayer", "::renderLayer(");
  if (!source)
    return false;
  g_scrollLayerHook = HyprlandAPI::createFunctionHook(
      g_pluginHandle, source, reinterpret_cast<void *>(&hkScrollLayer));
  if (!g_scrollLayerHook || !g_scrollLayerHook->hook()) {
    removeRealBackgroundHook(g_scrollLayerHook);
    return false;
  }
  g_scrollLayerOriginal =
      reinterpret_cast<ScrollLayerFn>(g_scrollLayerHook->m_original);
  return g_scrollLayerOriginal;
}
void scrollNotify(ScrollSession &s, const Json &event) {
  const std::string data = event.dump() + "\n";
  if (s.control < 0 ||
      send(s.control, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL) !=
          static_cast<ssize_t>(data.size()))
    s.failed = true;
}
void stopScrollSession() {
  auto s = std::move(g_scrollSession);
  if (!s)
    return;
  s->axis.reset();
  s->button.reset();
  s->unmap.reset();
  if (s->controlSource)
    wl_event_source_remove(s->controlSource);
  if (s->control >= 0)
    close(s->control);
  if (s->timer && g_pEventLoopManager)
    g_pEventLoopManager->removeTimer(s->timer);
  s->timer.reset();
  if (s->sender)
    s->sender->stop();
  if (g_pHyprOpenGL) {
    g_pHyprOpenGL->makeEGLCurrent();
    resetWindowStreamPboReadback(s->pbo);
  }
}
bool scrollTargetAlive(ScrollSession &s) {
  if (!s.monitor || !s.monitor->m_activeWorkspace ||
      s.monitor->m_scale != s.scale ||
      static_cast<int>(s.monitor->m_transform) != s.transform)
    return false;
  if (s.window) {
    if (!isLiveWindowCaptureTarget(s.window) ||
        (s.window->wlSurface() ? s.window->wlSurface()->resource() : nullptr) !=
            s.surface)
      return false;
    return rectEqual(s.windowGeometry,
                     toRect(renderedWindowBox(
                         s.window, s.window->getFullWindowBoundingBox())));
  }
  return scrollContains(monitorRect(s.monitor), s.target);
}
void scrollDeliver(ScrollSession &s, const ScrollInput &input) {
  auto window = s.window
                    ? s.window
                    : Desktop::viewState()->hitTest().windowAt(
                          input.position, Desktop::View::RESERVED_EXTENTS |
                                              Desktop::View::INPUT_EXTENTS |
                                              Desktop::View::ALLOW_FLOATING);
  if (!isLiveWindowCaptureTarget(window)) {
    s.failed = true;
    return;
  }
  Vector2D local;
  auto surface = Desktop::viewState()->hitTest().windowSurfaceAt(input.position,
                                                                 window, local);
  if (!surface) {
    s.failed = true;
    return;
  }
  s.inputRoot = window->wlSurface()->resource();
  g_pSeatManager->setPointerFocus(surface, local);
  g_pSeatManager->sendPointerMotion(input.event.timeMs, local);
  // GTK 3 flushes either pointer motion or scroll data for a wl_pointer.frame.
  // Finish the synthetic positioning event first, otherwise a FINGER axis in
  // the same frame is swallowed (while Gecko's separate wheel path still works).
  g_pSeatManager->sendPointerFrame();
  if (input.native) {
    // Re-enter the native path exactly once, preserving per-device factors,
    // wheel v120 accumulation, application overrides and natural scrolling.
    s.replaying = true;
    const bool layer = g_pInputManager->m_lastFocusOnLS;
    g_pInputManager->m_lastFocusOnLS = false;
    g_pInputManager->onMouseWheel(input.event, input.pointer);
    g_pInputManager->onPointerFrame();
    g_pInputManager->m_lastFocusOnLS = layer;
    s.replaying = false;
  } else {
    g_pSeatManager->sendPointerAxis(
        input.event.timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, input.event.delta,
        input.event.deltaDiscrete / 120, input.event.deltaDiscrete,
        input.event.source, input.event.relativeDirection);
    g_pSeatManager->sendPointerFrame();
  }
  // Pointer motion naturally restores overlay focus on the next event.
}
void scrollInput(ScrollSession &s, ScrollInput input) {
  if (s.failed || s.paused)
    return;
  if (!s.active) {
    s.active = true;
    s.started = Time::steadyNow();
    scrollNotify(s, {{"event", "begin"}});
    s.timer->updateTimeout(std::chrono::milliseconds(1));
  }
  scrollNotify(s, {{"event", "motion"}, {"delta", input.event.delta}});
  if (!s.released) {
    if (s.pending.size() >= 256) {
      s.failed = true;
      return;
    }
    s.pending.push_back(input);
  } else
    scrollDeliver(s, input);
}
int scrollControlReadable(int fd, uint32_t mask, void *data) {
  auto &s = *static_cast<ScrollSession *>(data);
  if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
    s.failed = true;
    return 0;
  }
  char bytes[4096];
  const auto count = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
  if (count <= 0) {
    if (count == 0 || (errno != EAGAIN && errno != EINTR))
      s.failed = true;
    return 0;
  }
  s.input.append(bytes, count);
  if (s.input.size() > 16384) {
    s.failed = true;
    return 0;
  }
  while (true) {
    const auto end = s.input.find('\n');
    if (end == std::string::npos)
      break;
    auto j = Json::parse(s.input.substr(0, end), nullptr, false);
    s.input.erase(0, end + 1);
    if (!j.is_object()) {
      s.failed = true;
      break;
    }
    try {
      const auto type = j.at("command").get<std::string>();
      if (type == "stop") {
        s.failed = true;
        break;
      }
      if (type == "pause") {
        s.paused = true;
        continue;
      }
      if (type == "baseline") {
        if (!s.baseline || s.released)
          continue;
        s.released = true;
        for (const auto &input : s.pending)
          scrollDeliver(s, input);
        s.pending.clear();
        continue;
      }
      if (type == "target" && !s.active && s.windowAddress.empty()) {
        const auto r = j.at("rect");
        Rect target{r.at(0).get<double>(), r.at(1).get<double>(),
                    r.at(2).get<double>(), r.at(3).get<double>()};
        if (!std::isfinite(target.x) || !std::isfinite(target.y) ||
            !std::isfinite(target.width) || !std::isfinite(target.height) ||
            target.width < 64 || target.height < 96 ||
            target.width * target.height * s.scale * s.scale >
                32 * 1024 * 1024 ||
            !scrollContains(monitorRect(s.monitor), target)) {
          s.failed = true;
          break;
        }
        s.target = target;
        continue;
      }
      if (type == "exclude") {
        if (!j.at("rects").is_array() || j["rects"].size() > 32) {
          s.failed = true;
          break;
        }
        s.excluded.clear();
        for (const auto &r : j["rects"]) {
          Rect rect{r.at(0).get<double>(), r.at(1).get<double>(),
                    r.at(2).get<double>(), r.at(3).get<double>()};
          if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
              !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
              rect.width < 0 || rect.height < 0)
            throw std::runtime_error("exclude rect");
          s.excluded.push_back(rect);
        }
        continue;
      }
      if (type == "axis") {
        const double x = j.at("x"), y = j.at("y"), delta = j.at("delta");
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(delta) ||
            std::abs(delta) > 10000 ||
            !scrollContains(s.target, {x, y, 0.01, 0.01})) {
          s.failed = true;
          break;
        }
        const auto ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Time::steadyNow().time_since_epoch())
                .count());
        ScrollInput input{
            .event = {.timeMs = ms,
                      .source = j.value("finger", false)
                                    ? WL_POINTER_AXIS_SOURCE_FINGER
                                    : WL_POINTER_AXIS_SOURCE_WHEEL,
                      .axis = WL_POINTER_AXIS_VERTICAL_SCROLL,
                      .relativeDirection =
                          j.value("inverted", false)
                              ? WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
                              : WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
                      .delta = delta,
                      .deltaDiscrete = j.value("discrete", 0)},
            .position = {x, y}};
        scrollInput(s, input);
      }
    } catch (...) {
      s.failed = true;
      break;
    }
  }
  return 0;
}
void scrollTick(SP<CEventLoopTimer> timer) {
  auto *s = g_scrollSession.get();
  if (!s || s->timer != timer)
    return;
  if (s->failed ||
      s->sender->state() == WindowStreamSenderState::Disconnected) {
    stopScrollSession();
    return;
  }
  timer->updateTimeout(std::chrono::milliseconds(33));
  if (!s->active || s->paused)
    return;
  if (!scrollTargetAlive(*s) ||
      Time::steadyNow() - s->started > std::chrono::minutes(5)) {
    scrollNotify(*s,
                 {{"event", "stopped"},
                  {"reason", "Target or display changed; finish or cancel"}});
    s->paused = true;
    return;
  }
  // Offscreen reads intentionally suppress presentation feedback. A client
  // obscured by the editor may otherwise wait forever for its next frame
  // callback, leaving every capture identical to the baseline. Pace only the
  // actual scroll recipient at the capture cadence; do not fake presentation.
  if (s->released) {
    if (auto root = s->inputRoot.lock())
      root->breadthfirst([](SP<CWLSurfaceResource> surface, const Vector2D&, void*) {
        surface->frame(Time::steadyNow());
      }, nullptr);
  }
  g_pHyprOpenGL->makeEGLCurrent();
  GLint previousPack = 0, previousRead = 0, previousAlignment = 0;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPack);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
  glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
  const auto restore = [&] {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
    glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
  };
  auto &pbo = s->pbo;
  if (auto ready = takeReadyWindowStreamPboFrames(pbo)) {
    auto &f = ready->frame;
    if (!s->window && s->transform != 0) {
      RgbaReadback raw{.pixels = std::move(f.rgba),
                       .width = int(f.metadata.pixelWidth),
                       .height = int(f.metadata.pixelHeight)};
      raw = normalizeMonitorReadbackToLogicalOrientation(std::move(raw),
                                                         s->transform);
      const auto mon = monitorRect(s->monitor);
      raw = cropReadbackToBounds(
          raw, {.x = std::lround((s->target.x - mon.x) * s->scale),
                .y = std::lround((s->target.y - mon.y) * s->scale),
                .width = std::lround(s->target.width * s->scale),
                .height = std::lround(s->target.height * s->scale)});
      f.rgba = std::move(raw.pixels);
      f.metadata.pixelWidth = raw.width;
      f.metadata.pixelHeight = raw.height;
      f.metadata.stride = raw.width * 4;
      f.metadata.payloadBytes = f.rgba.size();
    }
    if (s->sender->submit(f.metadata, std::move(f.rgba)))
      s->baseline = true;
  }
  // Do not overwrite the baseline before the receiver has installed it.
  if ((s->baseline && !s->released) || pbo.pending) {
    restore();
    return;
  }
  const auto timeNs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Time::steadyNow().time_since_epoch())
          .count());
  int width = 0, height = 0, cropX = 0, cropTop = 0;
  Rect geometry = s->target;
  CBox artifact;
  g_scrollRendering = true;
  if (s->window) {
    const auto native = windowCaptureGeometry(
        renderedWindowBox(s->window, s->window->getFullWindowBoundingBox()),
        s->monitor);
    width = native.pixelWidth;
    height = native.pixelHeight;
    if (!ensureWindowStreamPboReadback(pbo, width, height,
                                       size_t(width) * height * 4)) {
      g_scrollRendering = false;
      restore();
      s->failed = true;
      return;
    }
    WindowRenderOptions options;
    options.framebufferOverride = &pbo.framebuffer;
    options.skipReadback = true;
    const auto info = renderWindowArtifactReadback(
        s->window, s->monitor, Time::steadyNow(), true, width, height, artifact,
        nullptr, false, options);
    cropX = info.cropX;
    cropTop = info.cropTopY;
    geometry = toRect(artifact);
  } else {
    const auto mon = monitorRect(s->monitor);
    width = std::lround(s->target.width * s->scale);
    height = std::lround(s->target.height * s->scale);
    cropX = std::lround((s->target.x - mon.x) * s->scale);
    cropTop = std::lround(s->monitor->m_pixelSize.y) -
              std::lround((s->target.y - mon.y) * s->scale) - height;
    if (s->transform != 0) {
      width = std::lround(s->monitor->m_pixelSize.x);
      height = std::lround(s->monitor->m_pixelSize.y);
      cropX = cropTop = 0;
    }
    if (!ensureWindowStreamPboReadback(pbo, width, height,
                                       size_t(width) * height * 4)) {
      g_scrollRendering = false;
      restore();
      s->failed = true;
      return;
    }
    renderMonitorReadback(s->monitor, Time::steadyNow(), 0, 0, width, height,
                          nullptr, &pbo.framebuffer, true);
    artifact = {geometry.x, geometry.y, geometry.width, geometry.height};
  }
  g_scrollRendering = false;
  if (!pbo.framebuffer || width < 1 || height < 1) {
    restore();
    s->failed = true;
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebufferId(*pbo.framebuffer));
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  WindowStreamFrameMetadata meta{.sequence = ++s->sequence,
                                 .captureMonotonicNs = timeNs,
                                 .geometryEpoch = 1,
                                 .logicalX = geometry.x,
                                 .logicalY = geometry.y,
                                 .logicalWidth = geometry.width,
                                 .logicalHeight = geometry.height,
                                 .pixelWidth = uint32_t(width),
                                 .pixelHeight = uint32_t(height),
                                 .stride = uint32_t(width * 4),
                                 .payloadBytes = uint64_t(width) * height * 4};
  if (!issueWindowStreamPboReadback(pbo, meta, s->id, *pbo.framebuffer, cropX,
                                    cropTop, artifact, artifact))
    s->failed = true;
  restore();
}
} // namespace

LaunchResult startScrollSessionFromRequestFile(const std::string &path) {
  const auto raw = readPrivateRequestFile(path);
  if (!raw || g_scrollSession || !g_pEventLoopManager || !g_pSeatManager)
    return {.success = false, .error = "scroll session unavailable"};
  const auto j = Json::parse(*raw, nullptr, false);
  if (!j.is_object())
    return {.success = false, .error = "invalid scroll request"};
  auto s = std::make_unique<ScrollSession>();
  try {
    if (j.at("version") != 1)
      throw std::runtime_error("version");
    s->id = j.at("id").get<std::string>();
    s->windowAddress = j.value("windowAddress", std::string{});
    if (s->id.empty() || s->id.size() > 96)
      throw std::runtime_error("id");
    const auto r = j.at("rect");
    s->target = {r.at(0).get<double>(), r.at(1).get<double>(),
                 r.at(2).get<double>(), r.at(3).get<double>()};
    if (!std::isfinite(s->target.x) || !std::isfinite(s->target.y) ||
        !std::isfinite(s->target.width) || !std::isfinite(s->target.height) ||
        s->target.width < 64 || s->target.height < 96)
      throw std::runtime_error("rect");
    for (const auto &m : State::monitorState()->monitors())
      if (m && scrollContains(monitorRect(m), s->target)) {
        s->monitor = m;
        break;
      }
    if (!s->monitor)
      throw std::runtime_error("monitor");
    s->scale = s->monitor->m_scale;
    s->transform = static_cast<int>(s->monitor->m_transform);
    if (s->target.width * s->target.height * s->scale * s->scale >
        32 * 1024 * 1024)
      throw std::runtime_error("size");
    if (!s->windowAddress.empty()) {
      s->window = findWindowByAddress(s->windowAddress);
      if (!isLiveWindowCaptureTarget(s->window))
        throw std::runtime_error("window");
      s->windowGeometry = toRect(
          renderedWindowBox(s->window, s->window->getFullWindowBoundingBox()));
      s->surface =
          s->window->wlSurface() ? s->window->wlSurface()->resource() : nullptr;
    }
    const auto frames = j.at("socketPath").get<std::string>(),
               control = j.at("controlPath").get<std::string>();
    if (!trustedPrivateStreamSocketPath(frames) ||
        !trustedPrivateStreamSocketPath(control) ||
        control.size() >= sizeof(sockaddr_un{}.sun_path))
      throw std::runtime_error("socket");
    s->control = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, control.c_str(), control.size() + 1);
    if (s->control < 0 ||
        connect(s->control, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0)
      throw std::runtime_error("connect");
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(s->control, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
        cred.uid != getuid())
      throw std::runtime_error("owner");
    s->owner = cred.pid;
    if (!ensureScrollLayerHook())
      throw std::runtime_error("layer exclusion");
    s->sender = std::make_unique<WindowStreamSender>(
        WindowStreamSenderConfig{.socketPath = frames});
    s->timer = makeShared<CEventLoopTimer>(
        std::chrono::milliseconds(33),
        [](SP<CEventLoopTimer> t, void *) { scrollTick(t); }, nullptr);
    s->controlSource =
        wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, s->control,
                             WL_EVENT_READABLE, scrollControlReadable, s.get());
    if (!s->controlSource)
      throw std::runtime_error("control watch");
    auto *ptr = s.get();
    if (s->window)
      s->unmap = s->window->m_events.unmap.listen([ptr] {
        ptr->paused = true;
        scrollNotify(*ptr, {{"event", "stopped"},
                            {"reason", "Target closed; finish or cancel"}});
      });
    s->button = Event::bus()->m_events.input.mouse.button.listen(
        [ptr](IPointer::SButtonEvent, Event::SCallbackInfo &) {
          // A scroll frame keeps target focus until the receiver consumes it.
          // A later click must re-hit-test the annotation layer first.
          if (ptr->active && !ptr->replaying && !ptr->failed)
            g_pInputManager->simulateMouseMovement();
        });
    s->axis = Event::bus()->m_events.input.mouse.axis.listen(
        [ptr](IPointer::SAxisEvent e, Event::SCallbackInfo &info) {
          if (ptr->replaying || ptr->failed || ptr->paused ||
              (g_pInputManager->getModsFromAllKBs() & Input::HL_MODIFIER_CTRL) ||
              g_pInputManager->hasHeldButtons())
            return;
          const auto point = g_pInputManager->getMouseCoordsInternal();
          if (!scrollContains(ptr->target, {point.x, point.y, .01, .01}))
            return;
          for (const auto &r : ptr->excluded)
            if (scrollContains(r, {point.x, point.y, .01, .01}))
              return;
          if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
          info.cancelled = true;
          scrollInput(*ptr, {.event = e,
                             .position = point,
                             .native = true,
                             .pointer = g_pSeatManager->m_mouse.lock()});
        });
  } catch (const std::exception &e) {
    if (s->controlSource)
      wl_event_source_remove(s->controlSource);
    if (s->control >= 0)
      close(s->control);
    return {.success = false,
            .error = std::string("scroll session: ") + e.what()};
  }
  g_scrollSession = std::move(s);
  g_pEventLoopManager->addTimer(g_scrollSession->timer);
  return {.success = true};
}
LaunchResult stopScrollSessionFromRequestFile(const std::string &path) {
  const auto raw = readPrivateRequestFile(path);
  if (!raw)
    return {.success = false, .error = "invalid scroll stop"};
  const auto j = Json::parse(*raw, nullptr, false);
  if (!j.is_object() || !j.contains("id") || !j["id"].is_string() ||
      !g_scrollSession || j["id"] != g_scrollSession->id)
    return {.success = false, .error = "scroll session not active"};
  stopScrollSession();
  return {.success = true};
}
