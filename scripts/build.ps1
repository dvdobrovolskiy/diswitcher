param(
  [ValidateSet("Release","Debug")]
  [string]$Config = "Release",
  [ValidateSet("auto","msvc","mingw")]
  [string]$Toolchain = "auto"
)

$ErrorActionPreference = "Stop"

function Test-Exe([string]$name) {
  return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

function Import-MsvcEnvIfAvailable {
  if (Test-Exe "cl") { return $true }

  $vswhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
    (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\Installer\vswhere.exe")
  )

  $vswhere = $vswhereCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $vswhere) { return $false }

  $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  if (-not $installPath) { return $false }

  $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) { return $false }

  $envDump = cmd /c "call `"$vcvars`" >nul && set"
  foreach ($line in $envDump) {
    if ($line -match "^(?<k>[^=]+)=(?<v>.*)$") {
      $k = $Matches.k
      $v = $Matches.v
      try { Set-Item -Path "Env:$k" -Value $v } catch {}
    }
  }

  return (Test-Exe "cl")
}

function Import-MingwEnvIfAvailable {
  if (Test-Exe "gcc") { return $true }

  $paths = @(
    "C:\msys64\mingw64\bin",
    "C:\msys64\ucrt64\bin",
    "C:\mingw64\bin",
    "C:\MinGW\bin"
  )
  foreach ($p in $paths) {
    if (Test-Path (Join-Path $p "gcc.exe")) {
      $env:Path = "$p;$env:Path"
      break
    }
  }

  return (Test-Exe "gcc")
}

if ($Toolchain -eq "auto") {
  if (Import-MsvcEnvIfAvailable) { $Toolchain = "msvc" }
  elseif (Import-MingwEnvIfAvailable) { $Toolchain = "mingw" }
  else {
    throw @"
No supported compiler found.

Install one of:
  - MSVC Build Tools (cl.exe) + Windows SDK
  - MSYS2 MinGW-w64 (gcc.exe)

Then re-run:
  .\scripts\build.ps1 -Config $Config
"@
  }
}

$outDir = Join-Path $PSScriptRoot "..\build-$Toolchain-$Config"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$defs = @("UNICODE","_UNICODE","WIN32_LEAN_AND_MEAN","NOMINMAX")

if ($Toolchain -eq "msvc") {
  $cflags = @("/nologo","/W4","/utf-8")
  if ($Config -eq "Release") { $cflags += "/O2" } else { $cflags += @("/Od","/Zi") }
  foreach ($d in $defs) { $cflags += "/D$d" }

  $exe = Join-Path $outDir "Diswitcher.exe"
  Push-Location $outDir
  try {
    # Generate icon + compile resources so the EXE has a real icon in Explorer/Taskbar.
    $iconGen = Join-Path $outDir "icon_gen.exe"
    & cl /nologo /O2 /utf-8 "..\tools\icon_gen.c" /Fe:$iconGen user32.lib gdi32.lib | Write-Host
    if (Test-Path $iconGen) {
      & $iconGen (Join-Path $outDir "diswitcher.ico") | Out-Null
      $rcFile = Join-Path $outDir "diswitcher.rc"
      '1 ICON "diswitcher.ico"' | Set-Content -Encoding ASCII -Path $rcFile
      $resFile = Join-Path $outDir "diswitcher.res"
      if (Test-Exe "rc.exe") {
        & rc.exe /nologo /fo $resFile $rcFile | Out-Null
      }
    }

    $res = Join-Path $outDir "diswitcher.res"
    if (Test-Path $res) {
      & cl @cflags "..\src\main.c" $res /Fe:$exe user32.lib shell32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS | Write-Host
    } else {
      & cl @cflags "..\src\main.c" /Fe:$exe user32.lib shell32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS | Write-Host
    }
  } finally {
    Pop-Location
  }
  Write-Host "Built: $exe"
}
elseif ($Toolchain -eq "mingw") {
  $cflags = @("-Wall","-Wextra")
  if ($Config -eq "Release") { $cflags += "-O2" } else { $cflags += @("-O0","-g") }
  foreach ($d in $defs) { $cflags += "-D$d" }

  $exe = Join-Path $outDir "Diswitcher.exe"
  $iconGen = Join-Path $outDir "icon_gen.exe"
  & gcc @cflags "-mconsole" (Join-Path $PSScriptRoot "..\tools\icon_gen.c") "-o" $iconGen "-luser32" "-lgdi32"
  if (Test-Path $iconGen) {
    & $iconGen (Join-Path $outDir "diswitcher.ico") | Out-Null
    $rcFile = Join-Path $outDir "diswitcher.rc"
    '1 ICON "diswitcher.ico"' | Set-Content -Encoding ASCII -Path $rcFile
    $resObj = Join-Path $outDir "diswitcher_res.o"
    if (Test-Exe "windres") {
      & windres $rcFile -O coff -o $resObj
    }
  }

  $resObj = Join-Path $outDir "diswitcher_res.o"
  if (Test-Path $resObj) {
    & gcc @cflags "-municode" "-mwindows" (Join-Path $PSScriptRoot "..\src\main.c") $resObj "-o" $exe "-luser32" "-lshell32" "-lgdi32"
  } else {
    & gcc @cflags "-municode" "-mwindows" (Join-Path $PSScriptRoot "..\src\main.c") "-o" $exe "-luser32" "-lshell32" "-lgdi32"
  }
  Write-Host "Built: $exe"
}
else {
  throw "Unknown toolchain: $Toolchain"
}
