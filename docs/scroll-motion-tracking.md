# Native text registration and input priors

Scroll amounts are evidence about likely motion, never document coordinates.
Browsers apply zoom, acceleration, smooth scrolling, overscroll and independent
sticky layers; they can also repaint or move without any input.

## Pixel search

Registration preserves native horizontal resolution throughout candidate
generation, including retained-history reacquisition. There is no 320-pixel
resizing stage. A fast path hashes complete informative native row gradients
across the entire vertical search interval, checks hash hits across every native
pixel and compares support across competing offsets. Beside independent side
chrome, adjacent native columns cover every horizontal pixel to nominate
candidates, which require a broad native body proof. A periodic prefix cannot
nominate a prior-selected candidate before the distinguishing column is read. Blank/constant rows cannot vote. A fallback
uses adjacent 256-pixel native column templates across the full width and native tile verification. A native row-proven contiguous overlap can exclude fixed or newly appearing edge chrome without shrinking the vertical search first. The prior
centres candidate evaluation without removing the rest of the search interval.

Multiple comparable native peaks remain ambiguous unless exactly one supported
peak falls inside a calibrated input interval. The tracker does not choose the
smallest offset or a period arbitrarily. Broad native pixel agreement prevents
periodic layout/prefixes being mistaken for a fixed sidebar or zero motion;
independent fixed layers retain their existing motion/source handling.

## Capture-bound input

The plugin accumulates input only after forwarding it, in separate units:
wheel v120 ticks, finger distance, continuous-axis distance and legacy wheel
distance. Each source has signed amount and absolute travel, plus the monotonic
time of the last delivered input. Absolute travel exposes direction reversals
which cancel in the signed amount.

The totals are snapshotted when the framebuffer is captured and travel through
the same PBO metadata slot, sender and receiver queue as its pixels. Subtracting
cumulative snapshots preserves input across discarded intermediate frames.
Queued wheel input is not counted until dispatched. Browser-generated momentum
without new axis events is observed as pixel motion, not fabricated input.

An optional 168-byte HCSF v2 header carries the snapshot; ordinary recording keeps
the 96-byte v1 header. The UI opts in with `motionMetadata:true` in its scroll
start request. A new plugin therefore remains compatible with an older helper;
a new helper also accepts old v1 frames without a prior. Completely identical images cannot distinguish a whole-period scroll from a clamped edge; pending input is retired after settling without adding rows or training. Size/version/reserved
bytes, finite totals, travel bounds and capture/input timestamp ordering are
validated before use.

## Session model and trust

A fresh stitcher learns a separate signed pixels-per-input-unit ratio for each
source. Calibration uses independently pixel-confirmed displacement summed over
a burst and completed after input/pixels settle. This accommodates render lag;
an unchanged frame immediately after dispatch does not consume pending input.
Zero movement at an edge does not train a zero gain, and long uncommanded finger flings cannot inflate one; discrete wheel bursts include their full smooth-scroll animation. Reversed or mixed-source
bursts, rejected frames and prior-selected ties cannot train the model.

At least two settled samples are required for a calibrated input prior. Recent
samples' spread sets its uncertainty. A strongly conflicting or insufficiently
precise prior leaves repeated pixels ambiguous. A unique native pixel match
wins even when it contradicts the input estimate. Recent velocity can predict
an easing tail for diagnostics/search ordering, but alone cannot resolve a
periodic tie: an unobserved rebound could reverse direction.

No accessibility service is required. The model deliberately leaves early,
uncalibrated or intrinsically repeated motion unresolved rather than inventing
rows. Insufficient source overlap retains the previous confirmed output.

## Diagnostics and tests

With timing enabled, `scroll.tracking` records measured offset, confidence and
reason. `scroll.prior` records source deltas, source gains, predicted offset,
uncertainty, whether a tie used the prior, capture time and last-input time.
Uncalibrated predictions are `nan`, not a falsely confident zero.

`scroll-text` covers 3072-pixel-wide monospace logs, code and chat rows whose
layout is uniform and only glyphs differ (including a single small glyph per row): tiny/fast steps, reverse motion, exact
output, true-period rejection, calibrated tie resolution, conflicting-prior
rejection and unique pixels overriding a wrong prediction. `scroll-motion`
covers render lag, dropped-frame totals, source isolation, rebound, page-edge
zero motion and prevention of self-training. Wire tests preserve the legacy
golden header and exercise optional snapshots, malformed packets and PBO
lineage. Existing sticky/header/FAB/native/HDR suites remain required.

A 3200-pixel fixture uses mirrored b/d glyphs with identical area in each coarse bin: both 320-pixel frames are exactly equal, but native registration still finds the 240px scroll, including a +1 luminance shift. Browser-frame fixtures add a fixed toolbar/footer and a mid-scroll compact header at480px steps.

Repeated native rows are verified once against a canonical row per hash bucket;
collisions retain per-hit native checks. This avoids quadratic pixel comparison
cost on uniform text without dropping candidates. A locked, independently
verified stationary body skips redundant motion retries, while retaining late
loading repair. Side-lane motion evidence prevents short sticky labels from
creating a spurious large bottom mask before column pinning completes.

Correlation-only candidates require at least a quarter-viewport overlap
(minimum 96px): a repeated card heading in a tiny overlap cannot establish a
large reverse jump. Short native overlaps need dense vertical detail (such as
photographs or barcodes), or a calibrated input interval supporting that native
peak. Sparse repeated card headings alone cannot prove a large jump. Illumination regressions exercise rejection and native reacquisition
without corrupting text coordinates.
