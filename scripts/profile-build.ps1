[CmdletBinding()]
param(
    [switch]$Clean,

    [string]$BuildRoot = "",

    [switch]$UseExistingConfiguration,

    [switch]$KeepProfilingConfiguration,

    [ValidateRange(1, 64)]
    [int]$Jobs = 18,

    [ValidateSet("level1", "level2", "level3")]
    [string]$TraceLevel = "level3"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outputRoot = Join-Path $repositoryRoot "artifacts\build-profile-$timestamp"
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $repositoryRoot "build-vs2026-stable"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildRoot)) {
    $BuildRoot = Join-Path $repositoryRoot $BuildRoot
}
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$repositoryPrefix = [System.IO.Path]::GetFullPath($repositoryRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if ($Clean -and -not $BuildRoot.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Clean profiling requires a build directory beneath $repositoryRoot."
}
if (-not $UseExistingConfiguration -and $BuildRoot -ne (Join-Path $repositoryRoot 'build-vs2026-stable')) {
    throw "A custom BuildRoot requires -UseExistingConfiguration."
}
$bundledRoot = Join-Path $env:ProgramFiles "Microsoft Visual Studio\18\Insiders"
$cmake = if ($env:WITNESS_CMAKE) {
    $env:WITNESS_CMAKE
} else {
    Join-Path $bundledRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
$vcperf = if ($env:WITNESS_VCPERF) {
    $env:WITNESS_VCPERF
} else {
    Join-Path $bundledRoot "VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\vcperf.exe"
}
if (-not (Test-Path -LiteralPath $vcperf)) {
    $vcperf = Get-ChildItem (Join-Path $bundledRoot "VC\Tools\MSVC") -Filter vcperf.exe -Recurse |
        Where-Object FullName -Like "*Hostx64*x64*" |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $vcperf)) {
    throw "VS2026 CMake and vcperf are required for build profiling."
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$session = "WitnessBuild-$PID-$timestamp"
$trace = Join-Path $outputRoot "build-timetrace.json"
$binlog = Join-Path $outputRoot "build.binlog"
$transcript = Join-Path $outputRoot "build-output.txt"
$sessionTranscript = Join-Path $outputRoot "session-output.txt"
$traceStarted = $false
$configurationChanged = $false
$traceStoppedSuccessfully = $true
$restoreSucceeded = $true
$previousProfileSetting = "OFF"
$cachePath = Join-Path $BuildRoot "CMakeCache.txt"
if (Test-Path -LiteralPath $cachePath) {
    $profileEntry = Select-String -LiteralPath $cachePath -Pattern '^WITNESS_PROFILE_BUILD:BOOL=(ON|OFF)$'
    if ($profileEntry) { $previousProfileSetting = $profileEntry.Matches[0].Groups[1].Value }
}

Start-Transcript -Path $sessionTranscript | Out-Null
Push-Location $repositoryRoot
try {
    if ($UseExistingConfiguration) {
        if (-not (Test-Path -LiteralPath (Join-Path $BuildRoot "CMakeCache.txt"))) {
            throw "No existing CMake configuration was found at $BuildRoot."
        }
        $configurationChanged = $true
        & $cmake -S $repositoryRoot -B $BuildRoot `
            -DWITNESS_PROFILE_BUILD=ON -DWITNESS_BOOTSTRAP_DEPENDENCIES=OFF -DVCPKG_MANIFEST_INSTALL=OFF
    } else {
        $configurationChanged = $true
        & $cmake --preset windows-vs2026-stable -DWITNESS_PROFILE_BUILD=ON `
            -DWITNESS_BOOTSTRAP_DEPENDENCIES=OFF -DVCPKG_MANIFEST_INSTALL=OFF
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Profile configure failed with exit code $LASTEXITCODE."
    }
    if ($Clean) {
        & $cmake --build $buildRoot --config RelWithDebInfo --target clean
        if ($LASTEXITCODE -ne 0) {
            throw "Clean failed with exit code $LASTEXITCODE."
        }
    }

    & $vcperf /start /noadmin "/$TraceLevel" $session
    if ($LASTEXITCODE -ne 0) {
        throw "vcperf could not start trace session $session."
    }
    $traceStarted = $true

    # Native output must be captured explicitly: PowerShell transcripts do not
    # reliably record redirected native compiler output. The timing switches
    # emit millions of lines, so keep them out of the interactive terminal.
    & $cmake --build $buildRoot --config RelWithDebInfo --parallel $Jobs -- "/bl:$binlog" /nr:false /v:minimal *> $transcript
    if ($LASTEXITCODE -ne 0) {
        throw "Profiled build failed with exit code $LASTEXITCODE."
    }
} finally {
    if ($traceStarted) {
        & $vcperf /stop $session /timetrace $trace
        $traceStoppedSuccessfully = $LASTEXITCODE -eq 0
        if (-not $traceStoppedSuccessfully) { Write-Warning "vcperf could not finalize the trace." }
    }
    if ($configurationChanged -and -not $KeepProfilingConfiguration) {
        & $cmake -S $repositoryRoot -B $BuildRoot `
            "-DWITNESS_PROFILE_BUILD=$previousProfileSetting" `
            -DWITNESS_BOOTSTRAP_DEPENDENCIES=OFF -DVCPKG_MANIFEST_INSTALL=OFF
        $restoreSucceeded = $LASTEXITCODE -eq 0
        if (-not $restoreSucceeded) { Write-Warning "Could not restore the previous profiling configuration." }
    }
    Pop-Location
    Stop-Transcript | Out-Null
}

if (-not $traceStoppedSuccessfully -or -not $restoreSucceeded) {
    throw "Profiling did not finalize successfully; inspect $sessionTranscript."
}

Write-Host "Build profile written to $outputRoot"
Write-Host "  build.binlog: inspect with MSBuild Structured Log Viewer"
Write-Host "  build-timetrace.json: open with edge://tracing"
Write-Host "  build-output.txt: /Bt+ and /d1reportTime compiler timings"
Write-Host "  session-output.txt: configure, trace collection and configuration restoration"
