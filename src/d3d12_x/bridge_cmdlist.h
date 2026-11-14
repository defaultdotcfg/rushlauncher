#pragma once

// ============================================================================
// BridgeCommandList & Specialized Vtables Implementation
// Native translation and ABI bridge between Durango (Xbox One) CommandLists
// and Desktop D3D12 GraphicsCommandLists.
// ============================================================================

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <utility>
#include <mutex>
#include <intrin.h>
#include <vector>
#include <unordered_map>

// Forward declarations for external translation layer logging/diagnostics
#include "descriptor_track.h"
void log_line(const char* format, ...);
bool IsKnownReturnedResource(void* ptr);
bool IsStagingBuffer(ID3D12Resource* pRes);
bool IsUploadResource(ID3D12Resource* pRes);
ID3D12Resource* LookupRtvResource(SIZE_T ptr);
bool LookupDsvKnown(SIZE_T ptr);
int LookupSigKind(void* sig);
UINT LookupRsExtraParam(void* rs);
void FixRawDescriptors(UINT64 gpu, void* rs, UINT param);
static bool IsAoRes(ID3D12Resource* r);
extern ID3D12Resource* g_bigCopySrc;
extern ID3D12Resource* g_CtrBuf;
void RecordLastRenderedTarget(ID3D12Resource* pRes);

#define BRIDGE_CMDLIST_MAGIC 0x425247434D444C53ULL // "BRGCMDLS"

struct BridgeCommandList
{
    const void* lpVtbl;                     // +0x00: Vtable pointer
    uint8_t pad_08_38[0x38 - 0x08];         // +0x08 .. +0x37: Padding up to offset 0x38
    void* pCurrent;                         // +0x38: Write cursor
    void* pad_40;                           // +0x40
    void* pEndDraw;                         // +0x48: Buffer limit for Draw / Dispatch
    void* pEndState;                        // +0x50: Buffer limit for State / Setup
    // Wrapper fields beyond 0x58
    ID3D12GraphicsCommandList* m_pRealCmdList;
    D3D12_COMMAND_LIST_TYPE m_type;
    volatile LONG m_refCount;
    uint64_t m_magic;
    bool m_isClosed;
    ID3D12CommandAllocator* m_pAllocator;
    ID3D12Resource* m_pActiveRenderTarget;
    void* m_stateMap;                       // ListStateMap* (per-list resource state tracking)
};

static_assert(offsetof(BridgeCommandList, lpVtbl) == 0x00, "BridgeCommandList lpVtbl offset mismatch");
static_assert(offsetof(BridgeCommandList, pCurrent) == 0x38, "BridgeCommandList pCurrent offset mismatch");
static_assert(offsetof(BridgeCommandList, pEndDraw) == 0x48, "BridgeCommandList pEndDraw offset mismatch");
static_assert(offsetof(BridgeCommandList, pEndState) == 0x50, "BridgeCommandList pEndState offset mismatch");

static inline void ListStatesClear(BridgeCommandList* l);
static void BcOnReset(void* list);

static inline ID3D12CommandList* UnwrapCommandList(void* pCmdList)
{
    if (!pCmdList || (uintptr_t)pCmdList < 0x10000) return nullptr;
    __try {
        BridgeCommandList* pBridge = (BridgeCommandList*)pCmdList;
        if (pBridge->m_magic == BRIDGE_CMDLIST_MAGIC && pBridge->m_pRealCmdList)
        {
            return pBridge->m_pRealCmdList;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return (ID3D12CommandList*)pCmdList;
}

// ----------------------------------------------------------------------------
// Diagnostic Stubs for Unmapped Durango CommandList Slots
// ----------------------------------------------------------------------------

static const int kDurangoCmdListSlots = 128;
inline volatile LONG g_CmdListStubHits[kDurangoCmdListSlots] = {};

template <int N>
static UINT64 STDMETHODCALLTYPE DurangoCmdListStub(void *self, UINT64 a1, UINT64 a2, UINT64 a3)
{
    LONG hits = InterlockedIncrement(&g_CmdListStubHits[N]);
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    if (hits <= 3)
    {
        log_line("[d3d12_x] UNMAPPED Durango CommandList slot %d (0x%X) caller=%p (game+0x%llX) self=%p args=%016llX %016llX %016llX (hit %ld)\r\n",
                 N, N * 8, retAddr, (unsigned long long)rva, self, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, hits);
    }
    return 0; // S_OK / void
}

template <int... Is>
static void InitCmdListStubs(void **table, std::integer_sequence<int, Is...>)
{
    ((table[Is] = (void *)&DurangoCmdListStub<Is>), ...);
}

// ----------------------------------------------------------------------------
// BridgeCommandList Forwarding Methods
// ----------------------------------------------------------------------------

static ULONG STDMETHODCALLTYPE BridgeCmdList_AddRef(BridgeCommandList* self);
static ULONG STDMETHODCALLTYPE BridgeCmdList_Release(BridgeCommandList* self);

static HRESULT STDMETHODCALLTYPE BridgeCmdList_QueryInterface(BridgeCommandList* self, REFIID riid, void** ppvObject)
{
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (!self) return E_POINTER;

    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList) ||
        // 1.80 asks for GraphicsCommandList1 when creating its lists (exe+0xAA31A); the Durango
        // layout of our vtable already covers what the game calls on it.
        riid == __uuidof(ID3D12GraphicsCommandList1))
    {
        BridgeCmdList_AddRef(self);
        *ppvObject = self;
        return S_OK;
    }

    log_line("[d3d12_x] BridgeCommandList::QueryInterface rejected unknown IID {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\r\n",
             riid.Data1, riid.Data2, riid.Data3,
             riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
             riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);

    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE BridgeCmdList_AddRef(BridgeCommandList* self)
{
    if (!self) return 0;
    if (self->m_pRealCmdList)
        self->m_pRealCmdList->AddRef();
    return (ULONG)InterlockedIncrement(&self->m_refCount);
}

static ULONG STDMETHODCALLTYPE BridgeCmdList_Release(BridgeCommandList* self)
{
    if (!self) return 0;
    if (self->m_pRealCmdList)
        self->m_pRealCmdList->Release();
    LONG count = InterlockedDecrement(&self->m_refCount);
    if (count <= 0)
    {
        delete self;
        return 0;
    }
    return (ULONG)count;
}

static HRESULT STDMETHODCALLTYPE BridgeCmdList_GetDevice(BridgeCommandList* self, REFIID riid, void** ppvDevice)
{
    if (self && self->m_pRealCmdList)
        return self->m_pRealCmdList->GetDevice(riid, ppvDevice);
    return E_POINTER;
}

static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE BridgeCmdList_GetType(BridgeCommandList* self)
{
    if (self) return self->m_type;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

// Per-interval command statistics (logged by the Present hook).
enum { STAT_DRAW, STAT_DRAWIDX, STAT_DISPATCH, STAT_CLEARRTV, STAT_COPYTEX, STAT_EXECIND, STAT_PSO, STAT_DRAW_RT, STAT_DRAW_NORT, STAT_COUNT };
static volatile LONG g_cmdStats[STAT_COUNT] = {};
static volatile LONG g_cmdTotal[STAT_COUNT] = {};   // never reset (ONRUSH_LOG_FRAME_DRAWS takes per-frame deltas)
#define CMD_STAT(i) (InterlockedIncrement(&g_cmdTotal[i]), InterlockedIncrement(&g_cmdStats[i]))

// Detailed draw logging window (set by the Present hook around a chosen frame).
static volatile LONG g_drawDetail = 0;
static thread_local void* t_lastPso = nullptr;
static thread_local UINT64 t_gtab[16] = {};
static thread_local UINT64 t_ctab[16] = {};
static void FindDescIn(const UINT64* tabs, const char* where);
// ONRUSH_LOG_PSHASH=<hash>: PSOs whose (original) PS has this FNV hash; their first draws dump every
// bound graphics descriptor table (see DbgPsoDraw).
static void* volatile g_dbgPso[8] = {};
static volatile LONG g_dbgPsoN = 0;
static volatile LONG g_pshN = 0, g_pshForce = 0;   // F9 (ONRUSH_RT_KEY): re-arm the PSHASH log for the next 12 matching draws
static thread_local UINT t_rc0[8] = {};
static uint64_t BcPsoHash(void* pso); static uint64_t BcVsHash(void* pso);
static void DbgPsoDraw(const char* kind, UINT cnt, UINT inst) {
    // ONRUSH_LOG_DRAWCNT=n1,n2,...: log VS/PS/depth state of every draw with one of these vertex/index counts
    // (from ONRUSH_PSHASH_FRAME on, 64 lines): finds the depth prepass that pairs with a G-buffer draw.
    { static UINT s_dc[8] = {}; static int s_dcN = -1;
      if (s_dcN < 0) { char b[96] = {}; s_dcN = 0; if (GetEnvironmentVariableA("ONRUSH_LOG_DRAWCNT", b, sizeof(b)) > 0) for (char* q = b; *q && s_dcN < 8; ) { s_dc[s_dcN++] = (UINT)strtoul(q, &q, 10); while (*q == ',') ++q; } }
      if (s_dcN > 0 && t_lastPso) {
        bool m = false; for (int k = 0; k < s_dcN; ++k) m |= s_dc[k] == cnt;
        extern volatile LONG g_rtLogFrame; static long s_fr = -2;
        if (s_fr == -2) { char fb[16] = {}; s_fr = GetEnvironmentVariableA("ONRUSH_PSHASH_FRAME", fb, sizeof(fb)) > 0 ? atol(fb) : -1; }
        static volatile LONG s_dn = 0;
        if (m && (s_fr < 0 || g_rtLogFrame >= s_fr) && InterlockedIncrement(&s_dn) <= 64)
          log_line("[drawcnt] fr=%ld %s cnt=%u inst=%u pso=%p vs=%016llx ps=%016llx rc0=%08X\r\n", (long)g_rtLogFrame, kind, cnt, inst, t_lastPso,
                   (unsigned long long)BcVsHash(t_lastPso), (unsigned long long)BcPsoHash(t_lastPso), t_rc0[0]);
      } }
    if (!g_dbgPsoN || !t_lastPso) return;
    bool hit = false;
    for (LONG k = 0; k < g_dbgPsoN && k < 8; ++k) if (g_dbgPso[k] == t_lastPso) hit = true;
    if (!hit) return;
    // ONRUSH_PSHASH_AFTER=seconds: only log draws after that many seconds (e.g. to reach the EULA).
    { static int s_after = -1; static ULONGLONG s_t0 = 0;
      if (s_after < 0) { char ab[16] = {}; s_after = GetEnvironmentVariableA("ONRUSH_PSHASH_AFTER", ab, sizeof(ab)) > 0 ? atoi(ab) : 0; s_t0 = GetTickCount64(); }
      if (s_after > 0 && GetTickCount64() - s_t0 < (ULONGLONG)s_after * 1000ull) return; }
    { extern volatile LONG g_rtLogFrame; static long s_fr = -2;  // ONRUSH_PSHASH_FRAME=N: only from frame N on
      if (s_fr == -2) { char fb[16] = {}; s_fr = GetEnvironmentVariableA("ONRUSH_PSHASH_FRAME", fb, sizeof(fb)) > 0 ? atol(fb) : -1; }
      if (!g_pshForce && s_fr >= 0 && g_rtLogFrame < s_fr) return; }
    LONG n = InterlockedIncrement(&g_pshN);
    if (n > 12) return;
    log_line("[pshash] %s cnt=%u inst=%u pso=%p vs=%016llx ps=%016llx rc0=%08X (#%ld)\r\n", kind, cnt, inst, t_lastPso, (unsigned long long)BcVsHash(t_lastPso), (unsigned long long)BcPsoHash(t_lastPso), t_rc0[0], n);
    static const bool s_tabOnly = GetEnvironmentVariableA("ONRUSH_LOG_PSTAB_ONLY", nullptr, 0) > 0;
    for (int p = 0; p < 16 && !s_tabOnly; ++p)
        if (t_gtab[p]) { char tg[16]; sprintf_s(tg, "p%d", p); DescTrack_LogTable(tg, (UINT)p, t_gtab[p], 24); }
    { int pp = -1, off = 0;  // ONRUSH_LOG_PSTAB=p:off[,p:off...] -> 12 entries of table p from off
      char lb[128] = {}; if (GetEnvironmentVariableA("ONRUSH_LOG_PSTAB", lb, sizeof(lb)) > 0) for (char* q = lb; q && *q; ) {
        if (sscanf_s(q, "%d:%d", &pp, &off) == 2 && pp >= 0 && pp < 16 && t_gtab[pp]) { char tg[16]; sprintf_s(tg, "ptab%d", pp); DescTrack_LogTable(tg, (UINT)off, t_gtab[pp] + (UINT64)off * 32ull, 12); }
        q = strchr(q, ','); if (q) ++q; } }
}
static thread_local void* t_gfxRs = nullptr; static thread_local void* t_csRs = nullptr;
static volatile LONG g_drawDetailCount = 0;

// Per-draw pipeline statistics (frame 400 window). Created by the Present hook.
static ID3D12QueryHeap *g_psQueryHeap = nullptr;
static ID3D12Resource *g_psReadback = nullptr;
static const UINT kPsMax = 1024;
struct PsDrawInfo { char kind; UINT count; void *pso; void *rt; void *list; unsigned long long vs, ps; UINT rc0; };
static PsDrawInfo g_psInfo[kPsMax];
static volatile LONG g_psNext = 0;

static thread_local D3D12_GPU_VIRTUAL_ADDRESS t_rootCbv[16] = {};
static thread_local D3D12_GPU_VIRTUAL_ADDRESS t_ccbv[16] = {};   // compute root CBVs
// Compute root arguments and descriptor heaps as the game set them (the upscaler records its own passes in the
// middle of the game's command list and puts these back afterwards). Kind: 1 table, 2 CBV, 3 SRV, 4 UAV, 5 constants.
static thread_local UINT8 t_cKind[16] = {};
static thread_local D3D12_GPU_VIRTUAL_ADDRESS t_cVa[16] = {};
static thread_local UINT t_cConst[16][32] = {}; static thread_local UINT t_cConstN[16] = {};   // values and how many were set
static thread_local ID3D12DescriptorHeap* t_heaps[2] = {}; static thread_local UINT t_nHeaps = 0;
static thread_local int t_topo = 0;
struct OmState { void* list; UINT n; BOOL single; unsigned long long h0, h1, h2, ds; };
static thread_local OmState t_om = {};
static thread_local void* t_vpList = nullptr;
static thread_local void* t_scList = nullptr;
static thread_local D3D12_VIEWPORT t_vp0 = {};
static thread_local D3D12_RECT t_sc0 = {};
// F11 (burst capture) also logs every draw of the next frame as [dl] (VS/PS hashes, counts, viewport): find the draw behind an artifact.
static volatile LONG g_drawList = 0;
static void (*g_pfnLogCbvDump)(const D3D12_GPU_VIRTUAL_ADDRESS* cbv) = nullptr;   // set by the bridge: prints the root CBV contents
static void DrawListLog(const char* kind, UINT cnt, UINT inst) {
    // ONRUSH_CBV_PS=<ps hash, hex>: print the root CBV contents of the first draws that use that pixel shader ([cbvdump])
    static unsigned long long s_cbvPs = (unsigned long long)-1; static volatile LONG s_cbvN = 0;
    if (s_cbvPs == (unsigned long long)-1) { char e[40]; s_cbvPs = GetEnvironmentVariableA("ONRUSH_CBV_PS", e, sizeof(e)) > 0 ? _strtoui64(e, nullptr, 16) : 0; }
    if (s_cbvPs && t_lastPso && g_pfnLogCbvDump && (unsigned long long)BcPsoHash(t_lastPso) == s_cbvPs && InterlockedIncrement(&s_cbvN) <= 3) g_pfnLogCbvDump(t_rootCbv);
    static const bool s_panel = GetEnvironmentVariableA("ONRUSH_LOG_PANEL", nullptr, 0) > 0; static volatile LONG s_panelN = 0;
    const bool panelVp = s_panel && t_lastPso && t_vp0.TopLeftX < 1536 && t_vp0.Height <= 768 && ((t_vp0.Width == 768 && t_vp0.Height == 384) || (t_vp0.Width == 384 && t_vp0.Height == 768) || (t_vp0.Width == 768 && t_vp0.Height == 768) || (t_vp0.Width == 384 && t_vp0.Height == 384) || (t_vp0.Width == 1536 && t_vp0.Height == 768)) && InterlockedIncrement(&s_panelN) <= 600;
    if ((!g_drawList && !panelVp) || !t_lastPso) return;
    char b[256];   // formats that start with '%' are not capped by the lean-log limit
    sprintf_s(b, "[dl] %s cnt=%u inst=%u vs=%016llx ps=%016llx pso=%p vp=%.0f,%.0f %.0fx%.0f sc=%ld,%ld-%ld,%ld\r\n", kind, cnt, inst,
              (unsigned long long)BcVsHash(t_lastPso), (unsigned long long)BcPsoHash(t_lastPso), t_lastPso, t_vp0.TopLeftX, t_vp0.TopLeftY, t_vp0.Width, t_vp0.Height,
              t_sc0.left, t_sc0.top, t_sc0.right, t_sc0.bottom);
    log_line("%s", b);
    // ONRUSH_FIND_SRV3D=WxHxD: which draws sample a 3D texture of that size ([srv3d])
    static unsigned s_fw = 1, s_fh = 0, s_fd = 0;
    if (s_fw == 1) { char e[40]; s_fw = 0; if (GetEnvironmentVariableA("ONRUSH_FIND_SRV3D", e, sizeof(e)) > 0) sscanf_s(e, "%ux%ux%u", &s_fw, &s_fh, &s_fd); }
    if (s_fw && g_drawList) for (UINT p = 0; p < 16; ++p) { if (!t_gtab[p]) continue; UINT sl = 0; ID3D12Resource* r = DescTrack_FindTex3D(t_gtab[p], 64, s_fw, s_fh, s_fd, 1, &sl);
        if (r) { log_line("[srv3d] ps=%016llx vs=%016llx param=%u slot=%u res=%p cnt=%u inst=%u\r\n", (unsigned long long)BcPsoHash(t_lastPso), (unsigned long long)BcVsHash(t_lastPso), p, sl, (void*)r, cnt, inst); break; } }
}
static thread_local D3D12_INDEX_BUFFER_VIEW t_ib = {};
static thread_local D3D12_VERTEX_BUFFER_VIEW t_vb[4] = {};
static void (*g_pfnLogGeom)(UINT drawIdx, const D3D12_INDEX_BUFFER_VIEW* ib, const D3D12_VERTEX_BUFFER_VIEW* vb) = nullptr;
static void (*g_pfnLogRootCbv)(UINT drawIdx, void* pso, const D3D12_GPU_VIRTUAL_ADDRESS* cbv) = nullptr;
static ID3D12QueryHeap* g_tsHeap = nullptr; static ID3D12Resource* g_tsReadback = nullptr; static volatile LONG g_tsNext = 0;
struct TsInfo { void* pso; UINT gx, gy, gz; }; static TsInfo g_tsInfo[1024];
static uint64_t BcPsoHash(void* pso); static uint64_t BcVsHash(void* pso);
struct PsQueryScope {
    ID3D12GraphicsCommandList *cl = nullptr; LONG idx = -1;
    PsQueryScope(BridgeCommandList *self, char kind, UINT count) {
        if (!g_drawDetail || !g_psQueryHeap || !self || !self->m_pRealCmdList || self->m_type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        // ONRUSH_PSQ_VS=h1,h2,...: only query draws whose VS (or PS) hash is listed (debug: does a draw produce pixels?)
        static char s_vsf[16384] = {}; static int s_vsfi = 0;
        if (!s_vsfi) s_vsfi = GetEnvironmentVariableA("ONRUSH_PSQ_VS", s_vsf, sizeof(s_vsf)) > 0 ? 1 : 2;
        const unsigned long long vsh = BcVsHash(t_lastPso), psh = BcPsoHash(t_lastPso);
        if (s_vsfi == 1) { char hb[20], hp[20]; sprintf_s(hb, "%016llx", vsh); sprintf_s(hp, "%016llx", psh); if (!strstr(s_vsf, hb) && !strstr(s_vsf, hp)) return; }
        LONG i = InterlockedIncrement(&g_psNext) - 1;
        if (i >= (LONG)kPsMax) return;
        idx = i; cl = self->m_pRealCmdList;
        g_psInfo[i] = { kind, count, t_lastPso, self->m_pActiveRenderTarget, self, vsh, psh, t_rc0[0] };
        if (g_pfnLogRootCbv && i < 40) g_pfnLogRootCbv((UINT)i, t_lastPso, t_rootCbv);
        if (g_pfnLogGeom && (i < 40 || kind == 'X') && i < 400) g_pfnLogGeom((UINT)i, &t_ib, t_vb);
        if (g_drawDetail && kind == 'X' && i < 1024) log_line("[om] X#%ld omList=%p thisList=%p n=%u single=%d h0=%llX h1=%llX h2=%llX ds=%llX dsKnown=%d rtvKnown=%d,%d,%d\r\n", (long)i, t_om.list, (void*)self, t_om.n, (int)t_om.single, t_om.h0, t_om.h1, t_om.h2, t_om.ds, (int)LookupDsvKnown((SIZE_T)t_om.ds), (int)(LookupRtvResource((SIZE_T)t_om.h0) != nullptr), (int)(LookupRtvResource((SIZE_T)t_om.h1) != nullptr), (int)(LookupRtvResource((SIZE_T)t_om.h2) != nullptr));
        if (g_drawDetail && kind == 'X' && i < 1024) log_line("[topo2] X#%ld list=%p vpInThisList=%d scInThisList=%d rt=%p\r\n", (long)i, (void*)self, (int)(t_vpList == (void*)self), (int)(t_scList == (void*)self), self->m_pActiveRenderTarget);
        if (g_drawDetail && kind == 'X' && i < 1024) log_line("[topo] X#%ld topo=%d maxc=%u vp=(%.0f,%.0f %.0fx%.0f z=%.2f..%.2f) sc=(%ld,%ld,%ld,%ld)\r\n", (long)i, t_topo, count, t_vp0.TopLeftX, t_vp0.TopLeftY, t_vp0.Width, t_vp0.Height, t_vp0.MinDepth, t_vp0.MaxDepth, t_sc0.left, t_sc0.top, t_sc0.right, t_sc0.bottom);
        if (g_drawDetail && i < 40) log_line("[rc] draw#%ld pso=%p rc0=%08X %08X %08X %08X\r\n", (long)i, t_lastPso, t_rc0[0], t_rc0[1], t_rc0[2], t_rc0[3]);
        cl->BeginQuery(g_psQueryHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, (UINT)i);
    }
    ~PsQueryScope() {
        if (idx < 0) return;
        cl->EndQuery(g_psQueryHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, (UINT)idx);
        cl->ResolveQueryData(g_psQueryHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, (UINT)idx, 1, g_psReadback, (UINT64)idx * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    }
};

// Draws per render target (diagnostic): up to 24 distinct RTs per interval.
// Each entry holds a reference: the stats print (every 240 frames) calls GetDesc on it, and
// the game frees/recreates render targets when a race loads. GetDesc on a freed resource
// faulted inside D3D12Core while it held a lock -> the next D3D12 call froze the game.
// The print takes every entry out of the table and releases it.
struct RtDrawStat { ID3D12Resource* volatile res; volatile LONG draws; };
static RtDrawStat g_rtDrawStats[24];
static inline void CountRtDraw(ID3D12Resource* rt)
{
    if (!rt) return;
    for (int i = 0; i < 24; ++i) {
        if (g_rtDrawStats[i].res == rt) { InterlockedIncrement(&g_rtDrawStats[i].draws); return; }
        if (!g_rtDrawStats[i].res) {
            rt->AddRef();
            if (InterlockedCompareExchangePointer((PVOID*)&g_rtDrawStats[i].res, rt, nullptr) == nullptr) {
                InterlockedIncrement(&g_rtDrawStats[i].draws); return;
            }
            rt->Release();
            if (g_rtDrawStats[i].res == rt) { InterlockedIncrement(&g_rtDrawStats[i].draws); return; }
        }
    }
}

static HRESULT STDMETHODCALLTYPE BridgeCmdList_Close(BridgeCommandList* self)
{
    if (!self || !self->m_pRealCmdList) return E_POINTER;
    HRESULT hr = self->m_pRealCmdList->Close();
    if (SUCCEEDED(hr)) {
        self->m_isClosed = true;
    }
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    static volatile LONG s_closeLog = 0;
    if (FAILED(hr) || InterlockedIncrement(&s_closeLog) <= 60) {
        log_line("[d3d12_x] BridgeCmdList_Close: self=%p, real=%p, type=%d -> hr=0x%08lX (caller=game+0x%llX)\r\n",
                 self, self->m_pRealCmdList, (int)self->m_type, (unsigned long)hr, (unsigned long long)rva);
    }
    if (FAILED(hr)) {
        FlushD3D12InfoQueue("BridgeCmdList_Close_Failed");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE BridgeCmdList_Reset(
    BridgeCommandList* self,
    ID3D12CommandAllocator* pAllocator,
    ID3D12PipelineState* pInitialState)
{
    if (!self || !self->m_pRealCmdList) return E_POINTER;
    HRESULT hr = self->m_pRealCmdList->Reset(pAllocator, pInitialState);
    if (SUCCEEDED(hr)) {
        self->m_isClosed = false;
        self->m_pAllocator = pAllocator;
    }
    ListStatesClear(self);
    BcOnReset(self);
    self->pCurrent = (void*)(uintptr_t)1;
    self->pEndDraw = nullptr;
    self->pEndState = nullptr;
    self->m_pActiveRenderTarget = nullptr;
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    static volatile LONG s_resetLog = 0;
    if (FAILED(hr) || InterlockedIncrement(&s_resetLog) <= 60) {
        log_line("[d3d12_x] BridgeCmdList_Reset: self=%p, real=%p, type=%d, alloc=%p, pso=%p -> hr=0x%08lX (caller=game+0x%llX)\r\n",
                 self, self->m_pRealCmdList, (int)self->m_type, pAllocator, pInitialState, (unsigned long)hr, (unsigned long long)rva);
    }
    return hr;
}

static void STDMETHODCALLTYPE BridgeCmdList_ClearState(BridgeCommandList* self, ID3D12PipelineState* pPipelineState)
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->ClearState(pPipelineState);
}

// ONRUSH_LOG_RT_W=W: log each PSO that draws into a W-wide render target (once per PSO+RT) as [rtdraw].
static inline bool SafeGetResourceDesc(ID3D12Resource* pRes, D3D12_RESOURCE_DESC* pDesc);
static uint64_t BcPsoHash(void* pso);
static uint64_t BcVsHash(void* pso);
// ONRUSH_SKIP_DRAW_PS=h1,h2,...: drop draws whose PS (or VS) hash is listed (debug: find who draws what).
// PSOs whose PS has no translation (translator refused it): the Xbox bytecode drew garbage on PC
// (white/black rectangles), so their draws are dropped. ONRUSH_DRAW_UNTRANSLATED=1 = old behaviour.
static std::unordered_set<void*> g_untrPsPso; static SRWLOCK g_untrPsLk = SRWLOCK_INIT;
static void MarkPsoUntranslatedPs(void* pso) {
    static const bool s_keep = GetEnvironmentVariableA("ONRUSH_DRAW_UNTRANSLATED", nullptr, 0) > 0;
    if (s_keep || !pso) return;
    AcquireSRWLockExclusive(&g_untrPsLk); g_untrPsPso.insert(pso); ReleaseSRWLockExclusive(&g_untrPsLk);
    log_line("[xlate] PSO %p: PS not translated -> its draws are skipped\r\n", pso);
}
static bool SkipDrawByShader() {
    if (!g_untrPsPso.empty() && t_lastPso) { AcquireSRWLockShared(&g_untrPsLk); bool u = g_untrPsPso.count(t_lastPso) != 0; ReleaseSRWLockShared(&g_untrPsLk); if (u) return true; }
    static char s_list[16384] = {}; static int s_init = 0;
    if (!s_init) { s_init = GetEnvironmentVariableA("ONRUSH_SKIP_DRAW_PS", s_list, sizeof(s_list)) > 0 ? 1 : 2; }
    if (s_init != 1 || !t_lastPso) return false;
    static std::unordered_map<void*, bool> s_dec; static SRWLOCK s_lk = SRWLOCK_INIT;
    AcquireSRWLockShared(&s_lk); auto it = s_dec.find(t_lastPso); bool known = it != s_dec.end(), skip = known && it->second; ReleaseSRWLockShared(&s_lk);
    if (known) return skip;
    char a[20], b[20]; sprintf_s(a, "%016llx", (unsigned long long)BcPsoHash(t_lastPso)); sprintf_s(b, "%016llx", (unsigned long long)BcVsHash(t_lastPso));
    skip = strstr(s_list, a) || strstr(s_list, b);
    AcquireSRWLockExclusive(&s_lk); s_dec[t_lastPso] = skip; ReleaseSRWLockExclusive(&s_lk);
    if (skip) log_line("[skipdraw] pso=%p ps=%s vs=%s skipped\r\n", t_lastPso, a, b);
    return skip;
}
// ONRUSH_LOG_RT_W=1 logs every RT >= 256 wide. ONRUSH_LOG_RT_AFTER=N: only from frame N on.
extern volatile LONG g_rtLogFrame;
static void LogDrawOnRtWidth(BridgeCommandList* self, UINT count, UINT inst) {
    static int s_w = -2, s_after = 0;
    if (s_w == -2) { char b[16] = {}; s_w = GetEnvironmentVariableA("ONRUSH_LOG_RT_W", b, sizeof(b)) > 0 ? atoi(b) : -1;
                     char a2[16] = {}; s_after = GetEnvironmentVariableA("ONRUSH_LOG_RT_AFTER", a2, sizeof(a2)) > 0 ? atoi(a2) : 0; }
    if (s_w <= 0 || !self || !self->m_pActiveRenderTarget || g_rtLogFrame < s_after) return;
    D3D12_RESOURCE_DESC rd = {};
    if (!SafeGetResourceDesc(self->m_pActiveRenderTarget, &rd)) return;
    if (s_w == 1 ? rd.Width < 256 : (int)rd.Width != s_w) return;
    static std::set<std::pair<void*, void*>> s_seen; static SRWLOCK s_lk = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&s_lk); bool nw = s_seen.size() < 4000 && s_seen.insert({ (void*)((UINT64)t_lastPso ^ ((UINT64)(UINT)t_vp0.TopLeftX << 48) ^ ((UINT64)(UINT)t_vp0.Width << 32)), (void*)self->m_pActiveRenderTarget }).second; ReleaseSRWLockExclusive(&s_lk);
    if (nw) log_line("[rtdraw] f=%ld RT=%p %llux%u fmt=%d pso=%p ps=%016llx vs=%016llx count=%u inst=%u vp=(%.0f,%.0f %.0fx%.0f)\r\n", (long)g_rtLogFrame, (void*)self->m_pActiveRenderTarget, (unsigned long long)rd.Width, rd.Height, (int)rd.Format, t_lastPso,
                     (unsigned long long)BcPsoHash(t_lastPso), (unsigned long long)BcVsHash(t_lastPso), count, inst, t_vp0.TopLeftX, t_vp0.TopLeftY, t_vp0.Width, t_vp0.Height);
}

static LONG BcBegin(BridgeCommandList* self, int kind, UINT a, UINT b, UINT c);
static void BcEnd(BridgeCommandList* self, LONG slot);

static void STDMETHODCALLTYPE BridgeCmdList_DrawInstanced(
    BridgeCommandList* self,
    UINT VertexCountPerInstance,
    UINT InstanceCount,
    UINT StartVertexLocation,
    UINT StartInstanceLocation)
{
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    DWORD tid = GetCurrentThreadId();

    static volatile LONG s_drawLog = 0;
    if (InterlockedIncrement(&s_drawLog) <= 100) {
        log_line("[d3d12_x] BridgeCmdList_DrawInstanced: self=%p, real=%p, VertexCount=%u, InstanceCount=%u, StartVertex=%u, StartInstance=%u, tid=%u, caller=%p (game+0x%llX)\r\n",
                 self, self ? self->m_pRealCmdList : nullptr,
                 VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation,
                 tid, retAddr, (unsigned long long)rva);
    }

    CMD_STAT(STAT_DRAW);
    DrawListLog("draw", VertexCountPerInstance, InstanceCount);
    DbgPsoDraw("draw", VertexCountPerInstance, InstanceCount);
    { static int s_p = -2, s_slot = 0, s_cnt = 0; static volatile LONG s_n = 0;
      if (s_p == -2) { char sb[48] = {}; s_p = -1; if (GetEnvironmentVariableA("ONRUSH_LOG_SLOT", sb, sizeof(sb)) > 0) sscanf_s(sb, "%d:%d:%d", &s_p, &s_slot, &s_cnt); }
      if (s_p >= 0 && s_p < 16 && (int)VertexCountPerInstance == s_cnt && t_gtab[s_p] && InterlockedIncrement(&s_n) <= 20) {
        log_line("[slotlog] draw vtx=%u pso=%p table[%d]=%llX\n", VertexCountPerInstance, t_lastPso, s_p, (unsigned long long)t_gtab[s_p]);
        DescTrack_LogTable("slot", (UINT)s_slot, t_gtab[s_p] + (UINT64)s_slot * 32ull, 2);
        DescTrack_LogTable("tex0", 0, t_gtab[s_p], 6); } }
    if (g_drawDetail && InterlockedIncrement(&g_drawDetailCount) <= 400)
        log_line("[draw] DrawInstanced vtx=%u inst=%u RT=%p pso=%p tid=%u caller=game+0x%llX\r\n",
                 VertexCountPerInstance, InstanceCount, self ? self->m_pActiveRenderTarget : nullptr, t_lastPso, tid, (unsigned long long)rva);
    CMD_STAT((self && self->m_pActiveRenderTarget) ? STAT_DRAW_RT : STAT_DRAW_NORT);
    if (self) CountRtDraw(self->m_pActiveRenderTarget);
    LogDrawOnRtWidth(self, VertexCountPerInstance, InstanceCount);
    if (SkipDrawByShader()) return;
    if (self && self->m_pRealCmdList)
    {
        if (self->m_pActiveRenderTarget) {
            RecordLastRenderedTarget(self->m_pActiveRenderTarget);
        }
        PsQueryScope psq(self, 'D', VertexCountPerInstance);
        LONG bc = BcBegin(self, 'd', VertexCountPerInstance, InstanceCount, StartVertexLocation);
        self->m_pRealCmdList->DrawInstanced(
            VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        BcEnd(self, bc);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_DrawIndexedInstanced(
    BridgeCommandList* self,
    UINT IndexCountPerInstance,
    UINT InstanceCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation,
    UINT StartInstanceLocation)
{
    CMD_STAT(STAT_DRAWIDX);
    DrawListLog("drawIdx", IndexCountPerInstance, InstanceCount);
    DbgPsoDraw("drawIdx", IndexCountPerInstance, InstanceCount);
    FindDescIn(t_gtab, "draw");
    { static int s_p = -2, s_slot = 0, s_cnt = 0; static volatile LONG s_n = 0;
      if (s_p == -2) { char sb[48] = {}; s_p = -1; if (GetEnvironmentVariableA("ONRUSH_LOG_SLOT", sb, sizeof(sb)) > 0) sscanf_s(sb, "%d:%d:%d", &s_p, &s_slot, &s_cnt); }
      if (s_p >= 0 && s_p < 16 && (int)IndexCountPerInstance == s_cnt && t_gtab[s_p] && InterlockedIncrement(&s_n) <= 20) {
        log_line("[slotlog] draw idx=%u pso=%p table[%d]=%llX\n", IndexCountPerInstance, t_lastPso, s_p, (unsigned long long)t_gtab[s_p]);
        DescTrack_LogTable("slot", (UINT)s_slot, t_gtab[s_p] + (UINT64)s_slot * 32ull, 2);
        DescTrack_LogTable("tex0", 0, t_gtab[s_p], 6); } }
    if (g_drawDetail && InterlockedIncrement(&g_drawDetailCount) <= 400)
        log_line("[draw] DrawIndexed idx=%u inst=%u RT=%p pso=%p\r\n",
                 IndexCountPerInstance, InstanceCount, self ? self->m_pActiveRenderTarget : nullptr, t_lastPso);
    CMD_STAT((self && self->m_pActiveRenderTarget) ? STAT_DRAW_RT : STAT_DRAW_NORT);
    if (self) CountRtDraw(self->m_pActiveRenderTarget);
    LogDrawOnRtWidth(self, IndexCountPerInstance, InstanceCount);
    if (SkipDrawByShader()) return;
    if (self && self->m_pRealCmdList)
    {
        if (self->m_pActiveRenderTarget) {
            RecordLastRenderedTarget(self->m_pActiveRenderTarget);
        }
        PsQueryScope psq(self, 'I', IndexCountPerInstance);
        LONG bc = BcBegin(self, 'i', IndexCountPerInstance, InstanceCount, StartIndexLocation);
        self->m_pRealCmdList->DrawIndexedInstanced(
            IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        BcEnd(self, bc);
    }
}


// ONRUSH_FIND_UAV=WxH:fmt -> at each Dispatch, look through the bound compute tables (first 96 slots) for a UAV on a
// texture of that size/format and log the PSO + param/slot (to find which compute pass writes a given render target).
static void LogUavBuffersAtDispatch();
static void FindUavAtDispatch() { FindDescIn(t_ctab, "cs"); LogUavBuffersAtDispatch(); }
// ONRUSH_LOG_UAVBUF=1 -> first dispatch of each PSO: log every buffer UAV in the bound compute tables (first 96 slots).
static void LogUavBuffersAtDispatch() {
    static const bool s_on = GetEnvironmentVariableA("ONRUSH_LOG_UAVBUF", nullptr, 0) > 0;
    if (!s_on) return;
    { static void* s_seen[1024]; static volatile LONG s_ns = 0; LONG n_ = s_ns; for (LONG q = 0; q < n_ && q < 1024; ++q) if (s_seen[q] == t_lastPso) return; LONG id_ = InterlockedIncrement(&s_ns) - 1; if (id_ < 1024) s_seen[id_] = t_lastPso; else return; }
    for (int p = 0; p < 16; ++p) {
        UINT64 gpu = t_ctab[p]; if (!gpu) continue;
        SIZE_T cpuBase = 0; UINT inc = 0, left = 0;
        AcquireSRWLockShared(&g_descLock);
        for (auto &h : g_descHeaps) if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) { cpuBase = h.cpu + (SIZE_T)(gpu - h.gpu); inc = h.inc; left = (UINT)((h.gpu + (UINT64)h.num * h.inc - gpu) / h.inc); break; }
        ReleaseSRWLockShared(&g_descLock);
        if (!cpuBase) continue;
        for (UINT k = 0; k < left && k < 96; ++k) {
            DescInfo di; if (!DescTrack_Get(cpuBase + (SIZE_T)k * inc, &di) || !di.res) continue;
            D3D12_RESOURCE_DESC rd = {}; bool ok = false; __try { rd = di.res->GetDesc(); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (ok && rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                log_line("[uavbuf] pso=%p param=%d slot=%u %s res=%p w=%llu\r\n", t_lastPso, p, k, di.kind == 2 ? "UAV" : di.kind == 1 ? "SRV" : "CBV", (void*)di.res, (unsigned long long)rd.Width);
        }
    }
}
static void FindDescIn(const UINT64* tabs, const char* where) {
    static int s_w = -2, s_h = 0, s_f = 0, s_k = 2; static volatile LONG s_n = 0;
    if (s_w == -2) { char sb[48] = {}; s_w = -1; if (GetEnvironmentVariableA("ONRUSH_FIND_UAV", sb, sizeof(sb)) > 0) sscanf_s(sb, "%dx%d:%d:%d", &s_w, &s_h, &s_f, &s_k); }
    if (s_w < 0) return;
    { static void* s_seen[512]; static volatile LONG s_ns = 0; LONG n_ = s_ns; for (LONG q = 0; q < n_ && q < 512; ++q) if (s_seen[q] == t_lastPso) return; LONG id_ = InterlockedIncrement(&s_ns) - 1; if (id_ < 512) s_seen[id_] = t_lastPso; else return; }
    for (int p = 0; p < 16; ++p) {
        UINT64 gpu = tabs[p]; if (!gpu) continue;
        SIZE_T cpuBase = 0; UINT inc = 0, left = 0;
        AcquireSRWLockShared(&g_descLock);
        for (auto &h : g_descHeaps) if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) { cpuBase = h.cpu + (SIZE_T)(gpu - h.gpu); inc = h.inc; left = (UINT)((h.gpu + (UINT64)h.num * h.inc - gpu) / h.inc); break; }
        ReleaseSRWLockShared(&g_descLock);
        if (!cpuBase) continue;
        for (UINT k = 0; k < left && k < 96; ++k) {
            DescInfo di; if (!DescTrack_Get(cpuBase + (SIZE_T)k * inc, &di) || (int)di.kind != s_k || !di.res) continue;
            D3D12_RESOURCE_DESC rd = {}; bool ok = false; __try { rd = di.res->GetDesc(); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (ok && (int)rd.Width == s_w && (int)rd.Height == s_h && (s_f == 0 || (int)rd.Format == s_f))
                log_line("[uavfind] %s pso=%p param=%d slot=%u res=%p\n", where, t_lastPso, p, k, (void*)di.res);
        }
    }
}
// ---- GPU breadcrumbs (ONRUSH_BREADCRUMBS=1) ----
// Every Dispatch / ExecuteIndirect is bracketed by WriteBufferImmediate markers into a CPU-visible buffer:
// begin writes the op id, end writes id|0x80000000. When the hang watchdog fires, ops whose slot still holds
// the bare id were started by the GPU but never finished -> that is the shader that hangs.
struct BcInfo { LONG id; void* pso; int kind; int listType; UINT a, b, c; void* list; };
static const UINT kBcSlots = 65536;
// Execution order: each ExecuteCommandLists records (queue, list, first op id, last op id) so the dump can
// walk ops in the order the GPU runs them and name the first one that never finished.
struct BcExec { LONG seq; void* queue; void* list; LONG first, last; };
static const UINT kBcExecSlots = 8192;
static BcExec g_bcExec[kBcExecSlots]; static volatile LONG g_bcExecNext = 0;
struct BcListFirst { void* list; volatile LONG first; };
static const UINT kBcListSlots = 4096;
static BcListFirst g_bcListFirst[kBcListSlots];
static BcListFirst* BcListEntry(void* list, bool create) {
    UINT h = (UINT)(((uintptr_t)list >> 4) * 2654435761u) % kBcListSlots;
    for (UINT n = 0; n < kBcListSlots; ++n, h = (h + 1) % kBcListSlots) {
        BcListFirst& e = g_bcListFirst[h];
        if (e.list == list) return &e;
        if (!e.list) {
            if (!create) return nullptr;
            if (InterlockedCompareExchangePointer((PVOID*)&e.list, list, nullptr) == nullptr || e.list == list) return &e;
        }
    }
    return nullptr;
}
static ID3D12Resource* g_bcBuf = nullptr; static volatile UINT* g_bcMem = nullptr;
static BcInfo g_bcInfo[kBcSlots]; static volatile LONG g_bcNext = 0;
static ID3D12Resource* g_bcArgs = nullptr; static volatile UINT* g_bcArgsMem = nullptr;  // ExecuteIndirect argument snapshots (64 B/op)
struct BcRes { ID3D12Resource* arg; UINT64 aoff; ID3D12Resource* cnt; UINT64 coff; };
static BcRes g_bcRes[kBcSlots];
static volatile bool g_bcArgDumpWindow = false;   // true while recording the ONRUSH_BC_ARGDUMP_FRAME frame (copies are logged too)
static bool BreadcrumbsOn() {
    static int s_on = -1;
    if (s_on < 0) s_on = GetEnvironmentVariableA("ONRUSH_BREADCRUMBS", nullptr, 0) > 0 ? 1 : 0;
    if (s_on == 1 && !g_bcBuf && g_pMainDevice && g_TrueCreateCommittedResource) {
        static SRWLOCK s_lk = SRWLOCK_INIT; AcquireSRWLockExclusive(&s_lk);
        if (!g_bcBuf) {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_CUSTOM; hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kBcSlots * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* r = nullptr;
            HRESULT hr = g_TrueCreateCommittedResource(g_pMainDevice, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&r);
            void* m = nullptr;
            if (SUCCEEDED(hr) && r && SUCCEEDED(r->Map(0, nullptr, &m)) && m) { memset(m, 0, kBcSlots * 4); g_bcMem = (volatile UINT*)m; g_bcBuf = r; }
            log_line("[bc] breadcrumb buffer hr=0x%08lX res=%p mem=%p\r\n", (unsigned long)hr, (void*)r, m);
            if (!g_bcBuf) s_on = 0;
        }
        ReleaseSRWLockExclusive(&s_lk);
    }
    return s_on == 1 && g_bcBuf;
}
// Returns the slot (or -1) and writes the begin marker.
static LONG BcBegin(BridgeCommandList* self, int kind, UINT a, UINT b, UINT c) {
    if (!self || !self->m_pRealCmdList || !BreadcrumbsOn()) return -1;
    ID3D12GraphicsCommandList2* l2 = nullptr;
    if (FAILED(self->m_pRealCmdList->QueryInterface(__uuidof(ID3D12GraphicsCommandList2), (void**)&l2)) || !l2) return -1;
    LONG id = InterlockedIncrement(&g_bcNext) & 0x7FFFFFFF;
    LONG slot = id % kBcSlots;
    g_bcInfo[slot] = { id, t_lastPso, kind, (int)self->m_type, a, b, c, (void*)self };
    if (BcListFirst* le = BcListEntry(self, true)) InterlockedCompareExchange(&le->first, id, 0);
    g_bcMem[slot] = 0;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p = { g_bcBuf->GetGPUVirtualAddress() + (UINT64)slot * 4, (UINT)id };
    D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
    l2->WriteBufferImmediate(1, &p, &mode);
    l2->Release();
    return slot;
}
static void BcEnd(BridgeCommandList* self, LONG slot) {
    if (slot < 0) return;
    ID3D12GraphicsCommandList2* l2 = nullptr;
    if (FAILED(self->m_pRealCmdList->QueryInterface(__uuidof(ID3D12GraphicsCommandList2), (void**)&l2)) || !l2) return;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p = { g_bcBuf->GetGPUVirtualAddress() + (UINT64)slot * 4, (UINT)g_bcInfo[slot].id | 0x80000000u };
    D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    l2->WriteBufferImmediate(1, &p, &mode);
    l2->Release();
}
static void BcOnReset(void* list) {
    if (!g_bcBuf) return;
    if (BcListFirst* le = BcListEntry(list, false)) InterlockedExchange(&le->first, 0);
}
// Called at ExecuteCommandLists for each bridge list, in submission order.
static void BcRecordExec(void* queue, BridgeCommandList* list) {
    if (!g_bcBuf || !list) return;
    BcListFirst* le = BcListEntry(list, false);
    LONG first = le ? InterlockedExchange(&le->first, 0) : 0;
    if (!first) return;   // no instrumented ops in this list
    LONG seq = InterlockedIncrement(&g_bcExecNext);
    g_bcExec[seq % kBcExecSlots] = { seq, queue, list, first, g_bcNext & 0x7FFFFFFF };
}
static uint64_t BcPsoHash(void* pso) {
    uint64_t h = 0;
    if (!TryAcquireSRWLockShared(&g_PsoShaderLock)) return 0xDEADull;  // hang dump must never block (loading may hold the lock)
    auto it = g_PsoShaderMap.find(pso);
    if (it != g_PsoShaderMap.end()) {
        const auto& v = !it->second.csBytecode.empty() ? it->second.csBytecode : it->second.psBytecode;
        if (!v.empty()) h = Fnv1a64Fwd(v.data(), v.size());
    }
    ReleaseSRWLockShared(&g_PsoShaderLock);
    return h;
}
static uint64_t BcVsHash(void* pso) {
    uint64_t h = 0;
    if (!TryAcquireSRWLockShared(&g_PsoShaderLock)) return 0xDEADull;
    auto it = g_PsoShaderMap.find(pso);
    if (it != g_PsoShaderMap.end() && !it->second.vsBytecode.empty()) h = Fnv1a64Fwd(it->second.vsBytecode.data(), it->second.vsBytecode.size());
    ReleaseSRWLockShared(&g_PsoShaderLock);
    return h;
}
static void DumpBreadcrumbs() {
    log_line("[bc] dump start buf=%p next=%ld\r\n", (void*)g_bcBuf, (long)g_bcNext);
    if (!g_bcBuf) return;
    LONG last = g_bcNext & 0x7FFFFFFF;
    LONG lastDone[4] = { 0, 0, 0, 0 }; int inflight = 0;
    LONG from = last > (LONG)kBcSlots - 16 ? last - (LONG)kBcSlots + 16 : 1;
    for (LONG id = from; id <= last; ++id) {
        LONG slot = id % kBcSlots; const BcInfo& in = g_bcInfo[slot];
        if (in.id != id) continue;
        UINT v = g_bcMem[slot];
        if (v == ((UINT)id | 0x80000000u)) { if (in.listType >= 0 && in.listType < 4 && id > lastDone[in.listType]) lastDone[in.listType] = id; }
        else if (v == (UINT)id && inflight < 32) {
            ++inflight;
            log_line("[bc] IN-FLIGHT op %ld list type=%d kind=%c pso=%p shader=%016llx vs=%016llx args=%u,%u,%u\r\n", id, in.listType, in.kind, in.pso,
                     (unsigned long long)BcPsoHash(in.pso), (unsigned long long)BcVsHash(in.pso), in.a, in.b, in.c);
            if (in.kind == 'X' && g_bcArgsMem) {
                const volatile UINT* a = g_bcArgsMem + (size_t)slot * 16;
                log_line("[bc]    arg bytes: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\r\n",
                         a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
            }
        }
    }
    log_line("[bc] last op id=%ld | last completed: direct=%ld compute=%ld copy=%ld | in-flight=%d\r\n", last, lastDone[0], lastDone[2], lastDone[3], inflight);
    for (int t : { 0, 2 }) {   // first op after the last completed one on each list type = next to run
        for (LONG id = lastDone[t] + 1; id <= last && id < lastDone[t] + 200000; ++id) {
            LONG slot = id % kBcSlots; const BcInfo& in = g_bcInfo[slot];
            if (in.id == id && in.listType == t) {
                log_line("[bc] next pending on list type=%d: op %ld kind=%c pso=%p shader=%016llx vs=%016llx args=%u,%u,%u marker=%08X\r\n", t, id, in.kind, in.pso,
                         (unsigned long long)BcPsoHash(in.pso), (unsigned long long)BcVsHash(in.pso), in.a, in.b, in.c, g_bcMem[slot]);
                break;
            }
        }
    }
    // Execution order walk: per queue, the first submitted op that has no end marker.
    LONG execLast = g_bcExecNext;
    LONG execFrom = execLast > (LONG)kBcExecSlots - 64 ? execLast - (LONG)kBcExecSlots + 64 : 1;
    void* queues[8] = {}; int nq = 0;
    for (LONG s = execFrom; s <= execLast; ++s) { const BcExec& e = g_bcExec[s % kBcExecSlots]; if (e.seq != s) continue;
        bool f = false; for (int q = 0; q < nq; ++q) if (queues[q] == e.queue) f = true; if (!f && nq < 8) queues[nq++] = e.queue; }
    log_line("[bc] exec ring: seq %ld..%ld, %d queue(s)\r\n", (long)execFrom, (long)execLast, nq);
    LONG minId = last > (LONG)kBcSlots - 16 ? last - (LONG)kBcSlots + 16 : 1;
    for (int q = 0; q < nq; ++q) {
        long long steps = 0; bool found = false; LONG lastExecSeq = 0, pendingExecs = 0;
        for (LONG s = execFrom; s <= execLast && steps < 60000000; ++s) {
            const BcExec& e = g_bcExec[s % kBcExecSlots]; if (e.seq != s || e.queue != queues[q]) continue;
            lastExecSeq = s;
            if (found) { ++pendingExecs; continue; }
            if (e.first < minId) continue;   // ring already overwrote these ops
            LONG prevDone = 0;
            for (LONG id = e.first; id <= e.last; ++id, ++steps) {
                LONG slot = id % kBcSlots; const BcInfo& in = g_bcInfo[slot];
                if (in.id != id || in.list != e.list) continue;
                UINT v = g_bcMem[slot];
                if (v == ((UINT)id | 0x80000000u)) { prevDone = id; continue; }
                found = true;
                log_line("[bc] EXEC-ORDER queue=%p exec seq=%ld list=%p: first unfinished op %ld (%s) kind=%c pso=%p shader=%016llx vs=%016llx args=%u,%u,%u | prev done op in list=%ld\r\n",
                         queues[q], (long)s, e.list, (long)id, v == (UINT)id ? "STARTED, never ended" : "not started", in.kind, in.pso,
                         (unsigned long long)BcPsoHash(in.pso), (unsigned long long)BcVsHash(in.pso), in.a, in.b, in.c, (long)prevDone);
                if (in.kind == 'X' && g_bcArgsMem) {
                    const volatile UINT* a = g_bcArgsMem + (size_t)slot * 16;
                    log_line("[bc]    arg bytes: %08X %08X %08X %08X %08X %08X %08X %08X\r\n", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
                }
                int shown = 0;   // the next few ops in the same list, for context
                for (LONG j = id + 1; j <= e.last && shown < 4; ++j) {
                    LONG sl = j % kBcSlots; const BcInfo& n2 = g_bcInfo[sl];
                    if (n2.id != j || n2.list != e.list) continue; ++shown;
                    log_line("[bc]    then op %ld kind=%c shader=%016llx vs=%016llx args=%u,%u,%u marker=%08X\r\n", (long)j, n2.kind,
                             (unsigned long long)BcPsoHash(n2.pso), (unsigned long long)BcVsHash(n2.pso), n2.a, n2.b, n2.c, g_bcMem[sl]);
                }
                break;
            }
        }
        if (found) log_line("[bc] EXEC-ORDER queue=%p: %ld later submission(s) still queued behind it (last seq %ld)\r\n", queues[q], (long)pendingExecs, (long)lastExecSeq);
        else log_line("[bc] EXEC-ORDER queue=%p: every recorded op finished (last seq %ld) -> this queue is waiting (fence) or the hang is in an uninstrumented op\r\n", queues[q], (long)lastExecSeq);
    }
}

static bool Upscaler_OnDispatch(BridgeCommandList* self, UINT gx, UINT gy, UINT gz);   // upscaler.inc: true = the dispatch was recorded there
static void STDMETHODCALLTYPE BridgeCmdList_Dispatch(
    BridgeCommandList* self,
    UINT ThreadGroupCountX,
    UINT ThreadGroupCountY,
    UINT ThreadGroupCountZ)
{
    CMD_STAT(STAT_DISPATCH);
    if (g_drawList && t_lastPso) { char b[160]; sprintf_s(b, "[dl] dispatch cs=%016llx groups=%u,%u,%u\r\n", (unsigned long long)BcPsoHash(t_lastPso), ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); log_line("%s", b); }
    { static const bool s_first = GetEnvironmentVariableA("ONRUSH_LOG_DISP_FIRST", nullptr, 0) > 0;   // first dispatch of each CS (+ frame)
      if (s_first && t_lastPso) { extern volatile LONG g_rtLogFrame; static UINT64 s_h[2048]; static volatile LONG s_n = 0; UINT64 h = (UINT64)BcPsoHash(t_lastPso); bool seen = false; LONG n_ = s_n; for (LONG q = 0; q < n_ && q < 2048; ++q) if (s_h[q] == h) { seen = true; break; }
        if (!seen) { LONG id_ = InterlockedIncrement(&s_n) - 1; if (id_ < 2048) { s_h[id_] = h; log_line("[dfirst] f=%ld cs=%016llx groups=%u,%u,%u\r\n", (long)g_rtLogFrame, (unsigned long long)h, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); } } } }
    { static char s_lst[512]; static int s_i = -1; if (s_i < 0) { s_i = (int)GetEnvironmentVariableA("ONRUSH_LOG_DISP_FIRST", s_lst, sizeof(s_lst)); if (s_i < 2) s_i = 0; }   // value = hash list: log EVERY dispatch of those CS
      if (s_i > 1 && t_lastPso) { char hb[20]; sprintf_s(hb, "%016llx", (unsigned long long)BcPsoHash(t_lastPso)); if (strstr(s_lst, hb)) { extern volatile LONG g_rtLogFrame; log_line("[dall] f=%ld cs=%s groups=%u,%u,%u\r\n", (long)g_rtLogFrame, hb, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
          static volatile LONG s_tl = 0; if (g_rtLogFrame > 1000 && InterlockedIncrement(&s_tl) <= 2) for (UINT p = 0; p < 16; ++p) if (t_ctab[p]) DescTrack_LogTable("dall", p, t_ctab[p], 40); } } }
    { // ONRUSH_FIND_CS_TEX3D=WxHxD[:u]: first dispatch of each CS that has a 3D texture of that size bound as SRV (or UAV with ":u") ([cs3d])
      static unsigned s_w = 1, s_h = 0, s_d = 0, s_k = 1; static UINT64 s_hh[1024]; static volatile LONG s_nn = 0;
      if (s_w == 1) { char e[48]; s_w = 0; if (GetEnvironmentVariableA("ONRUSH_FIND_CS_TEX3D", e, sizeof(e)) > 0) { sscanf_s(e, "%ux%ux%u", &s_w, &s_h, &s_d); if (strstr(e, ":u")) s_k = 2; } }
      if (s_w && t_lastPso) for (UINT p = 0; p < 16; ++p) { if (!t_ctab[p]) continue; UINT sl = 0; ID3D12Resource* r = DescTrack_FindTex3D(t_ctab[p], 64, s_w, s_h, s_d, s_k, &sl); if (!r) continue;
          UINT64 h = (UINT64)BcPsoHash(t_lastPso); bool seen = false; LONG n_ = s_nn; for (LONG q = 0; q < n_ && q < 1024; ++q) if (s_hh[q] == h) { seen = true; break; }
          if (!seen) { extern volatile LONG g_rtLogFrame; LONG id_ = InterlockedIncrement(&s_nn) - 1; if (id_ < 1024) { s_hh[id_] = h; log_line("[cs3d] f=%ld cs=%016llx param=%u slot=%u res=%p groups=%u,%u,%u\r\n", (long)g_rtLogFrame, (unsigned long long)h, p, sl, (void*)r, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); } }
          break; } }
    FindUavAtDispatch();
    { static int s_p = -2, s_slot = 0, s_cnt = 1; static void* s_seen[256]; static volatile LONG s_ns = 0;
      if (s_p == -2) { char sb[48] = {}; s_p = -1; if (GetEnvironmentVariableA("ONRUSH_LOG_CSLOT", sb, sizeof(sb)) > 0) sscanf_s(sb, "%d:%d:%d", &s_p, &s_slot, &s_cnt); }
      if (s_p >= 0 && s_p < 16 && t_ctab[s_p]) { { static LONG s_per = -1, s_ep = -1; if (s_per < 0) { char pb[16] = {}; s_per = GetEnvironmentVariableA("ONRUSH_CSLOT_PERIOD", pb, sizeof(pb)) > 0 ? atol(pb) : 0; } if (s_per > 0 && g_rtLogFrame / s_per != s_ep) { s_ep = g_rtLogFrame / s_per; s_ns = 0; log_line("[cslot-epoch] f%ld\r\n", (long)g_rtLogFrame); } } bool seen = false; LONG n_ = s_ns; void* key_ = (void*)((UINT64)t_lastPso ^ t_ctab[s_p]); for (LONG q = 0; q < n_ && q < 256; ++q) if (s_seen[q] == key_) seen = true;
        if (!seen) { LONG id_ = InterlockedIncrement(&s_ns) - 1; if (id_ < 256) { s_seen[id_] = key_; log_line("[cslot] pso=%p groups=%u,%u,%u\r\n", t_lastPso, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); DescTrack_LogTable("cslot", (UINT)s_slot, t_ctab[s_p] + (UINT64)s_slot * 32ull, (UINT)s_cnt); } } } }
    { static const UINT s_w3 = GetEnvironmentVariableA("ONRUSH_FIND_UAV3D", nullptr, 0) > 0 ? 128u : 0u;   // which dispatches write a 128^3 volume
      if (s_w3) { static void* s_seen[256]; static volatile LONG s_ns = 0;
        for (UINT p = 0; p < 16; ++p) { if (!t_ctab[p]) continue; UINT sl = 0; ID3D12Resource* r = DescTrack_FindUav3D(t_ctab[p], 64, s_w3, &sl); if (!r) continue;
          bool seen = false; LONG n_ = s_ns; for (LONG q = 0; q < n_ && q < 256; ++q) if (s_seen[q] == t_lastPso) seen = true;
          if (!seen) { LONG id_ = InterlockedIncrement(&s_ns) - 1; if (id_ < 256) s_seen[id_] = t_lastPso; log_line("[uav3d] pso=%p param=%u slot=%u res=%p groups=%u,%u,%u\r\n", t_lastPso, p, sl, (void*)r, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
          break; } } }
    { static int s_p = -2, s_slot = 0, s_cnt = 1; static void* s_seen[256]; static volatile LONG s_ns = 0;   // second spec: ONRUSH_LOG_CSLOT2
      if (s_p == -2) { char sb[48] = {}; s_p = -1; if (GetEnvironmentVariableA("ONRUSH_LOG_CSLOT2", sb, sizeof(sb)) > 0) sscanf_s(sb, "%d:%d:%d", &s_p, &s_slot, &s_cnt); }
      if (s_p >= 0 && s_p < 16 && t_ctab[s_p]) { { static LONG s_per = -1, s_ep = -1; if (s_per < 0) { char pb[16] = {}; s_per = GetEnvironmentVariableA("ONRUSH_CSLOT_PERIOD", pb, sizeof(pb)) > 0 ? atol(pb) : 0; } if (s_per > 0 && g_rtLogFrame / s_per != s_ep) { s_ep = g_rtLogFrame / s_per; s_ns = 0; log_line("[cslot-epoch] f%ld\r\n", (long)g_rtLogFrame); } } bool seen = false; LONG n_ = s_ns; for (LONG q = 0; q < n_ && q < 256; ++q) if (s_seen[q] == t_lastPso) seen = true;
        if (!seen) { LONG id_ = InterlockedIncrement(&s_ns) - 1; if (id_ < 256) { s_seen[id_] = t_lastPso; log_line("[cslot2] pso=%p\n", t_lastPso); DescTrack_LogTable("cslot2", (UINT)s_slot, t_ctab[s_p] + (UINT64)s_slot * 32ull, (UINT)s_cnt); } } } }
    { static const bool s_wa = GetEnvironmentVariableA("ONRUSH_WATCH_AO", nullptr, 0) > 0;
      if (s_wa) for (UINT p = 0; p < 16; ++p) { if (!t_ctab[p]) continue; UINT sl = 0; ID3D12Resource* r = DescTrack_FindUav3D(t_ctab[p], 64, 128, &sl); if (r && IsAoRes(r)) { static void* s_seen[256]; static volatile LONG s_ns = 0; bool seen = false; LONG n_ = s_ns; void* k_ = (void*)((UINT64)t_lastPso ^ (UINT64)r ^ ((UINT64)ThreadGroupCountZ << 40)); for (LONG q = 0; q < n_ && q < 256; ++q) if (s_seen[q] == k_) seen = true;
          if (!seen) { LONG id_ = InterlockedIncrement(&s_ns) - 1; if (id_ < 256) s_seen[id_] = k_; log_line("[watchao] f%ld dispatch pso=%p param=%u slot=%u res=%p groups=%u,%u,%u type=%d\r\n", (long)g_rtLogFrame, t_lastPso, p, sl, (void*)r, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ, (int)self->m_type); } } } }
    { // ONRUSH_LOG_DISPATCH=1: every dispatch; =N (>1): only the dispatches recorded in frame N, with the CS hash and list type
      static long s_la = -2; if (s_la == -2) { char lb[16] = {}; s_la = GetEnvironmentVariableA("ONRUSH_LOG_DISPATCH", lb, sizeof(lb)) > 0 ? atol(lb) : -1; }
      extern volatile LONG g_rtLogFrame;
      if (s_la == 1 || (s_la > 1 && g_rtLogFrame == s_la)) log_line("%s pso=%p cs=%016llx list=%d groups=%u,%u,%u\r\n", "[dispall]", t_lastPso, (unsigned long long)BcPsoHash(t_lastPso), self ? (int)self->m_type : -1, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    LONG tsIdx = -1; ID3D12GraphicsCommandList* tsCl = nullptr;
    if (g_drawDetail && g_tsHeap && g_tsReadback && self && self->m_pRealCmdList && self->m_type == D3D12_COMMAND_LIST_TYPE_DIRECT) { LONG ti = InterlockedIncrement(&g_tsNext) - 1; if (ti < 1024) { tsIdx = ti; tsCl = self->m_pRealCmdList; g_tsInfo[ti] = { t_lastPso, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ }; tsCl->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * ti)); } }
    if (g_drawDetail && InterlockedIncrement(&g_drawDetailCount) <= 3000)
        log_line("[disp] pso=%p groups=%u,%u,%u\r\n", t_lastPso, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    if (self && self->m_pRealCmdList && Upscaler_OnDispatch(self, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ)) {
        if (tsIdx >= 0) { tsCl->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * tsIdx + 1)); tsCl->ResolveQueryData(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * tsIdx), 2, g_tsReadback, (UINT64)(2 * tsIdx) * 8); }
        return;
    }
    if (self && self->m_pRealCmdList)
    {
        LONG bc = BcBegin(self, 'D', ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        self->m_pRealCmdList->Dispatch(
            ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        BcEnd(self, bc);
    }
    if (tsIdx >= 0) { tsCl->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * tsIdx + 1)); tsCl->ResolveQueryData(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(2 * tsIdx), 2, g_tsReadback, (UINT64)(2 * tsIdx) * 8); }
}

static inline void ListEnsureState(BridgeCommandList* self, ID3D12Resource* res, D3D12_RESOURCE_STATES need);
static inline D3D12_RESOURCE_STATES ListCurrentState(BridgeCommandList* self, ID3D12Resource* res);
static void STDMETHODCALLTYPE BridgeCmdList_CopyBufferRegion(
    BridgeCommandList* self,
    ID3D12Resource* pDstBuffer,
    UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer,
    UINT64 SrcOffset,
    UINT64 NumBytes)
{
    if (SrcOffset >= (16ull << 20) && pSrcBuffer) g_bigCopySrc = pSrcBuffer;
    { static const bool s_logAll = GetEnvironmentVariableA("ONRUSH_LOG_DISPATCH", nullptr, 0) > 0; static volatile LONG s_n = 0;
      if (s_logAll && InterlockedIncrement(&s_n) <= 3000) log_line("[cbr] dst=%p+%llu src=%p+%llu n=%llu\n", (void*)pDstBuffer, (unsigned long long)DstOffset, (void*)pSrcBuffer, (unsigned long long)SrcOffset, (unsigned long long)NumBytes); }
    if (self && self->m_pRealCmdList)
    {
        // The game copies UAV hidden counters in/out of their counter resource with CopyBufferRegion
        // (reset from a zero buffer, read back into ExecuteIndirect args). Without transitions the
        // buffers stayed in whatever state the copy left them in while compute used them as UAV /
        // counter. Copy lists keep implicit promotion/decay (only COMMON/COPY_* are legal there).
        static const bool s_noCbrStates = GetEnvironmentVariableA("ONRUSH_NO_CBR_STATES", nullptr, 0) > 0;
        const bool doStates = !s_noCbrStates && self->m_type != D3D12_COMMAND_LIST_TYPE_COPY &&
                              pDstBuffer && pSrcBuffer && pDstBuffer != pSrcBuffer;
        D3D12_RESOURCE_STATES dBefore = D3D12_RESOURCE_STATE_COMMON, sBefore = D3D12_RESOURCE_STATE_COMMON;
        if (doStates) {
            dBefore = ListCurrentState(self, pDstBuffer);
            sBefore = ListCurrentState(self, pSrcBuffer);
            ListEnsureState(self, pDstBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
            ListEnsureState(self, pSrcBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        LONG bcC = (g_bcArgDumpWindow && g_bcArgsMem && pSrcBuffer && pDstBuffer) ? BcBegin(self, 'C', (UINT)NumBytes, 0, 0) : -1;
        if (bcC >= 0) {   // snapshot what is copied (first 56 bytes of the source range) right before the copy
            g_bcRes[bcC] = { pDstBuffer, DstOffset, pSrcBuffer, SrcOffset };
            for (int i = 0; i < 16; ++i) g_bcArgsMem[(size_t)bcC * 16 + i] = 0xCDCDCDCDu;
            if (self->m_type != D3D12_COMMAND_LIST_TYPE_COPY && g_bcArgs) {
                D3D12_RESOURCE_STATES sb = ListCurrentState(self, pSrcBuffer);
                ListEnsureState(self, pSrcBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
                self->m_pRealCmdList->CopyBufferRegion(g_bcArgs, (UINT64)bcC * 64, pSrcBuffer, SrcOffset, NumBytes < 56 ? NumBytes : 56);
                ListEnsureState(self, pSrcBuffer, sb);
            }
        }
        self->m_pRealCmdList->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
        BcEnd(self, bcC);
        if (doStates) {
            ListEnsureState(self, pDstBuffer, dBefore);
            ListEnsureState(self, pSrcBuffer, sBefore);
        }
    }
}

static inline bool SafeGetResourceDesc(ID3D12Resource* pRes, D3D12_RESOURCE_DESC* pDesc);
static void WatchAo(const char* what, ID3D12Resource* r);
static void STDMETHODCALLTYPE BridgeCmdList_CopyTextureRegion(
    BridgeCommandList* self,
    const D3D12_TEXTURE_COPY_LOCATION* pDst,
    UINT DstX, UINT DstY, UINT DstZ,
    const D3D12_TEXTURE_COPY_LOCATION* pSrc,
    const D3D12_BOX* pSrcBox)
{
    if (pDst) WatchAo("copytex-dst", pDst->pResource);
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    DWORD tid = GetCurrentThreadId();

    CMD_STAT(STAT_COPYTEX);
    static volatile LONG s_copyLog = 0;
    if (InterlockedIncrement(&s_copyLog) <= 100)
    log_line("[d3d12_x] BridgeCmdList_CopyTextureRegion: self=%p, real=%p, type=%d, pDst=%p, Dst=(%u,%u,%u), pSrc=%p, pSrcBox=%p, tid=%u, caller=%p (game+0x%llX)\r\n",
             self, self ? self->m_pRealCmdList : nullptr, self ? (int)self->m_type : -1,
             pDst, DstX, DstY, DstZ, pSrc, pSrcBox, tid, retAddr, (unsigned long long)rva);

    // A placed footprint whose RowPitch is smaller than one row of its format makes Close() fail;
    // executing the unclosed list then removes the device (DXGI_ERROR_INVALID_CALL). Seen when the
    // tutorial starts: 1600 px R16G16B16A16_FLOAT with RowPitch 6400 (the Xbox layout of a 4-byte format).
    const D3D12_TEXTURE_COPY_LOCATION* locs[2] = { pDst, pSrc };
    for (int li = 0; li < 2; ++li) {
        const D3D12_TEXTURE_COPY_LOCATION* L = locs[li];
        if (!L || L->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT || !g_pMainDevice) continue;
        const D3D12_SUBRESOURCE_FOOTPRINT& f = L->PlacedFootprint.Footprint;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = f.Depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = f.Width; rd.Height = f.Height; rd.DepthOrArraySize = (UINT16)(f.Depth ? f.Depth : 1);
        rd.MipLevels = 1; rd.Format = f.Format; rd.SampleDesc.Count = 1;
        UINT64 rowSize = 0; UINT rows = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT need = {};
        g_pMainDevice->GetCopyableFootprints(&rd, 0, 1, 0, &need, &rows, &rowSize, nullptr);
        if (rowSize && f.RowPitch < rowSize) {
            const D3D12_TEXTURE_COPY_LOCATION* T = locs[1 - li];
            D3D12_RESOURCE_DESC td = {};
            if (T && T->pResource) SafeGetResourceDesc(T->pResource, &td);
            static volatile LONG s_bad = 0;
            if (InterlockedIncrement(&s_bad) <= 20)
                log_line("[copytex] SKIP bad footprint (%s): %ux%u fmt=%d RowPitch=%u < row %llu | texture %llux%u fmt=%d caller=game+0x%llX\r\n",
                         li ? "src" : "dst", f.Width, f.Height, (int)f.Format, f.RowPitch, (unsigned long long)rowSize,
                         (unsigned long long)td.Width, td.Height, (int)td.Format, (unsigned long long)rva);
            return;
        }
    }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

static void STDMETHODCALLTYPE BridgeCmdList_CopyResource(
    BridgeCommandList* self,
    ID3D12Resource* pDstResource,
    ID3D12Resource* pSrcResource)
{
    if (self && self->m_pRealCmdList)
        { WatchAo("copyres-dst", pDstResource); self->m_pRealCmdList->CopyResource(pDstResource, pSrcResource); }
}

static void STDMETHODCALLTYPE BridgeCmdList_ResolveSubresource(
    BridgeCommandList* self,
    ID3D12Resource* pDstResource,
    UINT DstSubresource,
    ID3D12Resource* pSrcResource,
    UINT SrcSubresource,
    DXGI_FORMAT Format)
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

static void STDMETHODCALLTYPE BridgeCmdList_IASetPrimitiveTopology(
    BridgeCommandList* self,
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
    if (self && self->m_pRealCmdList) {
        t_topo = (int)PrimitiveTopology;
        // tessellation bypass: the terrain PSOs run HS/DS folded into the VS (tess_bypass.py), so their
        // 3-control-point patch lists are drawn as triangle lists
        static const bool s_noTess = GetEnvironmentVariableA("ONRUSH_NO_TESS_BYPASS", nullptr, 0) > 0;
        if (!s_noTess && PrimitiveTopology == D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST)
            PrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        self->m_pRealCmdList->IASetPrimitiveTopology(PrimitiveTopology);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_RSSetViewports(
    BridgeCommandList* self,
    UINT NumViewports,
    const D3D12_VIEWPORT* pViewports)
{
    if (g_drawDetail && pViewports && NumViewports && InterlockedIncrement(&g_drawDetailCount) <= 400)
        log_line("[draw] Viewport n=%u (%.1f,%.1f %.1fx%.1f z=%.2f..%.2f) RT=%p\r\n",
                 NumViewports, pViewports[0].TopLeftX, pViewports[0].TopLeftY, pViewports[0].Width, pViewports[0].Height,
                 pViewports[0].MinDepth, pViewports[0].MaxDepth, self ? self->m_pActiveRenderTarget : nullptr);
    if (pViewports && NumViewports) { t_vp0 = pViewports[0]; t_vpList = self; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->RSSetViewports(NumViewports, pViewports);
}

static void STDMETHODCALLTYPE BridgeCmdList_RSSetScissorRects(
    BridgeCommandList* self,
    UINT NumRects,
    const D3D12_RECT* pRects)
{
    if (g_drawDetail && pRects && NumRects && InterlockedIncrement(&g_drawDetailCount) <= 400)
        log_line("[draw] Scissor n=%u (%ld,%ld %ld,%ld)\r\n", NumRects, pRects[0].left, pRects[0].top, pRects[0].right, pRects[0].bottom);
    if (pRects && NumRects) { t_sc0 = pRects[0]; t_scList = self; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->RSSetScissorRects(NumRects, pRects);
}

static void STDMETHODCALLTYPE BridgeCmdList_OMSetBlendFactor(
    BridgeCommandList* self,
    const FLOAT BlendFactor[4])
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->OMSetBlendFactor(BlendFactor);
}

static void STDMETHODCALLTYPE BridgeCmdList_OMSetStencilRef(
    BridgeCommandList* self,
    UINT StencilRef)
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->OMSetStencilRef(StencilRef);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetPipelineState(
    BridgeCommandList* self,
    ID3D12PipelineState* pPipelineState)
{
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    DWORD tid = GetCurrentThreadId();

    CMD_STAT(STAT_PSO);
    t_lastPso = pPipelineState;
    static volatile LONG s_psoLog = 0;
    bool logPso = InterlockedIncrement(&s_psoLog) <= 200;
    bool isKnownRes = IsKnownReturnedResource(pPipelineState);
    if (isKnownRes)
    {
        log_line("[d3d12_x] ALERT: SetPipelineState received known ID3D12Resource pointer! pso=%p, self=%p, real=%p, tid=%u, caller=%p (game+0x%llX)\r\n",
                 pPipelineState, self, self ? self->m_pRealCmdList : nullptr, tid, retAddr, (unsigned long long)rva);
    }
    else if (logPso)
    {
        log_line("[d3d12_x] BridgeCmdList_SetPipelineState: pso=%p, self=%p, real=%p, tid=%u, caller=%p (game+0x%llX)\r\n",
                 pPipelineState, self, self ? self->m_pRealCmdList : nullptr, tid, retAddr, (unsigned long long)rva);
    }

    DumpPsoShaders(pPipelineState);

    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetPipelineState(pPipelineState);
}

static SRWLOCK g_ResStateLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_ResStateMap;

inline void TrackResourceState(ID3D12Resource* pRes, D3D12_RESOURCE_STATES state)
{
    if (!pRes) return;
    AcquireSRWLockExclusive(&g_ResStateLock);
    g_ResStateMap[pRes] = state;
    ReleaseSRWLockExclusive(&g_ResStateLock);
}

inline D3D12_RESOURCE_STATES GetTrackedResourceState(ID3D12Resource* pRes, D3D12_RESOURCE_STATES defaultState)
{
    if (!pRes) return defaultState;
    AcquireSRWLockShared(&g_ResStateLock);
    auto it = g_ResStateMap.find(pRes);
    D3D12_RESOURCE_STATES st = (it != g_ResStateMap.end()) ? it->second : defaultState;
    ReleaseSRWLockShared(&g_ResStateLock);
    return st;
}

static inline void ExecuteResourceBarrierSafe(ID3D12GraphicsCommandList* pCmdList, UINT count, const D3D12_RESOURCE_BARRIER* pBarriers)
{
    __try {
        pCmdList->ResourceBarrier(count, pBarriers);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[d3d12_x] Exception in RealCmdList->ResourceBarrier (NumBarriers=%u)\r\n", count);
    }
}

static inline bool SafeGetResourceDesc(ID3D12Resource* pRes, D3D12_RESOURCE_DESC* pDesc)
{
    __try {
        *pDesc = pRes->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ----------------------------------------------------------------------------
// Per-command-list resource state tracking.
// Durango has no resource states and records command lists on many threads that are
// executed in a different order; a single global state map updated at *record* time
// produced millions of D3D12 state-mismatch errors (and garbage/zero render targets).
// Each list now remembers, per resource, the state it first assumed (`first`) and the
// state it leaves behind (`cur`). At ExecuteCommandLists a small "prologue" command list
// transitions resources from their real execution-time state to `first`, and the global
// map (execution-time truth) is then updated from `cur`.
// ----------------------------------------------------------------------------
struct ListResState { D3D12_RESOURCE_STATES first; D3D12_RESOURCE_STATES cur; };
typedef std::unordered_map<ID3D12Resource*, ListResState> ListStateMap;

static inline ListStateMap* GetListStates(BridgeCommandList* l, bool create)
{
    if (!l->m_stateMap && create) l->m_stateMap = new ListStateMap();
    return (ListStateMap*)l->m_stateMap;
}

static inline void ListStatesClear(BridgeCommandList* l)
{
    if (l && l->m_stateMap) ((ListStateMap*)l->m_stateMap)->clear();
}

static inline void ListEnsureState(BridgeCommandList* self, ID3D12Resource* res, D3D12_RESOURCE_STATES need)
{
    if (!self || !self->m_pRealCmdList || !res) return;
    if (IsUploadResource(res) || IsStagingBuffer(res)) return;
    ListStateMap* lm = GetListStates(self, true);
    auto it = lm->find(res);
    if (it == lm->end()) { (*lm)[res] = { need, need }; return; }
    if (it->second.cur == need) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = it->second.cur;
    b.Transition.StateAfter = need;
    ExecuteResourceBarrierSafe(self->m_pRealCmdList, 1, &b);
    it->second.cur = need;
}

// State `res` is in at this point of the list (list-local if touched, else the execution-time map).
static inline D3D12_RESOURCE_STATES ListCurrentState(BridgeCommandList* self, ID3D12Resource* res)
{
    if (self && res) {
        ListStateMap* lm = GetListStates(self, false);
        if (lm) { auto it = lm->find(res); if (it != lm->end()) return it->second.cur; }
    }
    return GetTrackedResourceState(res, D3D12_RESOURCE_STATE_COMMON);
}

// Barriers needed before `b` can run, given the execution-time state of every resource it touches.
static inline void BuildListPrologue(BridgeCommandList* b, D3D12_COMMAND_LIST_TYPE queueType,
                                     std::vector<D3D12_RESOURCE_BARRIER>& out)
{
    ListStateMap* lm = (ListStateMap*)b->m_stateMap;
    if (!lm) return;
    for (auto& kv : *lm) {
        D3D12_RESOURCE_STATES want = kv.second.first;
        D3D12_RESOURCE_STATES actual = GetTrackedResourceState(kv.first, D3D12_RESOURCE_STATE_COMMON);
        if (queueType == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            const UINT kGfxOnly = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
            if (want & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                want = (D3D12_RESOURCE_STATES)((want & ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (actual & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                actual = (D3D12_RESOURCE_STATES)((actual & ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if ((want & kGfxOnly) || (actual & kGfxOnly)) continue;
        }
        if (actual == want) continue;
        D3D12_RESOURCE_BARRIER tb = {};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = kv.first;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = actual;
        tb.Transition.StateAfter = want;
        out.push_back(tb);
    }
}

// After the list was submitted: its final states become the execution-time truth.
static inline void CommitListStates(BridgeCommandList* b)
{
    ListStateMap* lm = (ListStateMap*)b->m_stateMap;
    if (!lm) return;
    for (auto& kv : *lm) TrackResourceState(kv.first, kv.second.cur);
}

static UINT STDMETHODCALLTYPE BridgeCmdList_ResourceBarrier(
    BridgeCommandList* self,
    UINT NumBarriers,
    const D3D12_RESOURCE_BARRIER* pBarriers)
{
    if (!self || !self->m_pRealCmdList || !pBarriers || NumBarriers == 0)
        return 0;

    // Desktop D3D12 specification: Resource barriers are NOT supported on Copy command lists.
    // Copy queues require all resources to be in D3D12_RESOURCE_STATE_COMMON (implicit promotion & decay).
    if (self->m_type == D3D12_COMMAND_LIST_TYPE_COPY) {
        return 0;
    }

    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

    static volatile LONG s_barrierLogCount = 0;
    bool shouldLog = (InterlockedIncrement(&s_barrierLogCount) <= 60);
    if (shouldLog) {
        log_line("[d3d12_x] BridgeCmdList_ResourceBarrier: self=%p, real=%p, type=%d, NumBarriers=%u, caller=game+0x%llX\r\n",
                 self, self->m_pRealCmdList, (int)self->m_type, NumBarriers, (unsigned long long)rva);
    }

    std::vector<D3D12_RESOURCE_BARRIER> validBarriers;
    validBarriers.reserve(NumBarriers);

    const UINT kValidD3D12StateMask = 
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | // 0x1
        D3D12_RESOURCE_STATE_INDEX_BUFFER |               // 0x2
        D3D12_RESOURCE_STATE_RENDER_TARGET |               // 0x4
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS |            // 0x8
        D3D12_RESOURCE_STATE_DEPTH_WRITE |                 // 0x10
        D3D12_RESOURCE_STATE_DEPTH_READ |                  // 0x20
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |   // 0x40
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |       // 0x80
        D3D12_RESOURCE_STATE_STREAM_OUT |                  // 0x100
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |           // 0x200
        D3D12_RESOURCE_STATE_COPY_DEST |                   // 0x400
        D3D12_RESOURCE_STATE_COPY_SOURCE |                 // 0x800
        D3D12_RESOURCE_STATE_RESOLVE_DEST |                // 0x1000
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE;               // 0x2000

    for (UINT i = 0; i < NumBarriers; ++i) {
        D3D12_RESOURCE_BARRIER b = pBarriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            // Desktop D3D12 does not support Durango split barriers across command lists.
            // Split barriers left open cause desktop GPU drivers to return black (all zeroes) when sampled.
            // Clear any BEGIN_ONLY / END_ONLY flags so all transitions execute immediately and completely.
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

            ID3D12Resource* pRes = b.Transition.pResource;
            if (!pRes || (uintptr_t)pRes < 0x10000 || IsBadReadPtr(pRes, sizeof(void*))) {
                if (shouldLog) log_line("  barrier[%u]: SKIPPED null/invalid/unreadable pResource=%p\r\n", i, pRes);
                continue;
            }

            // Skip barriers on upload heap resources (D3D12 forbids transitions on upload heaps)
            if (IsUploadResource(pRes) || IsStagingBuffer(pRes)) {
                if (shouldLog) log_line("  barrier[%u]: SKIPPED upload heap buffer %p\r\n", i, pRes);
                continue;
            }

            D3D12_RESOURCE_DESC desc = {};
            if (!SafeGetResourceDesc(pRes, &desc)) {
                if (shouldLog) log_line("  barrier[%u]: SKIPPED exception querying GetDesc() for %p\r\n", i, pRes);
                continue;
            }

            // The state model is per resource, so every transition covers all subresources.
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                b.Transition.Subresource = 0;
            }

            // Sanitize Durango console bitflags (such as 0x400000, etc.)
            D3D12_RESOURCE_STATES stateBefore = (D3D12_RESOURCE_STATES)(b.Transition.StateBefore & kValidD3D12StateMask);
            D3D12_RESOURCE_STATES stateAfter  = (D3D12_RESOURCE_STATES)(b.Transition.StateAfter  & kValidD3D12StateMask);
            { static const bool s_l3 = GetEnvironmentVariableA("ONRUSH_LOG_BAR3D", nullptr, 0) > 0; static volatile LONG s_n3 = 0;
              if (s_l3 && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc.Width == 128 && desc.Height == 128 && InterlockedIncrement(&s_n3) <= 600)
                log_line("[bar3d] f%ld res=%p %llux%u fmt=%d type=%d %X->%X raw %X->%X sub=%X flags=%X caller=game+0x%llX\r\n", (long)g_rtLogFrame, (void*)pRes, (unsigned long long)desc.Width, desc.Height, (int)desc.Format, (int)self->m_type,
                         (UINT)stateBefore, (UINT)stateAfter, (UINT)pBarriers[i].Transition.StateBefore, (UINT)pBarriers[i].Transition.StateAfter, pBarriers[i].Transition.Subresource, (UINT)pBarriers[i].Flags, (unsigned long long)rva); }

            // Command list type restrictions
            if (self->m_type == D3D12_COMMAND_LIST_TYPE_COPY) {
                const UINT kCopyMask = D3D12_RESOURCE_STATE_COMMON | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE;
                stateBefore = (D3D12_RESOURCE_STATES)(stateBefore & kCopyMask);
                stateAfter  = (D3D12_RESOURCE_STATES)(stateAfter  & kCopyMask);
            } else if (self->m_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
                if (stateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                    stateAfter = (D3D12_RESOURCE_STATES)((stateAfter & ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                if (stateBefore & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                    stateBefore = (D3D12_RESOURCE_STATES)((stateBefore & ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                const UINT kComputeInvalid = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
                if ((stateBefore & kComputeInvalid) || (stateAfter & kComputeInvalid)) {
                    if (shouldLog) log_line("  barrier[%u]: SKIPPED compute-incompatible barrier (res=%p, before=0x%X, after=0x%X)\r\n",
                                            i, pRes, (UINT)stateBefore, (UINT)stateAfter);
                    continue;
                }
            }

            // Validate resource flag capabilities for both before and after states
            if (((stateBefore | stateAfter) & D3D12_RESOURCE_STATE_RENDER_TARGET) && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                if (shouldLog) log_line("  barrier[%u]: SKIPPED RTV state for resource %p without ALLOW_RENDER_TARGET\r\n", i, pRes);
                continue;
            }
            if (((stateBefore | stateAfter) & D3D12_RESOURCE_STATE_DEPTH_WRITE) && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
                if (shouldLog) log_line("  barrier[%u]: SKIPPED DSV state for resource %p without ALLOW_DEPTH_STENCIL\r\n", i, pRes);
                continue;
            }
            if (((stateBefore | stateAfter) & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
                if (shouldLog) log_line("  barrier[%u]: SKIPPED UAV state for resource %p without ALLOW_UNORDERED_ACCESS\r\n", i, pRes);
                continue;
            }

            // D3D12 specification: for buffers, Subresource MUST be 0 and RTV/DSV states are strictly forbidden
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                b.Transition.Subresource = 0;
                const UINT kInvalidBufferStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
                if ((stateBefore & kInvalidBufferStates) || (stateAfter & kInvalidBufferStates)) {
                    stateBefore = (D3D12_RESOURCE_STATES)(stateBefore & ~kInvalidBufferStates);
                    stateAfter  = (D3D12_RESOURCE_STATES)(stateAfter  & ~kInvalidBufferStates);
                    if (!stateBefore) stateBefore = D3D12_RESOURCE_STATE_COMMON;
                    if (!stateAfter)  stateAfter  = D3D12_RESOURCE_STATE_COMMON;
                }
            }

            bool isPureDepthFormat = (desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                                      desc.Format == DXGI_FORMAT_D32_FLOAT ||
                                      desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                                      desc.Format == DXGI_FORMAT_D16_UNORM);

            if (isPureDepthFormat || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
                // Pure depth-stencil formats cannot transition to SRV states in desktop D3D12;
                // map to DEPTH_READ to keep the command list valid and prevent E_INVALIDARG.
                const D3D12_RESOURCE_STATES kSRVMask = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (stateAfter & kSRVMask) {
                    stateAfter = (D3D12_RESOURCE_STATES)((stateAfter & ~kSRVMask) | D3D12_RESOURCE_STATE_DEPTH_READ);
                }
                if (stateBefore & kSRVMask) {
                    stateBefore = (D3D12_RESOURCE_STATES)((stateBefore & ~kSRVMask) | D3D12_RESOURCE_STATE_DEPTH_READ);
                }
            }

            // Per-list state model: the first barrier for a resource fixes the state this list
            // assumes on entry (the Execute-time prologue reconciles it); later barriers must
            // start from what this list itself left behind.
            {
                ListStateMap* lm = GetListStates(self, true);
                auto lit = lm->find(pRes);
                if (lit == lm->end()) {
                    (*lm)[pRes] = { stateBefore, stateBefore };
                } else if (lit->second.cur != stateBefore) {
                    stateBefore = lit->second.cur;
                }
                (*lm)[pRes].cur = stateAfter;
            }

            // Drop no-op transitions (e.g. pure Durango internal cache flushes)
            if (stateBefore == stateAfter) {
                continue;
            }

            b.Transition.StateBefore = stateBefore;
            b.Transition.StateAfter  = stateAfter;

            if (shouldLog && i < 10) {
                log_line("  barrier[%u]: TRANSITION res=%p sub=%u 0x%X -> 0x%X (orig 0x%X -> 0x%X)\r\n",
                         i, pRes, b.Transition.Subresource, (UINT)stateBefore, (UINT)stateAfter,
                         (UINT)pBarriers[i].Transition.StateBefore, (UINT)pBarriers[i].Transition.StateAfter);
            }
            validBarriers.push_back(b);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            validBarriers.push_back(b);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            // all port resources are committed: aliasing barriers are invalid (debug layer #528)
        }
    }

    if (!validBarriers.empty()) {
        ExecuteResourceBarrierSafe(self->m_pRealCmdList, (UINT)validBarriers.size(), validBarriers.data());
    }
    return 0;
}

static void STDMETHODCALLTYPE BridgeCmdList_ExecuteBundle(
    BridgeCommandList* self,
    ID3D12GraphicsCommandList* pCommandList)
{
    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
    DWORD tid = GetCurrentThreadId();

    ID3D12GraphicsCommandList* pRealBundle = (ID3D12GraphicsCommandList*)UnwrapCommandList(pCommandList);
    bool isKnownResIn = IsKnownReturnedResource(pCommandList);
    bool isKnownResUnwrapped = IsKnownReturnedResource(pRealBundle);

    if (isKnownResIn || isKnownResUnwrapped)
    {
        log_line("[d3d12_x] ALERT: ExecuteBundle received known ID3D12Resource pointer! bundleIn=%p, realBundle=%p, self=%p, real=%p, tid=%u, caller=%p (game+0x%llX)\r\n",
                 pCommandList, pRealBundle, self, self ? self->m_pRealCmdList : nullptr, tid, retAddr, (unsigned long long)rva);
    }
    else
    {
        log_line("[d3d12_x] BridgeCmdList_ExecuteBundle: self=%p, real=%p, bundleIn=%p, realBundle=%p, tid=%u, caller=%p (game+0x%llX)\r\n",
                 self, self ? self->m_pRealCmdList : nullptr, pCommandList, pRealBundle, tid, retAddr, (unsigned long long)rva);
    }

    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->ExecuteBundle(pRealBundle);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetDescriptorHeaps(
    BridgeCommandList* self,
    UINT NumDescriptorHeaps,
    ID3D12DescriptorHeap* const* ppDescriptorHeaps)
{
    if (!self || !self->m_pRealCmdList) return;
    static volatile LONG s_heapLogCount = 0;
    if (InterlockedIncrement(&s_heapLogCount) <= 50) {
        void* retAddr = _ReturnAddress();
        HMODULE hExe = GetModuleHandleW(nullptr);
        uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
        log_line("[d3d12_x] BridgeCmdList_SetDescriptorHeaps: self=%p, NumHeaps=%u, caller=game+0x%llX\r\n",
                 self, NumDescriptorHeaps, (unsigned long long)rva);
    }
    if (NumDescriptorHeaps == 0 || !ppDescriptorHeaps || (uintptr_t)ppDescriptorHeaps <= 0x10000) return;

    if (NumDescriptorHeaps > 2) {
        NumDescriptorHeaps = 2;
    }

    ID3D12DescriptorHeap* validHeaps[2] = {};
    UINT validCount = 0;
    for (UINT i = 0; i < NumDescriptorHeaps; ++i) {
        ID3D12DescriptorHeap* pHeap = ppDescriptorHeaps[i];
        if (pHeap && (uintptr_t)pHeap > 0x10000) {
            validHeaps[validCount++] = pHeap;
        }
    }

    if (validCount > 0) {
        t_heaps[0] = validHeaps[0]; t_heaps[1] = validHeaps[1]; t_nHeaps = validCount;
        self->m_pRealCmdList->SetDescriptorHeaps(validCount, validHeaps);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRootSignature(
    BridgeCommandList* self,
    ID3D12RootSignature* pRootSignature)
{
    static volatile LONG s_sigLogCount = 0;
    if (InterlockedIncrement(&s_sigLogCount) <= 50) {
        void* retAddr = _ReturnAddress();
        HMODULE hExe = GetModuleHandleW(nullptr);
        uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
        log_line("[d3d12_x] BridgeCmdList_SetComputeRootSignature: self=%p, rootSig=%p, caller=game+0x%llX\r\n",
                 self, pRootSignature, (unsigned long long)rva);
    }
    if (!self || !self->m_pRealCmdList || !pRootSignature || (uintptr_t)pRootSignature <= 0x10000)
        return;

    t_csRs = pRootSignature; memset(t_cKind, 0, sizeof(t_cKind)); memset(t_cConstN, 0, sizeof(t_cConstN));
    self->m_pRealCmdList->SetComputeRootSignature(pRootSignature);
    { UINT xp = LookupRsExtraParam(pRootSignature); if (xp != 0xFFFFFFFFu && g_CtrBuf) self->m_pRealCmdList->SetComputeRootUnorderedAccessView(xp, g_CtrBuf->GetGPUVirtualAddress()); }
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRootSignature(
    BridgeCommandList* self,
    ID3D12RootSignature* pRootSignature)
{
    static volatile LONG s_sigLogCount = 0;
    if (InterlockedIncrement(&s_sigLogCount) <= 50) {
        void* retAddr = _ReturnAddress();
        HMODULE hExe = GetModuleHandleW(nullptr);
        uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
        log_line("[d3d12_x] BridgeCmdList_SetGraphicsRootSignature: self=%p, rootSig=%p, caller=game+0x%llX\r\n",
                 self, pRootSignature, (unsigned long long)rva);
    }
    if (!self || !self->m_pRealCmdList || !pRootSignature || (uintptr_t)pRootSignature <= 0x10000)
        return;

    t_gfxRs = pRootSignature;
    self->m_pRealCmdList->SetGraphicsRootSignature(pRootSignature);
    { UINT xp = LookupRsExtraParam(pRootSignature); if (xp != 0xFFFFFFFFu && g_CtrBuf) self->m_pRealCmdList->SetGraphicsRootUnorderedAccessView(xp, g_CtrBuf->GetGPUVirtualAddress()); }
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRootDescriptorTable(
    BridgeCommandList* self,
    UINT RootParameterIndex,
    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    static volatile LONG s_cTableLogCount = 0;
    if (InterlockedIncrement(&s_cTableLogCount) <= 20) {
        void* retAddr = _ReturnAddress();
        HMODULE hExe = GetModuleHandleW(nullptr);
        uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
        log_line("[d3d12_x] SetComputeRootDescriptorTable: self=%p, slot=%u, BaseDescriptor=0x%llX, caller=game+0x%llX\r\n",
                 self, RootParameterIndex, (unsigned long long)BaseDescriptor.ptr, (unsigned long long)rva);
    }
    FixRawDescriptors(BaseDescriptor.ptr, t_csRs, RootParameterIndex);
    if (RootParameterIndex < 16) { t_ctab[RootParameterIndex] = BaseDescriptor.ptr; t_cKind[RootParameterIndex] = 1; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRootDescriptorTable(
    BridgeCommandList* self,
    UINT RootParameterIndex,
    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    static volatile LONG s_gTableLogCount = 0;
    if (InterlockedIncrement(&s_gTableLogCount) <= 20) {
        void* retAddr = _ReturnAddress();
        HMODULE hExe = GetModuleHandleW(nullptr);
        uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
        log_line("[d3d12_x] SetGraphicsRootDescriptorTable: self=%p, slot=%u, BaseDescriptor=0x%llX, caller=game+0x%llX\r\n",
                 self, RootParameterIndex, (unsigned long long)BaseDescriptor.ptr, (unsigned long long)rva);
    }
    if (g_drawDetail && InterlockedIncrement(&g_drawDetailCount) <= 400)
        DescTrack_LogTable("gfx", RootParameterIndex, BaseDescriptor.ptr, 6);
    FixRawDescriptors(BaseDescriptor.ptr, t_gfxRs, RootParameterIndex);
    if (RootParameterIndex < 16) t_gtab[RootParameterIndex] = BaseDescriptor.ptr;
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
}

static void STDMETHODCALLTYPE BridgeCmdList_Slot29_XboxStub(
    BridgeCommandList* self,
    void* a1,
    UINT a2,
    UINT a3)
{
    // Durango ID3D12CommandListX fast constant / cache commit stub (slot 29 / 0xE8, game+0xB595A)
    // Safely absorbed on desktop D3D12 to prevent root signature corruption and command list invalidation.
    static volatile LONG s_s29Log = 0;
    if (InterlockedIncrement(&s_s29Log) <= 10) {
        log_line("[d3d12_x] BridgeCmdList_Slot29_XboxStub called: self=%p, a1=%p, a2=%u, a3=%u (safely absorbed)\r\n",
                 self, a1, a2, a3);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_Slot30_XboxTileDecompress(
    BridgeCommandList* self,
    void* a1,
    void* a2,
    void* a3,
    void* a4,
    void* a5,
    void* a6)
{
    // Durango Xbox hardware tile decompression / cache maintenance extension (slot 30 / 0xF0).
    // Safely handled by desktop GPU driver; return cleanly without crashing.
}

static void STDMETHODCALLTYPE BridgeCmdList_SmartSlot32(
    BridgeCommandList* self,
    UINT arg1,
    void* arg2)
{
    if (!self || !self->m_pRealCmdList) return;

    // Distinguish between SetDescriptorHeaps(NumHeaps, ppHeaps) and SetGraphicsRootDescriptorTable(RootParamIndex, BaseDescriptor)
    // On Durango / OnRush, Slot 32 (+0x100) is SetDescriptorHeaps where arg1 = NumDescriptorHeaps (1 or 2),
    // and arg2 is a host memory pointer to the heap pointer array on stack or data section.
    bool isSetDescriptorHeaps = false;
    if (arg1 >= 1 && arg1 <= 4 && (uintptr_t)arg2 > 0x10000) {
        __try {
            void* firstHeap = *(void**)arg2;
            if ((uintptr_t)firstHeap > 0x10000) {
                isSetDescriptorHeaps = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            isSetDescriptorHeaps = false;
        }
    }

    if (isSetDescriptorHeaps) {
        BridgeCmdList_SetDescriptorHeaps(self, arg1, (ID3D12DescriptorHeap* const*)arg2);
    } else {
        D3D12_GPU_DESCRIPTOR_HANDLE handle;
        handle.ptr = (UINT64)arg2;
        BridgeCmdList_SetGraphicsRootDescriptorTable(self, arg1, handle);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRoot32BitConstant(
    BridgeCommandList* self, UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    if (RootParameterIndex < 16 && DestOffsetIn32BitValues < 32) { t_cConst[RootParameterIndex][DestOffsetIn32BitValues] = SrcData; t_cKind[RootParameterIndex] = 5; if (t_cConstN[RootParameterIndex] < DestOffsetIn32BitValues + 1) t_cConstN[RootParameterIndex] = DestOffsetIn32BitValues + 1; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRoot32BitConstant(
    BridgeCommandList* self, UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    if (self && self->m_pRealCmdList)
        if (RootParameterIndex == 0 && DestOffsetIn32BitValues < 8) t_rc0[DestOffsetIn32BitValues] = SrcData;
        self->m_pRealCmdList->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRoot32BitConstants(
    BridgeCommandList* self, UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)
{
    if (RootParameterIndex < 16 && pSrcData) { for (UINT k = 0; k < Num32BitValuesToSet && DestOffsetIn32BitValues + k < 32; ++k) t_cConst[RootParameterIndex][DestOffsetIn32BitValues + k] = ((const UINT*)pSrcData)[k]; t_cKind[RootParameterIndex] = 5; { UINT e = DestOffsetIn32BitValues + Num32BitValuesToSet; if (e > 32) e = 32; if (t_cConstN[RootParameterIndex] < e) t_cConstN[RootParameterIndex] = e; } }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRoot32BitConstants(
    BridgeCommandList* self, UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)
{
    if (self && self->m_pRealCmdList)
        if (RootParameterIndex == 0 && pSrcData) for (UINT k = 0; k < Num32BitValuesToSet && DestOffsetIn32BitValues + k < 8; ++k) t_rc0[DestOffsetIn32BitValues + k] = ((const UINT*)pSrcData)[k];
        self->m_pRealCmdList->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRootConstantBufferView(
    BridgeCommandList* self, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (RootParameterIndex < 16) { t_ccbv[RootParameterIndex] = BufferLocation; t_cKind[RootParameterIndex] = 2; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRootConstantBufferView(
    BridgeCommandList* self, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (self && self->m_pRealCmdList)
        if (RootParameterIndex < 16) t_rootCbv[RootParameterIndex] = BufferLocation;
        self->m_pRealCmdList->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRootShaderResourceView(
    BridgeCommandList* self, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (RootParameterIndex < 16) { t_cVa[RootParameterIndex] = BufferLocation; t_cKind[RootParameterIndex] = 3; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRootShaderResourceView(
    BridgeCommandList* self, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetComputeRootUnorderedAccessView(
    BridgeCommandList* self, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    { static const bool s_logAll = GetEnvironmentVariableA("ONRUSH_LOG_DISPATCH", nullptr, 0) > 0; if (s_logAll) log_line("[ruav] p=%u va=%llX pso=%p\n", RootParameterIndex, (unsigned long long)BufferLocation, t_lastPso); }
    if (RootParameterIndex < 16) { t_cVa[RootParameterIndex] = BufferLocation; t_cKind[RootParameterIndex] = 4; }
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
}

static void STDMETHODCALLTYPE BridgeCmdList_SetGraphicsRootUnorderedAccessView(
    BridgeCommandList* self, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
}

static void STDMETHODCALLTYPE BridgeCmdList_IASetIndexBuffer(
    BridgeCommandList* self, const D3D12_INDEX_BUFFER_VIEW* pView)
{
    if (self && self->m_pRealCmdList)
    {
        if (pView && (uintptr_t)pView <= 0x10000) pView = nullptr;
        if (pView) t_ib = *pView; else t_ib = {};
        self->m_pRealCmdList->IASetIndexBuffer(pView);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_IASetVertexBuffers(
    BridgeCommandList* self, UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews)
{
    if (self && self->m_pRealCmdList)
    {
        if (NumViews > 0 && (!pViews || (uintptr_t)pViews <= 0x10000)) return;
        if (NumViews > 16) NumViews = 16;
        for (UINT k = 0; k < NumViews; ++k) if (StartSlot + k < 4) t_vb[StartSlot + k] = pViews[k];
        self->m_pRealCmdList->IASetVertexBuffers(StartSlot, NumViews, pViews);
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_SOSetTargets(
    BridgeCommandList* self, UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews)
{
    if (self && self->m_pRealCmdList)
        self->m_pRealCmdList->SOSetTargets(StartSlot, NumViews, pViews);
}

static void STDMETHODCALLTYPE BridgeCmdList_OMSetRenderTargets(
    BridgeCommandList* self, UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    if (!self || !self->m_pRealCmdList) return;

    if (NumRenderTargetDescriptors > 0) {
        if (!pRenderTargetDescriptors || (uintptr_t)pRenderTargetDescriptors <= 0x10000) {
            NumRenderTargetDescriptors = 0;
            pRenderTargetDescriptors = nullptr;
        } else {
            if (NumRenderTargetDescriptors > 8) NumRenderTargetDescriptors = 8;
            if (!pRenderTargetDescriptors[0].ptr || pRenderTargetDescriptors[0].ptr <= 0x10000) {
                NumRenderTargetDescriptors = 0;
                pRenderTargetDescriptors = nullptr;
            }
        }
    }

    if (NumRenderTargetDescriptors > 0 && pRenderTargetDescriptors && (uintptr_t)pRenderTargetDescriptors > 0x10000) {
        if (pRenderTargetDescriptors[0].ptr > 0x10000) {
            self->m_pActiveRenderTarget = LookupRtvResource(pRenderTargetDescriptors[0].ptr);
            if (!RTsSingleHandleToDescriptorRange) {
                for (UINT ri = 0; ri < NumRenderTargetDescriptors; ++ri) {
                    ID3D12Resource* rtRes = LookupRtvResource(pRenderTargetDescriptors[ri].ptr);
                    if (rtRes) ListEnsureState(self, rtRes, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else if (self->m_pActiveRenderTarget) {
                // One handle + count: the RTVs are consecutive in the heap. Transition every one of
                // them (only RT0 was handled, leaving the G-buffer's normal/material targets in the
                // wrong state).
                static UINT s_rtvInc = 0;
                if (!s_rtvInc && g_pMainDevice) s_rtvInc = g_pMainDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                for (UINT ri = 0; ri < NumRenderTargetDescriptors; ++ri) {
                    ID3D12Resource* rtRes = ri ? LookupRtvResource(pRenderTargetDescriptors[0].ptr + (SIZE_T)ri * s_rtvInc) : self->m_pActiveRenderTarget;
                    if (rtRes) { ListEnsureState(self, rtRes, D3D12_RESOURCE_STATE_RENDER_TARGET); if (ri) CountRtDraw(rtRes); }
                }
            }
            if (!RTsSingleHandleToDescriptorRange)
                for (UINT ri = 1; ri < NumRenderTargetDescriptors; ++ri) CountRtDraw(LookupRtvResource(pRenderTargetDescriptors[ri].ptr));
            static volatile LONG s_omLog = 0;
            if (InterlockedIncrement(&s_omLog) <= 60) {
                log_line("[d3d12_x] OMSetRenderTargets: count=%u, handle[0]=0x%llX -> res=%p\r\n",
                         NumRenderTargetDescriptors, (unsigned long long)pRenderTargetDescriptors[0].ptr, self->m_pActiveRenderTarget);
            }
        } else {
            self->m_pActiveRenderTarget = nullptr;
        }
    } else {
        self->m_pActiveRenderTarget = nullptr;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE* pDS = nullptr;
    if (pDepthStencilDescriptor && (uintptr_t)pDepthStencilDescriptor > 0x10000 && pDepthStencilDescriptor->ptr > 0x10000) {
        pDS = pDepthStencilDescriptor;
    }

    t_om = { (void*)self, NumRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, (NumRenderTargetDescriptors > 0 && pRenderTargetDescriptors) ? (unsigned long long)pRenderTargetDescriptors[0].ptr : 0ull,
             (NumRenderTargetDescriptors > 1 && pRenderTargetDescriptors) ? (unsigned long long)pRenderTargetDescriptors[1].ptr : 0ull, (NumRenderTargetDescriptors > 2 && pRenderTargetDescriptors) ? (unsigned long long)pRenderTargetDescriptors[2].ptr : 0ull, pDS ? (unsigned long long)pDS->ptr : 0ull };
    self->m_pRealCmdList->OMSetRenderTargets(
        NumRenderTargetDescriptors,
        pRenderTargetDescriptors,
        RTsSingleHandleToDescriptorRange,
        pDS);
}

static void STDMETHODCALLTYPE BridgeCmdList_PipelineBarrier(
    BridgeCommandList* self, UINT Flags, UINT64 a1, UINT64 a2)
{
    // Durango ID3D12CommandListX::PipelineBarrier (slot 67 / 0x218)
    // Pipeline synchronization is handled via ResourceBarrier on PC D3D12.
}

static void STDMETHODCALLTYPE BridgeCmdList_ClearDepthStencilView(
    BridgeCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
    FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects)
{
    if (!self || !self->m_pRealCmdList || !DepthStencilView.ptr || DepthStencilView.ptr <= 0x10000)
        return;
    __try {
        self->m_pRealCmdList->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_ClearRenderTargetView(
    BridgeCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT* ColorRGBA,
    UINT NumRects, const D3D12_RECT* pRects)
{
    CMD_STAT(STAT_CLEARRTV);
    if (!self || !self->m_pRealCmdList || !RenderTargetView.ptr || RenderTargetView.ptr <= 0x10000)
        return;

    ID3D12Resource* pRes = LookupRtvResource(RenderTargetView.ptr);
    if (pRes) {
        self->m_pActiveRenderTarget = pRes;
        RecordLastRenderedTarget(pRes);

        ListEnsureState(self, pRes, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    static const FLOAT s_defaultColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const FLOAT* pColorToUse = s_defaultColor;
    if ((uintptr_t)ColorRGBA > 0x10000) {
        pColorToUse = ColorRGBA;
    }

    __try {
        self->m_pRealCmdList->ClearRenderTargetView(RenderTargetView, pColorToUse, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Logs the first clears/discards per resource (ONRUSH_LOG_CLEARS=1); ONRUSH_NO_DISCARD=1 skips DiscardResource;
// ONRUSH_CLEAR_FULL=1 restores the old behaviour of ignoring clear rects.
static bool IsAoRes(ID3D12Resource* r) { if (!r) return false; for (int i = 0; i < 6; ++i) if (g_aoRes[i] == r) return true; return false; }
static void WatchAo(const char* what, ID3D12Resource* r) {
    static const bool s_on = GetEnvironmentVariableA("ONRUSH_WATCH_AO", nullptr, 0) > 0; static volatile LONG s_n = 0;
    if (s_on && IsAoRes(r) && InterlockedIncrement(&s_n) <= 300) log_line("[watchao] f%ld %s res=%p\r\n", (long)g_rtLogFrame, what, (void*)r);
}
static void LogClearOnce(const char* what, ID3D12Resource* r, UINT nRects) {
    WatchAo(what, r);
    static const bool s_on = GetEnvironmentVariableA("ONRUSH_LOG_CLEARS", nullptr, 0) > 0;
    if (!s_on || !r) return;
    static void* s_seen[512]; static volatile LONG s_n = 0; LONG n = s_n;
    for (LONG i = 0; i < n && i < 512; ++i) if (s_seen[i] == r) return;
    LONG id = InterlockedIncrement(&s_n) - 1; if (id < 512) s_seen[id] = r;
    D3D12_RESOURCE_DESC d = {}; __try { d = r->GetDesc(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    log_line("[clear] %s res=%p dim=%d %llux%ux%u fmt=%d rects=%u\r\n", what, (void*)r, (int)d.Dimension, (unsigned long long)d.Width, d.Height, (unsigned)d.DepthOrArraySize, (int)d.Format, nRects);
}
static void STDMETHODCALLTYPE BridgeCmdList_ClearUnorderedAccessViewUint(
    BridgeCommandList* self,
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
    ID3D12Resource* pResource,
    const UINT Values[4],
    UINT NumRects,
    const D3D12_RECT* pRects)
{
    if (self && self->m_pRealCmdList && pResource && (uintptr_t)pResource > 0x10000)
    {
        __try {
            static const bool s_full = GetEnvironmentVariableA("ONRUSH_CLEAR_FULL", nullptr, 0) > 0; LogClearOnce("uavu", pResource, NumRects);
            self->m_pRealCmdList->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, s_full ? 0 : NumRects, s_full ? nullptr : pRects);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_ClearUnorderedAccessViewFloat(
    BridgeCommandList* self,
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
    ID3D12Resource* pResource,
    const FLOAT Values[4],
    UINT NumRects,
    const D3D12_RECT* pRects)
{
    if (self && self->m_pRealCmdList && pResource && (uintptr_t)pResource > 0x10000)
    {
        __try {
            static const bool s_full = GetEnvironmentVariableA("ONRUSH_CLEAR_FULL", nullptr, 0) > 0; LogClearOnce("uavf", pResource, NumRects);
            self->m_pRealCmdList->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, s_full ? 0 : NumRects, s_full ? nullptr : pRects);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void STDMETHODCALLTYPE BridgeCmdList_DiscardResource(
    BridgeCommandList* self,
    ID3D12Resource* pResource,
    const D3D12_DISCARD_REGION* pRegion)
{
    if (self && self->m_pRealCmdList && pResource && (uintptr_t)pResource > 0x10000) {
        LogClearOnce("discard", pResource, pRegion ? pRegion->NumRects : 0);
        static const bool s_nodisc = GetEnvironmentVariableA("ONRUSH_NO_DISCARD", nullptr, 0) > 0;
        if (!s_nodisc) self->m_pRealCmdList->DiscardResource(pResource, pRegion);
    }
}

// Durango extended copy (slot 84 / 0x2A0): (dst, src, flags 0 / 0x80). Was an unmapped stub that silently
// dropped ~25 copies per frame. Implemented as CopyResource with explicit state handling.
static void STDMETHODCALLTYPE BridgeCmdList_Slot84_CopyResourceX(
    BridgeCommandList* self, ID3D12Resource* pDst, ID3D12Resource* pSrc, UINT flags)
{
    if (!self || !self->m_pRealCmdList) return;
    if ((uintptr_t)pDst <= 0x10000 || (uintptr_t)pSrc <= 0x10000) return;
    D3D12_RESOURCE_DESC dd = {}, sd = {};
    if (!SafeGetResourceDesc(pDst, &dd) || !SafeGetResourceDesc(pSrc, &sd)) return;
    static volatile LONG s_log = 0;
    LONG n = InterlockedIncrement(&s_log);
    if (n <= 16)
        log_line("[copyX] dst=%p %llux%u fmt=%d samples=%u | src=%p %llux%u fmt=%d samples=%u flags=0x%X\r\n",
                 pDst, (unsigned long long)dd.Width, dd.Height, (int)dd.Format, dd.SampleDesc.Count,
                 pSrc, (unsigned long long)sd.Width, sd.Height, (int)sd.Format, sd.SampleDesc.Count, flags);
    if (pDst == pSrc || IsUploadResource(pDst) || IsStagingBuffer(pDst)) return;
    D3D12_RESOURCE_STATES dBefore = GetTrackedResourceState(pDst, D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_STATES sBefore = GetTrackedResourceState(pSrc, D3D12_RESOURCE_STATE_COMMON);
    ListEnsureState(self, pDst, D3D12_RESOURCE_STATE_COPY_DEST);
    ListEnsureState(self, pSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
    __try {
        const bool srcD32S8 = sd.Format >= DXGI_FORMAT_R32G8X24_TYPELESS && sd.Format <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        const bool dstR32 = dd.Format >= DXGI_FORMAT_R32_TYPELESS && dd.Format <= DXGI_FORMAT_R32_SINT;
        if (sd.SampleDesc.Count > 1 && dd.SampleDesc.Count == 1) {
            self->m_pRealCmdList->ResolveSubresource(pDst, 0, pSrc, 0, dd.Format);
        } else if (srcD32S8 && dstR32 && dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            // depth plane (plane 0 of D32_S8X24) -> R32 texture: CopyResource rejects the format mismatch.
            D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
            dl.pResource = pDst; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
            sl.pResource = pSrc; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
            self->m_pRealCmdList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        } else if (dd.Dimension == sd.Dimension && dd.Width == sd.Width && dd.Height == sd.Height &&
                   dd.DepthOrArraySize == sd.DepthOrArraySize && dd.MipLevels == sd.MipLevels) {
            self->m_pRealCmdList->CopyResource(pDst, pSrc);
        } else if (dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
            dl.pResource = pDst; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
            sl.pResource = pSrc; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
            D3D12_BOX box = { 0, 0, 0, (UINT)(dd.Width < sd.Width ? dd.Width : sd.Width), (dd.Height < sd.Height ? dd.Height : sd.Height), 1 };
            self->m_pRealCmdList->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Durango has no states: put the resources back where this list found them.
    ListEnsureState(self, pDst, dBefore);
    ListEnsureState(self, pSrc, sBefore);
}

// ---- indirect-dispatch guard ----
// GPU-built dispatch arguments can be garbage: a particle counter that drops below zero turns into
// (count + 63) / 64 = 0x03FFFFFB thread groups, which never finishes and hangs the compute queue (the
// game then spins forever on its frame fence). D3D12 caps each dimension at 65535, so any argument with
// a non-zero upper half is invalid. Before the ExecuteIndirect we copy the upper 16 bits of X, Y and Z
// into a per-list 8-byte predicate and enable predication NOT_EQUAL_ZERO: the GPU drops the dispatch
// by itself when any dimension is out of range. No shader, so the game's compute state is untouched.
// Disable with ONRUSH_NO_XDISPATCH_GUARD=1.
int LookupSigDispatchOffset(void* sig);
static ID3D12Resource* g_guardZero = nullptr;  // 16 zero bytes in an upload heap (always GENERIC_READ)
static std::unordered_map<BridgeCommandList*, ID3D12Resource*> g_guardPred; static SRWLOCK g_guardLock = SRWLOCK_INIT;
static ID3D12Resource* XDispatchGuardBegin(BridgeCommandList* self, ID3D12CommandSignature* sig, UINT maxc,
                                           ID3D12Resource* arg, UINT64 aoff, ID3D12Resource* cnt) {
    static const bool s_off = GetEnvironmentVariableA("ONRUSH_NO_XDISPATCH_GUARD", nullptr, 0) > 0;
    if (s_off || maxc != 1 || cnt || !self || !self->m_pRealCmdList || self->m_type == D3D12_COMMAND_LIST_TYPE_COPY) return nullptr;
    int doff = LookupSigDispatchOffset(sig);
    const bool isDraw = doff >= 0 && (doff & 0x10000);
    if (isDraw) {
        static const bool s_drawOff = GetEnvironmentVariableA("ONRUSH_NO_XDRAW_GUARD", nullptr, 0) > 0;
        if (s_drawOff) return nullptr;
        doff &= 0xFFFF;
    }
    if (doff < 0 || !g_pMainDevice || !g_TrueCreateCommittedResource) return nullptr;
    D3D12_RESOURCE_DESC ad = {};
    if (!SafeGetResourceDesc(arg, &ad) || aoff + (UINT64)doff + 12 > ad.Width) return nullptr;
    ID3D12Resource* pred = nullptr;
    AcquireSRWLockExclusive(&g_guardLock);
    if (!g_guardZero) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* z = nullptr; void* m = nullptr;
        if (SUCCEEDED(g_TrueCreateCommittedResource(g_pMainDevice, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&z)) && z &&
            SUCCEEDED(z->Map(0, nullptr, &m)) && m) { memset(m, 0, 256); z->Unmap(0, nullptr); g_guardZero = z; }
    }
    auto it = g_guardPred.find(self);
    if (it != g_guardPred.end()) pred = it->second;
    else if (g_guardZero) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_TrueCreateCommittedResource(g_pMainDevice, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&pred))) pred = nullptr;
        g_guardPred[self] = pred;
        static volatile LONG s_n = 0;
        if (InterlockedIncrement(&s_n) <= 4) log_line("[xguard] predicate buffer %p for list %p (type %d)\r\n", (void*)pred, (void*)self, (int)self->m_type);
    }
    ReleaseSRWLockExclusive(&g_guardLock);
    if (!pred) return nullptr;
    ID3D12GraphicsCommandList* rl = self->m_pRealCmdList;
    ListEnsureState(self, arg, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE));
    ListEnsureState(self, pred, D3D12_RESOURCE_STATE_COPY_DEST);
    // bytes 0..5 = high halves of X, Y, Z; bytes 6..7 = 0 (four non-overlapping copies, no ordering needed)
    if (!isDraw) {
        for (int d = 0; d < 3; ++d) rl->CopyBufferRegion(pred, (UINT64)d * 2, arg, aoff + (UINT64)doff + (UINT64)d * 4 + 2, 2);
        rl->CopyBufferRegion(pred, 6, g_guardZero, 0, 2);
    } else {
        // draw: drop it when InstanceCount >= 65536 (bytes 0..1) or Vertex/IndexCount >= 2^24 (byte 2); bytes 3..7 = 0.
        // A negative instance count (end < begin from a broken counter) otherwise draws ~4 billion instances and hangs the GPU.
        rl->CopyBufferRegion(pred, 0, arg, aoff + (UINT64)doff + 6, 2);
        rl->CopyBufferRegion(pred, 2, arg, aoff + (UINT64)doff + 3, 1);
        rl->CopyBufferRegion(pred, 3, g_guardZero, 0, 5);
    }
    ListEnsureState(self, pred, D3D12_RESOURCE_STATE_PREDICATION);
    rl->SetPredication(pred, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    return pred;
}

// Breadcrumb mode: snapshot the 64 argument bytes each ExecuteIndirect reads (printed for in-flight ops on a hang).
static void BcCopyArgs(BridgeCommandList* self, LONG slot, ID3D12Resource* arg, UINT64 aoff, ID3D12Resource* cnt, UINT64 coff) {
    if (!g_bcArgs) {
        static SRWLOCK s_lk = SRWLOCK_INIT; static bool s_tried = false;
        AcquireSRWLockExclusive(&s_lk);
        if (!s_tried) {
            s_tried = true;
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_CUSTOM; hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kBcSlots * 64; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* r = nullptr; void* m = nullptr;
            if (SUCCEEDED(g_TrueCreateCommittedResource(g_pMainDevice, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&r)) && r &&
                SUCCEEDED(r->Map(0, nullptr, &m)) && m) { memset(m, 0xCD, (size_t)kBcSlots * 64); g_bcArgsMem = (volatile UINT*)m; g_bcArgs = r; }
        }
        ReleaseSRWLockExclusive(&s_lk);
        if (!g_bcArgs) return;
    }
    D3D12_RESOURCE_DESC d = {};
    if (!SafeGetResourceDesc(arg, &d) || aoff >= d.Width) return;
    UINT64 n = d.Width - aoff; if (n > 56) n = 56;
    for (int i = 0; i < 16; ++i) g_bcArgsMem[(size_t)slot * 16 + i] = 0xCDCDCDCDu;
    ListEnsureState(self, arg, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE));
    self->m_pRealCmdList->CopyBufferRegion(g_bcArgs, (UINT64)slot * 64, arg, aoff, n);
    g_bcRes[slot] = { arg, aoff, nullptr, 0 };
    if (cnt) {   // count buffer value -> bytes 60..63 of the snapshot
        D3D12_RESOURCE_DESC cd = {};
        if (SafeGetResourceDesc(cnt, &cd) && coff + 4 <= cd.Width) {
            if (cnt != arg) ListEnsureState(self, cnt, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE));
            self->m_pRealCmdList->CopyBufferRegion(g_bcArgs, (UINT64)slot * 64 + 60, cnt, coff, 4);
            g_bcRes[slot].cnt = cnt; g_bcRes[slot].coff = coff;
        }
    }
}

// ONRUSH_BC_ARGDUMP_FRAME=N (needs ONRUSH_BREADCRUMBS=1): logs every ExecuteIndirect recorded during frame N with the
// argument bytes the GPU actually read ([bcx] lines), a few frames later once the GPU has finished them.
static void BcArgDumpTick(long frame) {
    static long s_at = -2; static LONG s_from = 0, s_to = 0;
    if (s_at == -2) { char fb[16] = {}; s_at = GetEnvironmentVariableA("ONRUSH_BC_ARGDUMP_FRAME", fb, sizeof(fb)) > 0 ? atol(fb) : -1; }
    if (s_at < 0 || !g_bcBuf || !g_bcArgsMem) return;
    if (frame == s_at) { s_from = (g_bcNext & 0x7FFFFFFF) + 1; g_bcArgDumpWindow = true; }
    else if (frame == s_at + 1) { g_bcArgDumpWindow = false; s_to = g_bcNext & 0x7FFFFFFF; }
    else if (frame == s_at + 6 && s_to >= s_from) {
        log_line("[bcx] ExecuteIndirect ops %ld..%ld recorded in frame %ld\r\n", s_from, s_to, s_at);
        for (LONG id = s_from; id <= s_to; ++id) {
            LONG slot = id % kBcSlots; const BcInfo& in = g_bcInfo[slot];
            if (in.id != id) continue;
            if (in.kind == 'D') {
                log_line("[bcx] op %ld list=%d DISPATCH pso=%p sh=%016llx groups=%u,%u,%u%s\r\n", id, in.listType, in.pso,
                         (unsigned long long)BcPsoHash(in.pso), in.a, in.b, in.c, g_bcMem[slot] == ((UINT)id | 0x80000000u) ? "" : " NOTDONE");
                continue;
            }
            if (in.kind == 'C') {
                const volatile UINT* a = g_bcArgsMem + (size_t)slot * 16; const BcRes& r = g_bcRes[slot];
                D3D12_RESOURCE_DESC dd = {}, sd = {}; SafeGetResourceDesc(r.arg, &dd); SafeGetResourceDesc(r.cnt, &sd);
                log_line("[bcx] op %ld list=%d COPY n=%u dst=%p(w=%llu)+%llu src=%p(w=%llu)+%llu%s | %08X %08X %08X %08X %08X %08X\r\n", id, in.listType, in.a,
                         (void*)r.arg, (unsigned long long)dd.Width, (unsigned long long)r.aoff, (void*)r.cnt, (unsigned long long)sd.Width, (unsigned long long)r.coff,
                         g_bcMem[slot] == ((UINT)id | 0x80000000u) ? "" : " NOTDONE", a[0], a[1], a[2], a[3], a[4], a[5]);
                continue;
            }
            if (in.kind != 'X') continue;
            const volatile UINT* a = g_bcArgsMem + (size_t)slot * 16; const BcRes& r = g_bcRes[slot];
            bool done = g_bcMem[slot] == ((UINT)id | 0x80000000u);
            log_line("[bcx] op %ld list=%d sig=%u max=%u pso=%p sh=%016llx arg=%p+%llu cnt=%p+%llu%s | %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X | count=%08X\r\n",
                     id, in.listType, in.a, in.b, in.pso, (unsigned long long)BcPsoHash(in.pso), (void*)r.arg, (unsigned long long)r.aoff,
                     (void*)r.cnt, (unsigned long long)r.coff, done ? "" : " NOTDONE",
                     a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[15]);
        }
    }
}

struct XProbe { int sigKind; ID3D12Resource* arg; UINT64 aoff; ID3D12Resource* cnt; UINT64 coff; UINT maxc; ID3D12CommandSignature* sig; ID3D12Resource* rt; };
static XProbe g_xProbe[16]; static volatile LONG g_xProbeCount = 0;
static void ExecIndirectScoped(BridgeCommandList* self, ID3D12CommandSignature* sig, UINT maxc, ID3D12Resource* arg, UINT64 aoff, ID3D12Resource* cnt, UINT64 coff)
{
    if (g_drawDetail && (((LookupSigKind(sig) / 1000) == 2 || (LookupSigKind(sig) / 1000) == 105) == (GetEnvironmentVariableA("ONRUSH_XPROBE_DISPATCH", nullptr, 0) > 0))) { static volatile LONG s_xp = 0; LONG ix = InterlockedIncrement(&s_xp) - 1; if (ix < 16) { g_xProbe[ix] = { LookupSigKind(sig), arg, aoff, cnt, coff, maxc, sig, (self->m_pActiveRenderTarget ? self->m_pActiveRenderTarget : LookupRtvResource((SIZE_T)t_om.h0)) }; g_xProbeCount = ix + 1; } }
    { static long s_la = -2; if (s_la == -2) { char lb[16] = {}; s_la = GetEnvironmentVariableA("ONRUSH_LOG_DISPATCH", lb, sizeof(lb)) > 0 ? atol(lb) : -1; }
      extern volatile LONG g_rtLogFrame;
      if (s_la > 1 && g_rtLogFrame == s_la) log_line("%s pso=%p sh=%016llx list=%d sig=%d max=%u arg=%p+%llu\r\n", "[dispall] X", t_lastPso, (unsigned long long)BcPsoHash(t_lastPso), self ? (int)self->m_type : -1, LookupSigKind(sig), maxc, (void*)arg, (unsigned long long)aoff); }
    PsQueryScope psqx(self, 'X', maxc);
    if ((LookupSigKind(sig) / 1000) <= 1) { LogDrawOnRtWidth(self, 0xFFFFFFFFu, maxc); if (SkipDrawByShader()) return; }
    LONG bc = BcBegin(self, 'X', (UINT)LookupSigKind(sig), maxc, (UINT)aoff);
    if (bc >= 0) BcCopyArgs(self, bc, arg, aoff, cnt, coff);
    ID3D12Resource* guard = XDispatchGuardBegin(self, sig, maxc, arg, aoff, cnt);
    self->m_pRealCmdList->ExecuteIndirect(sig, maxc, arg, aoff, cnt, coff);
    if (guard) self->m_pRealCmdList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    BcEnd(self, bc);
}

// Indirect dispatches by compute-shader hash: logged once each ([xdisp]); ONRUSH_SKIP_XDISPATCH_CS=h1,h2 skips them.
static bool XDispatchSkipByHash(void* pso, ID3D12CommandSignature* sig, UINT maxCount, int listType)
{
    static std::unordered_map<void*, uint64_t> s_psoHash; static SRWLOCK s_lk = SRWLOCK_INIT;
    static char s_skipList[16384] = {}; static int s_init = 0;
    if (!s_init) { s_init = 1; GetEnvironmentVariableA("ONRUSH_SKIP_XDISPATCH_CS", s_skipList, sizeof(s_skipList)); }
    uint64_t h = 0; bool known = false;
    AcquireSRWLockShared(&s_lk); auto it = s_psoHash.find(pso); if (it != s_psoHash.end()) { h = it->second; known = true; } ReleaseSRWLockShared(&s_lk);
    if (!known) {
        AcquireSRWLockShared(&g_PsoShaderLock);
        auto pit = g_PsoShaderMap.find(pso);
        if (pit != g_PsoShaderMap.end() && !pit->second.csBytecode.empty()) h = Fnv1a64Fwd(pit->second.csBytecode.data(), pit->second.csBytecode.size());
        ReleaseSRWLockShared(&g_PsoShaderLock);
        AcquireSRWLockExclusive(&s_lk); s_psoHash[pso] = h; ReleaseSRWLockExclusive(&s_lk);
        log_line("[xdisp] pso=%p cs=%016llx sigkind=%d max=%u list type=%d\r\n", pso, (unsigned long long)h, LookupSigKind(sig), maxCount, listType);
    }
    if (h && s_skipList[0]) { char hx[20]; sprintf_s(hx, "%016llx", (unsigned long long)h); if (strstr(s_skipList, hx)) return true; }
    return false;
}

static void STDMETHODCALLTYPE BridgeCmdList_ExecuteIndirect(
    BridgeCommandList* self, ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
    ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
    ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset)
{
    if (!self || !self->m_pRealCmdList) return;

    void* retAddr = _ReturnAddress();
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t rva = (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

    static volatile LONG s_execCount = 0;
    if (InterlockedIncrement(&s_execCount) <= 30) {
        log_line("[d3d12_x] BridgeCmdList_ExecuteIndirect: self=%p, real=%p, type=%d, sig=%p, maxCount=%u, argBuf=%p, offset=%llu, countBuf=%p, countOffset=%llu, caller=game+0x%llX\r\n",
                 self, self->m_pRealCmdList, (int)self->m_type, pCommandSignature, MaxCommandCount, pArgumentBuffer,
                 (unsigned long long)ArgumentBufferOffset, pCountBuffer, (unsigned long long)CountBufferOffset,
                 (unsigned long long)rva);
    }

    // Bypass calls from gputiming (profiling/timestamp readback) which do not pass standard PC indirect arguments
    if (rva >= 0xBECE8 && rva <= 0xBEFD0) {
        return;
    }

    // Console Durango ABI detection: if offset is passed in param 4 (r9) and buffer in stack slot 6 (pCountBuffer)
    if ((uintptr_t)pArgumentBuffer <= 0x10000 && (uintptr_t)pCountBuffer > 0x10000) {
        ID3D12Resource* pRealArgBuffer = pCountBuffer;
        UINT64 realOffset = (UINT64)(uintptr_t)pArgumentBuffer;
        pArgumentBuffer = pRealArgBuffer;
        ArgumentBufferOffset = realOffset;
        pCountBuffer = nullptr;
        CountBufferOffset = 0;
    }

    // Safety checks: do NOT pass null/invalid signature or buffer to PC D3D12 runtime, as it invalidates the command list
    if (!pCommandSignature || (uintptr_t)pCommandSignature <= 0x10000) {
        return;
    }
    if (!pArgumentBuffer || (uintptr_t)pArgumentBuffer <= 0x10000) {
        return;
    }
    if (MaxCommandCount == 0) {
        return;
    }

    D3D12_RESOURCE_DESC argDesc = {};
    if (!SafeGetResourceDesc(pArgumentBuffer, &argDesc) || argDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return;
    }

    if (pCountBuffer) {
        if ((uintptr_t)pCountBuffer <= 0x10000) {
            pCountBuffer = nullptr;
            CountBufferOffset = 0;
        } else {
            D3D12_RESOURCE_DESC countDesc = {};
            if (!SafeGetResourceDesc(pCountBuffer, &countDesc) || countDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                pCountBuffer = nullptr;
                CountBufferOffset = 0;
            }
        }
    }

    {   // Diagnostics: ONRUSH_SKIP_XDISPATCH=1 / ONRUSH_SKIP_XDRAW=1 drop indirect dispatches / draws.
        static const bool s_skipD = GetEnvironmentVariableA("ONRUSH_SKIP_XDISPATCH", nullptr, 0) > 0;
        static const bool s_skipG = GetEnvironmentVariableA("ONRUSH_SKIP_XDRAW", nullptr, 0) > 0;
        const int k = LookupSigKind(pCommandSignature) / 1000;
        const bool isDispatch = (k == 2 || k == 105);
        if ((isDispatch && s_skipD) || (!isDispatch && s_skipG)) return;
        if (isDispatch && t_lastPso && XDispatchSkipByHash(t_lastPso, pCommandSignature, MaxCommandCount, (int)self->m_type)) return;
    }
    __try {
        if (self->m_pActiveRenderTarget) {
            RecordLastRenderedTarget(self->m_pActiveRenderTarget);
        }
        CMD_STAT(STAT_EXECIND);
        ExecIndirectScoped(self, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[d3d12_x] Exception in RealCmdList->ExecuteIndirect (sig=%p, count=%u, argBuf=%p)\r\n",
                 pCommandSignature, MaxCommandCount, pArgumentBuffer);
    }
}

// ----------------------------------------------------------------------------
// Specialized Global Vtables
// ----------------------------------------------------------------------------

inline void* g_BridgeDirectCommandListVtable[kDurangoCmdListSlots] = {};
inline void* g_BridgeComputeCommandListVtable[kDurangoCmdListSlots] = {};
inline void* g_BridgeCopyCommandListVtable[kDurangoCmdListSlots] = {};
inline std::once_flag g_BridgeVtablesOnce;

inline void InitBridgeCommandListVtables()
{
    std::call_once(g_BridgeVtablesOnce, []() {
        // 1. Inicializa todos os slots com stubs diagnósticos seguros
        InitCmdListStubs(g_BridgeDirectCommandListVtable, std::make_integer_sequence<int, kDurangoCmdListSlots>());
        InitCmdListStubs(g_BridgeComputeCommandListVtable, std::make_integer_sequence<int, kDurangoCmdListSlots>());
        InitCmdListStubs(g_BridgeCopyCommandListVtable, std::make_integer_sequence<int, kDurangoCmdListSlots>());

        // --------------------------------------------------------------------
        // Métodos Base Comuns (IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList)
        // Sustentados pela especificação COM e ciclo de vida:
        //   - Slots 0..2: QueryInterface, AddRef, Release
        //   - Slot 7: GetDevice
        //   - Slot 8: GetType
        //   - Slot 9: Close (game+0xAA33F)
        //   - Slot 10: Reset (telemetria de execução controlada)
        // --------------------------------------------------------------------
        auto InitCommon = [](void** vtbl) {
            vtbl[0]  = (void*)&BridgeCmdList_QueryInterface;
            vtbl[1]  = (void*)&BridgeCmdList_AddRef;
            vtbl[2]  = (void*)&BridgeCmdList_Release;
            vtbl[7]  = (void*)&BridgeCmdList_GetDevice;
            vtbl[8]  = (void*)&BridgeCmdList_GetType;
            vtbl[9]  = (void*)&BridgeCmdList_Close;
            vtbl[10] = (void*)&BridgeCmdList_Reset;
        };

        InitCommon(g_BridgeDirectCommandListVtable);
        InitCommon(g_BridgeComputeCommandListVtable);
        InitCommon(g_BridgeCopyCommandListVtable);

        // --------------------------------------------------------------------
        // COPY Command List Vtable (D3D12_COMMAND_LIST_TYPE_COPY = 3)
        // --------------------------------------------------------------------
        g_BridgeCopyCommandListVtable[11] = (void*)&BridgeCmdList_CopyBufferRegion;
        g_BridgeCopyCommandListVtable[12] = (void*)&BridgeCmdList_CopyTextureRegion;
        g_BridgeCopyCommandListVtable[13] = (void*)&BridgeCmdList_CopyResource;
        g_BridgeCopyCommandListVtable[15] = (void*)&BridgeCmdList_ResourceBarrier;
        g_BridgeCopyCommandListVtable[16] = (void*)&BridgeCmdList_CopyTextureRegion;
        g_BridgeCopyCommandListVtable[17] = (void*)&BridgeCmdList_CopyResource;
        g_BridgeCopyCommandListVtable[26] = (void*)&BridgeCmdList_ResourceBarrier;
        g_BridgeCopyCommandListVtable[83] = (void*)&BridgeCmdList_CopyTextureRegion;

        // --------------------------------------------------------------------
        // DIRECT Command List Vtable (D3D12_COMMAND_LIST_TYPE_DIRECT = 0)
        // Confirmados por call site ou header/ABI identificado:
        //   - Slot 12 (+0x60): DrawInstanced (game+0x1213E2)
        //   - Slot 14 (+0x70): Dispatch (game+0xB40DC)
        //   - Slot 32 (+0x100): SetDescriptorHeaps (game+0xABBF3)
        //   - Slot 59 (+0x1D8): ExecuteIndirect (game+0xB4175 e d3d12.h)
        //   - Slot 83 (+0x298): CopyTextureRegion (game+0xAF971, ramo não-COPY)
        // --------------------------------------------------------------------
        g_BridgeDirectCommandListVtable[11] = (void*)&BridgeCmdList_ClearState;
        g_BridgeDirectCommandListVtable[12] = (void*)&BridgeCmdList_DrawInstanced;
        g_BridgeDirectCommandListVtable[13] = (void*)&BridgeCmdList_DrawIndexedInstanced;
        g_BridgeDirectCommandListVtable[14] = (void*)&BridgeCmdList_Dispatch;

        g_BridgeDirectCommandListVtable[15] = (void*)&BridgeCmdList_CopyBufferRegion;
        g_BridgeDirectCommandListVtable[16] = (void*)&BridgeCmdList_CopyTextureRegion;
        g_BridgeDirectCommandListVtable[17] = (void*)&BridgeCmdList_CopyResource;
        g_BridgeDirectCommandListVtable[19] = (void*)&BridgeCmdList_ResolveSubresource;

        g_BridgeDirectCommandListVtable[20] = (void*)&BridgeCmdList_IASetPrimitiveTopology;
        g_BridgeDirectCommandListVtable[21] = (void*)&BridgeCmdList_RSSetViewports;
        g_BridgeDirectCommandListVtable[22] = (void*)&BridgeCmdList_RSSetScissorRects;
        g_BridgeDirectCommandListVtable[23] = (void*)&BridgeCmdList_OMSetBlendFactor;
        g_BridgeDirectCommandListVtable[24] = (void*)&BridgeCmdList_OMSetStencilRef;
        g_BridgeDirectCommandListVtable[25] = (void*)&BridgeCmdList_SetPipelineState;
        g_BridgeDirectCommandListVtable[26] = (void*)&BridgeCmdList_ResourceBarrier;
        g_BridgeDirectCommandListVtable[27] = (void*)&BridgeCmdList_ExecuteBundle;

        // Direct Command List Vtable:
        // Slots 28-31: Durango extensions / fallback mappings
        g_BridgeDirectCommandListVtable[28] = (void*)&BridgeCmdList_SetDescriptorHeaps;
        g_BridgeDirectCommandListVtable[29] = (void*)&BridgeCmdList_Slot29_XboxStub;
        g_BridgeDirectCommandListVtable[30] = (void*)&BridgeCmdList_Slot30_XboxTileDecompress;
        g_BridgeDirectCommandListVtable[31] = (void*)&BridgeCmdList_Slot29_XboxStub;

        // Durango XDK Direct3D 12 Shifted Layout (+4 slots relative to PC D3D12):
        g_BridgeDirectCommandListVtable[32] = (void*)&BridgeCmdList_SmartSlot32;                       // 0x100: SetDescriptorHeaps (0xABBF3, 0xC069A, etc.)
        g_BridgeDirectCommandListVtable[33] = (void*)&BridgeCmdList_SetComputeRootSignature;            // 0x108: SetComputeRootSignature (0xB597C, 0xB5A3E)
        g_BridgeDirectCommandListVtable[34] = (void*)&BridgeCmdList_SetGraphicsRootSignature;           // 0x110: SetGraphicsRootSignature (0xB5EB0, 0xB5F79)
        g_BridgeDirectCommandListVtable[35] = (void*)&BridgeCmdList_SetComputeRootDescriptorTable;      // 0x118: SetComputeRootDescriptorTable (0xB8FBE)
        g_BridgeDirectCommandListVtable[36] = (void*)&BridgeCmdList_SetGraphicsRootDescriptorTable;     // 0x120: SetGraphicsRootDescriptorTable (0xB94EE)
        g_BridgeDirectCommandListVtable[37] = (void*)&BridgeCmdList_SetComputeRoot32BitConstant;        // 0x128: SetComputeRoot32BitConstant
        g_BridgeDirectCommandListVtable[38] = (void*)&BridgeCmdList_SetGraphicsRoot32BitConstant;       // 0x130: SetGraphicsRoot32BitConstant
        g_BridgeDirectCommandListVtable[39] = (void*)&BridgeCmdList_SetComputeRoot32BitConstants;       // 0x138: SetComputeRoot32BitConstants (0xB9379)
        g_BridgeDirectCommandListVtable[40] = (void*)&BridgeCmdList_SetGraphicsRoot32BitConstants;      // 0x140: SetGraphicsRoot32BitConstants (0xB98B9)
        g_BridgeDirectCommandListVtable[41] = (void*)&BridgeCmdList_SetComputeRootConstantBufferView;  // 0x148: SetComputeRootConstantBufferView (0xB93ED)
        g_BridgeDirectCommandListVtable[42] = (void*)&BridgeCmdList_SetGraphicsRootConstantBufferView; // 0x150: SetGraphicsRootConstantBufferView (0xB9933)
        g_BridgeDirectCommandListVtable[43] = (void*)&BridgeCmdList_SetComputeRootShaderResourceView;  // 0x158: SetComputeRootShaderResourceView (0xB927B)
        g_BridgeDirectCommandListVtable[44] = (void*)&BridgeCmdList_SetGraphicsRootShaderResourceView; // 0x160: SetGraphicsRootShaderResourceView (0xB97BD)
        g_BridgeDirectCommandListVtable[45] = (void*)&BridgeCmdList_SetComputeRootUnorderedAccessView; // 0x168: SetComputeRootUnorderedAccessView (0xB92B8)
        g_BridgeDirectCommandListVtable[46] = (void*)&BridgeCmdList_SetGraphicsRootUnorderedAccessView; // 0x170: SetGraphicsRootUnorderedAccessView (0xB97FA)
        g_BridgeDirectCommandListVtable[47] = (void*)&BridgeCmdList_IASetIndexBuffer;                   // 0x178: IASetIndexBuffer (0xB6E06)
        g_BridgeDirectCommandListVtable[48] = (void*)&BridgeCmdList_IASetVertexBuffers;                 // 0x180: IASetVertexBuffers (0xB6DB1)
        g_BridgeDirectCommandListVtable[49] = (void*)&BridgeCmdList_SOSetTargets;                       // 0x188: SOSetTargets
        g_BridgeDirectCommandListVtable[50] = (void*)&BridgeCmdList_OMSetRenderTargets;                 // 0x190: OMSetRenderTargets (0xB7137)
        g_BridgeDirectCommandListVtable[51] = (void*)&BridgeCmdList_ClearDepthStencilView;             // 0x198: ClearDepthStencilView (0xAB780, 0xAB432)
        g_BridgeDirectCommandListVtable[52] = (void*)&BridgeCmdList_ClearRenderTargetView;             // 0x1A0: ClearRenderTargetView (0xAB82F, 0xAB49B)
        g_BridgeDirectCommandListVtable[53] = (void*)&BridgeCmdList_ClearUnorderedAccessViewUint;      // 0x1A8: ClearUnorderedAccessViewUint
        g_BridgeDirectCommandListVtable[54] = (void*)&BridgeCmdList_ClearUnorderedAccessViewFloat;     // 0x1B0: ClearUnorderedAccessViewFloat
        g_BridgeDirectCommandListVtable[55] = (void*)&BridgeCmdList_DiscardResource;                  // 0x1B8: DiscardResource
        g_BridgeDirectCommandListVtable[59] = (void*)&BridgeCmdList_ExecuteIndirect;                   // 0x1D8: ExecuteIndirect (0xB4175)
        g_BridgeDirectCommandListVtable[60] = (void*)&BridgeCmdList_ExecuteIndirect;
        g_BridgeDirectCommandListVtable[67] = (void*)&BridgeCmdList_PipelineBarrier;
        g_BridgeDirectCommandListVtable[83] = (void*)&BridgeCmdList_CopyTextureRegion;                 // 0x298: CopyTextureRegion (0xAF971)
        g_BridgeDirectCommandListVtable[84] = (void*)&BridgeCmdList_Slot84_CopyResourceX;          // 0x2A0: Durango CopyResource with flags

        // --------------------------------------------------------------------
        // COMPUTE Command List Vtable (D3D12_COMMAND_LIST_TYPE_COMPUTE = 2)
        // --------------------------------------------------------------------
        g_BridgeComputeCommandListVtable[11] = (void*)&BridgeCmdList_ClearState;
        g_BridgeComputeCommandListVtable[14] = (void*)&BridgeCmdList_Dispatch;
        g_BridgeComputeCommandListVtable[15] = (void*)&BridgeCmdList_CopyBufferRegion;
        g_BridgeComputeCommandListVtable[16] = (void*)&BridgeCmdList_CopyTextureRegion;
        g_BridgeComputeCommandListVtable[17] = (void*)&BridgeCmdList_CopyResource;
        g_BridgeComputeCommandListVtable[25] = (void*)&BridgeCmdList_SetPipelineState;
        g_BridgeComputeCommandListVtable[26] = (void*)&BridgeCmdList_ResourceBarrier;

        // Durango extensions:
        g_BridgeComputeCommandListVtable[28] = (void*)&BridgeCmdList_SetDescriptorHeaps;
        g_BridgeComputeCommandListVtable[29] = (void*)&BridgeCmdList_Slot29_XboxStub;
        g_BridgeComputeCommandListVtable[30] = (void*)&BridgeCmdList_Slot30_XboxTileDecompress;
        g_BridgeComputeCommandListVtable[31] = (void*)&BridgeCmdList_Slot29_XboxStub;

        // Durango slots (+4 shifted):
        g_BridgeComputeCommandListVtable[32] = (void*)&BridgeCmdList_SmartSlot32;
        g_BridgeComputeCommandListVtable[33] = (void*)&BridgeCmdList_SetComputeRootSignature;
        g_BridgeComputeCommandListVtable[35] = (void*)&BridgeCmdList_SetComputeRootDescriptorTable;
        g_BridgeComputeCommandListVtable[37] = (void*)&BridgeCmdList_SetComputeRoot32BitConstant;
        g_BridgeComputeCommandListVtable[39] = (void*)&BridgeCmdList_SetComputeRoot32BitConstants;
        g_BridgeComputeCommandListVtable[41] = (void*)&BridgeCmdList_SetComputeRootConstantBufferView;
        g_BridgeComputeCommandListVtable[43] = (void*)&BridgeCmdList_SetComputeRootShaderResourceView;
        g_BridgeComputeCommandListVtable[45] = (void*)&BridgeCmdList_SetComputeRootUnorderedAccessView;
        g_BridgeComputeCommandListVtable[51] = (void*)&BridgeCmdList_ClearDepthStencilView;
        g_BridgeComputeCommandListVtable[52] = (void*)&BridgeCmdList_ClearRenderTargetView;
        g_BridgeComputeCommandListVtable[53] = (void*)&BridgeCmdList_ClearUnorderedAccessViewUint;
        g_BridgeComputeCommandListVtable[54] = (void*)&BridgeCmdList_ClearUnorderedAccessViewFloat;
        g_BridgeComputeCommandListVtable[55] = (void*)&BridgeCmdList_DiscardResource;
        g_BridgeComputeCommandListVtable[59] = (void*)&BridgeCmdList_ExecuteIndirect;
        g_BridgeComputeCommandListVtable[60] = (void*)&BridgeCmdList_ExecuteIndirect;
        g_BridgeComputeCommandListVtable[67] = (void*)&BridgeCmdList_PipelineBarrier;
        g_BridgeComputeCommandListVtable[83] = (void*)&BridgeCmdList_CopyTextureRegion;
        // game+0xBA2FA: on any non-copy list the game issues CopyResource(dst, src, flags) through slot 84
        // (copy lists use slot 13); it was an unmapped stub on compute lists, so those copies were dropped.
        g_BridgeComputeCommandListVtable[84] = (void*)&BridgeCmdList_Slot84_CopyResourceX;

        log_line("[d3d12_x] Initialized specialized BridgeCommandList vtables (Direct, Compute, Copy - thread-safe once)\r\n");
    });
}

static inline BridgeCommandList* CreateBridgeCommandList(ID3D12GraphicsCommandList* pReal, D3D12_COMMAND_LIST_TYPE type)
{
    InitBridgeCommandListVtables();
    BridgeCommandList* pBridge = new BridgeCommandList();
    memset(pBridge, 0, sizeof(BridgeCommandList));

    switch (type)
    {
    case D3D12_COMMAND_LIST_TYPE_COPY:
        pBridge->lpVtbl = g_BridgeCopyCommandListVtable;
        break;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        pBridge->lpVtbl = g_BridgeComputeCommandListVtable;
        break;
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
    default:
        pBridge->lpVtbl = g_BridgeDirectCommandListVtable;
        break;
    }

    pBridge->pCurrent = (void*)(uintptr_t)1;
    pBridge->pEndDraw = nullptr;
    pBridge->pEndState = nullptr;
    pBridge->m_pRealCmdList = pReal;
    pBridge->m_type = type;
    pBridge->m_refCount = 1;
    pBridge->m_magic = BRIDGE_CMDLIST_MAGIC;
    return pBridge;
}

