// Pipeline (PSO) disk cache format, shared by d3d12_x.dll (records + uses the cache in game)
// and RushLauncher.exe (rebuilds it before the game starts, e.g. after a driver update).
//
// Files, all in <game>\shaders\pso_cache\:
//   manifest.bin  "ORPSOMF1" + records { u32 'PSCR', u64 key, u32 size, payload[size] }
//   library.bin   ID3D12PipelineLibrary::Serialize() output (driver specific)
//   state.txt     "<manifest bytes covered by library.bin>"
// The payload describes one PSO exactly (every desc field is written explicitly, so struct
// padding never reaches the key). Shaders are referenced by the FNV-1a64 of the original Xbox
// bytecode + the translated file variant + the FNV-1a64 of the translated bytecode itself.
#pragma once
#include <d3d12.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace psocache {

static const char kManifestMagic[8] = { 'O', 'R', 'P', 'S', 'O', 'M', 'F', '1' };
static const uint32_t kRecMagic = 0x52435350u;  // "PSCR"
enum : uint8_t { SH_NONE = 0, SH_CSO = 1, SH_DXIL = 2 };
enum : uint8_t { KIND_GFX = 0, KIND_CS = 1 };

inline uint64_t Fnv(const void *p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  const uint8_t *q = (const uint8_t *)p;
  for (size_t i = 0; i < n; ++i) { h ^= q[i]; h *= 0x100000001b3ull; }
  return h;
}

struct ShaderRef {
  uint64_t orig = 0;     // FNV of the original (Xbox) bytecode = translated file name
  uint8_t variant = SH_NONE;
  uint64_t content = 0;  // FNV of the translated bytecode actually used
};

struct Writer {
  std::vector<uint8_t> b;
  void raw(const void *p, size_t n) { b.insert(b.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
  void u8(uint8_t v) { raw(&v, 1); }
  void u32(uint32_t v) { raw(&v, 4); }
  void u64(uint64_t v) { raw(&v, 8); }
  void f32(float v) { raw(&v, 4); }
  void blob(const void *p, uint32_t n) { u32(n); raw(p, n); }
  void str(const char *s) { uint32_t n = s ? (uint32_t)strlen(s) : 0; blob(s, n); }
  void ref(const ShaderRef &r) { u64(r.orig); u8(r.variant); u64(r.content); }
};

struct Reader {
  const uint8_t *p; size_t n, o = 0; bool ok = true;
  Reader(const void *d, size_t sz) : p((const uint8_t *)d), n(sz) {}
  void raw(void *d, size_t k) { if (!ok || o + k > n) { ok = false; memset(d, 0, k); return; } memcpy(d, p + o, k); o += k; }
  uint8_t u8() { uint8_t v; raw(&v, 1); return v; }
  uint32_t u32() { uint32_t v; raw(&v, 4); return v; }
  uint64_t u64() { uint64_t v; raw(&v, 8); return v; }
  float f32() { float v; raw(&v, 4); return v; }
  std::vector<uint8_t> blob() { uint32_t k = u32(); std::vector<uint8_t> v; if (ok && o + k <= n) { v.assign(p + o, p + o + k); o += k; } else ok = false; return v; }
  std::string str() { auto v = blob(); return std::string(v.begin(), v.end()); }
  ShaderRef ref() { ShaderRef r; r.orig = u64(); r.variant = u8(); r.content = u64(); return r; }
};

// ---- graphics ----
inline void WriteGfx(Writer &w, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d, const std::vector<uint8_t> &rs,
                     const ShaderRef &vs, const ShaderRef &ps) {
  w.u8(KIND_GFX);
  w.blob(rs.data(), (uint32_t)rs.size());
  w.ref(vs); w.ref(ps);
  w.u32(d.BlendState.AlphaToCoverageEnable); w.u32(d.BlendState.IndependentBlendEnable);
  for (int i = 0; i < 8; ++i) {
    const D3D12_RENDER_TARGET_BLEND_DESC &t = d.BlendState.RenderTarget[i];
    w.u32(t.BlendEnable); w.u32(t.LogicOpEnable); w.u32(t.SrcBlend); w.u32(t.DestBlend); w.u32(t.BlendOp);
    w.u32(t.SrcBlendAlpha); w.u32(t.DestBlendAlpha); w.u32(t.BlendOpAlpha); w.u32(t.LogicOp); w.u8(t.RenderTargetWriteMask);
  }
  w.u32(d.SampleMask);
  const D3D12_RASTERIZER_DESC &r = d.RasterizerState;
  w.u32(r.FillMode); w.u32(r.CullMode); w.u32(r.FrontCounterClockwise); w.u32((uint32_t)r.DepthBias); w.f32(r.DepthBiasClamp);
  w.f32(r.SlopeScaledDepthBias); w.u32(r.DepthClipEnable); w.u32(r.MultisampleEnable); w.u32(r.AntialiasedLineEnable);
  w.u32(r.ForcedSampleCount); w.u32(r.ConservativeRaster);
  const D3D12_DEPTH_STENCIL_DESC &z = d.DepthStencilState;
  w.u32(z.DepthEnable); w.u32(z.DepthWriteMask); w.u32(z.DepthFunc); w.u32(z.StencilEnable); w.u8(z.StencilReadMask); w.u8(z.StencilWriteMask);
  for (const D3D12_DEPTH_STENCILOP_DESC *f : { &z.FrontFace, &z.BackFace }) {
    w.u32(f->StencilFailOp); w.u32(f->StencilDepthFailOp); w.u32(f->StencilPassOp); w.u32(f->StencilFunc);
  }
  w.u32(d.InputLayout.NumElements);
  for (UINT i = 0; i < d.InputLayout.NumElements; ++i) {
    const D3D12_INPUT_ELEMENT_DESC &e = d.InputLayout.pInputElementDescs[i];
    w.str(e.SemanticName); w.u32(e.SemanticIndex); w.u32(e.Format); w.u32(e.InputSlot); w.u32(e.AlignedByteOffset);
    w.u32(e.InputSlotClass); w.u32(e.InstanceDataStepRate);
  }
  w.u32(d.IBStripCutValue); w.u32(d.PrimitiveTopologyType); w.u32(d.NumRenderTargets);
  for (int i = 0; i < 8; ++i) w.u32(d.RTVFormats[i]);
  w.u32(d.DSVFormat); w.u32(d.SampleDesc.Count); w.u32(d.SampleDesc.Quality); w.u32(d.NodeMask); w.u32(d.Flags);
}

// A graphics desc rebuilt from a payload; owns the strings/element arrays the desc points to.
struct GfxRecord {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  std::vector<uint8_t> rootSig;
  ShaderRef vs, ps;
  std::vector<std::string> names;
  std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
};

inline bool ReadGfx(Reader &rd, GfxRecord &g) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC &d = g.desc;
  d = {};
  g.rootSig = rd.blob();
  g.vs = rd.ref(); g.ps = rd.ref();
  d.BlendState.AlphaToCoverageEnable = rd.u32(); d.BlendState.IndependentBlendEnable = rd.u32();
  for (int i = 0; i < 8; ++i) {
    D3D12_RENDER_TARGET_BLEND_DESC &t = d.BlendState.RenderTarget[i];
    t.BlendEnable = rd.u32(); t.LogicOpEnable = rd.u32(); t.SrcBlend = (D3D12_BLEND)rd.u32(); t.DestBlend = (D3D12_BLEND)rd.u32();
    t.BlendOp = (D3D12_BLEND_OP)rd.u32(); t.SrcBlendAlpha = (D3D12_BLEND)rd.u32(); t.DestBlendAlpha = (D3D12_BLEND)rd.u32();
    t.BlendOpAlpha = (D3D12_BLEND_OP)rd.u32(); t.LogicOp = (D3D12_LOGIC_OP)rd.u32(); t.RenderTargetWriteMask = rd.u8();
  }
  d.SampleMask = rd.u32();
  D3D12_RASTERIZER_DESC &r = d.RasterizerState;
  r.FillMode = (D3D12_FILL_MODE)rd.u32(); r.CullMode = (D3D12_CULL_MODE)rd.u32(); r.FrontCounterClockwise = rd.u32();
  r.DepthBias = (INT)rd.u32(); r.DepthBiasClamp = rd.f32(); r.SlopeScaledDepthBias = rd.f32(); r.DepthClipEnable = rd.u32();
  r.MultisampleEnable = rd.u32(); r.AntialiasedLineEnable = rd.u32(); r.ForcedSampleCount = rd.u32();
  r.ConservativeRaster = (D3D12_CONSERVATIVE_RASTERIZATION_MODE)rd.u32();
  D3D12_DEPTH_STENCIL_DESC &z = d.DepthStencilState;
  z.DepthEnable = rd.u32(); z.DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)rd.u32(); z.DepthFunc = (D3D12_COMPARISON_FUNC)rd.u32();
  z.StencilEnable = rd.u32(); z.StencilReadMask = rd.u8(); z.StencilWriteMask = rd.u8();
  for (D3D12_DEPTH_STENCILOP_DESC *f : { &z.FrontFace, &z.BackFace }) {
    f->StencilFailOp = (D3D12_STENCIL_OP)rd.u32(); f->StencilDepthFailOp = (D3D12_STENCIL_OP)rd.u32();
    f->StencilPassOp = (D3D12_STENCIL_OP)rd.u32(); f->StencilFunc = (D3D12_COMPARISON_FUNC)rd.u32();
  }
  uint32_t ne = rd.u32();
  if (!rd.ok || ne > 64) return false;
  g.names.resize(ne); g.elems.resize(ne);
  for (uint32_t i = 0; i < ne; ++i) {
    g.names[i] = rd.str();
    D3D12_INPUT_ELEMENT_DESC &e = g.elems[i];
    e.SemanticIndex = rd.u32(); e.Format = (DXGI_FORMAT)rd.u32(); e.InputSlot = rd.u32(); e.AlignedByteOffset = rd.u32();
    e.InputSlotClass = (D3D12_INPUT_CLASSIFICATION)rd.u32(); e.InstanceDataStepRate = rd.u32();
  }
  for (uint32_t i = 0; i < ne; ++i) g.elems[i].SemanticName = g.names[i].c_str();
  d.InputLayout.NumElements = ne; d.InputLayout.pInputElementDescs = ne ? g.elems.data() : nullptr;
  d.IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)rd.u32(); d.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)rd.u32();
  d.NumRenderTargets = rd.u32();
  for (int i = 0; i < 8; ++i) d.RTVFormats[i] = (DXGI_FORMAT)rd.u32();
  d.DSVFormat = (DXGI_FORMAT)rd.u32(); d.SampleDesc.Count = rd.u32(); d.SampleDesc.Quality = rd.u32();
  d.NodeMask = rd.u32(); d.Flags = (D3D12_PIPELINE_STATE_FLAGS)rd.u32();
  return rd.ok;
}

// ---- compute ----
inline void WriteCs(Writer &w, const D3D12_COMPUTE_PIPELINE_STATE_DESC &d, const std::vector<uint8_t> &rs, const ShaderRef &cs) {
  w.u8(KIND_CS);
  w.blob(rs.data(), (uint32_t)rs.size());
  w.ref(cs);
  w.u32(d.NodeMask); w.u32(d.Flags);
}

struct CsRecord {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  std::vector<uint8_t> rootSig;
  ShaderRef cs;
};

inline bool ReadCs(Reader &rd, CsRecord &c) {
  c.desc = {};
  c.rootSig = rd.blob();
  c.cs = rd.ref();
  c.desc.NodeMask = rd.u32(); c.desc.Flags = (D3D12_PIPELINE_STATE_FLAGS)rd.u32();
  return rd.ok;
}

inline void KeyName(uint64_t key, wchar_t out[17]) {
  static const wchar_t *hx = L"0123456789abcdef";
  for (int i = 0; i < 16; ++i) out[i] = hx[(key >> ((15 - i) * 4)) & 15];
  out[16] = 0;
}

// Translated-shader file for a reference, relative to the game folder.
inline void ShaderFile(const ShaderRef &r, wchar_t *out, size_t cch) {
  _snwprintf_s(out, cch, _TRUNCATE, L"shaders\\translated\\%016llx%ls", (unsigned long long)r.orig,
               r.variant == SH_DXIL ? L".dxil.cso" : L".cso");
}

}  // namespace psocache
