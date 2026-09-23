# Motion-vector provider comparison, 2026-09-23

The same read-only `MotionVectorScaleProbe` used for the earlier Hikvision trial
sampled main RTSP streams from Tapo cameras 8–9 and Reolink cameras 10–13,
sequentially. Each sample decoded 150 frames with FFmpeg `export_mvs` enabled.
URLs came from the user's local database snapshot; credentials and URLs were
not logged. No production settings or recordings were changed.

| Provider / cameras | Codec / resolution | Frames with exported MVs | `motion_scale` | Block area in sampled vectors | Vectors accepted by current per-vector gate |
| --- | --- | ---: | --- | --- | ---: |
| Hikvision 1, 3, 4, 5, 7 | H.264 1920×1080 | 146/150 each | 4 only | Mostly 256 px²; also 64 px² on all five, 128 px² on 5 | Varies by scene; see earlier trial |
| Tapo 8 | H.264 2304×1296 | 147/150 | 4 only | 256 px² only | 50,202 / 1,705,977 (2.9%) |
| Tapo 9 | H.264 2304×1296 | 146/150 | 4 only | 256 px² only | 47,021 / 1,680,813 (2.8%) |
| Reolink 10, 12 | HEVC 3840×2160 | 0/150 each | unavailable | unavailable | unavailable |
| Reolink 11, 13 | HEVC 1920×1080 | 0/150 each | unavailable | unavailable | unavailable |

The Tapo result rules out a **different `motion_scale`** as the explanation for
their motion-filter behaviour: it is 4, just like Hikvision. Both brands have
the same equivalent current per-vector squared-pixel gate of **0.25–8000**
(`motion_x² + motion_y²` between 4 and 128000, divided by `4²`). However,
the Tapo vectors in this sample all cover 16×16 blocks; Hikvision also emitted
smaller blocks, with camera 3 emitting about 12% 8×8 blocks. Resolution,
scene activity, vector distribution, frame timing, masks, and downstream bucket
scoring could matter. The accepted-vector proportions are not detection rates
and cannot be fairly compared as sensitivity metrics from unrelated scenes.

For all four Reolink HEVC streams, decoding succeeded but FFmpeg provided no
`AV_FRAME_DATA_MOTION_VECTORS`, including on inter frames. The FFmpeg source
in this dependency build sets `AV_CODEC_EXPORT_DATA_MVS`, but its frame-side
data emission path is present in `libavcodec/mpegutils.c` and no HEVC path
emits this side data. Consequently, `motion_scale` normalization cannot make
the existing MV filter work on these HEVC feeds. The Reolink event feed is a
separate motion source. A future HEVC MV approach would need decoder support
or a different analysis method.

Practical next step: do not globally change thresholds based on this trial.
Capture labelled quiet/active Tapo and Hikvision intervals, replay their
vector distributions through the *whole* current filter (including bucket
scores and masks), and compare false positives/misses per camera. The scale
normalization can still be coded defensively for future H.264 cameras with a
different precision, but it is not the Tapo fix and should retain the current
scale-4 behaviour exactly.
