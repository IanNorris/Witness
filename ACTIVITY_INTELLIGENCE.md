# Activity intelligence design

This document turns the activity-quality backlog into staged work. The first
release should group repetitive activity without making identity claims or
silently deleting evidence. Persistent-object reasoning should follow only
after Witness can represent what was actually observed.

## Product boundaries

Keep three concepts separate:

1. **Similarity grouping** says that clips look like the same recurring event.
2. **User relevance** says that an operator wants fewer events like an example.
3. **Object persistence** says that an object appears to remain, move, or leave.

A group is not an identity, and marking a group uninteresting must not delete
recordings. All derived decisions must be reversible and retain their reason,
algorithm version, and supporting observations.

## Prerequisite: preserve observation truth

The current pipeline cannot reliably infer disappearance or continued
presence:

- `ObjectTracker` associates greedily by IoU without class gating, expires
  tracks after update calls rather than observed time, and restarts IDs at 1.
- Live and reprocessing paths instantiate separate trackers, so their IDs are
  not interchangeable.
- Idle-baseline detections are removed from results, and motion-time detections
  matching the baseline are suppressed.
- Only non-empty analysis results are published. Downstream code therefore
  cannot distinguish an analyzed-empty frame from skipped, failed, or filtered
  analysis.
- Stored detections currently lose the upstream baseline decision.

Before persistence work, introduce an `AnalysisObservation` record containing
camera and scene epoch, capture time, source/run/model versions, status
(`complete`, `empty`, `skipped`, or `failed`), examined regions, quality, and
baseline annotations. Relevance filtering should consume observations rather
than erase them. Full decoded frames do not need to be retained.

## Stage 1: cheap, high-precision grouping

Run grouping as bounded optional post-processing. Reuse analysis frames or
thumbnails already decoded, and pause it when essential queues are pressured.

Create a versioned descriptor from early, highest-interest, and late moments:

- full-scene perceptual and block-mean hashes;
- a small spatial grid of change magnitude and direction;
- object class counts, normalized occupied regions, displacement, confidence;
- hashes of object crops and wider context crops;
- analysis-coverage and media-quality flags.

OpenCV already provides inexpensive pHash and block-mean implementations.
These are near-duplicate signals, not semantic proof:
https://docs.opencv.org/4.12.0/d4/d93/group__img__hash.html

Candidate clips must initially share camera, scene-layout epoch, compatible
day/night mode and event summary, nearby time, adequate coverage, and no
important new object or transition. Compare each candidate with a bounded set
of group representatives, not only the previous clip; pairwise chaining can
otherwise join unrelated endpoints.

Suggested starting limits for live evaluation are a 60-second inter-clip gap,
a ten-minute maximum group span, and fifty clips per group. These are product
defaults to measure, not established thresholds. Choose a medoid or the
highest-interest frame as representative and enforce a maximum member distance.

The UI should render one expandable activity card with the representative,
count, first/last time, object classes, and meaningful changes. Every original
clip remains independently addressable. This is separate from physical clip
coalescing.

## Stage 2: user-labelled routine patterns

Start with camera-local exemplar rules instead of training a classifier:

- **Hide this group** affects only the selected group.
- **Show fewer like this on this camera** creates a suggested recurring rule.
- **Always show this class/object/zone transition** is an explicit exclusion.

Store exemplars, positive counterexamples, distance and zone constraints,
scene epoch, provenance, version, and expiry. Preview matches before enabling a
rule, begin in suggestion/shadow mode, and expose undo plus a searchable
routine/hidden view. Frequency alone must never imply irrelevance.

A cobweb rule must not hide a person behind the web. A parked-car rule may
de-emphasize unchanged presence, but not arrival, departure, movement, or a
person approaching it.

## Stage 3: usual-place and persistence reasoning

First make tracking class-compatible, timestamp-aware, deterministic, and
explicit about unobserved gaps. A timestamp-aware IoU cost with Hungarian
assignment is the smallest useful upgrade. ByteTrack and OC-SORT are later
comparison candidates, not prerequisites:

- ByteTrack: https://arxiv.org/abs/2110.06864
- OC-SORT: https://arxiv.org/abs/2203.14360

Separate short continuous tracklets from persistent scene objects. Persistent
links carry confidence and may remain unresolved. Use states such as:

`first observed -> present/moving -> stationary -> temporarily unobserved -> last seen`

Confirm departure only after later usable observations cover the relevant
region. Downtime, occlusion, dropped AI work, and corrupt media imply unknown,
not disappearance.

For usual locations, maintain camera-local class/zone occupancy from observed
time, with dwell, movement, recurrence, and evidence counts. Use a decayed grid
or small spatial prototypes; separate day/night regimes and version/reset the
model after camera movement or crop changes. Background subtraction may help
with change descriptors but cannot be object memory because stationary objects
are eventually absorbed into the background.

## Optional embeddings

Only benchmark learned embeddings if the cheap descriptor misses a measured
requirement. MobileCLIP2-S0 is the first compact contextual candidate; DINOv2
or DINOv3 small models are comparison baselines. Evaluate object crops and
context crops separately: semantic embeddings can incorrectly merge different
cars in the same driveway. Do not start with video-language models, continuous
per-frame embeddings, or segmentation.

## Data model

- `AnalysisObservation`: immutable capture-time evidence and coverage.
- `Tracklet`: scoped ID, class distribution, trajectory, times, confidence.
- `SceneObject`: optional linked tracklets, spatial prototype, state, link confidence.
- `ActivityDescriptor`: clip UID, extractor version, sampled times, features, coverage.
- `ActivityGroup` and membership: representative, span, algorithm version, reasons.
- `RelevanceRule`: owner/scope, exemplars, counterexamples, exclusions, expiry.
- `ActivityTransition`: first-observed, moved, stationary, last-seen, and uncertainty.

Derived summaries should be rebuildable. Reprocessing must not overwrite user
labels or mix offline tracker identifiers with live ones.

## Acceptance and rollout

Ship observation coverage first, then grouping in shadow mode. Measure:

- important-event false grouping, not just card-count reduction;
- hidden-important-event rate using a curated suite and live audit sample;
- transition precision/recall, false disappearances per camera-day, timestamp
  error, track fragmentation, and identity switches;
- descriptor CPU time, optional-queue latency, memory/database growth, and
  essential-queue p95 before and after.

The regression suite should include grass, cobwebs, rain, shadows, headlights,
IR transitions, camera movement, corruption, outages, a parked car moving, a
person crossing a routine zone, similar objects, occlusion, stationary people,
and detector-empty/skipped frames.

