<#
.SYNOPSIS
    Builds the local components/bt from the pinned ESP-IDF plus this project's patches.

.DESCRIPTION
    The stock Bluedroid A2DP sink speaks SBC and nothing else, so AAC and aptX need
    changes inside the stack itself. Rather than vendoring a whole IDF tree, this
    project keeps the delta as patches and rebuilds a local copy of the bt component
    from the shared IDF on demand. See docs/BASE.md.

    The shared IDF at $env:IDF_PATH is never written to. It is read, copied out of,
    and left exactly as it was found.

.PARAMETER Clean
    Discard an existing components/bt and copy it again. Without this the script
    refuses to touch a copy that is already there, because patches are not idempotent.

.EXAMPLE
    . C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
    .\tools\setup.ps1
#>

[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$btLocal  = Join-Path $repoRoot 'components\bt'
$patchDir = Join-Path $repoRoot 'patches'
$pinFile  = Join-Path $repoRoot 'idf.pin'

function Fail($message) {
    Write-Host ''
    Write-Host "setup: $message" -ForegroundColor Red
    exit 1
}

# --- 1. The pinned IDF must be the one that is active -------------------------

if (-not $env:IDF_PATH) {
    Fail 'IDF_PATH is not set. Activate ESP-IDF first (see CLAUDE.md, section Збірка).'
}
if (-not (Test-Path $pinFile)) {
    Fail "idf.pin not found at $pinFile"
}

$pin = @{}
foreach ($line in Get-Content $pinFile) {
    if ($line -match '^\s*([a-z]+)\s*=\s*(\S+)\s*$') { $pin[$Matches[1]] = $Matches[2] }
}
if (-not $pin.version -or -not $pin.commit) {
    Fail 'idf.pin must define both version= and commit=.'
}

$versionFile = Join-Path $env:IDF_PATH 'version.txt'
$actualVersion = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { '<no version.txt>' }
if ($actualVersion -ne $pin.version) {
    Fail "IDF_PATH is $actualVersion, idf.pin wants $($pin.version). Patches are cut against $($pin.version) only."
}

# The tag can be moved; the commit cannot. Check it when the tree is a git checkout.
$actualCommit = & git -C $env:IDF_PATH rev-parse HEAD 2>$null
if ($LASTEXITCODE -eq 0) {
    if ($actualCommit.Trim() -ne $pin.commit) {
        Fail "IDF_PATH is at commit $($actualCommit.Trim()), idf.pin wants $($pin.commit)."
    }
} else {
    Write-Host 'setup: IDF_PATH is not a git checkout, commit not verified.' -ForegroundColor Yellow
}

Write-Host "setup: IDF $($pin.version) at $env:IDF_PATH — matches idf.pin." -ForegroundColor Green

# --- 2. Copy components/bt out of the shared tree -----------------------------

$btSource = Join-Path $env:IDF_PATH 'components\bt'
if (-not (Test-Path $btSource)) {
    Fail "No bt component at $btSource."
}

if (Test-Path $btLocal) {
    if (-not $Clean) {
        Fail 'components/bt already exists. Re-run with -Clean to rebuild it: patches do not apply twice.'
    }
    Write-Host 'setup: removing the old components/bt ...'
    Remove-Item -Recurse -Force $btLocal
}

New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot 'components') | Out-Null
Write-Host 'setup: copying components/bt (about 71 MB, takes a moment) ...'
Copy-Item -Recurse -Force $btSource $btLocal

# --- 3. Apply our patches on top ----------------------------------------------

# Note on --3way: docs/BASE.md asks for it, but it cannot work here. components/bt
# is in .gitignore, so its blobs are not in this repository's object database and
# git has nothing to merge against. Plain `git apply` is used instead; it fails
# loudly on the first hunk that does not fit, which is the property we actually
# wanted. A patch that no longer applies is a signal, not an inconvenience.
$patches = @()
if (Test-Path $patchDir) {
    $patches = @(Get-ChildItem -Path $patchDir -Filter '*.patch' | Sort-Object Name)
}

if ($patches.Count -eq 0) {
    Write-Host 'setup: no patches yet — this is stock Bluedroid, which means SBC only.' -ForegroundColor Yellow
} else {
    foreach ($patch in $patches) {
        Write-Host "setup: applying $($patch.Name) ..."
        & git -C $repoRoot apply --directory=components/bt --whitespace=nowarn -- $patch.FullName
        if ($LASTEXITCODE -ne 0) {
            Fail "$($patch.Name) did not apply. Do not hand-edit components/bt — fix the patch and re-run with -Clean."
        }
    }
    Write-Host "setup: $($patches.Count) patch(es) applied." -ForegroundColor Green
}

# --- 4. Say what to do next ----------------------------------------------------

Write-Host ''
Write-Host 'setup: done. Next:' -ForegroundColor Green
Write-Host '    idf.py set-target esp32'
Write-Host '    idf.py build'
