# Third-party code

| Part of this repository | Derived from | License | License text |
|---|---|---|---|
| `src/d3d12_x`, `src/MMDevAPI` | [WinDurango](https://github.com/WinDurango/WinDurango) (D3D12X, MMDevAPI) | LGPL-3.0 | `licenses/WinDurango-LGPL-3.0.txt` |
| `src/kernelx`, `src/combase`, `src/EtwPlus` | SlimEra by XWine1 (ERA kernelx, incl. `MapTitleEsramPages`) | MIT, Copyright (c) 2025 XWine1 | `licenses/SlimEra-MIT.txt` |

The upscaler options use the vendors' own libraries. None of their files is kept in this repository: `scripts/fetch_upscalers.ps1` downloads them from the official repositories at a fixed version (checked by hash) when building, and the release zip ships the runtime DLLs, unmodified, next to the port's:

| Component | Files in the release | License |
|---|---|---|
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.4.0 (FSR 3.1 / FSR 4) | `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` (signed binaries); `d3d12_x.dll` uses only its API headers and loads the DLLs at run time | AMD FidelityFX SDK license, Copyright (C) Advanced Micro Devices, Inc., `licenses/AMD-FidelityFX-SDK-LICENSE.md` |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) 310.9.16 | `nvngx_dlss.dll`; `rl_dlss.dll` links the SDK's NGX library | NVIDIA RTX SDKs License, `licenses/NVIDIA-RTX-SDKs-LICENSE.txt` |

The NVIDIA SDK is proprietary, so it is kept out of the GPL code: it is linked only into `rl_dlss.dll` (`src/rl_dlss`, a small module written for this project), which `d3d12_x.dll` loads at run time when present; without it DLSS is simply unavailable. `rl_dlss.dll` contains source code provided by NVIDIA Corporation (the NGX helper functions).

The release zip also bundles, unmodified, in `translator\`:

| Component | License | License text (in the zip) |
|---|---|---|
| [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) 1.8.2502 (`dxc.exe`, `dxcompiler.dll`, `dxil.dll`) | University of Illinois/NCSA + MIT; `dxil.dll` under the Microsoft license included with the DXC release | `translator\licenses\DirectXShaderCompiler-LICENSE-*.txt` |
| Python runtime (packaged with [PyInstaller](https://pyinstaller.org), whose bootloader is GPL with an exception for bundled programs) | PSF License | `translator\licenses\Python-LICENSE.txt` |

Everything else (launcher, GCN→HLSL translator, tessellation bypass, scripts) was written for this project and is distributed under the repository license (GPL-3.0, `LICENSE`). LGPL-3.0 code may be redistributed under GPL-3.0 (LGPL-3.0 section 2).

No file of the game ONRUSH, or of the Xbox One system software, is included. The source contains only what is needed to interoperate with them: function/structure layouts, enum mappings and shader hashes.
