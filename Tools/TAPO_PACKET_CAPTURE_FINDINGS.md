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

## Long-session rejection and bounded requalification

The October 2 health export showed preview repair counts of 2,042 (camera 8)
and 1,314 (camera 9), with saturation counts of 7,333 and 10,120. Both had
normalization disabled. Camera 9 remained at init generation zero. The initial
reading of zero repairs was from the main streams, not the previews.

Inspection found that the 120-second cadence check permanently rejected
normalization after drift exceeded +/-5%. Non-monotonic packet drops return
success, so this condition need not trigger an input reconnect. Main and
preview feeds use separate workers/channels; the recent AAC change is gated
to Reolink and does not apply to these audio-less Tapo previews. The exact
production trigger of the cadence rejection remains unproven.

Rejected previews now have bounded requalification: cooldowns of 60, 120 and
240 seconds, fresh valid cadence evidence for ten seconds, then a clean
keyframe. Recovery resets the mux before buffering that packet and emits a
discontinuity/new init segment. Three attempts are allowed; ten minutes of
healthy normalized, non-corrupt, non-saturated processing restores the budget.
Health exports include rejection timing, cadence evidence and retry exhaustion.
Separate native source/toolchain fingerprints and configuration identifiers
distinguish the server/Camera DLL from web bundle and browser identities.

Validation: policy and build-identity tests passed, as did twelve web regression
tests. A paced synthetic integration test forced rejection, requalified the
actual mux, and verified one discontinuity, a new init generation and continued
fragments. Both saved generations independently decoded with FFmpeg without
errors. The VS2026 RelWithDebInfo build completed without rebuilding external
dependencies. This round did not run a new live-camera soak; production
confirmation is still required.

## October 3 production follow-up: delivery-window evidence

The matching native `8e9d8a53f373` / web `25f90af6b0d1` export covers about
22.6 hours. Six displayed non-Tapo players have no restarts or stalls. Tapo
camera 8 is currently normalized but has accumulated 990 restarts/968 stalls;
camera 9 preview exhausted three recovery attempts early and remains rejected.
Its final rejection compares 120,000 ms of nominal durations with 126,415 ms
of mux arrival time. Camera 8 also rejected a 120,000/132,245 ms window.

These comparisons do not prove genuine variable frame rate. The duration sum
counts packets surviving earlier timestamp/keyframe decisions, whereas elapsed
time includes pauses. A missing access unit, discarded invalid timestamp,
server scheduling stall or sustained lower delivered cadence could all produce
this result. Do not simply widen the tolerance based on this export.

The next diagnostic build retains raw delivery evidence for the rejected
arrival-clock window: packet count, original-duration total, invalid durations,
duration changes (allowing one tick of rounding), corrupt flags, maximum gap,
and excess time in substantial gaps (>250 ms and >4 nominal frame intervals).
Sampling occurs before mux drop decisions. Both health and full-debug exports
carry the snapshot, and Health displays it with the rejection evidence.
Evidence remains bounded and adds no packet/media retention.

These are **mux delivery** times, not NIC/RTP ingress times. Long-gap excess is
an indicator, not a quantity to subtract blindly from the clock. The diagnostic
does not change normalization, correction limits or retry budgets. Rejection
window counters must be paired with a fresh input/output capture before
choosing between delivery-clock correction, outage handling or rate adaptation.

Validation: the native policy/evidence tests cover regular delivery, sustained
slower delivery, a substantial pause, a pause across a window boundary, invalid
durations, flagged corruption and rational-duration rounding. Seven browser
playback/drift regression tests passed. The final VS2026 RelWithDebInfo server,
Camera DLL and web build passed (existing ABI/Sass/linker warnings remain).
No new live-camera capture has been taken in this round.

## Local validation and buffered-pause reproducer

Subsequent direct-camera tests used native `a515cbca678d` without starting the
local server or changing authentication. Camera 9 ran for 900 seconds:
17,344 accepted packets, 792 repairs, 14 startup drops, zero established drops,
zero mux errors and no normalization rejection/generation change. Its phase
ended at 734 ms and correction saturation counted 4,144 packets, so delivery
remains close to the correction limit. Camera 8 ran for 300 seconds: 4,977
accepted, 251 repairs, one startup drop, zero established drops, zero mux
errors, zero saturation and no rejection/generation change.
Both saved outputs independently decoded without FFmpeg errors (898.746 s and
299.072 s of media). This is not a reproduction of the production failure.

Camera 8's additional 30-second raw capture contains 500 inputs / 500 decisions,
all written, with no changed payload hashes, binary hash failures or writer
queue losses. One decision at the capture boundary precedes its matching input
record because of lookahead. Its reconstructed output also decoded cleanly.
Artifacts remain ignored under `build-vs2026`, including
`packet-captures/camera-8-preview-1791066106590-1` and
`tapo{8,9}-cadence-evidence-20261003.mp4`.

`Tools/Tests/LiveCadencePauseTests.cpp` reproduces a real mux-policy failure
offline: a steady 20fps/no-B-frame fixture with a Tapo-like backward source
clock stays normalized; injecting a 15-second buffered pause followed by a
burst rejects its clock (120,000 ms nominal versus 128,950 ms observed), then
discards otherwise valid regressing packets. Raw evidence reports a 15,050 ms
maximum gap and 15,000 ms long-gap excess. Virtual monotonic time exercises the
actual mux/qualification logic; this is not a simulated network-loss decoder
test or proof that production has this specific pause.

The raw captures previously lacked independent observation timestamps. Added
demux, mux-entry and decision monotonic timestamps allow the next capture to
separate arrival gaps from Witness pipeline delay, including its one-packet
lookahead. The reporter explicitly refuses timing estimates for older captures.
The next policy change should address contaminated validation windows and
clean-keyframe timeline recovery, not merely widen the cadence tolerance.

The observation-timestamp build (`ff757174ded5`) completed successfully.
A fresh 45-second camera 9 probe, including a 30-second raw capture at
`build-vs2026/packet-captures/camera-9-preview-1791066906749-1`, validates the
new timing fields. All 615 captured inputs have monotonic observations, with
no changed hashes, binary failures or capture queue losses. The capture
includes initial probing/startup packets, so its 20.56 delivered FPS must not
be treated as a steady-state camera rate. Arrival gaps have p50 48.32 ms,
p95 98.71 ms, p99 117.10 ms and maximum 162.08 ms. Demux-to-mux time closely
matches these gaps because of lookahead; actual mux-to-decision work is p50
0.006 ms, p99 0.066 ms and maximum 0.1 ms in this unloaded direct probe.
The reconstructed output decodes without FFmpeg errors. The probe finishes
normalized with zero established drops/mux errors. Reporter and credential-safe
runner tests pass. These unloaded local timings do not establish production
server timing under memory/CPU pressure.

## Delivery-pause recovery candidate

The `codex/tapo-pause-recovery` candidate handles a >=2-second gap only on an
already qualified, video-only, no-B-frame arrival-clock preview. It defers
validation of that contaminated window for at most 30 seconds. Fresh delivery
must meet the original 5% cadence tolerance on unchanged declared durations,
span at least one second and reach a clean keyframe. Only then does the mux
retire the pending old-generation packet, publish a discontinuity/new init and
re-anchor the independent output clock. Genuine sustained rate mismatch,
invalid/corrupt samples and changed declarations do not inherit this exception.
An expired recovery cannot repeatedly rearm the validation exemption on the
same generation. Pause recovery does not consume or clear cadence-retry budget.
Recording, audio and source-clock normalization paths are unchanged.

Native policy tests and actual-mux tests passed for H.264 and HEVC. The tests
cover a buffered pause/burst, fresh-keyframe recovery, the one pending-frame
retirement, unchanged cadence-retry budget and continued rejection of a sustained
10% rate mismatch. Both control and both pause generations for each codec
independently decode without errors. The earlier rejected-clock requalification
integration test and seven browser playback/drift tests also pass.

Five-minute ordinary live checks with native `86d6d681ff18` passed on cameras
8 and 9. Camera 8 accepted 4,975 packets and repaired 252; camera 9 accepted
5,761 and repaired 263. Both had zero established drops, zero mux errors,
no rejection and no unexpected pause resets; both saved outputs decode cleanly.

A subsequent 210-second camera 9 test injected one 12-second processing pause
into the standalone test connection at 124 seconds. It recovered at one new
generation, remained normalized with zero established drops/mux errors, and
consumed no cadence-retry attempt. The raw capture
`build-vs2026/packet-captures/camera-9-preview-1791068277917-1` verifies an
independent 12,014.873 ms demux gap, unchanged payload hashes, and no writer
queue loss. However, it exposed one `beforeVideoEpoch` drop shortly after the
reset: the source clock stepped 74.7 ms before the newly selected epoch.

The final candidate (`a50a695dabb9`) preserves that picture only when the
already qualified arrival clock, unchanged duration, equal PTS/DTS and clean
packet prove a bounded regression (at most one second/20 nominal frames).
Unqualified, audio, B-frame, corrupt and unbounded-reset cases keep their
guards. The H.264/HEVC integration fixtures now inject an epoch-crossing
picture and still have zero timestamp drops and clean decoded generations.

Validation limitation: the live pause's new generation decoded cleanly, but
the old generation produced three non-monotonic decoded-frame DTS messages
at the FFmpeg null output sink. Its 2,669 encoded packet DTS values were
strictly increasing, and decoded pictures were I/P only. This is not a mux
packet-DTS regression; presentation reordering around the camera pause remains
an investigation item. Do not claim the production Tapo problem fully solved
or all pause outputs timing-clean from these checks.

A 45-second camera 9 confirmation of the final epoch guard accepted 854
packets, repaired 39, and had only one startup drop, with zero established
drops/mux errors, no rejection and no unexpected pause reset. It remained
normalized. The candidate server/Camera DLL/Web/models were preserved together
under `build-vs2026/deploy/tapo-pause-recovery-a50a695dabb9`; copied server/DLL
SHA-256 hashes match the built binaries. The previous
`tapo-pause-recovery-86d6d681ff18` staging directory is an earlier candidate
without the epoch-crossing guard, not the final deployment candidate.

### Input picture-order investigation (4 October)

The reporter can now export exact Annex-B input independently of the mux.
The pause capture exported 508 access units from activity 3529. Its first 400
decoded frame MD5s exactly match the independently reconstructed generation-0
output (zero image/order mismatches); both keyframe-started replays decode
without errors or non-monotonic output warnings.

FFmpeg `trace_headers` on that **input** shows H.264 `pic_order_cnt_type=2`,
10-bit frame numbers, one reference picture and allowed frame-number gaps.
After a clean IDR, the source sequence reaches frame numbers 0,1,2,3,4, then
returns to 1,2,3,4 without an IDR (access units 240..248 in this replay).
This unusual elementary-stream transition is upstream of Witness's timestamp
rewrite. The whole-generation decoder, unlike the independently started slice,
presents an extra/reordered picture at this point; subsequent frame hashes
match with an index offset. This explains why increasing encoded DTS alone
does not prove increasing decoded presentation order. It does not prove the
camera firmware, network or demuxer is the origin of that transition, nor
justify dropping every regressing timestamp or mutating encoded slice headers.
Raw RTP would be the next layer if the upstream transition needs attribution.

The final `a50a695dabb9` binary was then tested for a planned 360 seconds with
the same controlled 12-second pause at 124 seconds. It recovered once (12,009 ms
gap), with no additional timestamp drops or cadence-retry attempts. A fresh
30-second capture contains 574 inputs/573 decisions, all decisions written,
zero changed hashes, zero binary mismatches and zero capture queue loss.
Artifacts: `camera-9-preview-1791069652748-1` and
`tapo9-final-pause-validation-20261004.mp4` plus its generation-1 file.
Both saved generations decode without errors or non-monotonic null-sink DTS.

**The extended live test nevertheless failed:** an unplanned later delivery
stall produced a 20.3-second phase error at 305 seconds, then an input
`FrameError` (`readResult=8`) ended the probe at 318 seconds. There was one
established dropped/corruption packet during that later episode, no mux errors,
and no cadence rejection. The bounded capture had already ended and cannot
attribute this second stall. The standalone probe deliberately terminates on
read failure; the server's existing worker recreates its input on this error.
Do not report this run as a six-minute stability pass or conflate the later
read failure with failure to recover the controlled pause.

A fresh 60-second connection immediately afterward succeeded: 1,131 accepted,
52 repaired, one startup drop, zero established drops/mux errors/rejections,
and a clean independent decode (`tapo9-reconnect-check-20261004.mp4`). The
camera was not persistently latched offline. This short reconnect check does
not establish the cause or frequency of the late input-read failure.

### Extended-probe instrumentation correction (4 October)

The standalone probe previously installed a private FFmpeg callback before
initializing its input. `Stream::OneTimeInit()` then replaced it with the
production callback. Consequently, the probe's earlier `ffmpegErrors`,
`audioDurationWarnings` and `missingPtsWarnings` zero counts were not valid
evidence that FFmpeg emitted no such messages. Packet, mux and capture evidence
is unaffected by this counter defect.

The probe now reads thread-local cumulative counters maintained by the production
callback. It does not replace that callback or bypass the scoped error tracking
used by production corruption handling. `LivePreviewProbe --log-self-test`
checks synthetic warning/error counts and `FFmpegLogContextScope::HasError()`
across initialization of two streams, without connecting to a camera. This
self-test passes; the three wrapper tests pass. The existing VS2026 probe target
builds successfully with the previously recorded ABI/link warnings.

The wrapper supports `--capture-count 1..8` and `--capture-every >=31`, together
with `--capture-delay`. Every 30-second window must fit inside the requested run.
Existing per-window 128 MiB byte / 8 MiB queued-byte limits are unchanged, so eight
windows accept at most 1 GiB of capture record data. There is at least a nominal
one-second gap between windows: this is not continuous network/RTP capture.
Ambient capture settings are cleared and stream credentials remain environment
only. Captures begin before the frame-processing call so an immediately following
stall does not postpone the requested capture until after that stall.

Numeric reports include last/max `ProcessFrame` time and calls over one second.
These measure the whole demux/mux operation, not solely socket-read latency, and
exclude the deliberately injected sleep. They must not be labelled network RTT
or browser decode latency. The probe still uses passthrough mode and stops on
input errors rather than silently reconnecting and obscuring the failure.

The corrected probe completed both live runs with exit zero:

| Camera | Run | Accepted / repaired | Established drops / mux errors | End phase | Max ProcessFrame |
| --- | --- | --- | --- | --- | --- |
| 8 | 300 seconds, ordinary delivery | 4,974 / 252 | 0 / 0 | -17.8 ms | 192.4 ms |
| 9 | 390 seconds, 12-second pause at 124 | 7,524 / 339 | 0 / 0 | +477.8 ms | 148.6 ms |

Camera 9 recovered once at a 12,061 ms observed gap, with one discontinuity/new
generation and zero cadence retries or rejection. Its 16 startup drops (four
before-epoch and eleven non-monotonic input) did not increase during recovery;
camera 8 had one startup drop. Neither run had a ProcessFrame call over one
second or reproduced the prior late input failure. Eight capture windows per
camera are retained under `camera-8-preview-1791076164956-1` through
`camera-8-preview-1791076381929-8`, and `camera-9-preview-1791076123153-1`
through `camera-9-preview-1791076340146-8`.

Camera 9's window seven covers the previous failure time (306..336 seconds).
It contains 584 inputs/decisions, all written, unchanged payloads and hashes,
zero capture queue loss, and a maximum arrival gap of 134.0 ms. Window-boundary
decisions can refer to the single lookahead packet received before capture
started; a missing source correlation at that boundary is not proof of packet
loss. Camera 8's last window has 498 inputs/decisions, all written, unchanged
payloads/hashes, zero queue loss and a maximum arrival gap of 148.3 ms.

Independent decoding found an H.264 macroblock error in camera 9's pause window,
both in the exact raw-input Annex-B replay and generation-zero mux replay.
The complete generation-zero recording fails strict `ffmpeg -xerror` decoding
on the same error. This is not a fully corruption-free pass; unchanged payload
hashes and reproduction in input attribute this instance upstream of muxing,
but do not distinguish camera firmware from transport or demux behavior.
The complete camera 9 generation-one and camera 8 recordings pass strict decode.

Some default FFmpeg null-sink checks also reported duplicate output DTS in late
replays. Direct decoded `best_effort_timestamp` inspection instead shows zero
duplicates/regressions: 550 frames/minimum step 4,548 ticks in camera 9 window
seven, and 489 frames/minimum step 5,387 ticks in camera 8 window eight. Both
replays decode without warnings using `-enc_time_base 1:1000000 -fps_mode
passthrough`. These late-window null-sink warnings are output timebase
quantization, not evidence of a decoded presentation regression. Keep this
distinction in future validation; a decoder exit zero alone also does not prove
no codec errors unless strict error handling or captured diagnostics are checked.

A matched server/DLL/Web/models build (native identity `830b8380f811`) is staged
at `build-vs2026/deploy/tapo-extended-capture-830b8380f811`. The server and Camera
DLL SHA-256 hashes and all 48 web files match the built output. This validation
does not establish long-running production or browser/MSE stability, nor resolve
the raw-input codec error around the controlled processing pause.

`Tools/Tests/check_preview_decode.py <local-recording.mp4>` now makes this
validation repeatable. It combines strict `-xerror` decoding with a microsecond
output timebase and separate packet DTS / decoded presentation timestamp checks.
Missing, duplicate, regressing or empty timestamp evidence cannot produce a pass;
neither can decoder diagnostic lines despite an exit zero. It only opens local
files (512 MiB limit), disallows network protocols, bounds each subprocess to
120 seconds, and emits structured counts rather than relaying arbitrary raw
decoder text. Its three unit tests pass.

The native timestamp guard and packet-capture tests pass. The actual mux pause
test was rerun with its required 140-second H.264 and HEVC fixtures (a no-argument
invocation is not a test pass). Both control/pause cases pass while the deliberate
10% slower-delivery case remains rejected. Requalification also passes. All six
saved control/pause recordings and both requalification generations pass the
new strict decode/timestamp checker. These are offline mux/codec tests, not
browser integration or physical-camera stability tests.

### Fifteen-minute natural-delivery confirmation (4 October)

The matched `830b8380f811` Camera DLL/probe then ran cameras 8 and 9 concurrently
for 900 seconds each, without injected pauses. Both completed with exit zero:

| Camera | Accepted / repaired | Established drops / mux errors | End phase | Max ProcessFrame |
| --- | --- | --- | --- | --- |
| 8 | 14,970 / 757 | 0 / 0 | -9.2 ms | 273.8 ms |
| 9 | 17,440 / 790 | 0 / 0 | +190.4 ms | 257.3 ms |

Both stayed normalized, with zero cadence rejection, retries or pause resets;
neither had a processing call over one second. Startup counters stayed fixed
(one drop for camera 8; 16 for camera 9, including six before-epoch/nine
non-monotonic input). Eight bounded captures per camera started every 120 seconds
from time zero. This leaves 90-second uncaptured intervals and is not continuous
transport capture. Artifacts are `tapo8-natural-15minute-20261004.mp4` and
`tapo9-natural-15minute-20261004.mp4`, plus the capture directory families starting
`camera-8-preview-1791076823075-1` / `camera-9-preview-1791076806958-1` and ending
`camera-8-preview-1791077663103-8` / `camera-9-preview-1791077647002-8`.

Both complete recordings pass the strict decoder/timestamp checker with no
codec diagnostics, missing timestamps, duplicate timestamps or regressions.
Camera 8 has 14,968 finalized packet/frame samples; camera 9 has 17,440.
The counts were sampled before mux shutdown and the files contain finalized
fragment callbacks; this difference alone does not establish a timestamp-policy
drop or transport loss. Camera 9's one very short step (4.93 ms)
and all four steps below 40 ms occur within the first two seconds of startup,
not throughout the established stream; its maximum packet step is 413.9 ms.

This provides a measured 15-minute two-camera demux/mux pass and does not
reproduce the earlier input stall. It does not prove full production-load or
browser/MSE stability, attribute the intermittent input issue, or eliminate the
raw-input codec error seen in the separate artificial-pause test. No server
restart, production database mutation or production deployment occurred.

A subsequent lifetime audit found that the probe's mux destructor could flush
its callback after the captured output stream/local state had been destroyed.
The probe now explicitly shuts down the mux while those captures are alive,
flushes the output before checking its status, and has a scope guard that clears
the callback on early exits. This corrects the diagnostic recorder only, not
the server stream path. Previously saved files independently decode as recorded,
but their termination tail must not be used as proof that all accepted packets
were finalized. The live read/cadence counts above are still the directly sampled
pre-shutdown evidence, not a claim about final tail completeness.
The corrected probe builds, passes its log self-test and all three wrapper
tests, and completes a fresh 30-second camera-9 run with exit zero. Its finalized
recording passes strict decode and packet/frame timestamp checks.

### Current stream-state implementation checkpoint (4 October, evening)

Camera 9 ran for 900 seconds using native identity `5d75fe33ed9e`, after the
LiveOutputStream private-state migration and PTZ configuration changes. It
accepted 17,452 video packets and repaired 793 timestamps, with zero established
drops, mux errors, cadence rejection/retries/pause resets, generation changes or
FFmpeg errors. End phase was +341.833 ms. One ProcessFrame call took 1,095.66 ms;
it did not cause a cadence reset. Two startup corruption/recovery events and
16 startup drops stayed fixed throughout the established stream.

The complete finalized recording has 17,452 decoded-frame and packet timestamp
samples; strict decoding reports no diagnostics, missing/duplicate timestamps
or regressions. Eight 30-second captures, starting every 120 seconds, also
reconstruct and decode cleanly. All recorded binary hashes match. Later windows
have one decision without a captured input: consistent with the pending
one-packet lookahead crossing the capture-start boundary, not proof of complete
source correlation. There are still 90-second uncaptured intervals.

Artifacts are `artifacts/tapo9-current-15minute-20261004.*`, the eight
`artifacts/tapo9-current-window*-report.txt` / `*-decode.json` files, and capture
directories `camera-9-preview-1791138146682-1` through
`camera-9-preview-1791138986727-8`. These measurements exercise camera input,
production demux/mux and saved output, not browser MSE or full server load.

Camera 8 initially failed initialization and a separate TCP reachability check
timed out. A later bounded retry initialized in 1,557.01 ms, completed ten
seconds with no established drops/mux errors, and its recording passed strict
decode/timestamp checks. This changing reachability is not attributed to the
cadence fix; a fresh 15-minute camera-8 run is required before claiming the
current two-camera validation is complete.

The probe now emits numeric initialization result and duration, rather than
only exit 3 on setup failure. Its credential-safe wrapper requires complete
numeric report syntax and a bounded relative capture-directory pattern;
arbitrary text sharing a report prefix is rejected. All four wrapper tests and
the native log self-test pass. The rebuilt runtime identity `9100299d68a0`
differs because explicit telemetry test targets were added to server CMake;
the Camera stream implementation is unchanged from the 900-second run above.

### Camera 8 reproduced cadence rejection; first candidate failed

The later 900-second camera-8 baseline (`9100299d68a0`) reproduced the actual
failure: at 120 seconds of declared packet duration, the arrival reference was
131.284 seconds. Repair was disabled, followed by 5,734 established timestamp
drops and 194 corruption notifications by the end of the run. There were no
FFmpeg errors, source-duration changes or invalid declared durations. ProcessFrame
never exceeded 299.111 ms. A probe exit zero only means reading/muxing continued;
it is emphatically not a playback-health pass.

In the captured rejection window (`camera-8-preview-1791139369180-2`), all raw
input access units replay and strictly decode without errors, but 124 of 682 mux
decisions drop non-monotonic timestamps. Recorded input hashes and output hashes
match, with one boundary lookahead decision lacking its captured source record.
The reconstructed output still strictly decodes (frequent random-access frames
can mask reference loss), but that does not excuse the measured packet loss.
Artifacts: `artifacts/tapo8-before-rate-window2.*` and the baseline
`artifacts/tapo8-current-retry-15minute-20261004.*`.

A first bounded arrival-duration learner used a 20-second window, clean/stable
duration metadata, pause/corruption/order guards, and a strict 10% rate bound.
Its H.264/HEVC offline tests passed for an 8% mismatch, while preserving 10%
rejection, pause recovery and stable-source-clock behavior. Timestamp recovery
and H.264/AAC ownership tests passed too. An integer comparison corrected an
initial MSVC floating-point ambiguity at the exact 10% boundary.

The subsequent 180-second live experiment (`4c926fc8926c`) nevertheless failed:
20 seconds declared took 22.404 seconds to deliver (about 12% slower), and the
bound rejected it before any rate adjustment. It produced 1,124 established
drops and 47 corruption notifications. See
`artifacts/tapo8-bounded-rate-candidate1-20261004.*`. This disproves treating the
8% synthetic case as sufficient camera coverage. The candidate remains
experimental, not a deployable fix; simply moving the arbitrary percentage
threshold would not establish safe adaptation across changing delivery rates.

### Measured-rate consistency candidate: short live pass

Candidate `80f201d75caa` replaces that declaration-relative limit with two clean,
consistent 20-second delivery windows before adopting a frame-duration estimate.
It applies only to already-qualified faulty source clocks on video-only,
no-B-frame previews. Intact source clocks, audio and provider-profile streams
retain their existing policy. Corruption, reordered/missing timestamps, changing
duration declarations, substantial pauses and persistently inconsistent rates
prevent learning; reconnect clears the learned estimate.

Rebuilt guard, H.264/HEVC cadence/pause, timestamp recovery and H.264/AAC ownership
tests all pass. Ten generated output files strictly decode with no missing,
duplicate or regressing timestamps. These include 12% rate mismatch, pause/burst,
inconsistent rate rejection and intact-clock controls. The credential-safe probe
wrapper's four tests also pass. Build log: `artifacts/tapo-rate-consistency-build.log`.

A fresh 180-second camera-8 run accepted 2,978 packets, repaired 153 timestamps,
and had zero established drops, cadence rejection, retries, mux errors, FFmpeg
errors or generation changes. One startup drop/corruption/recovery remained fixed.
The delivered interval in this run was about 60 ms, rather than the earlier
44.8 ms sample; seven estimate updates ended at 60.0222 ms with -34.8667 ms phase
error. Maximum ProcessFrame was 209.334 ms. The finalized recording strictly
decodes with 2,977 packet/frame timestamp samples and no diagnostics.

Artifacts: `artifacts/tapo8-rate-consistency-candidate2-20261004.*` and
`artifacts/tapo8-rate-consistency-candidate2-window*-report.txt`. Three 30-second
captures were collected. Established windows 2 and 3 each write all 500 packet
decisions, with zero payload/hash changes and one source-correlation gap at the
lookahead capture boundary. Window 1 contains startup keyframe waiting. This is
a short production-demux/mux check, not browser MSE, full server load or a claim
of sustained production stability. Concurrent 15-minute camera-8/9 validation
remains the next gate before producing a deployment package.

### Concurrent 15-minute candidate validation

Both camera-8 and camera-9 probes completed 900 seconds concurrently using
candidate `80f201d75caa`. Camera 8 accepted 14,969 video packets and repaired 754
timestamps; camera 9 accepted 17,446 and repaired 794. Both had zero established
drops, cadence rejection/retries, pause resets, mux errors, FFmpeg errors,
discontinuities or generation changes. Startup drop totals stayed fixed at one
and sixteen respectively, with one startup corruption/recovery pair each.
Final phase errors were +64.9889 ms and -244.2 ms. Learned durations ended at
60.0222 ms and 51.9222 ms; maximum ProcessFrame calls were 243.465 ms and
179.637 ms, with none over one second.

Both finalized recordings strictly decode with no diagnostics, missing/duplicate
timestamps or regressions (14,968 and 17,446 packet/frame samples). All sixteen
30-second captured input replays and reconstructed output replays strictly
decode too. Every established-window decision is written, rather than discarded
for non-monotonic timestamps. All binary hashes match and all pre-mux payloads
remain unchanged. Startup windows include waiting for a clean keyframe; later
windows each have one uncorrelated lookahead-boundary decision. Writer queues
reject no capture records. These are eight windows per camera spaced 120 seconds
apart, leaving 90-second gaps: not continuous input/output correlation.

Artifacts: `artifacts/tapo{8,9}-rate-consistency-15minute-20261004.*` and
`artifacts/tapo{8,9}-rate-consistency-15minute-window*.report.txt` / `*.checks.json`.
The probe exit status was checked alongside these health/disposition metrics;
exit zero alone is not accepted as proof. This closes the concurrent production
demux/mux validation gate for this candidate. Browser MSE, full server workload
and longer production stability remain separate verification limits.
