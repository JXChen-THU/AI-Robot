# ============================================================================
#  compile_all.ps1 -- verify every sketch in firmware\ actually compiles.
#
#  Usage (from project root, after tools\setup_build_env.ps1):
#     powershell -ExecutionPolicy Bypass -File tools\compile_all.ps1
#     powershell -ExecutionPolicy Bypass -File tools\compile_all.ps1 -TargetDir D:\ArduinoToolchain
#     powershell -ExecutionPolicy Bypass -File tools\compile_all.ps1 -Only 01_display
#
#  Board options mirror docs/02_环境搭建.md:
#     PSRAM=opi, FlashSize=16M, FlashMode=qio, CDCOnBoot=default (Disabled)
#     PartitionScheme=huge_app  (3MB APP / 1MB SPIFFS -- SPIFFS is required by the
#     SiliconFlow TTS backend; there is no "12.5MB SPIFFS" option in core 3.x)
#
#  ASCII only (PowerShell 5.1 + BOM-less UTF-8 issue).
# ============================================================================
param(
  [string]$TargetDir = '',
  [string]$Only = ''
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($TargetDir)) { $TargetDir = Join-Path $root '.toolchain' }
$cli = Join-Path $TargetDir 'arduino-cli.exe'
$cfg = Join-Path $TargetDir 'arduino-cli.yaml'
if (-not (Test-Path $cli)) { throw "arduino-cli not found: $cli  (run tools\setup_build_env.ps1 first)" }

$env:TEMP = Join-Path $TargetDir 'tmp'
$env:TMP  = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

$fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,FlashMode=qio,CDCOnBoot=default,PartitionScheme=huge_app'
$report = Join-Path $TargetDir 'compile_report.txt'
"" | Set-Content -Path $report -Encoding UTF8

# 07_main needs secrets.h; create a throwaway one from the template if missing
$secrets = Join-Path $root 'firmware\07_main\secrets.h'
$tempSecrets = $false
if (-not (Test-Path $secrets)) {
  Copy-Item (Join-Path $root 'firmware\07_main\secrets.example.h') $secrets -Force
  $tempSecrets = $true
}

$sketches = Get-ChildItem (Join-Path $root 'firmware') -Directory |
            Where-Object { $_.Name -ne '_shared' } |
            Where-Object { [string]::IsNullOrWhiteSpace($Only) -or $_.Name -like "*$Only*" } |
            Sort-Object Name

$pass = 0; $fail = 0
foreach ($s in $sketches) {
  Write-Host ("=== {0} ===" -f $s.Name)
  $out = & $cli --config-file $cfg compile --fqbn $fqbn --warnings default $s.FullName 2>&1
  $code = $LASTEXITCODE
  $errs = $out | Select-String -Pattern ': error:|: fatal error:|error:' | Select-Object -First 25
  if ($code -eq 0) {
    $size = ($out | Select-String -Pattern 'Sketch uses (\d+) bytes').Matches.Groups[1].Value
    Write-Host "  PASS  (flash $size bytes)"
    Add-Content -Path $report -Value ("PASS  {0}  flash={1}" -f $s.Name, $size) -Encoding UTF8
    $pass++
  } else {
    Write-Host "  FAIL"
    Add-Content -Path $report -Value ("FAIL  {0}" -f $s.Name) -Encoding UTF8
    foreach ($e in $errs) {
      Write-Host ("    " + $e.Line.Trim())
      Add-Content -Path $report -Value ("      " + $e.Line.Trim()) -Encoding UTF8
    }
    $fail++
  }
}

if ($tempSecrets) { Remove-Item $secrets -Force }

Write-Host ""
Write-Host ("RESULT: {0} passed, {1} failed  (report: {2})" -f $pass, $fail, $report)
Add-Content -Path $report -Value ("`nRESULT: {0} passed, {1} failed" -f $pass, $fail) -Encoding UTF8
if ($fail -gt 0) { exit 1 }
