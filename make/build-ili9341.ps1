param(
    [string]$BuildDir = "build_ili9341",
    [switch]$Clean,
    [switch]$NoMerge
)

$script = Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..")) "script\build-ili9341.ps1"
& $script -BuildDir $BuildDir -Clean:$Clean -NoMerge:$NoMerge
exit $LASTEXITCODE
