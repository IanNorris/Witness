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

# Ian Notes

* I agree YAMNet feels like a good first step. I can provide some clips with interesting sound on them and we can establish how the model performs on them - both in terms of tagging results, but also in terms of cpu cost.
* One interesting idea that comes out from this is selective muting. One established pattern I'm seeing is that the cameras are exposed to the wind and the wind noise on the microphone is quite distracting. Given the cameras record a lot when windy, this means constant wind noise from the CCTV TV in the current setup. It will be interesting if we could run this with latency that would allow us to mute the clip that's ~1.5s behind display from the processing in time to not hear that.
* Similar to my patent on recognizing motion in cameras from their motion vectors, I'm wondering if we can potentially use audio packet sizes to establish when there's audio to be heard that's worth running the model on.
* For now we'll evaluate existing model performance and not follow up with any additional fine tuning or tuning.

## Initial offline prototype results

The first dependency-light evaluator is `scripts/audio_intelligence_eval.py`.
It uses FFmpeg to decode directly to an in-memory mono 16 kHz waveform and the
existing ONNX Runtime Python package for inference. Decoded audio is not written
to disk. `scripts/download-models.py --audio` downloads an immutable revision of
the straight YAMNet v1 ONNX conversion and verifies SHA-256 hashes for both the
model and AudioSet class map.

On three existing Reolink recordings, using one ONNX CPU thread, a 0.96-second
window took roughly 3-5 ms CPU (about 0.6-1.0% of one core per continuously
analysed stream at YAMNet's 0.48-second hop). Whole-clip real-time factors were
0.006-0.009. This leaves ample headroom for an offline/shadow trial and makes a
decision inside the live player's roughly 1.5-second delay technically
plausible: the dominant latency is collecting up to 0.96 seconds of audio, not
inference.

The unlabelled samples produced sensible broad results (silence for a very quiet
clip and outdoor/wind classes for an exposed camera), alongside expected noisy
false labels such as heartbeat and heart murmur. This reinforces the need for
camera-specific thresholds and grouped triggers rather than exposing raw class
names as facts.

Audio packet bytes were weakly or negatively correlated with loudness and
non-silence model scores in these samples. Packet size should therefore remain
an experimental scheduling hint, never a sole gate: fixed/near-fixed bitrate
codecs and codec noise floors can make quiet and interesting windows look alike.
The evaluator exports per-window packet bytes, RMS, grouped scores, and raw top
classes so this can be measured again on labelled user clips.

## Implemented shadow mode

The first server integration is deliberately observational and opt-in per
camera. Completed clips are decoded in memory to mono 16 kHz float audio by a
low-priority worker, only while no camera is actively recording. The YAMNet
wrapper uses one CPU thread, emits only curated product groups, applies a
hysteresis release threshold, and coalesces adjacent windows. SQLite stores
only group, time range, peak confidence, clip/camera identity, and model
version; decoded samples are discarded after inference.

Camera administration exposes enable and confidence controls. Recent Activity
shows one badge per detected sound family with the peak score in its tooltip.
`WitnessServer.exe /test-audio <clip.mp4> [confidence]` exercises the exact
production decoder and classifier without changing the database. Recording
triggers, actions, live selective muting, and model fine-tuning remain outside
this shadow-mode milestone.
