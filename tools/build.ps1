# LithoLattice engine + LithoKraft game - Windows Release x64 build (MSVC via VS 2022 Build Tools)
# Usage: powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Clean] [-Ship]
#   -Ship: shipping build (LITHOKRAFT_DEV_BUILD=OFF): scripts load from the copy next to the exe
#          instead of the source tree (the default dev build, so F10 edits stay in the repo).
param([switch]$Clean, [switch]$Ship)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $env:USERPROFILE 'agent-tools\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ('lithokraft-build-' + (Get-Date -Format 'yyyy-MM-dd') + '.log')

function Log([string]$msg) {
  $line = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $msg
  Write-Host $line
  Add-Content -Path $log -Value $line -Encoding ASCII
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
# Cortana 2026-10-15 1500 (TOOLS, MC-03): newest VS 2022 or later (VS 2026 = 18.x); generator follows the found version.
$vs = & $vswhere -latest -version '[17.0,' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { Log 'ERROR: VS 2022 or newer with C++ tools not found'; exit 1 }
$vsMajor = [int]((& $vswhere -path $vs -property catalog_productLineVersion | Select-Object -First 1) -replace '\D','')
$vsVer = (& $vswhere -path $vs -property installationVersion | Select-Object -First 1)
$gen = switch -regex ($vsVer) { '^17\.' { 'Visual Studio 17 2022' } '^18\.' { 'Visual Studio 18 2026' } default { '' } }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }
Log "=== LithoKraft build: repo=$repo vs=$vs"
Log ("cmake: " + (& $cmake --version | Select-Object -First 1))

$build = Join-Path $repo 'build'
if ($Clean -and (Test-Path $build)) { Log 'Cleaning build dir'; Remove-Item -Recurse -Force $build }

function Run([string[]]$cmakeArgs) {
  Log ("> cmake " + ($cmakeArgs -join ' '))
  $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  & $cmake @cmakeArgs 2>&1 | ForEach-Object { $s = "$_"; Write-Host $s; Add-Content -Path $log -Value $s -Encoding ASCII }
  $code = $LASTEXITCODE; $ErrorActionPreference = $prev
  if ($code -ne 0) { Log "ERROR: cmake exited $code"; exit $code }
}
$dev = if ($Ship) { 'OFF' } else { 'ON' }
$genArgs = if ($gen) { @('-G', $gen, '-A', 'x64') } else { @('-A', 'x64') }  # unknown future VS: let CMake pick
Run (@('-S', $repo, '-B', $build) + $genArgs + @("-DLITHOKRAFT_DEV_BUILD=$dev"))
Run @('--build', $build, '--config', 'Release', '--parallel')

$exe = Join-Path $build 'Release\LithoKraft.exe'
if (Test-Path $exe) { Log ("OK: $exe (" + [math]::Round((Get-Item $exe).Length / 1KB) + ' KB)') } else { Log 'ERROR: exe missing'; exit 1 }
