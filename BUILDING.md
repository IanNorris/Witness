# Building Witness

## Deployment identities

Health reports and the console dashboard distinguish the web bundle ID from
the native source/toolchain fingerprint embedded independently in the server
EXE and camera DLL. The native fingerprint includes native sources/headers,
build definitions and dependency manifests, but excludes web assets. It is
generated on each build and its header changes only when those inputs change;
a web-only rebuild therefore leaves native binaries untouched. Native build
configuration is reported separately, and an EXE/camera-DLL fingerprint or
configuration mismatch produces a warning.

These fingerprints are not binary checksums or proof of the installed external
dependency contents. Use SHA-256 file hashes for byte-exact deployment audits.
Deploy the EXE, Witness DLLs and Web folder together; do not identify native
binaries solely by the web bundle ID or filesystem timestamps.

## Stable Windows build

The preferred Windows environment deliberately separates dependency changes
from ordinary configuration and compilation. It uses Visual Studio 2026's
latest installed `v145` compiler while vcpkg stays within the MSVC 14.51 ABI
family, builds third-party
dependencies once in Release mode, and ignores compiler executable churn from
compatible servicing updates.

Because this is a native x64-on-x64 build, vcpkg host tools and target
libraries use the same managed triplet. Otherwise code generators retain the
default compiler-tracked `x64-windows` identity and can still churn after an
IDE update.

Bootstrap dependencies explicitly on a new checkout or after an intentional
manifest, registry, triplet, or vcpkg update:

```powershell
.\scripts\bootstrap-vcpkg.ps1       # first checkout, or when the pin changes
.\scripts\bootstrap-dependencies.ps1
```

This creates `build-vs2026-stable` without touching older `build` or
`build-vs2026` trees. It uses a persistent binary cache at
`%LOCALAPPDATA%\Witness\vcpkg-binary-cache`; set
`WITNESS_VCPKG_BINARY_CACHE` to a shared absolute directory before running the
script to share packages with another machine or CI worker.

After bootstrap, normal work cannot install, remove, or rebuild dependencies:

```powershell
cmake --preset windows-vs2026-stable
.\scripts\build-windows.ps1
```

The scripts prefer Visual Studio 2026's bundled CMake because older CMake
installations do not recognise the VS 2026 generator. Set `WITNESS_CMAKE` to an
explicit executable to override it. Visual Studio itself can configure and
build the same preset normally.

If a pinned input changes, configure stops and names the bootstrap command. It
does not silently accept vcpkg's proposed package churn. Review the diff first,
record why the ABI or package graph changed, and run bootstrap only when that
change is intentional. A compiler servicing update within 14.51 is allowed; a
future compiler ABI family requires changing the triplet and bumping its
documented ABI epoch. Keeping a new triplet/build directory lets the old
environment remain usable until its replacement has successfully built.

The file `build-vs2026-stable/witness-dependency-environment.txt` records the
manifest, registry, triplet, vcpkg, CMake, compiler, generator, and toolset used
for the environment. The first four form the enforced dependency fingerprint;
the others make build reports diagnosable without making harmless IDE/CMake
updates mutate installed packages.

[`build-environment.json`](build-environment.json) is the reviewed source of
truth for the dependency ABI epoch, vcpkg commit, and supported MSVC family.
The dependency bootstrap accepts a matching `VCPKG_ROOT` or integrated vcpkg,
but prefers the ignored repository-local `.build-tools/vcpkg` installed by the
vcpkg bootstrap helper. A machine-wide vcpkg update therefore cannot silently
alter the managed environment.

## Existing and non-Windows builds

Existing build trees and the legacy presets continue to behave as before. The
managed policy is opt-in through `windows-vs2026-stable`, which avoids breaking
the current known-good dependency installation while the new release-only
environment is populated and tested.

Linux remains on the existing CMake/vcpkg path for now. The broader comparison
of CMake/vcpkg, generated native Visual Studio projects, Meson, and Conan is a
separate measured decision; the current build must remain available until an
alternative produces equivalent Windows and Linux artifacts.

## Build-time profiling

Do not restructure headers or add PIMPL solely on intuition. Once the stable
environment is built, capture an incremental profile and then a clean profile:

```powershell
.\scripts\profile-build.ps1
.\scripts\profile-build.ps1 -Clean
```

The script combines an MSBuild binary log, MSVC `/Bt+` and `/d1reportTime`
compiler timings, and a level-3 C++ Build Insights trace collected by
`vcperf /noadmin`. Results are placed under ignored `artifacts/build-profile-*`
directories. Open `build.binlog` in MSBuild Structured Log Viewer and
`build-timetrace.json` in `edge://tracing`. Use the measured translation units,
headers, templates, and build fan-out to prioritize ABI/PIMPL work.

The 2026-09-18 clean RelWithDebInfo baseline contained 69 C++ translation units
and 439 seconds of aggregate compiler front-end work versus 82 seconds in the
back end. The generated projects did not enable MSVC `/MP`, so the trace saw
only two compiler processes even when the build was launched with 18-way
parallelism. Witness now enables `/MP` for C++ compilation; CMake/MSBuild
parallelism continues to control concurrent projects. A subsequent clean
RelWithDebInfo build, including the web bundle and link, completed in 136
seconds on the same 12-logical-CPU development machine.

The next largest measured issue is repeated parsing of the exported camera
header graph. Typical server translation units spent about five seconds in the
front end, repeatedly reaching `OutputStream.h`, `Stream.h`, `RecordFilter.h`,
and `SourceStats.h`. Address that separately with forward declarations, PIMPL,
and then a measured PCH experiment; do not hide the associated C4251 ABI
warnings globally.

On 2026-09-19, isolating the shared segment-buffer type and removing the
`CameraWorker.h` dependency from `GlobalContext.h` let HTTP translation units
include only the camera implementation headers they use. A server-project
RelWithDebInfo rebuild then took 89 seconds (92 seconds after the value-type
export cleanup). A Crow/Asio-only PCH reduced the same project rebuild to 65
seconds on the same machine (about 29% less than the latter baseline); it
does not include camera headers or change the native dependency ABI. The
precompiled-header experiment is enabled for MSVC only.

On a fresh VS2026 RelWithDebInfo project rebuild on 2026-09-19, the Camera
DLL emitted 88 warnings and the server 296. A scoped ABI pass moved STL and
callback state from `InputStream`, `ContinuousOutputStream`, and the existing
`ReolinkUnofficialFilter` PIMPL behind DLL-owned pointers. It also made the
`ImageProcessingJobQueue` forward declaration agree with its `struct`
definition, eliminating C4099. Rebuilding both projects with the same command
and `/t:Rebuild` produced 61 Camera warnings and 193 server warnings (130
fewer in total), with no errors. The remaining compiler warnings are C4251,
concentrated in `LiveOutputStream` and `ReolinkBaichuanClient`. Those classes
own substantially more active streaming/network state and need their own
reviewed PIMPL migration and cross-DLL lifetime tests; do not suppress C4251
globally.

### Profiling safely in an existing dependency environment

Use `scripts/profile-build.ps1 -BuildRoot build-vs2026 -UseExistingConfiguration`
to measure the existing build without switching dependency installations. The
profiler explicitly disables dependency installation/bootstrap, captures verbose
native timing output in `build-output.txt`, and restores the previous
`WITNESS_PROFILE_BUILD` value even when the build fails. `session-output.txt`
records configuration and trace collection separately. Use
`-KeepProfilingConfiguration` only when intentionally retaining timing flags.
`-Clean` is restricted to a build directory beneath this repository; it cleans
project build outputs, not the installed vcpkg package tree.

Summarize a trace without printing captured environment variables or command
lines:

```powershell
python Tools/summarize_build_trace.py artifacts/build-profile-YYYYMMDD-HHMMSS/build-timetrace.json
python -m unittest discover -s Tools/Tests -p test_build_trace_summary.py
```

The 2026-10-04 clean VS2026 RelWithDebInfo profile recorded 79 front-end and
79 back-end passes across 78 distinct source paths, with no dropped trace
events or unmatched begin/end events. Aggregate front-end work was 629.1s and
back-end work 308.7s. These are overlapping compiler durations with heavy
profiling enabled, not build elapsed time or a comparable uninstrumented
baseline. All phase durations were attributed to a source path.

The largest individual phase totals were `CrowRoutes_Debug.cpp` (50.5s),
`CrowListener.cpp` (45.0s), and `CameraWorker.cpp` (36.4s). The Crow PCH took
17.7s. Repeated inclusive project-header timings put `RecordFilter.h` first
(84.2s over 37 visits), followed by `Stream.h` (50.0s over 26 visits) and
`SourceStats.h` (46.3s over 37 visits). Header timings include nested headers;
they cannot be added together or interpreted as the savings from removing
one include. This identifies candidates for a subsequent measured header/PIMPL
pass, not proof that a particular refactor is already faster.

Raw traces and binary logs may contain machine paths, environment variables,
and command lines. Keep those ignored artifacts local and review/redact them
before sharing. The summary omits these fields by design.
