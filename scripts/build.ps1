# ============================================================================
# OnRush Native Port - Automated Build Script (PowerShell)
# Builds all translation bridge DLLs and the OnRush Launcher.
# ============================================================================
param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path "$PSScriptRoot\.."

# Upscaler SDK files (AMD FidelityFX SDK for FSR 3.1/4, NVIDIA DLSS SDK for rl_dlss.dll), downloaded once
# into deps\upscalers and checked by hash
$ups = (& "$PSScriptRoot\fetch_upscalers.ps1" | Select-Object -Last 1)
$ffxInc = "$ups\ffx\Kits\FidelityFX"
$dlss = "$ups\dlss"
if (-not (Test-Path "$ffxInc\api\include\ffx_api.h") -or -not (Test-Path "$dlss\include\nvsdk_ngx.h")) { throw "upscaler SDK files not found in $ups" }

# Locate MSVC toolchain dynamically
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}

$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

if (-not $vsPath) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    )
    foreach ($cand in $candidates) {
        if (Test-Path "$cand\VC\Tools\MSVC") {
            $vsPath = $cand
            break
        }
    }
}

if (-not $vsPath) {
    throw "Could not locate Visual Studio MSVC installation. Please install Visual Studio with C++ desktop workload."
}

$msvcDir = Get-ChildItem "$vsPath\VC\Tools\MSVC" | Sort-Object Name | Select-Object -Last 1
$vc = $msvcDir.FullName

# Locate Windows 10/11 SDK
$sdkBase = "C:\Program Files (x86)\Windows Kits\10"
if (-not (Test-Path $sdkBase)) {
    $sdkBase = "C:\Program Files\Windows Kits\10"
}
$sdkVerDir = Get-ChildItem "$sdkBase\Include" | Where-Object { $_.Name -like "10.0.*" } | Sort-Object Name | Select-Object -Last 1
$ver = $sdkVerDir.Name

Write-Host "Found MSVC: $vc" -ForegroundColor Cyan
Write-Host "Found Windows SDK: $ver ($sdkBase)" -ForegroundColor Cyan

# Configure Environment Variables
$env:PATH = "$vc\bin\Hostx64\x64;$sdkBase\bin\$ver\x64;$env:PATH"
$env:INCLUDE = "$vc\include;$sdkBase\Include\$ver\ucrt;$sdkBase\Include\$ver\shared;$sdkBase\Include\$ver\um;$sdkBase\Include\$ver\winrt"
$env:LIB = "$vc\lib\x64;$sdkBase\Lib\$ver\ucrt\x64;$sdkBase\Lib\$ver\um\x64"

$outDir = "$root\bin"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Invoke-Cl([string[]]$arguments) {
    Write-Host "Compiling: $($arguments[-1])" -ForegroundColor Yellow
    & cl.exe /nologo @arguments
    if ($LASTEXITCODE) { throw "Compilation failed with exit code $LASTEXITCODE" }
}

$kernelx = "$root\src\kernelx"
$combase = "$root\src\combase"
$etwplus = "$root\src\EtwPlus"
$mmdevapi = "$root\src\MMDevAPI"
$d3d12x = "$root\src\d3d12_x"
$launcher = "$root\src\launcher"

Push-Location $outDir
try {
    # 1. kernelx.dll
    Invoke-Cl @("/O2", "/std:c++20", "/LD", "/I$kernelx", "$kernelx\kernelx.cpp")

    # 2. combase.dll
    Invoke-Cl @("/O2", "/std:c++20", "/LD", "/I$kernelx", "$combase\combase.cpp")

    # 3. EtwPlus.dll
    Invoke-Cl @("/O2", "/std:c++20", "/LD", "/I$kernelx", "$etwplus\EtwPlus.cpp")

    # 4. MMDevAPI.dll
    Invoke-Cl @("/O2", "/std:c++20", "/LD", "/I$mmdevapi\include\WinDurango.MMDevAPI",
        "/FeMMDevAPI.dll", "$mmdevapi\src\WinDurango.MMDevAPI.cpp", "/link", "user32.lib",
        "/DEF:$mmdevapi\Exports.def")

    # 5. d3d12_x.dll
    # (FSR and DLSS are loaded at run time: only headers here)
    Invoke-Cl @("/O2", "/std:c++20", "/LD", "/EHsc", "/I$kernelx", "/I$d3d12x", "/I$ffxInc\api\include",
        "/I$ffxInc\upscalers\include", "/Fed3d12_x.dll",
        "$d3d12x\onrush_d3d12_x.cpp", "$d3d12x\xbox_sampler_translator.cpp", "/link", "kernelx.lib")

    # 5b. rl_dlss.dll: NVIDIA DLSS behind a small C interface (the proprietary NGX SDK is linked only here)
    Invoke-Cl @("/O2", "/std:c++20", "/LD", "/EHsc", "/I$dlss\include", "/Ferl_dlss.dll",
        "$root\src\rl_dlss\rl_dlss.cpp", "/link", "$dlss\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib", "advapi32.lib", "user32.lib")

    # 5c. the vendors' runtime DLLs next to the port's
    Copy-Item "$dlss\lib\Windows_x86_64\rel\nvngx_dlss.dll", "$ffxInc\signedbin\amd_fidelityfx_loader_dx12.dll",
        "$ffxInc\signedbin\amd_fidelityfx_upscaler_dx12.dll" $outDir -Force

    # 6. RushLauncher.exe
    # /utf-8: the source is UTF-8 (accents); /MANIFEST:EMBED: Common Controls v6 dependency
    # launcher.rc: the executable icon (app.ico, resource id 1; also the window icon)
    Push-Location $launcher
    & rc.exe /nologo /fo "$outDir\RushLauncher.res" launcher.rc
    $rcExit = $LASTEXITCODE
    Pop-Location
    if ($rcExit) { throw "Resource compilation failed with exit code $rcExit" }
    Invoke-Cl @("/O2", "/std:c++20", "/EHsc", "/utf-8", "/FeRushLauncher.exe",
        "$launcher\OnRushLauncher.cpp", "/link", "/SUBSYSTEM:WINDOWS", "/MANIFEST:EMBED", "$outDir\RushLauncher.res")

    Write-Host "`nAll components built successfully to: $outDir" -ForegroundColor Green
}
finally {
    Pop-Location
}

