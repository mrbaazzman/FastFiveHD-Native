#!/usr/bin/env pwsh
# Assemble the installer's build payload:
#   build/builder/toolchain/     minimal MinGW-w64
#   build/builder/fast5build.exe  the builder (translates and compiles the game)
#   build/builder/analysis/       the P-code dump of the analysed library
#   build/builder.zip
#
# The P-code dump travels with the payload so that the installer needs neither
# Ghidra nor a Java runtime on the end user's machine: the builder can go straight
# to the translation step.  It is the dump of one specific library, which the
# installer verifies against the APK's SHA-256.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/make_builder.ps1

param(
    [switch]$SkipToolchain,
    [string]$Out = "build"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $outDir = Join-Path $root $Out
    $toolchain = Join-Path $outDir 'toolchain'
    $builder = Join-Path $outDir 'fast5build.exe'

    if (-not $SkipToolchain) {
        & (Join-Path $PSScriptRoot 'build_toolchain.ps1') -Out (Join-Path $Out 'toolchain')
        if ($LASTEXITCODE -ne 0) { throw "toolchain build failed" }
    }
    if (-not (Test-Path $toolchain)) { throw "toolchain missing; run without -SkipToolchain" }

    if (-not (Test-Path $builder)) { throw "builder missing: $builder (build it first)" }

    $stage = Join-Path $outDir 'builder'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $stage | Out-Null

    Copy-Item $toolchain (Join-Path $stage 'toolchain') -Recurse -Force
    Copy-Item $builder (Join-Path $stage 'fast5build.exe') -Force

    # The analysed library's P-code, so the installer does not need Ghidra.  The zip
    # compresses it (it is mostly text) to a fraction of its size.
    $reports = Join-Path (Join-Path $stage 'analysis') 'reports'
    New-Item -ItemType Directory -Force $reports | Out-Null
    foreach ($f in @('pcode_all.tsv', 'pcode_extra.tsv')) {
        $src = Join-Path $root "analysis/reports/$f"
        if (-not (Test-Path $src)) { throw "missing $src - run the pipeline once first" }
        Copy-Item $src (Join-Path $reports $f) -Force
    }

    $zipPath = Join-Path $outDir 'builder.zip'
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -Force
    if (-not (Test-Path $zipPath)) { throw "builder.zip was not created" }

    Write-Host ("builder.zip: {0:N1} MB" -f ((Get-Item $zipPath).Length / 1MB))
}
finally {
    Pop-Location
}
