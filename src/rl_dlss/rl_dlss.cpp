// rl_dlss.dll: NVIDIA DLSS Super Resolution behind the small C interface of rl_dlss.h (see there why it is a
// separate module). Built by scripts/build.ps1 against the NVIDIA DLSS SDK fetched by scripts/fetch_upscalers.ps1.
// This software contains source code provided by NVIDIA Corporation (the NGX helper functions it uses).
#include <windows.h>
#include <stdio.h>
#include <d3d12.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "rl_dlss.h"

// Identifies this application to NGX (custom engine): a fixed GUID for RushLauncher.
static const char kProjectId[] = "6f0b3c1e-5a52-4b8e-9d3a-52524c444c53";

static ID3D12Device* g_dev = nullptr;
static NVSDK_NGX_Parameter* g_params = nullptr;
static NVSDK_NGX_Handle* g_feature = nullptr;
static bool g_ok = false;
static wchar_t g_dllDir[MAX_PATH] = {};
static const wchar_t* g_paths[1] = { g_dllDir };

static void Log(const char* fmt, ...) {
  char b[512]; va_list a; va_start(a, fmt); _vsnprintf_s(b, _TRUNCATE, fmt, a); va_end(a);
  OutputDebugStringA(b);
}

extern "C" __declspec(dllexport) int __cdecl RlDlssApiVersion() { return RL_DLSS_API_VERSION; }

extern "C" __declspec(dllexport) int __cdecl RlDlssInit(ID3D12Device* device, const wchar_t* dllDir, const wchar_t* logDir) {
  if (g_dev) return g_ok ? 1 : 0;
  g_dev = device;
  if (dllDir) wcsncpy_s(g_dllDir, dllDir, _TRUNCATE);
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = g_paths; info.PathListInfo.Length = g_dllDir[0] ? 1 : 0;
  NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", logDir ? logDir : g_dllDir,
                                                           device, &info, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(r)) { Log("[rl_dlss] NGX init failed 0x%08X\n", (unsigned)r); return 0; }
  r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params);
  if (NVSDK_NGX_FAILED(r) || !g_params) { Log("[rl_dlss] NGX capability parameters failed 0x%08X\n", (unsigned)r); return 0; }
  int avail = 0, needsDriver = 0;
  NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
  NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
  g_ok = avail != 0;
  return g_ok ? 1 : (needsDriver ? -1 : 0);   // -1: a newer NVIDIA driver is needed
}

// Quality mode for the ratio the game renders at (DLSS accepts any render size inside the mode's dynamic range;
// the mode sets the network's expectations).
static NVSDK_NGX_PerfQuality_Value ModeFor(float ratio) {
  if (ratio >= 2.6f) return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
  if (ratio >= 1.85f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  if (ratio >= 1.6f) return NVSDK_NGX_PerfQuality_Value_Balanced;
  if (ratio >= 1.2f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  return NVSDK_NGX_PerfQuality_Value_DLAA;
}

extern "C" __declspec(dllexport) int __cdecl RlDlssCreate(ID3D12GraphicsCommandList* cl, unsigned maxRenderW, unsigned maxRenderH, unsigned displayW, unsigned displayH) {
  if (!g_ok) return 0;
  if (g_feature) { NVSDK_NGX_D3D12_ReleaseFeature(g_feature); g_feature = nullptr; }
  NVSDK_NGX_DLSS_Create_Params cp = {};
  cp.Feature.InWidth = maxRenderW; cp.Feature.InHeight = maxRenderH;
  cp.Feature.InTargetWidth = displayW; cp.Feature.InTargetHeight = displayH;
  cp.Feature.InPerfQualityValue = ModeFor((float)displayW / (float)(maxRenderW ? maxRenderW : 1));
  // HDR linear colour, reversed depth, motion vectors at render resolution without the jitter, exposure from DLSS
  cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &g_feature, g_params, &cp);
  if (NVSDK_NGX_FAILED(r)) { Log("[rl_dlss] create failed 0x%08X\n", (unsigned)r); g_feature = nullptr; return 0; }
  return 1;
}

extern "C" __declspec(dllexport) int __cdecl RlDlssEvaluate(ID3D12GraphicsCommandList* cl, const RlDlssEval* e) {
  if (!g_feature || !e) return 0;
  NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
  ep.Feature.pInColor = e->color; ep.Feature.pInOutput = e->output;
  ep.pInDepth = e->depth; ep.pInMotionVectors = e->motion;
  ep.InJitterOffsetX = e->jitterX; ep.InJitterOffsetY = e->jitterY;
  ep.InMVScaleX = e->mvScaleX; ep.InMVScaleY = e->mvScaleY;
  ep.InRenderSubrectDimensions.Width = e->renderW; ep.InRenderSubrectDimensions.Height = e->renderH;
  ep.InReset = e->reset;
  ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
  ep.InFrameTimeDeltaInMsec = e->frameTimeMs;
  NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cl, g_feature, g_params, &ep);
  return NVSDK_NGX_FAILED(r) ? -(int)(r & 0x7FFFFFFF) : 1;
}

extern "C" __declspec(dllexport) void __cdecl RlDlssShutdown() {
  if (g_feature) { NVSDK_NGX_D3D12_ReleaseFeature(g_feature); g_feature = nullptr; }
  if (g_params) { NVSDK_NGX_D3D12_DestroyParameters(g_params); g_params = nullptr; }
  if (g_dev) { NVSDK_NGX_D3D12_Shutdown1(g_dev); g_dev = nullptr; }
  g_ok = false;
}
