$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Example = Join-Path $Root "src\secrets.example.h"
$Target = Join-Path $Root "src\secrets.h"

if (!(Test-Path $Example)) { throw "Missing src\secrets.example.h" }

if (Test-Path $Target) {
    Write-Host "src\secrets.h already exists. No changes made."
    exit 0
}

Copy-Item $Example $Target
Write-Host "Created src\secrets.h from src\secrets.example.h"
Write-Host "Edit src\secrets.h and enter your Wi-Fi SSID and password."
