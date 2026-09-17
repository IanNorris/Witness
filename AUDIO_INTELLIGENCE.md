# Audio intelligence research

## Recommendation

Prototype local sound-event classification with **YAMNet** first. It is small
enough to establish CPU cost and product usefulness before integrating a larger
audio foundation model. Keep inference and retention opt-in per camera, do not
perform speech-to-text, and treat audio scores as noisy evidence requiring
hysteresis and camera-specific calibration.

YAMNet uses MobileNetV1 to predict 521 AudioSet classes from mono 16 kHz audio.
It evaluates overlapping 0.96-second windows, produces a reusable 1024-value
embedding, and has 3.7 million weights with about 69.2 million multiplies per
window according to its official model documentation:
https://github.com/tensorflow/models/blob/master/research/audioset/yamnet/README.md

The AudioSet vocabulary covers useful starting families including speech,
footsteps/human locomotion, dogs and other animals, vehicles, engines, alarms,
glass, doors, and weather:
https://research.google.com/audioset/ontology/

## What the first version can and cannot promise

Useful first-class triggers are likely to include:

- dog barking and common animal sounds;
- speech presence (without identifying speakers or words);
- footsteps, knocks, doors, glass, alarms, and sirens;
- vehicle/engine presence and some broad engine-state sounds.

“A car started” should not initially be exposed as a precise built-in fact.
Generic AudioSet engine and vehicle labels may fire, but distinguishing a start
from an idling/passing engine is a temporal, environment-specific event. Once
real recordings and labels exist, train a small temporal head on frozen YAMNet
embeddings for that trigger rather than replacing the base model.

## Proposed pipeline

1. Tap the decoded camera audio before playback/recording-specific transforms.
2. Downmix to mono and resample to 16 kHz float audio.
3. Maintain a short bounded ring buffer; evaluate 0.96-second windows with
   roughly 50% overlap.
4. Batch ready windows in the optional inference lane and shed work under
   essential queue pressure.
5. Aggregate related class scores, then apply per-trigger threshold, minimum
   duration, hysteresis, and cooldown.
6. Correlate events with camera, capture timestamp, clip, motion/detections,
   media quality, and model version.
7. Retain raw audio only under the camera's recording policy; ordinary
   classification need retain only scores and event metadata.

Export or convert the selected model to ONNX only after a small reference
harness proves preprocessing and output parity. The repository already uses
ONNX Runtime, but conversion correctness, operator support, threading, and
quantization must be measured on the deployment CPU rather than assumed.

## Candidate comparison

| Model | Role | Strength | Cost/risk |
| --- | --- | --- | --- |
| YAMNet | MVP and embedding baseline | Small, documented, broad 521-class vocabulary | Older baseline; class scores need calibration |
| PANNs | Accuracy comparison | Strong AudioSet tagging family and sound-event examples | Larger models and a 32 kHz reference pipeline |
| BEATs | Later representation/accuracy comparison | Modern pretrained audio representation with AudioSet checkpoints | Transformer integration and compute are materially heavier |

Official sources:

- YAMNet: https://github.com/tensorflow/models/blob/master/research/audioset/yamnet/README.md
- PANNs: https://github.com/qiuqiangkong/audioset_tagging_cnn
- BEATs: https://github.com/microsoft/unilm/blob/master/beats/README.md

The production choice should be based on Witness recordings, not public
leaderboards alone. Microphone frequency response, compression, wind, HVAC,
rain, camera housings, distance, and local background noise can dominate the
error profile.

## Product and privacy controls

- Disabled by default and independently configurable per camera and event.
- Separate “detect,” “mark clip,” “start/extend recording,” and “run action.”
- Never retain or export continuous audio merely to classify it.
- Clearly identify speech-*presence* detection as sensitive even without
  transcription; allow it to be disabled while other classes remain active.
- Show confidence, trigger duration, model version, and contributing labels.
- Provide per-camera thresholds, quiet hours, cooldowns, and a test/listen mode.
- Make user correction easy and retain corrections for evaluation only with
  explicit consent.

## Evaluation plan

Build a time-separated, camera-separated evaluation set containing real
positives, hard negatives, mixed events, quiet periods, and network/media
damage. Random adjacent-window splits would overstate performance.

Measure per-trigger precision/recall, false triggers per camera-day, detection
latency, event fragmentation, CPU time per window, real-time factor, queue
delay, memory, and performance impact on essential video processing. Include
speech and privacy regression tests that verify no waveform is retained when
retention is disabled.

Suggested rollout:

1. Offline harness over exported clips with YAMNet and a selected PANN model.
2. Shadow-mode server inference with health-dashboard timing and queue stats.
3. Read-only activity markers and user feedback.
4. Opt-in recording extension and actions only after precision targets are met.
5. Train narrow embedding heads for valuable unsupported events such as a
   specific vehicle-start signature.

