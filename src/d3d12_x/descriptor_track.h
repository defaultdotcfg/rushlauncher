#pragma once
// ---------------------------------------------------------------------------
// Diagnostic descriptor tracker: remembers which resource each CPU descriptor handle
// refers to (CreateShaderResourceView / CreateUnorderedAccessView / CopyDescriptors*),
// and which shader-visible heap range each GPU handle belongs to, so that a root
// descriptor table bound by the game can be resolved back to concrete resources.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <d3d12.h>
#include <unordered_map>
#include <vector>

void log_line(const char *format, ...);

struct DescInfo { ID3D12Resource *res; DXGI_FORMAT fmt; UINT dim; UINT kind; };  // kind: 1=SRV 2=UAV 3=CBV
struct DescHeapRec { SIZE_T cpu; UINT64 gpu; UINT num; UINT inc; };

static SRWLOCK g_descLock = SRWLOCK_INIT;
static std::unordered_map<SIZE_T, DescInfo> g_descMap;
static std::vector<DescHeapRec> g_descHeaps;

static inline void DescTrack_Set(SIZE_T cpu, const DescInfo &di) {
  if (!cpu) return;
  AcquireSRWLockExclusive(&g_descLock);
  g_descMap[cpu] = di;
  ReleaseSRWLockExclusive(&g_descLock);
}

static inline bool DescTrack_Get(SIZE_T cpu, DescInfo *out) {
  AcquireSRWLockShared(&g_descLock);
  auto it = g_descMap.find(cpu);
  bool ok = it != g_descMap.end();
  if (ok) *out = it->second;
  ReleaseSRWLockShared(&g_descLock);
  return ok;
}

// GPU VA of the constant buffer each CPU descriptor slot points at (native CBVs and converted Xbox V#s).
static std::unordered_map<SIZE_T, UINT64> g_cbvVa;
static inline void DescTrack_SetCbv(SIZE_T cpu, UINT64 va) {
  if (!cpu) return;
  AcquireSRWLockExclusive(&g_descLock);
  g_cbvVa[cpu] = va;
  ReleaseSRWLockExclusive(&g_descLock);
}
// Constant buffer VA of slot `k` of the table at GPU handle `gpu` (0 if unknown).
static inline UINT64 DescTrack_TableCbv(UINT64 gpu, UINT k) {
  UINT64 va = 0;
  AcquireSRWLockShared(&g_descLock);
  for (auto &h : g_descHeaps)
    if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) {
      auto it = g_cbvVa.find(h.cpu + (SIZE_T)(gpu - h.gpu) + (SIZE_T)k * h.inc);
      if (it != g_cbvVa.end()) va = it->second;
      break;
    }
  ReleaseSRWLockShared(&g_descLock);
  return va;
}
// Resource of SRV/UAV slot `k` of the table at GPU handle `gpu` (kind 1 = SRV, 2 = UAV), or null.
static inline ID3D12Resource *DescTrack_TableRes(UINT64 gpu, UINT k, UINT kind, DescInfo *out) {
  SIZE_T cpu = 0;
  AcquireSRWLockShared(&g_descLock);
  for (auto &h : g_descHeaps)
    if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) { cpu = h.cpu + (SIZE_T)(gpu - h.gpu) + (SIZE_T)k * h.inc; break; }
  ReleaseSRWLockShared(&g_descLock);
  DescInfo di;
  if (!cpu || !DescTrack_Get(cpu, &di) || !di.res || di.kind != kind) return nullptr;
  if (out) *out = di;
  return di.res;
}

static inline void DescTrack_RegisterHeap(SIZE_T cpu, UINT64 gpu, UINT num, UINT inc) {
  AcquireSRWLockExclusive(&g_descLock);
  g_descHeaps.push_back({ cpu, gpu, num, inc });
  ReleaseSRWLockExclusive(&g_descLock);
}

static inline void DescTrack_Copy(SIZE_T dst, SIZE_T src, UINT count, UINT inc) {
  for (UINT i = 0; i < count; ++i) {
    DescInfo di;
    if (DescTrack_Get(src + (SIZE_T)i * inc, &di)) DescTrack_Set(dst + (SIZE_T)i * inc, di);
  }
}

// Returns the first UAV (within `count` slots of the table at `gpu`) whose resource is a 3D texture of size w x w x w, or null.
static inline ID3D12Resource *DescTrack_FindUav3D(UINT64 gpu, UINT count, UINT w, UINT *slotOut) {
  SIZE_T cpuBase = 0; UINT inc = 0;
  AcquireSRWLockShared(&g_descLock);
  for (auto &h : g_descHeaps)
    if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) {
      cpuBase = h.cpu + (SIZE_T)(gpu - h.gpu); inc = h.inc;
      UINT left = (UINT)((h.gpu + (UINT64)h.num * h.inc - gpu) / h.inc); if (count > left) count = left; break;
    }
  ReleaseSRWLockShared(&g_descLock);
  if (!cpuBase) return nullptr;
  for (UINT k = 0; k < count; ++k) {
    DescInfo di;
    if (!DescTrack_Get(cpuBase + (SIZE_T)k * inc, &di) || !di.res || di.kind != 2) continue;
    D3D12_RESOURCE_DESC rd = {}; bool ok = false;
    __try { rd = di.res->GetDesc(); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (ok && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && rd.Width == w && rd.Height == w && rd.DepthOrArraySize == w) { if (slotOut) *slotOut = k; return di.res; }
  }
  return nullptr;
}


// Returns the first SRV/UAV (within `count` slots of the table at `gpu`) that is a 3D texture of exactly w x h x d, or null.
static inline ID3D12Resource *DescTrack_FindTex3D(UINT64 gpu, UINT count, UINT w, UINT h, UINT d, UINT kind, UINT *slotOut) {
  SIZE_T cpuBase = 0; UINT inc = 0;
  AcquireSRWLockShared(&g_descLock);
  for (auto &hh : g_descHeaps)
    if (hh.gpu && gpu >= hh.gpu && gpu < hh.gpu + (UINT64)hh.num * hh.inc) {
      cpuBase = hh.cpu + (SIZE_T)(gpu - hh.gpu); inc = hh.inc;
      UINT left = (UINT)((hh.gpu + (UINT64)hh.num * hh.inc - gpu) / hh.inc); if (count > left) count = left; break;
    }
  ReleaseSRWLockShared(&g_descLock);
  if (!cpuBase) return nullptr;
  for (UINT k = 0; k < count; ++k) {
    DescInfo di;
    if (!DescTrack_Get(cpuBase + (SIZE_T)k * inc, &di) || !di.res || di.kind != kind) continue;
    D3D12_RESOURCE_DESC rd = {}; bool ok = false;
    __try { rd = di.res->GetDesc(); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (ok && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && rd.Width == w && rd.Height == h && rd.DepthOrArraySize == d) { if (slotOut) *slotOut = k; return di.res; }
    if (ok && d == 0 && rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && rd.Width == w) { if (slotOut) *slotOut = k; return di.res; }  // WxDx0 = buffer of width W
  }
  return nullptr;
}

// Logs up to `count` descriptors of the table starting at GPU handle `gpu`.
static inline void DescTrack_LogTable(const char *tag, UINT slot, UINT64 gpu, UINT count) {
  SIZE_T cpuBase = 0; UINT inc = 0;
  AcquireSRWLockShared(&g_descLock);
  for (auto &h : g_descHeaps) {
    if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) {
      cpuBase = h.cpu + (SIZE_T)(gpu - h.gpu); inc = h.inc; break;
    }
  }
  ReleaseSRWLockShared(&g_descLock);
  if (!cpuBase) { log_line("[desc] %s slot=%u gpu=%llX (heap unknown)\r\n", tag, slot, (unsigned long long)gpu); return; }
  char line[1200]; int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "[desc] %s slot=%u gpu=%llX:", tag, slot, (unsigned long long)gpu);
  for (UINT k = 0; k < count && o < 1100; ++k) {
    DescInfo di;
    if (DescTrack_Get(cpuBase + (SIZE_T)k * inc, &di) && di.res) {
      D3D12_RESOURCE_DESC rd = {};
      bool okd = false;
      __try { rd = di.res->GetDesc(); okd = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
      o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " [%u:%s %p %llux%ux%u f%d v%u]", k,
                       di.kind == 1 ? "SRV" : di.kind == 2 ? "UAV" : "CBV", (void *)di.res,
                       okd ? (unsigned long long)rd.Width : 0ULL, okd ? rd.Height : 0,
                       okd ? (unsigned)rd.DepthOrArraySize : 0, okd ? (int)rd.Format : 0, di.dim);
      uint32_t w[2] = {}; __try { memcpy(w, (const void *)(cpuBase + (SIZE_T)k * inc), 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
      o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, "{%08X %08X vf%d}", w[0], w[1], (int)di.fmt);
    } else {
      uint32_t w[4] = {}; bool okr = true;
      __try { memcpy(w, (const void *)(cpuBase + (SIZE_T)k * inc), 16); } __except (EXCEPTION_EXECUTE_HANDLER) { okr = false; }
      if (okr && !(w[0] | w[1] | w[2] | w[3])) o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " [%u:0]", k);
      else if (okr) o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " [%u:raw %08X %08X %08X %08X]", k, w[0], w[1], w[2], w[3]);
      else o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " [%u:-]", k);
    }
  }
  log_line("%s\r\n", line);
}
