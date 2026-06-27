param(
    [string]$BuildDir = "build_ili9341",
    [switch]$Clean,
    [switch]$Merge,
    [switch]$NoMerge
)

$ErrorActionPreference = "Stop"

$Root = [string](Resolve-Path (Join-Path $PSScriptRoot ".."))
$ProjectDir = Join-Path $Root "make\esp-ili9341"
$ReleaseDir = Join-Path $Root "release\esp"
$BuildPath = Join-Path $ProjectDir $BuildDir
$FlashImage = Join-Path $ReleaseDir "flash_image_ILI9341.bin"
$AssetsResourceImage = Join-Path $Root "assets\resources.bin"
$ResourceImage = Join-Path $ReleaseDir "resources.bin"

function Add-MergePair {
    param(
        [string]$Offset,
        [string]$Source
    )

    $key = $Offset.ToLowerInvariant()
    if ((Test-Path $Source) -and !$script:mergeOffsets.ContainsKey($key)) {
        $script:mergeOffsets[$key] = $true
        $script:mergePairs += $Offset
        $script:mergePairs += $Source
    }
}

function Add-FlashPair {
    param(
        [string]$Offset,
        [string]$File
    )

    $key = $Offset.ToLowerInvariant()
    if (!$script:flashOffsets.ContainsKey($key)) {
        $script:flashOffsets[$key] = $true
        $script:flashPairs += [pscustomobject]@{ Offset = $Offset; File = $File }
    }
}

function Invoke-Checked {
    param(
        [string]$Description,
        [scriptblock]$Command
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function Invoke-GitQuiet {
    param(
        [string[]]$GitArgs
    )

    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & git @GitArgs >$null 2>$null
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

function Get-RelativePath {
    param(
        [string]$FromDirectory,
        [string]$ToPath
    )

    $from = (Resolve-Path -LiteralPath $FromDirectory).Path
    $to = (Resolve-Path -LiteralPath $ToPath).Path
    if (!$from.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $from += [System.IO.Path]::DirectorySeparatorChar
    }

    $fromUri = [System.Uri]::new($from)
    $toUri = [System.Uri]::new($to)
    return [System.Uri]::UnescapeDataString($fromUri.MakeRelativeUri($toUri).ToString()).Replace("/", [System.IO.Path]::DirectorySeparatorChar)
}

function Apply-SubmodulePatch {
    param(
        [string]$SubmodulePath,
        [string]$PatchPath
    )

    if (!(Test-Path $PatchPath)) {
        throw "Missing submodule patch: $PatchPath"
    }

    if (!(Test-Path $SubmodulePath)) {
        throw "Missing submodule path: $SubmodulePath"
    }

    $resolvedPatch = (Resolve-Path -LiteralPath $PatchPath).Path
    $displayPatch = Get-RelativePath $SubmodulePath $PatchPath
    $checkExit = Invoke-GitQuiet -GitArgs @("-C", $SubmodulePath, "apply", "--check", $resolvedPatch)
    if ($checkExit -eq 0) {
        Invoke-Checked "apply submodule patch $displayPatch" {
            git -C $SubmodulePath apply $resolvedPatch
        }
        return $true
    }

    $reverseCheckExit = Invoke-GitQuiet -GitArgs @("-C", $SubmodulePath, "apply", "--reverse", "--check", $resolvedPatch)
    if ($reverseCheckExit -eq 0) {
        Write-Host "submodule patch already applied: $displayPatch"
        return $false
    }

    throw "Submodule patch does not apply cleanly: $displayPatch"
}

function Restore-SubmodulePatch {
    param(
        [string]$SubmodulePath,
        [string]$PatchPath
    )

    $resolvedPatch = (Resolve-Path -LiteralPath $PatchPath).Path
    $displayPatch = Get-RelativePath $SubmodulePath $PatchPath
    Invoke-Checked "restore submodule patch $displayPatch" {
        git -C $SubmodulePath apply --reverse $resolvedPatch
    }
}

function Get-EsptoolCommand {
    $cmd = Get-Command esptool.py -ErrorAction SilentlyContinue
    $cmdPath = @($cmd.Path, $cmd.Source) | Where-Object { $_ } | Select-Object -First 1
    if ($cmdPath) {
        return [pscustomobject]@{
            Exe = $cmdPath
            Args = @()
        }
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    $pythonPath = @($python.Path, $python.Source) | Where-Object { $_ } | Select-Object -First 1
    if ($pythonPath) {
        return [pscustomobject]@{
            Exe = $pythonPath
            Args = @("-m", "esptool")
        }
    }

    throw "Neither esptool.py nor python was found. Open an ESP-IDF shell before running this script."
}

if (!(Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "idf.py was not found. Open an ESP-IDF shell or source export.ps1 before running this script."
}

if (!(Test-Path (Join-Path $Root "refs\tiny386\i386.c"))) {
    throw "refs/tiny386 is missing. Run git submodule update --init --recursive."
}

if (!(Test-Path (Join-Path $Root "src\esp\main\esp_main.c"))) {
    throw "src/esp/main sources are missing."
}

if ($Clean -and (Test-Path $BuildPath)) {
    Remove-Item -LiteralPath $BuildPath -Recurse -Force
}

$appliedPatches = @()
$emu8950Patch = Join-Path $Root "patches\submodules\0001-Place-ESP32-OPL-tables-in-external-BSS.patch"
$emu8950Path = Join-Path $Root "refs\emu8950"

if (Apply-SubmodulePatch $emu8950Path $emu8950Patch) {
    $appliedPatches += [pscustomobject]@{ SubmodulePath = $emu8950Path; PatchPath = $emu8950Patch }
}

Push-Location $ProjectDir
try {
    Invoke-Checked "idf.py build" { idf.py -B $BuildDir -DBOARD=ili9341 build }
}
finally {
    Pop-Location
    for ($i = $appliedPatches.Count - 1; $i -ge 0; $i--) {
        Restore-SubmodulePatch $appliedPatches[$i].SubmodulePath $appliedPatches[$i].PatchPath
    }
}

$flasherArgsPath = Join-Path $BuildPath "flasher_args.json"
if (!(Test-Path $flasherArgsPath)) {
    throw "Missing flasher args: $flasherArgsPath"
}

$flasherArgs = Get-Content $flasherArgsPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

$mergePairs = @()
$flashPairs = @()
$mergeOffsets = @{}
$flashOffsets = @{}
$flasherArgs.flash_files.PSObject.Properties | ForEach-Object {
    $offset = $_.Name
    $relativePath = $_.Value
    $source = Join-Path $BuildPath $relativePath
    if (!(Test-Path $source)) {
        throw "Missing flash artifact: $source"
    }

    $dest = Join-Path $ReleaseDir (Split-Path $relativePath -Leaf)
    Copy-Item -LiteralPath $source -Destination $dest -Force
    Add-MergePair $offset $source
    Add-FlashPair $offset (Split-Path $dest -Leaf)
}

$iniSource = Join-Path $ProjectDir "tiny386.ini"
if (!(Test-Path $iniSource)) {
    $iniSource = Join-Path $Root "refs\tiny386\esp\tiny386.ini"
}
if (Test-Path $iniSource) {
    Copy-Item -LiteralPath $iniSource -Destination (Join-Path $ReleaseDir "tiny386.ini") -Force
    Add-FlashPair "0x200000" "tiny386.ini"
}

foreach ($name in @("bios.bin", "vgabios.bin")) {
    $source = Join-Path $Root "release\$name"
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $ReleaseDir $name) -Force
        $offset = if ($name -eq "bios.bin") { "0x1d0000" } else { "0x1f0000" }
        Add-FlashPair $offset $name
    }
}

$dosSource = Join-Path $Root "release\dos.img"
if (Test-Path $dosSource) {
    Copy-Item -LiteralPath $dosSource -Destination (Join-Path $ReleaseDir "dos.img") -Force
    Add-FlashPair "0x210000" "dos.img"
}

$assetsDir = Join-Path $Root "assets"
$packResources = Join-Path $Root "script\pack-resources.py"
if ((Test-Path $assetsDir) -and (Test-Path $packResources)) {
    Invoke-Checked "pack resources" {
        python $packResources --assets $assetsDir --output $AssetsResourceImage --partition-size 0x100000
    }
    Copy-Item -LiteralPath $AssetsResourceImage -Destination $ResourceImage -Force
    Add-FlashPair "0xF00000" "resources.bin"
}

$flashArgsText = ($flashPairs | ForEach-Object { "$($_.Offset) $($_.File)" }) -join [Environment]::NewLine
Set-Content -LiteralPath (Join-Path $ReleaseDir "flash-files.txt") -Encoding ASCII -Value $flashArgsText
$settings = $flasherArgs.flash_settings

$flashScript = @'
param(
    [string]$Port = "COM101",
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Esptool = Get-Command esptool.py -ErrorAction SilentlyContinue
if ($Esptool) {
    $ToolCommand = $Esptool
    $PrefixArgs = @()
} else {
    $Python = Get-Command python -ErrorAction SilentlyContinue
    if (!$Python) {
        throw "Neither esptool.py nor python was found. Open an ESP-IDF shell before running this script."
    }
    $ToolCommand = $Python
    $PrefixArgs = @("-m", "esptool")
}

$ArgsList = @(
    "--chip", "esp32s3",
    "-p", $Port,
    "-b", "$Baud",
    "--before", "default_reset",
    "--after", "hard_reset",
    "write_flash",
    "--flash_mode", "@FLASH_MODE@",
    "--flash_freq", "@FLASH_FREQ@",
    "--flash_size", "@FLASH_SIZE@"
)

Get-Content (Join-Path $Here "flash-files.txt") | ForEach-Object {
    if ($_ -match "^\s*(0x[0-9a-fA-F]+)\s+(.+?)\s*$") {
        $ArgsList += $Matches[1]
        $ArgsList += (Join-Path $Here $Matches[2])
    }
}

$AllArgs = $PrefixArgs + $ArgsList
& $ToolCommand @AllArgs
if ($LASTEXITCODE -ne 0) {
    throw "esptool write_flash failed with exit code $LASTEXITCODE"
}
'@
$flashScript = $flashScript.Replace("@FLASH_MODE@", $settings.flash_mode)
$flashScript = $flashScript.Replace("@FLASH_FREQ@", $settings.flash_freq)
$flashScript = $flashScript.Replace("@FLASH_SIZE@", $settings.flash_size)
Set-Content -LiteralPath (Join-Path $ReleaseDir "flash-ili9341.ps1") -Encoding ASCII -Value $flashScript

$flashScriptSh = @'
#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-460800}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
args=(--chip esp32s3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset write_flash --flash_mode "@FLASH_MODE@" --flash_freq "@FLASH_FREQ@" --flash_size "@FLASH_SIZE@")
while read -r offset file; do
    [[ -z "${offset:-}" ]] && continue
    args+=("$offset" "$HERE/$file")
done < "$HERE/flash-files.txt"

if command -v esptool.py >/dev/null 2>&1; then
    esptool.py "${args[@]}"
else
    python3 -m esptool "${args[@]}"
fi
'@
$flashScriptSh = $flashScriptSh.Replace("@FLASH_MODE@", $settings.flash_mode)
$flashScriptSh = $flashScriptSh.Replace("@FLASH_FREQ@", $settings.flash_freq)
$flashScriptSh = $flashScriptSh.Replace("@FLASH_SIZE@", $settings.flash_size)
Set-Content -LiteralPath (Join-Path $ReleaseDir "flash-ili9341.sh") -Encoding ASCII -Value $flashScriptSh

if (!$Merge -and (Test-Path $FlashImage)) {
    Remove-Item -LiteralPath $FlashImage -Force
}

if ($Merge -and !$NoMerge) {
    $esptool = Get-EsptoolCommand
    $esptoolExe = $esptool.Exe
    $esptoolPrefixArgs = @($esptool.Args)

    $mergeArgs = @(
        "--chip", $flasherArgs.extra_esptool_args.chip,
        "merge_bin",
        "-o", $FlashImage,
        "--flash_mode", $settings.flash_mode,
        "--flash_freq", $settings.flash_freq,
        "--flash_size", $settings.flash_size
    )

    Add-MergePair "0x1d0000" (Join-Path $ReleaseDir "bios.bin")
    Add-MergePair "0x1f0000" (Join-Path $ReleaseDir "vgabios.bin")
    Add-MergePair "0x200000" (Join-Path $ReleaseDir "tiny386.ini")
    Add-MergePair "0x210000" (Join-Path $ReleaseDir "dos.img")
    Add-MergePair "0xF00000" (Join-Path $ReleaseDir "resources.bin")

    $mergeArgs += $mergePairs

    Invoke-Checked "esptool merge_bin" { & $esptoolExe @($esptoolPrefixArgs + $mergeArgs) }
}

Write-Host "ili9341 build finished: $BuildPath"
Write-Host "Release files: $ReleaseDir"
Write-Host "Flash file list: $(Join-Path $ReleaseDir "flash-files.txt")"
Write-Host "Flash script: $(Join-Path $ReleaseDir "flash-ili9341.ps1")"
if ($Merge -and !$NoMerge) {
    Write-Host "Firmware image: $FlashImage"
}
