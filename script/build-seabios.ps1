param(
    [switch]$SkipFetch,
    [switch]$SkipClean,
    [ValidateSet("Auto", "Wsl", "Msys2")]
    [string]$BuildEnv = "Auto"
)

$ErrorActionPreference = "Stop"

$Root = [string](Resolve-Path (Join-Path $PSScriptRoot ".."))
$SeaBIOS = Join-Path $Root "refs\seabios"
$Patch = Join-Path $Root "refs\tiny386\seabios\patch"
$Config = Join-Path $Root "refs\tiny386\seabios\config"
$Release = Join-Path $Root "release"

function Run-Git {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Args)
    & git -C $SeaBIOS @Args
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Args -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Test-WslReady {
    $wsl = Get-Command wsl -ErrorAction SilentlyContinue
    if (!$wsl) {
        return $false
    }

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & wsl bash -lc "command -v make >/dev/null && command -v gcc >/dev/null && command -v python3 >/dev/null" *> $null
        return $LASTEXITCODE -eq 0
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Invoke-WslBuild {
    $wslPath = ConvertTo-WslPath $SeaBIOS
    & wsl bash -lc "cd '$wslPath' && make PYTHON=python3 olddefconfig && make PYTHON=python3"
    if ($LASTEXITCODE -ne 0) {
        throw "WSL SeaBIOS build failed with exit code $LASTEXITCODE"
    }
}

function ConvertTo-WslPath {
    param([string]$WindowsPath)

    $drive = $WindowsPath.Substring(0, 1).ToLower()
    $pathWithoutDrive = $WindowsPath.Substring(2).Replace("\", "/")
    return "/mnt/$drive$pathWithoutDrive"
}

function Normalize-LineEndingsWithWsl {
    $drive = $SeaBIOS.Substring(0, 1).ToLower()
    $pathWithoutDrive = $SeaBIOS.Substring(2).Replace("\", "/")
    $wslPath = "/mnt/$drive$pathWithoutDrive"
    & wsl bash -lc "cd '$wslPath' && git ls-files -z | xargs -0 perl -pi -e 's/\r$//'"
    if ($LASTEXITCODE -ne 0) {
        throw "WSL line ending normalization failed with exit code $LASTEXITCODE"
    }
}

function Test-Msys2Ready {
    $bash = "C:\msys64\usr\bin\bash.exe"
    if (!(Test-Path $bash)) {
        return $false
    }

    & $bash -lc "command -v make >/dev/null && command -v gcc >/dev/null && command -v python3 >/dev/null"
    return $LASTEXITCODE -eq 0
}

function Invoke-Msys2Build {
    $bash = "C:\msys64\usr\bin\bash.exe"
    $drive = $SeaBIOS.Substring(0, 1).ToLower()
    $pathWithoutDrive = $SeaBIOS.Substring(2).Replace("\", "/")
    $msysPath = "/$drive$pathWithoutDrive"

    & $bash -lc "cd '$msysPath' && make olddefconfig && make"
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 SeaBIOS build failed with exit code $LASTEXITCODE"
    }
}

if (!(Test-Path $SeaBIOS)) {
    throw "SeaBIOS submodule not found at $SeaBIOS"
}
if (!(Test-Path $Patch)) {
    throw "SeaBIOS patch not found at $Patch"
}
if (!(Test-Path $Config)) {
    throw "SeaBIOS config not found at $Config"
}

if (!$SkipFetch) {
    Run-Git fetch origin
}

if (!$SkipClean) {
    Run-Git config core.autocrlf false
    Run-Git reset --hard origin/master
    Run-Git checkout-index "-f" "-a"
    Run-Git clean -fdx
}

if (Test-WslReady) {
    Normalize-LineEndingsWithWsl
}

Run-Git apply --ignore-space-change --check $Patch
Run-Git apply --ignore-space-change $Patch

Copy-Item -LiteralPath $Config -Destination (Join-Path $SeaBIOS ".config") -Force

if (($BuildEnv -eq "Auto" -or $BuildEnv -eq "Wsl") -and (Test-WslReady)) {
    Invoke-WslBuild
}
elseif (($BuildEnv -eq "Auto" -or $BuildEnv -eq "Msys2") -and (Test-Msys2Ready)) {
    Invoke-Msys2Build
}
else {
    throw "No supported SeaBIOS build environment found. Install a WSL distro with make/gcc/python3 or MSYS2 with make/gcc/python3, then rerun this script."
}

New-Item -ItemType Directory -Force -Path $Release | Out-Null
Copy-Item -LiteralPath (Join-Path $SeaBIOS "out\bios.bin") -Destination (Join-Path $Release "bios.bin") -Force
Copy-Item -LiteralPath (Join-Path $SeaBIOS "out\vgabios.bin") -Destination (Join-Path $Release "vgabios.bin") -Force

Write-Host "SeaBIOS build finished: $(Join-Path $SeaBIOS 'out\bios.bin')"
Write-Host "Release files:"
Write-Host "  $(Join-Path $Release 'bios.bin')"
Write-Host "  $(Join-Path $Release 'vgabios.bin')"
