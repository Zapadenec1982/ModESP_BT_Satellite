<#
.SYNOPSIS
    Packs everything needed to relink this firmware against a modified libfreeaptx.

.DESCRIPTION
    libfreeaptx is LGPL-2.1-or-later and is linked statically into libbt.a. The
    kit is what lets a recipient of the binary replace that library and link the
    same firmware again: every archive and object the final link consumed, the
    linker scripts, the link command as ninja ran it together with the response
    files it read, the image command, the configuration, the map file, the
    toolchain version and the source revision. It is produced from an existing
    build/ directory and never rebuilds anything.

    Output: build/relink-kit-<git sha>.zip, with a README.md inside that walks
    through the relink and a manifest.txt that maps every original path to its
    place in the kit.

.PARAMETER BuildDir
    The IDF build directory. Default: build.

.EXAMPLE
    . C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
    idf.py build
    .\tools\relink_kit.ps1
#>

[CmdletBinding()]
param(
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if ($BuildDir -eq '') { $BuildDir = Join-Path $repoRoot 'build' }
$BuildDir = (Resolve-Path $BuildDir).Path.TrimEnd('\')

function Fail($message) {
    Write-Host ''
    Write-Host "relink_kit: $message" -ForegroundColor Red
    exit 1
}

# --- 1. What was built, and from what -----------------------------------------

$elf = Get-ChildItem -Path $BuildDir -Filter '*.elf' | Select-Object -First 1
if (-not $elf) { Fail "no .elf in $BuildDir. Run idf.py build first." }
$project = [System.IO.Path]::GetFileNameWithoutExtension($elf.Name)

$ninja = Get-Command 'ninja' -ErrorAction SilentlyContinue
if (-not $ninja) { Fail 'ninja is not on PATH. Activate the IDF environment first.' }
$gcc = Get-Command 'xtensa-esp32-elf-gcc' -ErrorAction SilentlyContinue
if (-not $gcc) { Fail 'xtensa-esp32-elf-gcc is not on PATH. Activate the IDF environment first.' }
$toolchain = (& $gcc.Source --version | Select-Object -First 1)

# The link command as ninja runs it (the last command in the .elf target's
# chain) and the esptool elf2image step from the image target's chain.
$linkCmd = @(& $ninja.Source -C $BuildDir -t commands "$project.elf") | Select-Object -Last 1
if (-not $linkCmd) { Fail "ninja knows no link command for $project.elf" }
$imageCmd = @(& $ninja.Source -C $BuildDir -t commands 'gen_project_binary') | Where-Object { $_ -match 'elf2image' } | Select-Object -First 1

# The linker reads its inputs from a response file that ninja writes before the
# link and deletes after it, so the content has to be rebuilt from the build
# edge in build.ninja: rspfile_content is "$in $LINK_PATH $LINK_LIBRARIES" for
# CMake's Ninja generator.
function Unescape-Ninja([string]$s) {
    return $s.Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
}
$ninjaFile = Join-Path $BuildDir 'build.ninja'
if (-not (Test-Path $ninjaFile)) { Fail "no build.ninja in $BuildDir" }
$edgeVars = @{}
$edgeInputs = @()
$inEdge = $false
foreach ($line in [System.IO.File]::ReadLines($ninjaFile)) {
    if (-not $inEdge) {
        if ($line -match "^build $([regex]::Escape($project))\.elf:\s*\S+\s*(.*)$") {
            $inEdge = $true
            $rest = $Matches[1]
            $explicit = ($rest -split '\s\|\|?\s', 2)[0]
            if ($rest -match '^\|') { $explicit = '' }
            $edgeInputs = @(($explicit -split '\s+') | Where-Object { $_ -ne '' } | ForEach-Object { Unescape-Ninja $_ })
        }
        continue
    }
    if ($line -match '^  ([A-Z_]+) = (.*)$') { $edgeVars[$Matches[1]] = Unescape-Ninja $Matches[2]; continue }
    break
}
if (-not $inEdge) { Fail "no build edge for $project.elf in build.ninja" }
$linkPath      = $edgeVars['LINK_PATH']
$linkLibraries = $edgeVars['LINK_LIBRARIES']
$linkFlags     = $edgeVars['LINK_FLAGS']
$rspContent    = (@($edgeInputs) + @($linkPath) + @($linkLibraries)) -join ' '

$sha = (& git -C $repoRoot rev-parse --short=12 HEAD).Trim()
if ($LASTEXITCODE -ne 0) { Fail 'not a git checkout; the kit is named after the commit' }
$describe = (& git -C $repoRoot describe --always --dirty --long 2>$null)
$dirty = (& git -C $repoRoot status --porcelain --untracked-files=no)
if ($dirty) { Write-Host 'relink_kit: the working tree has uncommitted changes; recorded in the README.' -ForegroundColor Yellow }

$idfVersion = ''
$pin = Join-Path $repoRoot 'idf.pin'
if (Test-Path $pin) { $idfVersion = (Get-Content $pin | Where-Object { $_ -match '^(version|commit)=' }) -join ', ' }

# --- 2. Collect ---------------------------------------------------------------------

$kitName = "relink-kit-$sha"
$stage   = Join-Path $BuildDir $kitName
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$manifest = New-Object System.Collections.Generic.List[string]
$copied   = @{}

function Add-Kit([string]$source, [string]$relative) {
    $full = (Resolve-Path $source).Path
    if ($script:copied.ContainsKey($full)) { return }
    $dest = Join-Path $script:stage $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -Force $full $dest
    $script:copied[$full] = $relative
    $script:manifest.Add("$full -> $relative")
}

# Where a file goes: keyed by its path relative to the build directory, the IDF
# or this repository, so nothing collides and everything stays recognisable.
$idfRoot = ''
if ($env:IDF_PATH) { $idfRoot = [System.IO.Path]::GetFullPath($env:IDF_PATH).TrimEnd('\') }
function Kit-Path([string]$path, [string]$kind) {
    $p = [System.IO.Path]::GetFullPath($path)
    if ($p.StartsWith($BuildDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        return "$kind\build\" + $p.Substring($BuildDir.Length + 1)
    }
    if ($idfRoot -and $p.StartsWith($idfRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return "$kind\idf\" + $p.Substring($idfRoot.Length + 1)
    }
    if ($p.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return "$kind\repo\" + $p.Substring($repoRoot.Length + 1)
    }
    return "$kind\other\" + [System.IO.Path]::GetFileName($p)
}

function Resolve-Token([string]$token) {
    $t = $token.Trim('"')
    if ([System.IO.Path]::IsPathRooted($t)) { return $t }
    return Join-Path $BuildDir $t
}

# Everything the link names: archives and objects by path, more archives through
# -L/-l pairs, linker scripts through -T (bare names are looked up in the -L
# directories, which is what the linker does).
$tokens = @()
foreach ($text in @($linkFlags, $rspContent)) {
    $tokens += ([regex]::Matches($text, '"[^"]+"|\S+') | ForEach-Object { $_.Value })
}
$searchDirs = @()
foreach ($tok in $tokens) {
    if ($tok.Trim('"') -match '^-L(.+)$') { $searchDirs += (Resolve-Token $Matches[1]) }
}
function Find-InSearchDirs([string]$name) {
    $direct = Resolve-Token $name
    if (Test-Path $direct) { return $direct }
    foreach ($dir in $script:searchDirs) {
        $p = Join-Path $dir $name.Trim('"')
        if (Test-Path $p) { return $p }
    }
    return $null
}
$archives = 0; $objects = 0; $scripts = 0
for ($i = 0; $i -lt $tokens.Count; $i++) {
    $clean = $tokens[$i].Trim('"')
    if ($clean -match '\.a$') {
        $p = Find-InSearchDirs $clean
        if ($p) { Add-Kit $p (Kit-Path $p 'lib'); $archives++ }
    } elseif ($clean -match '\.(obj|o)$') {
        $p = Resolve-Token $clean
        if (Test-Path $p) { Add-Kit $p (Kit-Path $p 'obj'); $objects++ }
    } elseif ($clean -match '\.ld$') {
        $p = Find-InSearchDirs $clean
        if ($p) { Add-Kit $p (Kit-Path $p 'ld'); $scripts++ }
    } elseif ($clean -eq '-T' -and $i + 1 -lt $tokens.Count) {
        $p = Find-InSearchDirs $tokens[$i + 1]
        if ($p) { Add-Kit $p (Kit-Path $p 'ld'); $scripts++ }
        $i++
    } elseif ($clean -match '^-l(.+)$') {
        $p = Find-InSearchDirs "lib$($Matches[1]).a"
        if ($p) { Add-Kit $p (Kit-Path $p 'lib'); $archives++ }
    }
}

# The application's own objects, so main/ can be relinked without its sources.
foreach ($o in @(Get-ChildItem -Path (Join-Path $BuildDir 'esp-idf\main') -Recurse -Include '*.obj', '*.o')) {
    Add-Kit $o.FullName (Kit-Path $o.FullName 'obj'); $objects++
}

# The commands, the response files, the configuration and the evidence.
Set-Content -Path (Join-Path $stage 'link_command.txt') -Value $linkCmd -Encoding UTF8
Set-Content -Path (Join-Path $stage 'link_response.rsp') -Value $rspContent -Encoding UTF8
if ($imageCmd) { Set-Content -Path (Join-Path $stage 'image_command.txt') -Value $imageCmd -Encoding UTF8 }
foreach ($f in @(Get-ChildItem -Path (Join-Path $BuildDir 'toolchain') -File -ErrorAction SilentlyContinue)) {
    Add-Kit $f.FullName ('toolchain\' + $f.Name)
}
foreach ($name in @('sdkconfig', 'flasher_args.json', 'project_description.json', 'compile_commands.json')) {
    $p = Join-Path $BuildDir $name
    if (Test-Path $p) { Add-Kit $p $name }
}
$sdkconfigH = Join-Path $BuildDir 'config\sdkconfig.h'
if (Test-Path $sdkconfigH) { Add-Kit $sdkconfigH 'config\sdkconfig.h' }
foreach ($m in @(Get-ChildItem -Path $BuildDir -Filter '*.map')) { Add-Kit $m.FullName $m.Name }
Add-Kit $elf.FullName ('reference\' + $elf.Name)
$bin = Join-Path $BuildDir "$project.bin"
if (Test-Path $bin) { Add-Kit $bin "reference\$project.bin" }
if (Test-Path $pin) { Add-Kit $pin 'idf.pin' }

# Counted as files in the kit, not as mentions: the link names most archives
# several times over.
$archives = @($manifest | Where-Object { $_ -match ' -> lib\\' }).Count
$objects  = @($manifest | Where-Object { $_ -match ' -> obj\\' }).Count
$scripts  = @($manifest | Where-Object { $_ -match ' -> ld\\' }).Count

$btArchive = ($manifest | Where-Object { $_ -match 'libbt\.a$' } | Select-Object -First 1)
if ($btArchive) { $btArchive = ($btArchive -split ' -> ')[1] } else { $btArchive = '(libbt.a not found in the link inputs)' }

# --- 3. The README ---------------------------------------------------------------

$dirtyNote = ''
if ($dirty) { $dirtyNote = ' (working tree had uncommitted changes)' }
$readme = @"
# Relink kit for modesp-bt-satellite, commit $sha

Produced $(Get-Date -Format 'yyyy-MM-dd HH:mm') from the build directory of commit
``$describe``$dirtyNote.

The firmware links libfreeaptx (LGPL-2.1-or-later, see
``bt_overlay/host/bluedroid/external/libfreeaptx/main/COPYING`` in the source tree)
statically into ``libbt.a``. This kit contains everything the final link consumed, so
that the firmware can be linked again with a modified libfreeaptx and nothing else
from the original build.

| | |
|---|---|
| Toolchain | ``$toolchain`` |
| ESP-IDF | $idfVersion |
| Link command | ``link_command.txt``, exactly as ninja ran it (paths of the original machine) |
| Link inputs | ``link_response.rsp``: the response file the command reads (ninja deletes it after a successful link, so it is rebuilt here from the build edge); ``toolchain/cxxflags`` and ``toolchain/ldflags`` are the other two it names |
| Image command | ``image_command.txt``, the esptool elf2image step with the flash arguments used |
| Path map | ``manifest.txt``: every original path and where it is in this kit |
| Archives | ``lib/`` (${archives} in the link; libfreeaptx is inside ``${btArchive}``) |
| Objects | ``obj/`` (${objects}) |
| Linker scripts | ``ld/`` (${scripts}; generated ones under ``ld/build``, IDF ones under ``ld/idf``) |
| Configuration | ``sdkconfig``, ``config/sdkconfig.h`` |
| Evidence | ``*.map``, ``reference/`` (the original .elf and .bin), ``compile_commands.json`` |

## Relinking with a modified libfreeaptx

1. Install the toolchain named above. The ESP-IDF in ``idf.pin`` installs exactly
   it (``install.ps1`` / ``install.sh`` in that IDF), and the IDF is only needed for
   the toolchain and ``esptool.py``; every input of the link is in this kit.
2. Compile the modified ``freeaptx.c`` with the flags recorded for it in
   ``compile_commands.json`` (search for ``freeaptx.c``; the ``-I`` paths point at the
   original tree, point them at your copy of ``components/bt``). Keep the object
   name ``freeaptx.c.obj``.
3. Replace the member inside the archive:

       xtensa-esp32-elf-ar r $btArchive path/to/freeaptx.c.obj

   (``xtensa-esp32-elf-ar t $btArchive | findstr freeaptx`` shows the member as
   the build named it; ``ar r`` replaces a member by name.)
4. Run ``link_command.txt`` with its three response files pointed at this kit and
   every path inside them rewritten through ``manifest.txt``: archives to ``lib/``,
   objects to ``obj/``, ``-T`` scripts and ``-L`` directories to ``ld/``. The output
   is ``$project.elf``.
5. Produce the flashable image with ``image_command.txt`` (``esptool.py`` ships with
   the IDF; the ``--flash_*`` arguments must stay as recorded, they are also in
   ``flasher_args.json``).
6. Flash ``$project.bin`` to the application partition (offset in
   ``flasher_args.json``), leaving the bootloader and partition table as they are.

Compare the new map file against ``$project.map`` to confirm that only the
libfreeaptx symbols moved.
"@
Set-Content -Path (Join-Path $stage 'README.md') -Value $readme -Encoding UTF8
Set-Content -Path (Join-Path $stage 'manifest.txt') -Value $manifest -Encoding UTF8

# --- 4. Zip -----------------------------------------------------------------------

# .NET directly rather than Compress-Archive, which took minutes over this much
# data on Windows PowerShell 5.1. Entry by entry rather than CreateFromDirectory,
# which on .NET Framework names entries with backslashes that a Linux unzip
# turns into file names.
$zip = Join-Path $BuildDir "$kitName.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Add-Type -AssemblyName 'System.IO.Compression'
Add-Type -AssemblyName 'System.IO.Compression.FileSystem'
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in @(Get-ChildItem -Path $stage -Recurse -File)) {
        $entry = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Remove-Item -Recurse -Force $stage

$size = (Get-Item $zip).Length
Write-Host ("relink_kit: {0} ({1:N1} MB): {2} archives, {3} objects, {4} linker scripts" -f `
    $zip, ($size / 1MB), $archives, $objects, $scripts) -ForegroundColor Green
