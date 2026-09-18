[CmdletBinding()]
param(
    [ValidateRange(1, 64)]
    [int]$Concurrency = 18,

    [string]$Preset = "windows-vs2026-stable"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$environment = Get-Content (Join-Path $repositoryRoot "build-environment.json") -Raw | ConvertFrom-Json
$localVcpkgRoot = Join-Path $repositoryRoot ".build-tools\vcpkg"
$vcpkgCandidates = @($localVcpkgRoot)
if ($env:VCPKG_ROOT) {
    $vcpkgCandidates += $env:VCPKG_ROOT
}
$vcpkgHint = Join-Path $env:LOCALAPPDATA "vcpkg\vcpkg.path.txt"
if (Test-Path -LiteralPath $vcpkgHint) {
    $vcpkgCandidates += (Get-Content -LiteralPath $vcpkgHint -Raw).Trim()
}
$vcpkgRoot = $null
foreach ($candidate in $vcpkgCandidates | Select-Object -Unique) {
    $executable = Join-Path $candidate "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $executable)) {
        continue
    }
    $version = (& $executable version | Out-String).Trim()
    if ($LASTEXITCODE -eq 0 -and $version.Contains($environment.vcpkgCommit)) {
        $vcpkgRoot = $candidate
        break
    }
}
if (-not $vcpkgRoot) {
    throw "No vcpkg installation matches pinned commit $($environment.vcpkgCommit). Run scripts/bootstrap-vcpkg.ps1 first."
}
$env:VCPKG_ROOT = $vcpkgRoot
$bundledCmake = Join-Path $env:ProgramFiles "Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$cmake = if ($env:WITNESS_CMAKE) {
    $env:WITNESS_CMAKE
} elseif (Test-Path -LiteralPath $bundledCmake) {
    $bundledCmake
} else {
    (Get-Command cmake -ErrorAction Stop).Source
}
$cacheRoot = if ($env:WITNESS_VCPKG_BINARY_CACHE) {
    $env:WITNESS_VCPKG_BINARY_CACHE
} else {
    Join-Path $env:LOCALAPPDATA "Witness\vcpkg-binary-cache"
}

New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
$env:VCPKG_BINARY_SOURCES = "clear;files,$cacheRoot,readwrite"
$env:VCPKG_MAX_CONCURRENCY = $Concurrency.ToString()

Write-Host "Witness dependency bootstrap"
Write-Host "  cmake:       $cmake"
Write-Host "  vcpkg:      $vcpkgRoot ($($environment.vcpkgVersion))"
Write-Host "  preset:       $Preset"
Write-Host "  binary cache: $cacheRoot"
Write-Host "  concurrency:  $Concurrency"
Write-Host ""
Write-Host "This is the only supported command that installs or updates managed dependencies."

Push-Location $repositoryRoot
try {
    & $cmake --preset $Preset -DWITNESS_BOOTSTRAP_DEPENDENCIES=ON
    if ($LASTEXITCODE -ne 0) {
        throw "Dependency bootstrap failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Dependencies are ready. Routine configure will now run without package mutation."
Write-Host "Build with: .\scripts\build-windows.ps1"
