param(
    [string]$SourceDir = (Join-Path $PSScriptRoot "..\ffmpeg-8.1\ffmpeg-8.1"),
    [string]$InstallDir = (Join-Path $PSScriptRoot "..\ffmpeg-8.1\local-install"),
    [switch]$EnableShared = $true,
    [switch]$EnableStatic = $false,
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

function Require-Command {
    param([string]$Name, [string]$Hint)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing command '$Name'. $Hint"
    }
}

function To-MsysPath {
    param([string]$Path)

    $resolved = (Resolve-Path $Path).Path
    $normalized = $resolved -replace "\\", "/"

    if ($normalized -match "^([A-Za-z]):/(.*)$") {
        $drive = $matches[1].ToLowerInvariant()
        $rest = $matches[2]
        return "/$drive/$rest"
    }

    return $normalized
}

$sourceResolved = Resolve-Path $SourceDir
if (-not (Test-Path (Join-Path $sourceResolved "configure"))) {
    throw "FFmpeg source tree not found at '$sourceResolved' (missing configure script)."
}

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$installResolved = Resolve-Path $InstallDir

Require-Command -Name "bash" -Hint "Install MSYS2/WSL and ensure bash.exe is in PATH."
Require-Command -Name "cl" -Hint "Run this script from 'x64 Native Tools Command Prompt for VS'."

bash -lc "command -v make >/dev/null 2>&1"
if ($LASTEXITCODE -ne 0) {
    throw "'make' not found inside bash environment. Install build-essential/MSYS make first."
}

bash -lc "command -v nasm >/dev/null 2>&1"
if ($LASTEXITCODE -ne 0 -and -not (Get-Command "nasm" -ErrorAction SilentlyContinue)) {
    Write-Warning "nasm not found. FFmpeg may build slower or fail for some codecs."
}

$sourceMsys = To-MsysPath $sourceResolved
$installMsys = To-MsysPath $installResolved

$configureArgs = @(
    "--toolchain=msvc",
    "--target-os=win64",
    "--arch=x86_64",
    "--prefix=$installMsys",
    "--disable-programs",
    "--disable-doc",
    "--disable-debug"
)

if ($EnableShared) {
    $configureArgs += "--enable-shared"
} else {
    $configureArgs += "--disable-shared"
}

if ($EnableStatic) {
    $configureArgs += "--enable-static"
} else {
    $configureArgs += "--disable-static"
}

$configureLine = "cd '$sourceMsys' ; ./configure " + ($configureArgs -join " ")
$buildLine = "cd '$sourceMsys' ; make -j$Jobs"
$installLine = "cd '$sourceMsys' ; make install"

Write-Host "[1/3] Configure FFmpeg" -ForegroundColor Cyan
bash -lc $configureLine

Write-Host "[2/3] Build FFmpeg" -ForegroundColor Cyan
bash -lc $buildLine

Write-Host "[3/3] Install FFmpeg" -ForegroundColor Cyan
bash -lc $installLine

Write-Host "FFmpeg build done. Install path: $installResolved" -ForegroundColor Green
Write-Host "Now configure HyperMosaic with HYPERMOSAIC_FFMPEG_ROOT=$((Join-Path $PSScriptRoot "..\ffmpeg-8.1\local-install"))" -ForegroundColor Green
