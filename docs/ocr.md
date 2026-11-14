# OCR engine layer

The annotation editor exposes **Live text (O)** for whole-image recognition.
`text_layer` and `both` provide a selectable text layer; `capture` copies all text
on one click. No standalone Lua OCR action or capture-overlay toolbar button is
registered. `shot` sessions hide the editor tool. Recognition uses the captured
SDR base image at physical resolution, without annotations, on a cancellable
worker. Its private PNG is deleted before results are delivered and on cancellation.
Closing the editor or turning the layer off kills the engine process group.

HyprCapture does not link an OCR library. The shared engine is Qt-free and runs
external programs synchronously; compositor and UI callers must use a worker.

## Configuration

| Key | Default | Values |
| --- | --- | --- |
| `ocr_mode` | `off` | `off`, `capture`, `text_layer`, `both` |
| `ocr_engine` | `tesseract` | `tesseract`, `paddleocr`, `command` |
| `ocr_languages` | `eng+chi_sim` | Nonempty, at most 4096 bytes, no NUL |
| `ocr_command` | empty | At most 65536 bytes, no NUL; required when running `command` |
| `ocr_output` | `text` | `text`, `json`; used only by `command` |
| `ocr_timeout` | `30` | Integer seconds, `1..86400` |
| `ocr_api_url` | empty | Full PaddleOCR HTTP endpoint; empty uses the CLI |
| `ocr_api_key` | empty | Optional API key; stored in the Hyprland config in plain text |

`off` hides the editor tool and makes the engine API fail. `capture` copies
`joinOcrText()` for the entire image; `text_layer` and `both` allow text selection
with polygons. A command returning plain `text` falls back to copying all text.
The plugin warns once per configuration change; the editor supplies a fallback
tooltip and a first-successful-click toast. Results are cached per base image.

The agreed design does not specify numerical timeout or string limits. The
implementation adds the defensive bounds above. OCR fields in old session JSON
are optional and retain these defaults. Invalid present fields fail decoding.

## Unified results

With `ocr_engine=command` and `ocr_output=json`, stdout is JSON Lines:

```json
{"text":"Hello OCR 123","poly":[[9,32],[198,32],[198,57],[9,57]],"conf":0.99,"level":"line"}
{"text":"中文 OCR","poly":[[10,70],[200,70],[200,95],[10,95]],"conf":0.98,"words":[{"text":"中","poly":[[10,70],[40,70],[40,95],[10,95]]},{"text":"文","box":[40,70,70,95]},{"text":"OCR","poly":[[90,70],[200,70],[200,95],[90,95]],"conf":0.97}]}
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
- A line may have a `words` array in reading order. Each word has a required
  string `text`, `poly` with the same point/coordinate/winding rules as the line,
  or `box: [x1,y1,x2,y2]` with positive dimensions, and optional `conf` in `0..1`.
  Leading/trailing Unicode whitespace is trimmed; empty words are skipped.
  The word texts must match the line text in order, ignoring whitespace.
  Uncovered spaces and punctuation remain part of the line text.
- Malformed or unmatched word metadata drops that line's entire `words` array
  and records one warning per response; the line remains usable. `level: word`
  records ignore `words` and retain their previous behavior.
- Word polygon projections set selection intervals along the line direction.
  Intervals are clamped to the line and kept monotonic. The existing character
  width model distributes graphemes within each word and uncovered text within
  each gap. The estimate is exactly narrow/Latin `0.5` to wide/CJK `1.0` (1:2),
  including whole lines without valid words; this replaces the previous `0.55`.
  Selection uses analytic quads in the line frame. Without words,
  selection uses proportional estimates with that same 1:2 ratio; copying still
  uses the line text. A response can mix lines with valid words, missing words,
  and invalid words. Each line independently uses its valid words or estimates.
- Unknown fields are ignored. JSON nesting is limited to 32 levels.

The input limit is 8 MiB and at most 65536 results. Up to 262144 word entries
(including skipped entries) are accepted across the response. Excess word
metadata is discarded with the same warning; recognized lines are retained.
Each nonempty input line must
be an object; blank lines are errors. An empty stream means no recognition.
CRLF and a final newline are accepted. On any malformed record, the parser
returns no partial results and an error naming the line number.

`command` with `ocr_output=text` preserves all stdout, including final newlines,
as one `OcrResult` with an empty polygon. This representation is permitted only
for that adapter mode. `ocrSupportsTextLayer()` rejects such results. Text
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
by 100. It groups words by page/block/paragraph/line ids and sorts each line by
word id. Each result is a line with the union rectangle, joined text,
mean confidence, and the original word rectangles/confidences in `words`.
Joining uses the same `ocrTextSeparator()` / CJK-boundary rules as copy-all:
adjacent CJK words have no added space, while Latin words and CJK/Latin
boundaries retain their existing separators. Punctuation follows these same
rules. Copy-all text therefore matches the previous separate-word results
(for example, adjacent `中` and `文` become `中文`). Word texts still match within the joined
line in reading order. Nonpositive word dimensions, invalid confidence, and
malformed rows fail with the TSV line number.

### PaddleOCR 3.x

```text
paddleocr ocr -i <png> --save_path <private-directory> --use_doc_orientation_classify False --use_doc_unwarping False --use_textline_orientation False --enable_mkldnn False --return_word_box True
```

The adapter reads `<png-stem>_res.json`, not stdout logs. The saved plain JSON
contains parallel `rec_texts`, `rec_polys`, and `rec_scores` arrays. Each result
retains the line text and `rec_polys` geometry. Optional parallel `text_word`
and `text_word_boxes` arrays attach tokens inside that line's `words`, rather
than emitting separate `level=word` records. Word boxes accept polygons or
`[x1,y1,x2,y2]` rectangles. Whitespace tokens are skipped. Missing word arrays
retain line-only results; malformed metadata uses the soft warning above.
This saved-file format comes from the agreed design's local
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

## PaddleOCR HTTP API

A persistent PaddleOCR service avoids loading the CLI models on every click.
Install `curl` on the trusted PATH, then set the plugin options:

```lua
ocr_engine = "paddleocr",
ocr_api_url = "http://ocr.example.lan:8008/ocr",
ocr_api_key = "",   -- optional
```

Use these entries in your HyprCapture plugin configuration table, or assign
`hl.plugin.hyprcapture.ocr_engine`, `hl.plugin.hyprcapture.ocr_api_url`, and
`hl.plugin.hyprcapture.ocr_api_key` individually. The URL is a full endpoint.
An empty URL retains the existing local CLI behavior. Other engines ignore both
API settings and warn once per configuration change when the URL is nonempty.
The key is stored in the Hyprland config in plain text. API settings accept up to
65536 bytes each and reject NUL, CR and LF.

Both formats are detected automatically; there is no format option:

- **Simple PP-OCR server:** POST multipart/form-data with `file`, filename
  `image.png`, type `image/png`, and `return_word_box=true`. A nonempty key adds
  `Authorization: Bearer <key>`. The response has a `results` array of
  `{ "text": "...", "confidence": 0.99, "box": [[x,y], ...] }`.
  `poly`/`bbox` alias `box`; `score` aliases `confidence`.
  Each result may include `"words": [{"text":"hello", "box":[[0,0],[50,0],[50,20],[0,20]], "conf":0.98}]`.
  Words also accept `poly` or `box: [x1,y1,x2,y2]`. Servers that ignore the new
  request field and return no words retain line-only behavior.
- **Official PaddleOCR/PaddleX serving (including Baidu AI Studio):** POST JSON
  with base64 PNG in `file`, `fileType: 1`, and
  `useDocOrientationClassify`, `useDocUnwarping`, `useTextlineOrientation`,
  `visualize` all false, plus `returnWordBox: true`. A nonempty key adds `Authorization: token <key>`.
  Recognition comes from `result.ocrResults[].prunedResult` with `rec_texts`,
  `rec_polys`, `rec_scores`. All pages are concatenated. If `rec_polys` is absent,
  `rec_boxes` supplies `[x1,y1,x2,y2]` rectangles. Nonzero `errorCode` reports
  `errorMsg` as an error. Optional `prunedResult.text_word` contains token
  arrays per line, paired with `text_word_boxes` in the same order, for example
  `"text_word": [["中", " ", "文"]]` and
  `"text_word_boxes": [[[0,0,10,20], null, [10,0,20,20]]]` for one line.
  Whitespace tokens are skipped before validating their geometry; each
  remaining token becomes a nested word of the matching `rec_texts` line.

An exact-URL cached format takes priority. Otherwise, AI Studio hosts
(`aistudio-app.com`/`aistudio.baidu.com`, including subdomains) and endpoint paths
ending in `/layout-parsing` try the official format first. Other endpoints try
the simple format first. HTTP 400/415/422 or successful JSON with neither shape
triggers one retry with the other format, after the 422 field probe below. Network errors, timeouts, 401/403,
5xx, malformed JSON, invalid recognition data and nonzero `errorCode` are real
errors and do not trigger a format retry. An `errorCode` response explicitly
naming the optional word-box parameter can still use the parameter retry below.
A stale cached format that mismatches
is removed, and the other format is tried next without repeating the URL guess.
HTTP 400/422 in either format triggers one retry of the same format without
its optional word-box parameter (`return_word_box` or `returnWordBox`), including
servers whose error does not identify the field. A successful retry remembers
this exception per exact URL and request format, so later calls send only the
line-recognition parameters. A rejected word-box parameter does not become an
OCR error when line-only recognition succeeds. Ordinary FastAPI
form parameters ignore extra fields; explicitly configured `extra="forbid"`
form models can reject them (see the [FastAPI documentation](https://fastapi.tiangolo.com/tutorial/request-form-models/#forbid-extra-form-fields)).
If that retry also mismatches, format detection continues. Each of two formats
gets at most one parameter retry (four requests total). All probes
share one `ocr_timeout` budget.
Cancellation kills the active curl process group.

Success caches the request format in the helper process and in
`$XDG_CACHE_HOME/hyprcapture/ocr-api-format.json` (default
`~/.cache/hyprcapture/ocr-api-format.json`). This owner-only 0600 file maps the
SHA-256 of the exact URL to `"simple"` or `"paddlex"`; an optional sibling key
`"<hash>:no_word_box": {"recorded_at": 1800000000}` remembers a rejected optional
field for the cached format, with its Unix timestamp in seconds. It expires
exactly 24 hours after rejection. Calls within that interval keep the original
timestamp; they do not extend the lifetime. The next OCR after expiry requests
word boxes again. Success with the parameter clears the rejection flag.
Existing string entries remain compatible. A legacy rejection flag without a
timestamp expires immediately; older readers ignore the added flag keys.
The file stores no URL or key. The helper keeps an in-memory copy and reloads
it when the file's inode, modification time or size changes, including an
external atomic rewrite or deletion. Clearing the cache takes effect on the
next OCR without restarting the editor/helper. An unwritable or unsafe cache
does not prevent recognition.

API geometry uses floating input-pixel coordinates and the same polygon,
winding, confidence, nesting, 65536-result, 262144-word and 8 MiB response limits
as JSONL. Invalid word metadata preserves the corresponding line and adds one
warning per response.
API results have `level=line`. Empty results mean no recognition, not an error.
The shared worker remains Qt-free. OpenSSL libcrypto supplies SHA-256 and base64.
URL, headers and curl options travel through stdin; the official JSON body is a
0600 file inside the existing 0700 OCR temporary directory and never an argv
value. Files are removed after completion or cancellation. API settings also
travel from the plugin to the helper through the private session JSON, not argv.
HTTP is permitted for LAN services. HTTPS uses normal certificate verification;
only HTTP/HTTPS protocols are enabled. Redirects are reported as HTTP errors.
No proxy settings are added or changed.

### Cache commands and immediate probes

`hyprcapture-ocr` is a Qt-free companion installed beside `hyprcapture-shot` by
CMake, Nix, and the Omarchy recipe. It manages both the detected request format
and the 24-hour rejected-parameter flag in the cache file described above.

```sh
hyprcapture-ocr cache show
hyprcapture-ocr cache show --url http://ocr.example.lan:8008/ocr
hyprcapture-ocr cache clear
hyprcapture-ocr cache clear --url http://ocr.example.lan:8008/ocr
hyprcapture-ocr probe
hyprcapture-ocr probe --url http://ocr.example.lan:8008/ocr --key-file /private/ocr-key
```

- `cache show` prints short URL hashes, detected formats, the word-box parameter
  flag, and its age/expiry. `--url` marks the matching entry with `*`; it does
  not print the URL. Old string-only cache files remain readable.
- `cache clear` atomically removes all entries under the existing cache lock.
  `--url` removes only that URL's format and rejection flag. It prints the
  removed hashes and succeeds even when there are no entries.
- `probe` ignores both cached decisions and runs normal detection through the
  real OCR worker on a built-in PNG reading "OCR 123". It prints the format,
  observed word-box availability, line/word counts, and latency, then caches
  the fresh successful result. "Not returned" covers servers that accept the
  parameter but supply only lines; selection still uses proportional estimates.
- Without `--url`, `probe` reads `plugin:hyprcapture:ocr_api_url`, `ocr_api_key`,
  and `ocr_timeout` via `hyprctl getoption -j`. With `--url`, it uses that endpoint
  and the default timeout. `--key-file` overrides the configured key and requires
  a regular owned 0600 file. The key never appears in argv or command output.
- Each command accepts `--help`. Usage errors return 64; failed operations
  return a short reason and a nonzero status. The format cache keeps its normal
  invalidation rules; only rejected-parameter flags expire automatically.

After changing or upgrading your OCR server, run `hyprcapture-ocr cache clear`
or `hyprcapture-ocr probe` to re-test immediately.

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
is `/dev/null` for CLI engines; HTTP mode reads its private curl config. Stdout is capped at 8 MiB; stderr is capped at 64 KiB. Timeout,
output overflow, and completion terminate the process group, including ordinary
descendants. As with other group-based supervision, a child that deliberately
creates a new session is outside that group. Failures return no partial results.
Process errors include the first stderr line, truncated to 1024 bytes.

The caller supplies a readable SDR PNG. The editor worker owns its
owner-only temporary PNG and must remove it after the call. The engine never
removes a caller-owned input file.

## Reading order and API

`ocrReadingRows()` sorts polygon centroids in a common run frame, groups
items whose v-axis extents overlap by at least half the smaller height, and
orders items along u. Each row retains its common overlap to prevent a chain
of tall words from merging successive lines. Different orientations (>10°)
remain separate rows. This also supports rotated and vertical text.

`ocrCopyText()` is the shared Qt-free copy formatter. It returns formatted text
and one `OcrCopyRow` per reading row: unchanged in-row text/result indices, the
separator after the row, a reconstructed indentation prefix, and the CODE flag.
The final row has no separator. `joinOcrText()` returns this formatted text;
capture, copy-all, and multi-row Live Text selections use the same decisions.
A text-only result preserves the original stdout, including internal newlines.

Paragraph handling follows `ocr_blast`'s single-region strategy:

- Use the upper median row height and upper median positive v-axis gap. When
  no positive gaps exist, use `0.4 * median_height`.
- `para_gap = round(max(1.5 * median_height, 2 * median_gap) * 0.5)`.
  A gap at least this large emits `\n\n`. Widths use u extents; gaps use the
  preceding run's v axis. Different orientations (>10°) conservatively emit
  `\n` instead of joining across incompatible frames.
- Within a prose paragraph, a preceding row narrower than `0.92 * max_width`
  emits `\n`. Other boundaries join soft wraps. Insert one space only if
  neither boundary character is whitespace, ASCII/CJK punctuation, or a Han
  character in the reference `is_cjk` ranges. Existing in-row separators and
  text remain unchanged.

Before soft-wrapping prose, also preserve these structured boundaries as `\n`:

- A new row starts with a list marker: `-`, `*`, `•`, `·`, `+` followed by
  whitespace or CJK; alphabetic markers such as `a.` / `b)` / `A.`; numeric
  markers such as `1.` / `1)` / `(1)` / `１.`; circled numbers ①–⑳; CJK
  enumerations such as `一、` / `（一）`; or checkboxes `[ ]` / `[x]`.
  Leading whitespace is ignored for detection, and OCR-dropped spaces such as
  `-背景`, `a.用` and `a.English` are accepted. In-row text is preserved.
- The new row's left edge changes by at least one character advance, in either
  direction. Compare both edges along the preceding run's u axis, including for
  rotated/vertical text. Use the median word width per grapheme from the new
  row when available, then the preceding row's word estimate, otherwise the new
  row's width per grapheme. Single-character words and legacy `level: word`
  records also supply advances.
- The preceding row ends in `:` or `：`, ignoring trailing whitespace.

These rules keep existing paragraph gaps (`\n\n`). A preceding `。`, `.`, `!`
or `！` alone does not force a break. Ordinary decimals and dotted prose
abbreviations (`1.25`, `e.g.`, `i.e.`) are not enumeration markers.

### Code detection and indentation

Before joining prose, classify entire paragraph blocks and maximal runs of at
least two consecutive code-shaped rows. No font name or editor metadata enters
the classifier: it uses OCR text and geometry only. The final thresholds are:

- At least 85% of non-space characters are ASCII. At least 8% are code symbols
  (assignment, brackets, math, quotes and shell symbols), or a row has a common code shape: assignments, calls, dotted
  names, flags, key/value entries, or code keywords/comments. An unquoted
  sentence after a prose label does not count as a key/value code shape.
- With word boxes, at least three words of two or more graphemes from at least
  two rows establish an advance. Advance CV must be <=8%. At least 90% of
  measurable inter-word gaps must be within 0.20 cells of an integer multiple;
  overlaps beyond 0.20 cells reject the evidence.
- Without sufficient word boxes, per-row advance CV must be <=2%. This stricter
  threshold rejects proportional code-shaped English that passed at 4% in the
  rendered fixtures. Require three distinct rows, a longest/shortest character
  count ratio >=1.4, at least two rows <=40 graphemes, and one <=12; alternatively,
  require distinct rows and positive indentations all within
  0.12 cells of an integer grid. If OCR retains leading spaces but its box
  starts at the first glyph, an alternative advance excluding only those leading
  spaces must pass this same grid check. Long prose averages alone do not prove monospace.
- ICU supplies Unicode grapheme counts, including spaces, without Qt. Candidate
  sub-runs with clearly inconsistent word advances are excluded. Reconstructed
  indentation is limited to 4096 spaces per row; larger estimates decline CODE.

Code symbols (whitespace excluded):

```text
= ( ) [ ] { } < > ; : , . _ / \ " ' ` | & * + - # $ @ % ~ !
```

Detected code retains every internal row boundary (`\n`), and paragraph gaps
remain `\n\n`. Indentation is rounded from `(row_left - block_left) / advance`;
existing leading spaces are not duplicated. Adjacent code paragraphs share
the indentation origin across their preserved blank-line gap. CODE/prose transitions keep a hard
break. No extra newline is appended at EOF.

Live Text keeps its internal single `\n` row offsets, glyph geometry, and hit
positions. Copying replaces only crossed row boundaries with the formatter's
separators and adds the applicable indentation prefixes. Partial first/last
rows retain their selected in-row text; single-row selections are unchanged.

Public entry points:

```cpp
std::string validateOcrConfig(const OcrConfig&);
bool setOcrConfigValue(OcrConfig&, std::string_view key, std::string_view value, std::string& error);
OcrCommand splitOcrCommand(std::string_view command, std::string_view image);
OcrResponse runOcrEngine(const std::filesystem::path& png, const OcrConfig&, std::stop_token stop = {});
OcrResponse parseOcrJsonLines(std::string_view);
OcrResponse parseTesseractTsv(std::string_view);
OcrResponse parsePaddleOcrJson(std::string_view);
bool ocrSupportsTextLayer(const std::vector<OcrResult>&);
OcrCopyResult ocrCopyText(const std::vector<OcrResult>&);
std::string joinOcrText(const std::vector<OcrResult>&);
```

`CaptureDefaults::ocr` carries all eight keys. `OcrResponse` contains `results` or
a nonempty `error`; its boolean conversion tests success. The standalone static
target `hyprcapture-ocr` supplies the complete engine and process dependencies.
The editor links this target through `hyprcapture-live-text`. The compositor only
forwards configuration and never runs an OCR process.

## Offline tests

The main CMake build registers seven deterministic suites and two live smoke
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

## Editor layer and PaddleOCR probe

The text layout uses the same reading rows and in-row separators as the copy
formatter. Copied text uses `ocrCopyText()` for paragraph/code boundaries. Selection offsets are UTF-16 grapheme boundaries. Character
areas interpolate along each polygon's baseline with advance weights 1.0 for
CJK/full-width characters and 0.5 otherwise. Valid nested word boxes bound
those estimates within each word; missing or invalid words retain the 1:2
whole-line estimate. Pointer hit-testing uses the inverse
canvas transform, so zoom, pan, and rotated text retain image coordinates.
Double-click selects a word; triple-click selects its reading-order row.

The local PaddleOCR 3.4.0 probe with `--return_word_box True`, run under
`capmem 6G`, saved `text_word` and `text_word_boxes` in `input_res.json` for
"Hello OCR 123". Word boxes were `[x0,y0,x1,y1]`; the parser also accepts polygon
word boxes and validates parallel array lengths. The probe artifacts and command
output are recorded in this task's result card. This confirms one installed
engine and sample, not every PaddleOCR model or language.

For the complete offscreen suite, build the main project with `BUILD_TESTING=ON`
and run `tests/ocr_test_env.sh ctest --test-dir build --output-on-failure -j1`.
The wrapper applies `capmem 8G` before isolating the runtime/DBus environment,
uses a private mount/network namespace, and confines writable host paths to the
build directory. It supplies trusted disposable clipboard/engine fixtures via a
synthetic Nix store. Run the suite after compilation finishes; image-registration
and HEIC tests have independent per-test timeouts.
