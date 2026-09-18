[CmdletBinding()]
param(
    [ValidateSet("Release", "RelWithDebInfo")]
    [string]$Configuration = "RelWithDebInfo",

    [ValidateRange(1, 64)]
    [int]$Jobs = 18
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$bundledCmake = Join-Path $env:ProgramFiles "Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$cmake = if ($env:WITNESS_CMAKE) {
    $env:WITNESS_CMAKE
} elseif (Test-Path -LiteralPath $bundledCmake) {
    $bundledCmake
} else {
    (Get-Command cmake -ErrorAction Stop).Source
}

$env:VCPKG_MAX_CONCURRENCY = $Jobs.ToString()
Push-Location $repositoryRoot
try {
    & $cmake --preset windows-vs2026-stable
    if ($LASTEXITCODE -ne 0) {
        throw "Configure failed with exit code $LASTEXITCODE."
    }
    & $cmake --build build-vs2026-stable --config $Configuration --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}
