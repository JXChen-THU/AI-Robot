# Copy firmware\_shared\pinmap.h into every sketch folder.
# Usage:  powershell -ExecutionPolicy Bypass -File tools\sync_pinmap.ps1
# ASCII only -> runs on Windows PowerShell 5.1 and PowerShell 7.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'firmware\_shared\pinmap.h'
if (-not (Test-Path $src)) { throw "pinmap.h not found: $src" }

$sketches = Get-ChildItem (Join-Path $root 'firmware') -Directory | Where-Object { $_.Name -ne '_shared' }
foreach ($s in $sketches) {
  $dst = Join-Path $s.FullName 'pinmap.h'
  Copy-Item $src $dst -Force
  Write-Host ("  synced -> firmware\{0}\pinmap.h" -f $s.Name)
}
$h = (Get-FileHash $src -Algorithm SHA256).Hash
Write-Host ""
Write-Host "OK: $($sketches.Count) sketch folder(s) synced. SHA256=$($h.Substring(0,16))..."
