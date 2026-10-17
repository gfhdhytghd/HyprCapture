# HDR annotation layers

The editor keeps a frozen native HDR child surface beneath the Qt window.
Normal annotations are drawn by the existing transparent Qt canvas, using the
compositor's ordinary SDR/reference-white treatment. Changing a normal stroke
neither snapshots an HDR renderer nor queues a worker or native HDR upload.
The canvas invalidates the old/new stroke bounds instead of the whole window.
Moving/zooming the view changes native viewport geometry without reattaching
unchanged pixel storage.

The isolated HDR edit base is converted once on a worker. At the original
capture position, ready HDR desktop surfaces remain visible during this work;
other cases show the already available SDR image immediately. Ink and input
are available while conversion runs. Once ready, the background is replaced
with a synchronized child/parent commit. Its blank PQ frame also seeds the
exceptional-annotation cache, avoiding another cold base conversion.

## Tool behavior and color semantics

- Normal gain-1 shapes, pen, highlighter, number and committed text use Qt ink.
  Transparent overlaps follow the same SDR Qt composition as the SDR editor.
- Spotlight is a black alpha mask. It never samples the image; multiplying its
  zero RGB by HDR gain changes nothing. Moving it only invalidates the old/new
  holes; its unchanged outside mask is reused.
- Mosaic samples the SDR source and keeps the existing 203-nit linear promotion.
  Non-unit annotation gains likewise require extended linear channels. If either
  occurs, a separate HDR composite child above the immutable base uses the
  original exact renderer. Qt ink is hidden only while that composite is ready,
  avoiding duplicate annotations. Undoing/removing all exceptional ink restores
  the Qt layer and reuses the frozen base.

Qt preview white follows the output reference (173 nit on the investigated
DP-4). The unchanged export renderer uses its historical 203-nit annotation
white multiplied by hdrGain. Preview/reference-white behavior and SDR overlap
blending therefore deliberately differ from the original all-HDR preview.
Switching to an exceptional composite restores that original preview behavior.
Export pixels, alpha, order, linear blending and gains are unchanged by layer
selection. No HDR content is flattened into the SDR layer during export.

## Incremental native transport

Each updating PQ surface owns at most three persistent memfd/mmap slots with
reusable wl_shm buffers. `wl_buffer.release` controls writable ownership.
Busy storage is never overwritten or resubmitted after hide/show until its
old release is dispatched. Each slot accumulates changes since its last use;
updates dropped while all slots are busy are also accumulated. A released slot
is repaired from the latest complete PQ snapshot and submitted with
`wl_surface.damage_buffer`. A new/changed-size slot requires one full copy;
steady frames copy only damage. Geometry-only commits do not reattach pixels.
Mappings survive surface/pool teardown while a compositor buffer retains them.

Cold allocations, large/global edits, layout/source changes, or the first uses
of exceptional HDR slots can still be expensive. HDR composite snapshots may
also detach a full cached image if a pending consumer retains an older one.
This path is a bounded correctness fallback, not a universal 60-fps promise.
Normal ink avoids it entirely. Three 6K PQ slots occupy about 243 MiB per
exceptional surface when all three have been used; the immutable base needs
only its first slot. Buffers are retired on resize/close.

## Reproduction and evidence boundaries

Build with tests enabled. Run `hyprcapture-hdr-editor-benchmark` under
`QT_QPA_PLATFORM=offscreen`, `LANG=C.UTF-8 LC_ALL=C.UTF-8`, with `QT_IM_MODULE`
unset. Options `--interaction`, `--layered`, `--sdr`, `--spotlight` and
`--hdr-fallback` exercise the legacy allocation path, Qt ink, SDR comparison,
Qt Spotlight and the production shm pool respectively. `QT_SCALE_FACTOR=2`
models the investigated output's physical backing-store size.

Offline totals cover input dispatch, worker/GUI round-trips when needed,
render/conversion, actual memfd mapping and page copies, or Qt offscreen
painting. They exclude native protocol dispatch, compositor texture upload and
physical presentation. The fallback benchmark simulates two in-flight frames
with deterministic releases; initial allocations are reported separately.
Legacy close/free cost can move to the server in a real Wayland session because
the server retains a file reference, unlike the isolated allocator benchmark.

### Measured CPU costs

Measured on a Core Ultra 7 265K, Qt 6.11.2/GCC 16.2.1, with a
6144×3456 image and 3072×1728 logical canvas (`QT_SCALE_FACTOR=2`). Each
row has 12 changing frames; times are medians in milliseconds. The baseline
uses the parent revision 4d0d6f9 with the measurement harness added. Cold HDR
base conversion and initial fallback pool allocation are excluded from these
steady interaction rows and reported below.

| CPU stage | Previous PQ preview | Normal Qt ink | HDR gain fallback |
| --- | ---: | ---: | ---: |
| Worker queue | 0.020 | — | 0.011 |
| Render + PQ conversion | 0.949 | — | 0.903 |
| GUI queue | 5.939 | — | 5.083 |
| Allocate/map | 0.019 | — | 0 per-frame allocations |
| Copy / pool prepare | 30.581 | 0 HDR bytes | 0.016 |
| Unmap/close | 4.921 | — | 0 per-frame operations |
| Total through CPU submission / Qt paint | **42.000** | **0.023** | **6.033** |
| HDR bytes copied per frame | 84,934,656 | 0 | 98,416 |

Medians of individual stages need not sum to the median total. Normal Qt ink
cost includes localized offscreen painting; its first full paint was 6.912 ms.
Small-hole Spotlight median was 0.061 ms (first paint 9.965 ms). The unchanged
SDR editor's full raster paint measured 19.146 ms with this fixture. These
differences reflect paint area as well as HDR transport; they are not claims
about physical frame latency or a corresponding frame-rate multiplier.

The fallback prewarms three real shm slots, then models two in-flight frames.
Their initial allocation/map/full-copy cost was 104.777 ms total, so cold slot
use can still stall the GUI. In a separate trace-enabled run, cold background
conversion took 168.272 ms on the worker; warm fallback damage rendering and
PQ conversion medians were 0.530 and 0.373 ms respectively. Ordinary layered
Qt paint was 0.017 ms and input-dispatch-to-paint was 0.007 ms in its trace
run. Trace-enabled numbers are separate samples from the table.

Actual attach/commit, compositor upload, release cadence and presentation
latency remain unmeasured for the new helper. The production probes below
cover them where supported; an offscreen run cannot supply those results.

`HYPRCAPTURE_TIMING=1` enables stderr traces. Alternatively use
`HYPRCAPTURE_TIMING_FILE` with a private runtime path accepted by the helper.
The following production trace stages allow acceptance after updating:

- `editor.input_dispatch_to_paint`, `editor.canvas_paint.layered/composited`;
- `overlay.hdr_edit_worker_queue`, `overlay.hdr_edit_main_queue`;
- `hdr.preview.render_damage`, `hdr.preview.convert_damage`;
- `hdr.shm_allocate_map`, `hdr.shm_copy_damage`, `hdr.shm_create_protocol_buffer`;
- `hdr.attach_commit_flush`, `hdr.commit_to_present` for native child updates;
- `editor.parent_frame_queued` / `editor.parent_presented` for Qt ink on the
  parent surface, using the advertised Wayland presentation clock.

Parent timing covers paint-request-to-presentation, not hardware input latency.
Discarded/coalesced frames are logged; unsupported presentation feedback gives
no presentation evidence. Timing probes are opt-in and create no extra window.

Portable tests cover layer selection/transparency, unchanged export, gain,
exact cropped PQ frames, accumulated damage, held-reader immutability and
pool lifecycle. The explicit `hyprcapture-hdr-export-fingerprint` target permits
cross-revision byte comparisons in the same Qt environment, without pinning
platform-specific rasterizer hashes in CTest. Live smoothness, color/overlap
appearance, layer ordering across asynchronous readiness, and mixed-monitor
scaling require user acceptance of the updated helper. No runtime plugin reload
is performed by this work.
