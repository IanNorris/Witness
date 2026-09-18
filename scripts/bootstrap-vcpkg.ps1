[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$environment = Get-Content (Join-Path $repositoryRoot "build-environment.json") -Raw | ConvertFrom-Json
$toolsRoot = Join-Path $repositoryRoot ".build-tools"
$vcpkgRoot = Join-Path $toolsRoot "vcpkg"

New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $vcpkgRoot ".git"))) {
    & git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $vcpkgRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Could not clone vcpkg."
    }
}

Push-Location $vcpkgRoot
try {
    & git fetch origin $environment.vcpkgCommit --depth 1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not fetch pinned vcpkg commit $($environment.vcpkgCommit)."
    }
    & git checkout --detach $environment.vcpkgCommit
    if ($LASTEXITCODE -ne 0) {
        throw "Could not check out pinned vcpkg commit."
    }
    & .\bootstrap-vcpkg.bat -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        throw "Could not bootstrap pinned vcpkg."
    }
} finally {
    Pop-Location
}

Write-Host "Pinned vcpkg is ready at $vcpkgRoot"
