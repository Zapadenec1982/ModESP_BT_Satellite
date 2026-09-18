<#
.SYNOPSIS
    Builds and runs the link-layer tests on this machine, with no board involved.

.DESCRIPTION
    components/satlink has no ESP-IDF dependencies precisely so that this is
    possible: framing bugs are far cheaper to find in a terminal than over a UART.

.PARAMETER Cc
    The host C compiler. The directory it lives in is prepended to PATH, because a
    MSYS2 gcc cannot find its own cc1 without it and fails with no message at all.
#>

[CmdletBinding()]
param(
    [string]$Cc = 'C:\msys64\ucrt64\bin\gcc.exe'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$outDir   = Join-Path $repoRoot 'build\host'
$exe      = Join-Path $outDir 'test_satlink.exe'

if (-not (Test-Path $Cc)) {
    Write-Host "run_host_tests: no C compiler at $Cc. Pass -Cc <path to gcc>." -ForegroundColor Red
    exit 1
}
$env:PATH = "$(Split-Path -Parent $Cc);$env:PATH"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$sources = @(
    (Join-Path $repoRoot 'tests\host\main.c'),
    (Join-Path $repoRoot 'tests\host\test_satlink.c'),
    (Join-Path $repoRoot 'tests\host\test_messages.c'),
    (Join-Path $repoRoot 'components\satlink\cobs.c'),
    (Join-Path $repoRoot 'components\satlink\crc16.c'),
    (Join-Path $repoRoot 'components\satlink\frame.c'),
    (Join-Path $repoRoot 'components\satlink\codec.c'),
    (Join-Path $repoRoot 'components\satlink\messages.c')
)

$cflags = @(
    '-std=c17', '-Wall', '-Wextra', '-Werror',
    '-g', '-O1',
    "-I$(Join-Path $repoRoot 'components\satlink\include')",
    "-I$(Join-Path $repoRoot 'tests\host')"
)

# The undefined-behaviour sanitizer is worth having and not worth requiring: a
# MSYS2 gcc often ships without libubsan, and refusing to run the tests over a
# missing optional library would be the wrong trade. Probe, then decide.
$probe = Join-Path $outDir 'ubsan_probe.c'
Set-Content -Path $probe -Encoding ascii -Value 'int main(void){return 0;}'
# Start-Process rather than the call operator: a failing probe writes to stderr,
# and PowerShell turns native stderr into a terminating error under this
# preference. Here a failure is the answer, not a problem.
$probeRun = Start-Process -FilePath $Cc -NoNewWindow -Wait -PassThru `
    -ArgumentList @('-fsanitize=undefined', $probe, '-o', (Join-Path $outDir 'ubsan_probe.exe')) `
    -RedirectStandardError (Join-Path $outDir 'ubsan_probe.err') `
    -RedirectStandardOutput (Join-Path $outDir 'ubsan_probe.out')
if ($probeRun.ExitCode -eq 0) {
    $cflags += @('-fsanitize=undefined', '-fno-sanitize-recover=all')
} else {
    Write-Host 'run_host_tests: no libubsan here, running without the sanitizer.' -ForegroundColor Yellow
}

& $Cc @cflags @sources -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host 'run_host_tests: compilation failed.' -ForegroundColor Red
    exit 1
}

& $exe
exit $LASTEXITCODE
