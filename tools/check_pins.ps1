# 代码体检: 1) 写死引脚 2) 是否 include pinmap.h 3) pinmap.h 副本一致性 4) 禁区/重复引脚
# 用法:  powershell -ExecutionPolicy Bypass -File tools\check_pins.ps1
# 说明: 只用 ASCII 字符, 保证 Windows PowerShell 5.1 / PowerShell 7 都能直接跑
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$shared = Join-Path $root 'firmware\_shared\pinmap.h'
$fail   = 0

Write-Host "=== [1] pinmap.h forbidden / duplicate pins ===" -ForegroundColor Cyan
$defs = @{}
Select-String -Path $shared -Pattern '^\s*#define\s+PIN_(\w+)\s+(-?\d+)' | ForEach-Object {
  $name = $_.Matches[0].Groups[1].Value
  $num  = [int]$_.Matches[0].Groups[2].Value
  if ($num -lt 0) { return }                                  # -1 = not using a pin
  if ($name -eq 'BOOT_BTN') { return }                        # GPIO0 on purpose (onboard button, input only)
  if ($num -eq 0 -or ($num -ge 26 -and $num -le 37)) {
    Write-Host ("  [FORBIDDEN] PIN_{0} = GPIO{1}  (0 or 26-37 are reserved)" -f $name,$num) -ForegroundColor Red
    $script:fail++
  }
  if ($defs.ContainsKey($num)) {
    Write-Host ("  [DUPLICATE] GPIO{0} used by PIN_{1} and PIN_{2}" -f $num,$defs[$num],$name) -ForegroundColor Red
    $script:fail++
  } else { $defs[$num] = $name }
}
Write-Host ("  {0} GPIOs defined" -f $defs.Count)

Write-Host "`n=== [2] every sketch must include pinmap.h / no hardcoded pins ===" -ForegroundColor Cyan
$sketches = Get-ChildItem (Join-Path $root 'firmware') -Directory | Where-Object { $_.Name -ne '_shared' }
foreach ($s in $sketches) {
  $ino = Get-ChildItem $s.FullName -Filter *.ino -ErrorAction SilentlyContinue
  if (-not $ino) { Write-Host ("  [MISSING] firmware\{0} has no .ino" -f $s.Name) -ForegroundColor Red; $script:fail++; continue }
  $txt = Get-Content $ino.FullName -Raw
  if ($txt -notmatch 'include\s+"pinmap\.h"') {
    Write-Host ("  [MISSING] firmware\{0}\{1} : no #include `"pinmap.h`"" -f $s.Name,$ino.Name) -ForegroundColor Red; $script:fail++
  }
  $hard = Select-String -Path $ino.FullName -Pattern '\b(pinMode|digitalWrite|digitalRead|analogRead|ledcAttach|ledcAttachPin|ledcSetup|attach|writeMicroseconds|attachInterrupt)\s*\(\s*\d+\s*[,)]'
  foreach ($h in $hard) {
    Write-Host ("  [HARDCODED] {0}\{1}:{2}  {3}" -f $s.Name,$ino.Name,$h.LineNumber,$h.Line.Trim()) -ForegroundColor Yellow
    $script:fail++
  }
}

Write-Host "`n=== [3] pinmap.h copies in sync ===" -ForegroundColor Cyan
$srcHash = (Get-FileHash $shared -Algorithm SHA256).Hash
foreach ($s in $sketches) {
  $copy = Join-Path $s.FullName 'pinmap.h'
  if (-not (Test-Path $copy)) { Write-Host ("  [MISSING] firmware\{0}\pinmap.h  -> run sync_pinmap.ps1" -f $s.Name) -ForegroundColor Red; $script:fail++; continue }
  if ((Get-FileHash $copy -Algorithm SHA256).Hash -ne $srcHash) {
    Write-Host ("  [OUT-OF-SYNC] firmware\{0}\pinmap.h" -f $s.Name) -ForegroundColor Red; $script:fail++
  }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "ALL CHECKS PASSED" -ForegroundColor Green } else { Write-Host "$fail PROBLEM(S) FOUND" -ForegroundColor Red; exit 1 }
