# Motion-vector calibration and model exploration

## Current baseline

`MotionVectorFilter` now owns its core detection thresholds per instance, but all
cameras still receive the legacy defaults. No provider preset or trained model
is enabled by this change. The legacy score uses raw FFmpeg `motion_x/y` units,
samples every eighth vector at up to 1080p, and treats vector count rather
than covered block area as evidence. Preserve this as algorithm v1 so existing
camera behavior can be compared against any replacement.

An MV-backed camera must decode inter-frames. `SkipFrames > 1` currently
switches input analysis to keyframe-only decoding, which generally supplies
no MV side data. The server now forces 1 for MV filters. Long runs of decoded
frames without MV side data are logged as a warning; occasional keyframes are
normal.

## Calibration data and replay

1. Collect source *stream-copy* clips from each camera: quiet baseline,
   nuisance activity (wind, rain, insects, light changes), and desired activity.
   Include day and night. Re-encoding changes the vectors and invalidates the
   calibration. For missed events, sample continuous recordings or explicit
   capture, not only clips that the current detector chose to create.
2. Record camera, codec, resolution, frame rate, GOP, firmware if known,
   recording hash, algorithm/profile version, and labelled active intervals.
   Avoid credentials or full RTSP URLs in the calibration package.
3. Replay through the production decoder, filter and observer with a virtual
   clock. Compare current settings with bounded candidate settings. Keep
   complete recording sessions held out from tuning, not random frames.
4. Report event recall, baseline false triggers per hour, detection delay,
   resulting clip duration, and CPU cost. Recommend sensitive/balanced/quiet
   settings only when held-out evidence is sufficient. Apply explicitly to one
   camera and retain a rollback snapshot.

Profile inheritance should be versioned: legacy default -> provider + codec ->
optional model/stream mode -> camera override. Masks remain camera-local.
Provider detection needs an explicit setting with auto-detection only as a
suggestion; the current RTSP-path detector only recognises Reolink. Keep
native-event selection separate from MV tuning. A resolution, codec, firmware,
or encoder-setting change should flag a calibration as potentially stale.

## Model investigation

There is prior art, but no identified drop-in model trained for Witness's
binary, low-false-alarm surveillance trigger across these encoders:

- [CoViAR](https://arxiv.org/abs/1712.00636) uses I-frame RGB, motion vectors
  and residuals for *action recognition*. Its [reference implementation](https://github.com/chaoyuaw/pytorch-coviar)
  is a useful data-loader and architecture reference, not a calibrated motion
  trigger.
- [DMC-Net](https://arxiv.org/abs/1901.03460) learns to clean noisy vectors
  using optical-flow supervision and action labels. It motivates a small
  learned motion representation, but its published task and datasets differ
  from camera-specific foreground/event detection.
- [Compressed-domain H.264 surveillance activity research](https://doi.org/10.1016/j.jvcir.2011.03.010)
  emphasizes that encoder motion estimation, block partitions and reference
  choices affect the exported vectors. [FFmpeg's AVMotionVector](https://ffmpeg.org/doxygen/trunk/structAVMotionVector.html)
  exposes motion scale and block dimensions needed for normalization.

First benchmark a simple model against tuned v1 and a hand-designed normalized
v2: aggregate vectors into a small spatial grid using displacement divided by
`motion_scale`, block area, reference direction, vector-availability mask,
and temporal history. A tiny temporal CNN or gradient-boosted classifier on
these features is more plausible for CPU-only deployment than importing a
full action-recognition network. Labels should distinguish meaningful motion
from nuisance/background and no-vector/connection failures. Train across
multiple camera models, then evaluate on cameras and sessions held out
entirely. Only ship if event recall, false-trigger rate, latency and CPU all
beat the non-ML baseline. A classifier cannot recover an event from frames
that never carry usable vectors; retain a pixel-difference/native-event
fallback for those streams.

Do not infer exact pixels per second from `AVMotionVector.source`: FFmpeg
documents past/future direction, not the precise reference-frame interval.
