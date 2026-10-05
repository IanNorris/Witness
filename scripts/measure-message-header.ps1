[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$CompilerRoot,
    [Parameter(Mandatory)] [string]$SdkIncludeRoot,
    [Parameter(Mandatory)] [string]$InstalledRoot,
    [ValidateRange(1, 10)] [int]$Repeats = 3
)

# A local, no-PCH header probe. Never configures CMake, installs dependencies,
# deletes a build tree or writes into the compiler/SDK/installed-package roots.
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$compilerRootPath = (Resolve-Path -LiteralPath $CompilerRoot).Path
$sdkRootPath = (Resolve-Path -LiteralPath $SdkIncludeRoot).Path
$installedRootPath = (Resolve-Path -LiteralPath $InstalledRoot).Path
$compiler = Join-Path $compilerRootPath 'bin/Hostx64/x64/cl.exe'
$source = Join-Path $repositoryRoot 'Tools/Tests/MessageHeaderIsolationTests.cpp'
$externalIncludes = @(
    (Join-Path $compilerRootPath 'include'),
    (Join-Path $sdkRootPath 'ucrt'),
    (Join-Path $sdkRootPath 'shared'),
    (Join-Path $sdkRootPath 'um'),
    (Join-Path $installedRootPath 'include'),
    (Join-Path $installedRootPath 'include/opencv4')
)
$projectIncludes = @(
    (Join-Path $repositoryRoot 'WitnessServer/Source'),
    (Join-Path $repositoryRoot 'WitnessCamera/Include'),
    (Join-Path $repositoryRoot 'WitnessCommon/Include')
)
$includePaths = $projectIncludes + $externalIncludes
foreach ($path in @($compiler, $source) + $includePaths) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required probe path is missing: $path" }
}
$artifactRoot = Join-Path $repositoryRoot 'artifacts'
$outputRoot = Join-Path $artifactRoot ('message-header-probe-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $outputRoot | Out-Null
$common = @('/nologo', '/std:c++20', '/utf-8', '/EHsc', '/W3', '/WX', '/D_WINDOWS',
    '/DNDEBUG', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00', '/external:W0')
foreach ($path in $projectIncludes) { $common += '/I' + $path }
# Match CMake's system-header treatment; keep project warnings fatal without
# measuring diagnostic formatting for vendored Crow/SDK code as compile work.
foreach ($path in $externalIncludes) { $common += '/external:I' + $path }

$sizes = [ordered]@{}
foreach ($mode in @('isolated', 'contextControl')) {
    $arguments = @($common)
    if ($mode -eq 'contextControl') { $arguments += '/FIGlobalContext.h' }
    $arguments += @('/EP', $source)
    $generated = Join-Path $outputRoot ($mode + '.preprocessed.txt')
    $errorLog = Join-Path $outputRoot ($mode + '.preprocess-errors.txt')
    & $compiler @arguments > $generated 2> $errorLog
    if ($LASTEXITCODE -ne 0) { throw "Preprocessing failed: $mode; inspect $errorLog" }
    $sizes[$mode] = [ordered]@{
        bytes = (Get-Item -LiteralPath $generated).Length
        sha256 = (Get-FileHash -LiteralPath $generated -Algorithm SHA256).Hash
    }
}

$runs = @()
for ($iteration = 0; $iteration -lt $Repeats; $iteration++) {
    # Alternate order to avoid making every isolated run the warm-cache winner.
    $modes = if ($iteration % 2 -eq 0) { @('contextControl', 'isolated') } else { @('isolated', 'contextControl') }
    foreach ($mode in $modes) {
        $stem = Join-Path $outputRoot ("$iteration-$mode")
        $arguments = @($common)
        if ($mode -eq 'contextControl') { $arguments += '/FIGlobalContext.h' }
        $arguments += @('/c', '/O2', '/MD', '/Bt+', ('/Fo' + $stem + '.obj'), $source)
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        & $compiler @arguments *> ($stem + '.compile.txt')
        $exitCode = $LASTEXITCODE
        $timer.Stop()
        if ($exitCode -ne 0) { throw "Compilation failed: $mode; inspect $stem.compile.txt" }
        $runs += [ordered]@{ iteration = $iteration; mode = $mode; elapsedSeconds = $timer.Elapsed.TotalSeconds; exitCode = $exitCode }
        Write-Host "Probe $iteration $mode completed"
    }
}
$result = [ordered]@{
    schema = 1
    compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
    sourceSha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    repeats = $Repeats
    preprocessed = $sizes
    elapsedRuns = $runs
    limits = @(
        'Context control force-includes GlobalContext.h to model the removed transitive dependency.',
        'This no-PCH translation-unit probe does not represent the server Crow PCH or a full build.',
        'Elapsed durations include scheduling, filesystem/cache and other host activity; they are not CPU-work measurements.',
        'Preprocessed size is not token count. No runtime, stream, ABI or dependency-compatibility claim follows.'
    )
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputRoot 'summary.json')
Write-Host "Header probe written to $outputRoot"
