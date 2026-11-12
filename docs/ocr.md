# OCR engine layer

This is the engine layer for upcoming OCR actions. There is no OCR button,
`open("ocr")` action, selectable text layer, or helper CLI/config registration
in this phase. The shared configuration and session protocol carry the settings;
plugin registration and helper argument forwarding belong to phase 2b.
HyprCapture does not link any OCR library. The layer is Qt-free and runs external
programs synchronously. Call it from a worker, not the compositor/UI event thread.

## Configuration

| Key | Default | Values |
| --- | --- | --- |
| `ocr_mode` | `off` | `off`, `capture`, `text_layer`, `both` |
| `ocr_engine` | `tesseract` | `tesseract`, `paddleocr`, `command` |
| `ocr_languages` | `eng+chi_sim` | Nonempty, at most 4096 bytes, no NUL |
| `ocr_command` | empty | At most 65536 bytes, no NUL; required when running `command` |
| `ocr_output` | `text` | `text`, `json`; used only by `command` |
| `ocr_timeout` | `30` | Integer seconds, `1..86400` |

`off` makes the engine API fail with a message naming `ocr_mode`. The future
`capture` action will copy recognition text. `text_layer` will supply selectable
polygons in the editor. `both` enables both uses. `command` with `text` output
cannot be configured for `text_layer` or `both`; use `json` instead.

The agreed design does not specify numerical timeout or string limits. The
implementation adds the defensive bounds above. OCR fields in old session JSON
are optional and retain these defaults. Invalid present fields fail decoding.

## Unified results

With `ocr_engine=command` and `ocr_output=json`, stdout is JSON Lines:

```json
{"text":"Hello OCR 123","poly":[[9,32],[198,32],[198,57],[9,57]],"conf":0.99,"level":"line"}
{"text":"中文","poly":[[10,70],[90,70],[90,95],[10,95]],"level":"line"}
```

- `text` is a required UTF-8 JSON string.
- `poly` is a required array of 3..256 `[x,y]` points in physical input pixels.
  Coordinates must be finite and within +/-10000000. Signed area must be nonzero.
- Clockwise winding uses image coordinates: x increases right, y increases down.
  The parser reverses counterclockwise vertices while preserving the first
  vertex. Engines should start quads at the text-direction top-left; normalization
  preserves this direction, including rotated text.
- `conf` is optional, numeric, and in `0..1`.
- `level` is optional: `line` (default) or `word`.
- Unknown fields are ignored. JSON nesting is limited to 32 levels.

The input limit is 8 MiB and at most 65536 results. Each nonempty input line must
be an object; blank lines are errors. An empty stream means no recognition.
CRLF and a final newline are accepted. On any malformed record, the parser
returns no partial results and an error naming the line number.

`command` with `ocr_output=text` preserves all stdout, including final newlines,
as one `OcrResult` with an empty polygon. This representation is permitted only
for that adapter mode. `ocrSupportsTextLayer()` rejects such results. Future text
layer callers must check this predicate before using geometry.

## External adapters

### Tesseract

```text
tesseract <png> stdout -l <ocr_languages> tsv
```

Install Tesseract and the selected tessdata packages, for example
`tesseract-data-eng` and `tesseract-data-chi_sim`. The TSV adapter requires the
standard 12-column header. It reads level-5 word rows, skips blank text and
confidence `-1`, builds axis-aligned clockwise rectangles, and divides confidence
by 100. Nonpositive word dimensions, invalid confidence, and malformed rows fail
with the TSV line number.

### PaddleOCR 3.x

```text
paddleocr ocr -i <png> --save_path <private-directory> --use_doc_orientation_classify False --use_doc_unwarping False --use_textline_orientation False --enable_mkldnn False
```

The adapter reads `<png-stem>_res.json`, not stdout logs. The saved plain JSON
contains parallel `rec_texts`, `rec_polys`, and `rec_scores` arrays. Each result
has `level=line`. This saved-file format comes from the agreed design's local
PaddleOCR 3.4.0 verification. The engine disables MKLDNN by default, as required
by the task card's command, to avoid the recorded oneDNN
`ConvertPirAttribute2RuntimeAttribute` failure.

`ocr_languages` uses Tesseract syntax; the specified PaddleOCR command has no
equivalent argument. This adapter uses the installed PaddleOCR default model.
Use `command` and an explicit model/language option when a different model is
needed. No automatic conversion of `eng+chi_sim` is attempted. Cold model loading
can exceed 30 seconds; adjust the timeout explicitly.

The adapter atomically creates a 0700 directory under a private owned
`XDG_RUNTIME_DIR`, falling back to `/tmp`. It removes this directory after success,
parse failure, process failure, or timeout. The result must be a regular owned
file, not a symlink, and no larger than 8 MiB.

### Custom commands

Example configuration value (not a shell command):

```text
/home/user/bin/recognize --input="{image}" --format=json
```

Splitting rules:

1. Unquoted ASCII spaces, tabs, CR, and LF separate argv elements.
2. Single and double quotes group text and are removed. Adjacent fragments join.
   Empty quoted arguments are preserved.
3. Backslash quotes the next character outside single quotes, including inside
   double quotes. Inside single quotes, backslash is literal.
4. Unmatched quotes and a trailing backslash are errors.
5. A leading `~/` in each parsed element expands `HOME`, including quoted elements.
   No other variables, globbing, command substitution, redirects, or pipelines
   are evaluated. `;`, `$HOME`, and `$(...)` remain literal arguments.
6. Every `{image}` is replaced after splitting and tilde expansion, including
   inside an argument. A replacement is not scanned again. Spaces and quotes in
   the PNG filename therefore stay in the same argv element.

Executables must resolve to absolute trusted paths: regular executable files
owned by root or the current user with no group/other write permissions, under
trusted parent directories. Relative executable paths containing `/` are
rejected; use absolute paths or `~/...`. Bare names search absolute trusted PATH
directories and the usual system/Nix paths. Empty and relative PATH entries are
ignored. The child receives the existing desktop environment allowlist, plus
the filtered PATH; arbitrary environment variables and loader injections are
not inherited.

Commands run as argv. The existing fixed `supervised_process` shell supervisor
reports the exit status even under Hyprland's `SA_NOCLDWAIT`; it does not evaluate
the configured command as shell code. Child file creation uses umask 077. Stdin
is `/dev/null`. Stdout is capped at 8 MiB; stderr is capped at 64 KiB. Timeout,
output overflow, and completion terminate the process group, including ordinary
descendants. As with other group-based supervision, a child that deliberately
creates a new session is outside that group. Failures return no partial results.
Process errors include the first stderr line, truncated to 1024 bytes.

The caller supplies a readable SDR PNG. The future capture action owns its
owner-only temporary PNG and must remove it after the call. The engine never
removes a caller-owned input file.

## Reading order and API

`joinOcrText()` sorts polygon area centroids top-to-bottom, then groups adjacent
items whose vertical bounding boxes overlap by at least half the smaller height.
Each group retains the common vertical overlap to avoid merging successive
lines through a chain of tall words. Items in the group sort left-to-right by
centroid. Groups join with newline; items join with a space unless existing
boundary whitespace already separates them, or both boundary characters are
CJK. A text-only result preserves the original stdout.

Public entry points:

```cpp
std::string validateOcrConfig(const OcrConfig&);
bool setOcrConfigValue(OcrConfig&, std::string_view key, std::string_view value, std::string& error);
OcrCommand splitOcrCommand(std::string_view command, std::string_view image);
OcrResponse runOcrEngine(const std::filesystem::path& png, const OcrConfig&);
OcrResponse parseOcrJsonLines(std::string_view);
OcrResponse parseTesseractTsv(std::string_view);
OcrResponse parsePaddleOcrJson(std::string_view);
bool ocrSupportsTextLayer(const std::vector<OcrResult>&);
std::string joinOcrText(const std::vector<OcrResult>&);
```

`CaptureDefaults::ocr` carries all six keys. `OcrResponse` contains `results` or
a nonempty `error`; its boolean conversion tests success. The standalone static
target `hyprcapture-ocr` supplies the complete engine and process dependencies.
It is not linked into the UI/plugin until the future action wiring.

## Offline tests

The main CMake build registers five deterministic suites and two live smoke
checks. A smaller project uses the exact same library/test targets without Qt,
Hyprland, audio, HDR downloads, or other engine-independent dependencies:

```sh
env -u QT_IM_MODULE LANG=C.UTF-8 LC_ALL=C.UTF-8 capmem 8G cmake -S tests/ocr -B build-ocr
env -u QT_IM_MODULE LANG=C.UTF-8 LC_ALL=C.UTF-8 capmem 8G cmake --build build-ocr -j2
```

Run ctest from an isolated runtime/DBus environment, as with the repository's
other tests. Apply the isolated environment **after** `capmem`, because `capmem`
itself connects to the host user scope bus:

```sh
ocr_runtime=$(mktemp -d "$PWD/build-ocr/runtime-XXXXXX")
trap 'rmdir "$ocr_runtime"' EXIT
env -u QT_IM_MODULE LANG=C.UTF-8 LC_ALL=C.UTF-8 capmem 8G env \
  QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="$ocr_runtime" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=$ocr_runtime/no-bus" \
  ctest --test-dir build-ocr --output-on-failure
```

All deterministic engine tests use executable scripts created in the build
directory. When `bwrap` is installed, fixture suites remount that directory in
a synthetic Nix store inside a network-isolated namespace. This keeps the
production trust checks intact even when a checkout's ancestor is writable.
Only the build directory is writable on the host. Without `bwrap`, use a build
directory whose entire ancestor chain passes `trusted_path` checks. The adapter
suite checks the fake executable's trust before launching it, preventing an
accidental fallback to a real engine.

Live checks return 77 unless `HYPRCAPTURE_OCR_LIVE=1`, and
skip when the engine is absent. The PaddleOCR smoke check prepends
`~/data/model/.venv/bin` to PATH. These optional checks invoke real engines and
may require models already installed locally; the default offline suite never
downloads models or invokes a real engine.
