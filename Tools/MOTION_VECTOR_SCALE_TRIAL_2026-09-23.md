# Hikvision motion-vector scale trial, 2026-09-23

The five requested Hikvision main RTSP streams were sampled sequentially from
the development machine while the production server stayed running. The local
database copy supplied by the user was opened read-only to obtain stream URLs.
No credentials or raw URLs were logged. The probe decoded at most 150 frames
per camera with FFmpeg's `export_mvs` enabled; it did not record video or alter
camera/server settings. Sampling was a short snapshot, not a motion-labelled
baseline/active calibration run.

| Camera | Codec / size | Frames | MV frames | Vectors | Nonzero vectors | Accepted by current squared-magnitude limits | `motion_scale` |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | H.264 1920×1080 | 150 | 146 | 1,219,845 | 127,136 | 16,416 | 4 only |
| 3 | H.264 1920×1080 | 150 | 146 | 1,305,389 | 532,123 | 229,437 | 4 only |
| 4 | H.264 1920×1080 | 150 | 146 | 1,202,864 | 224,005 | 85,746 | 4 only |
| 5 | H.264 1920×1080 | 150 | 146 | 1,195,424 | 132,536 | 15,348 | 4 only |
| 7 | H.264 1920×1080 | 150 | 146 | 1,197,406 | 128,583 | 57,996 | 4 only |

All five have the same encoder vector precision. The current filter uses
`motion_x² + motion_y²` and accepts values from 4 through 128000. Dividing
each vector component by `motion_scale` converts to pixel displacement, so for
these cameras the equivalent squared-pixel range is **0.25 to 8000**. This
conversion should preserve the current Hikvision behaviour if normalization
is implemented. Other vector-sum/bucket thresholds must be converted by the
same squared-scale factor as well; changing only the per-vector gate would
change behaviour.

The large count differences are scene/instant dependent and do not establish
whether these cameras have equivalent motion sensitivity. `motion_scale` alone
cannot normalize block size, frame rate, vector density, masks or camera scene.
The follow-up Tapo and Reolink sample is documented in
`Tools/MOTION_VECTOR_PROVIDER_COMPARISON_2026-09-23.md`. The remaining work is
to collect labelled quiet/active clips per provider and replay identical input
through old and normalized scoring to verify equivalent Hikvision decisions.

The reproducible sampler is `Tools/MotionVectorScaleProbe.cpp`. Compile it
against the project's existing vcpkg FFmpeg installation. Pass a camera ID as
the sole command-line argument and the RTSP URL via
`WITNESS_MV_PROBE_URL`; output never includes the URL. A 5-second RTSP socket
timeout limits stalled reads. Do not commit URLs or credentials.
