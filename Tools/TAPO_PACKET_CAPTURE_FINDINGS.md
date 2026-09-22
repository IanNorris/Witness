# Tapo live-output packet capture findings

Capture date: 2026-09-22. Diagnostic build: `f54e0ac`.

## Result

The camera 9 preview input payload is valid, but Witness discards 134 of 395
unique H.264 access units (33.9%) because the source DTS periodically regresses.
The unmodified input packets decode cleanly in arrival order. This identifies
the live mux timestamp/drop policy—not damaged encoded input—as the direct
cause of this preview stream's missing frames and stutter.

## Captures

Raw captures are retained locally under `X:\WitnessCache\packet-captures`:

- `camera-9-preview-1790114769791-1`: 20 seconds, 395 video packets. 261
  written; 134 rejected as `nonMonotonicInput`. All 395 payload hashes are
  unique; no rejected payload duplicates a written payload. The capture queue
  rejected zero records and every stored binary hash verifies.
- `camera-9-main-1790115322763-3`: 20 seconds, 400 video packets. All 400
  written; no negative DTS deltas. This tier is healthy.
- `camera-8-main-1790115094785-2`: 20 seconds, 334 video packets. All 334
  written; no negative DTS deltas. This tier is healthy during the sample.

Camera 8's preview stream could not be sampled because the camera repeatedly
rejected that RTSP connection with `Operation not permitted`.

## Camera 9 preview clock

The preview declares a 90 kHz timebase, 4,500-tick packet durations, no PTS/DTS
reordering, and approximately 20 fps. Arrival order spans 19.798 seconds and
declared durations total 19.750 seconds, so its long-term cadence is sound.

However, DTS steps backwards 18 times in the 20-second capture—roughly once per
second—by 24,586 to 35,206 ticks (273–391 ms). Witness retains the previous DTS
high-water mark and rejects the next 6–8 unique frames while the source catches
up. This repeats throughout the capture.

The reconstructed `input-arrival-order.h264`, which includes every captured
access unit, decodes with no FFmpeg warnings. Input packet hashes are unchanged
at the live-mux decision point. Therefore the capture shows no payload mutation
between demux and mux policy.

## Fix direction

Do not merely clamp an individual regressing timestamp while retaining the raw
clock afterward; that would still compress or duplicate output intervals. For
this no-B-frame Tapo preview signature, preserve arrival order and synthesize a
continuous output DTS/PTS from the declared 4,500-tick cadence, while retaining
the existing guarded long-window drift test. The repaired stream must accept
all unique packets and should be checked for:

- monotonic output timestamps;
- approximately 20 seconds of output for a 20-second capture;
- zero packet loss at each one-second DTS sawtooth;
- clean FFmpeg decode of both captured input and remuxed output;
- no behavior change for camera 8 main, camera 9 main, Reolink, or genuine VFR
  streams.

## Implemented repair

The live preview mux now keeps the existing behavior until it observes at least
eight consecutive duration-matched packets spanning 250 ms. A backward DTS can
activate normalization only when all of these conditions hold:

- the stream is H.264 or H.265 without B-frames;
- PTS equals DTS and the declared duration remains within 10% of the qualified
  cadence;
- the regression is strictly backward, no larger than one second or 20 frames;
- the payload hash does not match any of the last 32 written video packets.

Once qualified, the access unit remains byte-for-byte unchanged and the output
DTS/PTS advances from the previous output duration. Long-window cadence checks
remain active and can reject normalization if the source and duration clocks
subsequently drift by more than 5%. The automatic path is enabled for preview
streams only; established Reolink main-stream and recording behavior is
unchanged.
