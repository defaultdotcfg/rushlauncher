// rl_dlss.dll: NVIDIA DLSS Super Resolution for d3d12_x.dll's upscaler (src/d3d12_x/upscaler.inc).
// A separate module because the NVIDIA NGX SDK is proprietary: it is linked here, not into the GPL bridge, and the
// bridge only loads this DLL at run time (DLSS is simply unavailable when the file or an RTX GPU is missing).
// The interface is plain C so either side can be rebuilt alone.
#pragma once
#include <d3d12.h>

#define RL_DLSS_API_VERSION 1

struct RlDlssEval {
  ID3D12Resource* color;    // render resolution, NON_PIXEL|PIXEL_SHADER_RESOURCE
  ID3D12Resource* depth;    // render resolution, reversed (1 = near)
  ID3D12Resource* motion;   // render resolution, RG16F, current -> previous in UV units
  ID3D12Resource* output;   // display resolution, UNORDERED_ACCESS
  float jitterX, jitterY;   // render pixels
  float mvScaleX, mvScaleY; // motion * scale = pixels
  unsigned renderW, renderH;
  int reset;
  float frameTimeMs;
};

extern "C" {
// Loads NGX on the device (dllDir: folder of nvngx_dlss.dll). 1 = DLSS Super Resolution is available.
typedef int(__cdecl* PFN_RlDlssInit)(ID3D12Device* device, const wchar_t* dllDir, const wchar_t* logDir);
// (Re)creates the feature for this size: maxRender = largest render size, display = output size.
typedef int(__cdecl* PFN_RlDlssCreate)(ID3D12GraphicsCommandList* cl, unsigned maxRenderW, unsigned maxRenderH, unsigned displayW, unsigned displayH);
typedef int(__cdecl* PFN_RlDlssEvaluate)(ID3D12GraphicsCommandList* cl, const RlDlssEval* e);
typedef void(__cdecl* PFN_RlDlssShutdown)();
typedef int(__cdecl* PFN_RlDlssApiVersion)();
}
