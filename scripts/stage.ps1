# ============================================================================
# stage.ps1 - assembles a playable folder from YOUR OWN copy of the game + the port's binaries.
#   -GameRoot     extracted Xbox One package (the folder with Mount\ and EmbeddedXvd\)
#   -Destination  folder to create (default: dist_game next to this repository)
#   -BinDir       built binaries (default: bin\ from scripts\build.ps1, or the release zip's root)
# Game data is never copied into the repository: dat\ and EmbeddedXvd\ become NTFS junctions.
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$GameRoot,
    [string]$Destination = (Join-Path $PSScriptRoot "..\dist_game"),
    [string]$BinDir = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $BinDir) { $BinDir = if (Test-Path "$root\bin\d3d12_x.dll") { "$root\bin" } else { "$root" } }
foreach ($b in "d3d12_x.dll", "RushLauncher.exe") {
    if (-not (Test-Path "$BinDir\$b")) { throw "$b not found in $BinDir. Run scripts\build.ps1 first (or use the release zip)." }
}
if (-not (Test-Path $GameRoot)) { throw "GameRoot does not exist: $GameRoot" }
$mount = Join-Path $GameRoot "Mount"
if (-not (Test-Path "$mount\game_release_xbox.exe")) {
    if (Test-Path "$GameRoot\game_release_xbox.exe") { $mount = $GameRoot } else { throw "game_release_xbox.exe not found in $GameRoot or $GameRoot\Mount" }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$Destination = (Resolve-Path $Destination).Path
Write-Host "Staging into $Destination" -ForegroundColor Cyan

# 1. game executable and metadata (from your copy)
foreach ($f in "game_release_xbox.exe", "appxmanifest.xml", "XboxIntegratedMultiplayer.dll", "Bink2XboxOne.dll",
               "appdata.bin", "Catalog000.bin", "resources.pri", "saveversions.txt", "SplashScreen.png") {
    if (Test-Path "$mount\$f") { Copy-Item "$mount\$f" $Destination -Force; Write-Host "  game file: $f" }
}
# 2. console system libraries the game loads (from your copy's EmbeddedXvd)
$sys32 = Join-Path $GameRoot "EmbeddedXvd\Windows\System32"
foreach ($f in "AcpHal.dll", "ACP_kernel.bin", "xg_x.dll", "xaudio2_9.dll", "D3DCompiler_47.dll") {
    if (Test-Path "$sys32\$f") { Copy-Item "$sys32\$f" $Destination -Force; Write-Host "  system library: $f" }
}
# 3. port binaries
foreach ($f in "kernelx.dll", "combase.dll", "EtwPlus.dll", "MMDevAPI.dll", "d3d12_x.dll", "RushLauncher.exe") {
    if (Test-Path "$BinDir\$f") { Copy-Item "$BinDir\$f" $Destination -Force; Write-Host "  port: $f" -ForegroundColor Green }
}
# 4. large asset folders as junctions (nothing is copied)
foreach ($j in @(@{ src = "$mount\dat"; dst = "$Destination\dat" }, @{ src = "$GameRoot\EmbeddedXvd"; dst = "$Destination\EmbeddedXvd" })) {
    if ((Test-Path $j.src) -and -not (Test-Path $j.dst)) { New-Item -ItemType Junction -Path $j.dst -Target $j.src | Out-Null; Write-Host "  junction: $($j.dst) -> $($j.src)" }
}
# 5. onrush-pc.exe (renamed copy with the ONRUSH name/icon; game_release_xbox.exe is not modified)
$brand = Join-Path $PSScriptRoot "brand_exe.py"
if ((Test-Path $brand) -and (Get-Command python -ErrorAction SilentlyContinue)) { python -I $brand $Destination }
else { Copy-Item "$Destination\game_release_xbox.exe" "$Destination\onrush-pc.exe" -Force }
Write-Host "`nDone. Start $Destination\RushLauncher.exe" -ForegroundColor Green
