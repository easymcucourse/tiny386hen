param(
    [string]$BuildDir = "build_ili9341",
    [switch]$Clean,
    [switch]$NoMerge
)

$ErrorActionPreference = "Stop"

$Root = [string](Resolve-Path (Join-Path $PSScriptRoot ".."))
$ProjectDir = Join-Path $Root "make\esp-ili9341"
$ReleaseDir = Join-Path $Root "release\esp"
$BuildPath = Join-Path $ProjectDir $BuildDir
$FlashImage = Join-Path $ReleaseDir "flash_image_ILI9341.bin"

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

Push-Location $ProjectDir
try {
    Invoke-Checked "idf.py build" { idf.py -B $BuildDir -DBOARD=ili9341 build }
}
finally {
    Pop-Location
}

$flasherArgsPath = Join-Path $BuildPath "flasher_args.json"
if (!(Test-Path $flasherArgsPath)) {
    throw "Missing flasher args: $flasherArgsPath"
}

$flasherArgs = Get-Content $flasherArgsPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

$mergePairs = @()
$flasherArgs.flash_files.PSObject.Properties | ForEach-Object {
    $offset = $_.Name
    $relativePath = $_.Value
    $source = Join-Path $BuildPath $relativePath
    if (!(Test-Path $source)) {
        throw "Missing flash artifact: $source"
    }

    Copy-Item -LiteralPath $source -Destination (Join-Path $ReleaseDir (Split-Path $relativePath -Leaf)) -Force
    $mergePairs += $offset
    $mergePairs += $source
}

$iniSource = Join-Path $Root "refs\tiny386\esp\tiny386.ini"
if (Test-Path $iniSource) {
    Copy-Item -LiteralPath $iniSource -Destination (Join-Path $ReleaseDir "tiny386.ini") -Force
}

foreach ($name in @("bios.bin", "vgabios.bin")) {
    $source = Join-Path $Root "release\$name"
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $ReleaseDir $name) -Force
    }
}

if (!$NoMerge) {
    $esptool = Get-EsptoolCommand
    $esptoolExe = $esptool.Exe
    $esptoolPrefixArgs = @($esptool.Args)

    $settings = $flasherArgs.flash_settings
    $mergeArgs = @(
        "--chip", $flasherArgs.extra_esptool_args.chip,
        "merge_bin",
        "-o", $FlashImage,
        "--flash_mode", $settings.flash_mode,
        "--flash_freq", $settings.flash_freq,
        "--flash_size", $settings.flash_size
    ) + $mergePairs

    Invoke-Checked "esptool merge_bin" { & $esptoolExe @($esptoolPrefixArgs + $mergeArgs) }
}

Write-Host "ili9341 build finished: $BuildPath"
Write-Host "Release files: $ReleaseDir"
if (!$NoMerge) {
    Write-Host "Firmware image: $FlashImage"
}
