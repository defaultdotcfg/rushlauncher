// ---------------------------------------------------------------------------
// Graphics diagnostic (launcher option "Diagnóstico gráfico" -> ONRUSH_GFX_DIAG=1).
//
// For reports we cannot reproduce (a GPU we do not have, e.g. the screen turning white on one AMD card): it
// names the pass whose output goes wrong. A few frames of the 3D scene are measured (the first one ~1 s after it
// starts, then one every ~1.5 s, within a fixed budget). In a measured frame, after every compute dispatch the
// float textures its shader can write (its RWTexture declarations, mapped through the root signature), and on
// every render target change the float targets the previous draws wrote, are scanned on the GPU by a small compute
// pass: NaN / infinity count, largest finite value, share of bright pixels (> 1 and > 16). A NaN or infinity is a
// problem; large finite values are not (65504, the half-float maximum, is a normal "far" value in depth chains).
// The results land in CPU memory and are logged a few frames later (no stall), one line per scan with the shader
// hash. Lines of a healthy frame match from one machine to another, so a player's log can be put next to ours.
// Everything is recorded on the game's command list, and its compute state put back after (as the upscaler does).
// Included by onrush_d3d12_x.cpp after upscaler.inc.
// ---------------------------------------------------------------------------
namespace gdiag {

static bool On() {
  static int s_on = -1;
  if (s_on < 0) { char b[8] = {}; DWORD n = GetEnvironmentVariableA("ONRUSH_GFX_DIAG", b, sizeof(b)); s_on = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0; }
  return s_on == 1;
}

// UAV ranges of every descriptor-table parameter, per root signature (filled with the root signature).
struct UavRange { UINT param, space, base, num, offset; };
static std::unordered_map<void*, std::vector<UavRange>> g_rsUav; static SRWLOCK g_rsUavLock = SRWLOCK_INIT;
// Writable textures a translated compute shader declares (register space, first register, count), per shader hash.
struct TexDecl { UINT space, reg, count; };
static std::unordered_map<UINT64, std::vector<TexDecl>> g_decl; static SRWLOCK g_declLock = SRWLOCK_INIT;

static const UINT kMaxRec = 32768;      // scans per session (results: 32 bytes each)
static const LONG kMaxPerFrame = 2000;  // scans in one measured frame
static const LONG kWarmup = 60, kEvery = 90, kLag = 10;   // frames

struct Rec { char kind; UINT64 hash, vs; ID3D12Resource* res; UINT w, h, d; DXGI_FORMAT fmt; UINT param, slot; };
static Rec g_rec[kMaxRec];
static volatile LONG g_recN = 0;

static volatile LONG g_arm = -1;        // frame (g_rtLogFrame) being measured, -1 = none
static LONG g_armStart = 0;
static volatile LONG g_lastTaa = -100;  // last frame with the game's TAA dispatch (= the 3D scene is drawn)
static LONG g_sceneStart = -1, g_nextAt = -1;
static int g_measured = 0, g_verbose = 0;
static bool g_firstBadShown = false;
struct Pending { LONG frame, first, end; int index; };
static Pending g_pend[8]; static int g_pendN = 0;
static std::unordered_set<UINT64> g_seen; static SRWLOCK g_seenLock = SRWLOCK_INIT;

static thread_local void* t_drawList = nullptr;   // list that drew since its last render target change (measured frame)
static thread_local void* t_drawPso = nullptr;

static inline bool Active() { const LONG a = g_arm; return a >= 0 && g_rtLogFrame == a; }

struct Gpu {
  bool tried = false, ok = false;
  ID3D12RootSignature* rs = nullptr; ID3D12PipelineState* pso[3] = {};
  ID3D12DescriptorHeap* heap = nullptr; UINT inc = 0;
  ID3D12Resource* res = nullptr; const volatile UINT* mem = nullptr;
};
static Gpu g;
static std::unordered_map<int, bool> g_typedLoad; static SRWLOCK g_tlLock = SRWLOCK_INIT;

static const char kHlsl[] = R"HLSL(
#define RS "RootConstants(num32BitConstants=4, b0), UAV(u0), DescriptorTable(SRV(t0)), DescriptorTable(UAV(u1))"
cbuffer P : register(b0) { uint W; uint H; uint D; uint N; };
RWByteAddressBuffer R : register(u0);
#if MODE == 0
Texture2DArray<float4> S : register(t0);
#elif MODE == 1
RWTexture2DArray<float4> S : register(u1);
#else
RWTexture3D<float4> S : register(u1);
#endif
groupshared uint gs[6];
// 0 finite, 1 NaN, 2 infinity (from the bits: no compiler assumption about NaN can drop the test)
uint Cls(float x) { uint b = asuint(x); return ((b >> 23) & 0xFF) == 0xFF ? ((b & 0x7FFFFF) != 0 ? 1u : 2u) : 0u; }
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
  if (gi < 6) gs[gi] = 0;
  GroupMemoryBarrierWithGroupSync();
  if (id.x < W && id.y < H && id.z < D) {
    float4 v = S[id];
    uint nan = 0, inf = 0; float mx = 0, lum = 0;
    [unroll] for (int c = 0; c < 4; ++c) {
      uint k = Cls(v[c]);
      if (k == 1) nan = 1; else if (k == 2) inf = 1;
      else { mx = max(mx, abs(v[c])); if (c < 3) lum = max(lum, v[c]); }
    }
    if (nan) InterlockedAdd(gs[0], 1);
    if (inf) InterlockedAdd(gs[1], 1);
    InterlockedMax(gs[2], asuint(mx));
    if (lum > 1.0) InterlockedAdd(gs[3], 1);
    if (lum > 16.0) InterlockedAdd(gs[4], 1);
    InterlockedAdd(gs[5], 1);
  }
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0) {
    uint o;
    R.InterlockedAdd(0, gs[0], o); R.InterlockedAdd(4, gs[1], o); R.InterlockedMax(8, gs[2], o);
    R.InterlockedAdd(12, gs[3], o); R.InterlockedAdd(16, gs[4], o); R.InterlockedAdd(20, gs[5], o);
  }
}
)HLSL";

static bool Init() {
  if (g.tried) return g.ok;
  g.tried = true;
  ID3D12Device* dev = ups::Dev();
  if (!dev) { log_line("[diag] no device\r\n"); return false; }
  EnsureD3DCompilerLoaded();
  if (!g_pfnD3DCompile) { log_line("[diag] D3DCompile unavailable\r\n"); return false; }
  for (int m = 0; m < 3; ++m) {
    char mode[2] = { (char)('0' + m), 0 };
    D3D_SHADER_MACRO defs[] = { { "MODE", mode }, { nullptr, nullptr } };
    ID3DBlob* cs = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = g_pfnD3DCompile(kHlsl, sizeof(kHlsl) - 1, "gfx_diag", defs, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
    if (FAILED(hr) || !cs) { log_line("[diag] scan shader %d failed: %s\r\n", m, err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    if (!g.rs) hr = dev->CreateRootSignature(0, cs->GetBufferPointer(), cs->GetBufferSize(), IID_PPV_ARGS(&g.rs));
    if (SUCCEEDED(hr)) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = g.rs; pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
      hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso[m]));
    }
    cs->Release();
    if (FAILED(hr)) { log_line("[diag] scan pipeline %d failed hr=0x%08lX\r\n", m, (unsigned long)hr); return false; }
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kMaxRec; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) { log_line("[diag] descriptor heap failed\r\n"); return false; }
  g.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  // In CPU memory, written by the scans and read here directly: no copy and no state change (lists of different
  // queues can scan at the same time); each scan has its own 32 bytes.
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kMaxRec * 32; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
  bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_CUSTOM; hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  void* m = nullptr; D3D12_RANGE none = { 0, 0 };
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g.res))) || FAILED(g.res->Map(0, &none, &m)) || !m) { log_line("[diag] results buffer failed\r\n"); return false; }
  memset(m, 0, (size_t)bd.Width);
  g.mem = (const volatile UINT*)m;
  g.ok = true;
  log_line("[diag] scan passes ready\r\n");
  return true;
}

// The float view format to scan with, or UNKNOWN (integer / UNORM data, depth, compressed: not scanned).
static DXGI_FORMAT FloatFmt(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
static bool IsTypeless(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_UNKNOWN || f == DXGI_FORMAT_R32G32B32A32_TYPELESS || f == DXGI_FORMAT_R16G16B16A16_TYPELESS || f == DXGI_FORMAT_R32G32_TYPELESS ||
         f == DXGI_FORMAT_R16G16_TYPELESS || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_R16_TYPELESS;
}
static const char* FmtName(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F"; case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R32G32_FLOAT: return "RG32F"; case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F"; case DXGI_FORMAT_R32_FLOAT: return "R32F"; case DXGI_FORMAT_R16_FLOAT: return "R16F";
    default: return "?";
  }
}
// Typed UAV loads of formats other than R32_FLOAT are optional in D3D12 (asked once per format).
static bool TypedLoad(DXGI_FORMAT f) {
  if (f == DXGI_FORMAT_R32_FLOAT) return true;
  AcquireSRWLockShared(&g_tlLock); auto it = g_typedLoad.find((int)f); bool known = it != g_typedLoad.end(), ok = known && it->second; ReleaseSRWLockShared(&g_tlLock);
  if (known) return ok;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { f };
  ok = SUCCEEDED(ups::Dev()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
  AcquireSRWLockExclusive(&g_tlLock); g_typedLoad[(int)f] = ok; ReleaseSRWLockExclusive(&g_tlLock);
  if (!ok) log_line("[diag] typed UAV load of format %d not supported: those targets are skipped\r\n", (int)f);
  return ok;
}

// The RWTexture declarations of a translated compute shader, from its HLSL
// ("RWTexture2D<float4> Ux_sp17_r0[36] : register(u0, space17);"). Buffers are left out: their contents are
// not float colours, any bit pattern (and so any "NaN") is valid there.
static const std::vector<TexDecl>& Decls(UINT64 hash) {
  AcquireSRWLockShared(&g_declLock);
  auto it = g_decl.find(hash);
  if (it != g_decl.end()) { const std::vector<TexDecl>& r = it->second; ReleaseSRWLockShared(&g_declLock); return r; }
  ReleaseSRWLockShared(&g_declLock);
  std::vector<TexDecl> v;
  char path[MAX_PATH]; sprintf_s(path, "shaders\\translated\\%016llx.hlsl", (unsigned long long)hash);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") == 0 && f) {
    char ln[512];
    while (fgets(ln, sizeof(ln), f)) {
      if (strncmp(ln, "RWTexture", 9) != 0) continue;
      const char* r = strstr(ln, "register(u");
      if (!r) continue;
      TexDecl d = { 0, 0, 1 };
      d.reg = (UINT)strtoul(r + 10, nullptr, 10);
      const char* sp = strstr(r, "space");
      if (sp) d.space = (UINT)strtoul(sp + 5, nullptr, 10);
      const char* br = strchr(ln, '[');
      if (br && br < r) d.count = (UINT)strtoul(br + 1, nullptr, 10);
      if (!d.count) d.count = 1;
      v.push_back(d);
    }
    fclose(f);
  }
  AcquireSRWLockExclusive(&g_declLock);
  auto& r = g_decl[hash]; r = std::move(v);
  ReleaseSRWLockExclusive(&g_declLock);
  return r;
}

static bool FirstTimeThisFrame(UINT64 hash, ID3D12Resource* res) {
  const UINT64 key = hash ^ ((UINT64)(uintptr_t)res * 0x9E3779B97F4A7C15ull);
  AcquireSRWLockExclusive(&g_seenLock); bool nw = g_seen.insert(key).second; ReleaseSRWLockExclusive(&g_seenLock);
  return nw;
}

// One scan of `res` recorded on the list. srvPath: read through an SRV (render targets, with state transitions);
// otherwise through a UAV (dispatch outputs: in the UAV state already, no transition).
static bool Scan(BridgeCommandList* self, char kind, UINT64 hash, UINT64 vs, ID3D12Resource* res, DXGI_FORMAT viewFmt, UINT param, UINT slot, bool srvPath) {
  if (!res || res == g.res || g_recN - g_armStart >= kMaxPerFrame) return false;
  D3D12_RESOURCE_DESC rd = {};
  __try { rd = res->GetDesc(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  if (rd.SampleDesc.Count > 1 || (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return false;
  if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) return false;
  if (srvPath && (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) return false;
  if (!srvPath && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
  if (srvPath) {   // its state in this list must be known (OMSetRenderTargets put it there), or the transitions would guess
    ListStateMap* lm = GetListStates(self, false);
    if (!lm || lm->find(res) == lm->end()) return false;
  }
  const DXGI_FORMAT fmt = FloatFmt(IsTypeless(viewFmt) ? rd.Format : viewFmt);
  if (fmt == DXGI_FORMAT_UNKNOWN || (!srvPath && !TypedLoad(fmt))) return false;
  const UINT W = (UINT)rd.Width, H = rd.Height, D = rd.DepthOrArraySize;
  const int mode = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 2 : (srvPath ? 0 : 1);
  if (!FirstTimeThisFrame(hash, res) || !Init()) return false;
  const LONG rec = InterlockedIncrement(&g_recN) - 1;
  if (rec >= (LONG)kMaxRec) return false;
  g_rec[rec] = { kind, hash, vs, res, W, H, D, fmt, param, slot };

  ID3D12Device* dev = ups::Dev();
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += (SIZE_T)rec * g.inc;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += (UINT64)rec * g.inc;
  if (mode == 0) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = fmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2DArray.MipLevels = 1; sv.Texture2DArray.ArraySize = D;
    dev->CreateShaderResourceView(res, &sv, cpu);
  } else {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.Format = fmt;
    if (mode == 1) { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uv.Texture2DArray.ArraySize = D; }
    else { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; uv.Texture3D.WSize = (UINT)-1; }
    dev->CreateUnorderedAccessView(res, nullptr, &uv, cpu);
  }

  ID3D12GraphicsCommandList* cl = self->m_pRealCmdList;
  D3D12_RESOURCE_BARRIER uavAll = {}; uavAll.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  D3D12_RESOURCE_STATES was = D3D12_RESOURCE_STATE_COMMON;
  if (srvPath) { was = ListCurrentState(self, res); ListEnsureState(self, res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); }
  else cl->ResourceBarrier(1, &uavAll);   // the game's writes are finished and visible
  cl->SetDescriptorHeaps(1, &g.heap);
  cl->SetComputeRootSignature(g.rs);
  cl->SetPipelineState(g.pso[mode]);
  const UINT pc[4] = { W, H, D, 0 };
  cl->SetComputeRoot32BitConstants(0, 4, pc, 0);
  cl->SetComputeRootUnorderedAccessView(1, g.res->GetGPUVirtualAddress() + (UINT64)rec * 32);
  cl->SetComputeRootDescriptorTable(mode == 0 ? 2 : 3, gpu);
  cl->Dispatch((W + 7) / 8, (H + 7) / 8, D);
  if (srvPath) ListEnsureState(self, res, was);
  else cl->ResourceBarrier(1, &uavAll);   // our reads done before the game writes it again
  return true;
}

static void Report(const Pending& p) {
  const LONG end = p.end < (LONG)kMaxRec ? p.end : (LONG)kMaxRec;
  int bad = 0, firstBad = -1, ran = 0;
  float sceneMax = 0; double sceneO1 = 0, sceneO16 = 0; bool scene = false;
  for (LONG i = p.first; i < end; ++i) {
    UINT r[8]; for (int k = 0; k < 8; ++k) r[k] = g.mem[(size_t)i * 8 + k];
    if (!r[5]) continue;
    ++ran;
    float mx; memcpy(&mx, &r[2], 4);
    if (r[0] || r[1]) { ++bad; if (firstBad < 0) firstBad = (int)i; }
    if (g_rec[i].kind == 'D' && g_rec[i].hash == ups::kTaaCsHash) {
      scene = true; if (mx > sceneMax) sceneMax = mx;
      const double o1 = 100.0 * r[3] / r[5], o16 = 100.0 * r[4] / r[5];
      if (o1 > sceneO1) sceneO1 = o1;
      if (o16 > sceneO16) sceneO16 = o16;
    }
  }
  // every scan of the first measured frames and of frames where something looks wrong; otherwise only the bad ones
  const bool alarm = bad > 0 || sceneO16 > 30.0;
  const bool verbose = p.index <= 3 || (alarm && g_verbose < 6);
  if (verbose && p.index > 3) ++g_verbose;
  char b[400];
  if (scene) sprintf_s(b, "[diag] ---- quadro %ld (medição %d): %d passos medidos, %d com NaN/infinito | cena (TAA): max %.4g, >1: %.1f%%, >16: %.1f%%%s\r\n",
                       (long)p.frame, p.index, ran, bad, sceneMax, sceneO1, sceneO16, end - p.first >= kMaxPerFrame ? " | limite de medições do quadro atingido" : "");
  else sprintf_s(b, "[diag] ---- quadro %ld (medição %d): %d passos medidos, %d com NaN/infinito\r\n", (long)p.frame, p.index, ran, bad);
  log_line("%s", b);
  if (!g_firstBadShown && firstBad >= 0) {
    g_firstBadShown = true;
    const Rec& q = g_rec[firstBad];
    sprintf_s(b, "[diag] PRIMEIRO PROBLEMA: %s %016llx (%ux%u %s), quadro %ld\r\n", q.kind == 'D' ? "compute" : "pixel shader", (unsigned long long)q.hash,
              q.w, q.h, FmtName(q.fmt), (long)p.frame);
    log_line("%s", b);
  }
  for (LONG i = p.first; i < end; ++i) {
    const Rec& q = g_rec[i];
    UINT r[8]; for (int k = 0; k < 8; ++k) r[k] = g.mem[(size_t)i * 8 + k];
    const bool isBad = r[0] || r[1];
    if (!verbose && !isBad) continue;
    if (!r[5]) continue;   // the list was never executed
    float mx; memcpy(&mx, &r[2], 4);
    char who[64];
    if (q.kind == 'D') sprintf_s(who, "cs=%016llx u%u.%u", (unsigned long long)q.hash, q.param, q.slot);
    else sprintf_s(who, "ps=%016llx vs=%016llx rt%u", (unsigned long long)q.hash, (unsigned long long)q.vs, q.param);
    sprintf_s(b, "[diag] #%ld %c %s %ux%ux%u %s nan=%u inf=%u max=%.4g >1=%.1f%% >16=%.1f%%%s\r\n", (long)(i - p.first), q.kind, who, q.w, q.h, q.d, FmtName(q.fmt),
              r[0], r[1], mx, 100.0 * r[3] / r[5], 100.0 * r[4] / r[5], isBad ? "  <-- PROBLEMA" : "");
    log_line("%s", b);
  }
}

}  // namespace gdiag

// Called with every root signature the game creates: remember its UAV ranges.
static void Diag_RecordRs(void* rs, const D3D12_ROOT_SIGNATURE_DESC* d) {
  if (!gdiag::On() || !rs || !d) return;
  std::vector<gdiag::UavRange> v;
  for (UINT i = 0; i < d->NumParameters; ++i) {
    const D3D12_ROOT_PARAMETER& p = d->pParameters[i];
    if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
    UINT run = 0;
    for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
      const D3D12_DESCRIPTOR_RANGE& rg = p.DescriptorTable.pDescriptorRanges[r];
      UINT off = (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) ? run : rg.OffsetInDescriptorsFromTableStart;
      UINT n = (rg.NumDescriptors == UINT_MAX) ? 4096 : rg.NumDescriptors;
      if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) v.push_back({ i, rg.RegisterSpace, rg.BaseShaderRegister, n, off });
      run = off + n;
    }
  }
  AcquireSRWLockExclusive(&gdiag::g_rsUavLock); gdiag::g_rsUav[rs] = std::move(v); ReleaseSRWLockExclusive(&gdiag::g_rsUavLock);
}

// After every dispatch: notes that the 3D scene runs (the game's TAA) and, in a measured frame, scans the float
// textures the shader can write.
static void Diag_AfterDispatch(BridgeCommandList* self) {
  using namespace gdiag;
  if (!On() || !self || !self->m_pRealCmdList) return;
  if (ups::IsTaa()) g_lastTaa = g_rtLogFrame;
  if (!Active() || !t_lastPso) return;
  const UINT64 hash = (UINT64)BcPsoHash(t_lastPso);
  const std::vector<TexDecl>& decls = Decls(hash);
  if (decls.empty()) return;
  std::vector<UavRange> ranges;
  AcquireSRWLockShared(&g_rsUavLock); auto it = g_rsUav.find(t_csRs); if (it != g_rsUav.end()) ranges = it->second; ReleaseSRWLockShared(&g_rsUavLock);
  bool any = false;
  for (const TexDecl& dc : decls)
    for (const UavRange& rg : ranges) {
      if (rg.space != dc.space || rg.param >= 16 || t_cKind[rg.param] != 1 || !t_ctab[rg.param]) continue;
      const UINT lo = dc.reg > rg.base ? dc.reg : rg.base;
      const UINT hi = (dc.reg + dc.count < rg.base + rg.num) ? dc.reg + dc.count : rg.base + rg.num;
      for (UINT reg = lo; reg < hi; ++reg) {
        const UINT slot = rg.offset + (reg - rg.base);
        DescInfo di = {};
        ID3D12Resource* r = DescTrack_TableRes(t_ctab[rg.param], slot, 2, &di);
        if (r) any |= Scan(self, 'D', hash, 0, r, di.fmt, rg.param, slot, false);
      }
    }
  if (any) ups::RestoreGameState(self);
}

static void Diag_OnDraw(BridgeCommandList* self) {
  if (!gdiag::On() || !gdiag::Active()) return;
  gdiag::t_drawList = self; gdiag::t_drawPso = t_lastPso;
}

// Before the render targets change (and at Close): scans the targets the previous draws wrote.
static void Diag_BeforeRtChange(BridgeCommandList* self) {
  using namespace gdiag;
  if (!On() || t_drawList != (void*)self) return;
  t_drawList = nullptr;
  if (!Active() || !self->m_pRealCmdList || self->m_type != D3D12_COMMAND_LIST_TYPE_DIRECT || t_om.list != (void*)self || !t_om.n) return;
  static UINT s_rtvInc = 0;
  if (!s_rtvInc && g_pMainDevice) s_rtvInc = g_pMainDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const unsigned long long hs[3] = { t_om.h0, t_om.h1, t_om.h2 };
  const UINT64 ps = (UINT64)BcPsoHash(t_drawPso), vs = (UINT64)BcVsHash(t_drawPso);
  bool any = false;
  for (UINT i = 0; i < t_om.n && i < 3; ++i) {
    const SIZE_T h = t_om.single ? (SIZE_T)(t_om.h0 + (unsigned long long)i * s_rtvInc) : (SIZE_T)hs[i];
    if (h) any |= Scan(self, 'R', ps, vs, LookupRtvResource(h), DXGI_FORMAT_UNKNOWN, i, 0, true);
  }
  if (any) ups::RestoreGameState(self);
}

// Once per present: logs finished measurements, and picks the next frame to measure.
static void Diag_OnPresent(LONG frame) {
  using namespace gdiag;
  if (!On()) return;
  static bool s_hello = false;
  if (!s_hello) { s_hello = true; log_line("[diag] diagnóstico gráfico ligado: alguns quadros da cena 3D serão medidos\r\n"); }
  if (g_arm >= 0 && frame > g_arm) {   // the measured frame was recorded
    if (g_pendN < (int)_countof(g_pend)) g_pend[g_pendN++] = { g_arm, g_armStart, g_recN, g_measured };
    InterlockedExchange(&g_arm, -1);
  }
  for (int i = 0; i < g_pendN;) {
    if (frame >= g_pend[i].frame + kLag && g.ok) { Report(g_pend[i]); g_pend[i] = g_pend[--g_pendN]; }
    else ++i;
  }
  if (g_lastTaa < frame - 3) { g_sceneStart = -1; return; }   // no 3D scene (loading)
  if (g_sceneStart < 0) { g_sceneStart = frame; g_nextAt = frame + kWarmup; }
  if (frame < g_nextAt || g_arm >= 0) return;
  if (g_recN >= (LONG)kMaxRec - kMaxPerFrame) {
    static bool s_full = false;
    if (!s_full) { s_full = true; log_line("[diag] limite de medições desta sessão atingido\r\n"); }
    return;
  }
  AcquireSRWLockExclusive(&g_seenLock); g_seen.clear(); ReleaseSRWLockExclusive(&g_seenLock);
  g_armStart = g_recN; ++g_measured; g_nextAt = frame + kEvery;
  InterlockedExchange(&g_arm, frame);
}
