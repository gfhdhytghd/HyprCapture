"""Compositor/config wiring checks; editor behaviour is covered by Qt tests."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
main = (root / 'src/plugin/main.cpp').read_text()
ui = (root / 'src/ui/main.cpp').read_text()
editor = (root / 'src/ui/annotation_editor.cpp').read_text()
assert 'annotationLiveText' in editor, 'Live text editor tool missing'
assert 'registerLuaFunction("ocr"' not in main, 'No standalone OCR action is permitted'
assert main.count('warnOcrFallback();') == 2, 'Warn only on initial config load and reload'
assert 'g_ocrFallbackWarning.update(readDefaults().ocr)' in main, 'Config warning must deduplicate identical values'
for key in ('mode', 'engine', 'languages', 'command', 'output', 'timeout', 'api_url', 'api_key'):
    assert f'"ocr_{key}"' in main, f'OCR config registration/read missing: {key}'
    cli_key = key.replace("_", "-")
    assert f'"ocr-{cli_key}"' in ui, f'OCR CLI option missing: {key}'

launcher = (root / 'src/plugin/session_launcher.cpp').read_text()
assert '"--ocr-api-url"' not in launcher, 'URL must travel in private session JSON'
assert '"--ocr-api-key"' not in launcher, 'Key must travel in private session JSON'
print('OCR editor/config/CLI wiring passed')
