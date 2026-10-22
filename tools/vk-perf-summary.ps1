# vk-perf-summary.ps1: whole-run GPU ms from a LithoKraft Vulkan log (0.0.1.59 F59-6 follow-up).
# Proposed by JD, Built by Cortana (2026-10-17). Scripts only: reads the existing "vulkan perf:" lines
# (one per 120 frames, vk_renderer.cpp readPerf) and the optional --bench-json; no engine change, no hash impact.
# Why: the old A/B quoted only the LAST 120-frame window. On a streaming bench GPU ms climbs with draws
# as the world loads, so one trailing window is not comparable between runs (F59-6 was that artifact).
# Report the whole-run average/median next to the last window AND its draw count; compare formats at
# matched draws (gpuPer1kDraws) or on a fully loaded scene.
#   powershell -ExecutionPolicy Bypass -File engine\tools\vk-perf-summary.ps1 -Log x.log [-Json x.json] [-Tag name]
#   dot-source it and call Get-VkPerfSummary / Format-VkBenchLine from other scripts.
# Script params are prefixed (VkPerf*) so dot-sourcing this file never overwrites a caller's $Log/$Json/$Tag.
param([Alias("Log")][string]$VkPerfLog = "", [Alias("Json")][string]$VkPerfJson = "", [Alias("Tag")][string]$VkPerfTag = "")

function Get-VkPerfSummary([string]$Log, [string]$Json = "") {
  $rx = 'vulkan perf: (\d+) frames: chunk draws/frame (\d+).*?gpu ms avg ([\d.]+) max ([\d.]+).*?mesh ([\d.]+) MB \((\w+)\)'
  $w = @(); foreach ($l in (Get-Content -LiteralPath $Log -ErrorAction SilentlyContinue)) {
    if ($l -match $rx) { $w += [pscustomobject]@{ frames = [int]$matches[1]; draws = [int]$matches[2]; gpu = [double]$matches[3]; max = [double]$matches[4]; mesh = [double]$matches[5]; fmt = $matches[6] } } }
  $o = [ordered]@{ windows = $w.Count; gpuLast = $null; drawsLast = $null; gpuRunAvg = $null; gpuRunMed = $null; gpuRunMax = $null;
                   gpuLoadedAvg = $null; loadedWindows = 0; gpuPer1kDraws = $null; meshMB = $null; fmt = ""; fps = $null; p99 = $null;
                   lockWindows = 0; lockAvg = $null; lockP99 = $null; lockMax = $null; lockSingleMax = $null; lockShare = $null; lockAcq = $null; lockCont = $null; lockBy = $null }
  if ($w.Count) {
    $last = $w[-1]; $o.gpuLast = $last.gpu; $o.drawsLast = $last.draws; $o.meshMB = $last.mesh; $o.fmt = $last.fmt
    $fr = ($w | Measure-Object frames -Sum).Sum
    $o.gpuRunAvg = [math]::Round((($w | % { $_.gpu * $_.frames } | Measure-Object -Sum).Sum) / [math]::Max(1, $fr), 3)  # frame-weighted
    $s = @($w.gpu | Sort-Object); $n = $s.Count
    $o.gpuRunMed = if ($n % 2) { $s[($n - 1) / 2] } else { [math]::Round(($s[$n / 2 - 1] + $s[$n / 2]) / 2, 3) }
    $o.gpuRunMax = ($w.max | Measure-Object -Maximum).Maximum
    # "loaded" = windows whose draw count is within 10% of the run's peak (the scene stopped growing)
    $peak = ($w.draws | Measure-Object -Maximum).Maximum; $ld = @($w | ? { $_.draws -ge 0.9 * $peak })
    $o.loadedWindows = $ld.Count; if ($ld.Count) { $o.gpuLoadedAvg = [math]::Round(($ld.gpu | Measure-Object -Average).Average, 3) }
    $big = @($w | ? { $_.draws -ge 1000 }); if ($big.Count) { $o.gpuPer1kDraws = [math]::Round((($big | % { $_.gpu / $_.draws * 1000 }) | Measure-Object -Average).Average, 3) }
  }
  # 0.0.1.60 (Proposed by JD, Built by Cortana): "lock timing:" lines (LL_LOCK_TIMING=1), one per 120 frames.
  $lrx = 'lock timing: (\d+) frames: main wait ms/frame avg ([\d.]+) p99 ([\d.]+) max ([\d.]+), longest single wait ([\d.]+) ms, share ([\d.]+)% of frame ([\d.]+) ms, acq/frame ([\d.]+) contended/frame ([\d.]+) \| by lock ms/frame: (.*) \| block holders ms/frame: (.*)$'
  $lw = @(); foreach ($l in (Get-Content -LiteralPath $Log -ErrorAction SilentlyContinue)) { if ($l -match $lrx) {
    $e = [ordered]@{ avg = [double]$matches[2]; p99 = [double]$matches[3]; max = [double]$matches[4]; single = [double]$matches[5]; share = [double]$matches[6]; frame = [double]$matches[7]; acq = [double]$matches[8]; cont = [double]$matches[9] }
    foreach ($m in [regex]::Matches($matches[10], '(\w+) ([\d.]+) \(')) { $e["L_" + $m.Groups[1].Value] = [double]$m.Groups[2].Value }
    foreach ($m in [regex]::Matches($matches[11], '(\w+) ([\d.]+)')) { $e["H_" + $m.Groups[1].Value] = [double]$m.Groups[2].Value }
    $lw += [pscustomobject]$e } }
  $o.lockWindows = $lw.Count
  if ($lw.Count) {
    $avg = { param($k) [math]::Round((($lw | % { $_.$k }) | Measure-Object -Average).Average, 3) }
    $o.lockAvg = & $avg 'avg'; $o.lockP99 = [math]::Round(($lw.p99 | Measure-Object -Maximum).Maximum, 3); $o.lockMax = ($lw.max | Measure-Object -Maximum).Maximum
    $o.lockSingleMax = ($lw.single | Measure-Object -Maximum).Maximum; $o.lockShare = & $avg 'share'; $o.lockAcq = & $avg 'acq'; $o.lockCont = & $avg 'cont'
    $o.lockBy = [ordered]@{}; foreach ($k in ($lw[0].PSObject.Properties.Name | ? { $_ -like 'L_*' -or $_ -like 'H_*' })) { $o.lockBy[$k] = & $avg $k }
  }  if ($Json -and (Test-Path -LiteralPath $Json)) {
    $j = Get-Content -LiteralPath $Json -Raw | ConvertFrom-Json
    if ($j.seconds) { $o.fps = [math]::Round($j.frames / $j.seconds, 1) }; $o.p99 = $j.frame_ms.p99
  }
  [pscustomobject]$o
}

function Format-VkBenchLine([string]$Tag, $s) {
  $f = { param($v, $d = 2) if ($null -eq $v) { 'n/a' } else { [math]::Round([double]$v, $d) } }
  "BENCH $Tag fps=$(& $f $s.fps 1) p99=$(& $f $s.p99) gpuLast=$(& $f $s.gpuLast)@$($s.drawsLast)draws gpuRunAvg=$(& $f $s.gpuRunAvg) gpuRunMed=$(& $f $s.gpuRunMed) " +
  "gpuLoadedAvg=$(& $f $s.gpuLoadedAvg)($($s.loadedWindows)win) gpuPer1kDraws=$(& $f $s.gpuPer1kDraws 3) windows=$($s.windows) meshMB=$(& $f $s.meshMB 1) fmt=$($s.fmt)" +
  $(if ($s.lockWindows) { " | LOCK wait ms/frame avg=$($s.lockAvg) p99max=$($s.lockP99) max=$($s.lockMax) single=$($s.lockSingleMax) share=$($s.lockShare)% acq=$($s.lockAcq) cont=$($s.lockCont) by: " + (($s.lockBy.GetEnumerator() | % { "$($_.Key.Substring(2))=$($_.Value)" }) -join " ") } else { "" })
}

if ($VkPerfLog) { if (-not $VkPerfTag) { $VkPerfTag = [IO.Path]::GetFileNameWithoutExtension($VkPerfLog) }; Format-VkBenchLine $VkPerfTag (Get-VkPerfSummary $VkPerfLog $VkPerfJson) }
