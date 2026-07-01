param(
    [string]$Port = "COM21",
    [ValidateSet("gdb", "openocd", "build-flash-gdb")]
    [string]$Action = "gdb",
    [string]$IdfPath = "C:\esp32\esp-idf-v5.4.4",
    [string]$Project = "c:\Users\2572\Documents\0629\tiny386hen-main\make\esp-ili9341",
    [string]$BuildDir = "c:\Users\2572\Documents\0629\build_ili9341"
)

$ErrorActionPreference = "Stop"

function Import-IdfEnv {
    . "$IdfPath\export.ps1"
}

function Show-Ports {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    Write-Host "Available COM ports: $($ports -join ', ')"
    if ($ports -notcontains $Port) {
        Write-Warning "Port $Port is not currently listed. USB-JTAG may still work; COM is only needed for UART flash/monitor."
    }
}

Show-Ports
Import-IdfEnv

$common = @(
    "-C", $Project,
    "-B", $BuildDir,
    "-p", $Port
)

switch ($Action) {
    "openocd" {
        Write-Host "Starting OpenOCD (ESP32-S3 built-in USB-JTAG, not COM port)..."
        idf.py @common openocd
    }
    "gdb" {
        Write-Host "Starting OpenOCD + GDB on $Port (serial) / USB-JTAG..."
        idf.py @common gdb
    }
    "build-flash-gdb" {
        Write-Host "Build + flash + GDB..."
        idf.py @common build flash gdb
    }
}
