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

.NOTES
    Deliberately pure ASCII. Windows PowerShell 5.1 reads a .ps1 without a byte
    order mark in the system codepage, not UTF-8, so a stray em dash in a string
    is enough to make the whole file fail to parse. It did, for a while.

.PARAMETER Clean
    Discard an existing components/bt and copy it again. Without this the script
    refuses to touch a copy that is already there, because patches are not idempotent.

.PARAMETER WithLhdc
    Also fetch the LHDC V5 decoder named in lhdc.pin into components/lhdcv5_decoder.
    It is not carried in this repository; lhdc.pin says why. The build uses it only
    with sdkconfig.lhdc.

.EXAMPLE
    . C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
    .\tools\setup.ps1
#>

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$WithLhdc
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$btLocal  = Join-Path $repoRoot 'components\bt'
$patchDir = Join-Path $repoRoot 'patches'
$pinFile  = Join-Path $repoRoot 'idf.pin'
$lhdcLocal = Join-Path $repoRoot 'components\lhdcv5_decoder'
$lhdcPin   = Join-Path $repoRoot 'lhdc.pin'

function Fail($message) {
    Write-Host ''
    Write-Host "setup: $message" -ForegroundColor Red
    exit 1
}

# --- 1. The pinned IDF must be the one that is active -------------------------

if (-not $env:IDF_PATH) {
    Fail 'IDF_PATH is not set. Activate ESP-IDF first (see CLAUDE.md, section Build).'
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

# version.cmake, not version.txt: the latter does not exist in an IDF checkout,
# which this script asserted against for a while and therefore never ran.
$versionFile = Join-Path $env:IDF_PATH 'tools/cmake/version.cmake'
if (-not (Test-Path $versionFile)) {
    Fail "No tools/cmake/version.cmake under $env:IDF_PATH. That does not look like an ESP-IDF."
}
$v = @{}
foreach ($line in Get-Content $versionFile) {
    if ($line -match '^\s*set\(IDF_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)\)') { $v[$Matches[1]] = $Matches[2] }
}
$actualVersion = "v$($v.MAJOR).$($v.MINOR).$($v.PATCH)"
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

Write-Host "setup: IDF $($pin.version) at $env:IDF_PATH - matches idf.pin." -ForegroundColor Green

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
    Write-Host 'setup: no patches yet - this is stock Bluedroid, which means SBC only.' -ForegroundColor Yellow
} else {
    foreach ($patch in $patches) {
        Write-Host "setup: applying $($patch.Name) ..."
        & git -C $repoRoot apply --directory=components/bt --whitespace=nowarn -- $patch.FullName
        if ($LASTEXITCODE -ne 0) {
            Fail "$($patch.Name) did not apply. Do not hand-edit components/bt - fix the patch and re-run with -Clean."
        }
    }
    Write-Host "setup: $($patches.Count) patch(es) applied." -ForegroundColor Green
}

# --- 4. Lay the new files on top ------------------------------------------------

# Patches carry changes to files that already exist. Files that are entirely ours
# -- the codec framework, the decoders, libfreeaptx, Helix -- live in bt_overlay/
# as real source and are copied in here.
#
# They are not in the patches on purpose. A patch that adds 15000 lines of a third
# party codec is unreadable, and it hides the licence headers that are the whole
# reason we care which codec came from where. As files they can be read, graded and
# diffed against upstream.
$overlay = Join-Path $repoRoot 'bt_overlay'
if (Test-Path $overlay) {
    $files = @(Get-ChildItem -Path $overlay -Recurse -File)
    foreach ($file in $files) {
        $rel = $file.FullName.Substring($overlay.Length + 1)
        $dest = Join-Path $btLocal $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Copy-Item -Force $file.FullName $dest
    }
    Write-Host "setup: $($files.Count) file(s) copied from bt_overlay." -ForegroundColor Green
} else {
    Write-Host 'setup: no bt_overlay - only the patches were applied.' -ForegroundColor Yellow
}

# --- 5. The LHDC V5 decoder, only when asked for --------------------------------

# Not carried in this repository; lhdc.pin says why. A shallow, blobless fetch of
# the pinned commit brings its trees and no file contents, and the checkout of the
# three paths that are built then downloads only theirs: about 2 MB of a 110 MB
# repository. Fetching by full commit id is itself the check that it is the pin.
if ($WithLhdc) {
    if (-not (Test-Path $lhdcPin)) {
        Fail "lhdc.pin not found at $lhdcPin"
    }
    $lpin = @{}
    foreach ($line in Get-Content $lhdcPin) {
        if ($line -match '^\s*([a-z]+)\s*=\s*(\S+)\s*$') { $lpin[$Matches[1]] = $Matches[2] }
    }
    if (-not $lpin.url -or -not $lpin.commit) {
        Fail 'lhdc.pin must define both url= and commit=.'
    }

    $fetchDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
    New-Item -ItemType Directory -Force -Path $fetchDir | Out-Null
    try {
        Write-Host "setup: fetching the LHDC V5 decoder at $($lpin.commit.Substring(0, 12)) from $($lpin.url) ..."
        & git -C $fetchDir init -q
        & git -C $fetchDir remote add origin $lpin.url
        & git -C $fetchDir fetch -q --depth 1 --filter=blob:none origin $lpin.commit
        if ($LASTEXITCODE -ne 0) {
            Fail "could not fetch $($lpin.commit) from $($lpin.url)."
        }
        & git -C $fetchDir checkout -q FETCH_HEAD -- CMakeLists.txt README.md decoder
        if ($LASTEXITCODE -ne 0) {
            Fail 'the pinned commit has no CMakeLists.txt, README.md and decoder/.'
        }

        if (Test-Path $lhdcLocal) {
            Remove-Item -Recurse -Force $lhdcLocal
        }
        New-Item -ItemType Directory -Force -Path $lhdcLocal | Out-Null
        foreach ($name in 'CMakeLists.txt', 'README.md', 'decoder') {
            Copy-Item -Recurse -Force (Join-Path $fetchDir $name) $lhdcLocal
        }
        $count = @(Get-ChildItem -Path $lhdcLocal -Recurse -File).Count
        Write-Host "setup: $count file(s) of the LHDC V5 decoder in components/lhdcv5_decoder." -ForegroundColor Green
    } finally {
        Remove-Item -Recurse -Force $fetchDir -ErrorAction SilentlyContinue
    }
} elseif (Test-Path $lhdcLocal) {
    Write-Host 'setup: components/lhdcv5_decoder, from an earlier -WithLhdc, left as it is.'
}

# --- 6. Say what to do next ----------------------------------------------------

Write-Host ''
Write-Host 'setup: done. Next:' -ForegroundColor Green
Write-Host '    idf.py set-target esp32'
Write-Host '    idf.py build'
if ($WithLhdc) {
    Write-Host 'setup: LHDC V5 is built only with sdkconfig.lhdc, from a fresh sdkconfig:' -ForegroundColor Green
    Write-Host '    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lhdc" set-target esp32'
}
