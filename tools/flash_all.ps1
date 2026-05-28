# tools/flash_all.ps1
# PC8 一括コンパイル & フラッシュスクリプト
# 使い方:
#   cd g:\PC8
#   .\tools\flash_all.ps1
#   .\tools\flash_all.ps1 -GameFile "myfile.p8"
#   .\tools\flash_all.ps1 -SkipBios
#   .\tools\flash_all.ps1 -SkipGame
#   .\tools\flash_all.ps1 -SkipFirmware

param(
    [string]$GameFile    = "jelpi.p8",
    [switch]$SkipBios    = $false,
    [switch]$SkipGame    = $false,
    [switch]$SkipFirmware= $false,
    [string]$Port        = ""        # 空文字=自動検出
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

# ── esptool の場所 ─────────────────────────────────────────────
$esptool = "C:\Users\fooba\.platformio\packages\tool-esptoolpy\esptool.py"
$python  = "C:\Users\fooba\.platformio\penv\Scripts\python.exe"
$pio     = "C:\Users\fooba\.platformio\penv\Scripts\platformio.exe"
$compile = Join-Path $Root "tools\pc8_compile.exe"

if (-not (Test-Path $python))  { Write-Error "Python not found: $python"; exit 1 }
if (-not (Test-Path $esptool)) { Write-Error "esptool not found: $esptool"; exit 1 }
if (-not (Test-Path $compile)) { Write-Error "pc8_compile.exe not found: $compile`nRun build_tool.ps1 first."; exit 1 }

function Invoke-Esptool {
    param([string]$Offset, [string]$File)
    $args_list = @("--chip", "esp32s3")
    if ($Port) { $args_list += @("--port", $Port) }
    $args_list += @("write_flash", $Offset, $File)
    Write-Host ">> esptool $($args_list -join ' ')"
    & $python $esptool @args_list
    if ($LASTEXITCODE -ne 0) { Write-Error "esptool failed"; exit 1 }
}

Set-Location $Root
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  PC8 Compile & Flash Tool" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# ── 1. BIOS コンパイル & フラッシュ ──────────────────────────
if (-not $SkipBios) {
    Write-Host "`n[1/3] Compiling BIOS..." -ForegroundColor Yellow
    $biosIn  = Join-Path $Root "bios.p8"
    $biosOut = Join-Path $Root "data\bios.pc8c"

    if (-not (Test-Path $biosIn)) { Write-Error "bios.p8 not found: $biosIn"; exit 1 }

    & $compile bios $biosIn $biosOut
    if ($LASTEXITCODE -ne 0) { Write-Error "BIOS compile failed"; exit 1 }

    Write-Host "[1/3] Flashing BIOS to partition 0x7e0000..." -ForegroundColor Yellow
    Invoke-Esptool -Offset "0x7e0000" -File $biosOut
    Write-Host "[1/3] BIOS flash done." -ForegroundColor Green
}

# ── 2. ゲームコンパイル & フラッシュ ─────────────────────────
if (-not $SkipGame) {
    Write-Host "`n[2/3] Compiling Game: $GameFile..." -ForegroundColor Yellow
    $gameIn  = Join-Path $Root $GameFile
    $gameOut = Join-Path $Root "data\game.pc8c"

    if (-not (Test-Path $gameIn)) { Write-Error "Game file not found: $gameIn"; exit 1 }

    & $compile game $gameIn $gameOut
    if ($LASTEXITCODE -ne 0) { Write-Error "Game compile failed"; exit 1 }

    Write-Host "[2/3] Flashing Game to partition 0x7c0000..." -ForegroundColor Yellow
    Invoke-Esptool -Offset "0x7c0000" -File $gameOut
    Write-Host "[2/3] Game flash done." -ForegroundColor Green
}

# ── 3. ファームウェアビルド & アップロード ────────────────────
if (-not $SkipFirmware) {
    Write-Host "`n[3/3] Building & uploading firmware..." -ForegroundColor Yellow
    & $pio run -e m5stack-stamps3 --target upload
    if ($LASTEXITCODE -ne 0) { Write-Error "Firmware upload failed"; exit 1 }
    Write-Host "[3/3] Firmware upload done." -ForegroundColor Green
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  All done! Open serial monitor to verify:" -ForegroundColor Cyan
Write-Host "  $pio device monitor --baud 115200" -ForegroundColor White
Write-Host "========================================" -ForegroundColor Cyan
