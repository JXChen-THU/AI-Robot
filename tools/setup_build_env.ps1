# ============================================================================
#  setup_build_env.ps1 -- install a standalone arduino-cli toolchain for this
#  project (so you can verify "does it compile" without opening Arduino IDE).
#
#  Everything goes into .toolchain\ (git-ignored), it never touches your
#  existing Arduino IDE installation.
#
#  Usage (from project root):
#     powershell -ExecutionPolicy Bypass -File tools\setup_build_env.ps1
#
#  NOTE: ASCII only on purpose -- Windows PowerShell 5.1 reads BOM-less UTF-8
#        files as ANSI and chokes on non-ASCII characters.
# ============================================================================
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tc   = Join-Path $root '.toolchain'
$cfg  = Join-Path $tc 'arduino-cli.yaml'
$cli  = Join-Path $tc 'arduino-cli.exe'
$coreVersion = '3.0.7'
$espUrl = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'

New-Item -ItemType Directory -Force -Path $tc | Out-Null

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

function CLI { & $cli --config-file $cfg @args }
Write-Host "[2/5] arduino-cli version:"
CLI version

# --- 3. esp32 core ----------------------------------------------------------
Write-Host "[3/5] updating index (espressif) ..."
CLI core update-index --additional-urls $espUrl
Write-Host "[3/5] installing esp32:esp32@$coreVersion (about 250MB) ..."
CLI core install "esp32:esp32@$coreVersion" --additional-urls $espUrl

# --- 4. libraries -----------------------------------------------------------
Write-Host "[4/5] installing libraries ..."
foreach ($lib in 'TFT_eSPI','ESP32Servo','ArduinoJson','ESP32-audioI2S') {
  CLI lib install $lib
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
Write-Host "DONE. Compile example:"
Write-Host "  & '$cli' --config-file '$cfg' compile --fqbn esp32:esp32:esp32s3 firmware\01_display"
