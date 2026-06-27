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
    "--flash_mode", "dio",
    "--flash_freq", "80m",
    "--flash_size", "16MB"
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
