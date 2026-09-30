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

## September 30 follow-up: preview repair rejection

The September 29 health export identifies camera 8 preview freezing at
23:20:11 UTC. Its current epoch contains 301,458 dropped packets and only 111
timestamp repairs; recent events show repeated `nonMonotonicInput` drops and
decode holds. Other visible players' error counters are cumulative and do not
establish a simultaneous failure at this time.

Inspection found that automatic preview repair still uses the damaged source
clock both for phase steering and for the two-minute cadence validation. A
failed validation permanently disables repair until input reconnects. The
export does not retain that rejection log, so this is a code-supported failure
mechanism, not proof of the exact rejection event in production.

For automatically qualified **video-only** previews, the repair now uses steady
arrival time as its reference. Packet durations still create the continuous
output timeline; a bounded 5% correction gently follows arrival cadence rather
than reproducing source-clock jumps. Two-minute cadence validation still rejects
sustained rate mismatch, but compares durations to elapsed arrival time. Audio
streams, Reolink's explicit profile, B-frame streams, and unqualified/VFR inputs
retain their existing policies. Reconnect resets the reference anchors.

`TimestampRegressionGuardTests` covers repeated source regressions beyond two
minutes, arrival jitter, sustained rate mismatch, source-clock fallback, and
reference reset. `LivePreviewProbe` is an opt-in real-input test, not part of
normal builds or automated tests. Set `WITNESS_PREVIEW_PROBE_URL` privately, then
run `LivePreviewProbe 150 output.mp4`; it exercises the actual preview mux,
reports packet dispositions, and writes published fMP4 for independent decoding.
The URL is deliberately not a command argument. Handle the output as sensitive
video. Use the built probe beside the normal runtime DLLs.

Local camera 8 verification: a fresh 25-second direct RTSP sample observed 22
backward video-DTS steps (about 290–420 ms). A subsequent 150-second test through
the actual patched Witness mux accepted 2,478 packets, repaired 124 regressions,
and recorded zero non-monotonic input/output drops, zero mux errors, zero
correction saturation, and no generation change. Normalization remained active
after the two-minute validation boundary. One other packet was dropped; the
initial probe did not export its disposition. The saved output contained 2,477
packets across 149.104 seconds, strictly increasing DTS, and decoded through
FFmpeg with zero errors. This is bounded local verification, not a production
soak test or a claim to resolve all genuine source/network corruption.

## Parallel 15-minute soak: cameras 8 and 9

Both fresh probes completed 900 seconds on September 30 using the same patched
preview mux. No manual Tapo profile or forced normalization was selected. The
probe now supports 10–1,800 seconds, minute-by-minute counters, and an optional
camera ID: `LivePreviewProbe 900 output.mp4 9` (supply that camera's preview URL
privately through the environment). When wrapping the probe, drain stdout and
stderr concurrently: an initial wrapper blocked camera 9 on a full stderr pipe;
those interrupted runs were excluded and both tests restarted with fresh files.

| Metric | Camera 8 | Camera 9 |
| --- | ---: | ---: |
| Accepted video packets | 14,974 | 11,599 |
| Total drops | 1 | 5,851 |
| Established-stream drops | 0 | 5,812 |
| Non-monotonic input drops | 0 | 5,850 |
| Non-monotonic output drops | 0 | 0 |
| Timestamp repairs | 757 | 0 |
| Normalization active at end | Yes | No |
| Correction saturation / mux errors | 0 / 0 | 0 / 0 |
| Output duration | 899.104 s | 899.840 s |
| Independent FFmpeg decode errors | 0 | 0 |

Both outputs have strictly increasing packet DTS and neither probe changed
generation or lost its input connection. Camera 8 passes this bounded soak.
Camera 9 still rejects about one third of incoming packets and repeatedly
enters/exits presentation holds (243 corruption notifications, 242 recoveries).
It never activates the repair, so the next investigation is its qualification
evidence, not the subsequent arrival-clock validation. A clean independent
decode does **not** make its missing frames or highly uneven pacing acceptable:
camera 9 output durations range from 11 microseconds to about 497 ms.

For independent decoding, preserve the demux timebase, e.g.
`ffmpeg -v error -i output.mp4 -an -fps_mode passthrough -enc_time_base:v demux -f null -`.
The default null-output timebase rounds camera 9's near-adjacent timestamps
together and reports duplicate-DTS warnings; those are not H.264 decode errors.
Local captures and the full minute-by-minute report are retained under
`build-vs2026/tapo-camera-{8,9}-preview-15min-drained.mp4` and
`build-vs2026/tapo-preview-15min-results.txt` (ignored, sensitive local artifacts).

## Camera 9 qualification fix and verification

A complete 30-second capture (`camera-9-preview-1790727172803-1`) contained
583 input records and 582 decisions: 370 written, 174 non-monotonic drops,
37 waiting for the first keyframe, and one missing-DTS startup packet. All
582 timestamped payloads were unique, declared 4,500-tick/50 ms durations, and
had PTS equal to DTS. Input decoded cleanly without B-frames; binary hashes
verified and the capture writer rejected zero records.

Source deltas alternated around 35–45 ms and 70–80 ms. The mux's source-DTS
lookahead substituted these deltas for duration before qualification. Replaying
the actual C++ guard reached only two stable samples (eight required), and all
174 drops had zero qualifying evidence. Thus the guard never activated repair.

The new video-only preview path measures a bounded rolling arrival window
independently of DTS, requires at least one second of declared-duration
evidence, and retains the existing no-B-frame, PTS/DTS, unique-payload and
bounded-regression checks. Declared durations stay anchored to one value (one
tick of rounding is allowed), preventing a gradual VFR ramp from qualifying.
Arrival qualification allows 10% short-window variation; the existing 5%
two-minute drift check and bounded phase steering remain active after repair.
Generic source timing is unchanged until qualification; audio streams and
explicit Reolink normalization retain their original paths.

Parallel 900-second runs of the initial implementation (`19951df`) completed:

| Metric | Camera 8 | Camera 9 |
| --- | ---: | ---: |
| Accepted packets | 14,958 | 17,465 |
| Startup drops | 7 | 16 |
| Established-stream drops | 0 | 0 |
| Repaired regressions | 760 | 792 |
| Normalization active at end | Yes | Yes |
| Mux errors / generation changes | 0 / 0 | 0 / 0 |
| Saved output duration | 898.452 s | 898.967 s |
| Independent full-decode errors | 0 | 0 |

Both outputs had strictly increasing DTS. After the first five media seconds,
camera 8 durations were 59.700–63.000 ms and camera 9 durations 49.922–52.500 ms.
Camera 9 needed appreciable phase steering (2,531 saturation samples; final
phase error 369 ms), but did not resume dropping packets or entering presentation
holds after startup. This is native input/mux verification, not a browser,
WebSocket, whole-server load or production soak test.

`fdbe924` subsequently tightened the declared-duration check for gradually
varying declarations, with tests for ramps and rational rounding. Both final
binaries passed a separate parallel 180-second live confirmation: camera 8
accepted 2,977 packets and repaired 151 regressions; camera 9 accepted 3,524 and
repaired 157. Each had one startup drop, zero non-monotonic input/output drops,
zero established-stream drops, zero mux errors and no generation change.
Both retained normalization and independently decoded with zero errors and
strictly increasing DTS (179.298 s and 179.593 s of saved media respectively).
The final VS2026 server and DLL build completed, and all qualification/clock
unit tests passed. The complete
15-minute captures are retained as ignored sensitive artifacts:
`build-vs2026/tapo-camera-{8,9}-arrival-qualification-fix-15min.mp4`.
Final confirmation files are
`build-vs2026/tapo-camera-{8,9}-final-qualification-3min.mp4`; full counters are
in `build-vs2026/tapo-preview-qualification-fix-results.txt`.
