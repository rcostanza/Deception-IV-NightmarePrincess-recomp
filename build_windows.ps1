<#
.SYNOPSIS
Local build and test on Windows: build.sh (through Git Bash or MSYS2's bash) plus the Python tests,
a boot check and the package.

.DESCRIPTION
Needs MSYS2 with the CLANG64 packages (docs/building.md), Python 3.10 or newer, and Git for Windows
or MSYS2 for bash. Options combine:

  .\build_windows.ps1                      build out\deception.exe and out\deception-setup.exe
  .\build_windows.ps1 -Test                also run every C/C++ test and the Python tests
  .\build_windows.ps1 -Run [-Game DIR] [-Timeout N]
                                           also start the game (DECEPTION_GAME_DIR, or DIR) for N
                                           seconds (default 20) and print its first STOP:/Fault: line
  .\build_windows.ps1 -Package [-PythonEmbed ZIP]
                                           also write the release zip (tools\package.py --zip), with
                                           python.org's embeddable Python when ZIP is given
  .\build_windows.ps1 -Clean               remove out\ first (a full rebuild)
#>
param(
    [switch]$Test,
    [switch]$Run,
    [string]$Game = $env:DECEPTION_GAME_DIR,
    [int]$Timeout = 20,
    [switch]$Package,
    [string]$PythonEmbed,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

# Git Bash or MSYS2's bash; never System32's bash.exe, which is WSL.
$msys2 = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$bash = @(
    "$env:ProgramFiles\Git\bin\bash.exe",
    "${env:ProgramFiles(x86)}\Git\bin\bash.exe",
    "$env:LOCALAPPDATA\Programs\Git\bin\bash.exe",
    "$msys2\usr\bin\bash.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $bash) { throw 'Need Git for Windows or MSYS2 for bash (docs/building.md).' }
if (-not (Test-Path -LiteralPath "$msys2\clang64\bin\clang.exe")) {
    throw "Need MSYS2's CLANG64 toolchain in $msys2 (docs/building.md; MSYS2_ROOT for another folder)."
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { throw 'Need Python 3.10 or newer on PATH (python.org).' }

function Invoke-Checked([string]$what, [scriptblock]$block) {
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)." }
}

if ($Clean -and (Test-Path -LiteralPath out)) { Remove-Item -LiteralPath out -Recurse -Force }
$buildArgs = @('build.sh') + $(if ($Test) { @('--test') } else { @() })
# MSYSTEM is set by build.sh itself; CHERE_INVOKING keeps an MSYS2 bash in this folder.
$env:CHERE_INVOKING = '1'
Invoke-Checked 'build.sh' { & $bash @buildArgs }

if ($Test) {
    Push-Location tests
    try { Invoke-Checked 'The Python tests' { & $python.Source -m unittest discover -p 'test_*.py' } }
    finally { Pop-Location }
}

if ($Run) {
    if (-not $Game) { throw '-Run needs the game dump: -Game DIR or DECEPTION_GAME_DIR.' }
    # The game's output goes to out\boot.log; its exit code is reported, not thrown (a timeout ends it).
    & $python.Source run.py --game $Game --timeout $Timeout *> out\boot.log
    $code = $LASTEXITCODE
    $first = Select-String -LiteralPath out\boot.log -Pattern '^(STOP|Fault):' | Select-Object -First 1
    if ($first) { $first.Line } else { 'No STOP: or Fault: line.' }
    "Exit code $code; the full log is out\boot.log."
}

if ($Package) {
    $packageArgs = @('tools\package.py', '--zip')
    if ($PythonEmbed) { $packageArgs += @('--python', $PythonEmbed) }
    Invoke-Checked 'tools\package.py' { & $python.Source @packageArgs }
}

exit 0
