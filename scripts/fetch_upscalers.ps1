# ============================================================================
# Upscaler SDK files for the build (nothing of them is kept in the repository):
#   - NVIDIA DLSS SDK 310.9.16 (headers, nvsdk_ngx_s.lib, nvngx_dlss.dll) for rl_dlss.dll
#   - AMD FidelityFX SDK 2.4.0 (API headers, signed amd_fidelityfx_loader/upscaler_dx12.dll) for FSR 3.1 / FSR 4
# Each file is downloaded once from the official GitHub repositories at a fixed tag and checked by SHA-256.
# Prints the output folder (deps\upscalers): dlss\..., ffx\Kits\FidelityFX\... (original relative paths).
# ============================================================================
param(
    [string]$Out = "$PSScriptRoot\..\deps\upscalers"
)
$ErrorActionPreference = "Stop"
$src = @{
    dlss = "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16"
    ffx  = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0"
}
$files = @(
    @("dlss", "include/nvsdk_ngx.h", "96F21266A05EF06BB49F58C89DE1A7845F2D9B1E0E0E069FE03740B8EAF0344D"),
    @("dlss", "include/nvsdk_ngx_defs.h", "A9481CBC78D53EE56639F0A573EC095C6C1EA1E5F296D139C20C42BF342C2E1B"),
    @("dlss", "include/nvsdk_ngx_defs_dlssd.h", "AB2EF79188233B199CDEBB224A283F2B7D8F03EA95ADF880A8B225AEA2F08586"),
    @("dlss", "include/nvsdk_ngx_defs_dlssg.h", "7874005392F8EFB83E338E2404FB70C0808A746D644F4EBD84E56FFB31ACEB23"),
    @("dlss", "include/nvsdk_ngx_defs_vk.h", "43B821C5D297D86165E6D2F96A476F5C30F2158C7D5A0451425C6B927ABFEC88"),
    @("dlss", "include/nvsdk_ngx_helpers.h", "4569AA7566667D8C0D93C8E50DDD3F2C4FA1C89DA536FFDC2571BBDC97A539E9"),
    @("dlss", "include/nvsdk_ngx_helpers_cuda.h", "18F2FD3ADDA56124A975A9E5F7347E61CA2E25BD58E78B7922EDF6C5C0857281"),
    @("dlss", "include/nvsdk_ngx_helpers_d3d.h", "9B3C356CD2D04E7924C0B6B97FC1F1A37E5D30095A8C22133A5A08398CEF2C4C"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssd.h", "4144EF09594DCF0DDEC5C2DF1E0F87531049AB76DAF995A8607B255C2BDB0DF9"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssd_cuda.h", "32A74447C395586DD8C8C4D1EF29DF1A327A040DC12B01A101723DC083C47BF1"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssd_d3d.h", "BE5ECFC8F5ECB17861600485CE9FB617DC56FAA5F79EE1525081A54BF4281475"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssd_vk.h", "89351368B07B0AFA28E0923DE1B0276991EFB9B5E95922C89047DFCFCE70BB7D"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssg.h", "A8ABDF14B689F2CBBBBED9991CB369FACB1F8D5B260C5AE4426A0E9146D95DDE"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssg_d3d.h", "6343BC03FE12238F19A6D02C427EB9F9DC72433C58A00C164B05473ACA710ED9"),
    @("dlss", "include/nvsdk_ngx_helpers_dlssg_vk.h", "EEC70917A474A1F8BE114EB11D5DA3028CC752C410C3B3F12E03B29017D60726"),
    @("dlss", "include/nvsdk_ngx_helpers_vk.h", "754697D9FAA93913985FC91FCEAE2073763461710D742A1F262CED2A7B6B62C9"),
    @("dlss", "include/nvsdk_ngx_loader.h", "E0F8339E696DC26568A9D4AEFEE90C8497CE5AE048CC8B24CCC213CD3A8A2073"),
    @("dlss", "include/nvsdk_ngx_params.h", "51A1F94116CF82F95C63A2C197371ED22E08BDDB09918D36CC6E2AC712753C80"),
    @("dlss", "include/nvsdk_ngx_params_dlssd.h", "7E4FC6D5C84B436D31E289F863762A178AB2EF5D8C1FF40B614B01B342385F05"),
    @("dlss", "include/nvsdk_ngx_params_dlssg.h", "82B1D573B4B48D4CEFC26AC01162A45646D99F0B84806DAC9DB3516CC5F733B5"),
    @("dlss", "include/nvsdk_ngx_standalone_common.h", "231C6DEB2515B5D30DCCBC18A992CD5A2470A6BF3833B2A3B0FC7E3912BD9756"),
    @("dlss", "include/nvsdk_ngx_standalone_cuda.h", "9CCA6B101025E2C9E915C8E7F1DD8798DC7CBB7AE45DC5E98D7F73CD3045BFB9"),
    @("dlss", "include/nvsdk_ngx_vk.h", "3BE54D0103CCE02174FD34912C60C9F40C73B5EEC919E42613A2268C589C53AD"),
    @("dlss", "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib", "7B9A5239C1145AF5F645AB472AA066F9EA79DB36AC819562312AAE69D65F3765"),
    @("dlss", "lib/Windows_x86_64/rel/nvngx_dlss.dll", "D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E"),
    @("dlss", "LICENSE.txt", "3027F23CA5A46DD9CB8183FBD522983A86F64D7DAAC5982912BF9F214671F294"),
    @("ffx", "Kits/FidelityFX/api/include/ffx_api.h", "30C3F76FCDAA74A8C553E9B2ED8538EB2B6458586127FFCCD0BBA2053E688F88"),
    @("ffx", "Kits/FidelityFX/api/include/ffx_api_types.h", "83B62646D1254B24FE0C9F18322A28AF1152E3852CE223879002FD523677B92A"),
    @("ffx", "Kits/FidelityFX/api/include/ffx_api_loader.h", "10795186FD2A8FF53BB13AA0C81079C7FFD4A0B1BB501C629ED0B874C492A20B"),
    @("ffx", "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h", "2082D6C2914E9C2FA9FAE6247D35F64C643CCB703311C83C8DBA9A35356BC711"),
    @("ffx", "Kits/FidelityFX/upscalers/include/ffx_upscale.h", "98635F4FAADBB051A6DB5F8E5706DA6C54DE0278015A3CB230C5FC51F877E5A5"),
    @("ffx", "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll", "E2191D3C0828BE45EB8AFFD4E3EC0378F2293ADC345B772A7170EDCF3B021ECB"),
    @("ffx", "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll", "AAD4FCCCCB29084A655F9A4F9F613BD22C8C03FB4A0F8840148CC6543B4CCACE"),
    @("ffx", "Kits/FidelityFX/docs/license.md", "B8933184884F58F13677CC43B7C5BED84BAE7E74C6B8829822E5FBDF88CEB6CD")
)
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
foreach ($f in $files) {
    $dst = Join-Path $Out (Join-Path $f[0] $f[1])
    if ((Test-Path $dst) -and (Get-FileHash $dst -Algorithm SHA256).Hash -eq $f[2]) { continue }
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Write-Host "Downloading $($f[0])/$($f[1])..." -ForegroundColor Cyan
    Invoke-WebRequest "$($src[$f[0]])/$($f[1])" -OutFile $dst
    if ((Get-FileHash $dst -Algorithm SHA256).Hash -ne $f[2]) { Remove-Item $dst; throw "hash mismatch: $($f[0])/$($f[1])" }
}
Write-Output $Out
