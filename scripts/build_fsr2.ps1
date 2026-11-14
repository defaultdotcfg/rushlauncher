# ============================================================================
# AMD FidelityFX Super Resolution 2 (MIT) for d3d12_x.dll: downloads the official v2.2.1 source
# (pinned by hash), builds the DX12 backend as static libraries with the static runtime (/MT, like the
# rest of the port) and prints the API folder (headers + bin\ffx_fsr2_api\*.lib).
# Needs CMake and Visual Studio with the C++ workload. Nothing is rebuilt when the libraries exist.
# ============================================================================
param(
    [string]$Out = "$PSScriptRoot\..\deps\fsr2"
)

$ErrorActionPreference = "Stop"
$version = "2.2.1"
$sha256 = "B1F7F5C138A230CAA5D40609FA739B239800794604B62A1A667C1E45F2F1E9AD"

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$api = "$Out\FidelityFX-FSR2-$version\src\ffx-fsr2-api"
$libs = @("$api\bin\ffx_fsr2_api\ffx_fsr2_api_x64.lib", "$api\bin\ffx_fsr2_api\ffx_fsr2_api_dx12_x64.lib")

if (-not (($libs | ForEach-Object { Test-Path $_ }) -contains $false)) {
    Write-Output $api
    return
}

$zip = "$Out\fsr2-$version.zip"
if (-not (Test-Path $zip)) {
    Write-Host "Downloading FidelityFX FSR2 $version..." -ForegroundColor Cyan
    Invoke-WebRequest "https://github.com/GPUOpen-Effects/FidelityFX-FSR2/archive/refs/tags/v$version.zip" -OutFile $zip
}
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha256) {
    Remove-Item $zip
    throw "FSR2 source archive hash mismatch"
}
if (-not (Test-Path "$api\CMakeLists.txt")) { Expand-Archive $zip -DestinationPath $Out -Force }

Write-Host "Building FidelityFX FSR2 $version (DX12, /MT)..." -ForegroundColor Cyan
$flags = "/MT /O2 /Ob2 /DNDEBUG"
& cmake -S $api -B "$Out\build" -A x64 -DFFX_FSR2_API_DX12=ON -DFFX_FSR2_API_VK=OFF `
    "-DCMAKE_CXX_FLAGS_RELEASE=$flags" "-DCMAKE_C_FLAGS_RELEASE=$flags" | Out-Host
if ($LASTEXITCODE) { throw "FSR2 configure failed" }
& cmake --build "$Out\build" --config Release | Out-Host
if ($LASTEXITCODE) { throw "FSR2 build failed" }
foreach ($l in $libs) { if (-not (Test-Path $l)) { throw "FSR2 build did not produce $l" } }
Write-Output $api
