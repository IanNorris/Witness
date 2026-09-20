# Building Witness

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

The server still emits 296 warnings on that clean project rebuild, mostly
MSVC C4251 from exported camera implementation classes; the camera library
also emits C4251. Header-defined value types no longer export a class DLL
interface unnecessarily, but `InputStream`, `LiveOutputStream`,
`ContinuousOutputStream`, `OutputStream`, and several filters still need a
measured PIMPL/ABI migration. Do not replace this remaining work with a global
warning disable.
