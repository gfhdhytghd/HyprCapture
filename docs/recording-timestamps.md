# Compositor recording timestamps

Normal video recording now sends RGBA frames in a NUT stream with monotonic
capture timestamps. FFmpeg preserves those timestamps (`fps_mode=passthrough`)
rather than producing a constant-rate stream by duplicating input frames.

The old scheduler represented a missed interval by increasing the repeat count
on the next queued picture. The worker transferred and encoded each repetition.
When encoding fell below the requested rate, those repeats consumed the two
queue slots and suppressed new captures, which caused still larger repeat
counts. The new video path queues one sample for each accepted capture. Queue
capacity remains bounded; a full queue skips a capture opportunity without
accumulating work for the missing intervals.

Window PBO slots now carry the timestamp of their render, rather than the time
when the CPU eventually maps them. Startup discards dimension-probe PBO samples
before taking the initial frame. Already presented PBO warmup samples are ignored.
Video PTS and audio trimming share that initial frame's monotonic clock. Resizing
still fits pixels into the fixed recording canvas.

At stop, at most one terminal sample holds the last picture until the end time.
Its position is the stop timestamp minus one nominal frame duration; it is not
an instruction to encode all the missed frames. B-frames are disabled on this
path because reordering sparse timestamps through software H.264 was observed
to produce an incorrect MP4/MOV duration. GIF, APNG intermediates and WebP retain
their existing animation timing. Encoder selection, including automatic HEVC
and software fallback, is unchanged.

## Verification

`hyprcapture-recording-timestamps-test` exercises the real NUT writer and FFmpeg
for MP4, MOV, MKV/FFV1 and WebM/VP9. Five differently colored samples at 0, 0.033333,
0.2, 0.75 and 1.5 seconds, followed by a terminal sample, remain six encoded
frames with the correct pixels and a 2-second duration. A real sound-finalize
run verifies that an audio pulse aligns with video t=0.75 after trimming a
0.5-second audio lead, and preserves the video bitstream.

A manual HEVC NVENC test at 5204×3356 preserved those same six timestamps and
2-second duration and passed decoding. An isolated 5-second producer/encoder
queue test at that size accepted 227 samples (45.4 samples/second), produced a
5.000059-second HEVC file and drained in 0.154 seconds. The earlier repeat-queue
reproduction accepted only 20 new samples in approximately five seconds and
needed 3.017 seconds to drain. These are local synthetic transport/queue results,
not end-to-end window-capture frame rates; compositor rendering, PBO readback,
and image postprocessing were not included.

After the user reloads the plugin, real window recording still needs verification
for motion, resize, backgrounds/decorations, capture stalls, stopping and sound sync.
This change does not remove the existing CPU readback or guarantee 60 fps.
