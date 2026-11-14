// ---------------------------------------------------------------------------
// Upscaler: AMD FidelityFX Super Resolution 2 in place of the game's temporal pass.
//
// The game's TAA is the compute shader 83717de270dda518 ("history:UpdateHistoryBuffer"), one dispatch per
// frame at output resolution that resolves the render-resolution scene into AccumHistoryA/B (ping-pong). It also
// writes a display-resolution depth and a history weight that later passes read, so it keeps running; after it:
//   1. a small compute pass that turns the game's depth + per-object world-space motion into the screen-space
//      motion vectors FSR2 needs (same reconstruction as the game's shader, from its camera constants);
//   2. the FSR2 dispatch (scene colour, depth, motion vectors, the game's own jitter for this frame);
//   3. a copy of the result into both history buffers (the passes after the TAA read one of them).
// Everything is recorded on the game's command list, then the game's compute state is put back.
//
// Bindings of the TAA dispatch (game root signature): table param 1 = [CB camera][t0 depth][..][t6 colour]
// [t7 world motion][t8/t9 history]; the jitter index is (TAA object +0x60) & 7 into the game's 8-entry table.
// Included by onrush_d3d12_x.cpp after bridge_cmdlist.h.
// ---------------------------------------------------------------------------
#include "ffx_fsr2.h"
#include "dx12/ffx_fsr2_dx12.h"

namespace ups {

static const unsigned long long kTaaCsHash = 0x83717de270dda518ull;
static const UINT kTabParam = 1;   // root table: slot 0 = camera CBV, slot 1 + n = shader t<n>
static const UINT kSlotDepth = 1, kSlotColour = 7, kSlotMotion = 8, kSlotHistA = 9, kSlotHistB = 10;

static uintptr_t g_exe = 0;
static volatile LONG g_jitterCtr = -1;   // written by the hook in the game's TAA setup (exe+0x155FC8)

static bool IsTaa() {
  static thread_local void* s_pso = nullptr; static thread_local bool s_is = false;
  if (t_lastPso != s_pso) { s_pso = t_lastPso; s_is = t_lastPso && (unsigned long long)BcPsoHash(t_lastPso) == kTaaCsHash; }
  return s_is;
}

// 0 = the game's TAA, 1 = FSR2: the GRAPHICS > UPSCALER row (onrush.ini [Graphics] Upscaler, ONRUSH_UPSCALER).
static int Mode() { return gfxopt::g_upscaler ? 1 : 0; }

// ---- A device that calls the real D3D12 functions directly. The bridge wraps the device's vtable to
// translate the game's Xbox-specific calls; FSR2 and our own pass must not go through that.
struct ProxyDevice { void** vtbl; ID3D12Device* real; };
static ProxyDevice g_proxy = {};
static ID3D12Device* Dev() {
  if (g_proxy.vtbl) return (ID3D12Device*)&g_proxy;
  void** orig = psodc::g_origDevVtbl;
  if (!orig || !g_pMainDevice) return nullptr;
  const int kSlots = 128;
  uint8_t* code = (uint8_t*)AllocateNearAddress((void*)&Dev, kSlots * 16 + kSlots * sizeof(void*));
  DWORD old = 0;
  if (!code || !VirtualProtect(code, kSlots * 16 + kSlots * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
  void** vt = (void**)(code + kSlots * 16);
  for (int i = 0; i < kSlots; ++i) {
    uint8_t* q = code + i * 16;
    MEMORY_BASIC_INFORMATION mbi = {};
    const bool valid = VirtualQuery(orig[i], &mbi, sizeof(mbi)) && (mbi.Protect == PAGE_EXECUTE_READ || mbi.Protect == PAGE_EXECUTE_READWRITE);
    // mov rcx, [rcx+8] ; mov rax, imm64 ; jmp rax  (this -> real device, then the driver's function)
    q[0] = 0x48; q[1] = 0x8B; q[2] = 0x49; q[3] = 0x08; q[4] = 0x48; q[5] = 0xB8;
    void* f = valid ? orig[i] : nullptr; memcpy(q + 6, &f, 8); q[14] = 0xFF; q[15] = 0xE0;
    vt[i] = valid ? (void*)q : nullptr;
  }
  FlushInstructionCache(GetCurrentProcess(), code, kSlots * 16);
  g_proxy.real = g_pMainDevice; g_proxy.vtbl = vt;
  return (ID3D12Device*)&g_proxy;
}

// ---- Motion vectors from depth + world-space object motion (the game's reconstruction).
static const char kMvHlsl[] = R"HLSL(
#define RS "CBV(b0), RootConstants(num32BitConstants=6, b1), DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0))"
cbuffer Cam : register(b0) { float4 C[80]; }
cbuffer P : register(b1) { uint2 Size; float2 InvSize; float2 Jit; }
Texture2D<float> Depth : register(t0);
Texture2D<float4> WorldMotion : register(t1);
RWTexture2D<float2> Mv : register(u0);
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= Size.x || id.y >= Size.y) return;
  float d = Depth[id.xy];
  float2 uv = (id.xy + 0.5) * InvSize;
  // camera-relative position: ray through the pixel scaled by the view distance of the reversed depth
  float3 ray = C[77].xyz + C[78].xyz * uv.x + C[79].xyz * uv.y;
  float k = 1.0 / (C[75].y + C[75].x * d);
  float3 pos = (ray - C[35].xyz) * (-k) + C[35].xyz;
  pos -= WorldMotion[id.xy].xyz;   // where this point was one frame ago
  float4 clip = pos.x * C[40] + pos.y * C[41] + pos.z * C[42] + C[43];
  float2 prevNdc = clip.xy / clip.w;
  float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
  // the rays are jittered and the previous view-projection is not: measure from the unjittered position
  Mv[id.xy] = (clip.w > 0) ? prevUv - (uv - Jit) : float2(0, 0);
}
)HLSL";

struct Gpu {
  bool tried = false, ok = false;
  ID3D12RootSignature* mvRs = nullptr; ID3D12PipelineState* mvPso = nullptr;
  ID3D12DescriptorHeap* heap = nullptr; UINT inc = 0; UINT ring = 0;
  ID3D12Resource* mv = nullptr; UINT mvW = 0, mvH = 0;
  ID3D12Resource* out = nullptr; UINT outW = 0, outH = 0;
  FfxFsr2Context ctx = {}; bool ctxOk = false; UINT ctxW = 0, ctxH = 0;
  void* scratch = nullptr;
  LARGE_INTEGER last = {};
  UINT frames = 0;
  LONG lastFrame = -100;   // render frame of the previous FSR2 dispatch
};
static Gpu g;

static bool InitStatic() {
  if (g.tried) return g.ok;
  g.tried = true;
  ID3D12Device* dev = Dev();
  if (!dev) { log_line("[ups] no device (vtable %p device %p)\r\n", (void*)psodc::g_origDevVtbl, (void*)g_pMainDevice); return false; }
  EnsureD3DCompilerLoaded();
  if (!g_pfnD3DCompile) { log_line("[ups] D3DCompile unavailable\r\n"); return false; }
  ID3DBlob* cs = nullptr; ID3DBlob* err = nullptr;
  HRESULT hr = g_pfnD3DCompile(kMvHlsl, sizeof(kMvHlsl) - 1, "ups_mv", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
  if (FAILED(hr) || !cs) { log_line("[ups] motion vector shader failed: %s\r\n", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
  hr = dev->CreateRootSignature(0, cs->GetBufferPointer(), cs->GetBufferSize(), IID_PPV_ARGS(&g.mvRs));
  if (SUCCEEDED(hr)) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = g.mvRs; pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.mvPso));
  }
  cs->Release();
  if (FAILED(hr)) { log_line("[ups] motion vector pipeline failed hr=0x%08lX\r\n", (unsigned long)hr); return false; }
  D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 3 * 16; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) { log_line("[ups] descriptor heap failed\r\n"); return false; }
  g.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g.scratch = malloc(ffxFsr2GetScratchMemorySizeDX12());
  QueryPerformanceCounter(&g.last);
  g.ok = g.scratch != nullptr;
  log_line("[ups] FSR2 ready\r\n");
  return g.ok;
}

static ID3D12Resource* MakeTex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_STATES st) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
  d.Format = f; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* r = nullptr;
  if (FAILED(Dev()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
  return r;
}

static bool EnsureSizes(UINT rw, UINT rh, UINT dw, UINT dh) {
  if (g.mv && (g.mvW != rw || g.mvH != rh)) { g.mv->Release(); g.mv = nullptr; }
  if (!g.mv) { g.mv = MakeTex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); g.mvW = rw; g.mvH = rh; }
  if (g.out && (g.outW != dw || g.outH != dh)) { g.out->Release(); g.out = nullptr; }
  if (!g.out) { g.out = MakeTex(dw, dh, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); g.outW = dw; g.outH = dh; }
  if (g.ctxOk && (g.ctxW != dw || g.ctxH != dh)) { ffxFsr2ContextDestroy(&g.ctx); g.ctxOk = false; }
  if (!g.ctxOk) {
    FfxFsr2ContextDescription cd = {};
    if (ffxFsr2GetInterfaceDX12(&cd.callbacks, Dev(), g.scratch, ffxFsr2GetScratchMemorySizeDX12()) != FFX_OK) { log_line("[ups] FSR2 interface failed\r\n"); return false; }
    cd.device = ffxGetDeviceDX12(Dev());
    cd.maxRenderSize = { dw, dh };   // any render scale up to native without re-creating
    cd.displaySize = { dw, dh };
    cd.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_DEPTH_INVERTED | FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    FfxErrorCode e = ffxFsr2ContextCreate(&g.ctx, &cd);
    if (e != FFX_OK) { log_line("[ups] FSR2 context failed (%d)\r\n", (int)e); return false; }
    g.ctxOk = true; g.ctxW = dw; g.ctxH = dh; g.frames = 0;
    log_line("[ups] FSR2 context %ux%u\r\n", dw, dh);
  }
  return g.mv && g.out;
}

// Put the game's compute state back after our passes (they changed heaps, root signature, PSO, arguments).
static void RestoreGameState(BridgeCommandList* self) {
  ID3D12GraphicsCommandList* cl = self->m_pRealCmdList;
  UINT8 kind[16]; memcpy(kind, t_cKind, sizeof(kind)); UINT nConst[16]; memcpy(nConst, t_cConstN, sizeof(nConst));
  if (t_nHeaps) cl->SetDescriptorHeaps(t_nHeaps, t_heaps);
  if (t_csRs) BridgeCmdList_SetComputeRootSignature(self, (ID3D12RootSignature*)t_csRs);   // (clears t_cKind)
  memcpy(t_cKind, kind, sizeof(kind)); memcpy(t_cConstN, nConst, sizeof(nConst));
  for (UINT p = 0; p < 16; ++p) {
    switch (kind[p]) {
      case 1: { D3D12_GPU_DESCRIPTOR_HANDLE h = { t_ctab[p] }; cl->SetComputeRootDescriptorTable(p, h); break; }
      case 2: cl->SetComputeRootConstantBufferView(p, t_ccbv[p]); break;
      case 3: cl->SetComputeRootShaderResourceView(p, t_cVa[p]); break;
      case 4: cl->SetComputeRootUnorderedAccessView(p, t_cVa[p]); break;
      case 5: if (t_cConstN[p]) cl->SetComputeRoot32BitConstants(p, t_cConstN[p], t_cConst[p], 0); break;
    }
  }
  if (t_lastPso) cl->SetPipelineState((ID3D12PipelineState*)t_lastPso);
}

static bool Run(BridgeCommandList* self) {
  if (!InitStatic()) return false;
  const UINT64 tab = t_ctab[kTabParam];
  if (!tab || t_cKind[kTabParam] != 1) return false;
  DescInfo dDepth = {}, dColour = {}, dMotion = {};
  ID3D12Resource* depth = DescTrack_TableRes(tab, kSlotDepth, 1, &dDepth);
  ID3D12Resource* colour = DescTrack_TableRes(tab, kSlotColour, 1, &dColour);
  ID3D12Resource* motion = DescTrack_TableRes(tab, kSlotMotion, 1, &dMotion);
  ID3D12Resource* histA = DescTrack_TableRes(tab, kSlotHistA, 1, nullptr);
  ID3D12Resource* histB = DescTrack_TableRes(tab, kSlotHistB, 1, nullptr);
  const UINT64 camVa = DescTrack_TableCbv(tab, 0);
  const LONG ctr = g_jitterCtr;
  if (!depth || !colour || !motion || !histA || !histB || !camVa || ctr < 0) {
    static volatile LONG s_w = 0; if (InterlockedIncrement(&s_w) <= 5) log_line("[ups] TAA inputs not found (depth %p colour %p motion %p hist %p/%p cam %llX ctr %ld)\r\n", (void*)depth, (void*)colour, (void*)motion, (void*)histA, (void*)histB, (unsigned long long)camVa, (long)ctr);
    return false;
  }
  const D3D12_RESOURCE_DESC rc = colour->GetDesc(), rh = histA->GetDesc();
  const UINT rw = (UINT)rc.Width, rhh = rc.Height, dw = (UINT)rh.Width, dh = rh.Height;
  if (!EnsureSizes(rw, rhh, dw, dh)) return false;
  ID3D12GraphicsCommandList* cl = self->m_pRealCmdList;
  ID3D12Device* dev = Dev();

  // inputs readable by compute (pixel|non-pixel = FSR2's COMPUTE_READ)
  const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ListEnsureState(self, depth, kRead);
  ListEnsureState(self, colour, kRead);
  ListEnsureState(self, motion, kRead);

  // 1. motion vectors
  const UINT base = (g.ring++ % 16) * 3;
  D3D12_CPU_DESCRIPTOR_HANDLE c0 = g.heap->GetCPUDescriptorHandleForHeapStart(); c0.ptr += (SIZE_T)base * g.inc;
  D3D12_GPU_DESCRIPTOR_HANDLE g0 = g.heap->GetGPUDescriptorHandleForHeapStart(); g0.ptr += (UINT64)base * g.inc;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
  sv.Format = DXGI_FORMAT_R32_FLOAT; dev->CreateShaderResourceView(depth, &sv, c0);
  D3D12_CPU_DESCRIPTOR_HANDLE c1 = c0; c1.ptr += g.inc; sv.Format = dMotion.fmt ? dMotion.fmt : DXGI_FORMAT_R16G16B16A16_FLOAT; dev->CreateShaderResourceView(motion, &sv, c1);
  D3D12_CPU_DESCRIPTOR_HANDLE c2 = c1; c2.ptr += g.inc;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.Format = DXGI_FORMAT_R16G16_FLOAT; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; dev->CreateUnorderedAccessView(g.mv, nullptr, &uv, c2);
  cl->SetDescriptorHeaps(1, &g.heap);
  cl->SetComputeRootSignature(g.mvRs);
  cl->SetPipelineState(g.mvPso);
  cl->SetComputeRootConstantBufferView(0, camVa);
  // the game's jitter in FSR2's convention (pixels; the projection moves by (+2x/w, -2y/h) in NDC): minus the
  // table entry, as measured from the camera constants (row 6)
  const float* tbl = *(const float* const*)(g_exe + 0x013F4EE8);   // the game's 8-entry jitter table
  const UINT ji = (UINT)ctr & 7;
  const float jx = -tbl[2 * ji], jy = -tbl[2 * ji + 1];
  struct { UINT w, h; float iw, ih, jx, jy; } pc = { rw, rhh, 1.0f / rw, 1.0f / rhh, jx / rw, jy / rhh };
  cl->SetComputeRoot32BitConstants(1, 6, &pc, 0);
  cl->SetComputeRootDescriptorTable(2, g0);
  cl->Dispatch((rw + 7) / 8, (rhh + 7) / 8, 1);
  D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = g.mv; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = kRead;
  cl->ResourceBarrier(1, &b);

  // 2. FSR2
  LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
  float dt = (float)((now.QuadPart - g.last.QuadPart) * 1000.0 / fq.QuadPart); g.last = now;
  if (dt <= 0.0f || dt > 200.0f) dt = 16.6f;
  FfxFsr2DispatchDescription dd = {};
  dd.commandList = ffxGetCommandListDX12(cl);
  dd.color = ffxGetResourceDX12(&g.ctx, colour, L"colour", FFX_RESOURCE_STATE_COMPUTE_READ);
  dd.depth = ffxGetResourceDX12(&g.ctx, depth, L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
  dd.motionVectors = ffxGetResourceDX12(&g.ctx, g.mv, L"mv", FFX_RESOURCE_STATE_COMPUTE_READ);
  dd.output = ffxGetResourceDX12(&g.ctx, g.out, L"out", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
  dd.jitterOffset = { jx, jy };
  dd.motionVectorScale = { (float)rw, (float)rhh };
  dd.renderSize = { rw, rhh };
  dd.enableSharpening = true; dd.sharpness = 0.3f;
  dd.frameTimeDelta = dt;
  dd.preExposure = 1.0f;
  // new history after a pause in FSR2 (option switched, loading, menus without the 3D view)
  const LONG frame = g_rtLogFrame;
  dd.reset = g.frames == 0 || frame - g.lastFrame > 2;
  g.lastFrame = frame;
  dd.cameraNear = 0.1f; dd.cameraFar = 55000.0f;
  dd.cameraFovAngleVertical = 1.1f;
  dd.viewSpaceToMetersFactor = 1.0f;
  FfxErrorCode e = ffxFsr2ContextDispatch(&g.ctx, &dd);
  b.Transition.StateBefore = kRead; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  cl->ResourceBarrier(1, &b);
  if (e != FFX_OK) { static volatile LONG s_e = 0; if (InterlockedIncrement(&s_e) <= 5) log_line("[ups] FSR2 dispatch failed (%d)\r\n", (int)e); }
  ++g.frames;

  // 3. result -> both history buffers
  D3D12_RESOURCE_BARRIER ob = b; ob.Transition.pResource = g.out; ob.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; ob.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cl->ResourceBarrier(1, &ob);
  for (ID3D12Resource* h : { histA, histB }) {
    const D3D12_RESOURCE_STATES was = ListCurrentState(self, h);
    ListEnsureState(self, h, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(h, g.out);
    ListEnsureState(self, h, was);
  }
  ob.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; ob.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  cl->ResourceBarrier(1, &ob);

  RestoreGameState(self);
  { static volatile LONG s_l = 0; if (InterlockedIncrement(&s_l) <= 3) log_line("[ups] FSR2 %ux%u -> %ux%u jitter[%u] %.4f %.4f\r\n", rw, rhh, dw, dh, ji, jx, jy); }
  return true;
}

// Hook in the game's TAA constant setup: remember the frame counter it takes the jitter index from.
static void Install(HMODULE hExe) {
  g_exe = (uintptr_t)hExe;
  uint8_t* site = (uint8_t*)hExe + 0x00155FC8;   // mov ecx,[rbx+60h] ; vxorps xmm0,xmm0,xmm0
  static const uint8_t kSite[] = { 0x8B, 0x4B, 0x60, 0xC5, 0xF8, 0x57, 0xC0 };
  const uint8_t* tblPtr = (const uint8_t*)hExe + 0x013F4EE8;
  if (memcmp(site, kSite, sizeof(kSite)) != 0 || site[7] != 0x89 || *(const uintptr_t*)tblPtr == 0) {
    log_line("[ups] WARNING: unexpected TAA setup code; FSR2 unavailable\r\n");
    return;
  }
  uint8_t* mem = (uint8_t*)AllocateNearAddress(site, 0x40);
  if (!mem) return;
  DWORD old = 0; VirtualProtect(mem, 0x40, PAGE_EXECUTE_READWRITE, &old);
  uint8_t* q = mem;
  *q++ = 0x8B; *q++ = 0x4B; *q++ = 0x60;                       // mov ecx, [rbx+60h]
  *q++ = 0x50;                                                  // push rax
  *q++ = 0x48; *q++ = 0xB8; { void* p = (void*)&g_jitterCtr; memcpy(q, &p, 8); q += 8; }   // mov rax, &g_jitterCtr
  *q++ = 0x89; *q++ = 0x08;                                     // mov [rax], ecx
  *q++ = 0x58;                                                  // pop rax
  *q++ = 0xC5; *q++ = 0xF8; *q++ = 0x57; *q++ = 0xC0;           // vxorps xmm0, xmm0, xmm0
  q = gfxopt::EmitJmpAbs(q, site + sizeof(kSite));
  FlushInstructionCache(GetCurrentProcess(), mem, 0x40);
  if (gfxopt::WriteJmpRel(site, mem, sizeof(kSite))) log_line("[ups] TAA jitter hook installed\r\n");
}

// ONRUSH_TAA_DUMP=1 (diagnostic): the constants of a few TAA dispatches are copied on the GPU, right before the
// dispatch, into a readback buffer and printed a few frames later (the game fills them after recording).
static const UINT kRbChunk = 80 * 16;   // rows 0..79 of one constant buffer
struct Pending { LONG n; LONG frame; LONG ctr; int count; char tag[8][16]; };
static Pending g_pend = {};
static volatile LONG g_pendReady = 0;
static ID3D12Resource* g_rb = nullptr; static const uint8_t* g_rbMem = nullptr;
static bool FindBuf(D3D12_GPU_VIRTUAL_ADDRESS va, ID3D12Resource** res, UINT64* off, UINT64* avail) {
  LONG n = g_placedBufCount; if (n > 20000) n = 20000;
  for (LONG k = 0; k < n; ++k) {
    const PlacedBufInfo& q = g_placedBufs[k];
    if (va >= q.va && va < q.va + q.size) { *res = q.res; *off = va - q.va; *avail = q.size - (va - q.va); return true; }
  }
  return false;
}
static void Dump(BridgeCommandList* self) {
  static const bool s_on = GetEnvironmentVariableA("ONRUSH_TAA_DUMP", nullptr, 0) > 0;
  if (!s_on || g_pendReady) return;
  static volatile LONG s_n = 0; LONG n = InterlockedIncrement(&s_n);
  if (!((n >= 1100 && n < 1104) || (n > 1104 && n % 600 == 0))) return;
  if (!g_rb) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_CUSTOM; hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 8 * kRbChunk; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr; void* m = nullptr;
    if (FAILED(g_TrueCreateCommittedResource(g_pMainDevice, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&r)) || !r || FAILED(r->Map(0, nullptr, &m)) || !m) return;
    g_rb = r; g_rbMem = (const uint8_t*)m;
  }
  Pending pd = {}; pd.n = n; pd.frame = (LONG)g_rtLogFrame; pd.ctr = g_jitterCtr;
  auto grab = [&](D3D12_GPU_VIRTUAL_ADDRESS va, const char* tag) {
    if (!va || pd.count >= 8) return;
    ID3D12Resource* res; UINT64 off, av;
    if (!FindBuf(va, &res, &off, &av)) return;
    UINT64 len = av < kRbChunk ? av : kRbChunk;
    D3D12_RESOURCE_STATES was = ListCurrentState(self, res);
    ListEnsureState(self, res, D3D12_RESOURCE_STATE_COPY_SOURCE);
    self->m_pRealCmdList->CopyBufferRegion(g_rb, (UINT64)pd.count * kRbChunk, res, off, len);
    ListEnsureState(self, res, was);
    strcpy_s(pd.tag[pd.count], tag); ++pd.count;
  };
  for (UINT p = 0; p < 16; ++p) {
    char t[16]; sprintf_s(t, "root%u", p); grab(t_ccbv[p], t);
    if (t_ctab[p]) for (UINT k = 0; k < 4; ++k) { sprintf_s(t, "tab%u.cb%u", p, k); grab(DescTrack_TableCbv(t_ctab[p], k), t); }
    if (t_ctab[p]) DescTrack_LogTable("taa", p, t_ctab[p], 24);
  }
  g_pend = pd; InterlockedExchange(&g_pendReady, 1);
}

}  // namespace ups

static void Ups_OnExecute() {
  using namespace ups;
  if (!g_pendReady || g_rtLogFrame < g_pend.frame + 6) return;
  const Pending& pd = g_pend;
  char b[200]; sprintf_s(b, "[taa] ---- dispatch #%ld frame %ld jitter counter %ld\r\n", (long)pd.n, (long)pd.frame, (long)pd.ctr); log_line("%s", b);
  static const int kRows[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 35, 40, 41, 42, 43, 61, 65, 68, 75, 76, 77, 78, 79 };
  for (int i = 0; i < pd.count; ++i)
    for (int r : kRows) {
      const float* f = (const float*)(g_rbMem + (size_t)i * kRbChunk + r * 16); const UINT* u = (const UINT*)f;
      sprintf_s(b, "[taa] %s row%d %g %g %g %g | %08X %08X %08X %08X\r\n", pd.tag[i], r, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]); log_line("%s", b);
    }
  InterlockedExchange(&g_pendReady, 0);
}

static bool Upscaler_OnDispatch(BridgeCommandList* self, UINT gx, UINT gy, UINT gz) {
  if (!ups::IsTaa()) return false;
  ups::Dump(self);
  if (ups::Mode() != 1 || self->m_type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
  // The game's pass still runs: besides the colour history it writes the display-resolution depth and the
  // history weight that depth of field / motion blur read later. FSR2 then replaces the colour.
  LONG bc = BcBegin(self, 'D', gx, gy, gz);
  self->m_pRealCmdList->Dispatch(gx, gy, gz);
  BcEnd(self, bc);
  ups::Run(self);
  return true;
}
