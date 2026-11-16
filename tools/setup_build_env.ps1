# ============================================================================
#  setup_build_env.ps1 -- install a standalone arduino-cli toolchain so you can
#  verify "does it compile" without opening the Arduino IDE.
#
#  SAFETY: everything (cli, config, packages, libraries, temp files) stays inside
#  the target folder. It does NOT touch C:, the registry, PATH, or an existing
#  Arduino IDE. Deleting the target folder undoes everything.
#
#  Usage (from project root):
#     powershell -ExecutionPolicy Bypass -File tools\setup_build_env.ps1
#     powershell -ExecutionPolicy Bypass -File tools\setup_build_env.ps1 -TargetDir D:\ArduinoToolchain
#
#  NOTE: ASCII only on purpose -- Windows PowerShell 5.1 reads BOM-less UTF-8
#        files as ANSI and chokes on non-ASCII characters.
# ============================================================================
param(
  [string]$TargetDir = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($TargetDir)) { $TargetDir = Join-Path $root '.toolchain' }
$tc  = [IO.Path]::GetFullPath($TargetDir)
$cfg = Join-Path $tc 'arduino-cli.yaml'
$cli = Join-Path $tc 'arduino-cli.exe'
$coreVersion = '3.0.7'
$espUrl = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'

New-Item -ItemType Directory -Force -Path $tc | Out-Null

# Keep every temp file inside the target folder (never C:).
$env:TEMP = Join-Path $tc 'tmp'
$env:TMP  = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

Write-Host "target dir : $tc"
Write-Host "temp dir   : $env:TEMP"

# --- 1. download arduino-cli ------------------------------------------------
if (-not (Test-Path $cli)) {
  Write-Host "[1/5] downloading arduino-cli ..."
  $zip = Join-Path $tc 'arduino-cli.zip'
  Invoke-WebRequest -Uri 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip' `
                    -OutFile $zip -UseBasicParsing -TimeoutSec 900
  Expand-Archive -Path $zip -DestinationPath $tc -Force
  Remove-Item $zip -Force
}
if (-not (Test-Path $cli)) { throw "arduino-cli.exe not found after unzip" }

# --- 2. isolated config -----------------------------------------------------
@"
directories:
  data: $(Join-Path $tc 'data')
  downloads: $(Join-Path $tc 'staging')
  user: $(Join-Path $tc 'user')
"@ | Set-Content -Path $cfg -Encoding ASCII

function ACLI { & $cli --config-file $cfg @args }
Write-Host "[2/5] arduino-cli version:"
ACLI version

# --- 3. esp32 core ----------------------------------------------------------
Write-Host "[3/5] updating index (espressif) ..."
ACLI core update-index --additional-urls $espUrl
Write-Host "[3/5] installing esp32:esp32@$coreVersion (about 250MB download) ..."
ACLI core install "esp32:esp32@$coreVersion" --additional-urls $espUrl

$installed = (ACLI core list 2>&1) -join "`n"
if ($installed -notmatch 'esp32:esp32') { throw "esp32 core install FAILED (see log above)" }
Write-Host "[3/5] core OK"

# --- 4. libraries -----------------------------------------------------------
Write-Host "[4/5] installing libraries (pinned versions) ..."
# These exact versions are the ones that were actually verified to compile with
# core 3.0.7. Note: the audio library is published as "ESP32-audioI2S-master";
# 4.x requires core 3.3.x (it uses IDF 5.4 APIs), so 3.0.13 is the right pair.
$libs = @(
  'TFT_eSPI@2.5.43',
  'ESP32Servo@3.2.1',
  'ArduinoJson@7.4.3',
  'ESP32-audioI2S-master@3.0.13'
)
foreach ($lib in $libs) {
  ACLI lib install $lib
}

# --- 5. TFT_eSPI User_Setup ------------------------------------------------
Write-Host "[5/5] configuring TFT_eSPI ..."
$tftDir = Join-Path $tc 'user\libraries\TFT_eSPI'
if (Test-Path $tftDir) {
  Copy-Item (Join-Path $root 'config\User_Setup.h') (Join-Path $tftDir 'User_Setup.h') -Force
  Write-Host "      User_Setup.h copied"
} else {
  Write-Warning "      TFT_eSPI folder not found, skipped"
}

Write-Host ""
Write-Host "DONE."
Write-Host "  cli     = $cli"
Write-Host "  config  = $cfg"
Write-Host "  compile example:"
Write-Host "    & '$cli' --config-file '$cfg' compile --fqbn esp32:esp32:esp32s3 firmware\01_display"
