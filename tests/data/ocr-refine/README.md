# Offline OCR refinement fixtures

Rendered calibration images and recorded PP-OCRv6 service responses. No fonts,
font paths, hostnames or private screenshots are included. This directory is
self-contained; tests do not read the untracked reference directory.

- Fit: sans 24 px, serif 54 px.
- Diagnostic holdout: WenQuanYi 38 px.
- Validation: PingFang 31 px (the original service prototype's final validation
  split; this is recorded validation, not a new blind application evaluation).
- Additional geometry/load inputs: mixed, dense (18 lines), perspective skew.
- `ground-truth.json`: original nonzero-alpha glyph ink boxes, half-open pixels;
  only the four selected images, with font-file metadata removed.
- `vendor.json`: matching original service output, no service-side refinement.
- `prototype.json`: matching original prototype output, already ink-calibrated.

Coordinates in recorded responses are rounded to four decimals. Scoring uses
NFKC token matching, ignores spaces, matches glyph ink by line and reading order,
and measures both horizontal edges. ASCII tokens count as Latin; other tokens
count as CJK, including punctuation, as in `compare_service.py`.

The test prints all/fit/holdout/validation tables for each source. Estimated
before/after uses the same generated tokenization and 1:2 grapheme initializer.
The timing table measures 101 warm dense calls, including per-line allocation,
rectification and fallback alignment, excluding PNG decoding, response copying
and OCR. CTest gates the median at 5 ms; p95 and maximum retain scheduler noise.
