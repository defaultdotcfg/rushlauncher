# Third-party code

| Part of this repository | Derived from | License | License text |
|---|---|---|---|
| `src/d3d12_x`, `src/MMDevAPI` | [WinDurango](https://github.com/WinDurango/WinDurango) (D3D12X, MMDevAPI) | LGPL-3.0 | `licenses/WinDurango-LGPL-3.0.txt` |
| `src/kernelx`, `src/combase`, `src/EtwPlus` | SlimEra by XWine1 (ERA kernelx, incl. `MapTitleEsramPages`) | MIT, Copyright (c) 2025 XWine1 | `licenses/SlimEra-MIT.txt` |

`d3d12_x.dll` also links, unmodified, [AMD FidelityFX Super Resolution 2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2) 2.2.1 (the FSR 2 upscaler option; source downloaded and built by `scripts/build_fsr2.ps1`): MIT, Copyright (c) 2022-2023 Advanced Micro Devices, Inc., `licenses/FidelityFX-FSR2-MIT.txt`.

The release zip also bundles, unmodified, in `translator\`:

| Component | License | License text (in the zip) |
|---|---|---|
| [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) 1.8.2502 (`dxc.exe`, `dxcompiler.dll`, `dxil.dll`) | University of Illinois/NCSA + MIT; `dxil.dll` under the Microsoft license included with the DXC release | `translator\licenses\DirectXShaderCompiler-LICENSE-*.txt` |
| Python runtime (packaged with [PyInstaller](https://pyinstaller.org), whose bootloader is GPL with an exception for bundled programs) | PSF License | `translator\licenses\Python-LICENSE.txt` |

Everything else (launcher, GCN→HLSL translator, tessellation bypass, scripts) was written for this project and is distributed under the repository license (GPL-3.0, `LICENSE`). LGPL-3.0 code may be redistributed under GPL-3.0 (LGPL-3.0 section 2).

No file of the game ONRUSH, or of the Xbox One system software, is included. The source contains only what is needed to interoperate with them: function/structure layouts, enum mappings and shader hashes.
