"""Guard production wiring that the isolated C++ callback/renderer tests cannot link."""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
main = (root / 'src/plugin/main.cpp').read_text()
artifact = (root / 'src/plugin/artifact_capture.cpp').read_text()
# These callbacks must be connected to the real EventBus, not just unit-tested.
reload = re.search(r'g_configReloadListeners\.listen\((.*?)\n    \}\);', main, re.S)
assert reload, 'config reload callbacks are not connected'
assert 'Event::bus()->m_events.config, restoreConfigRegistrations' in reload[1]
assert 'installOverlayLayerRule();' in reload[1], 'no-animation rule lost after clearAllRules'
assert 'g_configReloadListeners.reset();' in main, 'reload callbacks survive plugin exit'
# All native standalone render entries must select isolated scene resources.
assert '#include "plugin/capture_render.inc"' in artifact
assert not re.search(r'g_pHyprRenderer->begin(?:FullFake)?Render\(', artifact), 'capture bypasses isolated render entry'
assert artifact.count('beginCaptureRender(') == 6, 'monitor/cursor/window/matte/real-background route missing'
# Both CPU artifact publication and the recording framebuffer must reject the
# provisional raw prepass when the compositor did not expose a blur-only draw.
assert 'usableRealBackground(state.captured, state.requiresBlur, state.blurCaptured)' in artifact
assert 'usableRealBackground(states.front().captured, states.front().requiresBlur, states.front().blurCaptured)' in artifact
print('PASS production reload, isolated capture and background publication wiring')
