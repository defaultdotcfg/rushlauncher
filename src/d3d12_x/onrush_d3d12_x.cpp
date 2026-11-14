// OnRush d3d12_x.dll diagnostic & desktop translation bridge.
//
// Derived from WinDurango.D3D12X (https://github.com/WinDurango/WinDurango),
// licensed under LGPL-3.0.
// ESRAM mapping uses MapTitleEsramPages from XWine1 SlimEra kernelx (MIT).
//
// Bridges:
// 1. d3d12_x imports: D3D12XboxCreateDevice, DXGIXPresentArray,
// D3D12SerializeRootSignature, D3DMapEsramMemory.
// 2. WinRT/UWP CoreApplication desktop hook: Intercepts RoGetActivationFactory
// for
//    Windows.ApplicationModel.Core.CoreApplication, creates a Win32 desktop
//    window (HWND), wraps it in ICoreWindowEra/ICoreWindowInterop, and drives
//    the game's IFrameworkViewSource.

#define WIN32_LEAN_AND_MEAN
#include "context_allocator_tracker.h"
#include "xbox_sampler_translator.h"
#include "keymap_defs.h"
#include <activation.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <era.h>
#include <eventtoken.h>
#include <exception>
#include <inspectable.h>
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <utility>
#include <vector>
#include <windows.h>
#include <errno.h>
#include <windows.storage.h>
#include <mmdeviceapi.h>
#include <audioclient.h>


#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static SRWLOCK g_log_lock = SRWLOCK_INIT;
static HANDLE g_hLogFile = INVALID_HANDLE_VALUE;

// Full log only in debug mode (launcher option, ONRUSH_VERBOSE_LOG=1). Otherwise each message type
// (same format string) is written at most LOG_CAP times per session: per-call traces like
// GetGPUVirtualAddress made ~1.4 million lines (176 MB) in one session. Formats starting with '%'
// (crash/hang dumps built elsewhere) are never capped.
static bool VerboseLog() {
  static const bool s = GetEnvironmentVariableA("ONRUSH_VERBOSE_LOG", nullptr, 0) > 0;
  return s;
}
static const unsigned LOG_CAP = 50;
// returns 0 = write, 1 = write + "omitted" note, 2 = drop
static int LogCapCheck(const char *format) {
  if (format[0] == '%' || VerboseLog()) return 0;
  enum { N = 16384 };
  static const char *volatile s_keys[N];
  static volatile LONG s_counts[N];
  size_t h = ((uintptr_t)format >> 3) * 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < 64; i++) {
    size_t k = (h + i) & (N - 1);
    const char *cur = s_keys[k];
    if (!cur) cur = (const char *)InterlockedCompareExchangePointer((PVOID volatile *)&s_keys[k], (PVOID)format, nullptr);
    if (!cur || cur == format) {
      LONG c = InterlockedIncrement(&s_counts[k]);
      return c <= (LONG)LOG_CAP ? 0 : c == (LONG)LOG_CAP + 1 ? 1 : 2;
    }
  }
  return 0;  // table full: no cap
}

static void log_line(const char *format, ...) {
  const int cap = LogCapCheck(format);
  if (cap == 2) return;
  char text[1024];
  va_list args;
  va_start(args, format);
  _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
  va_end(args);

  // The game performs tens of thousands of allocations while loading
  // (localisation DB, etc). Logging each one (plus a flush) made boot take
  // minutes, so the allocator chatter is opt-in: ONRUSH_LOG_ALLOC=1.
  static const bool s_logAlloc = []() {
    char v[8] = {};
    return GetEnvironmentVariableA("ONRUSH_LOG_ALLOC", v, sizeof(v)) && v[0] == '1';
  }();
  if (!s_logAlloc &&
      (strncmp(text, "[BridgeAlloc]", 13) == 0 ||
       strncmp(text, "[allocator]", 11) == 0 ||
       strncmp(text, "[realloc_hook]", 14) == 0)) {
    return;
  }
  // The launcher runs the game under its debug loop: every OutputDebugString stops the game until the
  // launcher has read and logged it, which made launcher runs very slow. Our own log file has all of
  // it; mirroring to the debugger is opt-in (ONRUSH_LOG_TO_DEBUGGER=1).
  static const bool s_toDbg = GetEnvironmentVariableA("ONRUSH_LOG_TO_DEBUGGER", nullptr, 0) > 0;
  if (s_toDbg && IsDebuggerPresent()) OutputDebugStringA(text);

  AcquireSRWLockExclusive(&g_log_lock);
  if (g_hLogFile == INVALID_HANDLE_VALUE) {
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&log_line), &self);
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (length && length < MAX_PATH) {
      wchar_t *slash = wcsrchr(path, L'\\');
      if (slash) {
        // one log per session; the previous one is kept as onrush_d3d12_x.prev.log
        wchar_t prev[MAX_PATH];
        wcscpy_s(prev, path);
        wcscpy_s(prev + (slash + 1 - path), MAX_PATH - (slash + 1 - path), L"onrush_d3d12_x.prev.log");
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"onrush_d3d12_x.log");
        MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
        g_hLogFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      }
    }
  }
  if (g_hLogFile != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(g_hLogFile, text, (DWORD)strlen(text), &written, nullptr);
    if (cap == 1) {
      static const char note[] = "[log] (more lines of this kind omitted; full log: launcher debug mode)\r\n";
      WriteFile(g_hLogFile, note, (DWORD)(sizeof(note) - 1), &written, nullptr);
    }
    // Flush at most every 250 ms, but always on crash-related lines.
    static DWORD s_lastFlush = 0;
    DWORD now = GetTickCount();
    if (now - s_lastFlush > 250 || strstr(text, "VECTORED") || strstr(text, "Crash")) {
      FlushFileBuffers(g_hLogFile);
      s_lastFlush = now;
    }
  }
  ReleaseSRWLockExclusive(&g_log_lock);
}

#include "descriptor_track.h"

static void log_guid(const char *label, const IID *riid) {
  if (!riid) {
    log_line("  %s = (null)\r\n", label);
    return;
  }
  log_line("  %s = {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\r\n",
           label, riid->Data1, riid->Data2, riid->Data3, riid->Data4[0],
           riid->Data4[1], riid->Data4[2], riid->Data4[3], riid->Data4[4],
           riid->Data4[5], riid->Data4[6], riid->Data4[7]);
}

void TrackResourceState(ID3D12Resource* pRes, D3D12_RESOURCE_STATES state);
D3D12_RESOURCE_STATES GetTrackedResourceState(ID3D12Resource* pRes, D3D12_RESOURCE_STATES defaultState);

// ============================================================================
// D3D12 Diagnostic & InfoQueue Infrastructure
// ============================================================================

static ID3D12InfoQueue *g_pD3D12InfoQueue = nullptr;
static ID3D12Device *g_pMainDevice = nullptr;
static IDXGIFactory2 *g_pDXGIFactory2 = nullptr;
namespace gfxopt { static bool TearingFlag(IDXGIFactory2 *f); }  // gfx_options.inc
static ID3D12CommandQueue *g_pDirectCommandQueue = nullptr;
static IDXGISwapChain1 *g_pMainSwapChain = nullptr;
static ID3D12Resource *g_pSwapChainBuffers[4] = {};
static UINT g_SwapChainBufferCount = 0;
static SRWLOCK g_RtvMapLock = SRWLOCK_INIT;
static std::unordered_map<SIZE_T, ID3D12Resource*> g_RtvToResourceMap;
static ID3D12Resource* g_pLastRenderedTarget = nullptr;

ID3D12Resource* LookupRtvResource(SIZE_T ptr) {
    if (!ptr || ptr <= 0x10000) return nullptr;
    AcquireSRWLockShared(&g_RtvMapLock);
    auto it = g_RtvToResourceMap.find(ptr);
    ID3D12Resource* pRes = (it != g_RtvToResourceMap.end()) ? it->second : nullptr;
    ReleaseSRWLockShared(&g_RtvMapLock);
    return pRes;
}

static ID3D12Resource* g_pLastOffscreenRenderTarget = nullptr;

void RecordLastRenderedTarget(ID3D12Resource* pRes) {
    if (pRes) {
        g_pLastRenderedTarget = pRes;
        if (pRes != g_pSwapChainBuffers[0] && pRes != g_pSwapChainBuffers[1] &&
            pRes != g_pSwapChainBuffers[2] && pRes != g_pSwapChainBuffers[3]) {
            g_pLastOffscreenRenderTarget = pRes;
        }
        static volatile LONG s_recCount = 0;
        if (InterlockedIncrement(&s_recCount) <= 60) {
            void* retAddr = _ReturnAddress();
            log_line("[d3d12_x] RecordLastRenderedTarget: res=%p, caller=%p\r\n", pRes, retAddr);
        }
    }
}

ID3D12Resource* GetLastRenderedTarget() {
    return g_pLastRenderedTarget;
}

static void FlushD3D12InfoQueue(const char *context) {
  if (!g_pD3D12InfoQueue)
    return;
  UINT64 numMessages = g_pD3D12InfoQueue->GetNumStoredMessages();
  if (numMessages == 0)
    return;
  log_line("[d3d12_x] --- ID3D12InfoQueue Messages (%s: %llu messages) ---\r\n",
           context, numMessages);
  for (UINT64 i = 0; i < numMessages; ++i) {
    SIZE_T messageLength = 0;
    g_pD3D12InfoQueue->GetMessage(i, nullptr, &messageLength);
    if (messageLength > 0) {
      std::vector<uint8_t> buffer(messageLength);
      D3D12_MESSAGE *pMessage =
          reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
      if (SUCCEEDED(
              g_pD3D12InfoQueue->GetMessage(i, pMessage, &messageLength))) {
        const char *severity = "UNKNOWN";
        switch (pMessage->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
          severity = "CORRUPTION";
          break;
        case D3D12_MESSAGE_SEVERITY_ERROR:
          severity = "ERROR";
          break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
          severity = "WARNING";
          break;
        case D3D12_MESSAGE_SEVERITY_INFO:
          severity = "INFO";
          break;
        case D3D12_MESSAGE_SEVERITY_MESSAGE:
          severity = "MESSAGE";
          break;
        }
        log_line("[d3d12_x][D3D12 %s #%d] %s\r\n", severity, pMessage->ID,
                 pMessage->pDescription);
      }
    }
  }
  g_pD3D12InfoQueue->ClearStoredMessages();
  log_line("[d3d12_x] --- End of ID3D12InfoQueue Messages ---\r\n");
}

static void FlushDXGIInfoQueue(const char *context) {
  typedef HRESULT(WINAPI * PFN_DXGIGetDebugInterface1)(UINT Flags, REFIID riid,
                                                       void **pDebug);
  // Only with the D3D12 debug option (ONRUSH_D3D12_DEBUG=1): this runs on every ExecuteCommandLists.
  static int s_on = -1;
  if (s_on < 0) { char b[8] = {}; DWORD n = GetEnvironmentVariableA("ONRUSH_D3D12_DEBUG", b, sizeof(b)); s_on = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0; }
  if (!s_on) return;
  HMODULE hDxgi = GetModuleHandleW(L"dxgi.dll");
  if (!hDxgi)
    hDxgi = LoadLibraryW(L"dxgi.dll");
  if (!hDxgi)
    return;

  auto pfn = (PFN_DXGIGetDebugInterface1)GetProcAddress(
      hDxgi, "DXGIGetDebugInterface1");
  if (!pfn)
    return;

  IDXGIInfoQueue *pDxgiInfoQueue = nullptr;
  HRESULT hr = pfn(0, __uuidof(IDXGIInfoQueue), (void **)&pDxgiInfoQueue);
  if (SUCCEEDED(hr) && pDxgiInfoQueue) {
    UINT64 num = pDxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
    if (num == 0) { pDxgiInfoQueue->Release(); return; }   // nothing new: no log spam per submit
    log_line(
        "[d3d12_x] --- IDXGIInfoQueue Messages (%s: %llu messages) ---\r\n",
        context, num);
    for (UINT64 i = 0; i < num; ++i) {
      SIZE_T len = 0;
      pDxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &len);
      if (len > 0) {
        std::vector<uint8_t> buf(len);
        DXGI_INFO_QUEUE_MESSAGE *msg =
            reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE *>(buf.data());
        if (SUCCEEDED(
                pDxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, msg, &len))) {
          log_line("[d3d12_x][DXGI #%d] %s\r\n", msg->ID, msg->pDescription);
        }
      }
    }
    pDxgiInfoQueue->ClearStoredMessages(DXGI_DEBUG_ALL);
    pDxgiInfoQueue->Release();
    log_line("[d3d12_x] --- End of IDXGIInfoQueue Messages ---\r\n");
  }
}

static void LogD3D12InfoQueueRange(LONG samplerIdx, UINT64 startIndex,
                                   UINT64 endIndex) {
  if (!g_pD3D12InfoQueue || endIndex <= startIndex)
    return;
  UINT64 totalInQueue = g_pD3D12InfoQueue->GetNumStoredMessages();
  if (startIndex >= totalInQueue)
    return;
  if (endIndex > totalInQueue)
    endIndex = totalInQueue;

  log_line("[d3d12_x] --- ID3D12InfoQueue Messages for CreateSampler [#%ld] "
           "(%llu new message(s): index %llu..%llu) ---\r\n",
           samplerIdx, endIndex - startIndex, startIndex, endIndex - 1);
  for (UINT64 i = startIndex; i < endIndex; ++i) {
    SIZE_T messageLength = 0;
    g_pD3D12InfoQueue->GetMessage(i, nullptr, &messageLength);
    if (messageLength > 0) {
      std::vector<uint8_t> buffer(messageLength);
      D3D12_MESSAGE *pMessage =
          reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
      if (SUCCEEDED(
              g_pD3D12InfoQueue->GetMessage(i, pMessage, &messageLength))) {
        const char *severity = "UNKNOWN";
        switch (pMessage->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
          severity = "CORRUPTION";
          break;
        case D3D12_MESSAGE_SEVERITY_ERROR:
          severity = "ERROR";
          break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
          severity = "WARNING";
          break;
        case D3D12_MESSAGE_SEVERITY_INFO:
          severity = "INFO";
          break;
        case D3D12_MESSAGE_SEVERITY_MESSAGE:
          severity = "MESSAGE";
          break;
        }
        log_line("[d3d12_x][CreateSampler #%ld][D3D12 %s #%d] %s\r\n",
                 samplerIdx, severity, pMessage->ID, pMessage->pDescription);
      }
    }
  }
  log_line("[d3d12_x] --- End of ID3D12InfoQueue Messages for CreateSampler "
           "[#%ld] ---\r\n",
           samplerIdx);
}

static void LogD3D12Failure(ID3D12Device *pDevice, const char *opName,
                            HRESULT hr, const char *extraDetails = nullptr) {
  log_line("[d3d12_x] ERROR: D3D12 call '%s' failed with hr=0x%08lX%s%s\r\n",
           opName, (unsigned long)hr, extraDetails ? " - " : "",
           extraDetails ? extraDetails : "");
  if (pDevice) {
    HRESULT reason = pDevice->GetDeviceRemovedReason();
    log_line("  -> pDevice->GetDeviceRemovedReason() = 0x%08lX\r\n",
             (unsigned long)reason);
  }
  FlushD3D12InfoQueue(opName);
}

static HWND EnsureDesktopWindow();

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2 *This, IUnknown *pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain);
static PFN_CreateSwapChainForHwnd g_TrueCreateSwapChainForHwnd = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSwapChainForCoreWindow)(
    IDXGIFactory2 *This, IUnknown *pDevice, IUnknown *pWindow,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
    IDXGISwapChain1 **ppSwapChain);
static PFN_CreateSwapChainForCoreWindow g_TrueCreateSwapChainForCoreWindow =
    nullptr;

static void *g_WrappedFactoryVtable[256] = {};

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(
    IDXGIFactory2 *This, IUnknown *pDevice, IUnknown *pWindow,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
    IDXGISwapChain1 **ppSwapChain) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;
  log_line("[d3d12_x] HookedCreateSwapChainForCoreWindow called from "
           "caller=game+0x%llX:\r\n",
           (unsigned long long)rva);
  log_line("  This=%p, pDeviceIn=%p, pWindowIn=%p, pDesc=%p, "
           "pRestrictToOutput=%p, ppSwapChain=%p\r\n",
           This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);

  if (pDesc) {
    log_line("  Desc: %ux%u, Format=%u, Stereo=%d, SampleDesc={Count=%u, "
             "Qual=%u}, Usage=0x%X, BufferCount=%u, Scaling=%u, SwapEffect=%u, "
             "AlphaMode=%u, Flags=0x%X\r\n",
             pDesc->Width, pDesc->Height, pDesc->Format, pDesc->Stereo,
             pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality,
             pDesc->BufferUsage, pDesc->BufferCount, pDesc->Scaling,
             pDesc->SwapEffect, pDesc->AlphaMode, pDesc->Flags);
  }

  if (!ppSwapChain) {
    log_line("[d3d12_x] ERROR: ppSwapChain is null\r\n");
    return E_POINTER;
  }
  *ppSwapChain = nullptr;

  if (!g_pDirectCommandQueue && pDevice) {
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&queue)) && queue) {
      g_pDirectCommandQueue = queue;
      log_line("  -> Recovered g_pDirectCommandQueue=%p from pDevice QI\r\n", g_pDirectCommandQueue);
    }
  }

  // Requirement 5: Se não houver fila DIRECT válida ou HWND válido, falhe com
  // log claro e pare; não crie substitutos especulativos.
  if (!g_pDirectCommandQueue) {
    log_line("[d3d12_x] ERROR: No valid DIRECT CommandQueue captured! Cannot "
             "create swapchain.\r\n");
    return E_FAIL;
  }

  HWND hwnd = EnsureDesktopWindow();
  if (!hwnd || !IsWindow(hwnd)) {
    log_line("[d3d12_x] ERROR: No valid desktop HWND available! Cannot create "
             "swapchain.\r\n");
    return E_FAIL;
  }
  log_line("  -> Adapting to CreateSwapChainForHwnd using DIRECT "
           "CommandQueue=%p, HWND=%p\r\n",
           g_pDirectCommandQueue, hwnd);

  if (!g_TrueCreateSwapChainForHwnd && (This || g_pDXGIFactory2)) {
    IDXGIFactory2 *fact = This ? This : g_pDXGIFactory2;
    void **vtbl = *(void ***)fact;
    if (vtbl) {
      g_TrueCreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)vtbl[15];
      log_line("  -> Recovered g_TrueCreateSwapChainForHwnd=%p from factory vtbl[15]\r\n",
               g_TrueCreateSwapChainForHwnd);
    }
  }

  if (!g_TrueCreateSwapChainForHwnd) {
    log_line("[d3d12_x] ERROR: g_TrueCreateSwapChainForHwnd is null\r\n");
    return E_FAIL;
  }

  // Desktop DXGI flip-model swap chains do not accept
  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB (29) as the buffer format;
  // DXGI_FORMAT_R8G8B8A8_UNORM (28) is allowed. Convert ONLY the swap-chain
  // surface format, ONLY when the received format is exactly 29, in a local
  // copy. The caller's *pDesc is not modified and every other field is
  // preserved. This does not address gamma of RTVs created later.
  const DXGI_SWAP_CHAIN_DESC1 *pDescForCall = pDesc;
  DXGI_SWAP_CHAIN_DESC1 descConverted = {};
  if (pDesc) {
    descConverted = *pDesc;
    if (descConverted.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
      descConverted.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      log_line("  -> Swap-chain surface format converted: UNORM_SRGB (29) -> UNORM (28)\r\n");
    }
    // D3D12 on desktop Windows requires FLIP model!
    if (descConverted.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        descConverted.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) {
      descConverted.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      log_line("  -> SwapEffect converted to DXGI_SWAP_EFFECT_FLIP_DISCARD (4) for D3D12\r\n");
    }
    if (descConverted.BufferCount < 2) {
      descConverted.BufferCount = 2;
    }
    // Vertical sync off (in-game option) presents with tearing allowed, which needs the swap-chain flag.
    if (gfxopt::TearingFlag(This ? This : g_pDXGIFactory2)) descConverted.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    // Stretch the fixed-size backbuffer to the window (window resize / Alt+Enter toggle).
    descConverted.Scaling = DXGI_SCALING_STRETCH;
    pDescForCall = &descConverted;
  }

  IDXGIFactory2 *pFactoryToUse = This;
  if (!pFactoryToUse && g_pDXGIFactory2) {
    pFactoryToUse = g_pDXGIFactory2;
  }

  // Call CreateSwapChainForHwnd using captured DIRECT queue, the bridge's
  // in-process desktop HWND, and the received DXGI_SWAP_CHAIN_DESC1
  HRESULT hr = g_TrueCreateSwapChainForHwnd(pFactoryToUse, g_pDirectCommandQueue, hwnd,
                                            pDescForCall,
                                            nullptr, // pFullscreenDesc
                                            pRestrictToOutput, ppSwapChain);

  // Alt+Enter is handled by the bridge window (borderless toggle), not by DXGI exclusive mode.
  if (SUCCEEDED(hr) && pFactoryToUse && hwnd)
    pFactoryToUse->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  log_line("[d3d12_x] CreateSwapChainForHwnd returned hr=0x%08lX, "
           "finalFormat=%u, ppSwapChain=%p, *ppSwapChain=%p\r\n",
           (unsigned long)hr,
           pDescForCall ? (unsigned)pDescForCall->Format : 0u, ppSwapChain,
           ppSwapChain ? *ppSwapChain : nullptr);

  if (FAILED(hr)) {
    log_line(
        "[d3d12_x] ERROR: CreateSwapChainForHwnd failed with hr=0x%08lX\r\n",
        (unsigned long)hr);
    if (pDevice) {
      ID3D12Device *dev = nullptr;
      if (SUCCEEDED(
              pDevice->QueryInterface(__uuidof(ID3D12Device), (void **)&dev)) &&
          dev) {
        HRESULT reason = dev->GetDeviceRemovedReason();
        log_line("  -> DeviceRemovedReason: 0x%08lX\r\n",
                 (unsigned long)reason);
        dev->Release();
      }
    }
    FlushD3D12InfoQueue("CreateSwapChainForHwnd");
    FlushDXGIInfoQueue("CreateSwapChainForHwnd");
    return hr;
  }

  FlushDXGIInfoQueue("CreateSwapChainForHwnd_Success");

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    g_pMainSwapChain = *ppSwapChain;
    log_line("[d3d12_x] Captured and stored g_pMainSwapChain = %p\r\n", g_pMainSwapChain);
  }

  // Requirement 4: Retain SwapChain buffers for presentation blit pipeline.
  if (SUCCEEDED(hr) && *ppSwapChain) {
    IDXGISwapChain1 *sc = *ppSwapChain;
    g_SwapChainBufferCount = descConverted.BufferCount;
    if (g_SwapChainBufferCount > 4) g_SwapChainBufferCount = 4;
    for (UINT i = 0; i < g_SwapChainBufferCount; ++i) {
      if (g_pSwapChainBuffers[i]) {
        g_pSwapChainBuffers[i]->Release();
        g_pSwapChainBuffers[i] = nullptr;
      }
      HRESULT hrBuf = sc->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&g_pSwapChainBuffers[i]);
      log_line("[d3d12_x] SwapChain buffer %u -> hr=0x%08lX, res=%p\r\n",
               i, (unsigned long)hrBuf, g_pSwapChainBuffers[i]);
      if (SUCCEEDED(hrBuf) && g_pSwapChainBuffers[i]) {
        TrackResourceState(g_pSwapChainBuffers[i], D3D12_RESOURCE_STATE_PRESENT);
        D3D12_RESOURCE_DESC d = g_pSwapChainBuffers[i]->GetDesc();
        log_line("  Buffer %u Desc: Dim=%d, %llux%u, Fmt=%d, Mip=%u, Flags=0x%X\r\n",
                 i, d.Dimension, (unsigned long long)d.Width, d.Height, d.Format, d.MipLevels, d.Flags);
      }
    }
  }

  return hr;
}

static void WrapFactoryInstance(IDXGIFactory2 *pFactory) {
  if (!pFactory)
    return;
  void **pCurrentVtbl = *(void ***)pFactory;
  if (pCurrentVtbl == g_WrappedFactoryVtable) {
    return; // already wrapped
  }
  int validFactorySlots = 0;
  for (int i = 0; i < 256; ++i) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(pCurrentVtbl[i], &mbi, sizeof(mbi)) &&
        (mbi.Protect == PAGE_EXECUTE_READ ||
         mbi.Protect == PAGE_EXECUTE_READWRITE)) {
      g_WrappedFactoryVtable[i] = pCurrentVtbl[i];
      validFactorySlots = i + 1;
    } else {
      break;
    }
  }
  g_TrueCreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)pCurrentVtbl[15];
  g_TrueCreateSwapChainForCoreWindow =
      (PFN_CreateSwapChainForCoreWindow)pCurrentVtbl[16];
  g_WrappedFactoryVtable[16] = (void *)&HookedCreateSwapChainForCoreWindow;
  *(void ***)pFactory = g_WrappedFactoryVtable;
  log_line(
      "[d3d12_x] WrapFactoryInstance: Wrapped IDXGIFactory2 %p (%d valid slots "
      "copied, Slot 16 Hooked, Slot 15 TrueCreateSwapChainForHwnd=%p)\r\n",
      pFactory, validFactorySlots, g_TrueCreateSwapChainForHwnd);
}

static bool EnsureDXGIFactoryGlobal(HMODULE hExe) {
  if (!hExe)
    hExe = GetModuleHandleW(nullptr);
  if (!hExe) {
    log_line("[d3d12_x] EnsureDXGIFactoryGlobal: invalid hExe\r\n");
    return false;
  }

  void **ppFactoryGlobal = (void **)((uint8_t *)hExe + 0x01538AA8);
  if (g_pDXGIFactory2 && *ppFactoryGlobal == g_pDXGIFactory2) {
    return true;
  }

  typedef HRESULT(WINAPI * PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid,
                                                   void **ppFactory);
  typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID riid,
                                                   void **ppFactory);
  HMODULE hDxgi = GetModuleHandleW(L"dxgi.dll");
  if (!hDxgi)
    hDxgi = LoadLibraryW(L"dxgi.dll");
  if (!hDxgi) {
    log_line("[d3d12_x] EnsureDXGIFactoryGlobal: LoadLibrary(dxgi.dll) failed "
             "(Win32=%lu)\r\n",
             GetLastError());
    return false;
  }

  IDXGIFactory2 *pFactory2 = nullptr;
  auto pfnCreate2 =
      (PFN_CreateDXGIFactory2)GetProcAddress(hDxgi, "CreateDXGIFactory2");
  if (pfnCreate2) {
    HRESULT hr2 = pfnCreate2(0, __uuidof(IDXGIFactory2), (void **)&pFactory2);
    log_line("[d3d12_x] Lazy CreateDXGIFactory2 outside loader lock -> "
             "hr=0x%08lX, factory=%p\r\n",
             (unsigned long)hr2, pFactory2);
    if (FAILED(hr2) || !pFactory2) {
      if (pFactory2) {
        pFactory2->Release();
        pFactory2 = nullptr;
      }
    }
  }

  if (!pFactory2) {
    auto pfnCreate1 =
        (PFN_CreateDXGIFactory1)GetProcAddress(hDxgi, "CreateDXGIFactory1");
    if (pfnCreate1) {
      IDXGIFactory1 *pFactory1 = nullptr;
      HRESULT hr1 = pfnCreate1(__uuidof(IDXGIFactory1), (void **)&pFactory1);
      log_line(
          "[d3d12_x] Lazy CreateDXGIFactory1 -> hr=0x%08lX, factory1=%p\r\n",
          (unsigned long)hr1, pFactory1);
      if (SUCCEEDED(hr1) && pFactory1) {
        // CRITICAL: Strictly QueryInterface for IDXGIFactory2, do NOT store
        // IDXGIFactory1 as IDXGIFactory2!
        HRESULT hrQI = pFactory1->QueryInterface(__uuidof(IDXGIFactory2),
                                                 (void **)&pFactory2);
        log_line("[d3d12_x] Lazy Factory1->QueryInterface(IDXGIFactory2) -> "
                 "hr=0x%08lX, factory2=%p\r\n",
                 (unsigned long)hrQI, pFactory2);
        pFactory1->Release();
        pFactory1 = nullptr;

        if (FAILED(hrQI) || !pFactory2) {
          if (pFactory2) {
            pFactory2->Release();
            pFactory2 = nullptr;
          }
        }
      } else {
        if (pFactory1) {
          pFactory1->Release();
          pFactory1 = nullptr;
        }
      }
    }
  }

  if (!pFactory2) {
    log_line("[d3d12_x] ERROR: Could not create IDXGIFactory2; global remains "
             "unchanged\r\n");
    return false;
  }

  DWORD oldProtFac = 0;
  if (!VirtualProtect(ppFactoryGlobal, sizeof(void *), PAGE_EXECUTE_READWRITE,
                      &oldProtFac)) {
    DWORD err = GetLastError();
    log_line("[d3d12_x] ERROR: VirtualProtect failed for factory global "
             "(Win32=%lu)\r\n",
             err);
    pFactory2->Release();
    pFactory2 = nullptr;
    g_pDXGIFactory2 = nullptr;
    return false;
  }

  *ppFactoryGlobal = pFactory2;
  VirtualProtect(ppFactoryGlobal, sizeof(void *), oldProtFac, &oldProtFac);

  g_pDXGIFactory2 = pFactory2;
  WrapFactoryInstance(pFactory2);
  log_line("[d3d12_x] Initialized [hExe + 0x01538AA8] with real IDXGIFactory2 "
           "%p\r\n",
           g_pDXGIFactory2);
  return true;
}

// ============================================================================
// WinRT & CoreApplication Interfaces & Desktop Implementation
// ============================================================================

typedef PCWSTR(WINAPI *PFN_WindowsGetStringRawBuffer)(HSTRING string,
                                                      UINT32 *length);
typedef HRESULT(WINAPI *PFN_WindowsCreateString)(PCNZWCH sourceString,
                                                 UINT32 length,
                                                 HSTRING *string);
static PFN_WindowsCreateString g_pfnWindowsCreateString = nullptr;

static HRESULT CreateHString(const wchar_t *str, HSTRING *outStr) {
  if (!outStr)
    return E_POINTER;
  if (!g_pfnWindowsCreateString) {
    HMODULE hCb = GetModuleHandleW(L"combase.dll");
    if (hCb)
      g_pfnWindowsCreateString =
          (PFN_WindowsCreateString)GetProcAddress(hCb, "WindowsCreateString");
  }
  if (g_pfnWindowsCreateString && str)
    return g_pfnWindowsCreateString(str, (UINT32)wcslen(str), outStr);
  *outStr = nullptr;
  return S_OK;
}

// Forward declarations
struct IFrameworkView;
struct ICoreApplicationViewEra;
struct ICoreWindowEra;

// {0AACF7A4-5E1D-49DF-8034-FB6A68BC5ED1}
static const GUID IID_ICoreApplicationEra = {
    0x0aacf7a4,
    0x5e1d,
    0x49df,
    {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};

// {79B9D5F2-879E-4B89-B798-79E47598030C}
static const GUID IID_ICoreWindowEra = {
    0x79b9d5f2,
    0x879e,
    0x4b89,
    {0xb7, 0x98, 0x79, 0xe4, 0x75, 0x98, 0x03, 0x0c}};

// {45D64A29-A63E-4CB6-B498-5781D298CB4F}
static const GUID IID_ICoreWindowInterop = {
    0x45d64a29,
    0xa63e,
    0x4cb6,
    {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};

// {638D2246-3A4B-4BE6-80C5-50E2A4491E70}
static const GUID IID_ICoreApplicationView = {
    0x638d2246,
    0x3a4b,
    0x4be6,
    {0x80, 0xc5, 0x50, 0xe2, 0xa4, 0x49, 0x1e, 0x70}};

// {00000035-0000-0000-C000-000000000046}
static const GUID IID_IActivationFactory = {
    0x00000035,
    0x0000,
    0x0000,
    {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}
static const GUID IID_IAgileObject = {
    0x94ea2b94,
    0xe9cc,
    0x49e0,
    {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// {42A6A529-3CC6-4867-B5B7-6D1FAF5325F9}
static const GUID IID_ICoreApplicationGpuPolicyEra = {
    0x42a6a529,
    0x3cc6,
    0x4867,
    {0xb5, 0xb7, 0x6d, 0x1f, 0xaf, 0x53, 0x25, 0xf9}};

// {9FF3799E-6E83-468C-A5B0-501CE6BB0FA1}
static const GUID IID_ICoreApplicationResourceAvailabilityEra = {
    0x9ff3799e,
    0x6e83,
    0x468c,
    {0xa5, 0xb0, 0x50, 0x1c, 0xe6, 0xbb, 0x0f, 0xa1}};

// {4D239005-3C2A-41B1-9022-536BB9CF93B1}
static const GUID IID_ICoreWindowStatic = {
    0x4d239005,
    0x3c2a,
    0x41b1,
    {0x90, 0x22, 0x53, 0x6b, 0xb9, 0xcf, 0x93, 0xb1}};

MIDL_INTERFACE("45D64A29-A63E-4CB6-B498-5781D298CB4F")
ICoreWindowInterop : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND * hwnd) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(boolean value) = 0;
};

MIDL_INTERFACE("FAAB5CD0-8932-407B-BF81-F0E14F594074")
IFrameworkViewSource : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateView(IFrameworkView *
                                               *viewProvider) = 0;
};

MIDL_INTERFACE("1A1D14E0-E246-4EE6-99C5-85A009AC2980")
IFrameworkView : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Initialize(ICoreApplicationViewEra *
                                               applicationView) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetWindow(ICoreWindowEra * window) = 0;
  virtual HRESULT STDMETHODCALLTYPE Load(HSTRING entryPoint) = 0;
  virtual HRESULT STDMETHODCALLTYPE Run(void) = 0;
  virtual HRESULT STDMETHODCALLTYPE Uninitialize(void) = 0;
};

struct RectF {
  float X, Y, Width, Height;
};

MIDL_INTERFACE("79B9D5F2-879E-4B89-B798-79E47598030C")
ICoreWindowEra : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE _abi_get_Bounds(RectF * rect) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_get_Dispatcher(void **dispatcher) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_get_IsInputEnabled(boolean *
                                                            value) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_set_IsInputEnabled(boolean value) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_get_Visible(boolean * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE Activate(void) = 0;
  virtual HRESULT STDMETHODCALLTYPE Close(void) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAsyncKeyState(int virtualKey,
                                                     int *KeyState) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetKeyState(int virtualKey,
                                                int *KeyState) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_Activated(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_Activated(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_CharacterReceived(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_CharacterReceived(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_Closed(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_Closed(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_InputEnabled(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_InputEnabled(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_KeyDown(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_KeyDown(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_KeyUp(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_KeyUp(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_PointerMoved(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_PointerMoved(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_PointerEntered(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_PointerEntered(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_PointerExited(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_PointerExited(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_SizeChanged(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_SizeChanged(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_VisibilityChanged(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_VisibilityChanged(
      EventRegistrationToken token) = 0;
};

MIDL_INTERFACE("4D239005-3C2A-41B1-9022-536BB9CF93B1")
ICoreWindowStatic : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE GetForCurrentThread(ICoreWindowEra *
                                                        *ppWindow) = 0;
};

MIDL_INTERFACE("638D2246-3A4B-4BE6-80C5-50E2A4491E70")
ICoreApplicationViewEra : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_CoreWindow(ICoreWindowEra * *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Activated(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Activated(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsMain(boolean * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsHosted(boolean * value) = 0;
};

MIDL_INTERFACE("42A6A529-3CC6-4867-B5B7-6D1FAF5325F9")
ICoreApplicationGpuPolicyEra : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DisableKinectGpuReservation(
      bool *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE set_DisableKinectGpuReservation(
      bool value) = 0;
};

MIDL_INTERFACE("9FF3799E-6E83-468C-A5B0-501CE6BB0FA1")
ICoreApplicationResourceAvailabilityEra : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE _abi_get_ResourceAvailability(
      int *resourceAvailability) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_ResourceAvailabilityChanged(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_ResourceAvailabilityChanged(
      EventRegistrationToken token) = 0;
};

MIDL_INTERFACE("0AACF7A4-5E1D-49DF-8034-FB6A68BC5ED1")
ICoreApplicationEra : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE _abi_get_Id(HSTRING * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_Suspending(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_Suspending(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_add_Resuming(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_remove_Resuming(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_get_Properties(void **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_GetCurrentView(
      ICoreApplicationViewEra * *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_Run(IFrameworkViewSource *
                                             viewSource) = 0;
  virtual HRESULT STDMETHODCALLTYPE _abi_RunWithActivationFactories(
      void *activationFactoryCallback) = 0;
};

// Global desktop window
static HWND g_desktopHwnd = nullptr;

// Alt+Enter: toggle between the windowed style and a borderless window covering the monitor.
static bool g_isBorderlessFull = false;
static RECT g_savedWindowRect = {};
static LONG_PTR g_savedStyle = 0, g_savedExStyle = 0;
static void ToggleBorderlessFullscreen(HWND hwnd) {
  if (!g_isBorderlessFull) {
    GetWindowRect(hwnd, &g_savedWindowRect);
    g_savedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    g_savedExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(hwnd, GWL_STYLE, (g_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
    g_isBorderlessFull = true;
  } else {
    SetWindowLongPtrW(hwnd, GWL_STYLE, g_savedStyle);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_savedExStyle);
    SetWindowPos(hwnd, HWND_NOTOPMOST, g_savedWindowRect.left, g_savedWindowRect.top,
                 g_savedWindowRect.right - g_savedWindowRect.left, g_savedWindowRect.bottom - g_savedWindowRect.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
    g_isBorderlessFull = false;
  }
}

// The game opened as a borderless window covering the screen (Borderless / Fullscreen): Alt+Enter must
// know it is already "full" and where to go back to, a framed window. That window shows the render
// size (shrunk, same aspect, when it does not fit the work area) centred on the monitor's work area.
static void InitWindowedRestore(HWND hwnd, int clientW, int clientH) {
  const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE, exStyle = WS_EX_APPWINDOW;
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT &work = mi.rcWork;
  RECT fr = {0, 0, 100, 100};
  AdjustWindowRectEx(&fr, style, FALSE, exStyle);
  int frameW = (fr.right - fr.left) - 100, frameH = (fr.bottom - fr.top) - 100;
  int availW = (work.right - work.left) - frameW, availH = (work.bottom - work.top) - frameH;
  int cw = clientW > 0 ? clientW : 1280, ch = clientH > 0 ? clientH : 720;
  if (cw > availW || ch > availH) {
    double s = 0.9 * min((double)availW / cw, (double)availH / ch);
    cw = (int)(cw * s); ch = (int)(ch * s);
  }
  int w = cw + frameW, h = ch + frameH;
  g_savedWindowRect.left = work.left + ((work.right - work.left) - w) / 2;
  g_savedWindowRect.top = work.top + ((work.bottom - work.top) - h) / 2;
  g_savedWindowRect.right = g_savedWindowRect.left + w;
  g_savedWindowRect.bottom = g_savedWindowRect.top + h;
  g_savedStyle = style;
  g_savedExStyle = exStyle;
  g_isBorderlessFull = true;
}

// Called every Present: the window may be owned by a thread that never pumps messages, so
// Alt+Enter is detected from the keyboard state (only while the game window has focus).
// Alt+F4 / window close button: the Xbox game has no "quit" of its own (the console just suspends it),
// so the process ends here with exit code 0 (the launcher treats it as a normal exit).
static void QuitGameFromWindow(const char *why) {
  log_line("[window] %s -> closing the game\r\n", why);
  TerminateProcess(GetCurrentProcess(), 0);
}

static void PollAltEnterToggle() {
  static bool s_wasDown = false;
  if (!g_desktopHwnd) return;
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  // Alt+F4 also from the keyboard state: the window's own thread may never see the key message
  if (pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F4) & 0x8000))
    QuitGameFromWindow("Alt+F4");
  bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_RETURN) & 0x8000);
  if (down && !s_wasDown) {
    ToggleBorderlessFullscreen(g_desktopHwnd);
    log_line("[window] Alt+Enter -> %s\r\n", g_isBorderlessFull ? "borderless fullscreen" : "windowed");
  }
  s_wasDown = down;
}

namespace mouse { static bool OnMessage(HWND, UINT, WPARAM, LPARAM, LRESULT *); static void Update(bool focused); static DWORD Buttons(bool focused); }   // mouse.inc
static LRESULT CALLBACK DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
  { LRESULT r = 0; if (mouse::OnMessage(hwnd, msg, wParam, lParam, &r)) return r; }
  switch (msg) {
  case WM_ACTIVATE:
  case WM_ACTIVATEAPP:
    // which path activates the window (background test mode should never be activated)
    log_line("[window] %s wParam=0x%llX (frame thread activity, fg=%p)\r\n", msg == WM_ACTIVATE ? "WM_ACTIVATE" : "WM_ACTIVATEAPP",
             (unsigned long long)wParam, GetForegroundWindow());
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  case WM_SYSKEYDOWN:
    if (wParam == VK_RETURN && (lParam & (1 << 29)))  // Alt+Enter: toggled by PollAltEnterToggle()
      return 0;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  case WM_SYSCHAR:
    if (wParam == VK_RETURN) return 0;  // no "ding" on Alt+Enter
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  case WM_SYSCOMMAND:
    // Alt or F10 alone (and Alt+Space) open the window menu from the keyboard. DefWindowProc then runs a
    // modal menu loop on this thread, which also renders the frames: the game froze until Esc or a click.
    if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  case WM_ERASEBKGND:
    // Prevent GDI from overwriting Direct3D 12 backbuffers with background brush
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_CLOSE:  // Alt+F4 (DefWindowProc) or the title bar's close button
    QuitGameFromWindow("WM_CLOSE");
    return 0;
  case WM_DESTROY:
    return 0;
  default:
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

// ONRUSH_NO_FOCUS=1 (test runs): the window opens behind other windows without taking focus and is
// never pushed to the front, so a test can run while the user plays something else. Real pad /
// keyboard input stays off because it is only read while this window is focused.
static bool NoFocusMode() {
  static int s = -1;
  if (s < 0) { char b[8] = {}; s = (GetEnvironmentVariableA("ONRUSH_NO_FOCUS", b, sizeof(b)) > 0 && b[0] == '1') ? 1 : 0; }
  return s == 1;
}

static HWND EnsureDesktopWindow() {
  if (g_desktopHwnd)
    return g_desktopHwnd;

  int width = 1920;
  int height = 1080;
  wchar_t envBuf[64] = {};
  if (GetEnvironmentVariableW(L"ONRUSH_RES_WIDTH", envBuf, 64) > 0) {
    int val = _wtoi(envBuf);
    if (val > 0)
      width = val;
  }
  if (GetEnvironmentVariableW(L"ONRUSH_RES_HEIGHT", envBuf, 64) > 0) {
    int val = _wtoi(envBuf);
    if (val > 0)
      height = val;
  }

  DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
  DWORD exStyle = WS_EX_APPWINDOW;

  if (GetEnvironmentVariableW(L"ONRUSH_WINDOW_MODE", envBuf, 64) > 0) {
    if (_wcsicmp(envBuf, L"Borderless") == 0 ||
        _wcsicmp(envBuf, L"Fullscreen") == 0) {
      windowStyle = WS_POPUP | WS_VISIBLE;
      exStyle = WS_EX_APPWINDOW | WS_EX_TOPMOST;
    }
  }

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = DesktopWindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"OnRushDesktopWindowClass";
  wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
  // Icon group 1 written into onrush-pc.exe by scripts/brand_exe.py (taskbar / Alt+Tab).
  wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
  wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int posX = (screenW > width) ? (screenW - width) / 2 : 0;
  int posY = (screenH > height) ? (screenH - height) / 2 : 0;

  if (NoFocusMode()) {
    windowStyle = WS_OVERLAPPEDWINDOW;  // shown below without activation
    exStyle = WS_EX_APPWINDOW | WS_EX_NOACTIVATE;  // never topmost; Windows never activates it (alt-tab / closing another app)
  }

  g_desktopHwnd = CreateWindowExW(
      exStyle, wc.lpszClassName, L"ONRUSH\x2122",
      windowStyle, posX, posY, width, height, nullptr,
      nullptr, wc.hInstance, nullptr);

  if (NoFocusMode()) {
    ShowWindow(g_desktopHwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_desktopHwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Windows never hands activation to a disabled window (e.g. when another app minimizes).
    EnableWindow(g_desktopHwnd, FALSE);
    log_line("[window] ONRUSH_NO_FOCUS: window opened in the background\r\n");
  } else {
    if (windowStyle & WS_POPUP) InitWindowedRestore(g_desktopHwnd, width, height);
    ShowWindow(g_desktopHwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_desktopHwnd);
    SetActiveWindow(g_desktopHwnd);
    SetFocus(g_desktopHwnd);
  }
  UpdateWindow(g_desktopHwnd);
  log_line("[bridge] Created desktop HWND: %p (%dx%d, style=0x%08lX)\r\n",
           g_desktopHwnd, width, height, windowStyle);
  return g_desktopHwnd;
}

class MockCoreDispatcher : public IInspectable {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    static const IID s_iidCoreDispatcher = {
        0x60db2fa8,
        0xb705,
        0x4fde,
        {0xa7, 0xd6, 0xeb, 0xbb, 0x18, 0x91, 0xd3, 0x9e}};
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == s_iidCoreDispatcher || riid == IID_IAgileObject) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    log_guid("[MockCoreDispatcher] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE get_HasThreadAccess(boolean *value) {
    if (value)
      *value = TRUE;
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE ProcessEvents(int options) {
    log_line("[bridge] CoreDispatcher::ProcessEvents(options=%d)\r\n", options);
    if (options == 2) // ProcessUntilQuit
    {
      MSG msg = {};
      while (g_desktopHwnd && IsWindow(g_desktopHwnd)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          if (msg.message == WM_QUIT)
            return S_OK;
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
      }
      return S_OK;
    } else // options == 3 (ProcessAllIfPresent), 0, 1: non-blocking pump
    {
      MSG msg = {};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
          return S_OK;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      return S_OK;
    }
  }

  virtual HRESULT STDMETHODCALLTYPE RunAsync(int priority, IUnknown *callback,
                                             void **asyncAction) {
    if (callback) {
      typedef HRESULT(STDMETHODCALLTYPE * PFN_Invoke)(IUnknown *);
      void **vtbl = *(void ***)callback;
      if (vtbl && vtbl[3]) {
        PFN_Invoke pfn = (PFN_Invoke)vtbl[3];
        pfn(callback);
      }
    }
    if (asyncAction) {
      *asyncAction = this;
      AddRef();
    }
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE RunIdleAsync(IUnknown *callback,
                                                 void **asyncAction) {
    return RunAsync(0, callback, asyncAction);
  }
};

static MockCoreDispatcher g_mockDispatcher;

class MockCoreWindow : public ICoreWindowEra, public ICoreWindowInterop {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_ICoreWindowEra || riid == IID_IAgileObject) {
      *ppv = static_cast<ICoreWindowEra *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ICoreWindowInterop) {
      *ppv = static_cast<ICoreWindowInterop *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND *hwnd) override {
    *hwnd = EnsureDesktopWindow();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_MessageHandled(boolean value) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE _abi_get_Bounds(RectF *rect) override {
    if (!rect)
      return E_POINTER;
    rect->X = 0;
    rect->Y = 0;
    rect->Width = 1920;
    rect->Height = 1080;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_get_Dispatcher(void **dispatcher) override {
    if (!dispatcher)
      return E_POINTER;
    *dispatcher = &g_mockDispatcher;
    g_mockDispatcher.AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_get_IsInputEnabled(boolean *value) override {
    *value = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_set_IsInputEnabled(boolean value) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_get_Visible(boolean *value) override {
    *value = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Activate(void) override {
    HWND hwnd = EnsureDesktopWindow();
    ShowWindow(hwnd, NoFocusMode() ? SW_SHOWNOACTIVATE : SW_SHOW);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Close(void) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetAsyncKeyState(int key, int *KeyState) override {
    *KeyState = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetKeyState(int key, int *KeyState) override {
    *KeyState = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_Activated(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_Activated(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_add_CharacterReceived(
      void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_CharacterReceived(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_Closed(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_Closed(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_InputEnabled(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_InputEnabled(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_KeyDown(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_KeyDown(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_KeyUp(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_KeyUp(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_PointerMoved(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_PointerMoved(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_add_PointerEntered(
      void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_PointerEntered(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_add_PointerExited(
      void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_PointerExited(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_SizeChanged(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_SizeChanged(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_add_VisibilityChanged(
      void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_VisibilityChanged(EventRegistrationToken token) override {
    return S_OK;
  }
};

static MockCoreWindow g_mockWindow;

class MockCoreWindowStatic : public ICoreWindowStatic {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_IActivationFactory || riid == IID_ICoreWindowStatic ||
        riid == IID_IAgileObject) {
      *ppv = static_cast<ICoreWindowStatic *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[MockCoreWindowStatic] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetForCurrentThread(ICoreWindowEra **ppWindow) override {
    log_line("[bridge] ICoreWindowStatic::GetForCurrentThread called -> "
             "returning g_mockWindow\r\n");
    if (!ppWindow)
      return E_POINTER;
    *ppWindow = &g_mockWindow;
    g_mockWindow.AddRef();

    HMODULE hExe = GetModuleHandleW(nullptr);
    if (hExe) {
      EnsureDXGIFactoryGlobal(hExe);
      void **ppFactoryGlobal = (void **)((uint8_t *)hExe + 0x01538AA8);
      if (ppFactoryGlobal && *ppFactoryGlobal) {
        WrapFactoryInstance((IDXGIFactory2 *)*ppFactoryGlobal);
      }
    }
    return S_OK;
  }
};

static MockCoreWindowStatic g_mockCoreWindowStatic;

// ============================================================================
// WinRT Activation Event Args & Delegate ABI
// (Windows.ApplicationModel.Activation)
// ============================================================================
// In the WinRT COM ABI (windows.applicationmodel.activation.h), every WinRT
// MIDL_INTERFACE inherits directly and solely from IInspectable (Slots 0..5).
// ILaunchActivatedEventArgs has a metadata "requires IActivatedEventArgs"
// contract (resolved via QueryInterface), NOT C++ single-vtable inheritance.
// Therefore:
// - IActivatedEventArgs       ({CF651713-CD08-4FD8-B697-A281B6544E2E}) has:
//     Slot 6 (+0x30): get_Kind(ActivationKind* value)
//     Slot 7 (+0x38): get_PreviousExecutionState(ApplicationExecutionState*
//     value) Slot 8 (+0x40): get_SplashScreen(ISplashScreen** value)
// - ILaunchActivatedEventArgs ({FBC93E26-A14A-4B4F-82B0-33BED920AF52}) has:
//     Slot 6 (+0x30): get_Arguments(HSTRING* value)
//     Slot 7 (+0x38): get_TileId(HSTRING* value)
// Disassembly of OnActivated (game+0x21B500) confirms:
// - game+0x21B573 calls Slot 6 (+0x30) on IActivatedEventArgs* -> get_Kind
// - game+0x219A80 calls QueryInterface(IID_ILaunchActivatedEventArgs)
// - game+0x21A9CE calls Slot 6 (+0x30) on the returned
// ILaunchActivatedEventArgs* -> get_Arguments

// {CF651713-CD08-4FD8-B697-A281B6544E2E}
static const GUID IID_IActivatedEventArgsEra = {
    0xcf651713,
    0xcd08,
    0x4fd8,
    {0xb6, 0x97, 0xa2, 0x81, 0xb6, 0x54, 0x4e, 0x2e}};

// {FBC93E26-A14A-4B4F-82B0-33BED920AF52}
static const GUID IID_ILaunchActivatedEventArgsEra = {
    0xfbc93e26,
    0xa14a,
    0x4b4f,
    {0x82, 0xb0, 0x33, 0xbe, 0xd9, 0x20, 0xaf, 0x52}};

enum ActivationKindEra : int {
  ActivationKindEra_Launch = 0,
  ActivationKindEra_Protocol = 4
};

enum ApplicationExecutionStateEra : int {
  ApplicationExecutionStateEra_NotRunning = 0,
  ApplicationExecutionStateEra_Running = 1,
  ApplicationExecutionStateEra_Suspended = 2,
  ApplicationExecutionStateEra_Terminated = 3,
  ApplicationExecutionStateEra_ClosedByUser = 4
};

MIDL_INTERFACE("CF651713-CD08-4FD8-B697-A281B6544E2E")
IActivatedEventArgsEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_Kind(ActivationKindEra * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_PreviousExecutionState(
      ApplicationExecutionStateEra * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SplashScreen(IInspectable * *value) = 0;
};

MIDL_INTERFACE("FBC93E26-A14A-4B4F-82B0-33BED920AF52")
ILaunchActivatedEventArgsEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_Arguments(HSTRING * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_TileId(HSTRING * value) = 0;
};

// ITypedEventHandler<CoreApplicationView*, IActivatedEventArgs*>
// Game delegate vtable at game+0x1416650:
//   Slot 0 (+0x00): QueryInterface (game+0x21BB80)
//   Slot 1 (+0x08): AddRef         (game+0x09FB90)
//   Slot 2 (+0x10): Release        (game+0x21BC40)
//   Slot 3 (+0x18): Invoke         (game+0x21DA80)
MIDL_INTERFACE("A856E674-B0B6-5BC3-BBA8-1BA06E40D4B5")
IActivatedEventHandlerEra : public IUnknown {
public:
  virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreApplicationViewEra * sender,
                                           IActivatedEventArgsEra * args) = 0;
};

class MockLaunchActivatedEventArgs : public IActivatedEventArgsEra,
                                     public ILaunchActivatedEventArgsEra {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_IActivatedEventArgsEra || riid == IID_IAgileObject) {
      *ppv = static_cast<IActivatedEventArgsEra *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ILaunchActivatedEventArgsEra) {
      *ppv = static_cast<ILaunchActivatedEventArgsEra *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[MockLaunchActivatedEventArgs] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // IActivatedEventArgsEra (Slots 6..8)
  HRESULT STDMETHODCALLTYPE get_Kind(ActivationKindEra *value) override {
    if (!value)
      return E_POINTER;
    *value = ActivationKindEra_Launch;
    log_line("[bridge] IActivatedEventArgs::get_Kind -> Launch (0)\r\n");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  get_PreviousExecutionState(ApplicationExecutionStateEra *value) override {
    if (!value)
      return E_POINTER;
    *value = ApplicationExecutionStateEra_NotRunning;
    log_line("[bridge] IActivatedEventArgs::get_PreviousExecutionState -> "
             "NotRunning (0)\r\n");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SplashScreen(IInspectable **value) override {
    if (!value)
      return E_POINTER;
    *value = nullptr;
    log_line("[bridge] IActivatedEventArgs::get_SplashScreen -> nullptr\r\n");
    return S_OK;
  }

  // ILaunchActivatedEventArgsEra (Slots 6..7 on second vtable)
  HRESULT STDMETHODCALLTYPE get_Arguments(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    // The game reads its options (e.g. -giveentitlements) from the launch
    // arguments (game+0x21B5A9 -> strtok -> option table 0x7D780), not from
    // the Win32 command line. Forward the process arguments, minus the
    // program name and the bridge's own flags.
    std::wstring args;
    {
      const wchar_t *p = GetCommandLineW();
      if (p) {
        if (*p == L'"') { ++p; while (*p && *p != L'"') ++p; if (*p) ++p; }
        else while (*p && *p != L' ' && *p != L'\t') ++p;
        while (*p) {
          while (*p == L' ' || *p == L'\t') ++p;
          if (!*p) break;
          const wchar_t *s = p;
          while (*p && *p != L' ' && *p != L'\t') ++p;
          std::wstring tok(s, p - s);
          if (_wcsicmp(tok.c_str(), L"-skipintro") == 0 || _wcsicmp(tok.c_str(), L"-gameplay") == 0 ||
              _wcsicmp(tok.c_str(), L"-unlockcosmetics") == 0 ||
              _wcsicmp(tok.c_str(), L"/gameplay") == 0)
            continue;
          if (!args.empty()) args += L' ';
          args += tok;
        }
      }
    }
    HRESULT hr = CreateHString(args.c_str(), value);
    log_line("[bridge] ILaunchActivatedEventArgs::get_Arguments -> hr=0x%08lX, "
             "hstr=%p args='%ls'\r\n",
             hr, *value, args.c_str());
    return hr;
  }
  HRESULT STDMETHODCALLTYPE get_TileId(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    HRESULT hr = CreateHString(L"App", value);
    log_line("[bridge] ILaunchActivatedEventArgs::get_TileId -> hr=0x%08lX, "
             "hstr=%p\r\n",
             hr, *value);
    return hr;
  }

  LONG GetRefCount() const { return m_ref; }
};

static MockLaunchActivatedEventArgs g_mockActivatedArgs;

static volatile bool g_inNativeActivationInit = false;
static volatile LONG g_nativeActivationAllocCount = 0;
static void LogAllocatorStateSnapshot(const char *phase, HMODULE hExe);

class MockCoreApplicationView : public ICoreApplicationViewEra {
  LONG m_ref = 1;
  IActivatedEventHandlerEra *m_activatedHandler = nullptr;
  EventRegistrationToken m_activatedToken = {0};
  INT64 m_nextTokenValue = 0;
  bool m_activatedFired = false;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_ICoreApplicationView || riid == IID_IAgileObject) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_CoreWindow(ICoreWindowEra **value) override {
    *value = &g_mockWindow;
    g_mockWindow.AddRef();
    return S_OK;
  }

  // Token & delegate lifetime treatment:
  // In IFrameworkView::Initialize (game+0x21B2A0), the game constructs the
  // C++/CX TypedEventHandler delegate (game+0x21A3A0) with refcount = 1, calls
  // add_Activated (game+0x21B334) passing a stack-local EventRegistrationToken
  // (&[rbp+0x30]), and immediately calls delegate->Release() (game+0x21B351 ->
  // game+0x21BC40). Calling AddRef() here retains ownership (refcount 1 -> 2 ->
  // 1 after Initialize's Release) so the delegate is not freed before
  // CoreApplicationDesktop::_abi_Run invokes it.
  HRESULT STDMETHODCALLTYPE
  add_Activated(void *handler, EventRegistrationToken *token) override {
    if (!token)
      return E_POINTER;
    token->value = 0;
    if (!handler) {
      log_line("[bridge] ERROR: MockCoreApplicationView::add_Activated called "
               "with null handler!\r\n");
      return E_INVALIDARG;
    }

    IActivatedEventHandlerEra *pNewHandler =
        reinterpret_cast<IActivatedEventHandlerEra *>(handler);
    ULONG newRef = pNewHandler->AddRef();

    if (m_activatedHandler) {
      m_activatedHandler->Release();
    }

    m_activatedHandler = pNewHandler;
    m_activatedToken.value = ++m_nextTokenValue;
    *token = m_activatedToken;

    log_line("[bridge] MockCoreApplicationView::add_Activated(handler=%p) -> "
             "AddRef=%lu, token=%lld\r\n",
             handler, newRef, (long long)token->value);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  remove_Activated(EventRegistrationToken token) override {
    log_line(
        "[bridge] MockCoreApplicationView::remove_Activated(token=%lld)\r\n",
        (long long)token.value);
    if (token.value != 0 && token.value == m_activatedToken.value &&
        m_activatedHandler) {
      ULONG remRef = m_activatedHandler->Release();
      log_line("[bridge] MockCoreApplicationView::remove_Activated released "
               "handler=%p (ref=%lu)\r\n",
               m_activatedHandler, remRef);
      m_activatedHandler = nullptr;
      m_activatedToken.value = 0;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_IsMain(boolean *value) override {
    *value = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsHosted(boolean *value) override {
    *value = FALSE;
    return S_OK;
  }

  HRESULT FireActivatedOnce() {
    if (m_activatedFired) {
      log_line("[bridge] MockCoreApplicationView::FireActivatedOnce skipped "
               "(already fired)\r\n");
      return S_OK;
    }
    if (!m_activatedHandler) {
      log_line("[bridge] WARNING: MockCoreApplicationView::FireActivatedOnce "
               "has no registered handler!\r\n");
      return E_UNEXPECTED;
    }

    m_activatedFired = true;
    IActivatedEventArgsEra *pArgs =
        static_cast<IActivatedEventArgsEra *>(&g_mockActivatedArgs);
    log_line(
        "[bridge] Invoking Activated delegate=%p (sender=%p, args=%p)...\r\n",
        m_activatedHandler, static_cast<ICoreApplicationViewEra *>(this),
        pArgs);
    HRESULT hr = m_activatedHandler->Invoke(
        static_cast<ICoreApplicationViewEra *>(this), pArgs);
    log_line("[bridge] Activated delegate->Invoke returned hr=0x%08lX\r\n", hr);
    return hr;
  }
};

static MockCoreApplicationView g_mockAppView;

// ICoreImmersiveApplication {1ADA0E3E-E4A2-4123-B451-DC96BF800419} (CoreApplication statics):
//   6 get_Views(IVectorView<CoreApplicationView>**), 7 CreateNewView(HSTRING, HSTRING, view**),
//   8 get_MainView(view**). Asked for after the EULA is accepted (exe+0x4F651A).
static const GUID IID_ICoreImmersiveApplicationEra =
    {0x1ada0e3e, 0xe4a2, 0x4123, {0xb4, 0x51, 0xdc, 0x96, 0xbf, 0x80, 0x04, 0x19}};

class MockAppViewVector : public IInspectable {  // IVectorView<CoreApplicationView> with one element
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override { if (!ppv) return E_POINTER; *ppv = this; return S_OK; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c = 0; if (i) *i = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { if (n) *n = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l = BaseTrust; return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, void **item) {
    if (!item) return E_POINTER;
    if (index != 0) { *item = nullptr; return E_BOUNDS; }
    *item = static_cast<ICoreApplicationViewEra *>(&g_mockAppView); return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *size) { if (!size) return E_POINTER; *size = 1; return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE IndexOf(void *value, UINT32 *index, boolean *found) {
    bool f = value == static_cast<ICoreApplicationViewEra *>(&g_mockAppView);
    if (index) *index = 0; if (found) *found = f; return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 cap, void **items, UINT32 *actual) {
    UINT32 n = 0;
    if (start == 0 && cap > 0 && items) { items[0] = static_cast<ICoreApplicationViewEra *>(&g_mockAppView); n = 1; }
    if (actual) *actual = n; return S_OK;
  }
};
static MockAppViewVector g_mockAppViewVector;

class MockCoreImmersiveApplication : public IInspectable {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override { if (!ppv) return E_POINTER; *ppv = this; return S_OK; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c = 0; if (i) *i = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override { if (n) *n = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l = BaseTrust; return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE get_Views(void **value) {
    // The game treats the slot-6 result as a CoreApplicationView (calls its slot 6 =
    // get_CoreWindow at exe+0x4F65BC), so hand back the main view, not a vector.
    log_line("[bridge] ICoreImmersiveApplication slot 6 -> main view\r\n");
    if (!value) return E_POINTER; *value = static_cast<ICoreApplicationViewEra *>(&g_mockAppView); return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE CreateNewView(HSTRING, HSTRING, void **view) {
    log_line("[bridge] ICoreImmersiveApplication::CreateNewView -> E_NOTIMPL\r\n");
    if (view) *view = nullptr; return E_NOTIMPL;
  }
  virtual HRESULT STDMETHODCALLTYPE get_MainView(void **value) {
    log_line("[bridge] ICoreImmersiveApplication::get_MainView -> mock view\r\n");
    if (!value) return E_POINTER; *value = static_cast<ICoreApplicationViewEra *>(&g_mockAppView); return S_OK;
  }
};
static MockCoreImmersiveApplication g_mockImmersiveApp;

class CoreApplicationDesktop : public ICoreApplicationEra,
                               public ICoreApplicationGpuPolicyEra,
                               public ICoreApplicationResourceAvailabilityEra {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_IActivationFactory || riid == IID_ICoreApplicationEra ||
        riid == IID_IAgileObject) {
      *ppv = static_cast<ICoreApplicationEra *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ICoreApplicationGpuPolicyEra) {
      *ppv = static_cast<ICoreApplicationGpuPolicyEra *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ICoreApplicationResourceAvailabilityEra) {
      *ppv = static_cast<ICoreApplicationResourceAvailabilityEra *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ICoreImmersiveApplicationEra) {
      *ppv = static_cast<IInspectable *>(&g_mockImmersiveApp);
      return S_OK;
    }
    log_guid("[bridge] CoreApplicationDesktop::QueryInterface unhandled GUID",
             &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE _abi_get_Id(HSTRING *value) override {
    *value = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_Suspending(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_Suspending(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_add_Resuming(void *handler, EventRegistrationToken *token) override {
    token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_remove_Resuming(EventRegistrationToken token) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_get_Properties(void **value) override {
    *value = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_GetCurrentView(ICoreApplicationViewEra **value) override {
    *value = &g_mockAppView;
    g_mockAppView.AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  _abi_RunWithActivationFactories(void *callback) override {
    return E_NOTIMPL;
  }

  // ICoreApplicationGpuPolicyEra
  HRESULT STDMETHODCALLTYPE
  get_DisableKinectGpuReservation(bool *value) override {
    if (value)
      *value = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  set_DisableKinectGpuReservation(bool value) override {
    return S_OK;
  }

  // ICoreApplicationResourceAvailabilityEra
  HRESULT STDMETHODCALLTYPE
  _abi_get_ResourceAvailability(int *resourceAvailability) override {
    if (resourceAvailability)
      *resourceAvailability = 0; // ResourceAvailability_Full
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_add_ResourceAvailabilityChanged(
      void *handler, EventRegistrationToken *token) override {
    if (token)
      token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE _abi_remove_ResourceAvailabilityChanged(
      EventRegistrationToken token) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  _abi_Run(IFrameworkViewSource *viewSource) override {
    log_line("[bridge] === CoreApplicationDesktop::Run called ===\r\n");
    if (!viewSource) {
      log_line("[bridge] ERROR: viewSource is null!\r\n");
      return E_INVALIDARG;
    }

    IFrameworkView *pView = nullptr;
    log_line("[bridge] Calling viewSource->CreateView(&pView)...\r\n");
    HRESULT hr = viewSource->CreateView(&pView);
    log_line("[bridge] CreateView returned hr=0x%08lX, pView=%p\r\n", hr,
             pView);
    if (FAILED(hr) || !pView)
      return FAILED(hr) ? hr : E_UNEXPECTED;

    if (!EnsureDesktopWindow()) {
      DWORD error = GetLastError();
      log_line("[bridge] ERROR: EnsureDesktopWindow failed (Win32=%lu)\r\n",
               error);
      pView->Release();
      return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }

    log_line("[bridge] Calling pView->Initialize...\r\n");
    hr = pView->Initialize(&g_mockAppView);
    log_line("[bridge] Initialize returned hr=0x%08lX\r\n", hr);
    if (FAILED(hr)) {
      pView->Release();
      return hr;
    }

    log_line("[bridge] Calling pView->SetWindow...\r\n");
    hr = pView->SetWindow(&g_mockWindow);
    log_line("[bridge] SetWindow returned hr=0x%08lX\r\n", hr);
    if (FAILED(hr)) {
      pView->Uninitialize();
      pView->Release();
      return hr;
    }

    log_line("[bridge] Calling pView->Load...\r\n");
    hr = pView->Load(nullptr);
    log_line("[bridge] Load returned hr=0x%08lX\r\n", hr);
    if (FAILED(hr)) {
      pView->Uninitialize();
      pView->Release();
      return hr;
    }

    HMODULE hExe = GetModuleHandleW(nullptr);
    LogAllocatorStateSnapshot("PRE-OnActivated", hExe);

    log_line("[bridge] Firing CoreApplicationView::Activated once (after Load, "
             "before Run)...\r\n");
    InterlockedExchange(&g_nativeActivationAllocCount, 0);
    g_inNativeActivationInit = true;
    HRESULT hrActivated = g_mockAppView.FireActivatedOnce();
    g_inNativeActivationInit = false;
    LONG initAllocCount =
        InterlockedCompareExchange(&g_nativeActivationAllocCount, 0, 0);
    log_line("[bridge] OnActivated completed (hr=0x%08lX, BridgeAlloc calls "
             "during native init=%ld).\r\n",
             hrActivated, (long)initAllocCount);
    log_line(
        "[bridge] NOTE: PatchAllocThunk remains active (+0x069B80 -> "
        "BridgeAlloc, +0x098EC0/+0x099040 -> DetourThreadAlloc/Free); runtime "
        "compatibility with native allocators is not yet proven.\r\n");

    LogAllocatorStateSnapshot("POST-OnActivated", hExe);
    if (FAILED(hrActivated)) {
      pView->Uninitialize();
      pView->Release();
      return hrActivated;
    }

    log_line("[bridge] Calling pView->Run (entering game view loop)...\r\n");
    hr = pView->Run();
    log_line("[bridge] pView->Run returned hr=0x%08lX\r\n", hr);

    pView->Uninitialize();
    pView->Release();
    return hr;
  }
};

static CoreApplicationDesktop g_coreAppDesktop;

// ============================================================================
// Windows.Storage.ApplicationData Desktop Implementation
// ============================================================================

MIDL_INTERFACE("4207A996-CA2F-42F7-BDE8-8B10457A7F30")
IStorageItemEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Path(HSTRING * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Attributes(
      ABI::Windows::Storage::FileAttributes * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DateCreated(
      ABI::Windows::Foundation::DateTime * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsOfType(
      ABI::Windows::Storage::StorageItemTypes type, boolean * value) = 0;
};

MIDL_INTERFACE("72D1CB78-B3EF-4F75-A80B-6FD9DAE2944B")
IStorageFolderEra : public IStorageItemEra{public : };

class StorageFolderDesktop : public IStorageFolderEra {
  LONG m_ref = 1;
  wchar_t m_savePath[MAX_PATH];

public:
  StorageFolderDesktop() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t *slash = wcsrchr(exePath, L'\\');
    if (slash)
      *slash = L'\0';
    swprintf_s(m_savePath, MAX_PATH, L"%ls\\save", exePath);
    CreateDirectoryW(m_savePath, nullptr);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == __uuidof(IStorageFolderEra) ||
        riid == __uuidof(IStorageItemEra) || riid == IID_IAgileObject) {
      *ppv = static_cast<IStorageFolderEra *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[StorageFolderDesktop] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Name(HSTRING *value) override {
    return CreateHString(L"save", value);
  }
  HRESULT STDMETHODCALLTYPE get_Path(HSTRING *value) override {
    log_line("[bridge] IStorageFolderEra::get_Path returning '%ls'\r\n",
             m_savePath);
    return CreateHString(m_savePath, value);
  }
  HRESULT STDMETHODCALLTYPE
  get_Attributes(ABI::Windows::Storage::FileAttributes *value) override {
    if (value)
      *value = ABI::Windows::Storage::FileAttributes_Directory;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  get_DateCreated(ABI::Windows::Foundation::DateTime *value) override {
    if (value)
      value->UniversalTime = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IsOfType(
      ABI::Windows::Storage::StorageItemTypes type, boolean *value) override {
    if (value)
      *value = (type == ABI::Windows::Storage::StorageItemTypes_Folder);
    return S_OK;
  }
};

static StorageFolderDesktop g_mockStorageFolder;

MIDL_INTERFACE("C3DA6FB7-B744-4B45-B0B8-223A0938D0DC")
IApplicationDataEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_LocalFolder(IStorageFolderEra *
                                                    *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RoamingFolder(IStorageFolderEra *
                                                      *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_TemporaryFolder(IStorageFolderEra *
                                                        *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_LocalSettings(
      ABI::Windows::Storage::IApplicationDataContainer * *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RoamingSettings(
      ABI::Windows::Storage::IApplicationDataContainer * *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RoamingStorageQuota(UINT64 * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_DataChanged(
      void *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_DataChanged(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE SignalDataChanged() = 0;
  virtual HRESULT STDMETHODCALLTYPE ClearAsync(
      ABI::Windows::Storage::ApplicationDataLocality locality,
      ABI::Windows::Foundation::IAsyncAction * *operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE ClearAllAsync(
      ABI::Windows::Foundation::IAsyncAction * *operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Version(UINT32 * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetVersionAsync(
      UINT32 desiredVersion, void *handler,
      ABI::Windows::Foundation::IAsyncAction **operation) = 0;
};

class ApplicationDataDesktop : public IApplicationDataEra {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == __uuidof(IApplicationDataEra) || riid == IID_IAgileObject) {
      *ppv = static_cast<IApplicationDataEra *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[ApplicationDataDesktop] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  get_LocalFolder(IStorageFolderEra **value) override {
    log_line("[bridge] IApplicationDataEra::get_LocalFolder\r\n");
    if (!value)
      return E_POINTER;
    *value = &g_mockStorageFolder;
    g_mockStorageFolder.AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  get_RoamingFolder(IStorageFolderEra **value) override {
    log_line("[bridge] IApplicationDataEra::get_RoamingFolder\r\n");
    if (!value)
      return E_POINTER;
    *value = &g_mockStorageFolder;
    g_mockStorageFolder.AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  get_TemporaryFolder(IStorageFolderEra **value) override {
    log_line("[bridge] IApplicationDataEra::get_TemporaryFolder\r\n");
    if (!value)
      return E_POINTER;
    *value = &g_mockStorageFolder;
    g_mockStorageFolder.AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_LocalSettings(
      ABI::Windows::Storage::IApplicationDataContainer **value) override {
    if (value)
      *value = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_RoamingSettings(
      ABI::Windows::Storage::IApplicationDataContainer **value) override {
    if (value)
      *value = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_RoamingStorageQuota(UINT64 *value) override {
    if (value)
      *value = 100 * 1024 * 1024;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  add_DataChanged(void *, EventRegistrationToken *token) override {
    if (token)
      token->value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  remove_DataChanged(EventRegistrationToken) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SignalDataChanged() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE
  ClearAsync(ABI::Windows::Storage::ApplicationDataLocality,
             ABI::Windows::Foundation::IAsyncAction **) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  ClearAllAsync(ABI::Windows::Foundation::IAsyncAction **) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Version(UINT32 *value) override {
    if (value)
      *value = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVersionAsync(
      UINT32, void *, ABI::Windows::Foundation::IAsyncAction **) override {
    return S_OK;
  }
};

static ApplicationDataDesktop g_mockApplicationData;

class ApplicationDataStaticsDesktop
    : public IActivationFactory,
      public ABI::Windows::Storage::IApplicationDataStatics {
  LONG m_ref = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == __uuidof(IActivationFactory) || riid == IID_IAgileObject) {
      *ppv = static_cast<IActivationFactory *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(ABI::Windows::Storage::IApplicationDataStatics)) {
      *ppv =
          static_cast<ABI::Windows::Storage::IApplicationDataStatics *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[ApplicationDataStaticsDesktop] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    *level = BaseTrust;
    return S_OK;
  }

  // IActivationFactory
  HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **instance) override {
    log_line(
        "[bridge] IActivationFactory::ActivateInstance(ApplicationData)\r\n");
    if (!instance)
      return E_POINTER;
    *instance = static_cast<IInspectable *>(&g_mockApplicationData);
    g_mockApplicationData.AddRef();
    return S_OK;
  }

  // IApplicationDataStatics
  HRESULT STDMETHODCALLTYPE
  get_Current(ABI::Windows::Storage::IApplicationData **value) override {
    log_line("[bridge] IApplicationDataStatics::get_Current\r\n");
    if (!value)
      return E_POINTER;
    *value = reinterpret_cast<ABI::Windows::Storage::IApplicationData *>(
        &g_mockApplicationData);
    g_mockApplicationData.AddRef();
    return S_OK;
  }
};

static ApplicationDataStaticsDesktop g_mockApplicationDataStatics;

// ============================================================================
// Mock Windows.Xbox.Graphics.Display.DisplayConfiguration
// ============================================================================

struct MockHdrModeInfo : public IInspectable {
  ULONG refCount = 1;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refCount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&refCount);
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // Slot 6 (+0x30): get_IsTvHdrSupported(boolean* value)
  virtual HRESULT STDMETHODCALLTYPE get_IsTvHdrSupported(boolean *value) {
    log_line(
        "[bridge] HdrModeInfo::get_IsTvHdrSupported -> FALSE (SDR mode)\r\n");
    if (value)
      *value = FALSE;
    return S_OK;
  }
  // Slot 7 (+0x38): get_IsTvDolbyVisionSupported(boolean* value)
  virtual HRESULT STDMETHODCALLTYPE Slot7(boolean *value) {
    if (value)
      *value = FALSE;
    return S_OK;
  }
  // Slot 8 (+0x40): get_IsAppAllowedToRenderHdr(boolean* value)
  virtual HRESULT STDMETHODCALLTYPE Slot8(boolean *value) {
    if (value)
      *value = FALSE;
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot9(void *a = nullptr) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Slot10(void *a = nullptr) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Slot11(void *a = nullptr) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Slot12(void *a = nullptr) { return S_OK; }
};
static MockHdrModeInfo g_mockHdrModeInfo;

struct MockDisplayConfiguration;
static void UnblockDisplayPhase(void *handler);

struct MockDisplayConfiguration : public IInspectable {
  ULONG refCount = 1;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refCount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&refCount);
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // Slot 6 (+0x30): add_DisplayChanged / put_Completed
  virtual HRESULT STDMETHODCALLTYPE add_DisplayChanged(void *handler = nullptr,
                                                       void *extra = nullptr) {
    log_line("[bridge] DisplayConfiguration::add_DisplayChanged(handler=%p, "
             "extra=%p)\r\n",
             handler, extra);
    // extra is ABI-opaque. In the supplied log it points into the bridge's
    // read-only image, so treating it as an output token caused a caught AV.
    (void)extra;
    UnblockDisplayPhase(handler);
    return S_OK;
  }
  // Slot 7 (+0x38): remove_DisplayChanged / get_Completed
  virtual HRESULT STDMETHODCALLTYPE remove_DisplayChanged(void *a = nullptr,
                                                          void *b = nullptr) {
    log_line("[bridge] DisplayConfiguration::remove_DisplayChanged(%p)\r\n", a);
    return S_OK;
  }
  // Slot 8 (+0x40): get_CurrentDisplayMode / GetResults
  virtual HRESULT STDMETHODCALLTYPE
  get_CurrentDisplayMode(void *pOut = nullptr) {
    log_line("[bridge] DisplayConfiguration::Slot8 (get_CurrentDisplayMode / "
             "GetResults) pOut=%p\r\n",
             pOut);
    if (pOut) {
      *(void **)pOut = &g_mockHdrModeInfo;
      g_mockHdrModeInfo.AddRef();
    }
    return S_OK;
  }
  // Slot 9 (+0x48): get_Id
  virtual HRESULT STDMETHODCALLTYPE Slot9(unsigned int *id = nullptr) {
    if (id)
      *id = 1;
    return S_OK;
  }
  // Slot 10 (+0x50): get_Status
  virtual HRESULT STDMETHODCALLTYPE Slot10(int *status = nullptr) {
    if (status)
      *status = 1; // 1 = Completed
    return S_OK;
  }
  // Slot 11 (+0x58): get_ErrorCode
  virtual HRESULT STDMETHODCALLTYPE Slot11(HRESULT *errorCode = nullptr) {
    if (errorCode)
      *errorCode = S_OK;
    return S_OK;
  }
  // Slot 12 (+0x60): Cancel
  virtual HRESULT STDMETHODCALLTYPE Slot12() { return S_OK; }
  // Slot 13 (+0x68): Close
  virtual HRESULT STDMETHODCALLTYPE Slot13() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Slot14(void *a = nullptr,
                                           void *b = nullptr) {
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot15(void *a = nullptr,
                                           void *b = nullptr) {
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot16(void *a = nullptr,
                                           void *b = nullptr) {
    return S_OK;
  }
};
static MockDisplayConfiguration g_mockDisplayConfiguration;

static void UnblockDisplayPhase(void *handler) {
  uintptr_t hExe = (uintptr_t)GetModuleHandleW(nullptr);
  if (!hExe)
    return;

  log_line("[bridge] === UnblockDisplayPhase START ===\r\n");

  if (handler) {
    __try {
      void **words = (void **)handler;
      void **vtbl0 = (void **)words[0];
      log_line("[bridge] handler=%p, vtbl0=%p\r\n", handler, vtbl0);
      if (vtbl0 && vtbl0[3]) {
        typedef HRESULT(STDMETHODCALLTYPE *
                        PFN_Invoke)(void *self, void *sender, int status);
        PFN_Invoke pfn = (PFN_Invoke)vtbl0[3];
        log_line("[bridge] Invoking delegate at vtbl0[3]=%p with status=1 "
                 "(Completed)...\r\n",
                 pfn);
        HRESULT hr = pfn(handler, &g_mockDisplayConfiguration, 1);
        log_line("[bridge] Delegate invoke returned hr=0x%08lX\r\n",
                 (unsigned long)hr);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      log_line("[bridge] Exception invoking delegate handler!\r\n");
    }
  }
  log_line("[bridge] === UnblockDisplayPhase END ===\r\n");
}

struct MockDisplayConfigurationStatics : public IActivationFactory {
  ULONG refCount = 1;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refCount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&refCount);
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // IActivationFactory Slot 6 (+0x30): ActivateInstance(IInspectable**
  // instance)
  HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **instance) override {
    log_line("[bridge] DisplayConfigurationStatics::ActivateInstance\r\n");
    if (instance) {
      *instance = &g_mockDisplayConfiguration;
      g_mockDisplayConfiguration.AddRef();
    }
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE add_DisplayChanged(void *handler = nullptr,
                                                       void *extra = nullptr) {
    log_line("[bridge] DisplayConfigurationStatics::add_DisplayChanged(%p)\r\n",
             handler);
    UnblockDisplayPhase(handler);
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE remove_DisplayChanged(void *a = nullptr,
                                                          void *b = nullptr) {
    log_line(
        "[bridge] DisplayConfigurationStatics::remove_DisplayChanged(%p)\r\n",
        a);
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot8(void *a = nullptr,
                                          void *b = nullptr) {
    if (a)
      *(void **)a = &g_mockDisplayConfiguration;
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot9(void *a = nullptr,
                                          void *b = nullptr) {
    if (a)
      *(void **)a = &g_mockDisplayConfiguration;
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot10(void *a = nullptr,
                                           void *b = nullptr) {
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot11(void *a = nullptr,
                                           void *b = nullptr) {
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Slot12(void *a = nullptr,
                                           void *b = nullptr) {
    return S_OK;
  }
};
static MockDisplayConfigurationStatics g_mockDisplayConfigurationStatics;

// ============================================================================
// Windows.System.Profile.AnalyticsInfo & AnalyticsVersionInfo Typed Mocks
// ============================================================================
// Official Windows SDK (<windows.system.profile.h>):
// - ABI::Windows::System::Profile::IAnalyticsInfoStatics
//     Desktop SDK IID: {1D5EE066-188D-5BA9-4387-ACAEB0E7E305}
//     Xbox ERA IID   : {8ED5EE10-C3C2-410C-A3C7-32323C5B25C3} (queried at
//     game+0x063C05 / +0x0639AB) Inherits directly from IInspectable (Slots
//     0..5):
//       Slot 6 (+0x30): get_VersionInfo(IAnalyticsVersionInfo** value)  [called
//       at game+0x06494E] Slot 7 (+0x38): get_DeviceForm(HSTRING* value)
// - ABI::Windows::System::Profile::IAnalyticsVersionInfo
//     Official IID   : {926130B8-9955-4C74-BDC1-7CD0DECF9B03}
//     Inherits directly from IInspectable (Slots 0..5):
//       Slot 6 (+0x30): get_DeviceFamily(HSTRING* value)
//       Slot 7 (+0x38): get_DeviceFamilyVersion(HSTRING* value)         [called
//       at game+0x06455E]
//
// In game+0x064790 (tail-called by game+0x21DC90 at +0x21DD6C):
//   1. Calls get_VersionInfo(&[rsp+0x68]) (Slot 6, +0x30)
//   2. Calls get_DeviceFamilyVersion(&[rsp+0x60]) (Slot 7, +0x38, RDX =
//   HSTRING*)
//   3. Calls WindowsGetStringRawBuffer([rsp+0x60], nullptr) -> stores wchar_t*
//   in RBX (+0x0647BD)
//   4. Calls WindowsDeleteString([rsp+0x60]) (+0x0647CF) BEFORE calling
//   wcstombs(buf, RBX, 31) (+0x064805)
//   5. Calls strtoull(buf, nullptr, 10) (+0x06481B)
// To keep RBX valid across the early WindowsDeleteString at +0x0647CF,
// MockAnalyticsVersionInfo retains a pinned HSTRING reference and returns a
// duplicated handle via WindowsDuplicateString.

// {1D5EE066-188D-5BA9-4387-ACAEB0E7E305}
static const GUID IID_IAnalyticsInfoStaticsSdk = {
    0x1d5ee066,
    0x188d,
    0x5ba9,
    {0x43, 0x87, 0xac, 0xae, 0xb0, 0xe7, 0xe3, 0x05}};

// {8ED5EE10-C3C2-410C-A3C7-32323C5B25C3}
static const GUID IID_IAnalyticsInfoStaticsEra = {
    0x8ed5ee10,
    0xc3c2,
    0x410c,
    {0xa3, 0xc7, 0x32, 0x32, 0x3c, 0x5b, 0x25, 0xc3}};

// {926130B8-9955-4C74-BDC1-7CD0DECF9B03}
static const GUID IID_IAnalyticsVersionInfoEra = {
    0x926130b8,
    0x9955,
    0x4c74,
    {0xbd, 0xc1, 0x7c, 0xd0, 0xde, 0xcf, 0x9b, 0x03}};

typedef HRESULT(WINAPI *PFN_WindowsDuplicateString)(HSTRING string,
                                                    HSTRING *newString);
typedef HRESULT(WINAPI *PFN_WindowsCreateStringReference)(
    PCWSTR sourceString, UINT32 length, HSTRING_HEADER *hstringHeader,
    HSTRING *string);
static PFN_WindowsDuplicateString g_pfnWindowsDuplicateString = nullptr;
static PFN_WindowsCreateStringReference g_pfnWindowsCreateStringReference =
    nullptr;
static bool g_bResolvedDuplicateString = false;
static bool g_bResolvedCreateStringRef = false;

static HRESULT CreateDuplicatedPinnedHString(
    const wchar_t *staticLiteral, HSTRING *pPinnedSlot,
    HSTRING_HEADER *pStaticFallbackHeader, HSTRING *outStr) {
  if (!outStr)
    return E_POINTER;
  *outStr = nullptr;
  if (!staticLiteral || !pPinnedSlot)
    return E_INVALIDARG;

  if (!*pPinnedSlot) {
    HRESULT hr = CreateHString(staticLiteral, pPinnedSlot);
    if (FAILED(hr)) {
      *pPinnedSlot = nullptr;
    }
  }

  HMODULE hCb = GetModuleHandleW(L"combase.dll");
  if (!g_bResolvedDuplicateString) {
    if (hCb)
      g_pfnWindowsDuplicateString = (PFN_WindowsDuplicateString)GetProcAddress(
          hCb, "WindowsDuplicateString");
    g_bResolvedDuplicateString = true;
  }

  if (g_pfnWindowsDuplicateString && *pPinnedSlot) {
    HRESULT hrDup = g_pfnWindowsDuplicateString(*pPinnedSlot, outStr);
    if (SUCCEEDED(hrDup) && *outStr)
      return hrDup;
    *outStr = nullptr;
  }

  // Fallback: use WindowsCreateStringReference with static backing text and
  // persistent HSTRING_HEADER. Never fall back to a separately allocated
  // CreateHString whose buffer would be freed by WindowsDeleteString.
  if (!g_bResolvedCreateStringRef) {
    if (hCb)
      g_pfnWindowsCreateStringReference =
          (PFN_WindowsCreateStringReference)GetProcAddress(
              hCb, "WindowsCreateStringReference");
    g_bResolvedCreateStringRef = true;
  }

  if (g_pfnWindowsCreateStringReference && pStaticFallbackHeader) {
    HRESULT hrRef = g_pfnWindowsCreateStringReference(
        staticLiteral, (UINT32)wcslen(staticLiteral), pStaticFallbackHeader,
        outStr);
    if (SUCCEEDED(hrRef) && *outStr)
      return hrRef;
    *outStr = nullptr;
    return FAILED(hrRef) ? hrRef : E_FAIL;
  }

  return E_FAIL;
}

MIDL_INTERFACE("926130B8-9955-4C74-BDC1-7CD0DECF9B03")
IAnalyticsVersionInfoEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_DeviceFamily(
      HSTRING * value) = 0; // Slot 6 (+0x30)
  virtual HRESULT STDMETHODCALLTYPE get_DeviceFamilyVersion(
      HSTRING * value) = 0; // Slot 7 (+0x38)
};

MIDL_INTERFACE("8ED5EE10-C3C2-410C-A3C7-32323C5B25C3")
IAnalyticsInfoStaticsEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_VersionInfo(
      IAnalyticsVersionInfoEra * *value) = 0; // Slot 6 (+0x30)
  virtual HRESULT STDMETHODCALLTYPE get_DeviceForm(HSTRING *
                                                   value) = 0; // Slot 7 (+0x38)
};

// Encodes Windows 10.0.17134.0: (10ULL << 48) | (0ULL << 32) | (17134ULL << 16)
// | 0ULL = 2814750890000384
static constexpr const wchar_t kAnalyticsDeviceFamily[] = L"Windows.Xbox";
static constexpr const wchar_t kAnalyticsDeviceFamilyVersion[] =
    L"2814750890000384";
static constexpr const wchar_t kAnalyticsDeviceForm[] = L"Xbox One";

class MockAnalyticsVersionInfo : public IAnalyticsVersionInfoEra {
  LONG m_ref = 1;
  HSTRING m_pinnedFamily = nullptr;
  HSTRING m_pinnedVersion = nullptr;
  HSTRING_HEADER m_fallbackHeaderFamily = {};
  HSTRING_HEADER m_fallbackHeaderVersion = {};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_IAnalyticsVersionInfoEra || riid == IID_IAgileObject) {
      *ppv = static_cast<IAnalyticsVersionInfoEra *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[MockAnalyticsVersionInfo] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // Slot 6 (+0x30): get_DeviceFamily(HSTRING* value)
  HRESULT STDMETHODCALLTYPE get_DeviceFamily(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    HRESULT hr =
        CreateDuplicatedPinnedHString(kAnalyticsDeviceFamily, &m_pinnedFamily,
                                      &m_fallbackHeaderFamily, value);
    log_line("[bridge] IAnalyticsVersionInfo::get_DeviceFamily(out=%p) -> "
             "'%ls' (hr=0x%08lX, hstr=%p)\r\n",
             value, kAnalyticsDeviceFamily, hr, *value);
    return hr;
  }

  // Slot 7 (+0x38): get_DeviceFamilyVersion(HSTRING* value)
  HRESULT STDMETHODCALLTYPE get_DeviceFamilyVersion(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    HRESULT hr = CreateDuplicatedPinnedHString(kAnalyticsDeviceFamilyVersion,
                                               &m_pinnedVersion,
                                               &m_fallbackHeaderVersion, value);
    log_line("[bridge] IAnalyticsVersionInfo::get_DeviceFamilyVersion(out=%p) "
             "-> '%ls' (hr=0x%08lX, hstr=%p)\r\n",
             value, kAnalyticsDeviceFamilyVersion, hr, *value);
    return hr;
  }

  LONG GetRefCount() const { return m_ref; }
};

static MockAnalyticsVersionInfo g_mockAnalyticsVersionInfo;

class MockAnalyticsInfoStatics : public IAnalyticsInfoStaticsEra {
  LONG m_ref = 1;
  HSTRING m_pinnedForm = nullptr;
  HSTRING_HEADER m_fallbackHeaderForm = {};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_IAnalyticsInfoStaticsEra ||
        riid == IID_IAnalyticsInfoStaticsSdk || riid == IID_IAgileObject) {
      *ppv = static_cast<IAnalyticsInfoStaticsEra *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[MockAnalyticsInfoStatics] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // Slot 6 (+0x30): get_VersionInfo(IAnalyticsVersionInfoEra** value)
  HRESULT STDMETHODCALLTYPE
  get_VersionInfo(IAnalyticsVersionInfoEra **value) override {
    if (!value)
      return E_POINTER;
    *value = &g_mockAnalyticsVersionInfo;
    g_mockAnalyticsVersionInfo.AddRef();
    log_line(
        "[bridge] IAnalyticsInfoStatics::get_VersionInfo(out=%p) -> %p\r\n",
        value, *value);
    return S_OK;
  }

  // Slot 7 (+0x38): get_DeviceForm(HSTRING* value)
  HRESULT STDMETHODCALLTYPE get_DeviceForm(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    HRESULT hr = CreateDuplicatedPinnedHString(
        kAnalyticsDeviceForm, &m_pinnedForm, &m_fallbackHeaderForm, value);
    log_line("[bridge] IAnalyticsInfoStatics::get_DeviceForm(out=%p) -> '%ls' "
             "(hr=0x%08lX, hstr=%p)\r\n",
             value, kAnalyticsDeviceForm, hr, *value);
    return hr;
  }

  LONG GetRefCount() const { return m_ref; }
};

static MockAnalyticsInfoStatics g_mockAnalyticsInfoStatics;

#include "mock_store.inc"

// ============================================================================
// Mock Tipado para Windows.Xbox.Services.XboxLiveConfiguration
// ============================================================================
// Em game+0x507180 (xbox::services::xbox_live_app_config::read), chamada por
// game+0x4673D0 (+0x4674DB) <- game+0x4EC130 (+0x4EC1A2), o jogo ativa
// L"Windows.Xbox.Services.XboxLiveConfiguration" três vezes com o IID Xbox ERA
// IXboxLiveConfigurationStatics {00193376-6A84-4C67-84A5-3B8A8AAEA712}:
//   - Bloco 1 (game+0x507254): chama Slot 8 (+0x40) get_SandboxId(HSTRING*
//   value)
//     e copia para [r15 + 0x48] (m_sandbox), comparado via _wcsicmp contra
//     L"RETAIL" (game+0xF47010) em game+0x4830A3 e game+0x4D2500, e contra
//     "CERT" / "CERT.DEBUG" / "CMSW." em game+0x08853C..0x088730.
//   - Bloco 2 (game+0x507354): chama Slot 7 (+0x38)
//   get_PrimaryServiceConfigId(HSTRING* value)
//     e copia para [r15 + 0x68] (m_scid), passado como 1º argumento (PCWSTR
//     serviceConfigurationId) para XboxIntegratedMultiplayer.dll!XimInitialize
//     em game+0x0829E5..0x082A7F.
//   - Bloco 3 (game+0x507457): chama Slot 6 (+0x30) get_TitleId(HSTRING*
//   value),
//     converte em base 10 via wcstol(..., nullptr, 10) em game+0x507532 e grava
//     o uint32_t resultante (0x071190C6 = 118591686) em [r15 + 0x20]
//     (m_titleId).

// {00193376-6A84-4C67-84A5-3B8A8AAEA712}
static const GUID IID_IXboxLiveConfigurationStaticsEra = {
    0x00193376,
    0x6a84,
    0x4c67,
    {0x84, 0xa5, 0x3b, 0x8a, 0x8a, 0xae, 0xa7, 0x12}};

MIDL_INTERFACE("00193376-6A84-4C67-84A5-3B8A8AAEA712")
IXboxLiveConfigurationStaticsEra : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_TitleId(HSTRING *
                                                value) = 0; // Slot 6 (+0x30)
  virtual HRESULT STDMETHODCALLTYPE get_PrimaryServiceConfigId(
      HSTRING * value) = 0; // Slot 7 (+0x38)
  virtual HRESULT STDMETHODCALLTYPE get_SandboxId(HSTRING *
                                                  value) = 0; // Slot 8 (+0x40)
};

// Valores extraídos de stage\Mount\appxmanifest.xml (linha 44):
//   <mx:XboxLive TitleId="071190C6"
//   PrimaryServiceConfigId="55f70100-c1b1-4bd8-916d-e23c071190c6" /> 0x071190C6
//   em decimal = 118591686 (convertido por wcstol(..., nullptr, 10) em
//   game+0x507532).
// SandboxId L"RETAIL": string literal presente em game+0xF47010 e comparada via
// _wcsicmp contra [app_config + 0x48] em game+0x4830A3 e game+0x4D2500 (além
// das checagens "CERT"/"CERT.DEBUG"/"CMSW." em game+0x08853C..0x088730); ainda
// assim anotado como hipótese de ambiente de execução no desktop.
static constexpr const wchar_t kXboxLiveTitleIdDecimal[] = L"118591686";
static constexpr const wchar_t kXboxLivePrimaryServiceConfigId[] =
    L"55f70100-c1b1-4bd8-916d-e23c071190c6";
static constexpr const wchar_t kXboxLiveSandboxId[] = L"RETAIL";

class MockXboxLiveConfigurationStatics
    : public IXboxLiveConfigurationStaticsEra {
  LONG m_ref = 1;
  HSTRING m_pinnedTitleId = nullptr;
  HSTRING m_pinnedScid = nullptr;
  HSTRING m_pinnedSandboxId = nullptr;
  HSTRING_HEADER m_fallbackHeaderTitleId = {};
  HSTRING_HEADER m_fallbackHeaderScid = {};
  HSTRING_HEADER m_fallbackHeaderSandboxId = {};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == IID_IInspectable ||
        riid == IID_IXboxLiveConfigurationStaticsEra ||
        riid == IID_IAgileObject) {
      *ppv = static_cast<IXboxLiveConfigurationStaticsEra *>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[MockXboxLiveConfigurationStatics] QI unknown", &riid);
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return InterlockedDecrement(&m_ref);
  }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count)
      *count = 0;
    if (iids)
      *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name)
      *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level)
      *level = BaseTrust;
    return S_OK;
  }

  // Slot 6 (+0x30): get_TitleId(HSTRING* value)
  HRESULT STDMETHODCALLTYPE get_TitleId(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    *value = nullptr;
    HRESULT hr =
        CreateDuplicatedPinnedHString(kXboxLiveTitleIdDecimal, &m_pinnedTitleId,
                                      &m_fallbackHeaderTitleId, value);
    log_line("[bridge] IXboxLiveConfigurationStatics::get_TitleId(out=%p) -> "
             "'%ls' (hr=0x%08lX, hstr=%p)\r\n",
             value, kXboxLiveTitleIdDecimal, hr, *value);
    return hr;
  }

  // Slot 7 (+0x38): get_PrimaryServiceConfigId(HSTRING* value)
  HRESULT STDMETHODCALLTYPE
  get_PrimaryServiceConfigId(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    *value = nullptr;
    HRESULT hr = CreateDuplicatedPinnedHString(kXboxLivePrimaryServiceConfigId,
                                               &m_pinnedScid,
                                               &m_fallbackHeaderScid, value);
    log_line("[bridge] "
             "IXboxLiveConfigurationStatics::get_PrimaryServiceConfigId(out=%p)"
             " -> '%ls' (hr=0x%08lX, hstr=%p)\r\n",
             value, kXboxLivePrimaryServiceConfigId, hr, *value);
    return hr;
  }

  // Slot 8 (+0x40): get_SandboxId(HSTRING* value)
  HRESULT STDMETHODCALLTYPE get_SandboxId(HSTRING *value) override {
    if (!value)
      return E_POINTER;
    *value = nullptr;
    HRESULT hr =
        CreateDuplicatedPinnedHString(kXboxLiveSandboxId, &m_pinnedSandboxId,
                                      &m_fallbackHeaderSandboxId, value);
    log_line("[bridge] IXboxLiveConfigurationStatics::get_SandboxId(out=%p) -> "
             "'%ls' (hr=0x%08lX, hstr=%p)\r\n",
             value, kXboxLiveSandboxId, hr, *value);
    return hr;
  }

  LONG GetRefCount() const { return m_ref; }
};

static MockXboxLiveConfigurationStatics g_mockXboxLiveConfigurationStatics;

// ============================================================================
// Windows.Xbox.Input.Gamepad & IVectorView<IGamepad*> Typed Mocks
// ============================================================================
// Xbox ERA IID: {8C4C1BCD-FDAB-4BF8-BDE9-ABB2B1D6FDD3} (queried at
// game+0x0C1B15)
//
// In game+0x0C7910 (lowapi/input/xbox/platform-xbox.cpp):
//   1. Calls RoGetActivationFactory("Windows.Xbox.Input.Gamepad") ->
//   IGamepadStatics
//   2. Calls IGamepadStatics::get_Gamepads(IVectorView<IGamepad*>** value)
//   (Slot 6, +0x30, RCX=this, RDX=out)
//   3. Calls IVectorView<IGamepad*>::get_Size(uint32_t* size) (Slot 7, +0x38,
//   RCX=this, RDX=&size)
//      Notice: In game+0x0C2FE8..0x0C797F, the game compiler loads the vtable
//      address into R8. The method MUST ONLY write to *size (RDX) and NEVER
//      read or write R8!
//   4. If *size == 0, game jumps to game+0x0C3338, releases IVectorView and
//   returns safely without calling GetAt.
//   5. If *size > 0, game calls GetAt(index, &item) (Slot 6, +0x30).

// {8C4C1BCD-FDAB-4BF8-BDE9-ABB2B1D6FDD3}
static const GUID IID_IGamepadStaticsEra = {
    0x8c4c1bcd,
    0xfdab,
    0x4bf8,
    {0xbd, 0xe9, 0xab, 0xb2, 0xb1, 0xd6, 0xfd, 0xd3}};

// {9776FF56-9BFD-4581-AD45-B645BBA526D6}
static const GUID IID_IControllerEra = {
    0x9776ff56,
    0x9bfd,
    0x4581,
    {0xad, 0x45, 0xb6, 0x45, 0xbb, 0xa5, 0x26, 0xd6}};

// {AACAAAAB-AAAA-AAAA-AAAA-CDAAAAAACAAA}
static const GUID IID_INavigationControllerEra = {
    0xaacaaaab,
    0xaaaa,
    0xaaaa,
    {0xaa, 0xaa, 0xcd, 0xaa, 0xaa, 0xaa, 0xca, 0xaa}};

// {082E402C-07DF-45E1-A5AB-A3127AF197B5}
static const GUID IID_IGamepadEra = {
    0x082e402c,
    0x07df,
    0x45e1,
    {0xa5, 0xab, 0xa3, 0x12, 0x7a, 0xf1, 0x97, 0xb5}};

// {ABCAAAAB-AAAA-AABA-AAAA-AAAAADDAAAAA}
static const GUID IID_IGamepadReadingEra = {
    0xabcaaaab,
    0xaaaa,
    0xaaba,
    {0xaa, 0xaa, 0xaa, 0xaa, 0xad, 0xda, 0xaa, 0xaa}};

// {AACAAAAB-ADAA-AAAA-AAAA-AAAAAAAACAAA}
static const GUID IID_INavigationReadingEra = {
    0xaacaaaab,
    0xadaa,
    0xaaaa,
    {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xca, 0xaa}};

// {7400624F-144B-47FC-9F3B-1DCAFB482873}
static const GUID IID_IControllerStaticsEra = {
    0x7400624f,
    0x144b,
    0x47fc,
    {0x9f, 0x3b, 0x1d, 0xca, 0xfb, 0x48, 0x28, 0x73}};

static inline bool GuidEqual(REFIID a, REFIID b) {
  return memcmp(&a, &b, sizeof(GUID)) == 0;
}

struct RawGamepadReading {
  UINT64 Timestamp;
  DWORD Buttons;
  float LeftTrigger;
  float RightTrigger;
  float LeftThumbstickX;
  float LeftThumbstickY;
  float RightThumbstickX;
  float RightThumbstickY;
};

struct GamepadVibration {
  float LeftMotorLevel;
  float RightMotorLevel;
  float LeftTriggerLevel;
  float RightTriggerLevel;
};

struct RawNavigationReading {
  UINT64 Timestamp;
  DWORD Buttons;
};

struct IGamepadReading : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(UINT64 *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Buttons(DWORD *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsDPadUpPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsDPadDownPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsDPadLeftPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsDPadRightPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsMenuPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsViewPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsLeftThumbstickPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsRightThumbstickPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsLeftShoulderPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsRightShoulderPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsAPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsBPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsXPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsYPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_LeftTrigger(float *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RightTrigger(float *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_LeftThumbstickX(float *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_LeftThumbstickY(float *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RightThumbstickX(float *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RightThumbstickY(float *value) = 0;
};

struct INavigationReading : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(UINT64 *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Buttons(DWORD *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsUpPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsDownPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsLeftPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsRightPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsMenuPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsViewPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsPreviousPagePressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsNextPagePressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsAcceptPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsCancelPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsXPressed(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsYPressed(unsigned char *value) = 0;
};

struct IGamepad : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE SetVibration(GamepadVibration value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(IGamepadReading **ppReading) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRawCurrentReading(RawGamepadReading *pReading) = 0;
};

struct IController : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Id(UINT64 *pId) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Type(HSTRING *pType) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_User(void **ppUser) = 0;
};

struct INavigationController : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetNavigationReading(INavigationReading **ppReading) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRawNavigationReading(RawNavigationReading *pReading) = 0;
};

// ----------------------------------------------------------------------------
// Windows.Xbox.System Async & Token Mocks
// ----------------------------------------------------------------------------
static const GUID IID_IAsyncInfo =
    {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_IGetTokenAndSignatureResult =
    {0x44a9796f, 0x723e, 0x4fdf, {0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84}};
static const GUID IID_IAsyncOperationToken =
    {0xb1f00d3b, 0x1f06, 0x5117, {0x93, 0xea, 0x2a, 0x0d, 0x79, 0x11, 0x67, 0x01}};

class MockGetTokenAndSignatureResult : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    return CreateHString(L"Windows.Xbox.System.GetTokenAndSignatureResult", name);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_Signature / get_RequiredBufferSize
  virtual HRESULT STDMETHODCALLTYPE get_Signature(void *val) {
    if (val) *(UINT32*)val = 0;
    return CreateHString(L"DUMMY_SIGNATURE", (HSTRING*)val);
  }
  // Slot 7: get_Token / get_Buffer
  virtual HRESULT STDMETHODCALLTYPE get_Token(void *val) {
    if (val) *(UINT32*)val = 0;
    return CreateHString(L"DUMMY_TOKEN", (HSTRING*)val);
  }
  virtual HRESULT STDMETHODCALLTYPE Dummy8() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy9() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy10() { return S_OK; }
};
static MockGetTokenAndSignatureResult g_mockTokenResult;

class MockAsyncInfoImpl : public IInspectable {
public:
  void *m_parent = nullptr;
  int m_status = 1; /* Completed */
  HRESULT m_error = S_OK;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_Id
  virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int *id) {
    if (id) *id = 1; return S_OK;
  }
  // Slot 7: get_Status
  virtual HRESULT STDMETHODCALLTYPE get_Status(int *status) {
    if (status) *status = m_status; return S_OK;
  }
  // Slot 8: get_ErrorCode
  virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *hr) {
    if (hr) *hr = m_error; return S_OK;
  }
  // Slot 9: Cancel
  virtual HRESULT STDMETHODCALLTYPE Cancel() { return S_OK; }
  // Slot 10: Close
  virtual HRESULT STDMETHODCALLTYPE Close() { return S_OK; }
};

class MockAsyncOperationToken : public IInspectable {
private:
  LONG m_ref = 1;
  void *m_handler = nullptr;
public:
  MockAsyncInfoImpl m_asyncInfo;

  MockAsyncOperationToken() {
    m_asyncInfo.m_parent = this;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (memcmp(&riid, &IID_IAsyncInfo, sizeof(GUID)) == 0) {
      *ppv = &m_asyncInfo;
      AddRef();
      return S_OK;
    }
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: put_Completed(void *handler)
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) {
    log_line("[bridge] MockAsyncOperationToken::put_Completed(handler=%p)\r\n", handler);
    m_handler = handler;
    if (handler) {
      typedef HRESULT (STDMETHODCALLTYPE *PFN_AsyncCompletedInvoke)(void *pThis, void *pAsyncInfo, int status);
      void **vtbl = *(void***)handler;
      if (vtbl && vtbl[3]) {
        PFN_AsyncCompletedInvoke pfn = (PFN_AsyncCompletedInvoke)vtbl[3];
        log_line("[bridge] MockAsyncOperationToken: invoking completion handler slot 3 -> %p\r\n", pfn);
        pfn(handler, this, 1 /* Completed */);
      }
    }
    return S_OK;
  }
  // Slot 7: get_Completed(void **handler)
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) {
    if (handler) *handler = m_handler;
    return S_OK;
  }
  // Slot 8: GetResults(void **results)
  virtual HRESULT STDMETHODCALLTYPE GetResults(void **results) {
    log_line("[bridge] MockAsyncOperationToken::GetResults -> &g_mockTokenResult\r\n");
    if (!results) return E_POINTER;
    *results = &g_mockTokenResult;
    g_mockTokenResult.AddRef();
    return S_OK;
  }
};

inline HRESULT STDMETHODCALLTYPE MockAsyncInfoImpl::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (memcmp(&riid, &IID_IAsyncInfo, sizeof(GUID)) == 0) {
    *ppv = this;
    AddRef();
    return S_OK;
  }
  if (m_parent) return ((IUnknown*)m_parent)->QueryInterface(riid, ppv);
  *ppv = this;
  AddRef();
  return S_OK;
}
inline ULONG STDMETHODCALLTYPE MockAsyncInfoImpl::AddRef() {
  if (m_parent) return ((IUnknown*)m_parent)->AddRef();
  return 1;
}
inline ULONG STDMETHODCALLTYPE MockAsyncInfoImpl::Release() {
  if (m_parent) return ((IUnknown*)m_parent)->Release();
  return 1;
}

static MockAsyncOperationToken g_mockAsyncOperationToken;

// ----------------------------------------------------------------------------
// Windows.Xbox.System.User Implementation
// ----------------------------------------------------------------------------
// Player name shown in game (gamertag); set by the launcher via ONRUSH_PLAYER_NAME.
static std::wstring OrPlayerName() {
  wchar_t v[64] = {};
  DWORD n = GetEnvironmentVariableW(L"ONRUSH_PLAYER_NAME", v, 64);
  if (n > 0 && n < 64 && v[0]) return v;
  return L"Player 1";
}

class MockUserDisplayInfo : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: Gamertag
  virtual HRESULT STDMETHODCALLTYPE Gamertag(HSTRING *pTag) {
    return CreateHString(OrPlayerName().c_str(), pTag);
  }
  // Slot 7: GamerScore
  virtual HRESULT STDMETHODCALLTYPE GamerScore(UINT32 *pScore) {
    if (pScore) *pScore = 1000; return S_OK;
  }
  // Slot 8: ApplicationDisplayName
  virtual HRESULT STDMETHODCALLTYPE ApplicationDisplayName(HSTRING *pName) {
    return CreateHString(OrPlayerName().c_str(), pName);
  }
  // Slot 9: GameDisplayName
  virtual HRESULT STDMETHODCALLTYPE GameDisplayName(HSTRING *pName) {
    return CreateHString(OrPlayerName().c_str(), pName);
  }
  // Slot 10: Reputation
  virtual HRESULT STDMETHODCALLTYPE Reputation(UINT32 *pRep) {
    if (pRep) *pRep = 100; return S_OK;
  }
  // Slot 11: AgeGroup (0=Unknown, 1=Child, 2=Teen, 3=Adult)
  virtual HRESULT STDMETHODCALLTYPE AgeGroup(DWORD *pAge) {
    if (pAge) *pAge = 3; return S_OK;
  }
  // Slot 12: Privileges
  virtual HRESULT STDMETHODCALLTYPE Privileges(void **ppPriv) {
    if (ppPriv) *ppPriv = nullptr; return S_OK;
  }
  // Slot 13: GetApplicationPictureAsync
  virtual HRESULT STDMETHODCALLTYPE GetApplicationPictureAsync(DWORD size, void *buffer, void **ppOp) {
    log_line("[bridge] IUserDisplayInfo::GetApplicationPictureAsync -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  // Slot 14: GetGamerPictureAsync
  virtual HRESULT STDMETHODCALLTYPE GetGamerPictureAsync(DWORD size, void *buffer, void **ppOp) {
    log_line("[bridge] IUserDisplayInfo::GetGamerPictureAsync -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Dummy15() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy16() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy17() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy18() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy19() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy20() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy21() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy22() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy23() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy24() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy25() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy26() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy27() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy28() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy29() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy30() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy31() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy32() { return S_OK; }
};
static MockUserDisplayInfo g_mockUserDisplayInfo;

class MockGamepad;
extern MockGamepad g_mockGamepad;

class MockVectorViewGamepad : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: GetAt(UINT index, void** item)
  virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, void **item) {
    if (!item) return E_POINTER;
    if (index == 0) {
      *item = (void*)(IGamepad*)&g_mockGamepad;
      ((IGamepad*)&g_mockGamepad)->AddRef();
      return S_OK;
    }
    *item = nullptr;
    return (HRESULT)0x8000000B; // E_BOUNDS
  }
  // Slot 7: get_Size(unsigned int* size)
  virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int *size) {
    if (!size) return E_POINTER;
    *size = 1; // 1 connected gamepad
    return S_OK;
  }
  // Slot 8: IndexOf
  virtual HRESULT STDMETHODCALLTYPE IndexOf(void *item, unsigned int *index, unsigned char *found) {
    if (index) *index = 0;
    if (found) *found = 1;
    return S_OK;
  }
  // Slot 9: GetMany
  virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned int startIndex, unsigned int capacity, void **items, unsigned int *actual) {
    if (items && capacity > 0 && startIndex == 0) {
      items[0] = (void*)(IGamepad*)&g_mockGamepad;
      ((IGamepad*)&g_mockGamepad)->AddRef();
      if (actual) *actual = 1;
      return S_OK;
    }
    if (actual) *actual = 0;
    return S_OK;
  }
};
static MockVectorViewGamepad g_mockVectorViewGamepad;

class MockVectorViewController : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: GetAt(UINT index, void** item)
  virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, void **item) {
    if (!item) return E_POINTER;
    if (index == 0) {
      *item = (void*)(IController*)&g_mockGamepad;
      ((IController*)&g_mockGamepad)->AddRef();
      return S_OK;
    }
    *item = nullptr;
    return (HRESULT)0x8000000B; // E_BOUNDS
  }
  // Slot 7: get_Size(unsigned int* size)
  virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int *size) {
    if (!size) return E_POINTER;
    *size = 1; // 1 connected controller
    return S_OK;
  }
  // Slot 8: IndexOf
  virtual HRESULT STDMETHODCALLTYPE IndexOf(void *item, unsigned int *index, unsigned char *found) {
    if (index) *index = 0;
    if (found) *found = 1;
    return S_OK;
  }
  // Slot 9: GetMany
  virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned int startIndex, unsigned int capacity, void **items, unsigned int *actual) {
    if (items && capacity > 0 && startIndex == 0) {
      items[0] = (void*)(IController*)&g_mockGamepad;
      ((IController*)&g_mockGamepad)->AddRef();
      if (actual) *actual = 1;
      return S_OK;
    }
    if (actual) *actual = 0;
    return S_OK;
  }
};
static MockVectorViewController g_mockVectorViewController;

class MockUser : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_Id
  virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32 *pId) {
    if (pId) *pId = 1; return S_OK;
  }
  // Slot 7: get_AudioDevices
  virtual HRESULT STDMETHODCALLTYPE get_AudioDevices(void **ppDevices) {
    if (ppDevices) *ppDevices = nullptr; return S_OK;
  }
  // Slot 8: get_Controllers
  virtual HRESULT STDMETHODCALLTYPE get_Controllers(void **ppControllers) {
    if (ppControllers) {
      *ppControllers = &g_mockVectorViewGamepad;
      g_mockVectorViewGamepad.AddRef();
    }
    return S_OK;
  }
  // Slot 9: get_DisplayInfo
  virtual HRESULT STDMETHODCALLTYPE get_DisplayInfo(void **ppDisplayInfo) {
    if (ppDisplayInfo) {
      *ppDisplayInfo = &g_mockUserDisplayInfo;
      g_mockUserDisplayInfo.AddRef();
    }
    return S_OK;
  }
  // Slot 10: get_IsGuest
  virtual HRESULT STDMETHODCALLTYPE get_IsGuest(unsigned char *pGuest) {
    if (pGuest) *pGuest = 0; return S_OK;
  }
  // Slot 11: get_IsSignedIn
  virtual HRESULT STDMETHODCALLTYPE get_IsSignedIn(unsigned char *pSignedIn) {
    if (pSignedIn) *pSignedIn = 1; return S_OK;
  }
  // Slot 12: get_Location (0=Local, 1=Remote)
  virtual HRESULT STDMETHODCALLTYPE get_Location(DWORD *pLocation) {
    if (pLocation) *pLocation = 0; return S_OK;
  }
  // Slot 13: get_Sponsor
  virtual HRESULT STDMETHODCALLTYPE get_Sponsor(void **ppUser) {
    if (ppUser) *ppUser = nullptr; return S_OK;
  }
  // Slot 14: get_XboxUserHash
  virtual HRESULT STDMETHODCALLTYPE get_XboxUserHash(HSTRING *pHash) {
    return CreateHString(L"1000000000000001", pHash);
  }
  // Slot 15: get_XboxUserId
  virtual HRESULT STDMETHODCALLTYPE get_XboxUserId(HSTRING *pId) {
    return CreateHString(L"1000000000000001", pId);
  }
  // Slot 16: GetTokenAndSignatureAsync
  virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureAsync(void *p1, void *p2, void *p3, void **ppOp) {
    log_line("[bridge] IUser::GetTokenAndSignatureAsync -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  // Slot 17: GetTokenAndSignatureAsync2
  virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureAsync2(void *p1, void *p2, void *p3, UINT32 s, void *b, void **ppOp) {
    log_line("[bridge] IUser::GetTokenAndSignatureAsync2 -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  // Slot 18: GetTokenAndSignatureAsync3
  virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureAsync3(void *p1, void *p2, void *p3, void *b, void **ppOp) {
    log_line("[bridge] IUser::GetTokenAndSignatureAsync3 -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Dummy19() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy20() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy21() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy22() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy23() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy24() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy25() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy26() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy27() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy28() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy29() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy30() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy31() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy32() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy33() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy34() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy35() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy36() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy37() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy38() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy39() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy40() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy41() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy42() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy43() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy44() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy45() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy46() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy47() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy48() { return S_OK; }
};
static MockUser g_mockUser;

class MockVectorViewUser : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: GetAt(UINT index, void** item)
  virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, void **item) {
    if (!item) return E_POINTER;
    if (index == 0) {
      *item = &g_mockUser;
      g_mockUser.AddRef();
      return S_OK;
    }
    *item = nullptr;
    return (HRESULT)0x8000000B; // E_BOUNDS
  }
  // Slot 7: get_Size
  virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int *size) {
    if (!size) return E_POINTER;
    *size = 1; return S_OK;
  }
  // Slot 8: IndexOf
  virtual HRESULT STDMETHODCALLTYPE IndexOf(void *item, unsigned int *index, unsigned char *found) {
    if (index) *index = 0; if (found) *found = 1; return S_OK;
  }
  // Slot 9: GetMany
  virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned int startIndex, unsigned int capacity, void **items, unsigned int *actual) {
    if (items && capacity > 0 && startIndex == 0) {
      items[0] = &g_mockUser;
      g_mockUser.AddRef();
      if (actual) *actual = 1;
      return S_OK;
    }
    if (actual) *actual = 0;
    return S_OK;
  }
};
static MockVectorViewUser g_mockVectorViewUser;

class MockUserStatics : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_OnlineState (0=Unknown, 1=Offline, 2=Online)
  virtual HRESULT STDMETHODCALLTYPE get_OnlineState(DWORD *pState) {
    log_line("[bridge] IUserStatics::get_OnlineState -> Online (2)\r\n");
    if (pState) *pState = 2; // Online
    return S_OK;
  }
  // Slot 7: add_OnlineStateChanged
  virtual HRESULT STDMETHODCALLTYPE add_OnlineStateChanged(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_OnlineStateChanged(handler=%p)\r\n", handler);
    if (token) *token = 1;
    return S_OK;
  }
  // Slot 8: remove_OnlineStateChanged
  virtual HRESULT STDMETHODCALLTYPE remove_OnlineStateChanged(int64_t token) {
    return S_OK;
  }
  // Slot 9: get_Users
  virtual HRESULT STDMETHODCALLTYPE get_Users(void **value) {
    log_line("[bridge] IUserStatics::get_Users -> MockVectorViewUser (count=1)\r\n");
    if (!value) return E_POINTER;
    *value = &g_mockVectorViewUser;
    g_mockVectorViewUser.AddRef();
    return S_OK;
  }
  // Slot 10: add_UserAdded
  virtual HRESULT STDMETHODCALLTYPE add_UserAdded(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_UserAdded(handler=%p)\r\n", handler);
    if (token) *token = 2;
    return S_OK;
  }
  // Slot 11: remove_UserAdded
  virtual HRESULT STDMETHODCALLTYPE remove_UserAdded(int64_t token) {
    return S_OK;
  }
  // Slot 12: add_UserRemoved
  virtual HRESULT STDMETHODCALLTYPE add_UserRemoved(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_UserRemoved(handler=%p)\r\n", handler);
    if (token) *token = 3;
    return S_OK;
  }
  // Slot 13: remove_UserRemoved
  virtual HRESULT STDMETHODCALLTYPE remove_UserRemoved(int64_t token) {
    return S_OK;
  }
  // Slot 14: GetUserById
  virtual HRESULT STDMETHODCALLTYPE GetUserById(UINT32 id, void **value) {
    log_line("[bridge] IUserStatics::GetUserById(id=%u)\r\n", id);
    if (!value) return E_POINTER;
    *value = &g_mockUser;
    g_mockUser.AddRef();
    return S_OK;
  }
  // Slot 15: GetTokenAndSignatureForAllUsersAsync1
  virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureForAllUsersAsync1(void *p1, void *p2, void *p3, void **ppOp) {
    log_line("[bridge] IUserStatics::GetTokenAndSignatureForAllUsersAsync1 -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  // Slot 16: GetTokenAndSignatureForAllUsersAsync2
  virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureForAllUsersAsync2(void *p1, void *p2, void *p3, void *p4, void **ppOp) {
    log_line("[bridge] IUserStatics::GetTokenAndSignatureForAllUsersAsync2 -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  // Slot 17: GetTokenAndSignatureForAllUsersAsync3
  virtual HRESULT STDMETHODCALLTYPE GetTokenAndSignatureForAllUsersAsync3(void *p1, void *p2, void *p3, void *p4, void **ppOp) {
    log_line("[bridge] IUserStatics::GetTokenAndSignatureForAllUsersAsync3 -> &g_mockAsyncOperationToken\r\n");
    if (!ppOp) return E_POINTER;
    *ppOp = &g_mockAsyncOperationToken;
    g_mockAsyncOperationToken.AddRef();
    return S_OK;
  }
  // Slot 18: add_AudioDeviceAdded
  virtual HRESULT STDMETHODCALLTYPE add_AudioDeviceAdded(void *handler, int64_t *token) {
    if (token) *token = 4; return S_OK;
  }
  // Slot 19: remove_AudioDeviceAdded
  virtual HRESULT STDMETHODCALLTYPE remove_AudioDeviceAdded(int64_t token) { return S_OK; }
  // Slot 20: add_AudioDeviceRemoved
  virtual HRESULT STDMETHODCALLTYPE add_AudioDeviceRemoved(void *handler, int64_t *token) {
    if (token) *token = 5; return S_OK;
  }
  // Slot 21: remove_AudioDeviceRemoved
  virtual HRESULT STDMETHODCALLTYPE remove_AudioDeviceRemoved(int64_t token) { return S_OK; }
  // Slot 22: add_AudioDeviceChanged
  virtual HRESULT STDMETHODCALLTYPE add_AudioDeviceChanged(void *handler, int64_t *token) {
    if (token) *token = 6; return S_OK;
  }
  // Slot 23: remove_AudioDeviceChanged
  virtual HRESULT STDMETHODCALLTYPE remove_AudioDeviceChanged(int64_t token) { return S_OK; }

  // Slot 24: add_SignInCompleted
  virtual HRESULT STDMETHODCALLTYPE add_SignInCompleted(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_SignInCompleted(handler=%p)\r\n", handler);
    if (token) *token = 7;
    return S_OK;
  }
  // Slot 25: remove_SignInCompleted
  virtual HRESULT STDMETHODCALLTYPE remove_SignInCompleted(int64_t token) { return S_OK; }

  // Slot 26: add_SignOutStarted
  virtual HRESULT STDMETHODCALLTYPE add_SignOutStarted(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_SignOutStarted(handler=%p)\r\n", handler);
    if (token) *token = 8;
    return S_OK;
  }
  // Slot 27: remove_SignOutStarted
  virtual HRESULT STDMETHODCALLTYPE remove_SignOutStarted(int64_t token) { return S_OK; }

  // Slot 28: add_SignOutCompleted
  virtual HRESULT STDMETHODCALLTYPE add_SignOutCompleted(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_SignOutCompleted(handler=%p)\r\n", handler);
    if (token) *token = 9;
    return S_OK;
  }
  // Slot 29: remove_SignOutCompleted
  virtual HRESULT STDMETHODCALLTYPE remove_SignOutCompleted(int64_t token) { return S_OK; }

  // Slot 30: add_UserDisplayInfoChanged
  virtual HRESULT STDMETHODCALLTYPE add_UserDisplayInfoChanged(void *handler, int64_t *token) {
    log_line("[bridge] IUserStatics::add_UserDisplayInfoChanged(handler=%p)\r\n", handler);
    if (token) *token = 10;
    return S_OK;
  }
  // Slot 31: remove_UserDisplayInfoChanged
  virtual HRESULT STDMETHODCALLTYPE remove_UserDisplayInfoChanged(int64_t token) { return S_OK; }

  // Slot 32: GetNetworkCacheIdForUsers
  virtual HRESULT STDMETHODCALLTYPE GetNetworkCacheIdForUsers(void *users, GUID *pGuid) {
    if (pGuid) memset(pGuid, 0, sizeof(GUID));
    return S_OK;
  }

  // Padding slots up to 64
  virtual HRESULT STDMETHODCALLTYPE Dummy33() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy34() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy35() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy36() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy37() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy38() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy39() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy40() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy41() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy42() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy43() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy44() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy45() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy46() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy47() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy48() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy49() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy50() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy51() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy52() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy53() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy54() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy55() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy56() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy57() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy58() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy59() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy60() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy61() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy62() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy63() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy64() { return S_OK; }
};
static MockUserStatics g_mockUserStatics;

// ----------------------------------------------------------------------------
// Windows.Xbox.Input.Gamepad & Controller Implementation
// ----------------------------------------------------------------------------

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, void *pState);
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD dwUserIndex, void *pVibration);
static PFN_XInputGetState s_pfnXInputGetState = nullptr;
static PFN_XInputSetState s_pfnXInputSetState = nullptr;
static bool s_xinputChecked = false;
static volatile LONG s_activePad = -1;  // XInput slot (0..3) currently in use

static void InitXInput() {
  if (s_xinputChecked) return;
  s_xinputChecked = true;
  HMODULE hXInput = LoadLibraryW(L"xinput1_4.dll");
  if (!hXInput) hXInput = LoadLibraryW(L"xinput9_1_0.dll");
  if (!hXInput) hXInput = LoadLibraryW(L"xinput1_3.dll");
  if (hXInput) {
    s_pfnXInputGetState = (PFN_XInputGetState)GetProcAddress(hXInput, "XInputGetState");
    s_pfnXInputSetState = (PFN_XInputSetState)GetProcAddress(hXInput, "XInputSetState");
    log_line("[input] XInput initialized successfully from DLL (get=%p set=%p)\r\n",
             s_pfnXInputGetState, s_pfnXInputSetState);
  } else {
    log_line("[input] No XInput DLL found; keyboard only\r\n");
  }
}

struct XINPUT_GAMEPAD_RAW {
  WORD wButtons;
  BYTE bLeftTrigger;
  BYTE bRightTrigger;
  SHORT sThumbLX;
  SHORT sThumbLY;
  SHORT sThumbRX;
  SHORT sThumbRY;
};
struct XINPUT_STATE_RAW {
  DWORD dwPacketNumber;
  XINPUT_GAMEPAD_RAW Gamepad;
};
struct XINPUT_VIBRATION_RAW {
  WORD wLeftMotorSpeed;
  WORD wRightMotorSpeed;
};

// Quick test mode: start with "-gameplay" (or ONRUSH_GAMEPLAY=1) to go straight into gameplay.
// Turns on the options needed to leave the title screen (ONRUSH_REAL_JOBBUSY, ONRUSH_ORIG_PHASE_WORKER)
// and lets the input code press A automatically during the boot phases.
static bool QuickGameplayEnabled() {
  static int s_qg = -1;
  if (s_qg < 0) {
    const wchar_t *cl = GetCommandLineW();
    bool on = cl && (wcsstr(cl, L"-gameplay") || wcsstr(cl, L"/gameplay"));
    char b[8] = {};
    DWORD n = GetEnvironmentVariableA("ONRUSH_GAMEPLAY", b, sizeof(b));
    if (n > 0 && n < sizeof(b) && b[0] != '0') on = true;
    // Skipping the intro (launcher option / -skipintro) needs the same phase path as -gameplay.
    char si[8] = {};
    DWORD sn = GetEnvironmentVariableA("ONRUSH_SKIP_INTRO", si, sizeof(si));
    const bool skipIntro = (sn > 0 && sn < sizeof(si) && si[0] != '0') || (cl && wcsstr(cl, L"-skipintro"));
    if (on) SetEnvironmentVariableA("ONRUSH_GAMEPLAY", "1");
    if (skipIntro) SetEnvironmentVariableA("ONRUSH_SKIP_INTRO", "1");
    // The real job-busy check + original phase worker are needed to leave the intro/title at all
    // (without them pressing A crashes the game). Always on; ONRUSH_LEGACY_PHASE=1 restores the old path.
    if (GetEnvironmentVariableA("ONRUSH_LEGACY_PHASE", nullptr, 0) == 0) {
      SetEnvironmentVariableA("ONRUSH_REAL_JOBBUSY", "1");
      SetEnvironmentVariableA("ONRUSH_ORIG_PHASE_WORKER", "1");
    }
    s_qg = on ? 1 : 0;
  }
  return s_qg == 1;
}

// vtable RVA of the engine's current phase object (0 when none / unreadable).
static uintptr_t CurrentPhaseVtblRva() {
  uint8_t *hx = (uint8_t *)GetModuleHandleW(nullptr);
  __try {
    void *cur = *(void **)(hx + 0x0167E930);
    return cur ? (uintptr_t)(*(uint8_t **)cur - hx) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool InputEnvFlag(const char *name) {
  static const bool s_qgInit = (QuickGameplayEnabled(), true);  // sets the env flags it implies
  (void)s_qgInit;
  char buf[8] = {};
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n > 0 && n < sizeof(buf) && buf[0] != '0';
}

// Input is only accepted while one of this process' windows has focus, so
// keystrokes / pad presses meant for other apps don't drive the game.
// Set ONRUSH_INPUT_ANYFOCUS=1 to disable this check.
static bool GameWindowFocused() {
  static const bool s_anyFocus = InputEnvFlag("ONRUSH_INPUT_ANYFOCUS");
  if (s_anyFocus) return true;
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

// Reads the first connected XInput pad (slots 0..3). Remembers the active slot
// and re-scans (at most once per second) after a disconnect / hot-plug.
static bool ReadXInput(XINPUT_STATE_RAW *out) {
  InitXInput();
  if (!s_pfnXInputGetState) return false;
  LONG active = s_activePad;
  if (active >= 0) {
    if (s_pfnXInputGetState((DWORD)active, out) == ERROR_SUCCESS) return true;
    log_line("[input] XInput pad %ld disconnected\r\n", active);
    s_activePad = -1;
  }
  static ULONGLONG s_nextScan = 0;
  ULONGLONG now = GetTickCount64();
  if (now < s_nextScan) return false;
  for (DWORD i = 0; i < 4; ++i) {
    if (s_pfnXInputGetState(i, out) == ERROR_SUCCESS) {
      s_activePad = (LONG)i;
      log_line("[input] XInput pad connected on slot %lu\r\n", (unsigned long)i);
      return true;
    }
  }
  s_nextScan = now + 1000;
  return false;
}

// Radial dead zone with rescaling so the stick output still spans 0..1.
static void ApplyStickDeadzone(SHORT x, SHORT y, SHORT deadzone, float *ox, float *oy) {
  float fx = (float)x / 32767.0f;
  float fy = (float)y / 32767.0f;
  if (fx < -1.0f) fx = -1.0f;
  if (fy < -1.0f) fy = -1.0f;
  float mag = sqrtf(fx * fx + fy * fy);
  float dz = (float)deadzone / 32767.0f;
  if (mag <= dz) { *ox = 0.0f; *oy = 0.0f; return; }
  float scaled = (mag - dz) / (1.0f - dz);
  if (scaled > 1.0f) scaled = 1.0f;
  *ox = fx / mag * scaled;
  *oy = fy / mag * scaled;
}

static void ApplyVibration(const GamepadVibration &vib) {
  LONG active = s_activePad;
  if (active < 0 || !s_pfnXInputSetState) return;
  auto to16 = [](float v) -> WORD {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (WORD)(v * 65535.0f);
  };
  XINPUT_VIBRATION_RAW v = {to16(vib.LeftMotorLevel), to16(vib.RightMotorLevel)};
  s_pfnXInputSetState((DWORD)active, &v);
}

// Xbox ERA button bits as exposed by this bridge (see IGamepadReading getters).
static const DWORD PAD_MENU = 0x4, PAD_VIEW = 0x8, PAD_A = 0x10, PAD_B = 0x20,
                   PAD_X = 0x40, PAD_Y = 0x80;

// Debug-only scripted presses (test menus without desktop keystrokes):
//   ONRUSH_PRESS_SCRIPT="A@95,A@115,D@125"  -> button held 300 ms at N seconds after the first query.
//   Buttons: A B X Y, U D L R (d-pad), M (Menu), V (View).
// Used by the gamepad poll and by the title-screen check (IsAdvanceInputHeld).
static DWORD ScriptedButtons() {
  struct ScriptPress { char b; double t; };
  static ScriptPress s_sp[32]; static volatile LONG s_init = 0; static int s_spN = 0; static ULONGLONG s_t0 = 0;
  if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
    s_t0 = GetTickCount64();
    char sb[512] = {};
    if (GetEnvironmentVariableA("ONRUSH_PRESS_SCRIPT", sb, sizeof(sb)) > 0) {
      char *ctx = nullptr; int n = 0;
      for (char *tok = strtok_s(sb, ",", &ctx); tok && n < 32; tok = strtok_s(nullptr, ",", &ctx)) {
        char *at = strchr(tok, '@');
        if (at) { s_sp[n].b = (char)toupper((unsigned char)tok[0]); s_sp[n].t = atof(at + 1); n++; }
      }
      log_line("[input] ONRUSH_PRESS_SCRIPT: %d presses\r\n", n);
      s_spN = n;
    }
    InterlockedExchange(&s_init, 2);
  }
  if (s_init != 2 || !s_spN) return 0;
  DWORD m = 0;
  double now = (double)(GetTickCount64() - s_t0) / 1000.0;
  for (int k = 0; k < s_spN; ++k) {
    if (now >= s_sp[k].t && now < s_sp[k].t + 0.3) {
      switch (s_sp[k].b) {
        case 'A': m |= PAD_A; break;
        case 'B': m |= PAD_B; break;
        case 'X': m |= PAD_X; break;
        case 'Y': m |= PAD_Y; break;
        case 'U': m |= 0x100; break;
        case 'D': m |= 0x200; break;
        case 'L': m |= 0x400; break;
        case 'R': m |= 0x800; break;
        case 'M': m |= PAD_MENU; break;
        case 'V': m |= PAD_VIEW; break;
        case 'Q': m |= 0x1000; break;  // left shoulder
        case 'E': m |= 0x2000; break;  // right shoulder
        case 'S': m |= 0x4000; break;  // left stick click
        case 'T': m |= 0x8000; break;  // right stick click
      }
    }
  }
  return m;
}

// Keyboard map (keymap_defs.h), read once from ONRUSH_KEYMAP="Accelerate=57,26;Brake=53,28;..." (hex VK codes).
struct Keymap {
  unsigned char keys[KM_COUNT][KM_SLOTS];
  bool Down(int a) const {
    for (int s = 0; s < KM_SLOTS; ++s)
      if (keys[a][s] && (GetAsyncKeyState(keys[a][s]) & 0x8000)) return true;
    return false;
  }
};

static const Keymap &GetKeymap() {
  static Keymap s_km;
  static volatile LONG s_init = 0;
  if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
    for (int a = 0; a < KM_COUNT; ++a)
      for (int s = 0; s < KM_SLOTS; ++s) s_km.keys[a][s] = kKeymapDefaults[a].keys[s];
    char buf[1024] = {};
    int custom = 0;
    DWORD envLen = GetEnvironmentVariableA("ONRUSH_KEYMAP", buf, sizeof(buf));
    if (envLen >= sizeof(buf)) { log_line("[input] ONRUSH_KEYMAP too long (%lu chars), using the default keys\r\n", envLen); buf[0] = 0; envLen = 0; }
    if (envLen > 0) {
      log_line("[input] ONRUSH_KEYMAP=%s\r\n", buf);
      char *ctx = nullptr;
      for (char *tok = strtok_s(buf, ";", &ctx); tok; tok = strtok_s(nullptr, ";", &ctx)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        int a = 0;
        while (a < KM_COUNT && _stricmp(kKeymapDefaults[a].ini, tok) != 0) ++a;
        if (a == KM_COUNT) { log_line("[input] ONRUSH_KEYMAP: unknown action '%s'\r\n", tok); continue; }
        unsigned char k[KM_SLOTS] = {};
        int n = 0;
        for (char *p = eq + 1; *p && n < KM_SLOTS;) {
          char *end = nullptr;
          unsigned long v = strtoul(p, &end, 16);
          if (end == p) break;
          if (v > 0 && v < 0xFF && !KeymapReserved((int)v)) k[n++] = (unsigned char)v;
          p = *end == ',' ? end + 1 : end;
          if (*end != ',') break;
        }
        for (int s = 0; s < KM_SLOTS; ++s) s_km.keys[a][s] = k[s];
        ++custom;
      }
    }
    log_line("[input] keyboard map: %s (%d actions from the launcher)\r\n", custom ? "custom" : "default", custom);
    InterlockedExchange(&s_init, 2);
  }
  while (s_init != 2) YieldProcessor();
  return s_km;
}

static void PollInput(RawGamepadReading *pReading) {
  InitXInput();
  pReading->Timestamp = GetTickCount64() * 10000;
  pReading->Buttons = 0;
  pReading->LeftTrigger = 0.0f;
  pReading->RightTrigger = 0.0f;
  pReading->LeftThumbstickX = 0.0f;
  pReading->LeftThumbstickY = 0.0f;
  pReading->RightThumbstickX = 0.0f;
  pReading->RightThumbstickY = 0.0f;

  const bool focused = GameWindowFocused();
  mouse::Update(focused);

  XINPUT_STATE_RAW xstate = {};
  if (focused && ReadXInput(&xstate)) {
    WORD w = xstate.Gamepad.wButtons;
    if (w & 0x0001) pReading->Buttons |= 0x100;  // DPadUp
    if (w & 0x0002) pReading->Buttons |= 0x200;  // DPadDown
    if (w & 0x0004) pReading->Buttons |= 0x400;  // DPadLeft
    if (w & 0x0008) pReading->Buttons |= 0x800;  // DPadRight
    if (w & 0x0010) pReading->Buttons |= PAD_MENU;  // Start -> Menu
    if (w & 0x0020) pReading->Buttons |= PAD_VIEW;  // Back -> View
    if (w & 0x0040) pReading->Buttons |= 0x4000; // LeftThumb
    if (w & 0x0080) pReading->Buttons |= 0x8000; // RightThumb
    if (w & 0x0100) pReading->Buttons |= 0x1000; // LeftShoulder
    if (w & 0x0200) pReading->Buttons |= 0x2000; // RightShoulder
    if (w & 0x1000) pReading->Buttons |= PAD_A;
    if (w & 0x2000) pReading->Buttons |= PAD_B;
    if (w & 0x4000) pReading->Buttons |= PAD_X;
    if (w & 0x8000) pReading->Buttons |= PAD_Y;

    const BYTE TRIGGER_THRESHOLD = 30;
    BYTE lt = xstate.Gamepad.bLeftTrigger;
    BYTE rt = xstate.Gamepad.bRightTrigger;
    pReading->LeftTrigger = lt > TRIGGER_THRESHOLD ? (float)(lt - TRIGGER_THRESHOLD) / (255.0f - TRIGGER_THRESHOLD) : 0.0f;
    pReading->RightTrigger = rt > TRIGGER_THRESHOLD ? (float)(rt - TRIGGER_THRESHOLD) / (255.0f - TRIGGER_THRESHOLD) : 0.0f;

    ApplyStickDeadzone(xstate.Gamepad.sThumbLX, xstate.Gamepad.sThumbLY, 7849,
                       &pReading->LeftThumbstickX, &pReading->LeftThumbstickY);
    ApplyStickDeadzone(xstate.Gamepad.sThumbRX, xstate.Gamepad.sThumbRY, 8689,
                       &pReading->RightThumbstickX, &pReading->RightThumbstickY);
  }

  // Keyboard as a controller (only while the game window is focused). Keys from the launcher's "Controles" tab
  // (ONRUSH_KEYMAP), defaults in keymap_defs.h. The keyboard adds to the pad: both work at the same time.
  if (focused) {
    const Keymap &km = GetKeymap();
    auto on = [&](int a) { return km.Down(a); };
    if (on(KM_A)) pReading->Buttons |= PAD_A;
    if (on(KM_B)) pReading->Buttons |= PAD_B;
    if (on(KM_X)) pReading->Buttons |= PAD_X;
    if (on(KM_Y)) pReading->Buttons |= PAD_Y;
    if (on(KM_VIEW)) pReading->Buttons |= PAD_VIEW;
    if (on(KM_MENU)) pReading->Buttons |= PAD_MENU;
    if (on(KM_LB)) pReading->Buttons |= 0x1000;
    if (on(KM_RB)) pReading->Buttons |= 0x2000;
    if (on(KM_L3)) pReading->Buttons |= 0x4000;
    if (on(KM_R3)) pReading->Buttons |= 0x8000;
    if (on(KM_ACCEL)) pReading->RightTrigger = 1.0f;
    if (on(KM_BRAKE)) pReading->LeftTrigger = 1.0f;
    // D-pad: own keys (in a race it opens the taunt wheel, so the driving keys must not press it)
    if (on(KM_DPAD_UP)) pReading->Buttons |= 0x100;
    if (on(KM_DPAD_DOWN)) pReading->Buttons |= 0x200;
    if (on(KM_DPAD_LEFT)) pReading->Buttons |= 0x400;
    if (on(KM_DPAD_RIGHT)) pReading->Buttons |= 0x800;
    // left stick; opposite keys held together cancel out
    const bool up = on(KM_UP), dn = on(KM_DOWN), lf = on(KM_LEFT), rt = on(KM_RIGHT);
    if (up != dn) pReading->LeftThumbstickY = up ? 1.0f : -1.0f;
    if (lf != rt) pReading->LeftThumbstickX = rt ? 1.0f : -1.0f;
    const bool lu = on(KM_RS_UP), ld = on(KM_RS_DOWN), ll = on(KM_RS_LEFT), lr = on(KM_RS_RIGHT);
    if (lu != ld) pReading->RightThumbstickY = lu ? 1.0f : -1.0f;
    if (ll != lr) pReading->RightThumbstickX = lr ? 1.0f : -1.0f;
  }
  // mouse in the menus: click = A, right click = B, wheel = d-pad up/down
  pReading->Buttons |= mouse::Buttons(focused);

  // Debug-only: periodically inject an 'A' press to walk past the title screen
  // unattended. OFF by default (it would otherwise press A every ~2 s during
  // menus and gameplay). Enable with ONRUSH_AUTOPULSE_A=1.
  static const bool s_autoPulseA = InputEnvFlag("ONRUSH_AUTOPULSE_A");
  static UINT32 s_readCount = 0;
  s_readCount++;
  if (s_autoPulseA && (s_readCount % 120) < 15) {
    pReading->Buttons |= PAD_A;
  }
  // -gameplay / ONRUSH_GAMEPLAY=1: press A by itself while the game is in a boot phase (intro video,
  // title / "continue offline" / EULA, controller loading screen) so it goes straight into gameplay.
  // Phase-driven instead of timed, so it copes with variable loading times.
  // Launcher option "Pular vídeos de introdução" (ONRUSH_SKIP_INTRO=1 or -skipintro): press A only
  // while the intro phase (0x142C090, Codemasters/ONRUSH videos) is active -> stops on the title screen.
  {
    static int s_si = -1;
    if (s_si < 0) {
      const wchar_t *cl = GetCommandLineW();
      s_si = (InputEnvFlag("ONRUSH_SKIP_INTRO") || (cl && wcsstr(cl, L"-skipintro"))) ? 1 : 0;
    }
    if (s_si == 1 && !QuickGameplayEnabled() && CurrentPhaseVtblRva() == 0x142C090) {
      static ULONGLONG s_siT0 = GetTickCount64();
      if (((GetTickCount64() - s_siT0) % 700) < 150) pReading->Buttons |= PAD_A;
    }
  }
  if (QuickGameplayEnabled()) {
    static ULONGLONG s_qgT0 = GetTickCount64();
    const uintptr_t rva = CurrentPhaseVtblRva();
    // The gameplay phase (0x141C3C0) still starts on the controller screen ("A: continue") once it
    // finishes loading, so keep pressing for a while after entering it (A is only boost in the tutorial).
    static ULONGLONG s_gpEnter = 0;
    if (rva == 0x141C3C0 && !s_gpEnter) s_gpEnter = GetTickCount64();
    const bool bootPhase = rva == 0x142C090 || rva == 0x142C2F0 || rva == 0x142BEC8 ||
                           (rva == 0x141C3C0 && GetTickCount64() - s_gpEnter < 25000);
    if (bootPhase && ((GetTickCount64() - s_qgT0) % 700) < 150) pReading->Buttons |= PAD_A;
    static uintptr_t s_lastRva = 1;
    if (rva != s_lastRva) { log_line("[gameplay] phase vtbl rva=0x%llX%s\r\n", (unsigned long long)rva, bootPhase ? " (auto A)" : ""); s_lastRva = rva; }
  }

  // Debug-only scripted presses (test menus without desktop keystrokes):
  //   ONRUSH_PRESS_SCRIPT="A@95,A@115,D@125"  -> button held 300 ms at N seconds after the first poll.
  //   Buttons: A B X Y, U D L R (d-pad), M (Menu), V (View).
  pReading->Buttons |= ScriptedButtons();

  // Debug-only autodrive (crash repro without a human driver): ONRUSH_AUTODRIVE=<sec>
  // From <sec> seconds after the first poll: right trigger held, gentle steering wave,
  // A tapped every 4 s (tutorial prompts / boost).
  { static double s_ad = -2; static ULONGLONG s_adT0 = 0;
    if (s_ad == -2) { char ab[32] = {}; s_ad = -1; s_adT0 = GetTickCount64();
      if (GetEnvironmentVariableA("ONRUSH_AUTODRIVE", ab, sizeof(ab)) > 0) s_ad = atof(ab); }
    if (s_ad >= 0) {
      double now = (double)(GetTickCount64() - s_adT0) / 1000.0;
      if (now >= s_ad) {
        pReading->RightTrigger = 1.0f;
        pReading->LeftThumbstickX = (float)(0.35 * sin(now * 0.7));
        if (fmod(now, 4.0) < 0.3) pReading->Buttons |= PAD_A;
      }
    } }

  // Diagnostics: log every change of the button mask, and how often the game
  // polls (tells us whether the engine reads input every frame).
  static DWORD s_prevButtons = 0;
  if (pReading->Buttons != s_prevButtons) {
    log_line("[input] Buttons 0x%04X -> 0x%04X (LX=%.2f LY=%.2f RX=%.2f RY=%.2f LT=%.2f RT=%.2f, pad=%ld)\r\n",
             s_prevButtons, pReading->Buttons,
             pReading->LeftThumbstickX, pReading->LeftThumbstickY,
             pReading->RightThumbstickX, pReading->RightThumbstickY,
             pReading->LeftTrigger, pReading->RightTrigger, (long)s_activePad);
    s_prevButtons = pReading->Buttons;
  }
  if (s_readCount % 600 == 0) {
    log_line("[input] poll count=%u (focused=%d, pad=%ld)\r\n", s_readCount, focused ? 1 : 0, (long)s_activePad);
  }
}

// True while a "confirm / skip" input is held (any face button or Start, on the pad or on the
// keyboard keys mapped to them). Used only by the boot/title pacing logic.
static bool IsAdvanceInputHeld() {
  if (ScriptedButtons() & PAD_A) return true;
  // -gameplay: the title check comes through here (not the gamepad poll) when the intro is skipped.
  if (QuickGameplayEnabled() && CurrentPhaseVtblRva() == 0x142C2F0) {
    static ULONGLONG s_t0 = GetTickCount64();
    if (((GetTickCount64() - s_t0) % 700) < 150) return true;
  }
  if (!GameWindowFocused()) return false;
  {
    const Keymap &km = GetKeymap();
    if (km.Down(KM_A) || km.Down(KM_X) || km.Down(KM_Y) || km.Down(KM_MENU)) return true;
  }
  XINPUT_STATE_RAW xs = {};
  if (ReadXInput(&xs)) {
    // A | B | X | Y | Start
    if (xs.Gamepad.wButtons & (0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x0010)) return true;
  }
  return false;
}

// The game keeps 7 "game context" objects in a table at exe+0x1651DB0 (created
// by the engine constructor at exe+0x1DA750, which allocates 0x850-byte
// objects). Entering StartScreenPhase dereferences table[1]; when the table was
// never created the game dies with an access violation at exe+0x1DFDCE.
// Forced phase transitions are therefore held back until the table exists.
// Set by a stub patched into the boot thread (exe+0x21ADD9) once the persistent phase
// has been constructed AND entered (OnEnter, exe+0x2F9200).
static volatile LONG64 g_bootThreadDone = 0;

static bool PhaseTransitionSafe(HMODULE hExe, void *curPhase) {
  (void)curPhase;
  if (!g_bootThreadDone) return false;
  // The persistent phase is built on the boot thread (ctor exe+0x2ED330, then
  // OnEnter exe+0x2F9200 which creates UI screen 0 into exe+0x18138D0[0]).
  // The main loop runs concurrently; any phase transition before that is done
  // dereferences null screens (AV at exe+0x331BA1). So hold back until ready.
  {
    void **screens = (void **)((uint8_t *)hExe + 0x018138D0);
    if (screens[0] == nullptr) {
      static volatile LONG s_notReady = 0;
      if ((InterlockedIncrement(&s_notReady) % 300) == 1) {
        log_line("[phase] Forced transition HELD: persistent phase not entered yet (screen table exe+0x18138D0[0] is null)\r\n");
      }
      return false;
    }
  }
  void **table = (void **)((uint8_t *)hExe + 0x01651DB0);
  for (int i = 0; i < 7; ++i) {
    if (table[i] == nullptr) {
      static volatile LONG s_blocked = 0;
      if ((InterlockedIncrement(&s_blocked) % 300) == 1) {
        log_line("[phase] Forced transition BLOCKED: game context table (exe+0x1651DB0) entry %d is null; "
                 "engine init (exe+0x1DA750) has not run yet. Skipping to avoid AV at exe+0x1DFDCE.\r\n", i);
      }
      return false;
    }
  }
  return true;
}

class MockGamepadReading : public IGamepadReading {
private:
  LONG m_ref = 1;
public:
  RawGamepadReading m_reading = {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (GuidEqual(riid, IID_IUnknown) ||
        GuidEqual(riid, IID_IInspectable) ||
        GuidEqual(riid, IID_IGamepadReadingEra)) {
      *ppv = static_cast<IGamepadReading*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Timestamp(UINT64 *value) override {
    if (value) *value = m_reading.Timestamp; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Buttons(DWORD *value) override {
    if (value) *value = m_reading.Buttons; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsDPadUpPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x100) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsDPadDownPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x200) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsDPadLeftPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x400) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsDPadRightPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x800) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsMenuPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x4) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsViewPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x8) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsLeftThumbstickPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x4000) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsRightThumbstickPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x8000) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsLeftShoulderPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x1000) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsRightShoulderPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x2000) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsAPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x10) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsBPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x20) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsXPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x40) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsYPressed(unsigned char *value) override {
    if (value) *value = (m_reading.Buttons & 0x80) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_LeftTrigger(float *value) override {
    if (value) *value = m_reading.LeftTrigger; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_RightTrigger(float *value) override {
    if (value) *value = m_reading.RightTrigger; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_LeftThumbstickX(float *value) override {
    if (value) *value = m_reading.LeftThumbstickX; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_LeftThumbstickY(float *value) override {
    if (value) *value = m_reading.LeftThumbstickY; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_RightThumbstickX(float *value) override {
    if (value) *value = m_reading.RightThumbstickX; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_RightThumbstickY(float *value) override {
    if (value) *value = m_reading.RightThumbstickY; return S_OK;
  }
};

class MockNavigationReading : public INavigationReading {
private:
  LONG m_ref = 1;
public:
  RawNavigationReading m_nav = {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (GuidEqual(riid, IID_IUnknown) ||
        GuidEqual(riid, IID_IInspectable) ||
        GuidEqual(riid, IID_INavigationReadingEra)) {
      *ppv = static_cast<INavigationReading*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Timestamp(UINT64 *value) override {
    if (value) *value = m_nav.Timestamp; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Buttons(DWORD *value) override {
    if (value) *value = m_nav.Buttons; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsUpPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x100) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsDownPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x200) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsLeftPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x400) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsRightPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x800) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsMenuPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x4) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsViewPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x8) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsPreviousPagePressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x1000) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsNextPagePressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x2000) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsAcceptPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x10) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsCancelPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x20) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsXPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x40) ? 1 : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsYPressed(unsigned char *value) override {
    if (value) *value = (m_nav.Buttons & 0x80) ? 1 : 0; return S_OK;
  }
};

class MockGamepad : public IGamepad, public IController, public INavigationController {
private:
  LONG m_ref = 1;
public:
  using IGamepad::QueryInterface;
  using IGamepad::AddRef;
  using IGamepad::Release;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (GuidEqual(riid, IID_IUnknown) ||
        GuidEqual(riid, IID_IInspectable) ||
        GuidEqual(riid, IID_IGamepadEra)) {
      *ppv = static_cast<IGamepad*>(this);
      AddRef();
      return S_OK;
    }
    if (GuidEqual(riid, IID_IControllerEra)) {
      *ppv = static_cast<IController*>(this);
      AddRef();
      return S_OK;
    }
    if (GuidEqual(riid, IID_INavigationControllerEra)) {
      *ppv = static_cast<INavigationController*>(this);
      AddRef();
      return S_OK;
    }
    log_guid("[MockGamepad] QI unknown", &riid);
    *ppv = static_cast<IGamepad*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    return CreateHString(L"Windows.Xbox.Input.Gamepad", name);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // --- IGamepad Methods ---
  // Slot 6: SetVibration
  HRESULT STDMETHODCALLTYPE SetVibration(GamepadVibration vib) override {
    ApplyVibration(vib);
    return S_OK;
  }
  // Slot 7: GetCurrentReading
  HRESULT STDMETHODCALLTYPE GetCurrentReading(IGamepadReading **ppReading) override {
    if (!ppReading) return E_POINTER;
    MockGamepadReading *reading = new MockGamepadReading();
    GetRawCurrentReading(&reading->m_reading);
    *ppReading = reading;
    return S_OK;
  }
  // Slot 8: GetRawCurrentReading
  HRESULT STDMETHODCALLTYPE GetRawCurrentReading(RawGamepadReading *pReading) override {
    if (!pReading) return E_POINTER;
    PollInput(pReading);
    return S_OK;
  }

  // --- IController Methods ---
  // Slot 6: get_Id
  HRESULT STDMETHODCALLTYPE get_Id(UINT64 *pId) override {
    if (pId) *pId = 1;
    return S_OK;
  }
  // Slot 7: get_Type
  HRESULT STDMETHODCALLTYPE get_Type(HSTRING *pType) override {
    return CreateHString(L"Windows.Xbox.Input.Gamepad", pType);
  }
  // Slot 8: get_User
  HRESULT STDMETHODCALLTYPE get_User(void **ppUser) override {
    if (ppUser) {
      *ppUser = &g_mockUser;
      g_mockUser.AddRef();
    }
    return S_OK;
  }

  // --- INavigationController Methods ---
  // Slot 6: GetNavigationReading
  HRESULT STDMETHODCALLTYPE GetNavigationReading(INavigationReading **ppReading) override {
    if (!ppReading) return E_POINTER;
    MockNavigationReading *nav = new MockNavigationReading();
    GetRawNavigationReading(&nav->m_nav);
    *ppReading = nav;
    return S_OK;
  }
  // Slot 7: GetRawNavigationReading
  HRESULT STDMETHODCALLTYPE GetRawNavigationReading(RawNavigationReading *pReading) override {
    if (!pReading) return E_POINTER;
    RawGamepadReading raw = {};
    PollInput(&raw);
    pReading->Timestamp = raw.Timestamp;
    pReading->Buttons = 0;
    if (raw.Buttons & 0x100) pReading->Buttons |= 0x100; // Up
    if (raw.Buttons & 0x200) pReading->Buttons |= 0x200; // Down
    if (raw.Buttons & 0x400) pReading->Buttons |= 0x400; // Left
    if (raw.Buttons & 0x800) pReading->Buttons |= 0x800; // Right
    if (raw.Buttons & 0x4)   pReading->Buttons |= 0x4;   // Menu
    if (raw.Buttons & 0x8)   pReading->Buttons |= 0x8;   // View
    if (raw.Buttons & 0x10)  pReading->Buttons |= 0x10;  // Accept (A)
    if (raw.Buttons & 0x20)  pReading->Buttons |= 0x20;  // Cancel (B)
    if (raw.Buttons & 0x40)  pReading->Buttons |= 0x40;  // X
    if (raw.Buttons & 0x80)  pReading->Buttons |= 0x80;  // Y
    if (raw.Buttons & 0x1000) pReading->Buttons |= 0x1000; // PrevPage (LB)
    if (raw.Buttons & 0x2000) pReading->Buttons |= 0x2000; // NextPage (RB)
    return S_OK;
  }
};
MockGamepad g_mockGamepad;

class MockGamepadStatics : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_Gamepads(void** value)
  virtual HRESULT STDMETHODCALLTYPE get_Gamepads(void **value) {
    if (!value) return E_POINTER;
    *value = &g_mockVectorViewGamepad;
    g_mockVectorViewGamepad.AddRef();
    log_line("[bridge] IGamepadStatics::get_Gamepads -> MockVectorViewGamepad\r\n");
    return S_OK;
  }
  // Slot 7: add_GamepadAdded
  virtual HRESULT STDMETHODCALLTYPE add_GamepadAdded(void *handler, int64_t *token) {
    if (token) *token = 1; return S_OK;
  }
  // Slot 8: remove_GamepadAdded
  virtual HRESULT STDMETHODCALLTYPE remove_GamepadAdded(int64_t token) { return S_OK; }
  // Slot 9: add_GamepadRemoved
  virtual HRESULT STDMETHODCALLTYPE add_GamepadRemoved(void *handler, int64_t *token) {
    if (token) *token = 2; return S_OK;
  }
  // Slot 10: remove_GamepadRemoved
  virtual HRESULT STDMETHODCALLTYPE remove_GamepadRemoved(int64_t token) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy11() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy12() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy13() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy14() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy15() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy16() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy17() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy18() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy19() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy20() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy21() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy22() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy23() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy24() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy25() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy26() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy27() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy28() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy29() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy30() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy31() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy32() { return S_OK; }
};
static MockGamepadStatics g_mockGamepadStatics;

class MockControllerStatics : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    if (name) *name = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_Controllers(IVectorView<IController*>** value)
  virtual HRESULT STDMETHODCALLTYPE get_Controllers(void **value) {
    if (!value) return E_POINTER;
    *value = &g_mockVectorViewController;
    g_mockVectorViewController.AddRef();
    log_line("[bridge] IControllerStatics::get_Controllers -> MockVectorViewController\r\n");
    return S_OK;
  }
  // Slot 7: add_ControllerAdded
  virtual HRESULT STDMETHODCALLTYPE add_ControllerAdded(void *handler, int64_t *token) {
    if (token) *token = 1; return S_OK;
  }
  // Slot 8: remove_ControllerAdded
  virtual HRESULT STDMETHODCALLTYPE remove_ControllerAdded(int64_t token) { return S_OK; }
  // Slot 9: add_ControllerRemoved
  virtual HRESULT STDMETHODCALLTYPE add_ControllerRemoved(void *handler, int64_t *token) {
    if (token) *token = 2; return S_OK;
  }
  // Slot 10: remove_ControllerRemoved
  virtual HRESULT STDMETHODCALLTYPE remove_ControllerRemoved(int64_t token) { return S_OK; }
  // Slot 11: add_ControllerPairingChanged
  virtual HRESULT STDMETHODCALLTYPE add_ControllerPairingChanged(void *handler, int64_t *token) {
    if (token) *token = 3; return S_OK;
  }
  // Slot 12: remove_ControllerPairingChanged
  virtual HRESULT STDMETHODCALLTYPE remove_ControllerPairingChanged(int64_t token) { return S_OK; }
  // Slot 13: add_ControllerOrderChanged
  virtual HRESULT STDMETHODCALLTYPE add_ControllerOrderChanged(void *handler, int64_t *token) {
    if (token) *token = 4; return S_OK;
  }
  // Slot 14: remove_ControllerOrderChanged
  virtual HRESULT STDMETHODCALLTYPE remove_ControllerOrderChanged(int64_t token) { return S_OK; }
  // Slot 15: GetControllerById
  virtual HRESULT STDMETHODCALLTYPE GetControllerById(UINT64 id, void **value) {
    if (!value) return E_POINTER;
    *value = (void*)(IController*)&g_mockGamepad;
    ((IController*)&g_mockGamepad)->AddRef();
    return S_OK;
  }
  // Slot 16: GetControllerMetadata
  virtual HRESULT STDMETHODCALLTYPE GetControllerMetadata(UINT64 unk, int version, void *unka) {
    return S_OK;
  }
  // Slot 17: GetControllersOrderedLeftToRight
  virtual HRESULT STDMETHODCALLTYPE GetControllersOrderedLeftToRight(int filter, void **value) {
    return get_Controllers(value);
  }
  virtual HRESULT STDMETHODCALLTYPE Dummy18() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy19() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy20() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy21() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy22() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy23() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy24() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy25() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy26() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy27() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy28() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy29() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy30() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy31() { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE Dummy32() { return S_OK; }
};
static MockControllerStatics g_mockControllerStatics;

// ============================================================================
// Universal WinRT / COM Mock for Unsupported Xbox Services (PlayGo,
// PackageTransfer, etc.)
// ============================================================================

static HRESULT STDMETHODCALLTYPE MockUniversal_QI(void *pThis, REFIID riid,
                                                  void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = pThis;
  return S_OK;
}

static ULONG STDMETHODCALLTYPE MockUniversal_AddRef(void *pThis) { return 2; }
static ULONG STDMETHODCALLTYPE MockUniversal_Release(void *pThis) { return 1; }

static HRESULT STDMETHODCALLTYPE MockUniversal_GetIids(void *pThis,
                                                       ULONG *count,
                                                       IID **iids) {
  if (count)
    *count = 0;
  if (iids)
    *iids = nullptr;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
MockUniversal_GetRuntimeClassName(void *pThis, HSTRING *name) {
  if (name)
    *name = nullptr;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
MockUniversal_GetTrustLevel(void *pThis, TrustLevel *level) {
  if (level)
    *level = BaseTrust;
  return S_OK;
}

// Slot 6 (+0x30): ActivateInstance / get_Current / Create
static HRESULT STDMETHODCALLTYPE MockUniversal_Method6_GetObj(void *pThis,
                                                              void **ppv, void **ppv2) {
  // Two shapes seen: Method6(this, &out) and Method6(this, <small value>, &out) (exe+0x9F20D).
  if ((uintptr_t)ppv <= 0x10000 && (uintptr_t)ppv2 > 0x10000 && (uintptr_t)ppv2 < 0x00007FFFFFFE0000ULL)
    *ppv2 = pThis;
  log_line("[mock_winrt] Method 6 (+0x30) called (arg=%p, ret=exe+0x%llX) -> returning universal mock "
           "object\r\n", (void *)ppv,
           (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr)));
  // Some interfaces pass a plain value here (e.g. 3 after the sign-in A press): only write
  // through something that looks like a real user-mode pointer.
  if ((uintptr_t)ppv > 0x10000 && (uintptr_t)ppv < 0x00007FFFFFFE0000ULL)
    *ppv = pThis;
  return S_OK;
}

// Slot 7 (+0x38): GetChunkStatus -> returns 2 (Installed)
static HRESULT STDMETHODCALLTYPE
MockUniversal_Method7_Status2(void *pThis, void *p1, DWORD *pOutStatus) {
  auto plausible = [](const void *p) { return (uintptr_t)p > 0x10000 && (uintptr_t)p < 0x00007FFFFFFE0000ULL; };
  // Also used as IVectorView::get_Size(this, &n) (exe+0x9C3F9): one out pointer in p1 and
  // garbage in p2 -> report an empty vector.
  if (plausible(p1) && !plausible(pOutStatus)) {
    log_line("[mock_winrt] Method 7 (+0x38) as get_Size -> 0\r\n");
    *(UINT32 *)p1 = 0;
    return S_OK;
  }
  log_line("[mock_winrt] Method 7 (+0x38, GetChunkStatus) called -> returning "
           "2 (INSTALLED)\r\n");
  if (plausible(pOutStatus))
    *pOutStatus = 2; // 2 = Complete / Installed
  return S_OK;
}

// Slot 10 (+0x50): GetOverallStatus -> returns 0 (Complete / Ready)
static HRESULT STDMETHODCALLTYPE
MockUniversal_Method10_Status0(void *pThis, DWORD *pOutStatus) {
  log_line("[mock_winrt] Method 10 (+0x50, GetOverallStatus) called -> "
           "returning 0 (READY)\r\n");
  if (pOutStatus)
    *pOutStatus = 0; // 0 = Ready / 100% complete
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE MockUniversal_GenericOk(void *pThis, ...) {
  return S_OK;
}

static void *g_UniversalMockVtable[64] = {
    (void *)&MockUniversal_QI,                  // 0: QueryInterface
    (void *)&MockUniversal_AddRef,              // 1: AddRef
    (void *)&MockUniversal_Release,             // 2: Release
    (void *)&MockUniversal_GetIids,             // 3: GetIids
    (void *)&MockUniversal_GetRuntimeClassName, // 4: GetRuntimeClassName
    (void *)&MockUniversal_GetTrustLevel,       // 5: GetTrustLevel
    (void *)&MockUniversal_Method6_GetObj,      // 6: Slot 6 (+0x30)
    (void *)&MockUniversal_Method7_Status2,     // 7: Slot 7 (+0x38)
    (void *)&MockUniversal_GenericOk,           // 8: Slot 8 (+0x40)
    (void *)&MockUniversal_GenericOk,           // 9: Slot 9 (+0x48)
    (void *)&MockUniversal_Method10_Status0,    // 10: Slot 10 (+0x50)
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk,
    (void *)&MockUniversal_GenericOk};

static struct UniversalWinRtObject {
  void **vtable;
} g_mockUniversalObject = {g_UniversalMockVtable};

// "Storage unavailable" mock: every method after the IInspectable slots fails, so the
// game's save code takes its error path instead of walking invented empty objects
// (the universal mock returned S_OK without filling out-params -> NULL derefs).
static HRESULT STDMETHODCALLTYPE MockFail_Generic(void *pThis, ...) {
  static volatile LONG s_n = 0;
  if (InterlockedIncrement(&s_n) <= 20)
    log_line("[mock_storage] method called on storage mock -> failing (E_FAIL)\r\n");
  return (HRESULT)0x80004005;  // E_FAIL
}
static void *g_FailMockVtable[64];
static struct FailWinRtObject { void **vtable; } g_mockFailObject = {g_FailMockVtable};
static void InitFailMockVtable() {
  for (int i = 0; i < 64; ++i) g_FailMockVtable[i] = (void *)&MockFail_Generic;
  g_FailMockVtable[0] = (void *)&MockUniversal_QI;
  g_FailMockVtable[1] = (void *)&MockUniversal_AddRef;
  g_FailMockVtable[2] = (void *)&MockUniversal_Release;
  g_FailMockVtable[3] = (void *)&MockUniversal_GetIids;
  g_FailMockVtable[4] = (void *)&MockUniversal_GetRuntimeClassName;
  g_FailMockVtable[5] = (void *)&MockUniversal_GetTrustLevel;
}

typedef HRESULT(WINAPI *PFN_RoGetActivationFactory)(HSTRING activatableClassId,
                                                    const IID &iid,
                                                    void **factory);
static PFN_RoGetActivationFactory g_TrueRoGetActivationFactory = nullptr;

// ============================================================================
// WinRT Mock: Windows.Foundation.Uri
// ============================================================================
static const GUID IID_IUriRuntimeClassFactory = {
    0x44a9796f, 0x723e, 0x4fdf, {0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84}};

static const GUID IID_IUriRuntimeClass = {
    0x9e365e57, 0x48b2, 0x4160, {0x95, 0x6f, 0xc7, 0x38, 0x51, 0x20, 0xbb, 0xfc}};

class MockUriRuntimeClass : public IInspectable {
private:
  LONG m_ref = 1;
  wchar_t m_rawUri[512] = L"wss://127.0.0.1/telemetry";

public:
  void SetUri(HSTRING hstr) {
    if (!hstr) return;
    static HMODULE hCombase = GetModuleHandleW(L"combase.dll");
    static auto pfnWindowsGetStringRawBuffer =
        (PFN_WindowsGetStringRawBuffer)GetProcAddress(hCombase, "WindowsGetStringRawBuffer");
    if (pfnWindowsGetStringRawBuffer) {
      UINT32 len = 0;
      const wchar_t *raw = pfnWindowsGetStringRawBuffer(hstr, &len);
      if (raw && len > 0) {
        wcsncpy_s(m_rawUri, raw, (len < 511) ? len : 511);
        m_rawUri[(len < 511) ? len : 511] = L'\0';
      }
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (memcmp(&riid, &IID_IUriRuntimeClass, sizeof(GUID)) == 0 ||
        memcmp(&riid, &IID_IInspectable, sizeof(GUID)) == 0 ||
        memcmp(&riid, &IID_IUnknown, sizeof(GUID)) == 0) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    return CreateHString(L"Windows.Foundation.Uri", name);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: get_AbsoluteUri
  virtual HRESULT STDMETHODCALLTYPE get_AbsoluteUri(HSTRING *value) { return CreateHString(m_rawUri, value); }
  // Slot 7: get_DisplayUri
  virtual HRESULT STDMETHODCALLTYPE get_DisplayUri(HSTRING *value) { return CreateHString(m_rawUri, value); }
  // Slot 8: get_Domain
  virtual HRESULT STDMETHODCALLTYPE get_Domain(HSTRING *value) { return CreateHString(L"127.0.0.1", value); }
  // Slot 9: get_Extension
  virtual HRESULT STDMETHODCALLTYPE get_Extension(HSTRING *value) { return CreateHString(L"", value); }
  // Slot 10: get_Fragment
  virtual HRESULT STDMETHODCALLTYPE get_Fragment(HSTRING *value) { return CreateHString(L"", value); }
  // Slot 11: get_Host
  virtual HRESULT STDMETHODCALLTYPE get_Host(HSTRING *value) { return CreateHString(L"127.0.0.1", value); }
  // Slot 12: get_Password
  virtual HRESULT STDMETHODCALLTYPE get_Password(HSTRING *value) { return CreateHString(L"", value); }
  // Slot 13: get_Path
  virtual HRESULT STDMETHODCALLTYPE get_Path(HSTRING *value) { return CreateHString(L"/telemetry", value); }
  // Slot 14: get_Query
  virtual HRESULT STDMETHODCALLTYPE get_Query(HSTRING *value) { return CreateHString(L"", value); }
  // Slot 15: get_QueryParsed
  virtual HRESULT STDMETHODCALLTYPE get_QueryParsed(void **value) { if (value) *value = nullptr; return S_OK; }
  // Slot 16: get_RawUri
  virtual HRESULT STDMETHODCALLTYPE get_RawUri(HSTRING *value) { return CreateHString(m_rawUri, value); }
  // Slot 17: get_SchemeName
  virtual HRESULT STDMETHODCALLTYPE get_SchemeName(HSTRING *value) { return CreateHString(L"wss", value); }
  // Slot 18: get_UserName
  virtual HRESULT STDMETHODCALLTYPE get_UserName(HSTRING *value) { return CreateHString(L"", value); }
  // Slot 19: get_Port
  virtual HRESULT STDMETHODCALLTYPE get_Port(INT32 *value) { if (value) *value = 443; return S_OK; }
  // Slot 20: get_Suspicious
  virtual HRESULT STDMETHODCALLTYPE get_Suspicious(boolean *value) { if (value) *value = 0; return S_OK; }
  // Slot 21: Equals
  virtual HRESULT STDMETHODCALLTYPE Equals(void *pUri, boolean *value) { if (value) *value = 1; return S_OK; }
  // Slot 22: CombineUri
  virtual HRESULT STDMETHODCALLTYPE CombineUri(HSTRING rel, void **instance) {
    if (instance) { *instance = this; AddRef(); } return S_OK;
  }
};
static MockUriRuntimeClass g_mockUri;

class MockUriRuntimeClassFactory : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
    if (count) *count = 0; if (iids) *iids = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
    return CreateHString(L"Windows.Foundation.Uri", name);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
    if (level) *level = BaseTrust; return S_OK;
  }

  // Slot 6: CreateUri(HSTRING uri, IUriRuntimeClass **instance)
  virtual HRESULT STDMETHODCALLTYPE CreateUri(HSTRING uri, void **instance) {
    log_line("[mock_uri] CreateUri called\r\n");
    if (!instance) return E_POINTER;
    g_mockUri.SetUri(uri);
    *instance = &g_mockUri;
    g_mockUri.AddRef();
    return S_OK;
  }
  // Slot 7: CreateWithRelativeUri(HSTRING baseUri, HSTRING relativeUri, IUriRuntimeClass **instance)
  virtual HRESULT STDMETHODCALLTYPE CreateWithRelativeUri(HSTRING baseUri, HSTRING relativeUri, void **instance) {
    log_line("[mock_uri] CreateWithRelativeUri called\r\n");
    if (!instance) return E_POINTER;
    g_mockUri.SetUri(relativeUri ? relativeUri : baseUri);
    *instance = &g_mockUri;
    g_mockUri.AddRef();
    return S_OK;
  }
};
static MockUriRuntimeClassFactory g_mockUriFactory;

// ============================================================================
// WinRT Mock: Windows.Networking.Sockets.MessageWebSocket
// ============================================================================
static const GUID IID_IMessageWebSocket = {
    0x33727d08, 0x34d5, 0x4746, {0xad, 0x7b, 0x8d, 0xde, 0x5b, 0xc2, 0xef, 0x88}};

static const GUID IID_IWebSocket = {
    0xf877396f, 0x99b1, 0x4e18, {0xbc, 0x08, 0x85, 0x0c, 0x9a, 0xdf, 0x15, 0x6e}};

static const GUID IID_IWebSocketControl = {
    0x2ec4bdc3, 0xd9a5, 0x455a, {0x98, 0x11, 0xde, 0x24, 0xd4, 0x53, 0x37, 0xe9}};

static const GUID IID_IMessageWebSocketControl = {
    0x8118388a, 0xc629, 0x4f0a, {0x80, 0xfb, 0x81, 0xfc, 0x05, 0x53, 0x88, 0x62}};

static const GUID IID_IAsyncAction = {
    0xa4ed5c81, 0x76c9, 0x40bd, {0x8b, 0xe6, 0xb1, 0xd9, 0x0f, 0xb2, 0x0a, 0xe7}};

// IVector<HSTRING> implementation for SupportedProtocols
class MockVectorHString : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this; AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Foundation.Collections.IVector`1<String>", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // Slot 6 (+0x30): GetAt
  virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned int index, HSTRING *item) { if (item) *item = nullptr; return S_OK; }
  // Slot 7 (+0x38): get_Size
  virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned int *size) { if (size) *size = 0; return S_OK; }
  // Slot 8 (+0x40): GetView
  virtual HRESULT STDMETHODCALLTYPE GetView(void **view) { if (view) *view = nullptr; return S_OK; }
  // Slot 9 (+0x48): IndexOf
  virtual HRESULT STDMETHODCALLTYPE IndexOf(HSTRING item, unsigned int *index, boolean *found) { if (found) *found = 0; return S_OK; }
  // Slot 10 (+0x50): SetAt
  virtual HRESULT STDMETHODCALLTYPE SetAt(unsigned int index, HSTRING item) { return S_OK; }
  // Slot 11 (+0x58): InsertAt
  virtual HRESULT STDMETHODCALLTYPE InsertAt(unsigned int index, HSTRING item) { return S_OK; }
  // Slot 12 (+0x60): RemoveAt
  virtual HRESULT STDMETHODCALLTYPE RemoveAt(unsigned int index) { return S_OK; }
  // Slot 13 (+0x68): Append(HSTRING item)
  virtual HRESULT STDMETHODCALLTYPE Append(HSTRING item) {
    log_line("[mock_vector] IVector<HSTRING>::Append called\r\n");
    return S_OK;
  }
  // Slot 14 (+0x70): RemoveAtEnd
  virtual HRESULT STDMETHODCALLTYPE RemoveAtEnd() { return S_OK; }
  // Slot 15 (+0x78): Clear
  virtual HRESULT STDMETHODCALLTYPE Clear() { return S_OK; }
};
static MockVectorHString g_mockVectorHString;

class MockWebSocketControl : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Networking.Sockets.WebSocketControl", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // IWebSocketControl methods:
  // Slot 6 (+0x30): get_OutboundBufferSizeInBytes
  virtual HRESULT STDMETHODCALLTYPE get_OutboundBufferSizeInBytes(UINT32 *val) { if (val) *val = 65536; return S_OK; }
  // Slot 7 (+0x38): put_OutboundBufferSizeInBytes
  virtual HRESULT STDMETHODCALLTYPE put_OutboundBufferSizeInBytes(UINT32 val) { return S_OK; }
  // Slot 8 (+0x40): get_ServerCredential
  virtual HRESULT STDMETHODCALLTYPE get_ServerCredential(void **val) { if (val) *val = nullptr; return S_OK; }
  // Slot 9 (+0x48): put_ServerCredential
  virtual HRESULT STDMETHODCALLTYPE put_ServerCredential(void *val) { return S_OK; }
  // Slot 10 (+0x50): get_ProxyCredential
  virtual HRESULT STDMETHODCALLTYPE get_ProxyCredential(void **val) { if (val) *val = nullptr; return S_OK; }
  // Slot 11 (+0x58): put_ProxyCredential
  virtual HRESULT STDMETHODCALLTYPE put_ProxyCredential(void *val) { return S_OK; }
  // Slot 12 (+0x60): get_SupportedProtocols
  virtual HRESULT STDMETHODCALLTYPE get_SupportedProtocols(void **val) {
    log_line("[mock_websocket] IWebSocketControl::get_SupportedProtocols -> &g_mockVectorHString\r\n");
    if (!val) return E_POINTER;
    *val = &g_mockVectorHString;
    g_mockVectorHString.AddRef();
    return S_OK;
  }
};
static MockWebSocketControl g_mockWebSocketControl;

class MockMessageWebSocketControl : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (memcmp(&riid, &IID_IWebSocketControl, sizeof(GUID)) == 0) {
      log_line("[mock_websocket] MessageWebSocketControl QI for IWebSocketControl -> &g_mockWebSocketControl\r\n");
      *ppv = (IInspectable*)&g_mockWebSocketControl;
      g_mockWebSocketControl.AddRef();
      return S_OK;
    }
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Networking.Sockets.MessageWebSocketControl", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // IMessageWebSocketControl methods:
  // Slot 6 (+0x30): get_MaxMessageSize
  virtual HRESULT STDMETHODCALLTYPE get_MaxMessageSize(UINT32 *val) { if (val) *val = 65536; return S_OK; }
  // Slot 7 (+0x38): put_MaxMessageSize
  virtual HRESULT STDMETHODCALLTYPE put_MaxMessageSize(UINT32 val) { return S_OK; }
  // Slot 8 (+0x40): get_MessageType
  virtual HRESULT STDMETHODCALLTYPE get_MessageType(int *val) { if (val) *val = 0; return S_OK; }
  // Slot 9 (+0x48): put_MessageType
  virtual HRESULT STDMETHODCALLTYPE put_MessageType(int val) { return S_OK; }
};
static MockMessageWebSocketControl g_mockMessageWebSocketControl;

inline HRESULT STDMETHODCALLTYPE MockWebSocketControl::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (memcmp(&riid, &IID_IWebSocketControl, sizeof(GUID)) == 0) {
    *ppv = (IInspectable*)this;
    AddRef();
    return S_OK;
  }
  if (memcmp(&riid, &IID_IMessageWebSocketControl, sizeof(GUID)) == 0) {
    *ppv = (IInspectable*)&g_mockMessageWebSocketControl;
    g_mockMessageWebSocketControl.AddRef();
    return S_OK;
  }
  *ppv = this;
  AddRef();
  return S_OK;
}

class MockWebSocketAsyncAction : public IInspectable {
private:
  LONG m_ref = 1;
  void *m_handler = nullptr;
public:
  MockAsyncInfoImpl m_asyncInfo;

  MockWebSocketAsyncAction() {
    m_asyncInfo.m_parent = this;
    m_asyncInfo.m_status = 3; /* Error */
    m_asyncInfo.m_error = 0x8007274C; /* WSAETIMEDOUT: Server offline */
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (memcmp(&riid, &IID_IAsyncInfo, sizeof(GUID)) == 0) {
      *ppv = &m_asyncInfo;
      AddRef();
      return S_OK;
    }
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Foundation.IAsyncAction", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // Slot 6: put_Completed(handler)
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) {
    log_line("[mock_websocket] ConnectAsync put_Completed(handler=%p)\r\n", handler);
    m_handler = handler;
    if (handler) {
      typedef HRESULT (STDMETHODCALLTYPE *PFN_ActionCompletedInvoke)(void *pThis, void *pAsyncInfo, int status);
      void **vtbl = *(void***)handler;
      if (vtbl && vtbl[3]) {
        PFN_ActionCompletedInvoke pfn = (PFN_ActionCompletedInvoke)vtbl[3];
        static const bool s_sync = GetEnvironmentVariableA("ONRUSH_WS_SYNC_COMPLETE", nullptr, 0) > 0;
        if (s_sync) {
          log_line("[mock_websocket] Invoking ConnectAsync completion handler slot 3 -> %p with status=3 (Offline)\r\n", pfn);
          pfn(handler, this, 3 /* Error / Offline */);
        } else {
          // Complete asynchronously, like a real socket: calling the handler inside put_Completed
          // re-entered the game's networking code while it still held its lock -> the game hung
          // when two GameSparks connections were started at once (entering gameplay).
          struct Ctx { PFN_ActionCompletedInvoke pfn; void *handler; void *self; };
          Ctx *c = new Ctx{ pfn, handler, this };
          ((IUnknown *)handler)->AddRef();
          log_line("[mock_websocket] ConnectAsync: completing (status=3, offline) on a worker thread\r\n");
          if (!QueueUserWorkItem([](void *p) -> DWORD {
                Ctx *c = (Ctx *)p;
                Sleep(50);
                c->pfn(c->handler, c->self, 3 /* Error / Offline */);
                ((IUnknown *)c->handler)->Release();
                delete c;
                return 0;
              }, c, WT_EXECUTEDEFAULT)) {
            c->pfn(c->handler, c->self, 3); ((IUnknown *)handler)->Release(); delete c;
          }
        }
      }
    }
    return S_OK;
  }
  // Slot 7: get_Completed(handler)
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) {
    if (handler) *handler = m_handler;
    return S_OK;
  }
  // Slot 8: GetResults()
  virtual HRESULT STDMETHODCALLTYPE GetResults() {
    return S_OK;
  }
};
static MockWebSocketAsyncAction g_mockWebSocketConnectAction;

class MockWebSocket : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Networking.Sockets.IWebSocket", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // IWebSocket methods:
  // Slot 6 (+0x30): get_OutputStream
  virtual HRESULT STDMETHODCALLTYPE get_OutputStream(void **val) {
    if (val) *val = &g_mockUniversalObject; return S_OK;
  }
  // Slot 7 (+0x38): ConnectAsync
  virtual HRESULT STDMETHODCALLTYPE ConnectAsync(void *uri, void **operation) {
    log_line("[mock_websocket] IWebSocket::ConnectAsync(uri=%p) -> &g_mockWebSocketConnectAction\r\n", uri);
    if (!operation) return E_POINTER;
    *operation = &g_mockWebSocketConnectAction;
    g_mockWebSocketConnectAction.AddRef();
    return S_OK;
  }
  // Slot 8 (+0x40): SetRequestHeader
  virtual HRESULT STDMETHODCALLTYPE SetRequestHeader(HSTRING name, HSTRING val) { return S_OK; }
  // Slot 9 (+0x48): add_Closed
  virtual HRESULT STDMETHODCALLTYPE add_Closed(void *handler, int64_t *token) { if (token) *token = 2; return S_OK; }
  // Slot 10 (+0x50): remove_Closed
  virtual HRESULT STDMETHODCALLTYPE remove_Closed(int64_t token) { return S_OK; }
  // Slot 11 (+0x58): CloseWithStatus
  virtual HRESULT STDMETHODCALLTYPE CloseWithStatus(UINT16 code, HSTRING reason) { return S_OK; }
};
static MockWebSocket g_mockWebSocket;

class MockMessageWebSocket : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (memcmp(&riid, &IID_IWebSocket, sizeof(GUID)) == 0) {
      log_line("[mock_websocket] QI for IWebSocket -> &g_mockWebSocket\r\n");
      *ppv = (IInspectable*)&g_mockWebSocket;
      g_mockWebSocket.AddRef();
      return S_OK;
    }
    if (memcmp(&riid, &IID_IWebSocketControl, sizeof(GUID)) == 0) {
      log_line("[mock_websocket] QI for IWebSocketControl -> &g_mockWebSocketControl\r\n");
      *ppv = (IInspectable*)&g_mockWebSocketControl;
      g_mockWebSocketControl.AddRef();
      return S_OK;
    }
    if (memcmp(&riid, &IID_IMessageWebSocketControl, sizeof(GUID)) == 0) {
      log_line("[mock_websocket] QI for IMessageWebSocketControl -> &g_mockMessageWebSocketControl\r\n");
      *ppv = (IInspectable*)&g_mockMessageWebSocketControl;
      g_mockMessageWebSocketControl.AddRef();
      return S_OK;
    }
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Networking.Sockets.MessageWebSocket", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // IMessageWebSocket methods:
  // Slot 6 (+0x30): get_Control
  virtual HRESULT STDMETHODCALLTYPE get_Control(void **value) {
    log_line("[mock_websocket] get_Control -> &g_mockMessageWebSocketControl\r\n");
    if (!value) return E_POINTER;
    *value = &g_mockMessageWebSocketControl;
    g_mockMessageWebSocketControl.AddRef();
    return S_OK;
  }
  // Slot 7 (+0x38): get_Information
  virtual HRESULT STDMETHODCALLTYPE get_Information(void **value) {
    if (!value) return E_POINTER;
    *value = &g_mockUniversalObject;
    return S_OK;
  }
  // Slot 8 (+0x40): add_MessageReceived
  virtual HRESULT STDMETHODCALLTYPE add_MessageReceived(void *handler, int64_t *token) {
    log_line("[mock_websocket] add_MessageReceived(handler=%p)\r\n", handler);
    if (token) *token = 1;
    return S_OK;
  }
  // Slot 9 (+0x48): remove_MessageReceived
  virtual HRESULT STDMETHODCALLTYPE remove_MessageReceived(int64_t token) {
    return S_OK;
  }
  // Slot 10 (+0x50): Close (IClosable)
  virtual HRESULT STDMETHODCALLTYPE Close() {
    return S_OK;
  }
};
static MockMessageWebSocket g_mockMessageWebSocket;

inline HRESULT STDMETHODCALLTYPE MockWebSocket::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (memcmp(&riid, &IID_IMessageWebSocket, sizeof(GUID)) == 0) {
    *ppv = (IInspectable*)&g_mockMessageWebSocket;
    g_mockMessageWebSocket.AddRef();
    return S_OK;
  }
  return g_mockMessageWebSocket.QueryInterface(riid, ppv);
}

class MockMessageWebSocketFactory : public IInspectable {
private:
  LONG m_ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = this; AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
  ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&m_ref); }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG *c, IID **i) override { if (c) *c=0; if (i) *i=nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {
    return CreateHString(L"Windows.Networking.Sockets.MessageWebSocket", n);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *l) override { if (l) *l=BaseTrust; return S_OK; }

  // Slot 6: ActivateInstance
  virtual HRESULT STDMETHODCALLTYPE ActivateInstance(void **instance) {
    log_line("[mock_websocket] ActivateInstance -> &g_mockMessageWebSocket\r\n");
    if (!instance) return E_POINTER;
    *instance = &g_mockMessageWebSocket;
    g_mockMessageWebSocket.AddRef();
    return S_OK;
  }
};
static MockMessageWebSocketFactory g_mockMessageWebSocketFactory;

#include "connected_storage.inc"

static HRESULT WINAPI HookedRoGetActivationFactory(HSTRING activatableClassId,
                                                   const IID &iid,
                                                   void **factory) {
  static HMODULE hCombase = GetModuleHandleW(L"combase.dll");
  static auto pfnWindowsGetStringRawBuffer =
      (PFN_WindowsGetStringRawBuffer)GetProcAddress(
          hCombase, "WindowsGetStringRawBuffer");

  UINT32 len = 0;
  const wchar_t *raw =
      pfnWindowsGetStringRawBuffer
          ? pfnWindowsGetStringRawBuffer(activatableClassId, &len)
          : nullptr;
  log_line("[bridge] RoGetActivationFactory('%ls')\r\n", raw ? raw : L"(null)");

  if (raw &&
      wcscmp(raw, L"Windows.ApplicationModel.Core.CoreApplication") == 0) {
    log_line("[bridge] -> Intercepted "
             "Windows.ApplicationModel.Core.CoreApplication!\r\n");
    return g_coreAppDesktop.QueryInterface(iid, factory);
  }

  if (raw && wcscmp(raw, L"Windows.UI.Core.CoreWindow") == 0) {
    log_line("[bridge] -> Intercepted Windows.UI.Core.CoreWindow!\r\n");
    return g_mockCoreWindowStatic.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Storage.ApplicationData") == 0 ||
              wcsstr(raw, L"Windows.Storage.ApplicationData") != nullptr)) {
    log_line("[bridge] -> Intercepted Windows.Storage.ApplicationData!\r\n");
    return g_mockApplicationDataStatics.QueryInterface(iid, factory);
  }

  if (raw &&
      (wcscmp(raw, L"Windows.Xbox.Graphics.Display.DisplayConfiguration") ==
           0 ||
       wcsstr(raw, L"Windows.Xbox.Graphics.Display.DisplayConfiguration") !=
           nullptr)) {
    log_line("[bridge] -> Intercepted "
             "Windows.Xbox.Graphics.Display.DisplayConfiguration!\r\n");
    return g_mockDisplayConfigurationStatics.QueryInterface(iid, factory);
  }

  if (raw && wcscmp(raw, L"Windows.ApplicationModel.Store.CurrentApp") == 0) {
    log_line("[bridge] -> Intercepted Windows.ApplicationModel.Store.CurrentApp (mock license)\r\n");
    return g_mockCurrentAppStatics.QueryInterface(iid, factory);
  }

  if (raw && wcscmp(raw, L"Windows.System.Profile.AnalyticsInfo") == 0) {
    log_line("[bridge] -> Intercepted Windows.System.Profile.AnalyticsInfo "
             "with typed MockAnalyticsInfoStatics!\r\n");
    return g_mockAnalyticsInfoStatics.QueryInterface(iid, factory);
  }

  if (raw && wcscmp(raw, L"Windows.Xbox.Services.XboxLiveConfiguration") == 0) {
    log_line(
        "[bridge] -> Intercepted Windows.Xbox.Services.XboxLiveConfiguration "
        "with typed MockXboxLiveConfigurationStatics!\r\n");
    return g_mockXboxLiveConfigurationStatics.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Xbox.System.User") == 0 ||
              wcsstr(raw, L"Windows.Xbox.System.User") != nullptr)) {
    log_line("[bridge] -> Intercepted Windows.Xbox.System.User with MockUserStatics!\r\n");
    return g_mockUserStatics.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Xbox.Input.Gamepad") == 0 ||
              wcsstr(raw, L"Windows.Xbox.Input.Gamepad") != nullptr)) {
    log_line("[bridge] -> Intercepted Windows.Xbox.Input.Gamepad with typed "
             "MockGamepadStatics!\r\n");
    return g_mockGamepadStatics.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Xbox.Input.Controller") == 0 ||
              wcsstr(raw, L"Windows.Xbox.Input.Controller") != nullptr)) {
    log_line("[bridge] -> Intercepted Windows.Xbox.Input.Controller with MockControllerStatics!\r\n");
    return g_mockControllerStatics.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Foundation.Uri") == 0 ||
              wcsstr(raw, L"Windows.Foundation.Uri") != nullptr)) {
    log_line("[bridge] -> Intercepted Windows.Foundation.Uri with MockUriRuntimeClassFactory!\r\n");
    return g_mockUriFactory.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Networking.Sockets.MessageWebSocket") == 0 ||
              wcsstr(raw, L"Windows.Networking.Sockets.MessageWebSocket") != nullptr)) {
    log_line("[bridge] -> Intercepted Windows.Networking.Sockets.MessageWebSocket with MockMessageWebSocketFactory!\r\n");
    return g_mockMessageWebSocketFactory.QueryInterface(iid, factory);
  }

  if (raw && (wcsstr(raw, L"Windows.Xbox.Management.Deployment") != nullptr ||
              wcsstr(raw, L"Windows.ApplicationModel.Package") != nullptr)) {
    log_line("[bridge] -> Intercepted %ls with Universal Mock!\r\n", raw);
    if (factory)
      *factory = &g_mockUniversalObject;
    return S_OK;
  }

  if (raw && wcsstr(raw, L"Windows.Xbox.Storage.ConnectedStorageSpace") != nullptr && CsEnabled()) {
    log_line("[bridge] -> Intercepted %ls with file-backed ConnectedStorage\r\n", raw);
    return g_csStatics.QueryInterface(iid, factory);
  }
  if (raw && wcsstr(raw, L"Windows.Xbox.Storage") != nullptr) {
    if (!g_FailMockVtable[6]) InitFailMockVtable();
    log_line("[bridge] -> Intercepted %ls with failing storage mock\r\n", raw);
    if (factory) *factory = &g_mockFailObject;
    return S_OK;
  }

  if (g_TrueRoGetActivationFactory) {
    HRESULT hr = g_TrueRoGetActivationFactory(activatableClassId, iid, factory);
    if (SUCCEEDED(hr))
      return hr;
    log_line("[bridge] True RoGetActivationFactory('%ls') returned hr=0x%08lX "
             "-> falling back to universal mock\r\n",
             raw ? raw : L"(null)", hr);
  }

  log_line("[bridge] RoGetActivationFactory('%ls') fallback -> returning "
           "universal mock\r\n",
           raw ? raw : L"(null)");
  if (factory)
    *factory = &g_mockUniversalObject;
  return S_OK;
}

typedef HRESULT(__cdecl *PFN_GetActivationFactoryByPCWSTR)(void *, const IID &,
                                                           void **);
static PFN_GetActivationFactoryByPCWSTR g_TrueGetActivationFactoryByPCWSTR =
    nullptr;

static HRESULT __cdecl HookedGetActivationFactoryByPCWSTR(void *pClassName,
                                                          const IID &iid,
                                                          void **factory) {
  const wchar_t *raw = (const wchar_t *)pClassName;
  log_line("[vccorlib] GetActivationFactoryByPCWSTR('%ls')\r\n",
           raw ? raw : L"(null)");

  if (!factory)
    return E_POINTER;
  *factory = nullptr;

  if (raw &&
      wcscmp(raw, L"Windows.ApplicationModel.Core.CoreApplication") == 0) {
    return g_coreAppDesktop.QueryInterface(iid, factory);
  }
  if (raw && wcscmp(raw, L"Windows.UI.Core.CoreWindow") == 0) {
    log_line("[vccorlib] -> Intercepted Windows.UI.Core.CoreWindow!\r\n");
    return g_mockCoreWindowStatic.QueryInterface(iid, factory);
  }
  if (raw && (wcscmp(raw, L"Windows.Storage.ApplicationData") == 0 ||
              wcsstr(raw, L"Windows.Storage.ApplicationData") != nullptr)) {
    return g_mockApplicationDataStatics.QueryInterface(iid, factory);
  }
  if (raw &&
      (wcscmp(raw, L"Windows.Xbox.Graphics.Display.DisplayConfiguration") ==
           0 ||
       wcsstr(raw, L"Windows.Xbox.Graphics.Display.DisplayConfiguration") !=
           nullptr)) {
    log_line("[vccorlib] -> Intercepted "
             "Windows.Xbox.Graphics.Display.DisplayConfiguration!\r\n");
    return g_mockDisplayConfigurationStatics.QueryInterface(iid, factory);
  }
  if (raw && wcscmp(raw, L"Windows.ApplicationModel.Store.CurrentApp") == 0) {
    log_line("[vccorlib] -> Intercepted Windows.ApplicationModel.Store.CurrentApp (mock license)\r\n");
    return g_mockCurrentAppStatics.QueryInterface(iid, factory);
  }

  if (raw && wcscmp(raw, L"Windows.System.Profile.AnalyticsInfo") == 0) {
    log_line("[vccorlib] -> Intercepted Windows.System.Profile.AnalyticsInfo "
             "with typed MockAnalyticsInfoStatics!\r\n");
    return g_mockAnalyticsInfoStatics.QueryInterface(iid, factory);
  }
  if (raw && wcscmp(raw, L"Windows.Xbox.Services.XboxLiveConfiguration") == 0) {
    log_line(
        "[vccorlib] -> Intercepted Windows.Xbox.Services.XboxLiveConfiguration "
        "with typed MockXboxLiveConfigurationStatics!\r\n");
    return g_mockXboxLiveConfigurationStatics.QueryInterface(iid, factory);
  }
  if (raw && (wcscmp(raw, L"Windows.Xbox.System.User") == 0 ||
              wcsstr(raw, L"Windows.Xbox.System.User") != nullptr)) {
    log_line("[vccorlib] -> Intercepted Windows.Xbox.System.User with MockUserStatics!\r\n");
    return g_mockUserStatics.QueryInterface(iid, factory);
  }
  if (raw && (wcscmp(raw, L"Windows.Xbox.Input.Gamepad") == 0 ||
              wcsstr(raw, L"Windows.Xbox.Input.Gamepad") != nullptr)) {
    log_line("[vccorlib] -> Intercepted Windows.Xbox.Input.Gamepad with typed "
             "MockGamepadStatics!\r\n");
    return g_mockGamepadStatics.QueryInterface(iid, factory);
  }
  if (raw && (wcscmp(raw, L"Windows.Xbox.Input.Controller") == 0 ||
              wcsstr(raw, L"Windows.Xbox.Input.Controller") != nullptr)) {
    log_line("[vccorlib] -> Intercepted Windows.Xbox.Input.Controller with MockControllerStatics!\r\n");
    return g_mockControllerStatics.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Foundation.Uri") == 0 ||
              wcsstr(raw, L"Windows.Foundation.Uri") != nullptr)) {
    log_line("[vccorlib] -> Intercepted Windows.Foundation.Uri with MockUriRuntimeClassFactory!\r\n");
    return g_mockUriFactory.QueryInterface(iid, factory);
  }

  if (raw && (wcscmp(raw, L"Windows.Networking.Sockets.MessageWebSocket") == 0 ||
              wcsstr(raw, L"Windows.Networking.Sockets.MessageWebSocket") != nullptr)) {
    log_line("[vccorlib] -> Intercepted Windows.Networking.Sockets.MessageWebSocket with MockMessageWebSocketFactory!\r\n");
    return g_mockMessageWebSocketFactory.QueryInterface(iid, factory);
  }

  if (raw && wcsstr(raw, L"Windows.Xbox.Storage.ConnectedStorageSpace") != nullptr && CsEnabled()) {
    log_line("[vccorlib] -> Intercepted %ls with file-backed ConnectedStorage\r\n", raw);
    return g_csStatics.QueryInterface(iid, factory);
  }
  if (raw && wcsstr(raw, L"Windows.Xbox.Storage") != nullptr) {
    if (!g_FailMockVtable[6]) InitFailMockVtable();
    log_line("[vccorlib] -> Intercepted %ls with failing storage mock\r\n", raw);
    if (factory) *factory = &g_mockFailObject;
    return S_OK;
  }

  if (g_TrueGetActivationFactoryByPCWSTR) {
    HRESULT hr = g_TrueGetActivationFactoryByPCWSTR(pClassName, iid, factory);
    if (SUCCEEDED(hr) && *factory)
      return hr;
  }

  log_line("[vccorlib] GetActivationFactoryByPCWSTR('%ls') -> returning "
           "universal mock (S_OK)\r\n",
           raw ? raw : L"(null)");
  *factory = &g_mockUniversalObject;
  return S_OK;
}

static void __cdecl HookedWinRTraiseClassNotRegisteredException() {
  log_line("[vccorlib] WARNING: __abi_WinRTraiseClassNotRegisteredException() "
           "called and suppressed from %p!\r\n",
           _ReturnAddress());
}

static void __cdecl HookedWinRTraiseCOMException(HRESULT hr) {
  log_line("[vccorlib] WARNING: __abi_WinRTraiseCOMException(0x%08lX) called "
           "and suppressed from %p!\r\n",
           hr, _ReturnAddress());
}

static void __cdecl HookedWinRTraiseFailureException() {
  log_line("[vccorlib] WARNING: __abi_WinRTraiseFailureException() called and "
           "suppressed from %p!\r\n",
           _ReturnAddress());
}

typedef HRESULT(__cdecl *PFN_XimSetIntendedLocalXboxUserIds)(uint32_t, const uint64_t *);
static PFN_XimSetIntendedLocalXboxUserIds g_TrueXimSetIntendedLocalXboxUserIds = nullptr;
static HRESULT __cdecl HookedXimSetIntendedLocalXboxUserIds(uint32_t count, const uint64_t *xuids) {
  HRESULT hr = g_TrueXimSetIntendedLocalXboxUserIds ? g_TrueXimSetIntendedLocalXboxUserIds(count, xuids) : E_FAIL;
  log_line("[xim] XimSetIntendedLocalXboxUserIds(count=%u, xuid0=%llX) -> 0x%08X%s\r\n", count,
           (unsigned long long)(count && xuids ? xuids[0] : 0), (unsigned)hr, FAILED(hr) ? " (reported as S_OK)" : "");
  return S_OK;
}
static void __cdecl HookedXimFailFastWithInform(void *a, void *b, void *c, void *d) {
  log_line("[xim] XimFailFastWithInform(%p, %p, %p, %p) from %p ignored\r\n", a, b, c, d, _ReturnAddress());
}

static bool HookIAT(HMODULE hTargetModule, const char *importDllName,
                    const char *functionName, void *newFunc, void **origFunc) {
  if (!hTargetModule)
    return false;
  auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(hTargetModule);
  if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
      reinterpret_cast<PBYTE>(hTargetModule) + dosHeader->e_lfanew);
  auto importDir =
      &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (importDir->VirtualAddress == 0)
    return false;

  auto importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
      reinterpret_cast<PBYTE>(hTargetModule) + importDir->VirtualAddress);

  for (; importDesc->Name; ++importDesc) {
    const char *dllName = reinterpret_cast<const char *>(
        reinterpret_cast<PBYTE>(hTargetModule) + importDesc->Name);
    if (_stricmp(dllName, importDllName) != 0)
      continue;

    auto thunkOrig = reinterpret_cast<PIMAGE_THUNK_DATA>(
        reinterpret_cast<PBYTE>(hTargetModule) +
        importDesc->OriginalFirstThunk);
    auto thunkIAT = reinterpret_cast<PIMAGE_THUNK_DATA>(
        reinterpret_cast<PBYTE>(hTargetModule) + importDesc->FirstThunk);
    if (!importDesc->OriginalFirstThunk)
      thunkOrig = thunkIAT;

    for (; thunkOrig->u1.AddressOfData; ++thunkOrig, ++thunkIAT) {
      if (IMAGE_SNAP_BY_ORDINAL(thunkOrig->u1.Ordinal))
        continue;

      auto importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
          reinterpret_cast<PBYTE>(hTargetModule) + thunkOrig->u1.AddressOfData);
      if (strcmp(importByName->Name, functionName) == 0) {
        DWORD oldProtect = 0;
        if (!VirtualProtect(&thunkIAT->u1.Function, sizeof(ULONG_PTR),
                            PAGE_READWRITE, &oldProtect))
          return false;

        if (origFunc && !*origFunc)
          *origFunc = reinterpret_cast<void *>(thunkIAT->u1.Function);

        thunkIAT->u1.Function = reinterpret_cast<ULONG_PTR>(newFunc);
        VirtualProtect(&thunkIAT->u1.Function, sizeof(ULONG_PTR), oldProtect,
                       &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), &thunkIAT->u1.Function,
                              sizeof(ULONG_PTR));
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// Direct3D 12 Xbox Imports
// ============================================================================

struct DXGIX_PRESENTARRAY_PARAMETERS {
  BOOL Disable;
  BOOL UsePreviousBuffer;
  D3D12_RECT SourceRect;
  POINT DestRectUpperLeft;
  FLOAT ScaleFactorVert;
  FLOAT ScaleFactorHorz;
  void *Cookie;
  UINT Flags;
};

// ---------------------------------------------------------------------------
// Game heap arena.
// The game stores pointers as 32-bit *relative offsets* (e.g. exe+0x18BEBB:
// `[obj+0x1C] = (int32)(owner - obj)`, undone by `owner = obj + (int32)[obj+0x1C]`
// in exe+0x18D6A9). That only works if all game heap objects live within +-2 GB
// of each other (true on the Xbox, false with the CRT heap on desktop, where
// objects end up in unrelated segments -> garbage pointers/AVs). The game's
// allocators never free (all free slots map to the Alloc stub), so a simple
// zero-initialised bump arena inside one < 2 GB reservation is enough.
// ---------------------------------------------------------------------------
// Logs DRED breadcrumbs (unfinished command lists, ops around the last completed one) and the
// GPU page fault, once per process. Needs ONRUSH_DRED=1 before device creation.
static void LogDredOnce(ID3D12Device *pDev) {
  static volatile LONG s_done = 0;
  if (!pDev || InterlockedExchange(&s_done, 1)) return;
  ID3D12DeviceRemovedExtendedData *pD = nullptr;
  if (FAILED(pDev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), (void **)&pD)) || !pD) {
    log_line("[dred] not available (set ONRUSH_DRED=1)\r\n");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
  if (SUCCEEDED(pD->GetAutoBreadcrumbsOutput(&bc))) {
    int nodes = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE *n = bc.pHeadAutoBreadcrumbNode; n && nodes < 64; n = n->pNext, ++nodes) {
      UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      if (done >= n->BreadcrumbCount) continue;  // finished list
      log_line("[dred] list %p '%S' queue '%S': %u/%u ops done\r\n", n->pCommandList,
               n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"", n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"",
               done, n->BreadcrumbCount);
      UINT from = done > 12 ? done - 12 : 0, to = done + 4 < n->BreadcrumbCount ? done + 4 : n->BreadcrumbCount;
      for (UINT i = from; i < to; ++i)
        log_line("[dred]   %s op[%u] = %d\r\n", i == done ? "->" : "  ", i, (int)n->pCommandHistory[i]);
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
  if (SUCCEEDED(pD->GetPageFaultAllocationOutput(&pf))) {
    log_line("[dred] page fault VA=0x%llX\r\n", (unsigned long long)pf.PageFaultVA);
    int k = 0;
    for (const D3D12_DRED_ALLOCATION_NODE *a = pf.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, ++k)
      log_line("[dred]   existing alloc type=%d '%S'\r\n", (int)a->AllocationType, a->ObjectNameW ? a->ObjectNameW : L"");
    k = 0;
    for (const D3D12_DRED_ALLOCATION_NODE *a = pf.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, ++k)
      log_line("[dred]   freed alloc type=%d '%S'\r\n", (int)a->AllocationType, a->ObjectNameW ? a->ObjectNameW : L"");
  }
  pD->Release();
}

static SRWLOCK g_arenaLock = SRWLOCK_INIT;
static uint8_t *g_arenaBase = nullptr;
static size_t g_arenaSize = 0x7A000000;      // ~1.9 GB reservation
static size_t g_arenaUsed = 0;
static size_t g_arenaCommitted = 0;

static uint64_t Fnv1a64Fwd(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data; uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}

static void *ArenaAlloc(size_t size, size_t align = 16) {
  if (size == 0) size = 16;
  if (align < 16) align = 16;
  AcquireSRWLockExclusive(&g_arenaLock);
  if (!g_arenaBase) {
    g_arenaBase = (uint8_t *)VirtualAlloc(nullptr, g_arenaSize, MEM_RESERVE, PAGE_READWRITE);
  }
  void *result = nullptr;
  if (g_arenaBase) {
    size_t off = (g_arenaUsed + (align - 1)) & ~(align - 1);
    size_t end = off + size;
    if (end <= g_arenaSize) {
      if (end > g_arenaCommitted) {
        size_t newCommit = (end + 0xFFFFF) & ~(size_t)0xFFFFF;  // 1 MB steps
        if (newCommit > g_arenaSize) newCommit = g_arenaSize;
        if (VirtualAlloc(g_arenaBase + g_arenaCommitted, newCommit - g_arenaCommitted,
                         MEM_COMMIT, PAGE_READWRITE)) {
          g_arenaCommitted = newCommit;
        }
      }
      if (end <= g_arenaCommitted) {
        result = g_arenaBase + off;
        g_arenaUsed = end;
      }
    }
  }
  ReleaseSRWLockExclusive(&g_arenaLock);
  if (!result) {
    static volatile LONG s_warned = 0;
    if (InterlockedIncrement(&s_warned) <= 3)
      log_line("[arena] WARNING: arena exhausted (size=%zu); falling back to CRT heap\r\n", size);
    result = calloc(1, size);
  }
  return result;
}

static const uint64_t kArenaBlockMagic = 0x4F4E525553484231ull;  // "ONRUSHB1"
static inline bool ArenaContains(const void *p) {
  uint8_t *b = g_arenaBase;
  return b && (const uint8_t *)p >= b && (const uint8_t *)p < b + g_arenaSize;
}

typedef void *(*AllocFn_t)(void *pThis, size_t size, void *stackInfo,
                           uint32_t align, void *extra);

static void *FallbackAllocMethod(void *pThis, size_t size, void *stackInfo,
                                 uint32_t align, void *extra) {
  (void)pThis;
  (void)stackInfo;
  (void)extra;
  // Each block carries a 16-byte header {MAGIC ^ size, size} so realloc can find its size
  // without a table. The old fixed-size AllocTracker (128K entries, never freed) filled up
  // while loading the tutorial; realloc then returned NULL, the engine's vector grow
  // (exe+0x7D020) kept capacity 0x80 and push_back wrote past the block (crashes at
  // exe+0x928020 / 0x19E6CE / 0x117C91).
  uint8_t *raw = (uint8_t *)ArenaAlloc(size + 16);
  void *ptr = nullptr;
  if (raw) {
    ((uint64_t *)raw)[0] = kArenaBlockMagic ^ (uint64_t)size;
    ((uint64_t *)raw)[1] = (uint64_t)size;
    ptr = raw + 16;
    if (!ArenaContains(ptr)) AllocTracker::Record(ptr, size);  // CRT-heap fallback blocks
  }
  log_line("[allocator] FallbackAlloc: size=%zu, align=%u -> %p\r\n", size,
           align, ptr);
  return ptr;
}

// Size of a block returned by FallbackAllocMethod (header first, tracker for CRT fallback).
static bool FallbackBlockSize(void *p, size_t *outSize) {
  if (ArenaContains(p) && (uint8_t *)p >= g_arenaBase + 16 && (uint8_t *)p <= g_arenaBase + g_arenaCommitted) {
    const uint64_t *h = (const uint64_t *)((uint8_t *)p - 16);
    if ((h[0] ^ h[1]) == kArenaBlockMagic) { *outSize = (size_t)h[1]; return true; }
  }
  return AllocTracker::Lookup(p, outSize);
}

typedef void *(__fastcall *PFN_ContextAllocatorReallocate)(void *pAllocator,
                                                           void *old_p,
                                                           size_t new_size);
static PFN_ContextAllocatorReallocate g_TrueContextAllocatorReallocate =
    nullptr;

static void *__fastcall HookedContextAllocatorReallocate(void *pAllocator,
                                                         void *old_p,
                                                         size_t new_size) {
  (void)pAllocator;

  if (new_size == 0) {
    log_line(
        "[realloc_hook] new_size=0, returning nullptr (old_p=%p preserved)\r\n",
        old_p);
    return nullptr;
  }

  if (!old_p) {
    void *new_p = FallbackAllocMethod(nullptr, new_size, nullptr, 0, nullptr);
    log_line("[realloc_hook] old_p=nullptr, new_size=%zu -> new_p=%p (initial "
             "allocation)\r\n",
             new_size, new_p);
    return new_p;
  }

  size_t tracked_old_size = 0;
  bool found = FallbackBlockSize(old_p, &tracked_old_size);
  if (!found) {
    // Block from another allocator: do what the original (exe+0x7CF60) does and copy
    // new_size bytes, clamped to the readable region. Returning NULL here makes the
    // engine's vector grow silently keep its old capacity -> heap overrun.
    MEMORY_BASIC_INFORMATION mbi = {};
    size_t avail = 0;
    if (VirtualQuery(old_p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
      avail = (size_t)((uint8_t *)mbi.BaseAddress + mbi.RegionSize - (uint8_t *)old_p);
    tracked_old_size = avail < new_size ? avail : new_size;
    static volatile LONG s_unknown = 0;
    if (InterlockedIncrement(&s_unknown) <= 5)
      log_line("[realloc] old_p=%p not a fallback block; copying %zu bytes\r\n", old_p, tracked_old_size);
  }

  void *new_p = FallbackAllocMethod(nullptr, new_size, nullptr, 0, nullptr);
  if (!new_p) {
    log_line("[realloc_hook] ERROR: Failed to allocate %zu bytes for realloc "
             "(old_p=%p preserved)!\r\n",
             new_size, old_p);
    return nullptr;
  }

  size_t copy_bytes =
      (tracked_old_size < new_size) ? tracked_old_size : new_size;
  if (copy_bytes > 0) {
    memcpy(new_p, old_p, copy_bytes);
  }
  if (new_size > copy_bytes) {
    memset((uint8_t *)new_p + copy_bytes, 0, new_size - copy_bytes);
  }
  log_line("[realloc_hook] old_p=%p (found=1, tracked_size=%zu), new_size=%zu "
           "-> new_p=%p, copied=%zu bytes\r\n",
           old_p, tracked_old_size, new_size, new_p, copy_bytes);

  // Preserve existing retention policy: do not free old_p
  return new_p;
}

static bool AllocatorContainsMethod(void *pThis, void *ptr) {
  (void)pThis;
  (void)ptr;
  return false;
}

static void *RootAllocMethod(void *pThis, size_t size, void *debug_info) {
  (void)pThis;
  (void)debug_info;
  void *ptr = ArenaAlloc(size);
  log_line("[root_allocator] RootAlloc: size=%zu -> %p\r\n", size, ptr);
  return ptr;
}

// Per-slot logging wrappers (first 6 calls of each slot) to learn which allocator methods the
// game really uses on g_FallbackAllocator and with which arguments.
template <int N>
static void *FallbackSlot(void *pThis, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
  static volatile LONG s_n = 0;
  if (InterlockedIncrement(&s_n) <= 6)
    log_line("[fallback_slot] slot %d (+0x%X) this=%p a=%llX b=%llX c=%llX d=%llX ret=exe+0x%llX\r\n", N, N * 8,
             pThis, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d,
             (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr)));
  return FallbackAllocMethod(pThis, (size_t)a, (void *)b, (uint32_t)c, (void *)d);
}

static void *g_RobustAllocatorVtable[32] = {
    (void *)&FallbackSlot<0>, // 0 (+0x00) - Alloc (BridgeAlloc, ustl string
                                  // alloc)
    (void *)&RootAllocMethod,     // 1 (+0x08) - Root Alloc
    // 2 (+0x10) - Realloc(this, old_p, new_size, flags): used by the engine's vector grow
    // (exe+0x7D0B2). It used to be FallbackAllocMethod(this, size=old_p) -> 0-byte blocks and
    // heap overruns (crash at exe+0x117C91 while building the main-menu phase).
    (void *)&HookedContextAllocatorReallocate,
    (void *)&FallbackSlot<3>, // 3 (+0x18)
    (void *)&FallbackSlot<4>, // 4 (+0x20)
    (void *)&FallbackSlot<5>, // 5 (+0x28)
    (void *)&FallbackSlot<6>, // 6 (+0x30)
    (void *)&FallbackSlot<7>, // 7 (+0x38)
    (void *)&FallbackSlot<8>, // 8 (+0x40)
    (void *)&FallbackSlot<9>, // 9 (+0x48)
    (void *)&FallbackSlot<10>, // 10 (+0x50)
    (void *)&AllocatorContainsMethod, // 11 (+0x58) - Contains(ptr)
    (void *)&FallbackSlot<12>,     // 12 (+0x60)
    (void *)&FallbackSlot<13>,     // 13 (+0x68)
    (void *)&FallbackSlot<14>,     // 14 (+0x70)
    (void *)&FallbackSlot<15>,     // 15 (+0x78)
    (void *)&FallbackSlot<16>,     // 16 (+0x80)
    (void *)&AllocatorContainsMethod, // 17 (+0x88) - Contains(ptr)
    (void *)&FallbackSlot<18>,     // 18
    (void *)&FallbackSlot<19>,     // 19
    (void *)&FallbackSlot<20>,     // 20
    (void *)&FallbackSlot<21>,     // 21
    (void *)&FallbackSlot<22>,     // 22
    (void *)&FallbackSlot<23>,     // 23
    (void *)&FallbackSlot<24>,     // 24
    (void *)&FallbackSlot<25>,     // 25
    (void *)&FallbackSlot<26>,     // 26
    (void *)&FallbackSlot<27>,     // 27
    (void *)&FallbackSlot<28>,     // 28
    (void *)&FallbackSlot<29>,     // 29
    (void *)&FallbackSlot<30>,     // 30
    (void *)&FallbackSlot<31>,     // 31
};

static struct {
  void **vtable;
} g_FallbackAllocator = {g_RobustAllocatorVtable};

static struct {
  void **vtable;
} g_FallbackRootAllocator = {g_RobustAllocatorVtable};

static void *BridgeAlloc(size_t size, void *allocator, uint64_t flags,
                         const char *file, int line) {
  void *caller = _ReturnAddress();
  if (size == 0)
    size = 16;
  // Durango's allocator hands out zeroed memory; game objects rely on it.
  void *ptr = ArenaAlloc(size);
  if (g_inNativeActivationInit) {
    LONG idx = InterlockedIncrement(&g_nativeActivationAllocCount);
    HMODULE hExe = GetModuleHandleW(nullptr);
    uintptr_t callerRva = (hExe && (uintptr_t)caller >= (uintptr_t)hExe)
                              ? ((uintptr_t)caller - (uintptr_t)hExe)
                              : 0;
    log_line(
        "[BridgeAlloc][NativeInit #%ld] Alloc %zu bytes (alloc=%p, "
        "flags=0x%llX, file=%s, line=%d, caller=%p [exe+0x%llX]) -> %p\r\n",
        (long)idx, size, allocator, flags, file ? file : "null", line, caller,
        (unsigned long long)callerRva, ptr);
  } else {
    log_line("[BridgeAlloc] Alloc %zu bytes (alloc=%p, flags=0x%llX, file=%s, "
             "line=%d) -> %p\r\n",
             size, allocator, flags, file ? file : "null", line, ptr);
  }
  return ptr;
}

static struct TrackerList {
  void *pad;
  void *start;
  size_t count;
} g_EmptyTrackerList = {nullptr, nullptr, 0};

// Validated allocator globals initialized by game+0x21DC90 (called from
// OnActivated at game+0x21B500):
// - [game+0x0167EF58]: Main flow loHeapAllocator* (0x2E0 bytes, constructed by
// game+0x069F90(0), written at game+0x21DCC3)
// - [allocator+0xC8] : Sub-pool descriptor table pointer inside the 0x2E0-byte
// loHeapAllocator (expected: game+0x015227C0, read at game+0x0670DA)
// - [game+0x01524490]: Root heap allocator* (constructed by game+0x069300
// inside game+0x06A960, written at game+0x06A9A6)
// - [game+0x01524488]: Main/string heap allocator* (0x3A8 bytes, constructed by
// game+0x069410+0x06AC90 inside game+0x06A960, written at game+0x06AA4C)
// - [game+0x01537B90]: Thread sub-pool 8 loHeapAllocator* (0x90 bytes,
// constructed by game+0x069300 inside game+0x098DB0, written at game+0x098E6A)
// - [game+0x01529128]: Command-line/config registry* (0x30 bytes, constructed
// by game+0x07C990 inside game+0x07D6C0, written at game+0x07D70A)
static void LogAllocatorStateSnapshot(const char *phase, HMODULE hExe) {
  if (!hExe) {
    log_line("[bridge][AllocatorSnapshot:%s] hExe is null!\r\n",
             phase ? phase : "?");
    return;
  }

  uint8_t *base = reinterpret_cast<uint8_t *>(hExe);
  void *flowAlloc = *reinterpret_cast<void **>(base + 0x0167EF58);
  void *rootAlloc = *reinterpret_cast<void **>(base + 0x01524490);
  void *mainAlloc = *reinterpret_cast<void **>(base + 0x01524488);
  void *threadAlloc = *reinterpret_cast<void **>(base + 0x01537B90);
  void *cmdRegistry = *reinterpret_cast<void **>(base + 0x01529128);

  log_line("[bridge][AllocatorSnapshot:%s] [game+0x167EF58] (flowAlloc)   = "
           "%p%s\r\n",
           phase, flowAlloc,
           (flowAlloc == &g_FallbackAllocator)
               ? " (g_FallbackAllocator, 8 bytes)"
               : "");

  if (flowAlloc == nullptr || flowAlloc == &g_FallbackAllocator ||
      flowAlloc == &g_FallbackRootAllocator) {
    log_line(
        "[bridge][AllocatorSnapshot:%s] [allocator+0xC8] (subPoolTbl)  = "
        "<skipped: allocator is %s, not 0x2E0-byte native loHeapAllocator>\r\n",
        phase, flowAlloc ? "8-byte fallback" : "null");
  } else {
    void *subPoolTable = nullptr;
    bool readOk = false;
    __try {
      subPoolTable = *reinterpret_cast<void **>(
          reinterpret_cast<uint8_t *>(flowAlloc) + 0xC8);
      readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      readOk = false;
    }
    if (readOk) {
      uintptr_t tblRva = ((uintptr_t)subPoolTable >= (uintptr_t)base)
                             ? ((uintptr_t)subPoolTable - (uintptr_t)base)
                             : 0;
      log_line("[bridge][AllocatorSnapshot:%s] [allocator+0xC8] (subPoolTbl)  "
               "= %p (exe+0x%llX)\r\n",
               phase, subPoolTable, (unsigned long long)tblRva);
    } else {
      log_line("[bridge][AllocatorSnapshot:%s] [allocator+0xC8] (subPoolTbl)  "
               "= <AV reading %p+0xC8>\r\n",
               phase, flowAlloc);
    }
  }

  log_line("[bridge][AllocatorSnapshot:%s] [game+0x1524490] (rootAlloc)   = "
           "%p%s\r\n",
           phase, rootAlloc,
           (rootAlloc == &g_FallbackRootAllocator)
               ? " (g_FallbackRootAllocator)"
               : "");
  log_line("[bridge][AllocatorSnapshot:%s] [game+0x1524488] (mainAlloc)   = "
           "%p%s\r\n",
           phase, mainAlloc,
           (mainAlloc == &g_FallbackAllocator) ? " (g_FallbackAllocator)" : "");
  log_line(
      "[bridge][AllocatorSnapshot:%s] [game+0x1537B90] (threadAlloc) = %p\r\n",
      phase, threadAlloc);
  log_line("[bridge][AllocatorSnapshot:%s] [game+0x1529128] (cmdRegistry) = "
           "%p%s\r\n",
           phase, cmdRegistry,
           (cmdRegistry == &g_EmptyTrackerList) ? " (g_EmptyTrackerList)" : "");
}

static bool Install14ByteDetour(void *targetAddr, void *hookFn) {
  uint8_t *pFunc = (uint8_t *)targetAddr;
  DWORD oldProt = 0;
  if (!VirtualProtect(pFunc, 14, PAGE_EXECUTE_READWRITE, &oldProt))
    return false;

  pFunc[0] = 0xFF;
  pFunc[1] = 0x25;
  pFunc[2] = 0x00;
  pFunc[3] = 0x00;
  pFunc[4] = 0x00;
  pFunc[5] = 0x00;
  uint64_t dest = (uint64_t)hookFn;
  memcpy(pFunc + 6, &dest, 8);
  VirtualProtect(pFunc, 14, oldProt, &oldProt);
  FlushInstructionCache(GetCurrentProcess(), pFunc, 14);
  return true;
}

// Validate the exact game instruction before changing it. The supplied log
// ends at game_release_xbox.exe+0xB6C42, matching the patch site at RVA
// 0xB6C2A.
static bool g_bufferPointerPatchReady = false;

static bool PatchVerifiedR8ToRcx(HMODULE hExe, uintptr_t rva,
                                 const char *label) {
  if (!hExe)
    return false;

  uint8_t *site = reinterpret_cast<uint8_t *>(hExe) + rva;
  DWORD oldProtect = 0;
  if (!VirtualProtect(site, 3, PAGE_EXECUTE_READWRITE, &oldProtect)) {
    DWORD error = GetLastError();
    log_line("[d3d12_x] FATAL: VirtualProtect failed for %s at RVA 0x%08llX "
             "(Win32=%lu)\r\n",
             label, (unsigned long long)rva, error);
    return false;
  }

  const uint8_t original[3] = {0x41, 0x8B, 0xC8};    // mov ecx, r8d (truncates)
  const uint8_t replacement[3] = {0x49, 0x8B, 0xC8}; // mov rcx, r8 (64-bit)
  bool ready = false;

  if (memcmp(site, original, sizeof(original)) == 0) {
    memcpy(site, replacement, sizeof(replacement));
    if (FlushInstructionCache(GetCurrentProcess(), site, sizeof(replacement))) {
      ready = true;
      log_line("[d3d12_x] Applied %s at RVA 0x%08llX: 41 8B C8 -> 49 8B C8\r\n",
               label, (unsigned long long)rva);
    } else {
      log_line("[d3d12_x] FATAL: FlushInstructionCache failed for %s at RVA "
               "0x%08llX (Win32=%lu)\r\n",
               label, (unsigned long long)rva, GetLastError());
    }
  } else if (memcmp(site, replacement, sizeof(replacement)) == 0) {
    ready = true;
    log_line("[d3d12_x] %s at RVA 0x%08llX is already patched (49 8B C8)\r\n",
             label, (unsigned long long)rva);
  } else {
    log_line("[d3d12_x] FATAL: signature mismatch for %s at RVA 0x%08llX; "
             "found %02X %02X %02X; refusing unsafe patch\r\n",
             label, (unsigned long long)rva, site[0], site[1], site[2]);
  }

  DWORD ignored = 0;
  if (!VirtualProtect(site, 3, oldProtect, &ignored)) {
    log_line("[d3d12_x] FATAL: could not restore page protection for %s at RVA "
             "0x%08llX (Win32=%lu)\r\n",
             label, (unsigned long long)rva, GetLastError());
    ready = false;
  }
  return ready;
}

static void DetourPlayGoInit(void *pPlayGo) {
  log_line("[bridge] DetourPlayGoInit: PlayGo=%p (marking package 100%% "
           "complete)\r\n",
           pPlayGo);
  if (pPlayGo) {
    *(float *)((char *)pPlayGo + 8) =
        1.0f; // m_progress = 1.0f (100% installed)
    *(uint32_t *)((char *)pPlayGo + 0x0C) = 1; // m_state = 1
    *(uint8_t *)((char *)pPlayGo + 0x10) = 0;  // m_pending = 0
  }
  // NOTE: [exe+0x1538940] is NOT a PlayGo chunk bitmap: it is the loPlatformEvent flag set
  // (bit 0 = eSessionInviteAccepted, bit 2 = eResumedFromSuspend, ...). Filling it with 1s
  // faked a multiplayer invite, which made IntroVideoPhase skip the intro video at once.
}

static int DetourPlayGoGetChunkStatus(void *pPlayGo, int chunkId) {
  log_line("[bridge] DetourPlayGoGetChunkStatus: PlayGo=%p, chunkId=%d -> "
           "returning 2 (INSTALLED)\r\n",
           pPlayGo, chunkId);
  return 2; // 2 = Installed
}

static void *DetourPackageTransferManager() {
  log_line("[bridge] DetourPackageTransferManager -> returning mock\r\n");
  return &g_mockUniversalObject;
}

static void *DetourThreadAlloc(size_t size, uint64_t flags) {
  size_t allocSize = (size + 4095) & ~4095;
  if (allocSize == 0)
    allocSize = 4096;
  void *ptr = VirtualAlloc(nullptr, allocSize, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
  log_line("[threading] DetourThreadAlloc: size=%zu (allocSize=%zu, "
           "flags=0x%llX) -> %p\r\n",
           size, allocSize, (unsigned long long)flags, ptr);
  return ptr;
}

static void DetourThreadFree(void *ptr) {
  log_line("[threading] DetourThreadFree: ptr=%p\r\n", ptr);
  if (!ptr)
    return;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) && mbi.AllocationBase == ptr) {
    VirtualFree(ptr, 0, MEM_RELEASE);
  } else {
    free(ptr);
  }
}

// ============================================================================
// Input & Phase Transition Detours (Title Screen & Gamepad Pairing)
// ============================================================================
static bool STDMETHODCALLTYPE DetourPlatformInputCheckButton(void *pThis) {
  HMODULE hExe = GetModuleHandleW(nullptr);

  // 1. Ensure active gamepad is populated in PlatformInputXbox
  if (pThis) {
    void **ppActive = (void **)((uint8_t *)pThis + 0x48);
    if (*ppActive == nullptr) {
      *ppActive = (void *)(IGamepad *)&g_mockGamepad;
      ((IGamepad *)&g_mockGamepad)->AddRef();
      log_line("[input] DetourPlatformInputCheckButton: Populated [PlatformInputXbox+0x48] with mock gamepad (%p)\r\n", *ppActive);
    }
  }

  // 2. Keyboard / XInput gamepad ("press any button")
  bool pressed = IsAdvanceInputHeld();
  if (pressed) {
    static volatile LONG s_pressLog = 0;
    if (InterlockedIncrement(&s_pressLog) <= 20)
      log_line("[input] DetourPlatformInputCheckButton: button press detected\r\n");
  }

  // 3. Debug-only auto-pulse (OFF by default; ONRUSH_AUTOPULSE_A=1 enables it)
  static const bool s_autoPulseEnabled = InputEnvFlag("ONRUSH_AUTOPULSE_A");
  static UINT32 s_autoPulse = 0;
  if (s_autoPulseEnabled) {
    s_autoPulse++;
    if (s_autoPulse > 180 && (s_autoPulse % 60) < 20) {
      pressed = true;
      if (s_autoPulse % 60 == 1) {
        log_line("[input] DetourPlatformInputCheckButton: Auto-pulsing TRUE at count %u\r\n", s_autoPulse);
      }
    }
  }

  // Old forced pacing only (ONRUSH_FORCE_PHASE=1). In the natural flow these writes broke race loading:
  // [0x167E8C8]=1 marked a still-loading phase ready (crash exe+0x17D532, null object), and 0x167E8C7 is
  // not a flag but the top byte of the pointer at exe+0x167E8C0 (crash exe+0x21B9E2 reading 0x0100xxxx...).
  static const bool s_forceFlags = InputEnvFlag("ONRUSH_FORCE_PHASE");
  if (pressed && s_forceFlags) {
    if (hExe) {
      void *nextPhase = *(void **)((uint8_t *)hExe + 0x0167E940);
      if (nextPhase != nullptr) {
        uint8_t *pTransitionFlag0 = (uint8_t *)hExe + 0x0167E948;
        uint8_t *pTransitionFlag1 = (uint8_t *)hExe + 0x0167E949;
        uint8_t *pPhaseReady1     = (uint8_t *)hExe + 0x0167E8C8;
        *pTransitionFlag0 = 1;
        *pTransitionFlag1 = 1;
        *pPhaseReady1 = 1;
        log_line("[input] DetourPlatformInputCheckButton: Set [0x0167E948]=1, [0x0167E949]=1 and [0x0167E8C8]=1 (nextPhase=%p)\r\n", nextPhase);
      }
    }
    return true;
  }

  return false;
}

static bool STDMETHODCALLTYPE DetourStartScreenCheckInput(void *pPhase) {
  HMODULE hExe = GetModuleHandleW(nullptr);
  void *pPlatformInput = nullptr;
  if (hExe) {
    pPlatformInput = *(void **)((uint8_t *)hExe + 0x01543480);
  }
  static volatile LONG s_startCheckCount = 0;
  if (InterlockedIncrement(&s_startCheckCount) <= 20) {
    log_line("[input] DetourStartScreenCheckInput: pPhase=%p, pPlatformInput=%p\r\n", pPhase, pPlatformInput);
  }
  return DetourPlatformInputCheckButton(pPlatformInput);
}

// ============================================================================
// Page Element Cleanup Null Guard Trampoline (RVA 0x001E3830)
// ============================================================================
typedef void (*PFN_PageElementCleanup)(void *rcx);
static PFN_PageElementCleanup g_TruePageElementCleanup = nullptr;
static uint8_t g_PageElementCleanupTrampoline[64] = {0};

static void DetourPageElementCleanup(void *rcx) {
  if (!rcx) {
    // Guard against NULL element pointer (would crash at [rcx + 0x600])
    return;
  }
  if (g_TruePageElementCleanup) {
    g_TruePageElementCleanup(rcx);
  }
}


static void DetourStartVideo(void *pPlayer, const char *filename) {
  HMODULE hExe = GetModuleHandleW(nullptr);
  log_line("[video] StartVideo called: pPlayer=%p, filename='%s'\r\n", pPlayer,
           filename ? filename : "<null>");
  if (!pPlayer || !filename)
    return;
  // Like the original (exe+0x23FBF6): only start when the player is idle (state 0).
  // The 1.80 intro calls StartVideo twice on the same player; reopening Bink over
  // the live handle crashed at exe+0x6B31D.
  int32_t *state = (int32_t *)((uint8_t *)pPlayer + 0x178);
  if (*state != 0) {
    log_line("[video] StartVideo ignored: player busy (state=%d)\r\n", *state);
    return;
  }
  *state = 1;

  // 1. Build the path like the original (exe+0x23FBF0): snprintf(player+0xF0, 0x7F, "%s%s",
  //    [exe+0x1418778] ("art:videos/"), filename). Without the prefix the game's Bink file
  //    callbacks cannot find the .bk2 in the dat archives and BinkOpen returns NULL.
  char *dest = (char *)pPlayer + 0xf0;
  {
    const char *prefix = *(const char **)((uint8_t *)hExe + 0x01418778);
    if (prefix && (uintptr_t)prefix > 0x10000) {
      _snprintf_s(dest, 0x7f, _TRUNCATE, "%s%s", prefix, filename);
    } else {
      strncpy_s(dest, 0x7f, filename, _TRUNCATE);
    }
  }

  // 2. Execute 0x14022B180 (VideoOpenWorker) synchronously
  typedef void (*PFN_VideoOpenWorker)(void *pPlayer);
  PFN_VideoOpenWorker openWorker =
      (PFN_VideoOpenWorker)((uint8_t *)hExe + 0x0023FC80);
  log_line("[video] Running VideoOpenWorker (0x0023FC80) synchronously for '%s'...\r\n",
           dest);
  openWorker(pPlayer);
  // 3. The job's completion (exe+0x23FCC0): state 2 + playback setup if Bink opened, else state 0.
  typedef void (*PFN_VideoOpenDone)(void *pPlayer);
  ((PFN_VideoOpenDone)((uint8_t *)hExe + 0x0023FCC0))(pPlayer);
  log_line("[video] VideoOpenWorker finished: state=%d, hBink=%p\r\n",
           *(int32_t *)((uint8_t *)pPlayer + 0x178),
           *(void **)((uint8_t *)pPlayer + 0x170));
}

static void *AllocateNearAddress(void *targetAddr, size_t size) {
  uintptr_t target = (uintptr_t)targetAddr;
  uintptr_t minAddr =
      (target > 0x70000000ULL) ? (target - 0x70000000ULL) : 0x10000ULL;
  uintptr_t maxAddr = target + 0x70000000ULL;

  for (uintptr_t addr = (target + 0x100000ULL) & ~0xFFFFULL; addr < maxAddr;
       addr += 0x10000ULL) {
    void *p = VirtualAlloc((void *)addr, size, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (p)
      return p;
  }
  for (uintptr_t addr = (target - 0x10000ULL) & ~0xFFFFULL; addr > minAddr;
       addr -= 0x10000ULL) {
    void *p = VirtualAlloc((void *)addr, size, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (p)
      return p;
  }
  return nullptr;
}

// Canonical user-mode pointer (rejects NULL, -1 and recycled heap links such as
// 0x01000249B1EE2DA0, which fault as non-canonical addresses).
static inline bool PlausiblePtr(const void *p) {
  uintptr_t v = (uintptr_t)p;
  return v > 0x10000 && v < 0x00007FFFFFFE0000ULL;
}

static bool MaterializePendingPhase(HMODULE hExe, bool force = false) {
  if (!hExe) return false;
  void ***ppTargetNextPhase = (void ***)((uint8_t *)hExe + 0x0167E8B8);
  if (!ppTargetNextPhase) return false;
  void **pTargetNextPhase = *ppTargetNextPhase;
  if (!pTargetNextPhase) return false;

  // If already materialized (nextPhase non-null), nothing pending
  // (the job-side worker passes force=true: the slot still holds the current phase after a
  //  transition, and the original worker exe+0x21B9B0 always overwrites it)
  if (*pTargetNextPhase != nullptr &&
      !(force && *pTargetNextPhase == *(void **)((uint8_t *)hExe + 0x0167E930))) return false;

  void *configDescObj = *(void **)((uint8_t *)hExe + 0x0167E8B0);
  void *phaseAllocObj = *(void **)((uint8_t *)hExe + 0x0167E8C0);
  // The game parks these globals at -1 (not just NULL) while a phase is being torn down.
  if (!PlausiblePtr(configDescObj) || !PlausiblePtr(phaseAllocObj)) return false;

  void **configVtbl = *(void ***)configDescObj;
  typedef void *(*ConfigCreatePhaseFn)(void *self, void *allocObj);
  ConfigCreatePhaseFn CreatePhase = (ConfigCreatePhaseFn)configVtbl[2];
  void *newPhaseInstance = CreatePhase(configDescObj, phaseAllocObj);

  *pTargetNextPhase = newPhaseInstance; // Sets g_NextPhase!

  if (newPhaseInstance) {
    void **phaseVtbl = *(void ***)newPhaseInstance;
    typedef void (*PhaseVtbl1Fn)(void *self);
    PhaseVtbl1Fn fn1 = (PhaseVtbl1Fn)phaseVtbl[1];
    fn1(newPhaseInstance);
  }

  // OnEnter may tear down / replace the allocator object; re-read and re-validate it.
  phaseAllocObj = *(void *volatile *)((uint8_t *)hExe + 0x0167E8C0);
  if (!PlausiblePtr(phaseAllocObj)) return true;
  {
    // A live allocator object has its vtable inside the exe image; a recycled one
    // contains heap-block links (e.g. 0x01000249B1EE2DA0) -> skip the release call.
    uintptr_t vt = *(uintptr_t *)phaseAllocObj;
    uintptr_t lo = (uintptr_t)hExe;
    if (vt < lo || vt >= lo + 0x1A00000) return true;
  }
  void **allocVtbl = *(void ***)phaseAllocObj;
  typedef void (*AllocVtbl4Fn)(void *self);
  AllocVtbl4Fn fn4 = (AllocVtbl4Fn)allocVtbl[4];
  fn4(phaseAllocObj);

  log_line("[phase] MaterializePendingPhase: Successfully materialized new phase instance %p (target=%p, config=%p)\r\n",
           newPhaseInstance, pTargetNextPhase, configDescObj);
  return true;
}

static void DetourPhaseWorker() {
  HMODULE hExe = GetModuleHandleW(nullptr);
  if (!MaterializePendingPhase(hExe, InputEnvFlag("ONRUSH_FORCE_WORKER")) && hExe) {
    void **pp = *(void ***)((uint8_t *)hExe + 0x0167E8B8);
    log_line("[phase] DetourPhaseWorker: materialize skipped (target=%p *target=%p config=%p alloc=%p)\r\n",
             (void *)pp, pp ? *pp : nullptr, *(void **)((uint8_t *)hExe + 0x0167E8B0),
             *(void **)((uint8_t *)hExe + 0x0167E8C0));
  }
  // The original worker (exe+0x21B9B0) ends with a tail call to exe+0x16A670; keep it
  // (ONRUSH_NO_PHASE_TAIL=1 skips it, the old behaviour).
  static const bool s_noTail = InputEnvFlag("ONRUSH_NO_PHASE_TAIL");
  if (!s_noTail && hExe) {
    log_line("[phase] DetourPhaseWorker: calling original tail exe+0x16A670\r\n");
    ((void (*)())((uint8_t *)hExe + 0x0016A670))();
  }
}

static void PatchPresentationHooks(HMODULE hExe) {
  if (!hExe)
    return;

  // 0. Patch RVA 0x0021B9B0 (Phase Constructor Worker): FF 25 00 00 00 00 [DetourPhaseWorker]
  {
    uint8_t *pWorker = (uint8_t *)hExe + 0x0021B9B0;
    DWORD oldProtWorker = 0;
    // ONRUSH_ORIG_PHASE_WORKER=1: keep the game's own phase worker (and do not build phases
    // from Present); meant to be used with ONRUSH_REAL_JOBBUSY=1.
    if (InputEnvFlag("ONRUSH_ORIG_PHASE_WORKER")) {
      log_line("[d3d12_x] ONRUSH_ORIG_PHASE_WORKER: keeping original 0x0021B9B0\r\n");
    } else if (VirtualProtect(pWorker, 14, PAGE_EXECUTE_READWRITE, &oldProtWorker)) {
      pWorker[0] = 0xFF;
      pWorker[1] = 0x25;
      *(uint32_t *)(pWorker + 2) = 0;
      *(uint64_t *)(pWorker + 6) = (uint64_t)&DetourPhaseWorker;
      VirtualProtect(pWorker, 14, oldProtWorker, &oldProtWorker);
      log_line("[d3d12_x] Patched 0x0021B9B0 with DetourPhaseWorker -> %p\r\n", &DetourPhaseWorker);
    }
  }

  // 0a. Patch RVA 0x00339FD0 (MusicAudioUpdate): ret (C3). Only with ONRUSH_NO_AUDIO=1 (audio is on by default since 2026-11-08).
  if (GetEnvironmentVariableA("ONRUSH_NO_AUDIO", nullptr, 0) != 0) {
    uint8_t *pMusicUpdate = (uint8_t *)hExe + 0x00339FD0;
    DWORD oldProtMusic = 0;
    if (VirtualProtect(pMusicUpdate, 1, PAGE_EXECUTE_READWRITE, &oldProtMusic)) {
      *pMusicUpdate = 0xC3;
      VirtualProtect(pMusicUpdate, 1, oldProtMusic, &oldProtMusic);
      log_line("[d3d12_x] Patched 0x00339FD0 (MusicAudioUpdate) with ret (0xC3)\r\n");
    }
  }

  // 0b. Patch RVA 0x0021D215: 0F 85 ED 00 00 00 (jne exit on JobQueue pending) -> 90 90 90 90 90 90
  {
    uint8_t *pJobQueueWait = (uint8_t *)hExe + 0x0021D215;
    DWORD oldProtJobWait = 0;
    if (VirtualProtect(pJobQueueWait, 6, PAGE_EXECUTE_READWRITE, &oldProtJobWait)) {
      if (pJobQueueWait[0] == 0x0F && pJobQueueWait[1] == 0x85) {
        memset(pJobQueueWait, 0x90, 6);
        VirtualProtect(pJobQueueWait, 6, oldProtJobWait, &oldProtJobWait);
        log_line("[d3d12_x] Patched 0x0021D215 (PhaseTransition JobQueue wait) with NOPs\r\n");
      } else {
        VirtualProtect(pJobQueueWait, 6, oldProtJobWait, &oldProtJobWait);
      }
    }
  }

  // 1. Patch RVA 0x000B2EE1: FF 90 80 00 00 00 (call qword ptr [rax+80h]) -> FF 15 [disp32]
  // This redirects CreateSwapChainForCoreWindow directly to HookedCreateSwapChainForCoreWindow,
  // ensuring the flip-model desktop swapchain is properly initialized and captured into g_pMainSwapChain.
  uint8_t *pCallSite = (uint8_t *)hExe + 0x000B2EE1;
  DWORD oldProt = 0;
  if (VirtualProtect(pCallSite, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
    if (pCallSite[0] == 0xFF && pCallSite[1] == 0x90 && pCallSite[2] == 0x80) {
      void *pSlot = AllocateNearAddress(pCallSite, sizeof(void *));
      if (pSlot) {
        *(void **)pSlot = (void *)&HookedCreateSwapChainForCoreWindow;
        int64_t diff = (int64_t)((uint8_t *)pSlot - (pCallSite + 6));
        if (diff >= INT32_MIN && diff <= INT32_MAX) {
          int32_t disp = (int32_t)diff;
          pCallSite[0] = 0xFF;
          pCallSite[1] = 0x15;
          memcpy(pCallSite + 2, &disp, 4);
          FlushInstructionCache(GetCurrentProcess(), pCallSite, 6);
          log_line("[d3d12_x] Patched 0x000B2EE1: CreateSwapChainForCoreWindow -> "
                   "HookedCreateSwapChainForCoreWindow (disp=0x%X, slot=%p)\r\n",
                   disp, pSlot);
        } else {
          log_line("[d3d12_x] ERROR: Near slot displacement 0x%llX exceeds 32 bits!\r\n",
                   diff);
        }
      } else {
        log_line("[d3d12_x] ERROR: Failed to allocate near slot for 0x000B2EE1 hook!\r\n");
      }
    } else {
      log_line("[d3d12_x] WARNING: 0x000B2EE1 unexpected bytes (%02X %02X %02X)\r\n",
               pCallSite[0], pCallSite[1], pCallSite[2]);
    }
    VirtualProtect(pCallSite, 6, oldProt, &oldProt);
  }

  // Note: RVA 0x000BEA30 is not a presentation function, but an internal descriptor
  // cache allocator for RTVs/DSVs (CreateDepthStencilView at [vtable+0xA8] / CreateRenderTargetView at [vtable+0xA0]).
  // The early exit check at 0x000BEA44 (test byte ptr [rcx+2Ch], 42h; jz 0xBAE84) must be preserved
  // to avoid underflowing the descriptor free-list (0x69cf0) on resources without RTV/DSV flags.
}

// The audio backend init (exe+0xA93230) submits a command to the Xbox ACP and
// then spins in `do { pump(0xA936F0) } while (this->pending [+0xB4] > 0)` until
// the ACP firmware acknowledges it. The port does not emulate the ACP, so the
// ctor of PersistentPhase (which also creates the UI selectors and starts the
// game's input polling) never returned. Make the loop run its pump once.
// Set ONRUSH_ACP_WAIT=1 to keep the original behaviour.
static void PatchAcpWait(HMODULE hExe) {
  char v[8] = {};
  if (GetEnvironmentVariableA("ONRUSH_ACP_WAIT", v, sizeof(v)) && v[0] == '1') {
    log_line("[acp] ACP ack wait left as-is (ONRUSH_ACP_WAIT=1)\r\n");
    return;
  }
  uint8_t *p = (uint8_t *)hExe + 0x00A936BF;  // ja 0xA936B0
  static const uint8_t expected[2] = {0x77, 0xEF};  // ja short 0xA936B0
  static const uint8_t nop2[2] = {0x90, 0x90};
  DWORD old = 0;
  if (memcmp(p, expected, 2) != 0) {
    log_line("[acp] WARNING: unexpected bytes at exe+0xA936BF (%02X %02X %02X %02X %02X %02X); not patched\r\n",
             p[0], p[1], p[2], p[3], p[4], p[5]);
    return;
  }
  if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
    memcpy(p, nop2, 2);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    VirtualProtect(p, 2, old, &old);
    log_line("[acp] Patched exe+0xA936BF: ACP ack wait loop disabled\r\n");
  }
}

// Wwise's WASAPI sink looks up its output device with
// IMMDeviceEnumerator::EnumAudioEndpoints(eRender, 0x1F, ...). Xbox accepts 0x1F,
// desktop Windows only accepts DEVICE_STATEMASK_ALL (0xF) and returns E_INVALIDARG,
// so the sink never found a device and the game ran silent. Use 0xF.
static void PatchWwiseDeviceStateMask(HMODULE hExe) {
  uint8_t *p = (uint8_t *)hExe + 0x00AA505D;  // lea r8d, [rdx+0x1F]
  static const uint8_t expected[4] = {0x44, 0x8D, 0x42, 0x1F};
  DWORD old = 0;
  if (memcmp(p, expected, 4) != 0) {
    log_line("[audio] WARNING: unexpected bytes at exe+0xAA505D; device state mask not patched\r\n");
    return;
  }
  if (VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) {
    p[3] = 0x0F;
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    VirtualProtect(p, 4, old, &old);
    log_line("[audio] Patched exe+0xAA505D: EnumAudioEndpoints state mask 0x1F -> 0xF\r\n");
  }
  // The sink init (exe+0xAA9FD0) calls two Xbox-only IAudioClient2 slots. On desktop
  // slot 0x98 is IAudioClient3::GetCurrentSharedModeEnginePeriod, which writes a
  // pointer over the sink's own callback vtable (the Xbox call registers that
  // callback), and slot 0x88 is GetBufferSizeLimits. That crashed the audio thread
  // when the second (music) output started. Drop both calls; the results are unused.
  static const uint32_t calls[2] = {0x00AAA11D, 0x00AAA147};
  static const uint8_t slots[2] = {0x98, 0x88};
  for (int i = 0; i < 2; ++i) {
    uint8_t *c = (uint8_t *)hExe + calls[i];
    const uint8_t want[6] = {0xFF, 0x90, slots[i], 0x00, 0x00, 0x00};
    if (memcmp(c, want, 6) != 0) {
      log_line("[audio] WARNING: unexpected bytes at exe+0x%X; not patched\r\n", calls[i]);
      continue;
    }
    if (VirtualProtect(c, 6, PAGE_EXECUTE_READWRITE, &old)) {
      static const uint8_t nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
      memcpy(c, nop6, 6);
      FlushInstructionCache(GetCurrentProcess(), c, 6);
      VirtualProtect(c, 6, old, &old);
    }
  }
  log_line("[audio] Patched Xbox-only IAudioClient2 calls at exe+0xAAA11D/0xAAA147\r\n");
}

// Launcher "Desbloquear cosméticos" (-unlockcosmetics or ONRUSH_UNLOCK_COSMETICS=1): like the game's
// own -unlockall, but only for the item checks: exe+0x3D7180 (outfits, tombstones, tricks, victory
// animations, vehicles) and exe+0x3D7360 (crashtags). Career events and class videos stay as saved.
// Each check starts with `if (HasOption("unlockall")) return true;` -> drop the je after it.
static void PatchUnlockCosmetics(HMODULE hExe) {
  const wchar_t *cl = GetCommandLineW();
  bool on = InputEnvFlag("ONRUSH_UNLOCK_COSMETICS") || (cl && wcsstr(cl, L"-unlockcosmetics"));
  if (!on) return;
  static const struct { uint32_t rva; uint8_t jeLen; } sites[2] = {{0x003D719F, 0x0E}, {0x003D7381, 0x0C}};
  for (const auto &s : sites) {
    uint8_t *p = (uint8_t *)hExe + s.rva;
    DWORD old = 0;
    if (p[-2] != 0x84 || p[-1] != 0xC0 || p[0] != 0x74 || p[1] != s.jeLen) {  // test al,al ; je
      log_line("[unlock] WARNING: unexpected bytes at exe+0x%X; not patched\r\n", s.rva);
      continue;
    }
    if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
      p[0] = 0x90; p[1] = 0x90;
      FlushInstructionCache(GetCurrentProcess(), p, 2);
      VirtualProtect(p, 2, old, &old);
      log_line("[unlock] cosmetics: item check at exe+0x%X always unlocked\r\n", s.rva);
    }
  }
}

// Wwise WASAPI sink init (exe+0xAA9FD0): this = sink, rdx = IMMDevice, r8 = output
// settings (+0x14 = output type; 2 = main, 3 = background music). The sink keeps its
// IAudioClient at +0x40. When the player's own playlist replaces the game music, the
// music output stream is silenced here (the effects/voice output is untouched).
typedef int (*PFN_WwiseSinkInit)(void *sink, void *device, void *settings);
static PFN_WwiseSinkInit g_TrueWwiseSinkInit = nullptr;
static uint8_t g_WwiseSinkInitTramp[64] = {};
static bool g_muteGameMusicOut = false;

#include "music_player.h"

// Wraps a sink's IAudioRenderClient (sink+0x48): measures what each output sends
// (ONRUSH_AUDIO_DIAG=1 logs per-output peaks) and can turn the output silent.
struct RenderClientWrap : IAudioRenderClient {
  IAudioRenderClient *inner; volatile LONG refs = 1; int type; UINT32 chans; BYTE *last = nullptr;
  volatile bool silent = false; float peak = 0; double sq = 0; UINT64 frames = 0; bool diag;
  RenderClientWrap(IAudioRenderClient *i, int t, UINT32 c, bool d) : inner(i), type(t), chans(c ? c : 2), diag(d) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **pp) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(IAudioRenderClient)) { *pp = this; AddRef(); return S_OK; }
    return inner->QueryInterface(id, pp);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&refs);
    if (r == 0) { inner->Release(); delete this; }
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 n, BYTE **pp) override {
    HRESULT hr = inner->GetBuffer(n, pp); last = SUCCEEDED(hr) && pp ? *pp : nullptr; return hr;
  }
  HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 n, DWORD flags) override {
    if (diag && last && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
      const float *f = (const float *)last;
      for (UINT32 i = 0; i < n * chans; ++i) { float a = fabsf(f[i]); if (a < 100.f) { sq += (double)a * a; if (a > peak) peak = a; } }
      frames += n;
      if (frames >= 48000 * 5) { log_line("[audio] diag out type=%d peak(5s)=%.4f rms=%.4f\r\n", type, peak, sqrt(sq / ((double)frames * chans + 1))); peak = 0; sq = 0; frames = 0; }
    }
    if (silent) flags |= AUDCLNT_BUFFERFLAGS_SILENT;
    return inner->ReleaseBuffer(n, flags);
  }
};
static RenderClientWrap *volatile g_musicOutWrap = nullptr;

static int DetourWwiseSinkInit(void *sink, void *device, void *settings) {
  int r = g_TrueWwiseSinkInit(sink, device, settings);
  int type = *(int *)((uint8_t *)settings + 0x14);
  log_line("[audio] Wwise sink init type=%d -> %d\r\n", type, r);
  IAudioRenderClient **slot = (IAudioRenderClient **)((uint8_t *)sink + 0x48);
  if (r != 1 || !*slot) return r;
  bool diag = GetEnvironmentVariableA("ONRUSH_AUDIO_DIAG", nullptr, 0) != 0;
  auto *w = new RenderClientWrap(*slot, type, *(UINT32 *)((uint8_t *)sink + 0x5C), diag);
  *slot = w;  // the sink owns the reference it got from GetService; the wrapper now holds it
  char dm[8] = {};  // diagnostics: ONRUSH_AUDIO_DIAG_MUTE=<type> silences that output
  if (GetEnvironmentVariableA("ONRUSH_AUDIO_DIAG_MUTE", dm, sizeof(dm)) && atoi(dm) == type) {
    w->silent = true; log_line("[audio] diag: output type %d muted\r\n", type);
  }
  if (type == 3) {
    g_musicOutWrap = w;
    if (g_muteGameMusicOut) { w->silent = true; log_line("[audio] game music output muted (own playlist)\r\n"); }
  }
  return r;
}

// Game audio layer SetRTPC(id, value) at exe+0x1F55B0: forwards to the audio system
// object [exe+0x1675920] vtable slot 0xE0. The option sliders go through it, among them
// "Music_Volume" (Wwise id = FNV-1 of the lower-case name). With the player's own
// playlist the game music is held at 0 while effects/voices keep their volume.
static HMODULE g_rtpcExe = nullptr;
static volatile bool g_forceGameMusicOff = false;
static volatile LONG g_rtpcLogged = 0;
static volatile uint32_t kRtpcMusicVolume = 0xFFFFFFFFu;  // game handle, learnt from the name lookup
// exe+0x1F3D40 (gameObj, name) -> handle: audio system vtable slot 0x58.
static uint32_t DetourRtpcLookup(int gameObj, const char *name) {
  void *sys = *(void **)((uint8_t *)g_rtpcExe + 0x01675920);
  typedef uint32_t (*PFN)(void *, int, const char *);
  uint32_t h = ((PFN)(*(void ***)sys)[0x58 / 8])(sys, gameObj, name);
  if (name && _stricmp(name, "Music_Volume") == 0) { kRtpcMusicVolume = h; log_line("[audio] Music_Volume rtpc handle=%X\r\n", h); }
  return h;
}
static void DetourSetRtpc(uint32_t id, float value) {
  if (g_rtpcLogged < 60 && GetEnvironmentVariableA("ONRUSH_AUDIO_DIAG", nullptr, 0)) {
    InterlockedIncrement(&g_rtpcLogged); log_line("[audio] rtpc id=%08X value=%.3f\r\n", id, value);
  }
  if (id == kRtpcMusicVolume && g_forceGameMusicOff) value = 0.0f;
  char zl[128] = {};  // diagnostics: ONRUSH_RTPC_ZERO=15D,15E forces those handles to 0
  if (GetEnvironmentVariableA("ONRUSH_RTPC_ZERO", zl, sizeof(zl))) {
    char *ctx = nullptr;
    for (char *t = strtok_s(zl, ",", &ctx); t; t = strtok_s(nullptr, ",", &ctx))
      if (strtoul(t, nullptr, 16) == id) value = 0.0f;
  }
  void *sys = *(void **)((uint8_t *)g_rtpcExe + 0x01675920);
  typedef void (*PFN)(void *, uint32_t, float);
  ((PFN)(*(void ***)sys)[0xE0 / 8])(sys, id, value);
}
static void HookSetRtpc(HMODULE hExe) {
  uint8_t *p = (uint8_t *)hExe + 0x001F55B0;
  static const uint8_t expected[9] = {0x8B, 0xD1, 0x48, 0x8B, 0x0D, 0x67, 0x03, 0x48, 0x01};  // 1.80: rcx = [exe+0x1675920]
  if (memcmp(p, expected, 9) != 0) { log_line("[audio] WARNING: unexpected SetRtpc bytes; not hooked\r\n"); return; }
  g_rtpcExe = hExe;
  DWORD old = 0;
  if (!VirtualProtect(p, 14, PAGE_EXECUTE_READWRITE, &old)) return;
  p[0] = 0xFF; p[1] = 0x25; *(uint32_t *)(p + 2) = 0; *(uint64_t *)(p + 6) = (uint64_t)&DetourSetRtpc;
  VirtualProtect(p, 14, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, 14);
  uint8_t *q = (uint8_t *)hExe + 0x001F3D40;
  static const uint8_t expectedLookup[9] = {0x44, 0x8B, 0xC9, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0x0D};
  if (memcmp(q, expectedLookup, 9) == 0 && VirtualProtect(q, 14, PAGE_EXECUTE_READWRITE, &old)) {
    q[0] = 0xFF; q[1] = 0x25; *(uint32_t *)(q + 2) = 0; *(uint64_t *)(q + 6) = (uint64_t)&DetourRtpcLookup;
    VirtualProtect(q, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), q, 14);
  } else {
    log_line("[audio] WARNING: unexpected rtpc lookup bytes; not hooked\r\n");
  }
  log_line("[audio] Hooked SetRtpc (game music forced off: %d)\r\n", g_forceGameMusicOff ? 1 : 0);
}

// Team identity setup (exe+0x2FBE10(this, session)): session+0x74 is the match type; types 0 and 4 use the
// default Blue/Orange teams, others look up the team ids stored at session+0xCD / +0x10D (Vs_Team_Logos table).
typedef void (*PFN_TeamSetup)(void *self, void *session);
static PFN_TeamSetup g_TrueTeamSetup = nullptr;
static uint8_t g_TeamSetupTramp[64] = {};
// Career events name their teams with grid-lineup codes ('BHMKTLD', 'OAYGSNO'...) whose table entries carry the
// generic ID_TEAM_BLUE / ID_TEAM_ORANGE logos. The retail game showed the character crews instead (local team =
// PLAYER_TEAM_<player character>, rival = another crew, e.g. Alpha Mile vs Gaijin Trap). Do that here with the
// game's own team table: exe+0x2DB1B0(charId) = crew of a character, 0x2DA530() = crew count, 0x2DB120(n) = n-th crew.
static bool IsLineupCode(const char *t) {
  if (!t || (t[0] != 'B' && t[0] != 'O')) return false;
  int n = 0; while (t[n] >= 'A' && t[n] <= 'Z') ++n;
  return n == 7 && t[7] == 0;
}
static void NameCareerTeams(uint8_t *session) {
  typedef const uint8_t *(*PFN_EntryByIdx)(int);
  typedef int (*PFN_Count)();
  uint8_t *exe = (uint8_t *)GetModuleHandleA(nullptr);
  __try {
    char *teamA = (char *)session + 0xCD, *teamB = (char *)session + 0x10D;
    int type = *(const int *)(session + 0x74);
    if (type == 0 || type == 4 || !IsLineupCode(teamA) || !IsLineupCode(teamB)) return;
    const uint8_t *profile = *(const uint8_t *const *)(exe + 0x1829E60);
    int charId = profile ? *(const int *)(profile + 0x38) : 0;
    const uint8_t *mine = ((PFN_EntryByIdx)(exe + 0x2DB1B0))(charId);
    int n = ((PFN_Count)(exe + 0x2DA530))();
    if (!mine || n < 2) return;
    // one rival per match: keep the pick while the session's lineup codes stay the same
    static char s_key[20] = {}; static int s_pick = -1;
    char key[20]; sprintf_s(key, "%.7s%.7s", teamA, teamB);
    if (strcmp(key, s_key) != 0 || s_pick < 0) {
      strcpy_s(s_key, key);
      s_pick = (int)((GetTickCount64() / 1000) % (ULONGLONG)n);
    }
    const uint8_t *rival = nullptr;
    for (int k = 0; k < n && !rival; ++k) {
      const uint8_t *e = ((PFN_EntryByIdx)(exe + 0x2DB120))((s_pick + k) % n);
      if (e && e != mine) rival = e;
    }
    if (!rival) return;
    log_line("[team] career teams '%s'/'%s' -> '%s'/'%s' (player character %d)\r\n", teamA, teamB,
             (const char *)mine + 0x48, (const char *)rival + 0x48, charId);
    strncpy_s(teamA, 64, (const char *)mine + 0x48, _TRUNCATE);
    strncpy_s(teamB, 64, (const char *)rival + 0x48, _TRUNCATE);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_line("[team] NameCareerTeams faulted\r\n");
  }
}
static void DetourTeamSetup(void *self, void *session) {
  if (session) {
    const uint8_t *s = (const uint8_t *)session;
    char a[65] = {}, b[65] = {};
    __try { memcpy(a, s + 0xCD, 64); memcpy(b, s + 0x10D, 64); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    log_line("[team] setup session=%p type=%d teamA='%s' teamB='%s'\r\n", session, *(const int *)(s + 0x74), a, b);
    static bool s_dumped = false;
    if (!s_dumped) {  // team table at exe+0x17B9B20: 50 entries of 0xC8, id string at +0x48, logo at +0x88
      s_dumped = true;
      const uint8_t *t = (const uint8_t *)GetModuleHandleA(nullptr) + 0x17B9B20;
      for (int i = 0; i < 50; ++i) {
        char id[33] = {}, lg[49] = {};
        __try { memcpy(id, t + i * 0xC8 + 0x48, 32); memcpy(lg, t + i * 0xC8 + 0x88, 48); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (id[0] || lg[0] || i < 3) log_line("[team] table[%d] id='%s' logo='%s' head='%.40s'\r\n", i, id, lg, (const char *)(t + i * 0xC8));
      }
    }
  }
  if (session && GetEnvironmentVariableA("ONRUSH_TEAM_ORIG", nullptr, 0) == 0) NameCareerTeams((uint8_t *)session);
  g_TrueTeamSetup(self, session);
}
static void HookTeamSetup(HMODULE hExe) {
  // prologue: mov [rsp+18],rbx; push rdi; sub rsp,0x80; mov rax,[rdx]  (16 bytes, no rip-relative)
  uint8_t *p = (uint8_t *)hExe + 0x002FBE10;  // 1.80: new prologue (mov rax,rsp; sub rsp,0x88), port pending
  static const uint8_t expected[16] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x02};
  if (memcmp(p, expected, 16) != 0) { log_line("[team] unexpected prologue; not hooked\r\n"); return; }
  DWORD old = 0;
  if (!VirtualProtect(g_TeamSetupTramp, sizeof(g_TeamSetupTramp), PAGE_EXECUTE_READWRITE, &old)) return;
  memcpy(g_TeamSetupTramp, p, 16);
  uint8_t *j = g_TeamSetupTramp + 16;
  j[0] = 0xFF; j[1] = 0x25; *(uint32_t *)(j + 2) = 0; *(uint64_t *)(j + 6) = (uint64_t)(p + 16);
  FlushInstructionCache(GetCurrentProcess(), g_TeamSetupTramp, sizeof(g_TeamSetupTramp));
  g_TrueTeamSetup = (PFN_TeamSetup)(void *)g_TeamSetupTramp;
  if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old)) return;
  p[0] = 0xFF; p[1] = 0x25; *(uint32_t *)(p + 2) = 0; *(uint64_t *)(p + 6) = (uint64_t)&DetourTeamSetup;
  p[14] = p[15] = 0x90;
  VirtualProtect(p, 16, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, 16);
  log_line("[team] hooked team setup\r\n");
}

static void HookWwiseSinkInit(HMODULE hExe) {
  // prologue: push rbp/rsi/rdi/r15; lea rbp,[rsp-0x3F]; sub rsp,0xA8  (18 bytes, no rip-relative)
  uint8_t *p = (uint8_t *)hExe + 0x00AA9FD0;
  static const uint8_t expected[18] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC1,
                                       0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00};
  if (memcmp(p, expected, 18) != 0) { log_line("[audio] WARNING: unexpected sink init prologue; not hooked\r\n"); return; }
  DWORD old = 0;
  if (!VirtualProtect(g_WwiseSinkInitTramp, sizeof(g_WwiseSinkInitTramp), PAGE_EXECUTE_READWRITE, &old)) return;
  memcpy(g_WwiseSinkInitTramp, p, 18);
  uint8_t *j = g_WwiseSinkInitTramp + 18;
  j[0] = 0xFF; j[1] = 0x25; *(uint32_t *)(j + 2) = 0; *(uint64_t *)(j + 6) = (uint64_t)(p + 18);
  FlushInstructionCache(GetCurrentProcess(), g_WwiseSinkInitTramp, sizeof(g_WwiseSinkInitTramp));
  g_TrueWwiseSinkInit = (PFN_WwiseSinkInit)(void *)g_WwiseSinkInitTramp;
  if (!VirtualProtect(p, 18, PAGE_EXECUTE_READWRITE, &old)) return;
  p[0] = 0xFF; p[1] = 0x25; *(uint32_t *)(p + 2) = 0; *(uint64_t *)(p + 6) = (uint64_t)&DetourWwiseSinkInit;
  p[14] = p[15] = p[16] = p[17] = 0x90;
  VirtualProtect(p, 18, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, 18);
  g_muteGameMusicOut = GetEnvironmentVariableA("ONRUSH_MUTE_GAME_MUSIC", nullptr, 0) != 0;
  log_line("[audio] Hooked Wwise sink init (mute game music: %d)\r\n", g_muteGameMusicOut ? 1 : 0);
}

// On Durango GPU virtual addresses are CPU-visible, and the resource init code
// at exe+0xBE0C5..0xBE0D1 does `memset(res->GetGPUVirtualAddress(), 0, size)`.
// On desktop D3D12 that address is not CPU-mapped, so the memset faults
// (AV in VCRUNTIME140). Route that single call through a fault-tolerant memset.
static volatile LONG s_gpuVaMemsetSkips = 0;
static void *__cdecl SafeMemsetGpuVa(void *dst, int val, size_t n) {
  // The address is usually the placed-resource backing in game memory (0x200xxxxxxxx), so zeroing
  // it is legitimate. ONRUSH_GPUVA_MEMSET_SKIP=1 never writes (tested: does not fix the main-menu
  // phase crash).
  static int s_allow = -1;
  if (s_allow < 0) s_allow = InputEnvFlag("ONRUSH_GPUVA_MEMSET_SKIP") ? 0 : 1;
  if (!s_allow) {
    if (InterlockedIncrement(&s_gpuVaMemsetSkips) <= 8)
      log_line("[gpuva] skipped memset(%p, %d, %zu) (GPU VA, never written)\r\n", dst, val, n);
    return dst;
  }
  __try {
    return memset(dst, val, n);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (InterlockedIncrement(&s_gpuVaMemsetSkips) <= 4) {
      log_line("[gpuva] skipped memset(%p, %d, %zu): GPU VA is not CPU-mapped\r\n",
               dst, val, n);
    }
    return dst;
  }
}

static void PatchGpuVaMemset(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x000BE0D1;  // call memset (0xD21EB2)
  uint8_t *memsetFn = (uint8_t *)hExe + 0x00D21EB2;
  if (site[0] != 0xE8 ||
      site + 5 + *(int32_t *)(site + 1) != memsetFn) {
    log_line("[gpuva] WARNING: exe+0xBE0D1 is not `call memset`; not patched\r\n");
    return;
  }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 64);
  if (!stub) {
    log_line("[gpuva] ERROR: no near memory for stub\r\n");
    return;
  }
  DWORD old = 0;
  VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
  // jmp qword ptr [rip+0] ; <abs64>
  stub[0] = 0xFF; stub[1] = 0x25; *(uint32_t *)(stub + 2) = 0;
  *(void **)(stub + 6) = (void *)&SafeMemsetGpuVa;
  FlushInstructionCache(GetCurrentProcess(), stub, 14);
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) {
    log_line("[gpuva] ERROR: stub too far (%lld)\r\n", (long long)diff);
    return;
  }
  if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
    *(int32_t *)(site + 1) = (int32_t)diff;
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    VirtualProtect(site, 5, old, &old);
    log_line("[gpuva] Patched exe+0xBE0D1: memset(GPU VA) is now fault-tolerant\r\n");
  }
}

// Redirect `call rel32` at exe+0x21ADD9 (end of the boot thread's persistent
// phase setup) through a stub that raises g_bootThreadDone first.
static void PatchBootDoneFlag(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x0021ADD9;
  uint8_t *orig = (uint8_t *)hExe + 0x0016A670;
  if (site[0] != 0xE8 || site + 5 + *(int32_t *)(site + 1) != orig) {
    log_line("[boot] WARNING: exe+0x21ADD9 is not `call 0x16A670`; boot-done flag not installed\r\n");
    g_bootThreadDone = 1;  // fail open
    return;
  }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 64);
  if (!stub) { g_bootThreadDone = 1; return; }
  DWORD old = 0;
  VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  *q++ = 0x48; *q++ = 0xB8; { LONG64 *f = (LONG64 *)&g_bootThreadDone; memcpy(q, &f, 8); q += 8; }  // mov rax, &flag
  *q++ = 0xC6; *q++ = 0x00; *q++ = 0x01;                                                            // mov byte [rax], 1
  *q++ = 0x48; *q++ = 0xB8; memcpy(q, &orig, 8); q += 8;                                           // mov rax, orig
  *q++ = 0xFF; *q++ = 0xE0;                                                                         // jmp rax
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) { g_bootThreadDone = 1; return; }
  if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
    *(int32_t *)(site + 1) = (int32_t)diff;
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    VirtualProtect(site, 5, old, &old);
    log_line("[boot] Installed boot-thread-done flag at exe+0x21ADD9\r\n");
  }
}

#include "gfx_options.inc"
#include "cameras.inc"
#include "mouse.inc"
#include "language.inc"
#include "main_menu.inc"
namespace ups { static void Install(HMODULE hExe); }   // upscaler.inc

// Title screen without the dead online services. After "press A" the title state machine
// (exe+0x3B4D42) sits in state 3 ("CONECTANDO") until GameSparks signs in, which never happens
// now, or until its timeout ([obj+0x2F0] elapsed > [obj+0x2F8], ~20 s) runs out; state 4 then
// shows "CONTINUAR OFFLINE" and waits for A ([obj+0x380] == 4) before loading (state 5).
// Both waits go: the timeout counts as expired at once and state 4 continues offline by itself.
static void PatchSkipOnlineWait(HMODULE hExe) {
  uint8_t *timeout = (uint8_t *)hExe + 0x003B4EAC;  // cmp [rdi+2F0h], rax ; setg bl
  uint8_t *accept = (uint8_t *)hExe + 0x003B4F45;   // jne (A not pressed on "continue offline")
  static const uint8_t kTimeout[] = {0x48, 0x39, 0x87, 0xF0, 0x02, 0x00, 0x00, 0x0F, 0x9F, 0xC3};
  static const uint8_t kAccept[] = {0x75, 0x28};
  if (memcmp(timeout, kTimeout, sizeof(kTimeout)) != 0 || memcmp(accept, kAccept, sizeof(kAccept)) != 0 ||
      memcmp(accept - 7, "\x83\xBF\x80\x03\x00\x00\x04", 7) != 0) {
    log_line("[online] WARNING: unexpected title-screen code; online wait not skipped\r\n");
    return;
  }
  static const uint8_t kExpired[] = {0xB3, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};  // mov bl, 1
  static const uint8_t kNop2[] = {0x90, 0x90};
  if (gfxopt::WriteCode(timeout, kExpired, sizeof(kExpired)) && gfxopt::WriteCode(accept, kNop2, sizeof(kNop2)))
    log_line("[online] title screen goes straight to offline play (no connection wait)\r\n");
}

// Voice (dub) language independent of the text language. audio:audiosystem.cfg maps each game_language
// to an audio_language; exe+0x1F42A0 walks that table and at +0x1E9067 does
// strcmp([r15+8] = current game language, entry.game_language). Launcher option ONRUSH_VOICE_LANG
// (locale such as pt-BR, or a game code such as "bra") swaps the left side for the chosen language's
// code, so the entry of that language (and its audio_language) is picked.
static char g_voiceLangCode[8] = {};
static void PatchVoiceLanguage(HMODULE hExe) {
  char v[32] = {};
  DWORD n = GetEnvironmentVariableA("ONRUSH_VOICE_LANG", v, sizeof(v));
  if (n == 0 || n >= sizeof(v)) return;
  for (char *c = v; *c; ++c) *c = (char)tolower((unsigned char)*c);
  static const struct { const char *pre, *code; } map[] = {
    {"en", "eng"}, {"fr", "fre"}, {"es", "spa"}, {"de", "ger"}, {"it", "ita"}, {"pt", "bra"}, {"pl", "pol"}};
  const char *code = nullptr;
  for (auto &m : map) if (strncmp(v, m.pre, 2) == 0 && (v[2] == '-' || v[2] == 0)) code = m.code;
  if (!code) for (auto &m : map) if (strcmp(v, m.code) == 0) code = m.code;
  if (!code) { log_line("[lang] ONRUSH_VOICE_LANG='%s' not recognised; voice follows the text language\r\n", v); return; }
  strcpy_s(g_voiceLangCode, code);

  // 1.80: the table walk moved into exe+0x1F5050(cfg, current language, out); at +0x1F5141 it does
  // strcmp(r14 = current language, entry.game_language).
  uint8_t *site = (uint8_t *)hExe + 0x1F5141;
  uint8_t *strcmpFn = (uint8_t *)hExe + 0x06E750;
  const uint8_t exp[4] = { 0x49, 0x8B, 0xCE, 0xE8 };  // mov rcx,r14; call strcmp
  if (memcmp(site, exp, 4) != 0 || site + 8 + *(int32_t *)(site + 4) != strcmpFn) {
    log_line("[lang] WARNING: voice-language site +0x1F5141 differs; not patched\r\n");
    return;
  }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 64);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  *q++ = 0x49; *q++ = 0x8B; *q++ = 0xCE;                                                    // mov rcx,r14
  *q++ = 0x48; *q++ = 0xB8; { char *p = g_voiceLangCode; memcpy(q, &p, 8); q += 8; }       // mov rax,&code
  *q++ = 0x80; *q++ = 0x38; *q++ = 0x00;                                                    // cmp byte [rax],0
  *q++ = 0x48; *q++ = 0x0F; *q++ = 0x45; *q++ = 0xC8;                                       // cmovne rcx,rax
  *q++ = 0x48; *q++ = 0xB8; memcpy(q, &strcmpFn, 8); q += 8;                                // mov rax,strcmp
  *q++ = 0xFF; *q++ = 0xE0;                                                                 // jmp rax
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) {
    uint8_t call[8] = { 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x00 };                               // call stub; nop3
    int32_t d = (int32_t)diff; memcpy(call + 1, &d, 4);
    memcpy(site, call, 8);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    VirtualProtect(site, 8, old, &old);
    log_line("[lang] Voice language forced to '%s' (+0x1F5141)\r\n", g_voiceLangCode);
  }
}

// exe+0x212FA0 is a thunk: obj -> fn_0x190150(obj->[0x18], obj->[8], 0). While a level loads,
// obj->[0x18] is sometimes still null when the phase-enter code (exe+0x32CF74) calls it: it is
// filled by a loading thread, so this is a race and the game crashed at exe+0x190168
// (read of [0+0x18]). Wait for the field (up to 3 s), and skip the call if it never appears.
typedef void *(__fastcall *PFN_Fn183150)(void *self, void *arg, uint8_t flag);
static PFN_Fn183150 g_fn183150 = nullptr;
// The callee also uses obj->[0x18]->[0x18] (passed on to a vtable call that writes [it+0x2C],
// crash at exe+0x1177D9), which the loading thread fills a little later, so wait for both.
static bool Race207210Ready(uint8_t *obj) {
  uint8_t *inner = *(uint8_t *volatile *)(obj + 0x18);
  return inner && *(void *volatile *)(inner + 0x18);
}
static void *__fastcall Thunk207210Safe(uint8_t *obj) {
  if (!Race207210Ready(obj)) {
    ULONGLONG t0 = GetTickCount64();
    while (!Race207210Ready(obj) && GetTickCount64() - t0 < 5000) Sleep(1);
    bool ok = Race207210Ready(obj);
    log_line("[race] exe+0x212FA0: obj=%p not ready (field+0x18=%p), %s after %llu ms\r\n", obj, *(void **)(obj + 0x18),
             ok ? "filled" : "STILL NULL -> call skipped", (unsigned long long)(GetTickCount64() - t0));
    if (!ok) return nullptr;
  }
  return g_fn183150(*(void **)(obj + 0x18), *(void **)(obj + 8), 0);
}
// exe+0x308090 picks a start-grid slot ("A1", "B1"...) from the track's list at [[this+0x1C60]+0x1440],
// count at +0x1448. When the race starts before the track finished loading the list is empty, the
// fallback lookup returns null and the game reads it (crash exe+0x3082D9). Wait for the list first.
static void __fastcall WaitStartGrid2E22B0(uint8_t *obj) {
  __try {
    auto ready = [](uint8_t *o) { uint8_t *t = *(uint8_t *volatile *)(o + 0x1C60); return t && *(volatile uint64_t *)(t + 0x1448) != 0; };
    static volatile LONG s_timeouts = 0;   // an empty list may also be legit (fallback path): stop waiting after 2 misses
    if (ready(obj) || s_timeouts >= 2) return;
    ULONGLONG t0 = GetTickCount64();
    while (!ready(obj) && GetTickCount64() - t0 < 5000) Sleep(5);
    if (!ready(obj)) InterlockedIncrement(&s_timeouts);
    log_line("[race] exe+0x308090: start grid not loaded yet, %s after %llu ms\r\n", ready(obj) ? "filled" : "STILL EMPTY",
             (unsigned long long)(GetTickCount64() - t0));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void PatchStartGridWait2E22B0(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x308090;
  static const uint8_t kExpect[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x41, 0x54 };
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0) { log_line("[race] exe+0x308090 bytes differ; not patched\r\n"); return; }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 96);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 96, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  uint64_t fn = (uint64_t)&WaitStartGrid2E22B0, back = (uint64_t)(site + 5);
  *q++ = 0x51; *q++ = 0x52; *q++ = 0x41; *q++ = 0x50; *q++ = 0x41; *q++ = 0x51;    // push rcx, rdx, r8, r9
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x28;                               // sub rsp, 0x28
  *q++ = 0x48; *q++ = 0xB8; memcpy(q, &fn, 8); q += 8;                             // mov rax, WaitStartGrid2E22B0
  *q++ = 0xFF; *q++ = 0xD0;                                                         // call rax
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x28;                               // add rsp, 0x28
  *q++ = 0x41; *q++ = 0x59; *q++ = 0x41; *q++ = 0x58; *q++ = 0x5A; *q++ = 0x59;    // pop r9, r8, rdx, rcx
  memcpy(q, site, 5); q += 5;                                                       // mov [rsp+0x10], rdx
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &back, 8); q += 8;  // jmp site+5
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
  site[0] = 0xE9; int32_t rel = (int32_t)diff; memcpy(site + 1, &rel, 4);
  VirtualProtect(site, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), site, 5);
  log_line("[race] exe+0x308090 guarded (waits for the start grid, crash at +0x3082D9)\r\n");
}
// exe+0xFA730 asks the user manager (vtable +0xA0) for a user's slot and reads table[slot]
// without checking for -1 ("not found"); right after the title screen sign-in this read
// went 16 GB past the table (crash at exe+0xF2C53). A -1 slot now takes the function's own
// "false" exit (exe+0xFA77A), the same one it uses for an empty slot (0x80).
// The same unchecked lookup exists in three user-privilege checks (scan for
// "mov ecx,eax; mov ecx,[rdi+rcx*4+0x48]; cmp ecx,0x80"): exe+0xFA6A0 crashed on the race
// loading screen (exe+0xF2BCF), exe+0xFA730 after the title. Each gets its own "empty slot" exit.
static void PatchUserSlotSite(HMODULE hExe, uint32_t siteRva, uint32_t failRva) {
  uint8_t *site = (uint8_t *)hExe + siteRva;
  static const uint8_t kExpect[] = { 0x8B, 0xC8, 0x8B, 0x4C, 0x8F, 0x48, 0x81, 0xF9, 0x80, 0x00, 0x00, 0x00 };
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0) { log_line("[user] exe+0x%X bytes differ; not patched\r\n", siteRva); return; }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 64);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  uint64_t back = (uint64_t)(site + 6), fail = (uint64_t)hExe + failRva;
  *q++ = 0x83; *q++ = 0xF8; *q++ = 0xFF;                            // cmp eax, -1
  *q++ = 0x74; *q++ = 0x14;                                         // je fail (+20)
  memcpy(q, site, 6); q += 6;                                       // mov ecx,eax ; mov ecx,[rdi+rcx*4+0x48]
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &back, 8); q += 8;  // jmp back
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &fail, 8); q += 8;  // fail: jmp <empty-slot exit>
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return;
  site[0] = 0xE9; int32_t rel = (int32_t)diff; memcpy(site + 1, &rel, 4); site[5] = 0x90;
  VirtualProtect(site, 6, old, &old);
  FlushInstructionCache(GetCurrentProcess(), site, 6);
  log_line("[user] exe+0x%X guarded (user slot -1 -> exe+0x%X)\r\n", siteRva, failRva);
}
static void PatchUserSlotF2C20(HMODULE hExe) {
  PatchUserSlotSite(hExe, 0xFA6E5, 0xFA6F6);   // exe+0xFA6A0: returns false
  PatchUserSlotSite(hExe, 0xFA769, 0xFA77A);   // exe+0xFA730: returns false
  PatchUserSlotSite(hExe, 0xFADEE, 0xFAF01);   // exe+0xF33xx: skips like an empty slot
}

// exe+0x1EF270 (set/clear bit 0 of obj+0xC8) is called by exe+0x3AEE90 with obj = owner+0x10, which
// can still be null while the hub menu loads after a race (crash exe+0x1E40E4). A null obj now returns 0.
static void PatchNullGuard1E40C0(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x1EF270;
  static const uint8_t kExpect[] = { 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20 };
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0) { log_line("[race] exe+0x1EF270 bytes differ; not patched\r\n"); return; }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 64);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  uint64_t back = (uint64_t)(site + 6);
  *q++ = 0x48; *q++ = 0x85; *q++ = 0xC9;                            // test rcx, rcx
  *q++ = 0x74; *q++ = 0x14;                                         // jz null (+20)
  memcpy(q, site, 6); q += 6;                                       // push r14 ; sub rsp,0x30
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &back, 8); q += 8;  // jmp back
  *q++ = 0x31; *q++ = 0xC0; *q++ = 0xC3;                            // null: xor eax,eax ; ret
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return;
  site[0] = 0xE9; int32_t rel = (int32_t)diff; memcpy(site + 1, &rel, 4); site[5] = 0x90;
  VirtualProtect(site, 6, old, &old);
  FlushInstructionCache(GetCurrentProcess(), site, 6);
  log_line("[race] exe+0x1EF270 guarded (null obj, crash at +0x1E40E4)\r\n");
}

// exe+0x1059B0 walks render items (0x60 bytes, word +0x50 = items in the group) and reads a 64-byte
// transform at table[item+0x4C & 0xFFFFFF]. On the race-prep screen some items carry a garbage index
// (0x48435C, 0xCF2E9D -> hundreds of MB past the table; crash exe+0xFD804). Such an item group is now
// ends the walk for that frame (rsi = end, exe+0x1060E0). Same if the walk already ran past the end.
static volatile LONG g_badXformSkips = 0;
static void PatchBadTransformIndexFD7E0(HMODULE hExe) {
  // 1.80: and r8d,0xFFFFFF; shl r8,6; add r8,r11 (table base in r11), 14 bytes at exe+0x105B11.
  uint8_t *site = (uint8_t *)hExe + 0x105B11;
  static const uint8_t kExpect[] = { 0x41, 0x81, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0x49, 0xC1, 0xE0, 0x06, 0x4D, 0x03, 0xC3 };
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0) { log_line("[race] exe+0x105B11 bytes differ; not patched\r\n"); return; }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 128);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 128, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  uint64_t back = (uint64_t)(site + 14), skip = (uint64_t)hExe + 0x1060E0, cnt = (uint64_t)&g_badXformSkips;
  *q++ = 0x48; *q++ = 0x3B; *q++ = 0xB5; *q++ = 0xC8; *q++ = 0x00; *q++ = 0x00; *q++ = 0x00;  // cmp rsi, [rbp+0xC8] (end)
  *q++ = 0x73; *q++ = 0x25;                                                         // jae bad (+37): walked past the end
  *q++ = 0x41; *q++ = 0x81; *q++ = 0xE0; *q++ = 0xFF; *q++ = 0xFF; *q++ = 0xFF; *q++ = 0x00;  // and r8d, 0xFFFFFF
  *q++ = 0x41; *q++ = 0x81; *q++ = 0xF8; *q++ = 0x00; *q++ = 0x00; *q++ = 0x10; *q++ = 0x00;  // cmp r8d, 0x100000
  *q++ = 0x73; *q++ = 0x15;                                                         // jae bad (+21)
  *q++ = 0x49; *q++ = 0xC1; *q++ = 0xE0; *q++ = 0x06;                               // shl r8, 6
  *q++ = 0x4D; *q++ = 0x03; *q++ = 0xC3;                                            // add r8, r11
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &back, 8); q += 8;  // jmp back
  // bad: (r8 is only the index here, dead on this path)
  *q++ = 0x49; *q++ = 0xB8; memcpy(q, &cnt, 8); q += 8;                            // mov r8, &g_badXformSkips
  *q++ = 0xF0; *q++ = 0x41; *q++ = 0xFF; *q++ = 0x00;                               // lock inc dword [r8]
  // The group count of a garbage item is garbage too, so stepping over it can run past the end and
  // loop forever: end the whole walk for this frame instead (rsi = end).
  *q++ = 0x48; *q++ = 0x8B; *q++ = 0xB5; *q++ = 0xC8; *q++ = 0x00; *q++ = 0x00; *q++ = 0x00;  // mov rsi, [rbp+0xC8]
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &skip, 8); q += 8;  // jmp exe+0x1060E0
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (!VirtualProtect(site, 14, PAGE_EXECUTE_READWRITE, &old)) return;
  site[0] = 0xE9; int32_t rel = (int32_t)diff; memcpy(site + 1, &rel, 4); memset(site + 5, 0x90, 9);
  VirtualProtect(site, 14, old, &old);
  FlushInstructionCache(GetCurrentProcess(), site, 14);
  log_line("[race] exe+0x105B11 guarded (bad transform index, crash at +0x105B36)\r\n");

  // Loop end "cmp rsi,[rbp+0xC8]; jne loop" never ends once a garbage group count steps past the end:
  // make it "jb loop" (exe+0x1060E7: 0F 85 -> 0F 82).
  uint8_t *je = (uint8_t *)hExe + 0x1060E7;
  if (je[0] == 0x0F && je[1] == 0x85 && VirtualProtect(je, 2, PAGE_EXECUTE_READWRITE, &old)) {
    je[1] = 0x82; VirtualProtect(je, 2, old, &old); FlushInstructionCache(GetCurrentProcess(), je, 2);
    log_line("[race] exe+0x1060E7 loop end is now 'below end' (no endless walk)\r\n");
  }
  // Bucket id 0x7FFF (garbage material entry, crash exe+0x106089 after a lock xadd into the wrong
  // table) -> skip the item through the function's own skip path exe+0x1060B6.
  uint8_t *bs = (uint8_t *)hExe + 0x105E82;
  static const uint8_t kB[] = { 0xD1, 0xE8, 0x25, 0xFF, 0x7F, 0x00, 0x00, 0x44, 0x8B, 0xF0 };
  if (memcmp(bs, kB, sizeof(kB)) != 0) { log_line("[race] exe+0x105E82 bytes differ; not patched\r\n"); return; }
  uint8_t *st2 = (uint8_t *)AllocateNearAddress(bs, 64);
  if (!st2) return;
  VirtualProtect(st2, 64, PAGE_EXECUTE_READWRITE, &old);
  q = st2;
  uint64_t b2 = (uint64_t)(bs + 10), sk2 = (uint64_t)hExe + 0x1060B6;
  *q++ = 0xD1; *q++ = 0xE8;                                                         // shr eax, 1
  *q++ = 0x25; *q++ = 0xFF; *q++ = 0x7F; *q++ = 0x00; *q++ = 0x00;                  // and eax, 0x7FFF
  *q++ = 0x3D; *q++ = 0xFF; *q++ = 0x7F; *q++ = 0x00; *q++ = 0x00;                  // cmp eax, 0x7FFF
  *q++ = 0x74; *q++ = 0x11;                                                         // je skip (+17)
  *q++ = 0x44; *q++ = 0x8B; *q++ = 0xF0;                                            // mov r14d, eax
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &b2, 8); q += 8;    // jmp back
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &sk2, 8); q += 8;   // skip: jmp exe+0x1060B6
  FlushInstructionCache(GetCurrentProcess(), st2, (size_t)(q - st2));
  int64_t d2 = (int64_t)(st2 - (bs + 5));
  if (d2 < INT32_MIN || d2 > INT32_MAX) return;
  if (!VirtualProtect(bs, 10, PAGE_EXECUTE_READWRITE, &old)) return;
  bs[0] = 0xE9; int32_t r2 = (int32_t)d2; memcpy(bs + 1, &r2, 4); memset(bs + 5, 0x90, 5);
  VirtualProtect(bs, 10, old, &old);
  FlushInstructionCache(GetCurrentProcess(), bs, 10);
  log_line("[race] exe+0x105E82 guarded (bucket 0x7FFF items skipped, crash at +0x106089)\r\n");
}

// exe+0x3B0C00 (hub widget update, same caller chain as the exe+0x1E40E4 crash) reads
// [mgr+0x25E0] via exe+0x27A220 and calls through it; while the hub reloads after a race it is
// still null (crash exe+0x3B0D34). Null now leaves the function through a copy of its epilogue.
static volatile LONG g_hubNullSkips = 0;
static void PatchHubWidgetNull36EC48(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x3B0D2E;
  static const uint8_t kExpect[] = { 0x48, 0x8B, 0xC8, 0x4C, 0x8B, 0xF8, 0x48, 0x8B, 0x10 };  // 1.80: mov rcx,rax; mov r15,rax; mov rdx,[rax]
  static const uint8_t kSaves[] = { 0x48, 0x89, 0x9C, 0x24, 0xF0, 0x00, 0x00, 0x00 };   // exe+0x3B0C34: mov [rsp+0xF0], rbx
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0 || memcmp((uint8_t *)hExe + 0x3B0C34, kSaves, sizeof(kSaves)) != 0) {
    log_line("[race] exe+0x3B0D2E bytes differ; not patched\r\n"); return;
  }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 160);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 160, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  uint64_t back = (uint64_t)(site + 9), cnt = (uint64_t)&g_hubNullSkips;
  auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *q++ = x; };
  put({ 0x48, 0x85, 0xC0 });                                // test rax, rax
  put({ 0x74, 0x17 });                                      // jz bail (+23)
  memcpy(q, site, 9); q += 9;                               // mov rcx,rax ; mov r15,rax ; mov rdx,[rax]
  put({ 0xFF, 0x25, 0, 0, 0, 0 }); memcpy(q, &back, 8); q += 8;  // jmp back
  // bail: count, restore what the prologue saved, return
  put({ 0x48, 0xB8 }); memcpy(q, &cnt, 8); q += 8;          // mov rax, &g_hubNullSkips
  put({ 0xF0, 0xFF, 0x00 });                                // lock inc dword [rax]
  put({ 0x48, 0x8B, 0x9C, 0x24, 0xF0, 0, 0, 0 });           // mov rbx, [rsp+0xF0]
  put({ 0x48, 0x8B, 0xBC, 0x24, 0xD0, 0, 0, 0 });           // mov rdi, [rsp+0xD0]
  put({ 0x4C, 0x8B, 0xA4, 0x24, 0xC8, 0, 0, 0 });           // mov r12, [rsp+0xC8]
  put({ 0x4C, 0x8B, 0xAC, 0x24, 0xC0, 0, 0, 0 });           // mov r13, [rsp+0xC0]
  put({ 0x4C, 0x8B, 0xBC, 0x24, 0xB0, 0, 0, 0 });           // mov r15, [rsp+0xB0]
  put({ 0xC5, 0xF8, 0x28, 0xB4, 0x24, 0xA0, 0, 0, 0 });     // vmovaps xmm6, [rsp+0xA0]
  put({ 0xC5, 0xF8, 0x28, 0xBC, 0x24, 0x90, 0, 0, 0 });     // vmovaps xmm7, [rsp+0x90]
  put({ 0xC5, 0x78, 0x28, 0x84, 0x24, 0x80, 0, 0, 0 });     // vmovaps xmm8, [rsp+0x80]
  put({ 0xC5, 0x78, 0x28, 0x4C, 0x24, 0x70 });              // vmovaps xmm9, [rsp+0x70]
  put({ 0x48, 0x81, 0xC4, 0xD8, 0, 0, 0 });                 // add rsp, 0xD8
  put({ 0x5E, 0x5D, 0xC3 });                                // pop rsi ; pop rbp ; ret
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &old)) return;
  site[0] = 0xE9; int32_t rel = (int32_t)diff; memcpy(site + 1, &rel, 4); memset(site + 5, 0x90, 4);
  VirtualProtect(site, 9, old, &old);
  FlushInstructionCache(GetCurrentProcess(), site, 9);
  log_line("[race] exe+0x3B0D2E guarded (hub widget null, crash at +0x3B0D34)\r\n");
}

// exe+0x1497B0 (render job, called from exe+0x149A68 with r8 = list read from the scene) gets a
// null list right after a race ends (crash exe+0x1497D5, several job threads). Null/tiny r8 -> return.
static void PatchNullList13E870(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x1497B0;
  static const uint8_t kExpect[] = { 0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48 };
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0) { log_line("[race] exe+0x1497B0 bytes differ; not patched\r\n"); return; }
  uint8_t *stub = (uint8_t *)AllocateNearAddress(site, 64);
  if (!stub) return;
  DWORD old = 0;
  VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = stub;
  uint64_t back = (uint64_t)(site + 5);
  *q++ = 0x49; *q++ = 0x81; *q++ = 0xF8; *q++ = 0x00; *q++ = 0x00; *q++ = 0x01; *q++ = 0x00;  // cmp r8, 0x10000
  *q++ = 0x72; *q++ = 0x13;                                                         // jb null (+19)
  memcpy(q, site, 5); q += 5;                                                       // push rbp ; push rdi ; push r12
  *q++ = 0xFF; *q++ = 0x25; memset(q, 0, 4); q += 4; memcpy(q, &back, 8); q += 8;  // jmp site+5
  *q++ = 0x31; *q++ = 0xC0; *q++ = 0xC3;                                            // null: xor eax,eax ; ret
  FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(q - stub));
  int64_t diff = (int64_t)(stub - (site + 5));
  if (diff < INT32_MIN || diff > INT32_MAX) return;
  if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
  site[0] = 0xE9; int32_t rel = (int32_t)diff; memcpy(site + 1, &rel, 4);
  VirtualProtect(site, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), site, 5);
  log_line("[race] exe+0x1497B0 guarded (null render list, crash at +0x1497D5)\r\n");
}

static void PatchLoadRace207210(HMODULE hExe) {
  uint8_t *site = (uint8_t *)hExe + 0x212FA0;
  static const uint8_t kExpect[12] = { 0x48, 0x8B, 0x51, 0x08, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0x49, 0x18, 0xE9 };
  if (memcmp(site, kExpect, sizeof(kExpect)) != 0) { log_line("[race] exe+0x212FA0 bytes differ; not patched\r\n"); return; }
  int32_t rel; memcpy(&rel, site + 12, 4);
  g_fn183150 = (PFN_Fn183150)(site + 16 + rel);
  if ((uint8_t *)g_fn183150 != (uint8_t *)hExe + 0x190150) { log_line("[race] exe+0x212FA0 jmp target unexpected; not patched\r\n"); return; }
  Install14ByteDetour(site, (void *)&Thunk207210Safe);
  log_line("[race] exe+0x212FA0 guarded (load race, crash at +0x190168)\r\n");
}

// The game's Free thunk (exe+0x69940) hands any pointer that its own allocators do
// not "contain" to the CRT free() through an IAT slot. Our arena pointers are not
// CRT heap blocks (heap corruption 0xC0000374), so route that single call site
// through a free() that ignores arena memory.
typedef void (__cdecl *PFN_CrtFree)(void *);
static PFN_CrtFree g_origCrtFree = nullptr;
static void __cdecl ArenaAwareFree(void *p) {
  if (g_arenaBase && (uint8_t *)p >= g_arenaBase && (uint8_t *)p < g_arenaBase + g_arenaSize) return;
  if (g_origCrtFree) g_origCrtFree(p);
}

static void PatchGameFreeThunk(HMODULE hExe) {
  // IAT slot of ucrtbase!_free_base in the game exe (used by the free thunks at
  // exe+0x69970 and exe+0x69BE0). Swap the slot itself so every caller is covered.
  void **iat = (void **)((uint8_t *)hExe + 0x00E90570);
  HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
  void *expected = ucrt ? (void *)GetProcAddress(ucrt, "_free_base") : nullptr;
  if (!expected || *iat != expected) {
    log_line("[arena] WARNING: exe IAT slot +0xE90570 is not ucrtbase!_free_base (%p vs %p); not patched\r\n", *iat, expected);
    return;
  }
  g_origCrtFree = (PFN_CrtFree)*iat;
  DWORD old = 0;
  if (VirtualProtect(iat, sizeof(void *), PAGE_READWRITE, &old)) {
    *iat = (void *)&ArenaAwareFree;
    VirtualProtect(iat, sizeof(void *), old, &old);
    log_line("[arena] Patched exe IAT _free_base -> ArenaAwareFree (arena pointers are never passed to CRT free)\r\n");
  }
}

static void PatchAllocThunk(HMODULE hExe) {
  PatchAcpWait(hExe);
  PatchWwiseDeviceStateMask(hExe);
  HookWwiseSinkInit(hExe);
  HookTeamSetup(hExe);
  g_forceGameMusicOff = GetEnvironmentVariableA("ONRUSH_MUTE_GAME_MUSIC", nullptr, 0) != 0;
  if (music::Start()) g_forceGameMusicOff = true;  // player's own playlist replaces the game music
  HookSetRtpc(hExe);
  PatchGameFreeThunk(hExe);
  PatchBootDoneFlag(hExe);
  gfxopt::Install(hExe);
  cams::Install(hExe);
  mouse::Install(hExe);
  lang::Install(hExe);
  mainmenu::Install(hExe);
  ups::Install(hExe);
  PatchSkipOnlineWait(hExe);
  PatchVoiceLanguage(hExe);
  PatchUnlockCosmetics(hExe);
  PatchLoadRace207210(hExe);
  PatchUserSlotF2C20(hExe);
  PatchNullGuard1E40C0(hExe);
  PatchBadTransformIndexFD7E0(hExe);
  PatchStartGridWait2E22B0(hExe);
  PatchHubWidgetNull36EC48(hExe);
  PatchNullList13E870(hExe);
  PatchGpuVaMemset(hExe);
  // 1. Detour 0x00069B80 (allocator thunk) with a 14-byte 64-bit indirect jump
  Install14ByteDetour((uint8_t *)hExe + 0x00069B80, (void *)&BridgeAlloc);
  log_line("[d3d12_x] Patched 0x00069B80 with BridgeAlloc detour -> %p\r\n",
           (void *)&BridgeAlloc);

  // 2. Detour PlayGo Package Watcher at 0x000A8450
  Install14ByteDetour((uint8_t *)hExe + 0x000A8450, (void *)&DetourPlayGoInit);
  log_line("[d3d12_x] Patched 0x000A8450 with DetourPlayGoInit -> %p\r\n",
           (void *)&DetourPlayGoInit);

  // 3. Detour PlayGo GetChunkStatus at 0x000A3E10
  Install14ByteDetour((uint8_t *)hExe + 0x000A3E10,
                      (void *)&DetourPlayGoGetChunkStatus);
  log_line(
      "[d3d12_x] Patched 0x000A3E10 with DetourPlayGoGetChunkStatus -> %p\r\n",
      (void *)&DetourPlayGoGetChunkStatus);

  // 4. Detour PackageTransferManager at 0x000A8360
  Install14ByteDetour((uint8_t *)hExe + 0x000A8360,
                      (void *)&DetourPackageTransferManager);
  log_line("[d3d12_x] Patched 0x000A8360 with DetourPackageTransferManager -> "
           "%p\r\n",
           (void *)&DetourPackageTransferManager);

  // 2. Initialize global flow allocator pointer at 0x0167EF58
  uintptr_t allocPtrRva = 0x0167EF58;
  void **ppAlloc = (void **)((uint8_t *)hExe + allocPtrRva);
  DWORD oldProt2 = 0;
  if (VirtualProtect(ppAlloc, sizeof(void *), PAGE_EXECUTE_READWRITE,
                     &oldProt2)) {
    if (*ppAlloc == nullptr) {
      *ppAlloc = &g_FallbackAllocator;
      log_line("[d3d12_x] Initialized [hExe + 0x%08llX] with fallback "
               "allocator %p\r\n",
               (unsigned long long)allocPtrRva, &g_FallbackAllocator);
    }
    VirtualProtect(ppAlloc, sizeof(void *), oldProt2, &oldProt2);
  }

  // 3. Initialize global string/standard allocator pointer at 0x01524488
  uintptr_t stringAllocPtrRva = 0x01524488;
  void **ppStringAlloc = (void **)((uint8_t *)hExe + stringAllocPtrRva);
  DWORD oldProtStr = 0;
  if (VirtualProtect(ppStringAlloc, sizeof(void *), PAGE_EXECUTE_READWRITE,
                     &oldProtStr)) {
    if (*ppStringAlloc == nullptr) {
      *ppStringAlloc = &g_FallbackAllocator;
      log_line("[d3d12_x] Initialized [hExe + 0x%08llX] with fallback string "
               "allocator %p\r\n",
               (unsigned long long)stringAllocPtrRva, &g_FallbackAllocator);
    }
    VirtualProtect(ppStringAlloc, sizeof(void *), oldProtStr, &oldProtStr);
  }

  // 4. Initialize global allocator ready flag at 0x015244D8
  uintptr_t flagRva = 0x015244D8;
  uint8_t *pFlag = (uint8_t *)hExe + flagRva;
  DWORD oldProtFlag = 0;
  if (VirtualProtect(pFlag, 1, PAGE_EXECUTE_READWRITE, &oldProtFlag)) {
    *pFlag = 1;
    VirtualProtect(pFlag, 1, oldProtFlag, &oldProtFlag);
  }

  // 5. Initialize global root heap allocator pointer at 0x01524490
  uintptr_t rootAllocPtrRva = 0x01524490;
  void **ppRootAlloc = (void **)((uint8_t *)hExe + rootAllocPtrRva);
  DWORD oldProt3 = 0;
  if (VirtualProtect(ppRootAlloc, sizeof(void *), PAGE_EXECUTE_READWRITE,
                     &oldProt3)) {
    if (*ppRootAlloc == nullptr) {
      *ppRootAlloc = &g_FallbackRootAllocator;
      log_line("[d3d12_x] Initialized [hExe + 0x%08llX] with fallback root "
               "allocator %p\r\n",
               (unsigned long long)rootAllocPtrRva, &g_FallbackRootAllocator);
    }
    VirtualProtect(ppRootAlloc, sizeof(void *), oldProt3, &oldProt3);
  }

  // 6. Initialize global memory tracker list pointer at 0x01529128
  uintptr_t trackerPtrRva = 0x01529128;
  void **ppTracker = (void **)((uint8_t *)hExe + trackerPtrRva);
  DWORD oldProt4 = 0;
  if (VirtualProtect(ppTracker, sizeof(void *), PAGE_EXECUTE_READWRITE,
                     &oldProt4)) {
    if (*ppTracker == nullptr) {
      *ppTracker = &g_EmptyTrackerList;
      log_line("[d3d12_x] Initialized [hExe + 0x%08llX] with empty tracker "
               "list %p\r\n",
               (unsigned long long)trackerPtrRva, &g_EmptyTrackerList);
    }
    VirtualProtect(ppTracker, sizeof(void *), oldProt4, &oldProt4);
  }

  // 6b. Initialize [hExe + 0x01813990] with dummy audio manager buffer
  void **ppMusicMgr = (void **)((uint8_t *)hExe + 0x01813990);
  if (ppMusicMgr && *ppMusicMgr == nullptr && GetEnvironmentVariableA("ONRUSH_NO_AUDIO", nullptr, 0) != 0) {
    void *pDummy = VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pDummy) {
      memset(pDummy, 0, 0x10000);
      *ppMusicMgr = pDummy;
      log_line("[d3d12_x] Initialized [hExe + 0x01813990] with dummy audio manager %p\r\n", pDummy);
    }
  }

  // 6c. Initialize [hExe + 0x017F8FF0] with dummy SFX singleton buffer
  void **ppSfxMgr = (void **)((uint8_t *)hExe + 0x017F8FF0);
  if (ppSfxMgr && *ppSfxMgr == nullptr) {
    void *pDummySfx = VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pDummySfx) {
      memset(pDummySfx, 0, 0x10000);
      *(uint32_t *)((uint8_t *)pDummySfx + 8) = 6;
      *ppSfxMgr = pDummySfx;
      log_line("[d3d12_x] Initialized [hExe + 0x017F8FF0] with dummy SFX manager %p (state=6)\r\n", pDummySfx);
    }
  }

  // 7. Hook ContextAllocator::Reallocate at RVA 0x013E6F00 (slot 2 of vtable
  // 0x141376A20)
  uintptr_t reallocVtableSlotRva = 0x013E6F00;
  void **ppReallocSlot = (void **)((uint8_t *)hExe + reallocVtableSlotRva);
  void *expectedOrigFn = (void *)((uint8_t *)hExe + 0x7CF60);
  void *actualOrigFn = *ppReallocSlot;
  log_line("[d3d12_x] ContextAllocator::Reallocate slot check at [hExe + "
           "0x%08llX]: expected=%p, actual=%p\r\n",
           (unsigned long long)reallocVtableSlotRva, expectedOrigFn,
           actualOrigFn);

  if (actualOrigFn != expectedOrigFn) {
    log_line("[d3d12_x] ERROR: ContextAllocator::Reallocate slot mismatch! "
             "(expected=%p, actual=%p). Aborting vtable patch.\r\n",
             expectedOrigFn, actualOrigFn);
  } else {
    DWORD oldProtRealloc = 0;
    if (VirtualProtect(ppReallocSlot, sizeof(void *), PAGE_EXECUTE_READWRITE,
                       &oldProtRealloc)) {
      g_TrueContextAllocatorReallocate =
          (PFN_ContextAllocatorReallocate)actualOrigFn;
      *ppReallocSlot = (void *)&HookedContextAllocatorReallocate;
      VirtualProtect(ppReallocSlot, sizeof(void *), oldProtRealloc,
                     &oldProtRealloc);
      log_line("[d3d12_x] Patched ContextAllocator::Reallocate at [hExe + "
               "0x%08llX] -> %p (orig=%p)\r\n",
               (unsigned long long)reallocVtableSlotRva,
               &HookedContextAllocatorReallocate,
               g_TrueContextAllocatorReallocate);
    }
  }

  // 7. IDXGIFactory2 at 0x01538AA8 is deferred to EnsureDXGIFactoryGlobal()
  // outside DllMain / loader lock to prevent DXGI_ERROR_INVALID_CALL
  // (0x887A0001).

  // 8. Patch 0x000BCD47 with: xor r13d, r13d; jmp 0x000BCDAE; followed by NOPs
  uint8_t *pDxgiQuery = (uint8_t *)hExe + 0x000BCD47;
  DWORD oldProtQuery = 0;
  if (VirtualProtect(pDxgiQuery, 0x67, PAGE_EXECUTE_READWRITE, &oldProtQuery)) {
    pDxgiQuery[0] = 0x45; // xor r13d, r13d (1.80: the interface pointer lives in r13)
    pDxgiQuery[1] = 0x31;
    pDxgiQuery[2] = 0xED;
    pDxgiQuery[3] = 0xEB; // jmp +0x62 (to 0x000BCDAE)
    pDxgiQuery[4] = 0x62;
    memset(pDxgiQuery + 5, 0x90, 0x67 - 5);
    VirtualProtect(pDxgiQuery, 0x67, oldProtQuery, &oldProtQuery);
    FlushInstructionCache(GetCurrentProcess(), pDxgiQuery, 0x67);
    log_line("[d3d12_x] Patched 0x000BCD47 to jump over D3D12->IDXGIDevice2 "
             "query chain directly to 0x000BCDAE\r\n");
  }

  // 9. Patch 0x000A98CB: 41 8B C8 (mov ecx, r8d) -> 49 8B C8 (mov rcx, r8)
  // On Durango, CPU descriptor handles were 32-bit addresses in the low 4GB.
  // On 64-bit Windows desktop, D3D12 allocates descriptor heaps above 4GB,
  // so truncating r8 to 32 bits with mov ecx, r8d causes an immediate crash
  // when dereferenced.
  uint8_t *pDescWrite = (uint8_t *)hExe + 0x000A98CB;
  DWORD oldProtDesc = 0;
  if (VirtualProtect(pDescWrite, 3, PAGE_EXECUTE_READWRITE, &oldProtDesc)) {
    if (pDescWrite[0] == 0x41 && pDescWrite[1] == 0x8B &&
        pDescWrite[2] == 0xC8) {
      pDescWrite[0] =
          0x49; // mov rcx, r8 (preserve 64-bit descriptor heap pointer)
      FlushInstructionCache(GetCurrentProcess(), pDescWrite, 3);
      log_line("[d3d12_x] Patched 0x000A98CB: mov ecx, r8d -> mov rcx, r8 "
               "(64-bit descriptor handle fix)\r\n");
    }
    VirtualProtect(pDescWrite, 3, oldProtDesc, &oldProtDesc);
  }

  // 10. Detour ThreadAlloc at 0x00098EC0 and ThreadFree at 0x00099040
  // When loHeapAllocator (at 0x01537B90) is uninitialized/null, 0x00098EC0
  // causes an immediate null pointer dereference. Detouring these functions
  // provides robust fallback VirtualAlloc memory for Intel TBB worker thread
  // creation and stack allocation.
  Install14ByteDetour((uint8_t *)hExe + 0x00098EC0, (void *)&DetourThreadAlloc);
  Install14ByteDetour((uint8_t *)hExe + 0x00099040, (void *)&DetourThreadFree);
  log_line("[d3d12_x] Patched 0x00098EC0 (ThreadAlloc) and 0x00099040 "
           "(ThreadFree) detours\r\n");

  // 11. Patch 0x000B6C2A: 41 8B C8 (mov ecx, r8d) -> 49 8B C8 (mov rcx, r8)
  // On Durango, 64-bit buffer descriptor addresses in r8 were truncated to 32
  // bits into ecx because Xbox One physical/virtual allocations were in the low
  // 4GB. On 64-bit Windows desktop, buffer heap addresses are allocated above
  // 4GB, so truncating r8 to 32 bits results in an unmapped low 32-bit address
  // write at 0x000B6C42.
  g_bufferPointerPatchReady = PatchVerifiedR8ToRcx(
      hExe, 0x000B6C2A, "buffer pointer-width fix for crash site +0xB6C42");

  // 11b. Title screen null read at exe+0x27AFB8 (intermittent, depends on when A is pressed): a map
  // lookup (0x20D7D0) returns NULL and the 64 bytes behind it are copied unchecked. Call site
  // "mov rcx,rax; call 0x4B84A0" -> "call cave; nop3"; the cave (int3 padding at 0x9B26A4) returns
  // the zero block at 0x1522390 when rax is NULL. ONRUSH_NO_TITLE_NULL_FIX=1 disables.
  if (GetEnvironmentVariableA("ONRUSH_NO_TITLE_NULL_FIX", nullptr, 0) == 0) {
    uint8_t *b = (uint8_t *)hExe;
    const uintptr_t site = 0x27AFB0, cave = 0x9B26A4, zeros = 0x1522390;
    const uint8_t expSite[8] = { 0x48, 0x8B, 0xC8, 0xE8, 0xE8, 0xD4, 0x23, 0x00 };  // mov rcx,rax; call 0x4B84A0
    bool caveFree = true;
    for (int k = 0; k < 13; ++k) if (b[cave + k] != 0xCC) caveFree = false;
    if (memcmp(b + site, expSite, 8) == 0 && caveFree) {
      uint8_t stub[13] = { 0x48, 0x85, 0xC0, 0x75, 0x07, 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3 };  // test rax,rax; jnz +7; lea rax,[rip+d]; ret
      int32_t d = (int32_t)((intptr_t)zeros - (intptr_t)(cave + 12)); memcpy(stub + 8, &d, 4);
      uint8_t call[8] = { 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x00 };
      int32_t r = (int32_t)((intptr_t)cave - (intptr_t)(site + 5)); memcpy(call + 1, &r, 4);
      DWORD op = 0;
      if (VirtualProtect(b + cave, sizeof(stub), PAGE_EXECUTE_READWRITE, &op)) { memcpy(b + cave, stub, sizeof(stub)); VirtualProtect(b + cave, sizeof(stub), op, &op); }
      if (VirtualProtect(b + site, sizeof(call), PAGE_EXECUTE_READWRITE, &op)) { memcpy(b + site, call, sizeof(call)); VirtualProtect(b + site, sizeof(call), op, &op); }
      FlushInstructionCache(GetCurrentProcess(), b + site, sizeof(call));
      FlushInstructionCache(GetCurrentProcess(), b + cave, sizeof(stub));
      log_line("[d3d12_x] Applied title-screen null-lookup guard at +0x27AFB0\r\n");
    } else {
      log_line("[d3d12_x] WARNING: title null-lookup guard not applied (bytes differ)\r\n");
    }
  }

  // 11c. Launcher option "Pular introdução" (ONRUSH_SKIP_INTRO=1 / -skipintro): the persistent phase
  // (exe+0x2F98C0) asks for IntroVideoPhaseConfig (Codemasters logo + intro video) as the first phase.
  // Point that `lea rax,[rip+d]` at StartScreenPhaseConfig instead, so the legal screen goes straight
  // to the title. ONRUSH_SKIP_INTRO_PATCH=0 keeps the original first phase (A is still auto-pressed).
  {
    const wchar_t *cl = GetCommandLineW();
    char si[8] = {}, sp[8] = {};
    DWORD sn = GetEnvironmentVariableA("ONRUSH_SKIP_INTRO", si, sizeof(si));
    bool on = (sn > 0 && sn < sizeof(si) && si[0] != '0') || (cl && wcsstr(cl, L"-skipintro"));
    DWORD pn = GetEnvironmentVariableA("ONRUSH_SKIP_INTRO_PATCH", sp, sizeof(sp));
    if (pn > 0 && sp[0] == '0') on = false;
    if (on) {
      uint8_t *b = (uint8_t *)hExe;
      const uintptr_t site = 0x2F9972, introCfg = 0x142C0F0, titleCfg = 0x142C350;
      int32_t d0 = (int32_t)((intptr_t)introCfg - (intptr_t)(site + 7));
      if (b[site] == 0x48 && b[site + 1] == 0x8D && b[site + 2] == 0x05 && memcmp(b + site + 3, &d0, 4) == 0) {
        int32_t d1 = (int32_t)((intptr_t)titleCfg - (intptr_t)(site + 7));
        DWORD op = 0;
        if (VirtualProtect(b + site + 3, 4, PAGE_EXECUTE_READWRITE, &op)) {
          memcpy(b + site + 3, &d1, 4); VirtualProtect(b + site + 3, 4, op, &op);
          FlushInstructionCache(GetCurrentProcess(), b + site, 7);
          log_line("[d3d12_x] Skip intro: first phase IntroVideoPhase -> StartScreenPhase (+0x2F9972)\r\n");
        }
      } else {
        log_line("[d3d12_x] WARNING: skip-intro first-phase patch not applied (bytes differ)\r\n");
      }
      // The legal screen (boot image) stays up for at least 3.1 s: exe+0x21C695 builds the deadline as
      // now + 1000000 * 3.1 us and loops until it passes AND the boot is done. "mov eax,1000000" ->
      // "mov eax,0" keeps only the real loading wait.
      const uintptr_t legal = 0x21C695;
      const uint8_t expLegal[5] = { 0xB8, 0x40, 0x42, 0x0F, 0x00 };
      if (memcmp(b + legal, expLegal, 5) == 0) {
        DWORD op = 0;
        if (VirtualProtect(b + legal + 1, 4, PAGE_EXECUTE_READWRITE, &op)) {
          memset(b + legal + 1, 0, 4); VirtualProtect(b + legal + 1, 4, op, &op);
          FlushInstructionCache(GetCurrentProcess(), b + legal, 5);
          log_line("[d3d12_x] Skip intro: legal screen minimum time (3.1 s) removed (+0x21C695)\r\n");
        }
      } else {
        log_line("[d3d12_x] WARNING: legal-screen timer patch not applied (bytes differ)\r\n");
      }
    }
  }

  // 12. Patch presentation hooks (CreateSwapChainForCoreWindow redirect + Present early-exit bypass)
  PatchPresentationHooks(hExe);

  // 13. Patch RVA 0x0007E427: null-check rsi before put_Completed call
  // At 0x0007E427, if rsi is NULL, game unconditionally called [rsi + 0x30],
  // crashing with AV targeting 0x0. We replace the 25 bytes:
  //   test rsi, rsi
  //   jz 0x0007E440 (+0x14)
  //   mov r8, qword ptr [rsi]
  //   mov rdx, rax
  //   mov rcx, rsi
  //   call qword ptr [r8 + 0x30]
  //   nop x 7
  uint8_t *pAsyncCallSite = (uint8_t *)hExe + 0x0007E427;
  DWORD oldProtAsync = 0;
  if (VirtualProtect(pAsyncCallSite, 25, PAGE_EXECUTE_READWRITE, &oldProtAsync)) {
    if (pAsyncCallSite[0] == 0x4C && pAsyncCallSite[1] == 0x8B && pAsyncCallSite[2] == 0x06) {
      const uint8_t patchBytes[25] = {
          0x48, 0x85, 0xF6,                         // test rsi, rsi
          0x74, 0x14,                               // jz +0x14 (to 0x7E440)
          0x4C, 0x8B, 0x06,                         // mov r8, [rsi]
          0x48, 0x8B, 0xD0,                         // mov rdx, rax
          0x48, 0x8B, 0xCE,                         // mov rcx, rsi
          0x41, 0xFF, 0x50, 0x30,                   // call qword ptr [r8 + 0x30]
          0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90  // 7 x NOP
      };
      memcpy(pAsyncCallSite, patchBytes, 25);
      FlushInstructionCache(GetCurrentProcess(), pAsyncCallSite, 25);
      log_line("[d3d12_x] Patched 0x0007E427: null check on rsi before put_Completed\r\n");
    } else {
      log_line("[d3d12_x] WARNING: 0x0007E427 unexpected bytes (%02X %02X %02X)\r\n",
               pAsyncCallSite[0], pAsyncCallSite[1], pAsyncCallSite[2]);
    }
    VirtualProtect(pAsyncCallSite, 25, oldProtAsync, &oldProtAsync);
  }

  // 14. Patch PlatformInputXbox vtable CheckButton slot at RVA 0x013ED600
  uintptr_t inputVtableSlotRva = 0x013ED600;  // XBoxJoypad vtable 0x13ED5B8, slot 9 (1.80 added a slot before it)
  void **ppInputSlot = (void **)((uint8_t *)hExe + inputVtableSlotRva);
  DWORD oldProtInput = 0;
  if (VirtualProtect(ppInputSlot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtInput)) {
    *ppInputSlot = (void *)&DetourPlatformInputCheckButton;
    VirtualProtect(ppInputSlot, sizeof(void *), oldProtInput, &oldProtInput);
    log_line("[d3d12_x] Patched PlatformInputXbox vtable[8] at [hExe + 0x%08llX] -> %p\r\n",
             (unsigned long long)inputVtableSlotRva, &DetourPlatformInputCheckButton);
  }

  // 15. Detour StartScreenPhase::CheckInput at RVA 0x002F8FF0
  Install14ByteDetour((uint8_t *)hExe + 0x002F8FF0, (void *)&DetourStartScreenCheckInput);
  log_line("[d3d12_x] Patched 0x002F8FF0 (StartScreenPhase::CheckInput) detour -> %p\r\n",
           &DetourStartScreenCheckInput);

  // 16. (removed) [exe+0x1538940] is the loPlatformEvent flag set, not a PlayGo chunk bitmap;
  //     setting every bit faked eSessionInviteAccepted and skipped the intro video.
  //     ONRUSH_OLD_EVENTBITS=1 restores the old behaviour for comparison.
  if (InputEnvFlag("ONRUSH_OLD_EVENTBITS")) {
    uint32_t *pEv = (uint32_t *)((uint8_t *)hExe + 0x01538940);
    for (int i = 0; i < 8; i++) pEv[i] = 0xFFFFFFFF;
    log_line("[d3d12_x] ONRUSH_OLD_EVENTBITS: platform event bits forced to 0xFFFFFFFF\r\n");
  }

  // 17. Patch 0x00098C90 (JobQueue::IsBusy) -> 31 C0 C3 90 90 90 90 (xor eax, eax; ret; 4 x NOP)
  // This allows phase unloading/transition evaluator (0x0021D1D0) and background asset loaders (0x0021BB0D)
  // to proceed past busy queue wait checks.
  // ONRUSH_REAL_JOBBUSY=1 keeps the original IsBusy: the phase code (exe+0x21C48D) only runs
  // queued jobs (exe+0x98F20) while IsBusy is true, so with the patch the main-menu phase
  // jobs queued after the EULA never run.
  uint8_t *pJobBusy = (uint8_t *)hExe + 0x00098C90;
  DWORD oldProtJob = 0;
  if (InputEnvFlag("ONRUSH_REAL_JOBBUSY")) {
    log_line("[d3d12_x] ONRUSH_REAL_JOBBUSY: keeping original JobQueue::IsBusy\r\n");
  } else if (VirtualProtect(pJobBusy, 7, PAGE_EXECUTE_READWRITE, &oldProtJob)) {
    pJobBusy[0] = 0x31; // xor eax, eax
    pJobBusy[1] = 0xC0;
    pJobBusy[2] = 0xC3; // ret
    pJobBusy[3] = 0x90; // nop
    pJobBusy[4] = 0x90;
    pJobBusy[5] = 0x90;
    pJobBusy[6] = 0x90;
    VirtualProtect(pJobBusy, 7, oldProtJob, &oldProtJob);
    FlushInstructionCache(GetCurrentProcess(), pJobBusy, 7);
    log_line("[d3d12_x] Patched 0x00098C90 (JobQueue::IsBusy) -> returns 0\r\n");
  }

  // 18. Patch RVA 0x001E3830 (Page Element Cleanup null guard trampoline)
  {
    uint8_t *pTarget = (uint8_t *)hExe + 0x001E3830;
    DWORD oldProtTramp = 0;
    if (VirtualProtect(g_PageElementCleanupTrampoline,
                       sizeof(g_PageElementCleanupTrampoline),
                       PAGE_EXECUTE_READWRITE, &oldProtTramp)) {
      memcpy(g_PageElementCleanupTrampoline, pTarget, 17);
      uint8_t *pTrampJump = g_PageElementCleanupTrampoline + 17;
      pTrampJump[0] = 0xFF;
      pTrampJump[1] = 0x25;
      *(uint32_t *)(pTrampJump + 2) = 0;
      *(uint64_t *)(pTrampJump + 6) = (uint64_t)(pTarget + 17);
      FlushInstructionCache(GetCurrentProcess(), g_PageElementCleanupTrampoline,
                            sizeof(g_PageElementCleanupTrampoline));
      g_TruePageElementCleanup =
          (PFN_PageElementCleanup)(void *)g_PageElementCleanupTrampoline;

      DWORD oldProtTarget = 0;
      if (VirtualProtect(pTarget, 17, PAGE_EXECUTE_READWRITE, &oldProtTarget)) {
        pTarget[0] = 0xFF;
        pTarget[1] = 0x25;
        *(uint32_t *)(pTarget + 2) = 0;
        *(uint64_t *)(pTarget + 6) = (uint64_t)&DetourPageElementCleanup;
        pTarget[14] = 0x90;
        pTarget[15] = 0x90;
        pTarget[16] = 0x90;
        VirtualProtect(pTarget, 17, oldProtTarget, &oldProtTarget);
        FlushInstructionCache(GetCurrentProcess(), pTarget, 17);
        log_line("[d3d12_x] Patched 0x001E3830 (PageElementCleanup) with "
                 "null-guard trampoline -> %p\r\n",
                 &DetourPageElementCleanup);
      }
    }
  }

  // 19. Detour RVA 0x0023FBF0 (VideoPlayer::StartVideo) with synchronous open
  Install14ByteDetour((uint8_t *)hExe + 0x0023FBF0, (void *)&DetourStartVideo);
  log_line("[d3d12_x] Patched 0x0023FBF0 (VideoPlayer::StartVideo) detour -> %p\r\n",
           &DetourStartVideo);


}

// ============================================================================
// Direct3D 12 Descriptor Heap ABI Adapters
// ============================================================================
// On Xbox One Durango ERA, GetCPUDescriptorHandleForHeapStart and
// GetGPUDescriptorHandleForHeapStart return the 64-bit handle directly in RAX.
// On desktop Windows x64 d3d12.dll, the COM interface expects RDX to be a
// pointer to a return struct and writes to [RDX]. We adapt between the two ABIs
// to prevent access violations.

typedef D3D12_CPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *PFN_TrueGetCPUHandle)(
    ID3D12DescriptorHeap *, D3D12_CPU_DESCRIPTOR_HANDLE *);
typedef D3D12_GPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *PFN_TrueGetGPUHandle)(
    ID3D12DescriptorHeap *, D3D12_GPU_DESCRIPTOR_HANDLE *);
static PFN_TrueGetCPUHandle g_TrueGetCPUHandle = nullptr;
static PFN_TrueGetGPUHandle g_TrueGetGPUHandle = nullptr;
static SRWLOCK g_HeapDescLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12DescriptorHeap *, D3D12_DESCRIPTOR_HEAP_DESC>
    g_HeapDescMap;

static bool TryGetModuleRva(HMODULE hMod, const void *addr, uintptr_t *outRva) {
  if (!hMod || !addr || !outRva)
    return false;

  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(hMod);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;

  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      reinterpret_cast<const uint8_t *>(hMod) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return false;

  uintptr_t uAddr = reinterpret_cast<uintptr_t>(addr);
  uintptr_t uModBase = reinterpret_cast<uintptr_t>(hMod);
  uintptr_t uModSize = static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);

  if (uAddr >= uModBase && uAddr < uModBase + uModSize) {
    *outRva = uAddr - uModBase;
    return true;
  }
  return false;
}

static bool IsHeapShaderVisible(ID3D12DescriptorHeap *pHeap) {
  if (!pHeap) {
    log_line("[d3d12_x] IsHeapShaderVisible: pHeap is nullptr, assuming NOT "
             "shader-visible\r\n");
    return false;
  }

  AcquireSRWLockShared(&g_HeapDescLock);
  auto it = g_HeapDescMap.find(pHeap);
  bool found = (it != g_HeapDescMap.end());
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  if (found) {
    desc = it->second;
  }
  ReleaseSRWLockShared(&g_HeapDescLock);

  if (!found) {
    log_line("[d3d12_x] IsHeapShaderVisible: unknown heap %p (description not "
             "recorded), assuming NOT shader-visible\r\n",
             pHeap);
    return false;
  }

  return (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
}

static uint64_t STDMETHODCALLTYPE
WrappedGetCPUDescriptorHandle(ID3D12DescriptorHeap *pHeap) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
  if (g_TrueGetCPUHandle)
    g_TrueGetCPUHandle(pHeap, &handle);
  log_line("[d3d12_x] WrappedGetCPUDescriptorHandle: pHeap=%p -> 0x%016llX\r\n",
           pHeap, (unsigned long long)handle.ptr);
  return (uint64_t)handle.ptr;
}

static uint64_t STDMETHODCALLTYPE
WrappedGetGPUDescriptorHandle(ID3D12DescriptorHeap *pHeap) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva = 0;
  bool hasRva = TryGetModuleRva(hExe, retAddr, &rva);

  if (!pHeap) {
    if (hasRva) {
      log_line("[d3d12_x] WrappedGetGPUDescriptorHandle: REJECTED: pHeap is "
               "nullptr; caller=%p (game+0x%llX); skipping native call and "
               "returning 0\r\n",
               retAddr, (unsigned long long)rva);
    } else {
      log_line("[d3d12_x] WrappedGetGPUDescriptorHandle: REJECTED: pHeap is "
               "nullptr; caller=%p; skipping native call and returning 0\r\n",
               retAddr);
    }
    return 0;
  }

  AcquireSRWLockShared(&g_HeapDescLock);
  auto it = g_HeapDescMap.find(pHeap);
  bool found = (it != g_HeapDescMap.end());
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  if (found) {
    desc = it->second;
  }
  ReleaseSRWLockShared(&g_HeapDescLock);

  if (!found) {
    if (hasRva) {
      log_line("[d3d12_x] WrappedGetGPUDescriptorHandle: REJECTED: pHeap=%p is "
               "unknown (description not recorded); caller=%p (game+0x%llX); "
               "skipping native call and returning 0\r\n",
               pHeap, retAddr, (unsigned long long)rva);
    } else {
      log_line("[d3d12_x] WrappedGetGPUDescriptorHandle: REJECTED: pHeap=%p is "
               "unknown (description not recorded); caller=%p; skipping native "
               "call and returning 0\r\n",
               pHeap, retAddr);
    }
    return 0;
  }

  if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
    if (hasRva) {
      log_line(
          "[d3d12_x] WrappedGetGPUDescriptorHandle: REJECTED: pHeap=%p is NOT "
          "shader-visible (Type=%d, NumDesc=%u, Flags=0x%X, NodeMask=0x%X); "
          "caller=%p (game+0x%llX); skipping native call and returning 0\r\n",
          pHeap, desc.Type, desc.NumDescriptors, (UINT)desc.Flags,
          desc.NodeMask, retAddr, (unsigned long long)rva);
    } else {
      log_line(
          "[d3d12_x] WrappedGetGPUDescriptorHandle: REJECTED: pHeap=%p is NOT "
          "shader-visible (Type=%d, NumDesc=%u, Flags=0x%X, NodeMask=0x%X); "
          "caller=%p; skipping native call and returning 0\r\n",
          pHeap, desc.Type, desc.NumDescriptors, (UINT)desc.Flags,
          desc.NodeMask, retAddr);
    }
    return 0;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE handle = {};
  if (g_TrueGetGPUHandle)
    g_TrueGetGPUHandle(pHeap, &handle);
  log_line("[d3d12_x] WrappedGetGPUDescriptorHandle: pHeap=%p -> 0x%016llX\r\n",
           pHeap, (unsigned long long)handle.ptr);
  return (uint64_t)handle.ptr;
}

static void *g_WrappedDescriptorHeapVtable[16] = {};

static void WrapDescriptorHeap(void **ppHeap) {
  if (!ppHeap || !*ppHeap)
    return;
  void **origVtable = *(void ***)(*ppHeap);
  if (!g_TrueGetCPUHandle) {
    for (int i = 0; i < 16; ++i)
      g_WrappedDescriptorHeapVtable[i] = origVtable[i];
    g_TrueGetCPUHandle = (PFN_TrueGetCPUHandle)origVtable[9];
    g_TrueGetGPUHandle = (PFN_TrueGetGPUHandle)origVtable[10];
    g_WrappedDescriptorHeapVtable[9] = (void *)&WrappedGetCPUDescriptorHandle;
    g_WrappedDescriptorHeapVtable[10] = (void *)&WrappedGetGPUDescriptorHandle;
  }
  *(void ***)(*ppHeap) = g_WrappedDescriptorHeapVtable;
  log_line(
      "[d3d12_x] Patched ID3D12DescriptorHeap %p vtable with ABI adapters\r\n",
      *ppHeap);
}

static ID3D12Resource *g_StagingBuffers36[8] = {};
static int g_NumStagingBuffers36 = 0;
static ID3D12Resource *g_TrackedUavBuffers[64] = {};
static int g_NumTrackedUavBuffers = 0;
static ID3D12Resource *g_pIndexBuffer24 = nullptr;

static bool IsUavBuffer(ID3D12Resource *pRes) {
  if (!pRes)
    return false;
  for (int i = 0; i < g_NumTrackedUavBuffers; ++i) {
    if (g_TrackedUavBuffers[i] == pRes)
      return true;
  }
  D3D12_RESOURCE_DESC desc = pRes->GetDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
    return true;
  }
  return false;
}

static bool IsStagingBuffer(ID3D12Resource *pRes) {
  for (int i = 0; i < g_NumStagingBuffers36; ++i) {
    if (g_StagingBuffers36[i] == pRes)
      return true;
  }
  return false;
}

static SRWLOCK g_UploadResLock = SRWLOCK_INIT;
static std::unordered_set<ID3D12Resource*> g_UploadResources;

void TrackUploadResource(ID3D12Resource* pRes) {
  if (!pRes) return;
  AcquireSRWLockExclusive(&g_UploadResLock);
  g_UploadResources.insert(pRes);
  ReleaseSRWLockExclusive(&g_UploadResLock);
}

bool IsUploadResource(ID3D12Resource* pRes) {
  if (!pRes) return false;
  AcquireSRWLockShared(&g_UploadResLock);
  bool found = (g_UploadResources.find(pRes) != g_UploadResources.end());
  ReleaseSRWLockShared(&g_UploadResLock);
  return found;
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_ResourceMap)(ID3D12Resource *, UINT,
                                                    const D3D12_RANGE *,
                                                    void **);
typedef void(STDMETHODCALLTYPE *PFN_ResourceUnmap)(ID3D12Resource *, UINT,
                                                   const D3D12_RANGE *);

static PFN_ResourceMap g_TrueResourceMap = nullptr;
static PFN_ResourceUnmap g_TrueResourceUnmap = nullptr;

static HRESULT STDMETHODCALLTYPE
HookedResourceMap(ID3D12Resource *pRes, UINT Subresource,
                  const D3D12_RANGE *pReadRange, void **ppData) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  HRESULT hr = g_TrueResourceMap
                   ? g_TrueResourceMap(pRes, Subresource, pReadRange, ppData)
                   : E_FAIL;
  void *mappedPtr = (ppData && SUCCEEDED(hr)) ? *ppData : nullptr;

  if (IsStagingBuffer(pRes)) {
    log_line("[d3d12_x] STAGING_36B Resource::Map(%p, Sub=%u, pReadRange=%p, "
             "ppData=%p) caller=game+0x%llX -> hr=0x%08lX, *ppData=%p\r\n",
             pRes, Subresource, pReadRange, ppData, (unsigned long long)rva,
             (unsigned long)hr, mappedPtr);
  } else {
    log_line("[d3d12_x] Resource::Map(%p, Sub=%u, pReadRange=%p, ppData=%p) "
             "caller=game+0x%llX -> hr=0x%08lX, *ppData=%p\r\n",
             pRes, Subresource, pReadRange, ppData, (unsigned long long)rva,
             (unsigned long)hr, mappedPtr);
  }

  return hr;
}

static void STDMETHODCALLTYPE HookedResourceUnmap(
    ID3D12Resource *pRes, UINT Subresource, const D3D12_RANGE *pWrittenRange) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  if (IsStagingBuffer(pRes)) {
    log_line(
        "[d3d12_x] STAGING_36B Resource::Unmap(%p, Sub=%u, pWrittenRange=%p) "
        "caller=game+0x%llX (memset 36B completed between Map and Unmap)\r\n",
        pRes, Subresource, pWrittenRange, (unsigned long long)rva);
  } else {
    log_line("[d3d12_x] Resource::Unmap(%p, Sub=%u, pWrittenRange=%p) "
             "caller=game+0x%llX\r\n",
             pRes, Subresource, pWrittenRange, (unsigned long long)rva);
  }

  if (g_TrueResourceUnmap)
    g_TrueResourceUnmap(pRes, Subresource, pWrittenRange);
}
typedef D3D12_RESOURCE_ALLOCATION_INFO *(
    STDMETHODCALLTYPE *PFN_GetResourceAllocationInfo)(
    ID3D12Device *pDevice, D3D12_RESOURCE_ALLOCATION_INFO *RetVal,
    UINT visibleMask, UINT numResourceDescs,
    const D3D12_RESOURCE_DESC *pResourceDescs);
static PFN_GetResourceAllocationInfo g_TrueGetResourceAllocationInfo = nullptr;

typedef D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE *PFN_GetGPUVirtualAddress)(
    ID3D12Resource *);
static PFN_GetGPUVirtualAddress g_TrueGetGPUVirtualAddress = nullptr;
static void *g_WrappedResourceVtable[32] = {};

static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
HookedResourceGetGPUVirtualAddress(ID3D12Resource *pRes) {
  D3D12_GPU_VIRTUAL_ADDRESS va = 0;
  if (g_TrueGetGPUVirtualAddress)
    va = g_TrueGetGPUVirtualAddress(pRes);

  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  if (IsStagingBuffer(pRes)) {
    log_line("[d3d12_x] STAGING_36B Resource::GetGPUVirtualAddress(%p) "
             "caller=game+0x%llX -> 0x%016llX\r\n",
             pRes, (unsigned long long)rva, (unsigned long long)va);
  } else {
    log_line("[d3d12_x] Resource::GetGPUVirtualAddress(%p) caller=game+0x%llX "
             "-> 0x%016llX\r\n",
             pRes, (unsigned long long)rva, (unsigned long long)va);
  }

  // Gated diagnostic bypass: Specifically intercept the UAV buffer zeroing
  // memset at game+0xBE0C8. NOTE: This scratch buffer receives the memset to
  // avoid CPU Access Violation on GPU VA. This does NOT initialize the D3D12
  // resource memory; it is strictly a diagnostic progression bypass!
  if (rva == 0xBE0C8 && IsUavBuffer(pRes)) {
    D3D12_RESOURCE_DESC desc = pRes->GetDesc();
    D3D12_RESOURCE_ALLOCATION_INFO allocInfo = {};
    if (g_pMainDevice && g_TrueGetResourceAllocationInfo) {
      g_TrueGetResourceAllocationInfo(g_pMainDevice, &allocInfo, 0, 1, &desc);
    }
    size_t sizeInBytes = (allocInfo.SizeInBytes > 0)
                             ? (size_t)allocInfo.SizeInBytes
                             : (size_t)desc.Width;
    if (sizeInBytes < 65536)
      sizeInBytes =
          65536; // Minimum buffer allocation alignment on desktop D3D12

    static void *s_uavClearingScratchBuffer = nullptr;
    static const size_t s_uavClearingScratchCapacity =
        16 * 1024 * 1024; // 16 MiB persistent capacity
    if (!s_uavClearingScratchBuffer) {
      s_uavClearingScratchBuffer =
          VirtualAlloc(nullptr, s_uavClearingScratchCapacity,
                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      log_line("[d3d12_x] Allocated persistent %zu-byte scratch buffer for UAV "
               "buffer clearing: %p\r\n",
               s_uavClearingScratchCapacity, s_uavClearingScratchBuffer);
    }

    if (!s_uavClearingScratchBuffer ||
        sizeInBytes > s_uavClearingScratchCapacity) {
      log_line("[d3d12_x] CRITICAL DIAGNOSTIC ERROR: Cannot provide adequate "
               "CPU scratch buffer for UAV zeroing!\r\n"
               "  ScratchBuffer=%p, ScratchCapacity=%zu, RequiredSize=%zu\r\n",
               s_uavClearingScratchBuffer, s_uavClearingScratchCapacity,
               sizeInBytes);
      return va; // Return native VA without altering; abort safely
    }

    log_line("[d3d12_x] DIAGNOSTIC BYPASS: Gated UAV buffer "
             "GetGPUVirtualAddress for caller=game+0x%llX:\r\n"
             "  Resource=%p, Dim=%d, Width=%llu, Flags=0x%X, SizeInBytes=%zu, "
             "Native GPU VA=0x%016llX, CPU Scratch Ptr=%p\r\n"
             "  NOTE: Scratch memory receives memset zeros. The D3D12 resource "
             "is NOT directly initialized by this CPU write.\r\n",
             (unsigned long long)rva, pRes, desc.Dimension,
             (unsigned long long)desc.Width, desc.Flags, sizeInBytes,
             (unsigned long long)va, s_uavClearingScratchBuffer);
    return (D3D12_GPU_VIRTUAL_ADDRESS)s_uavClearingScratchBuffer;
  }

  if (va == 0) {
    // On Xbox One Durango ERA, textures in placed/title memory have a CPU/GPU
    // accessible virtual address. The game calls Slot 11 to obtain the memory
    // address and immediately executes memset(va, 0, SizeInBytes). On Desktop
    // D3D12, textures return 0. We provide an allocated clearing buffer so
    // memset succeeds safely!
    static void *s_dummyClearBuffer = nullptr;
    static size_t s_dummyClearBufferSize = 0;
    size_t required = 32 * 1024 * 1024; // 32MB safe scratch buffer
    if (!s_dummyClearBuffer) {
      s_dummyClearBuffer = VirtualAlloc(
          nullptr, required, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      s_dummyClearBufferSize = required;
      log_line("[d3d12_x] Allocated dummy texture clearing buffer: %p "
               "(size=%zu)\r\n",
               s_dummyClearBuffer, s_dummyClearBufferSize);
    }
    return (D3D12_GPU_VIRTUAL_ADDRESS)s_dummyClearBuffer;
  }
  return va;
}

// ============================================================================
// Diagnostic Resource Tracking (Lock-protected set of returned resource
// pointers)
// ============================================================================
static SRWLOCK g_ResourceTrackerLock = SRWLOCK_INIT;
static std::unordered_set<void *> g_ReturnedResources;

static void RegisterReturnedResource(void *pResource) {
  if (!pResource)
    return;
  AcquireSRWLockExclusive(&g_ResourceTrackerLock);
  g_ReturnedResources.insert(pResource);
  ReleaseSRWLockExclusive(&g_ResourceTrackerLock);
}

static bool IsKnownReturnedResource(void *ptr) {
  if (!ptr)
    return false;
  AcquireSRWLockShared(&g_ResourceTrackerLock);
  bool found = (g_ReturnedResources.find(ptr) != g_ReturnedResources.end());
  ReleaseSRWLockShared(&g_ResourceTrackerLock);
  return found;
}

typedef ULONG(STDMETHODCALLTYPE *PFN_ResRelease)(ID3D12Resource *);
static PFN_ResRelease g_TrueResRelease = nullptr;
static PFN_ResRelease g_TrueResAddRef = nullptr;
// Deferred release: on Xbox the game owns the memory behind a placed resource, so dropping the last
// reference while the GPU still reads it is harmless there. Here the resource is a committed one and the
// memory goes away at once -> GPU page fault -> device removed (seen when the race-prep screen swaps cars).
// The last Release is held back until ~10 frames and 1 s have passed. ONRUSH_DEFER_RELEASE=0 turns it off.
struct DeferredRes { ID3D12Resource *res; UINT frame; ULONGLONG tick; };
static SRWLOCK g_deferLock = SRWLOCK_INIT;
static std::vector<DeferredRes> g_deferred;
static volatile LONG g_deferFrame = 0;
static volatile LONG g_deferTotal = 0;
static bool DeferReleaseOn() {
  static int s = -1;
  if (s < 0) { char b[8] = {}; s = (GetEnvironmentVariableA("ONRUSH_DEFER_RELEASE", b, sizeof(b)) > 0 && b[0] == '0') ? 0 : 1; }
  return s == 1;
}
static ULONG STDMETHODCALLTYPE HookedResourceRelease(ID3D12Resource *This) {
  static const bool leak = GetEnvironmentVariableA("ONRUSH_LEAK_RESOURCES", nullptr, 0) > 0;
  if (leak) return 2;   // experiment: keep every resource alive so command lists never reference freed objects
  if (!g_TrueResRelease) return 0;
  if (DeferReleaseOn() && g_TrueResAddRef) {
    g_TrueResAddRef(This);
    ULONG c = g_TrueResRelease(This);   // current count, unchanged
    if (c == 1) {                       // this Release would destroy it: park it instead
      AcquireSRWLockExclusive(&g_deferLock);
      g_deferred.push_back({ This, (UINT)g_deferFrame, GetTickCount64() });
      ReleaseSRWLockExclusive(&g_deferLock);
      InterlockedIncrement(&g_deferTotal);
      return 0;
    }
  }
  return g_TrueResRelease(This);
}
// Called once per Present.
static void DeferReleaseTick() {
  LONG f = InterlockedIncrement(&g_deferFrame);
  if (!g_TrueResRelease) return;
  std::vector<ID3D12Resource *> drop;
  ULONGLONG now = GetTickCount64();
  AcquireSRWLockExclusive(&g_deferLock);
  size_t keep = 0;
  for (size_t i = 0; i < g_deferred.size(); ++i) {
    const DeferredRes &d = g_deferred[i];
    if ((LONG)(f - (LONG)d.frame) >= 10 && now - d.tick >= 1000) drop.push_back(d.res);
    else g_deferred[keep++] = d;
  }
  g_deferred.resize(keep);
  size_t pending = keep;
  ReleaseSRWLockExclusive(&g_deferLock);
  for (ID3D12Resource *r : drop) g_TrueResRelease(r);
  if ((f % 600) == 0) log_line("[defer] frame %ld: %zu resource(s) waiting, %ld deferred so far\r\n", (long)f, pending, (long)g_deferTotal);
  { static LONG s_lastSkips = 0; LONG s = g_badXformSkips;
    if (s != s_lastSkips) { log_line("[race] exe+0xFD7E0: %ld render item walk(s) cut short on a bad item so far (frame %ld)\r\n", (long)s, (long)f); s_lastSkips = s; } }
  { static LONG s_lastHub = 0; LONG s = g_hubNullSkips;
    if (s != s_lastHub) { log_line("[race] exe+0x3B0D2E: %ld hub widget update(s) skipped (null) so far (frame %ld)\r\n", (long)s, (long)f); s_lastHub = s; } }
}

static void WrapResource(void **ppResource) {
  if (!ppResource || !*ppResource)
    return;
  void **origVtable = *(void ***)(*ppResource);
  if (!g_TrueGetGPUVirtualAddress) {
    for (int i = 0; i < 32; ++i)
      g_WrappedResourceVtable[i] = origVtable[i];
    g_TrueResourceMap = (PFN_ResourceMap)origVtable[8];
    g_TrueResourceUnmap = (PFN_ResourceUnmap)origVtable[9];
    g_TrueGetGPUVirtualAddress = (PFN_GetGPUVirtualAddress)origVtable[11];
    g_TrueResRelease = (PFN_ResRelease)origVtable[2];
    g_TrueResAddRef = (PFN_ResRelease)origVtable[1];
    g_WrappedResourceVtable[2] = (void *)&HookedResourceRelease;
    g_WrappedResourceVtable[8] = (void *)&HookedResourceMap;
    g_WrappedResourceVtable[9] = (void *)&HookedResourceUnmap;
    g_WrappedResourceVtable[11] = (void *)&HookedResourceGetGPUVirtualAddress;
  }
  *(void ***)(*ppResource) = g_WrappedResourceVtable;
  log_line("[d3d12_x] Wrapped ID3D12Resource %p vtable (Slots 8 Map, 9 Unmap, "
           "11 GetGPUVirtualAddress fallback)\r\n",
           *ppResource);
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateDescriptorHeap)(
    ID3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid,
    void **ppvHeap);
static PFN_CreateDescriptorHeap g_TrueCreateDescriptorHeap = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommandQueue)(
    ID3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid,
    void **ppCommandQueue);
static PFN_CreateCommandQueue g_TrueCreateCommandQueue = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommandAllocator)(
    ID3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE type, REFIID riid,
    void **ppCommandAllocator);
static PFN_CreateCommandAllocator g_TrueCreateCommandAllocator = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateGraphicsPipelineState)(
    ID3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc,
    REFIID riid, void **ppPipelineState);
static PFN_CreateGraphicsPipelineState g_TrueCreateGraphicsPipelineState =
    nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateComputePipelineState)(
    ID3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc,
    REFIID riid, void **ppPipelineState);
static PFN_CreateComputePipelineState g_TrueCreateComputePipelineState =
    nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommandList)(
    ID3D12Device *pDevice, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *pCommandAllocator,
    ID3D12PipelineState *pInitialState, REFIID riid, void **ppCommandList);
static PFN_CreateCommandList g_TrueCreateCommandList = nullptr;

typedef void(STDMETHODCALLTYPE *PFN_CreateSampler)(
    ID3D12Device *pDevice, const D3D12_SAMPLER_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
static PFN_CreateSampler g_TrueCreateSampler = nullptr;

typedef D3D12_HEAP_PROPERTIES *(STDMETHODCALLTYPE *PFN_GetCustomHeapProperties)(
    ID3D12Device *pDevice, D3D12_HEAP_PROPERTIES *RetVal, UINT nodeMask,
    D3D12_HEAP_TYPE heapType);
static PFN_GetCustomHeapProperties g_TrueGetCustomHeapProperties = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommittedResource)(
    ID3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource,
    void **ppvResource);
static PFN_CreateCommittedResource g_TrueCreateCommittedResource = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommandSignature)(
    ID3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc,
    ID3D12RootSignature *pRootSignature, REFIID riid,
    void **ppvCommandSignature);
static PFN_CreateCommandSignature g_TrueCreateCommandSignature = nullptr;

static void *g_WrappedDeviceVtable[256] = {};

// ---- descriptor tracking hooks (diagnostic) ----
typedef void(STDMETHODCALLTYPE *PFN_CreateSRV)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CreateUAV)(ID3D12Device *, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CopyDescriptorsSimple)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
typedef void(STDMETHODCALLTYPE *PFN_CopyDescriptors)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
static PFN_CreateSRV g_TrueCreateSRV = nullptr;
static PFN_CreateUAV g_TrueCreateUAV = nullptr;
static PFN_CopyDescriptorsSimple g_TrueCopyDescriptorsSimple = nullptr;
static PFN_CopyDescriptors g_TrueCopyDescriptors = nullptr;

static void STDMETHODCALLTYPE HookedCreateSRV(ID3D12Device *This, ID3D12Resource *pRes, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  if (pRes && dest.ptr > 0x10000)
    DescTrack_Set(dest.ptr, { pRes, pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN, pDesc ? (UINT)pDesc->ViewDimension : 0, 1 });
  { static const bool s_log = GetEnvironmentVariableA("ONRUSH_LOG_SRVBUF", nullptr, 0) > 0; static volatile LONG s_n = 0;
    static volatile LONG s_nt = 0;
    if (s_log && pDesc && pDesc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER && pDesc->Shader4ComponentMapping != 0x1688u && InterlockedIncrement(&s_nt) <= 400)
      log_line("[srvtex] res=%p fmt=%d dim=%d map=0x%X\r\n", (void*)pRes, (int)pDesc->Format, (int)pDesc->ViewDimension, (unsigned)pDesc->Shader4ComponentMapping);
    if (s_log && pRes && pDesc && pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && InterlockedIncrement(&s_n) <= 20000)
      log_line("[srvbuf] res=%p fmt=%d first=%llu n=%u stride=%u flags=%u dest=%llX\r\n", (void*)pRes, (int)pDesc->Format, (unsigned long long)pDesc->Buffer.FirstElement, pDesc->Buffer.NumElements, pDesc->Buffer.StructureByteStride, (unsigned)pDesc->Buffer.Flags, (unsigned long long)dest.ptr); }
  { static volatile LONG s_n3 = 0;
    if (pDesc && pDesc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D && InterlockedIncrement(&s_n3) <= 200)
      log_line("[srv3d] res=%p fmt=%d mip=%u nmip=%u map=0x%X clamp=%g dest=%llX\r\n", (void*)pRes, (int)pDesc->Format, pDesc->Texture3D.MostDetailedMip, pDesc->Texture3D.MipLevels, (unsigned)pDesc->Shader4ComponentMapping, pDesc->Texture3D.ResourceMinLODClamp, (unsigned long long)dest.ptr); }
  if (g_TrueCreateSRV) g_TrueCreateSRV(This, pRes, pDesc, dest);
}
// ---- engine-owned UAV counter buffer (replaces GCN hidden counters / IncrementCounter, which hangs the GPU)
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateRootSignature)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);
static PFN_CreateRootSignature g_TrueCreateRootSignature = nullptr;
#include "pso_disk_cache.h"
// Creates the root signature and remembers its exact blob (the PSO disk cache keys pipelines by it).
static HRESULT CreateRootSignatureNoted(ID3D12Device *dev, UINT nodeMask, const void *blob, SIZE_T len, REFIID riid, void **ppv) {
  HRESULT hr = g_TrueCreateRootSignature(dev, nodeMask, blob, len, riid, ppv);
  if (SUCCEEDED(hr) && ppv && *ppv) psodc::NoteRootSignature(*ppv, blob, len);
  return hr;
}
static std::unordered_map<void*, UINT> g_RsExtraParam; static SRWLOCK g_RsLock = SRWLOCK_INIT;
ID3D12Resource* g_CtrBuf = nullptr;
UINT LookupRsExtraParam(void* rs) { AcquireSRWLockShared(&g_RsLock); auto it = g_RsExtraParam.find(rs); UINT v = (it == g_RsExtraParam.end()) ? 0xFFFFFFFFu : it->second; ReleaseSRWLockShared(&g_RsLock); return v; }
static void EnsureCtrBuf(ID3D12Device* dev) {
  if (g_CtrBuf || !dev) return;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 0x100000ull + 4096ull * 2304ull * 4ull; /* [0,32K) counters reset per frame, [32K,64K) debug, 0x80000 photo token/echo, [1MB,..) photo pixels (stride 4096 px) */ bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&g_CtrBuf);
  log_line("[ctr] counter buffer %p va=%llX\r\n", (void*)g_CtrBuf, g_CtrBuf ? (unsigned long long)g_CtrBuf->GetGPUVirtualAddress() : 0ull);
}
static void RecordRsCbvSlots(void* rs, const D3D12_ROOT_SIGNATURE_DESC* d);
static HRESULT STDMETHODCALLTYPE HookedCreateRootSignature(ID3D12Device *dev, UINT nodeMask, const void *blob, SIZE_T len, REFIID riid, void **ppv) {
  static const bool s_off = GetEnvironmentVariableA("ONRUSH_NO_CTR_PARAM", nullptr, 0) > 0;
  if (!s_off && blob && len) {
    typedef HRESULT (WINAPI *PFN_Deser)(LPCVOID, SIZE_T, REFIID, void**); typedef HRESULT (WINAPI *PFN_Ser)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    static PFN_Deser pDeser = nullptr; static PFN_Ser pSer = nullptr;
    if (!pDeser) { HMODULE h = GetModuleHandleA("d3d12.dll"); if (!h) h = LoadLibraryA("d3d12.dll"); if (h) { pDeser = (PFN_Deser)GetProcAddress(h, "D3D12CreateRootSignatureDeserializer"); pSer = (PFN_Ser)GetProcAddress(h, "D3D12SerializeRootSignature"); } }
    ID3D12RootSignatureDeserializer* des = nullptr;
    if (pDeser && pSer && SUCCEEDED(pDeser(blob, len, __uuidof(ID3D12RootSignatureDeserializer), (void**)&des)) && des) {
      const D3D12_ROOT_SIGNATURE_DESC* d = des->GetRootSignatureDesc();
      if (d) {
        std::vector<D3D12_ROOT_PARAMETER> params(d->pParameters, d->pParameters + d->NumParameters);
        D3D12_ROOT_PARAMETER extra = {}; extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; extra.Descriptor.ShaderRegister = 200; extra.Descriptor.RegisterSpace = 99; extra.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(extra);
        D3D12_ROOT_SIGNATURE_DESC nd = *d; nd.NumParameters = (UINT)params.size(); nd.pParameters = params.data();
        ID3DBlob* out = nullptr; ID3DBlob* err = nullptr;
        if (SUCCEEDED(pSer(&nd, D3D_ROOT_SIGNATURE_VERSION_1, &out, &err)) && out) {
          HRESULT hr = CreateRootSignatureNoted(dev, nodeMask, out->GetBufferPointer(), out->GetBufferSize(), riid, ppv);
          if (SUCCEEDED(hr) && ppv && *ppv) { AcquireSRWLockExclusive(&g_RsLock); g_RsExtraParam[*ppv] = d->NumParameters; ReleaseSRWLockExclusive(&g_RsLock); EnsureCtrBuf(dev); RecordRsCbvSlots(*ppv, d);
            static volatile LONG s_n = 0; if (InterlockedIncrement(&s_n) <= 10) log_line("[ctr] root signature %p extended: counter UAV at param %u\r\n", *ppv, d->NumParameters); }
          out->Release(); if (err) err->Release(); des->Release();
          if (SUCCEEDED(hr)) return hr;
          return CreateRootSignatureNoted(dev, nodeMask, blob, len, riid, ppv);
        } else { if (err) { log_line("[ctr] serialize failed: %s\r\n", (const char*)err->GetBufferPointer()); err->Release(); } }
      }
      des->Release();
    }
  }
  return CreateRootSignatureNoted(dev, nodeMask, blob, len, riid, ppv);
}
// Per-frame counter reset on the game's direct queue (CopyBufferRegion from a zero buffer).
ID3D12CommandQueue* g_LastDirectQueue = nullptr;
static std::set<std::pair<ID3D12Resource*, UINT64>> g_CtrLocs; static SRWLOCK g_CtrLocLock = SRWLOCK_INIT;
static void RecordCtrLoc(ID3D12Resource* r, UINT64 off) { if (!r) return; AcquireSRWLockExclusive(&g_CtrLocLock); g_CtrLocs.insert({ r, off }); ReleaseSRWLockExclusive(&g_CtrLocLock); }
static void PhotoTokenCopy(ID3D12GraphicsCommandList* l, UINT slot);   // photo_capture.h
static void ResetCountersOnQueue() {
  static ID3D12Resource* zero = nullptr; static ID3D12CommandAllocator* alloc[4] = {}; static ID3D12GraphicsCommandList* list[4] = {}; static ID3D12Fence* fence = nullptr; static UINT64 fv = 0; static UINT idx = 0;
  if (!g_CtrBuf || !g_LastDirectQueue || !g_pMainDevice) return;
  if (!zero) { D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&zero); g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence); }
  if (!zero || !fence) return;
  UINT i = idx++ & 3;
  if (!alloc[i]) { g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc[i]); g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&list[i]); list[i]->Close(); }
  if (fv >= 4 && fence->GetCompletedValue() < fv - 3) { HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(fv - 3, ev); WaitForSingleObject(ev, 2000); CloseHandle(ev); }
  alloc[i]->Reset(); list[i]->Reset(alloc[i], nullptr); list[i]->CopyBufferRegion(g_CtrBuf, 0, zero, 0, 32768);
  PhotoTokenCopy(list[i], i);
  { static const bool s_all = GetEnvironmentVariableA("ONRUSH_CTR_RESET_ALL", nullptr, 0) > 0;
    if (s_all) { AcquireSRWLockShared(&g_CtrLocLock); for (auto& pr : g_CtrLocs) list[i]->CopyBufferRegion(pr.first, pr.second, zero, 0, 4); ReleaseSRWLockShared(&g_CtrLocLock); } }
  list[i]->Close();
  ID3D12CommandList* l[] = { list[i] }; g_LastDirectQueue->ExecuteCommandLists(1, l); g_LastDirectQueue->Signal(fence, ++fv);
}
// Debug readback of g_CtrBuf [64K,68K) (ONRUSH_DEBUG_POSDUMP): 64 slots x 4 vertices x float4.
inline D3D12_RESOURCE_STATES GetTrackedResourceState(ID3D12Resource* pRes, D3D12_RESOURCE_STATES defaultState);
static ID3D12Resource* g_aoRes[6] = {}; static volatile LONG g_aoN = 0; static SIZE_T g_aoDest[6] = {}; static UINT g_aoBytes[6][8] = {};   // ONRUSH_AO_READBACK: last three 128^3 fmt28 UAV volumes
static void DumpAoVolumes(UINT frame) {
  if (!g_LastDirectQueue || !g_pMainDevice) return;
  for (int v = 0; v < 6; ++v) {
    ID3D12Resource* res = g_aoRes[v]; if (!res) continue;
    if (g_aoDest[v]) { UINT cur[8] = {}; __try { memcpy(cur, (const void*)g_aoDest[v], 32); } __except (EXCEPTION_EXECUTE_HANDLER) {}
      log_line("[aodesc] f%u vol%d dest=%llX same=%d created=%08X %08X %08X %08X %08X %08X %08X %08X now=%08X %08X %08X %08X %08X %08X %08X %08X\r\n", frame, v, (unsigned long long)g_aoDest[v], memcmp(cur, g_aoBytes[v], 32) == 0,
               g_aoBytes[v][0], g_aoBytes[v][1], g_aoBytes[v][2], g_aoBytes[v][3], g_aoBytes[v][4], g_aoBytes[v][5], g_aoBytes[v][6], g_aoBytes[v][7], cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7]); }
    ID3D12Resource* rb = nullptr; ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fe = nullptr;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 128ull * 128ull * 512ull; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return;
    g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al);
    g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
    g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fe);
    D3D12_RESOURCE_STATES st = GetTrackedResourceState(res, D3D12_RESOURCE_STATE_COMMON);
    if (cl && fe) {
      D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = res; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; if (st != D3D12_RESOURCE_STATE_COPY_SOURCE && st != D3D12_RESOURCE_STATE_COMMON) cl->ResourceBarrier(1, &b);
      D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; dst.PlacedFootprint.Footprint.Width = 128; dst.PlacedFootprint.Footprint.Height = 128; dst.PlacedFootprint.Footprint.Depth = 128; dst.PlacedFootprint.Footprint.RowPitch = 512;
      D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
      cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter); if (st != D3D12_RESOURCE_STATE_COPY_SOURCE && st != D3D12_RESOURCE_STATE_COMMON) cl->ResourceBarrier(1, &b);
      cl->Close();
      ID3D12CommandList* l[] = { cl }; g_LastDirectQueue->ExecuteCommandLists(1, l); g_LastDirectQueue->Signal(fe, 1);
      HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fe->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 3000); CloseHandle(ev);
      const UINT* p = nullptr; D3D12_RANGE r = { 0, 128 * 128 * 512 };
      if (SUCCEEDED(rb->Map(0, &r, (void**)&p)) && p) { char zl[1024]; int o = 0; int tot = 0; int one = 0;
        for (int z = 0; z < 128; ++z) { int nz = 0; for (int k = 0; k < 16384; ++k) { UINT t = p[z * 16384 + k]; if (t) { ++nz; if ((t >> 24) == 0xFF) ++one; } } tot += nz; if (z % 4 == 0 && o < 1000) o += _snprintf_s(zl + o, sizeof(zl) - o, _TRUNCATE, "%d,", nz * 100 / 16384); }
        log_line("[aorb] f%u vol%d res=%p state=%X total=%d/%d w255=%d c=%08X perZ4%%: %s\r\n", frame, v, (void*)res, (unsigned)st, tot, 128 * 16384, one, p[64 * 16384 + 64 * 128 + 64], zl); rb->Unmap(0, nullptr); }
    }
    if (cl) cl->Release(); if (al) al->Release(); if (fe) fe->Release(); rb->Release();
  }
}
static void DumpDbgRegion(UINT frame) {
  { static const bool s_ao = GetEnvironmentVariableA("ONRUSH_AO_READBACK", nullptr, 0) > 0; if (s_ao) DumpAoVolumes(frame); }
  if (!g_CtrBuf || !g_LastDirectQueue || !g_pMainDevice) return;
  ID3D12Resource* rb = nullptr; ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fe = nullptr;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 4096; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return;
  g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al);
  g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
  g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fe);
  if (cl && fe) {
    cl->CopyBufferRegion(rb, 0, g_CtrBuf, 32768, 4096); cl->Close();
    ID3D12CommandList* l[] = { cl }; g_LastDirectQueue->ExecuteCommandLists(1, l); g_LastDirectQueue->Signal(fe, 1);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fe->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 3000); CloseHandle(ev);
    float* p = nullptr; D3D12_RANGE r = { 0, 4096 };
    HRESULT mh = rb->Map(0, &r, (void**)&p);
    { int nzw = 0; if (SUCCEEDED(mh) && p) for (int k = 0; k < 1024; ++k) if (((const UINT*)p)[k]) ++nzw; log_line("[dbgr] f%u map=%08X nonzero=%d\r\n", frame, (unsigned)mh, nzw); }
    if (SUCCEEDED(mh) && p) {
      for (int s = 0; s < 64; ++s) { const float* q = p + s * 16; bool nz = false; for (int k = 0; k < 16; ++k) if (((const UINT*)q)[k] != 0u) nz = true; if (!nz) continue;
        log_line("[posdump] f%u slot %d: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\r\n", frame, s, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
        const UINT* u = (const UINT*)q;
        log_line("[dbgu] f%u slot %d: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\r\n", frame, s, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]); }
      rb->Unmap(0, nullptr); }
  }
  if (cl) cl->Release(); if (al) al->Release(); if (fe) fe->Release(); rb->Release();
}
typedef void(STDMETHODCALLTYPE *PFN_CreateCBV)(ID3D12Device *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_CreateCBV g_TrueCreateCBV = nullptr;
static void STDMETHODCALLTYPE HookedCreateCBV(ID3D12Device *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  static volatile LONG s_n = 0; LONG id = InterlockedIncrement(&s_n);
  static const bool s_log = GetEnvironmentVariableA("ONRUSH_LOG_CBV", nullptr, 0) > 0;
  if (s_log && id <= 400) log_line("[cbvdesc] #%ld va=%llX size=%u dest=%llX\r\n", (long)id, pDesc ? (unsigned long long)pDesc->BufferLocation : 0ull, pDesc ? pDesc->SizeInBytes : 0u, (unsigned long long)dest.ptr);
  if (pDesc) DescTrack_SetCbv(dest.ptr, pDesc->BufferLocation);
  if (g_TrueCreateCBV) g_TrueCreateCBV(This, pDesc, dest);
}
// ---- Raw GCN descriptors written by the game straight into descriptor heap memory (Xbox inlines CreateConstantBufferView).
// A buffer V# (16 bytes) in a CBV_SRV_UAV slot is rewritten as a native CBV when the table is bound.
ID3D12Resource* g_bigCopySrc = nullptr;
// CBV slot offsets of every descriptor-table parameter, per root signature (filled by HookedCreateRootSignature).
static std::unordered_map<void*, std::vector<std::vector<UINT>>> g_RsCbvSlots; static SRWLOCK g_RsCbvLock = SRWLOCK_INIT;
static void RecordRsCbvSlots(void* rs, const D3D12_ROOT_SIGNATURE_DESC* d) {
  std::vector<std::vector<UINT>> v(d->NumParameters);
  for (UINT i = 0; i < d->NumParameters; ++i) {
    const D3D12_ROOT_PARAMETER& p = d->pParameters[i];
    if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
    UINT run = 0;
    for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
      const D3D12_DESCRIPTOR_RANGE& rg = p.DescriptorTable.pDescriptorRanges[r];
      UINT off = (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) ? run : rg.OffsetInDescriptorsFromTableStart;
      UINT n = (rg.NumDescriptors == UINT_MAX) ? 1 : rg.NumDescriptors;
      if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) for (UINT k = 0; k < n && k < 64; ++k) v[i].push_back(off + k);
      run = off + n;
    }
  }
  AcquireSRWLockExclusive(&g_RsCbvLock); g_RsCbvSlots[rs] = std::move(v); ReleaseSRWLockExclusive(&g_RsCbvLock);
}
static bool FixOneRawCbv(SIZE_T cpu, UINT64 gpu, UINT k) {
  uint32_t w[4];
  __try { memcpy(w, (const void *)cpu, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  if (w[3] != 0x0002403Cu || ((w[1] >> 16) & 0x3FFFu) != 16u || w[2] == 0 || w[2] > 4096u) return true;
  UINT64 base = (UINT64)w[0] | ((UINT64)(w[1] & 0xFFFFu) << 32);
  D3D12_CONSTANT_BUFFER_VIEW_DESC cd = {}; cd.BufferLocation = base & ~255ull;
  UINT64 sz = (UINT64)w[2] * 16ull + (base - cd.BufferLocation); sz = (sz + 255ull) & ~255ull; if (sz > 65536ull) sz = 65536ull; cd.SizeInBytes = (UINT)sz;
  D3D12_CPU_DESCRIPTOR_HANDLE hnd; hnd.ptr = cpu;
  g_TrueCreateCBV(g_pMainDevice, &cd, hnd);
  DescTrack_SetCbv(cpu, base);
  static volatile LONG s_fixed = 0; LONG c = InterlockedIncrement(&s_fixed);
  if (c <= 20 || (c & 0xFFFF) == 0) log_line("[rawdesc] #%ld CBV V# -> native: va=%llX size=%u (records=%u) at gpu=%llX+%u\r\n", (long)c, (unsigned long long)cd.BufferLocation, cd.SizeInBytes, w[2], (unsigned long long)gpu, k);
  return true;
}
void FixRawDescriptors(UINT64 gpu, void* rs, UINT param) {
  static const bool s_off = GetEnvironmentVariableA("ONRUSH_NO_RAWDESC_FIX", nullptr, 0) > 0;
  if (s_off || !g_TrueCreateCBV || !g_pMainDevice) return;
  SIZE_T cpuBase = 0; UINT inc = 0; UINT left = 0;
  AcquireSRWLockShared(&g_descLock);
  for (auto &h : g_descHeaps) if (h.gpu && gpu >= h.gpu && gpu < h.gpu + (UINT64)h.num * h.inc) { cpuBase = h.cpu + (SIZE_T)(gpu - h.gpu); inc = h.inc; left = (UINT)((h.gpu + (UINT64)h.num * h.inc - gpu) / h.inc); break; }
  ReleaseSRWLockShared(&g_descLock);
  if (!cpuBase || inc < 16) return;
  std::vector<UINT> slots; bool known = false;
  if (rs) { AcquireSRWLockShared(&g_RsCbvLock); auto it = g_RsCbvSlots.find(rs); if (it != g_RsCbvSlots.end() && param < it->second.size()) { slots = it->second[param]; known = true; } ReleaseSRWLockShared(&g_RsCbvLock); }
  if (known) { for (UINT k : slots) if (k < left && !FixOneRawCbv(cpuBase + (SIZE_T)k * inc, gpu, k)) return; return; }
  UINT n = left < 32 ? left : 32;
  for (UINT k = 0; k < n; ++k) if (!FixOneRawCbv(cpuBase + (SIZE_T)k * inc, gpu, k)) return;
}
typedef void(STDMETHODCALLTYPE *PFN_CreateDSV)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_CreateDSV g_TrueCreateDSV = nullptr;
static std::unordered_map<SIZE_T, ID3D12Resource*> g_DsvMap; static SRWLOCK g_DsvLock = SRWLOCK_INIT;
bool LookupDsvKnown(SIZE_T ptr) { AcquireSRWLockShared(&g_DsvLock); bool f = g_DsvMap.find(ptr) != g_DsvMap.end(); ReleaseSRWLockShared(&g_DsvLock); return f; }
static void STDMETHODCALLTYPE HookedCreateDSV(ID3D12Device *This, ID3D12Resource *pRes, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  { static volatile LONG s_nd = 0; LONG id = InterlockedIncrement(&s_nd); if (id <= 12) log_line("[dsv] #%ld res=%p dest=%llX fmt=%d dim=%d flags=%d\r\n", (long)id, (void*)pRes, (unsigned long long)dest.ptr, pDesc ? (int)pDesc->Format : -1, pDesc ? (int)pDesc->ViewDimension : -1, pDesc ? (int)pDesc->Flags : -1); }
  if (dest.ptr > 0x10000) { AcquireSRWLockExclusive(&g_DsvLock); g_DsvMap[dest.ptr] = pRes; ReleaseSRWLockExclusive(&g_DsvLock); }
  if (g_TrueCreateDSV) g_TrueCreateDSV(This, pRes, pDesc, dest);
}
static void STDMETHODCALLTYPE HookedCreateUAV(ID3D12Device *This, ID3D12Resource *pRes, ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  if (pRes && dest.ptr > 0x10000)
    DescTrack_Set(dest.ptr, { pRes, pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN, pDesc ? (UINT)pDesc->ViewDimension : 0, 2 });
  { static volatile LONG s_nu = 0; static volatile LONG s_nuc = 0; LONG iu = InterlockedIncrement(&s_nu); if (pCounter) InterlockedIncrement(&s_nuc);
    if ((pCounter && s_nuc <= 40) || iu <= 8) log_line("[uav] #%ld res=%p counter=%p dim=%d fmt=%d first=%llu n=%u stride=%u counterOff=%llu\r\n", (long)iu, (void*)pRes, (void*)pCounter, pDesc ? (int)pDesc->ViewDimension : -1, pDesc ? (int)pDesc->Format : -1,
             pDesc ? (unsigned long long)pDesc->Buffer.FirstElement : 0ull, pDesc ? (UINT)pDesc->Buffer.NumElements : 0u, pDesc ? (UINT)pDesc->Buffer.StructureByteStride : 0u, pDesc ? (unsigned long long)pDesc->Buffer.CounterOffsetInBytes : 0ull); }
  // Hidden-counter emulation: every structured buffer UAV gets a private 4-byte counter slot (4096-aligned) if the game gave none.
  static ID3D12Resource* s_ctrPool = nullptr; static volatile LONG s_ctrNext = 0; static const bool s_ctrOn = GetEnvironmentVariableA("ONRUSH_NO_COUNTER_POOL", nullptr, 0) == 0;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ctrDesc;
  if (s_ctrOn && pRes && !pCounter && pDesc && pDesc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER && pDesc->Buffer.StructureByteStride && pDesc->Format == DXGI_FORMAT_UNKNOWN && g_pMainDevice) {
    if (!s_ctrPool) { D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 4096ull * 8192ull; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&s_ctrPool); log_line("[uav] counter pool created %p\r\n", (void*)s_ctrPool); }
    LONG slot = InterlockedIncrement(&s_ctrNext) - 1;
    if (slot == 8192) log_line("[uav] counter pool wrapped (8192 structured UAVs created)\r\n");
    slot &= 8191;
    if (s_ctrPool) { ctrDesc = *pDesc; ctrDesc.Buffer.CounterOffsetInBytes = (UINT64)slot * 4096ull; pCounter = s_ctrPool; pDesc = &ctrDesc; }
  }
  if (pDesc && pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) {
    static volatile LONG s_n3 = 0;
    static const bool s_aorb = GetEnvironmentVariableA("ONRUSH_AO_READBACK", nullptr, 0) > 0;
    if (s_aorb && pRes && pDesc->Format == DXGI_FORMAT_R8G8B8A8_UNORM && pDesc->Texture3D.WSize == 128) { pRes->AddRef(); g_aoRes[(InterlockedIncrement(&g_aoN) - 1) % 6] = pRes; }
    if (InterlockedIncrement(&s_n3) <= 64) log_line("[uav3dview] res=%p fmt=%d mip=%u firstW=%u wsize=%u dest=%llX\r\n", (void*)pRes, (int)pDesc->Format, pDesc->Texture3D.MipSlice, pDesc->Texture3D.FirstWSlice, pDesc->Texture3D.WSize, (unsigned long long)dest.ptr);
  }
  if (pCounter && pDesc) RecordCtrLoc(pCounter, pDesc->Buffer.CounterOffsetInBytes);
  if (g_TrueCreateUAV) g_TrueCreateUAV(This, pRes, pCounter, pDesc, dest);
  if (g_aoN > 0 && pRes && pDesc && pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D && pDesc->Format == DXGI_FORMAT_R8G8B8A8_UNORM && pDesc->Texture3D.WSize == 128 && g_aoRes[(g_aoN - 1) % 6] == pRes) { LONG i_ = (g_aoN - 1) % 6; if (i_ >= 0) { g_aoDest[i_] = dest.ptr; __try { memcpy(g_aoBytes[i_], (const void*)dest.ptr, 32); } __except (EXCEPTION_EXECUTE_HANDLER) {} } }
}
static void STDMETHODCALLTYPE HookedCopyDescriptorsSimple(ID3D12Device *This, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  UINT inc = 0;
  __try { inc = This->GetDescriptorHandleIncrementSize(type); } __except (EXCEPTION_EXECUTE_HANDLER) {}
  { static const bool s_log = GetEnvironmentVariableA("ONRUSH_LOG_RAWDESC", nullptr, 0) > 0; static volatile LONG s_k = 0;
    if (s_log && inc && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
      for (UINT i = 0; i < n && i < 64; ++i) { DescInfo di; if (DescTrack_Get(src.ptr + (SIZE_T)i * inc, &di)) continue;
        uint32_t w[8] = {}; bool ok = true; __try { memcpy(w, (const void*)(src.ptr + (SIZE_T)i * inc), 32); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (InterlockedIncrement(&s_k) <= 300) log_line("[rawdesc] n=%u i=%u src=%llX ok=%d %08X %08X %08X %08X %08X %08X %08X %08X\r\n", n, i, (unsigned long long)src.ptr, (int)ok, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); } } }
  if (inc) DescTrack_Copy(dst.ptr, src.ptr, n, inc);
  if (g_TrueCopyDescriptorsSimple) g_TrueCopyDescriptorsSimple(This, n, dst, src, type);
}
static void STDMETHODCALLTYPE HookedCopyDescriptors(ID3D12Device *This, UINT nDst, const D3D12_CPU_DESCRIPTOR_HANDLE *pDst, const UINT *pDstSizes,
                                                    UINT nSrc, const D3D12_CPU_DESCRIPTOR_HANDLE *pSrc, const UINT *pSrcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  UINT inc = 0;
  __try { inc = This->GetDescriptorHandleIncrementSize(type); } __except (EXCEPTION_EXECUTE_HANDLER) {}
  if (inc && pDst && pSrc) {
    __try {
      UINT di = 0, doff = 0, si = 0, soff = 0;
      while (di < nDst && si < nSrc) {
        UINT dsz = pDstSizes ? pDstSizes[di] : 1, ssz = pSrcSizes ? pSrcSizes[si] : 1;
        DescTrack_Copy(pDst[di].ptr + (SIZE_T)doff * inc, pSrc[si].ptr + (SIZE_T)soff * inc, 1, inc);
        if (++doff >= dsz) { ++di; doff = 0; }
        if (++soff >= ssz) { ++si; soff = 0; }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  if (g_TrueCopyDescriptors) g_TrueCopyDescriptors(This, nDst, pDst, pDstSizes, nSrc, pSrc, pSrcSizes, type);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(
    ID3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid,
    void **ppvHeap) {
  log_line("[d3d12_x] CreateDescriptorHeap called: Type=%d, NumDescriptors=%u, "
           "Flags=0x%X, NodeMask=0x%X\r\n",
           pDesc ? pDesc->Type : -1, pDesc ? pDesc->NumDescriptors : 0,
           pDesc ? (UINT)pDesc->Flags : 0, pDesc ? pDesc->NodeMask : 0);
  HRESULT hr = g_TrueCreateDescriptorHeap(pDevice, pDesc, riid, ppvHeap);
  log_line("  -> CreateDescriptorHeap returned hr=0x%08lX, ppvHeap=%p\r\n", hr,
           ppvHeap ? *ppvHeap : nullptr);
  if (SUCCEEDED(hr) && ppvHeap && *ppvHeap) {
    if (pDesc) {
      AcquireSRWLockExclusive(&g_HeapDescLock);
      g_HeapDescMap[(ID3D12DescriptorHeap *)*ppvHeap] = *pDesc;
      ReleaseSRWLockExclusive(&g_HeapDescLock);
    }
    if (pDesc) {
      ID3D12DescriptorHeap *rh = (ID3D12DescriptorHeap *)*ppvHeap;
      __try {
        UINT incr = pDevice->GetDescriptorHandleIncrementSize(pDesc->Type);
        SIZE_T cpuS = rh->GetCPUDescriptorHandleForHeapStart().ptr;
        UINT64 gpuS = (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? rh->GetGPUDescriptorHandleForHeapStart().ptr : 0;
        DescTrack_RegisterHeap(cpuS, gpuS, pDesc->NumDescriptors, incr);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    WrapDescriptorHeap(ppvHeap);
  }
  return hr;
}

template <int N> struct DeviceSlotThunk {
  static HRESULT STDMETHODCALLTYPE Invoke(void *pDevice, void *a1, void *a2,
                                          void *a3, void *a4, void *a5) {
    log_line("[d3d12_x] UNHANDLED DEVICE VTABLE CALL: Slot %d (offset 0x%03X, "
             "pDevice=%p, a1=%p, a2=%p)\r\n",
             N, N * 8, pDevice, a1, a2);
    return S_OK;
  }
};

template <int... Is>
static void InitAllDeviceThunks(void **vtable,
                                std::integer_sequence<int, Is...>) {
  ((vtable[Is] = (void *)&DeviceSlotThunk<Is>::Invoke), ...);
}

static D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE HookedGetCustomHeapProperties(
    D3D12_HEAP_PROPERTIES *pRetVal, ID3D12Device *pDevice, UINT nodeMask,
    D3D12_HEAP_TYPE heapType) {
  log_line("[d3d12_x] HookedGetCustomHeapProperties: pRetVal=%p, pDevice=%p, "
           "nodeMask=%u, heapType=%d\r\n",
           pRetVal, pDevice, nodeMask, heapType);

  if (!pRetVal)
    return nullptr;

  memset(pRetVal, 0, sizeof(D3D12_HEAP_PROPERTIES));
  pRetVal->Type = heapType;
  pRetVal->CreationNodeMask = 1;
  pRetVal->VisibleNodeMask = 1;

  switch (heapType) {
  case D3D12_HEAP_TYPE_DEFAULT:
    pRetVal->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    pRetVal->MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
    break;
  case D3D12_HEAP_TYPE_UPLOAD:
    pRetVal->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    pRetVal->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    break;
  case D3D12_HEAP_TYPE_READBACK:
    pRetVal->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    pRetVal->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    break;
  case D3D12_HEAP_TYPE_CUSTOM:
  default:
    pRetVal->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    pRetVal->MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    break;
  }

  log_line("  -> Props: Type=%d, CPUPage=%d, Pool=%d\r\n", pRetVal->Type,
           pRetVal->CPUPageProperty, pRetVal->MemoryPoolPreference);

  return pRetVal;
}

static inline bool CanResourceSupportRenderTarget(DXGI_FORMAT fmt) {
  if (fmt == DXGI_FORMAT_UNKNOWN)
    return false;
  // Block compressed formats cannot be render targets
  if ((fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM) ||
      (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB)) {
    return false;
  }
  // Pure depth-stencil formats cannot be color render targets
  if (fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
      fmt == DXGI_FORMAT_D32_FLOAT ||
      fmt == DXGI_FORMAT_D24_UNORM_S8_UINT ||
      fmt == DXGI_FORMAT_D16_UNORM ||
      fmt == DXGI_FORMAT_R32G8X24_TYPELESS ||
      fmt == DXGI_FORMAT_R24G8_TYPELESS) {
    return false;
  }
  return true;
}

static inline DXGI_FORMAT GetDefaultTypedRTVFormat(DXGI_FORMAT fmt) {
  switch (fmt) {
  case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
  case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_FLOAT;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
  case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
  case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
  case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
  case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;
  case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
  default:                                return fmt;
  }
}

static inline D3D12_RESOURCE_DESC
SanitizeResourceDesc(const D3D12_RESOURCE_DESC &in) {
  D3D12_RESOURCE_DESC out = in;
  if (out.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    // On Xbox One Durango ERA, textures use vendor/Xbox-specific layout
    // enum/flags (e.g. 269 = 0x10D). Desktop Windows D3D12 strictly requires
    // D3D12_TEXTURE_LAYOUT_UNKNOWN (0) for non-standard swizzles.
    if (out.Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE &&
        out.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
      out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }

    // Targeted conversion for Xbox One typeless depth resources:
    // Dimension=TEXTURE2D, Format=R16_TYPELESS (53) or R32G8X24_TYPELESS (19),
    // Flags=0x01000002, Layout=256.
    // Bit 24 (0x01000000) is console HTILE/DCC metadata flag not supported by
    // desktop D3D12. Convert to D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL (0x2)
    // and D3D12_TEXTURE_LAYOUT_UNKNOWN (0).
    if (out.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (out.Format == DXGI_FORMAT_R16_TYPELESS ||
         out.Format == DXGI_FORMAT_R32G8X24_TYPELESS) &&
        out.Flags == (D3D12_RESOURCE_FLAGS)0x01000002 && in.Layout == 256) {
      out.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }

    // On Xbox One Durango ERA, textures can be used as render targets even if
    // creation flags only specify UAV (0x4). On desktop D3D12, CreateRenderTargetView
    // fails with ERROR #42 and triggers device removal (0x887A0001) if
    // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET is missing.
    // Automatically enable ALLOW_RENDER_TARGET ONLY for 2D color-renderable non-depth textures.
    if (out.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        !(out.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
        CanResourceSupportRenderTarget(out.Format)) {
      out.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
  } else {
    // Narrow conversion for Durango 24B index buffer query if passed through
    // GetResourceAllocationInfo: Dimension=BUFFER, Width=24, Layout=ROW_MAJOR,
    // Flags=0x10000.
    if (out.Width == 24 && out.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
        out.Flags == (D3D12_RESOURCE_FLAGS)0x10000) {
      out.Flags = D3D12_RESOURCE_FLAG_NONE;
    }
    // Durango-only buffer flag bits (e.g. 0x20000 in Flags=0x20004) make desktop
    // CreateCommittedResource fail with E_INVALIDARG. Keep only UAV for buffers.
    out.Flags &= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  }
  if (out.Alignment != 0 && out.Alignment != 65536 &&
      out.Alignment != 4194304) {
    out.Alignment = 0; // Let driver choose default alignment
  }
  return out;
}

static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
HookedGetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO *pRetVal,
                                ID3D12Device *pDevice, UINT visibleMask,
                                UINT numResourceDescs,
                                const D3D12_RESOURCE_DESC *pResourceDescs) {
  log_line("[d3d12_x] HookedGetResourceAllocationInfo: pRetVal=%p, pDevice=%p, "
           "visibleMask=0x%X, numDescs=%u\r\n",
           pRetVal, pDevice, visibleMask, numResourceDescs);

  if (pRetVal && pDevice && g_TrueGetResourceAllocationInfo) {
    std::vector<D3D12_RESOURCE_DESC> cleanDescs;
    if (pResourceDescs && numResourceDescs > 0) {
      cleanDescs.resize(numResourceDescs);
      for (UINT i = 0; i < numResourceDescs; ++i) {
        const D3D12_RESOURCE_DESC &rawD = pResourceDescs[i];
        cleanDescs[i] = SanitizeResourceDesc(rawD);
        if (rawD.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            rawD.Width == 24 && rawD.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
            rawD.Flags == (D3D12_RESOURCE_FLAGS)0x10000) {
          log_line("[d3d12_x] GetResourceAllocationInfo: matched 24B index "
                   "buffer query (Flags 0x10000 -> 0x0)\r\n");
        } else if (rawD.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   (rawD.Format == DXGI_FORMAT_R16_TYPELESS ||
                    rawD.Format == DXGI_FORMAT_R32G8X24_TYPELESS) &&
                   rawD.Flags == (D3D12_RESOURCE_FLAGS)0x01000002 &&
                   rawD.Layout == 256) {
          log_line("  GetResourceAllocationInfo[%u] received: Dim=%d, %llux%u, "
                   "DepthOrArray=%u, Mip=%u, Fmt=%d, Samples=%u/%u, Lay=%d, "
                   "Flags=0x%08X, Align=%llu\r\n",
                   i, rawD.Dimension, (unsigned long long)rawD.Width,
                   rawD.Height, rawD.DepthOrArraySize, rawD.MipLevels,
                   rawD.Format, rawD.SampleDesc.Count, rawD.SampleDesc.Quality,
                   rawD.Layout, rawD.Flags, (unsigned long long)rawD.Alignment);
          log_line(
              "  GetResourceAllocationInfo[%u] sent:     Dim=%d, %llux%u, "
              "DepthOrArray=%u, Mip=%u, Fmt=%d, Samples=%u/%u, Lay=%d, "
              "Flags=0x%08X, Align=%llu\r\n",
              i, cleanDescs[i].Dimension,
              (unsigned long long)cleanDescs[i].Width, cleanDescs[i].Height,
              cleanDescs[i].DepthOrArraySize, cleanDescs[i].MipLevels,
              cleanDescs[i].Format, cleanDescs[i].SampleDesc.Count,
              cleanDescs[i].SampleDesc.Quality, cleanDescs[i].Layout,
              cleanDescs[i].Flags, (unsigned long long)cleanDescs[i].Alignment);
        }
      }
    }
    g_TrueGetResourceAllocationInfo(
        pDevice, pRetVal, visibleMask, numResourceDescs,
        cleanDescs.empty() ? pResourceDescs : cleanDescs.data());
    log_line("  -> AllocInfo: SizeInBytes=%llu, Alignment=%llu\r\n",
             (unsigned long long)pRetVal->SizeInBytes,
             (unsigned long long)pRetVal->Alignment);
  }
  return pRetVal;
}

typedef void(STDMETHODCALLTYPE *PFN_CreateRenderTargetView)(
    ID3D12Device *This, ID3D12Resource *pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
static PFN_CreateRenderTargetView g_TrueCreateRenderTargetView = nullptr;

static void STDMETHODCALLTYPE HookedCreateRenderTargetView(
    ID3D12Device *This, ID3D12Resource *pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  if (!pResource) {
    if (g_TrueCreateRenderTargetView) {
      g_TrueCreateRenderTargetView(This, nullptr, pDesc, DestDescriptor);
    }
    return;
  }

  if (DestDescriptor.ptr > 0x10000) {
    AcquireSRWLockExclusive(&g_RtvMapLock);
    g_RtvToResourceMap[DestDescriptor.ptr] = pResource;
    ReleaseSRWLockExclusive(&g_RtvMapLock);

    static volatile LONG s_rtvLogCount = 0;
    if (InterlockedIncrement(&s_rtvLogCount) <= 40) {
      D3D12_RESOURCE_DESC rd = pResource->GetDesc();
      log_line("[d3d12_x] Registered RTV: handle=0x%llX -> res=%p (%llux%u, fmt=%d, caller=game+0x%llX)\r\n",
               (unsigned long long)DestDescriptor.ptr, pResource,
               (unsigned long long)rd.Width, rd.Height, rd.Format, (unsigned long long)rva);
    }
  }

  D3D12_RESOURCE_DESC rDesc = pResource->GetDesc();

  if (!CanResourceSupportRenderTarget(rDesc.Format)) {
    log_line("[d3d12_x] WARNING: CreateRenderTargetView called on non-rendertarget resource format %d (caller=game+0x%llX) - skipped!\r\n",
             rDesc.Format, (unsigned long long)rva);
    return;
  }

  // Ensure ALLOW_RENDER_TARGET flag is present
  if (!(rDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
    log_line("[d3d12_x] WARNING: CreateRenderTargetView called on resource %p without ALLOW_RENDER_TARGET (Flags=0x%X, Fmt=%d) - skipped!\r\n",
             pResource, rDesc.Flags, rDesc.Format);
    return;
  }

  D3D12_RENDER_TARGET_VIEW_DESC sanitizedDesc = {};
  const D3D12_RENDER_TARGET_VIEW_DESC *pDescToUse = pDesc;

  if (pDesc) {
    sanitizedDesc = *pDesc;

    // If view format is unknown or typeless, map it to a valid typed format
    if (sanitizedDesc.Format == DXGI_FORMAT_UNKNOWN ||
        sanitizedDesc.Format == rDesc.Format) {
      sanitizedDesc.Format = GetDefaultTypedRTVFormat(rDesc.Format);
    } else {
      sanitizedDesc.Format = GetDefaultTypedRTVFormat(sanitizedDesc.Format);
    }

    // Incompatible ViewDimension: e.g. Texture3D resource with Texture2D view or vice-versa
    if (rDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
      if (sanitizedDesc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D) {
        if (sanitizedDesc.Texture3D.WSize == 0) {
          sanitizedDesc.Texture3D.WSize = (UINT)-1; // Default to all slices
        }
      } else {
        // Fix incompatible view dimension for 3D texture
        sanitizedDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        sanitizedDesc.Texture3D.MipSlice = 0;
        sanitizedDesc.Texture3D.FirstWSlice = 0;
        sanitizedDesc.Texture3D.WSize = (UINT)-1;
      }
    } else if (rDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      if (sanitizedDesc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D) {
        sanitizedDesc.ViewDimension = (rDesc.DepthOrArraySize > 1)
                                          ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY
                                          : D3D12_RTV_DIMENSION_TEXTURE2D;
      }
    }

    pDescToUse = &sanitizedDesc;
  } else {
    // If pDesc is null, D3D12 requires a typed resource format.
    // If resource is typeless, synthesize a typed view desc:
    DXGI_FORMAT typedFmt = GetDefaultTypedRTVFormat(rDesc.Format);
    if (typedFmt != rDesc.Format) {
      memset(&sanitizedDesc, 0, sizeof(sanitizedDesc));
      sanitizedDesc.Format = typedFmt;
      if (rDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        sanitizedDesc.ViewDimension = (rDesc.DepthOrArraySize > 1)
                                          ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY
                                          : D3D12_RTV_DIMENSION_TEXTURE2D;
      } else if (rDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        sanitizedDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        sanitizedDesc.Texture3D.WSize = (UINT)-1;
      }
      pDescToUse = &sanitizedDesc;
    }
  }

  if (g_TrueCreateRenderTargetView) {
    g_TrueCreateRenderTargetView(This, pResource, pDescToUse, DestDescriptor);
  }
}

// Diagnostic: remember buffers created at game CPU addresses (Durango placed resources).
struct PlacedBufInfo { ID3D12Resource *res; void *cpu; UINT64 size; D3D12_GPU_VIRTUAL_ADDRESS va; bool upload; };
static PlacedBufInfo g_placedBufs[20000];
static volatile LONG g_placedBufCount = 0;
struct CbvProbe { ID3D12Resource* res; UINT64 off; UINT64 avail; UINT draw; int param; };
static CbvProbe g_cbvProbe[24]; static volatile LONG g_cbvProbeCount = 0;
struct GeoProbe { ID3D12Resource* res; UINT64 off; UINT64 avail; UINT draw; int kind; UINT stride; };
static GeoProbe g_geoProbe[48]; static volatile LONG g_geoProbeCount = 0;
static void LogGeomImpl(UINT drawIdx, const D3D12_INDEX_BUFFER_VIEW* ib, const D3D12_VERTEX_BUFFER_VIEW* vb) {
  auto find = [](D3D12_GPU_VIRTUAL_ADDRESS va, ID3D12Resource** r, UINT64* off, UINT64* avail) { LONG n = g_placedBufCount; if (n > 20000) n = 20000;
    for (LONG k = 0; k < n; ++k) { const PlacedBufInfo& q = g_placedBufs[k]; if (va >= q.va && va < q.va + q.size) { *r = q.res; *off = va - q.va; *avail = q.size - (va - q.va); return true; } } return false; };
  for (int kind = 0; kind < 2; ++kind) {
    D3D12_GPU_VIRTUAL_ADDRESS va = kind == 0 ? ib->BufferLocation : vb[0].BufferLocation; if (!va) continue;
    ID3D12Resource* r; UINT64 off, av; LONG ip = g_geoProbeCount; if (ip >= 48) return;
    if (!find(va, &r, &off, &av)) { log_line("[geo] draw#%u %s va=%llX NOT-IN-PLACED\r\n", drawIdx, kind ? "VB" : "IB", (unsigned long long)va); continue; }
    g_geoProbe[ip] = { r, off, av, drawIdx, kind, kind ? vb[0].StrideInBytes : (UINT)(ib->Format == DXGI_FORMAT_R16_UINT ? 2 : 4) }; g_geoProbeCount = ip + 1;
    log_line("[geo] draw#%u %s va=%llX size=%u stride=%u\r\n", drawIdx, kind ? "VB" : "IB", (unsigned long long)va, kind ? vb[0].SizeInBytes : ib->SizeInBytes, kind ? vb[0].StrideInBytes : 0u);
  }
}
static void LogRootCbvImpl(UINT drawIdx, void* pso, const D3D12_GPU_VIRTUAL_ADDRESS* cbv) {
  for (int pi = 0; pi < 4; ++pi) {
    D3D12_GPU_VIRTUAL_ADDRESS va = cbv[pi]; if (!va) continue;
    LONG n = g_placedBufCount; if (n > 20000) n = 20000; const PlacedBufInfo* hit = nullptr;
    for (LONG k = 0; k < n; ++k) { const PlacedBufInfo& q = g_placedBufs[k]; if (va >= q.va && va < q.va + q.size) { hit = &q; break; } }
    if (!hit) { log_line("[cbv] draw#%u pso=%p param%d va=%llX NOT-IN-PLACED\r\n", drawIdx, pso, pi, (unsigned long long)va); continue; }
    { static volatile LONG s_np = 0; LONG ip = InterlockedIncrement(&s_np) - 1; if (ip < 24) { g_cbvProbe[ip] = { hit->res, (UINT64)(va - hit->va), hit->size - (va - hit->va), drawIdx, pi }; g_cbvProbeCount = ip + 1; } }
    const float* f = (const float*)((const uint8_t*)hit->cpu + (va - hit->va)); int nz = 0; UINT64 maxn = hit->size - (va - hit->va); if (maxn > 148 * 16) maxn = 148 * 16;
    for (UINT64 k = 0; k < maxn / 4; ++k) if (f[k] != 0.0f) ++nz;
    log_line("[cbv] draw#%u pso=%p param%d va=%llX upload=%d nonzero=%d/%llu first=%g %g %g %g | %g %g %g %g\r\n", drawIdx, pso, pi, (unsigned long long)va, (int)hit->upload, nz, (unsigned long long)(maxn / 4), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
  }
}


static HRESULT STDMETHODCALLTYPE HookedCreatePlacedResourceX(
    ID3D12Device *pDevice, void *pHeapOrAddress,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *priid,
    void **ppvResource) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  log_line("[d3d12_x] HookedCreatePlacedResourceX (Slot 44): caller=%p "
           "(game+0x%llX), pDevice=%p, heapOrAddr=%p, pDesc=%p, state=0x%X, "
           "ppv=%p\r\n",
           retAddr, (unsigned long long)rva, pDevice, pHeapOrAddress, pDesc,
           (UINT)InitialState, ppvResource);

  if (pDesc) {
    log_line("  desc: Dim=%d, W=%llu, H=%u, DepthOrArray=%u, Mip=%u, Fmt=%d, "
             "Samples=%u/%u, Lay=%d, Flags=0x%X, Align=%llu\r\n",
             pDesc->Dimension, (unsigned long long)pDesc->Width, pDesc->Height,
             pDesc->DepthOrArraySize, pDesc->MipLevels, pDesc->Format,
             pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality, pDesc->Layout,
             pDesc->Flags, (unsigned long long)pDesc->Alignment);
  }

  if (!ppvResource)
    return E_POINTER;
  *ppvResource = nullptr;

  if (pDevice) {
    HRESULT preReason = pDevice->GetDeviceRemovedReason();
    if (preReason != S_OK) {
      log_line("[d3d12_x] CRITICAL: Device was ALREADY REMOVED prior to "
               "HookedCreatePlacedResourceX! Reason=0x%08lX\r\n",
               (unsigned long)preReason);
      FlushD3D12InfoQueue("Prior to CreatePlacedResourceX");
    }
  }

  IID riid = priid ? *priid : __uuidof(ID3D12Resource);
  D3D12_RESOURCE_DESC cleanDesc =
      pDesc ? SanitizeResourceDesc(*pDesc) : D3D12_RESOURCE_DESC{};

  bool isTypelessDepth =
      (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
       (pDesc->Format == DXGI_FORMAT_R16_TYPELESS ||
        pDesc->Format == DXGI_FORMAT_R32G8X24_TYPELESS) &&
       pDesc->Flags == (D3D12_RESOURCE_FLAGS)0x01000002 &&
       pDesc->Layout == 256);

  bool isStaging36 =
      (cleanDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
       cleanDesc.Width == 36 && cleanDesc.Flags == D3D12_RESOURCE_FLAG_NONE &&
       InitialState == (D3D12_RESOURCE_STATES)0x400);

  bool isUavBuffer =
      (cleanDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
       (cleanDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);

  // Other CPU-written staging buffers made by the same game helper
  // (game+0xBDE1D creates the buffer and Map()s it right away). On Durango
  // this memory is CPU-visible; a DEFAULT heap makes Map fail (E_INVALIDARG,
  // *ppData=NULL) and the game then memcpy()s into NULL (AV in VCRUNTIME).
  bool isCpuStagingBuffer =
      (!isStaging36 && rva == 0xBDE1D && pDesc &&
       cleanDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
       cleanDesc.Flags == D3D12_RESOURCE_FLAG_NONE &&
       InitialState == (D3D12_RESOURCE_STATES)0x400);

  bool isIndexBuffer24 =
      (rva == 0xBC823 && pDesc &&
       pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
       pDesc->Width == 24 && pDesc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
       pDesc->Flags == (D3D12_RESOURCE_FLAGS)0x10000 &&
       pOptimizedClearValue == nullptr);

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  D3D12_RESOURCE_STATES state = InitialState;

  if (isTypelessDepth) {
    const char *fmtName = (pDesc->Format == DXGI_FORMAT_R16_TYPELESS)
                              ? "R16_TYPELESS"
                              : "R32G8X24_TYPELESS";
    log_line("[d3d12_x] TRANSLATING %s DEPTH RESOURCE (%llux%u):\r\n", fmtName,
             (unsigned long long)pDesc->Width, pDesc->Height);
    log_line("  Durango received: Dim=%d, %llux%u, DepthOrArray=%u, Mip=%u, "
             "Fmt=%d, Samples=%u/%u, Flags=0x%08X, Layout=%d, Alignment=%llu, "
             "State=0x%X\r\n",
             pDesc->Dimension, (unsigned long long)pDesc->Width, pDesc->Height,
             pDesc->DepthOrArraySize, pDesc->MipLevels, pDesc->Format,
             pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality, pDesc->Flags,
             pDesc->Layout, (unsigned long long)pDesc->Alignment,
             (UINT)InitialState);
    log_line("  Desktop used:     Dim=%d, %llux%u, DepthOrArray=%u, Mip=%u, "
             "Fmt=%d, Samples=%u/%u, Flags=0x%08X, Layout=%d, Alignment=%llu, "
             "State=0x%X\r\n",
             cleanDesc.Dimension, (unsigned long long)cleanDesc.Width,
             cleanDesc.Height, cleanDesc.DepthOrArraySize, cleanDesc.MipLevels,
             cleanDesc.Format, cleanDesc.SampleDesc.Count,
             cleanDesc.SampleDesc.Quality, cleanDesc.Flags, cleanDesc.Layout,
             (unsigned long long)cleanDesc.Alignment, (UINT)state);
    if (pOptimizedClearValue) {
      log_line("  ClearValue:       Fmt=%d, Depth=%.3f, Stencil=%u\r\n",
               pOptimizedClearValue->Format,
               pOptimizedClearValue->DepthStencil.Depth,
               (UINT)pOptimizedClearValue->DepthStencil.Stencil);
    } else {
      log_line("  ClearValue:       nullptr\r\n");
    }
  } else if (isIndexBuffer24) {
    cleanDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    state = InitialState; // Preserved InitialState = 0x400
                          // (D3D12_RESOURCE_STATE_COPY_DEST)

    log_line("[d3d12_x] TRANSLATING 24B INDEX BUFFER (caller=game+0x%llX):\r\n",
             (unsigned long long)rva);
    log_line("  Durango received: Dim=%d, Width=%llu, Layout=%d, "
             "Alignment=%llu, Flags=0x%08X, State=0x%X, ClearVal=%p\r\n",
             pDesc->Dimension, (unsigned long long)pDesc->Width, pDesc->Layout,
             (unsigned long long)pDesc->Alignment, pDesc->Flags,
             (UINT)InitialState, pOptimizedClearValue);
    log_line("  Desktop used:     Dim=%d, Width=%llu, Layout=%d, "
             "Alignment=%llu, Flags=0x%08X, State=0x%X, HeapType=DEFAULT\r\n",
             cleanDesc.Dimension, (unsigned long long)cleanDesc.Width,
             cleanDesc.Layout, (unsigned long long)cleanDesc.Alignment,
             cleanDesc.Flags, (UINT)state);
  } else if (isStaging36 || isCpuStagingBuffer) {
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    state = D3D12_RESOURCE_STATE_GENERIC_READ;

    log_line("[d3d12_x] TRANSLATING STAGING BUFFER (Width=36, Flags=0, Durango "
             "State=0x400):\r\n");
    log_line("  Durango received: Dim=%d, Width=%llu, Flags=0x%X, "
             "InitialState=0x%X (COPY_DEST)\r\n",
             cleanDesc.Dimension, (unsigned long long)cleanDesc.Width,
             cleanDesc.Flags, (UINT)InitialState);
    log_line("  Desktop used:     HeapType=D3D12_HEAP_TYPE_UPLOAD (%d), "
             "State=D3D12_RESOURCE_STATE_GENERIC_READ (0x%X)\r\n",
             heapProps.Type, (UINT)state);
  } else {
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Use requested InitialState (sanitized to valid D3D12 states), defaulting to COMMON if 0 or COPY_DEST (to allow copy queue usage)
    state = (D3D12_RESOURCE_STATES)(InitialState & 0x3FFF);
    if (!state || state == D3D12_RESOURCE_STATE_COPY_DEST) {
      state = D3D12_RESOURCE_STATE_COMMON;
    }
  }

  { static const bool s_sim = GetEnvironmentVariableA("ONRUSH_SIMUL3D", nullptr, 0) > 0;
    if (s_sim && cleanDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && (cleanDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
      cleanDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS; log_line("[simul3d] %llux%ux%u fmt=%d\r\n", (unsigned long long)cleanDesc.Width, cleanDesc.Height, (unsigned)cleanDesc.DepthOrArraySize, (int)cleanDesc.Format); } }
  HRESULT hr = E_FAIL;
  if (g_TrueCreateCommittedResource) {
    hr = g_TrueCreateCommittedResource(pDevice, &heapProps,
                                       D3D12_HEAP_FLAG_NONE, &cleanDesc, state,
                                       pOptimizedClearValue, riid, ppvResource);
    log_line("  -> CreateCommittedResource returned hr=0x%08lX, res=%p\r\n",
             (unsigned long)hr, *ppvResource);

    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
      TrackResourceState((ID3D12Resource *)*ppvResource, state);
    }

    if (FAILED(hr)) {
      LogD3D12Failure(pDevice, "CreatePlacedResourceX->CreateCommittedResource",
                      hr);
      if (!isStaging36 && !isCpuStagingBuffer && state != D3D12_RESOURCE_STATE_COMMON) {
        hr = g_TrueCreateCommittedResource(
            pDevice, &heapProps, D3D12_HEAP_FLAG_NONE, &cleanDesc,
            D3D12_RESOURCE_STATE_COMMON, pOptimizedClearValue, riid,
            ppvResource);
        log_line("  -> Retry with COMMON returned hr=0x%08lX, res=%p\r\n",
                 (unsigned long)hr, *ppvResource);
        if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
          TrackResourceState((ID3D12Resource *)*ppvResource, D3D12_RESOURCE_STATE_COMMON);
        }
        if (FAILED(hr)) {
          LogD3D12Failure(pDevice, "CreatePlacedResourceX->RetryCOMMON", hr);
          if (cleanDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
            cleanDesc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            hr = g_TrueCreateCommittedResource(
                pDevice, &heapProps, D3D12_HEAP_FLAG_NONE, &cleanDesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, riid, ppvResource);
            log_line("  -> Retry without ALLOW_RENDER_TARGET returned hr=0x%08lX, res=%p\r\n",
                     (unsigned long)hr, ppvResource ? *ppvResource : nullptr);
            if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
              TrackResourceState((ID3D12Resource *)*ppvResource, D3D12_RESOURCE_STATE_COMMON);
            }
            if (FAILED(hr)) {
              LogD3D12Failure(pDevice, "CreatePlacedResourceX->RetryNoRTV", hr);
            }
          }
        }
      }
    }
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
      if (heapProps.Type == D3D12_HEAP_TYPE_UPLOAD) {
        TrackUploadResource((ID3D12Resource *)*ppvResource);
      }
      if (cleanDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        LONG ix = InterlockedIncrement(&g_placedBufCount) - 1;
        if (ix < 20000) {
          ID3D12Resource *r = (ID3D12Resource *)*ppvResource;
          g_placedBufs[ix] = { r, pHeapOrAddress, cleanDesc.Width, r->GetGPUVirtualAddress(), heapProps.Type == D3D12_HEAP_TYPE_UPLOAD };
        }
      }
      if (isStaging36 && g_NumStagingBuffers36 < 8) {
        g_StagingBuffers36[g_NumStagingBuffers36++] =
            (ID3D12Resource *)*ppvResource;
        log_line("  -> Tracked staging buffer [%d] = %p\r\n",
                 g_NumStagingBuffers36 - 1, *ppvResource);
      }
      if (isUavBuffer && g_NumTrackedUavBuffers < 64) {
        g_TrackedUavBuffers[g_NumTrackedUavBuffers++] =
            (ID3D12Resource *)*ppvResource;
        log_line(
            "  -> Tracked UAV buffer [%d] = %p (Width=%llu, Flags=0x%X)\r\n",
            g_NumTrackedUavBuffers - 1, *ppvResource,
            (unsigned long long)cleanDesc.Width, cleanDesc.Flags);
      }
      if (isIndexBuffer24) {
        g_pIndexBuffer24 = (ID3D12Resource *)*ppvResource;
        log_line("  -> Tracked 24B index buffer = %p, Width=%llu, Layout=%d, "
                 "Alignment=%llu, State=0x%X\r\n",
                 *ppvResource, (unsigned long long)cleanDesc.Width,
                 cleanDesc.Layout, (unsigned long long)cleanDesc.Alignment,
                 (UINT)state);
      }
      WrapResource(ppvResource);
      RegisterReturnedResource(*ppvResource);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource(
    ID3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource,
    void **ppvResource) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  D3D12_RESOURCE_DESC cleanDesc =
      pDesc ? SanitizeResourceDesc(*pDesc) : D3D12_RESOURCE_DESC{};
  log_line("[d3d12_x] HookedCreateCommittedResource: caller=%p (game+0x%llX), "
           "pDevice=%p, Dim=%d, W=%llu, H=%u, DepthOrArray=%u, Mip=%u, Fmt=%d, "
           "Lay=%d->%d, state=0x%X\r\n",
           retAddr, (unsigned long long)rva, pDevice, cleanDesc.Dimension,
           (unsigned long long)cleanDesc.Width, cleanDesc.Height,
           cleanDesc.DepthOrArraySize, cleanDesc.MipLevels, cleanDesc.Format,
           pDesc ? pDesc->Layout : 0, cleanDesc.Layout,
           (UINT)InitialResourceState);

  if (pDevice) {
    HRESULT preReason = pDevice->GetDeviceRemovedReason();
    if (preReason != S_OK) {
      log_line("[d3d12_x] CRITICAL: Device was ALREADY REMOVED prior to "
               "HookedCreateCommittedResource! Reason=0x%08lX\r\n",
               (unsigned long)preReason);
      FlushD3D12InfoQueue("Prior to CreateCommittedResource");
    }
  }

  HRESULT hr = E_FAIL;
  D3D12_RESOURCE_STATES stateToUse = InitialResourceState;
  // Desktop rules the Xbox runtime does not enforce (some drivers accept the console values,
  // others fail with E_INVALIDARG and the game then dereferences the null resource, e.g.
  // exe+0xB9A99 on a tester's PC): non-custom heaps take no CPU page / memory pool preference,
  // upload heaps must start in GENERIC_READ, readback heaps in COPY_DEST, and only desktop heap
  // flags are allowed.
  D3D12_HEAP_PROPERTIES heapProps = pHeapProperties ? *pHeapProperties : D3D12_HEAP_PROPERTIES{};
  if (pHeapProperties && heapProps.Type != D3D12_HEAP_TYPE_CUSTOM) {
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  }
  if (pHeapProperties) {
    heapProps.CreationNodeMask = 0;
    heapProps.VisibleNodeMask = 0;
  }
  HeapFlags = (D3D12_HEAP_FLAGS)(HeapFlags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_DENY_BUFFERS |
                                              D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
                                              D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES |
                                              D3D12_HEAP_FLAG_HARDWARE_PROTECTED | D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH |
                                              D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT |
                                              D3D12_HEAP_FLAG_CREATE_NOT_ZEROED));
  if (pHeapProperties && heapProps.Type == D3D12_HEAP_TYPE_UPLOAD)
    stateToUse = D3D12_RESOURCE_STATE_GENERIC_READ;
  else if (pHeapProperties && heapProps.Type == D3D12_HEAP_TYPE_READBACK)
    stateToUse = D3D12_RESOURCE_STATE_COPY_DEST;
  else if (pHeapProperties && heapProps.Type == D3D12_HEAP_TYPE_DEFAULT &&
           stateToUse == D3D12_RESOURCE_STATE_COPY_DEST) {
    stateToUse = D3D12_RESOURCE_STATE_COMMON;
  }
  if (pHeapProperties) pHeapProperties = &heapProps;
  if (g_TrueCreateCommittedResource) {
    hr = g_TrueCreateCommittedResource(
        pDevice, pHeapProperties, HeapFlags, &cleanDesc, stateToUse,
        pOptimizedClearValue, riidResource, ppvResource);
    if (FAILED(hr)) {
      LogD3D12Failure(pDevice, "CreateCommittedResource", hr);
      log_line("  -> failed: heap type=%d flags=0x%X state=0x%X dim=%d w=%llu h=%u fmt=%d resflags=0x%X layout=%d align=%llu caller=game+0x%llX\r\n",
               pHeapProperties ? (int)pHeapProperties->Type : -1, (UINT)HeapFlags, (UINT)stateToUse,
               cleanDesc.Dimension, (unsigned long long)cleanDesc.Width, cleanDesc.Height, cleanDesc.Format,
               (UINT)cleanDesc.Flags, cleanDesc.Layout, (unsigned long long)cleanDesc.Alignment,
               (unsigned long long)rva);
      if (stateToUse != D3D12_RESOURCE_STATE_COMMON &&
          (!pHeapProperties || pHeapProperties->Type == D3D12_HEAP_TYPE_DEFAULT ||
           pHeapProperties->Type == D3D12_HEAP_TYPE_CUSTOM)) {
        hr = g_TrueCreateCommittedResource(
            pDevice, pHeapProperties, HeapFlags, &cleanDesc,
            D3D12_RESOURCE_STATE_COMMON, pOptimizedClearValue, riidResource,
            ppvResource);
        log_line("  -> Retry with COMMON returned hr=0x%08lX, res=%p\r\n",
                 (unsigned long)hr, *ppvResource);
        if (FAILED(hr)) {
          LogD3D12Failure(pDevice, "CreateCommittedResource->RetryCOMMON", hr);
        }
      }
    }
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
      if (pHeapProperties && pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD) {
        TrackUploadResource((ID3D12Resource *)*ppvResource);
      }
      TrackResourceState((ID3D12Resource *)*ppvResource, stateToUse);
      WrapResource(ppvResource);
      RegisterReturnedResource(*ppvResource);
    }
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandAllocatorX(
    ID3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE type, REFIID riid,
    void **ppCommandAllocator) {
  D3D12_COMMAND_LIST_TYPE mappedType = type;
  if ((int)type == 16) {
    mappedType = D3D12_COMMAND_LIST_TYPE_COPY;
  }
  log_line("[d3d12_x] HookedCreateCommandAllocatorX: rawType=%d -> "
           "mappedType=%d, ppv=%p\r\n",
           (int)type, (int)mappedType, ppCommandAllocator);

  if (!ppCommandAllocator)
    return E_POINTER;
  *ppCommandAllocator = nullptr;

  HRESULT hr = E_FAIL;
  if (g_TrueCreateCommandAllocator) {
    hr = g_TrueCreateCommandAllocator(pDevice, mappedType, riid,
                                      ppCommandAllocator);
    log_line("  -> CreateCommandAllocator returned hr=0x%08lX, alloc=%p\r\n",
             (unsigned long)hr, *ppCommandAllocator);

    if (FAILED(hr)) {
      LogD3D12Failure(pDevice, "CreateCommandAllocator", hr);
      // Fallback to DIRECT allocator
      hr = g_TrueCreateCommandAllocator(pDevice, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        riid, ppCommandAllocator);
      log_line("  -> Fallback to DIRECT returned hr=0x%08lX, alloc=%p\r\n",
               (unsigned long)hr, *ppCommandAllocator);
      if (FAILED(hr)) {
        LogD3D12Failure(pDevice, "CreateCommandAllocator->FallbackDIRECT", hr);
      }
    }
  }
  return hr;
}

// ============================================================================
// PSO Shader Bytecode Cache & Disassembler Dump
// ============================================================================
struct PsoShaderBlobs {
  std::vector<uint8_t> vsBytecode;
  std::vector<uint8_t> psBytecode;
  std::vector<uint8_t> csBytecode;
};
static SRWLOCK g_PsoShaderLock = SRWLOCK_INIT;
static std::unordered_map<void*, PsoShaderBlobs> g_PsoShaderMap;

typedef HRESULT (WINAPI *PFN_D3DDisassemble)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    UINT Flags,
    LPCSTR szComments,
    ID3DBlob** ppDisassembly
);
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines,
    ID3DInclude *pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs
);
static PFN_D3DDisassemble g_pfnD3DDisassemble = nullptr;
static PFN_D3DCompile g_pfnD3DCompile = nullptr;
static bool g_bLoadedD3DDisassemble = false;

static void EnsureD3DCompilerLoaded() {
  if (g_bLoadedD3DDisassemble) return;
  g_bLoadedD3DDisassemble = true;
  HMODULE hComp = LoadLibraryA("D3DCompiler_47.dll");
  if (!hComp) {
    hComp = LoadLibraryA("C:\\Windows\\System32\\D3DCompiler_47.dll");
  }
  if (hComp) {
    g_pfnD3DDisassemble = (PFN_D3DDisassemble)GetProcAddress(hComp, "D3DDisassemble");
    g_pfnD3DCompile = (PFN_D3DCompile)GetProcAddress(hComp, "D3DCompile");
    log_line("[d3d12_x] Loaded D3DCompiler_47.dll: D3DDisassemble=%p, D3DCompile=%p\r\n",
             g_pfnD3DDisassemble, g_pfnD3DCompile);
  }
}

static void DumpPsoShaders(void* pso) {
  if (!pso) return;
  static std::unordered_set<void*> s_DumpedPsos;
  static SRWLOCK s_dumpLock = SRWLOCK_INIT;

  AcquireSRWLockExclusive(&s_dumpLock);
  if (s_DumpedPsos.find(pso) != s_DumpedPsos.end()) {
    ReleaseSRWLockExclusive(&s_dumpLock);
    return;
  }
  s_DumpedPsos.insert(pso);
  ReleaseSRWLockExclusive(&s_dumpLock);

  EnsureD3DCompilerLoaded();

  PsoShaderBlobs blobs;
  AcquireSRWLockShared(&g_PsoShaderLock);
  auto it = g_PsoShaderMap.find(pso);
  if (it != g_PsoShaderMap.end()) {
    blobs = it->second;
  }
  ReleaseSRWLockShared(&g_PsoShaderLock);

  if (blobs.vsBytecode.empty() && blobs.psBytecode.empty() && blobs.csBytecode.empty()) {
    return;
  }

  // One file per unique shader, named by FNV-1a64 of the bytecode (the same key the .cso cache
  // uses), written only once. The old per-PSO names produced ~200k duplicate dumps.
  CreateDirectoryW(L"shaders", nullptr);
  auto dumpOne = [](const std::vector<uint8_t>& bc, const char* kind) {
    if (bc.empty()) return;
    char path[MAX_PATH];
    sprintf_s(path, "shaders\\pso_h%016llx_%s.dxbc", (unsigned long long)Fnv1a64Fwd(bc.data(), bc.size()), kind);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(bc.data(), 1, bc.size(), f); fclose(f); }
    log_line("[shader_dump] new %s shader -> %s\r\n", kind, path);
  };
  dumpOne(blobs.vsBytecode, "vs");
  dumpOne(blobs.psBytecode, "ps");
  dumpOne(blobs.csBytecode, "cs");
}

// ============================================================================
// BridgeCommandList Layout & Queue Unwrapping
// ============================================================================

volatile LONG g_rtLogFrame = 0;
#include "bridge_cmdlist.h"
// ONRUSH_CBV_PS: rows of every root CBV of the draw (float4 per line), for checking what the CPU feeds a shader
static void LogCbvDumpImpl(const D3D12_GPU_VIRTUAL_ADDRESS* cbv) {
  static volatile LONG s_call = 0; LONG call = InterlockedIncrement(&s_call);
  for (int pi = 0; pi < 8; ++pi) {
    D3D12_GPU_VIRTUAL_ADDRESS va = cbv[pi]; if (!va) continue;
    LONG n = g_placedBufCount; if (n > 20000) n = 20000; const PlacedBufInfo* hit = nullptr;
    for (LONG k = 0; k < n; ++k) { const PlacedBufInfo& q = g_placedBufs[k]; if (va >= q.va && va < q.va + q.size) { hit = &q; break; } }
    if (!hit) { char b[96]; sprintf_s(b, "[cbvdump] #%ld param%d va=%llX NOT-IN-PLACED\r\n", call, pi, (unsigned long long)va); log_line("%s", b); continue; }
    const float* f = (const float*)((const uint8_t*)hit->cpu + (va - hit->va)); UINT64 rows = (hit->size - (va - hit->va)) / 16; if (rows > 150) rows = 150;
    for (UINT64 r = 0; r < rows; ++r) {
      if (!(r < 26 || r == 77 || r == 123 || r == 141)) continue;
      char b[200]; sprintf_s(b, "[cbvdump] #%ld p%d row%llu %g %g %g %g\r\n", call, pi, (unsigned long long)r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]); log_line("%s", b);
    }
  }
}
static struct CbvDumpInit { CbvDumpInit() { g_pfnLogCbvDump = LogCbvDumpImpl; } } g_cbvDumpInit;
#include "upscaler.inc"

typedef void(STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(
    ID3D12CommandQueue *pQueue, UINT NumCommandLists,
    ID3D12CommandList *const *ppCommandLists);
static PFN_ExecuteCommandLists g_TrueExecuteCommandLists = nullptr;
static void *g_WrappedQueueVtable[32] = {};
static bool g_QueueVtableInitialized = false;

// ---- Execute-time state prologue (see "Per-command-list resource state tracking") ----
struct PrologueSlot { ID3D12CommandAllocator *alloc; ID3D12GraphicsCommandList *list; UINT64 fence; };
struct PrologueQueueState {
  ID3D12Fence *fence = nullptr;
  UINT64 nextValue = 0;
  std::vector<PrologueSlot> freeSlots, busySlots;
};
static SRWLOCK g_prologueLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12CommandQueue *, PrologueQueueState> g_prologueQueues;
static volatile LONG g_prologueCount = 0, g_prologueBarriers = 0;

// Records `bars` into a pooled command list of the queue's type; returns it (closed) or nullptr.
static ID3D12CommandList *AcquirePrologueList(ID3D12CommandQueue *q, D3D12_COMMAND_LIST_TYPE type,
                                              const std::vector<D3D12_RESOURCE_BARRIER> &bars,
                                              PrologueQueueState **outQs, PrologueSlot **outSlot) {
  if (!g_pMainDevice) return nullptr;
  PrologueQueueState &qs = g_prologueQueues[q];
  if (!qs.fence) {
    if (FAILED(g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&qs.fence))) return nullptr;
  }
  UINT64 done = qs.fence->GetCompletedValue();
  for (size_t i = 0; i < qs.busySlots.size();) {
    if (qs.busySlots[i].fence <= done) { qs.freeSlots.push_back(qs.busySlots[i]); qs.busySlots.erase(qs.busySlots.begin() + i); }
    else ++i;
  }
  PrologueSlot slot = {};
  if (!qs.freeSlots.empty()) { slot = qs.freeSlots.back(); qs.freeSlots.pop_back(); }
  else {
    if (FAILED(g_pMainDevice->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), (void **)&slot.alloc))) return nullptr;
    if (FAILED(g_pMainDevice->CreateCommandList(0, type, slot.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&slot.list))) return nullptr;
    slot.list->Close();
  }
  slot.alloc->Reset();
  if (FAILED(slot.list->Reset(slot.alloc, nullptr))) return nullptr;
  // D3D12 limits a single ResourceBarrier call; chunk it.
  for (size_t off = 0; off < bars.size(); off += 256) {
    UINT n = (UINT)((bars.size() - off) < 256 ? (bars.size() - off) : 256);
    slot.list->ResourceBarrier(n, bars.data() + off);
  }
  if (FAILED(slot.list->Close())) { qs.freeSlots.push_back(slot); return nullptr; }
  slot.fence = 0;
  qs.busySlots.push_back(slot);
  *outQs = &qs; *outSlot = &qs.busySlots.back();
  return slot.list;
}

// Queue Signal/Wait history (ring buffer), printed by the hang watchdog: a queue that waits on a
// fence value nobody signals stalls every later Signal on that queue (the frame fence included).
typedef HRESULT(STDMETHODCALLTYPE *PFN_QueueFenceOp)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);
static PFN_QueueFenceOp g_TrueQueueSignal = nullptr, g_TrueQueueWait = nullptr;
struct QueueFenceEvent { ID3D12CommandQueue *q; ID3D12Fence *f; UINT64 v; char op; ULONGLONG t; uintptr_t caller; };
static QueueFenceEvent g_qfe[256];
static volatile LONG g_qfeN = 0;
static void RecordQueueFenceOp(char op, ID3D12CommandQueue *q, ID3D12Fence *f, UINT64 v, void *caller) {
  LONG i = InterlockedIncrement(&g_qfeN) - 1;
  QueueFenceEvent &e = g_qfe[i & 255];
  e.q = q; e.f = f; e.v = v; e.op = op; e.t = GetTickCount64();
  uintptr_t hx = (uintptr_t)GetModuleHandleW(nullptr);
  e.caller = (uintptr_t)caller >= hx ? (uintptr_t)caller - hx : 0;
}
static HRESULT STDMETHODCALLTYPE HookedQueueSignal(ID3D12CommandQueue *q, ID3D12Fence *f, UINT64 v) {
  RecordQueueFenceOp('S', q, f, v, _ReturnAddress());
  return g_TrueQueueSignal(q, f, v);
}
static HRESULT STDMETHODCALLTYPE HookedQueueWait(ID3D12CommandQueue *q, ID3D12Fence *f, UINT64 v) {
  RecordQueueFenceOp('W', q, f, v, _ReturnAddress());
  return g_TrueQueueWait(q, f, v);
}
static void DumpQueueFenceHistory() {
  LONG n = g_qfeN;
  LONG from = n > 64 ? n - 64 : 0;
  ULONGLONG now = GetTickCount64();
  for (LONG i = from; i < n; ++i) {
    const QueueFenceEvent &e = g_qfe[i & 255];
    UINT64 done = 0;
    __try { done = e.f ? e.f->GetCompletedValue() : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    log_line("[hang] %c queue=%p type=%d fence=%p value=%llu (completed now %llu) %llums ago caller=exe+0x%llX\r\n",
             e.op, e.q, e.q ? (int)e.q->GetDesc().Type : -1, e.f, (unsigned long long)e.v, (unsigned long long)done,
             (unsigned long long)(now - e.t), (unsigned long long)e.caller);
  }
}

static void STDMETHODCALLTYPE
HookedExecuteCommandLists(ID3D12CommandQueue *pQueue, UINT NumCommandLists,
                          ID3D12CommandList *const *ppCommandLists) {
  void *caller = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva = (hExe && caller >= hExe) ? ((uintptr_t)caller - (uintptr_t)hExe) : 0;
  static volatile LONG s_execLog = 0;
  if (InterlockedIncrement(&s_execLog) <= 200)
  log_line("[d3d12_x] CommandQueue::ExecuteCommandLists(pQueue=%p, NumLists=%u, ppLists=%p) caller=game+0x%llX\r\n",
           pQueue, NumCommandLists, ppCommandLists, (unsigned long long)rva);

  if (!ppCommandLists || NumCommandLists == 0) {
    if (g_TrueExecuteCommandLists)
      g_TrueExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
    return;
  }

  Ups_OnExecute();
  if (pQueue && pQueue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_LastDirectQueue = pQueue;
  std::vector<ID3D12CommandList *> validLists;
  validLists.reserve(NumCommandLists * 2);
  std::vector<std::pair<ID3D12CommandQueue *, ID3D12GraphicsCommandList *>> prologueUsed;
  for (UINT i = 0; i < NumCommandLists; ++i) {
    BridgeCommandList *pBridge = nullptr;
    if (ppCommandLists[i] && (uintptr_t)ppCommandLists[i] > 0x10000) {
      BridgeCommandList *b = (BridgeCommandList *)ppCommandLists[i];
      if (b->m_magic == BRIDGE_CMDLIST_MAGIC) {
        pBridge = b;
      }
    }
    ID3D12CommandList *unwrapped = UnwrapCommandList(ppCommandLists[i]);
    if (pBridge) {
      if (!pBridge->m_isClosed) {
        log_line("  cmdList[%u] %p is open! Calling Close()...\r\n", i, pBridge);
        HRESULT closeHr = pBridge->m_pRealCmdList->Close();
        log_line("  -> Close() returned hr=0x%08lX\r\n", (unsigned long)closeHr);
        if (SUCCEEDED(closeHr)) {
          pBridge->m_isClosed = true;
        }
      }
      if (pBridge->m_isClosed && unwrapped) {
        {
          // Reconcile resource states with what this list assumed when it was recorded.
          D3D12_COMMAND_LIST_TYPE qType = pQueue ? pQueue->GetDesc().Type : D3D12_COMMAND_LIST_TYPE_DIRECT;
          std::vector<D3D12_RESOURCE_BARRIER> pro;
          BuildListPrologue(pBridge, qType, pro);
          if (!pro.empty() && qType != D3D12_COMMAND_LIST_TYPE_COPY) {
            AcquireSRWLockExclusive(&g_prologueLock);
            PrologueQueueState *pqs = nullptr; PrologueSlot *pslot = nullptr;
            ID3D12CommandList *pl = AcquirePrologueList(pQueue, qType, pro, &pqs, &pslot);
            if (pl) {
              validLists.push_back(pl);
              prologueUsed.push_back({ pQueue, pslot ? pslot->list : nullptr });
              InterlockedIncrement(&g_prologueCount); InterlockedAdd(&g_prologueBarriers, (LONG)pro.size());
            }
            ReleaseSRWLockExclusive(&g_prologueLock);
          }
          CommitListStates(pBridge);
        }
        BcRecordExec(pQueue, pBridge);
        validLists.push_back(unwrapped);
        static volatile LONG s_subLog = 0;
        if (InterlockedIncrement(&s_subLog) <= 60) {
          log_line("  cmdList[%u] %p (bridge=%p, closed=1) -> SUBMITTING unwrapped %p\r\n",
                   i, ppCommandLists[i], pBridge, unwrapped);
        }
      } else {
        log_line("  cmdList[%u] %p (bridge=%p, closed=0) -> DROPPING unwrapped %p to protect queue!\r\n",
                 i, ppCommandLists[i], pBridge, unwrapped);
      }
    } else if (unwrapped) {
      validLists.push_back(unwrapped);
    }
  }

  if (!validLists.empty() && g_TrueExecuteCommandLists)
    g_TrueExecuteCommandLists(pQueue, (UINT)validLists.size(), validLists.data());
  if (!prologueUsed.empty()) {
    AcquireSRWLockExclusive(&g_prologueLock);
    PrologueQueueState &qs = g_prologueQueues[pQueue];
    if (qs.fence) {
      UINT64 v = ++qs.nextValue;
      pQueue->Signal(qs.fence, v);
      for (auto &pu : prologueUsed)
        for (auto &bs : qs.busySlots) if (bs.list == pu.second && bs.fence == 0) bs.fence = v;
    }
    ReleaseSRWLockExclusive(&g_prologueLock);
  }

  FlushD3D12InfoQueue("ExecuteCommandLists");
  FlushDXGIInfoQueue("ExecuteCommandLists");

  ID3D12Device *pDev = nullptr;
  if (pQueue && SUCCEEDED(pQueue->GetDevice(__uuidof(ID3D12Device), (void**)&pDev)) && pDev) {
    HRESULT reason = pDev->GetDeviceRemovedReason();
    if (FAILED(reason)) {
      log_line("  !!! CRITICAL: Device REMOVED during ExecuteCommandLists on queue %p! Reason=0x%08lX\r\n",
               pQueue, (unsigned long)reason);
      { static volatile LONG s_bcOnce = 0; if (InterlockedExchange(&s_bcOnce, 1) == 0) DumpBreadcrumbs(); }  // TDR can fire before the 6 s hang watchdog
      LogDredOnce(pDev);
      FlushD3D12InfoQueue("ExecuteCommandLists_DeviceRemoved");
      FlushDXGIInfoQueue("ExecuteCommandLists_DeviceRemoved");
    }
    pDev->Release();
  }
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandQueueX(
    ID3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc,
    const IID *priid, void **ppCommandQueue) {
  IID riid = priid ? *priid : __uuidof(ID3D12CommandQueue);
  D3D12_COMMAND_QUEUE_DESC desc = pDesc ? *pDesc : D3D12_COMMAND_QUEUE_DESC{};
  if ((int)desc.Type == 16) {
    desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
  }
  // Durango queue flags may contain Xbox-specific bits (e.g. 0x2).
  // Desktop D3D12 only permits D3D12_COMMAND_QUEUE_FLAG_NONE (0) or
  // DISABLE_GPU_TIMEOUT (1).
  desc.Flags =
      (D3D12_COMMAND_QUEUE_FLAGS)(desc.Flags &
                                  D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT);
  // 1.80 passes Xbox priority encodings (e.g. 0x0D000000); desktop accepts NORMAL(0),
  // HIGH(100) and GLOBAL_REALTIME(10000, needs privileges) only.
  if (desc.Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
      desc.Priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH)
    desc.Priority = (desc.Priority > 0) ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

  log_line("[d3d12_x] HookedCreateCommandQueueX: type=%u (mapped=%u), "
           "priority=%d, flags=0x%X, ppQueue=%p\r\n",
           pDesc ? pDesc->Type : 0, (UINT)desc.Type, desc.Priority, desc.Flags,
           ppCommandQueue);
  if (!ppCommandQueue)
    return E_POINTER;
  *ppCommandQueue = nullptr;
  HRESULT hr = E_FAIL;
  if (g_TrueCreateCommandQueue) {
    hr = g_TrueCreateCommandQueue(pDevice, &desc, riid, ppCommandQueue);
    if (FAILED(hr) && desc.Flags != D3D12_COMMAND_QUEUE_FLAG_NONE) {
      desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
      hr = g_TrueCreateCommandQueue(pDevice, &desc, riid, ppCommandQueue);
    }
    if (FAILED(hr) && desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      hr = g_TrueCreateCommandQueue(pDevice, &desc, riid, ppCommandQueue);
    }
    log_line("  -> CreateCommandQueue returned hr=0x%08lX, queue=%p\r\n",
             (unsigned long)hr, *ppCommandQueue);
    if (SUCCEEDED(hr) && *ppCommandQueue) {
      if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_pDirectCommandQueue = (ID3D12CommandQueue *)*ppCommandQueue;
        log_line(
            "[d3d12_x] Captured DIRECT CommandQueue: %p (desc.Type=%u)\r\n",
            g_pDirectCommandQueue, (UINT)desc.Type);
      }

      // Hook ExecuteCommandLists (slot 10 of ID3D12CommandQueue) to unwrap
      // BridgeCommandList
      void **origQueueVtbl = *(void ***)(*ppCommandQueue);
      if (!g_QueueVtableInitialized) {
        memcpy(g_WrappedQueueVtable, origQueueVtbl, sizeof(void *) * 32);
        g_TrueExecuteCommandLists = (PFN_ExecuteCommandLists)origQueueVtbl[10];
        g_WrappedQueueVtable[10] = (void *)&HookedExecuteCommandLists;
        g_TrueQueueSignal = (PFN_QueueFenceOp)origQueueVtbl[14];
        g_TrueQueueWait = (PFN_QueueFenceOp)origQueueVtbl[15];
        g_WrappedQueueVtable[14] = (void *)&HookedQueueSignal;
        g_WrappedQueueVtable[15] = (void *)&HookedQueueWait;
        g_QueueVtableInitialized = true;
        log_line("[d3d12_x] Intercepted "
                 "ID3D12CommandQueue::ExecuteCommandLists (slot 10)\r\n");
      }
      *(void ***)(*ppCommandQueue) = g_WrappedQueueVtable;
    }
    if (FAILED(hr)) {
      LogD3D12Failure(pDevice, "CreateCommandQueue", hr);
    }
  }
  return hr;
}

static ID3DBlob* g_pBlobFullScreenVS = nullptr;
static ID3DBlob* g_pBlobBootImagePS = nullptr;
static ID3DBlob* g_pBlobOverlayPS = nullptr;

static bool CompileShaderBlob(const char* filename, const char* defaultSource, const char* target, ID3DBlob** ppBlob) {
  EnsureD3DCompilerLoaded();
  if (!g_pfnD3DCompile) return false;

  std::string source = defaultSource;
  char filePath[MAX_PATH];
  sprintf_s(filePath, "shaders\\%s", filename);
  FILE* f = fopen(filePath, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0) {
      source.resize(sz);
      fread(&source[0], 1, sz, f);
      log_line("[d3d12_x] Loaded custom shader override from %s (%ld bytes)\r\n", filePath, sz);
    }
    fclose(f);
  } else {
    CreateDirectoryW(L"shaders", nullptr);
    FILE* fw = fopen(filePath, "wb");
    if (fw) {
      fwrite(defaultSource, 1, strlen(defaultSource), fw);
      fclose(fw);
    }
  }

  ID3DBlob* pCode = nullptr;
  ID3DBlob* pError = nullptr;
  HRESULT hr = g_pfnD3DCompile(source.data(), source.size(), filename, nullptr, nullptr,
                               "main", target, 0, 0, &pCode, &pError);
  if (FAILED(hr)) {
    if (pError) {
      log_line("[d3d12_x] D3DCompile failed for %s (%s): %s\r\n",
               filename, target, (const char*)pError->GetBufferPointer());
      pError->Release();
    }
    return false;
  }
  if (pError) pError->Release();
  *ppBlob = pCode;
  return true;
}

// ---- Auto-translated GCN shaders (scripts/diag/build_shader_cache.py) -------------------------------------------
static uint64_t Fnv1a64(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data; uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}
// A DXIL (SM6) container has a 'DXIL' part; DXBC shaders have 'SHEX'/'SHDR'.
static bool IsDxilBlob(ID3DBlob* b) {
  if (!b || b->GetBufferSize() < 32) return false;
  const uint8_t* p = (const uint8_t*)b->GetBufferPointer(); size_t n = b->GetBufferSize();
  for (size_t i = 0; i + 4 <= n && i < 4096; ++i) if (p[i] == 'D' && p[i+1] == 'X' && p[i+2] == 'I' && p[i+3] == 'L') return true;
  return false;
}
static ID3DBlob* LoadDxilTwin(const void* bytecode, size_t len) {
  uint64_t h = 0xcbf29ce484222325ull; const uint8_t* q = (const uint8_t*)bytecode;
  for (size_t i = 0; i < len; ++i) { h ^= q[i]; h *= 0x100000001b3ull; }
  char path[MAX_PATH]; sprintf_s(path, "shaders\\translated\\%016llx.dxil.cso", (unsigned long long)h);
  FILE* f = fopen(path, "rb"); if (!f) return nullptr;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  ID3DBlob* blob = nullptr; EnsureD3DCompilerLoaded();
  typedef HRESULT(WINAPI* PFN_CreateBlob)(SIZE_T, ID3DBlob**); static PFN_CreateBlob s_cb = nullptr;
  if (!s_cb) { HMODULE m = GetModuleHandleA("d3dcompiler_47.dll"); if (!m) m = LoadLibraryA("d3dcompiler_47.dll"); if (m) s_cb = (PFN_CreateBlob)GetProcAddress(m, "D3DCreateBlob"); }
  if (sz > 0 && s_cb && SUCCEEDED(s_cb((SIZE_T)sz, &blob)) && blob) { fread(blob->GetBufferPointer(), 1, (size_t)sz, f); psodc::NoteShaderBlob(blob, h, psocache::SH_DXIL); }
  fclose(f); return blob;
}
static std::unordered_set<ID3DBlob*> g_simplePsBlob;  // debug flat PS (ONRUSH_DEBUG_SIMPLEPS): stays DXBC
// Returns a compiled replacement for an original Durango DXBC (looked up by FNV-1a64 of its bytes), or nullptr.
static ID3DBlob* GetTranslatedShader(const void* bytecode, size_t len, const char* target) {
  static std::unordered_map<uint64_t, ID3DBlob*> s_cache;   // nullptr value = negative cache
  static SRWLOCK s_lock = SRWLOCK_INIT;
  uint64_t h = Fnv1a64(bytecode, len);
  AcquireSRWLockExclusive(&s_lock);
  auto it = s_cache.find(h);
  if (it != s_cache.end()) { ID3DBlob* b = it->second; ReleaseSRWLockExclusive(&s_lock); return b; }
  ID3DBlob* blob = nullptr;
  char path[MAX_PATH];
  // Debug: ONRUSH_LOG_PSLIST=1 writes every PS hash the game asks for to shaders\ps_used.txt.
  if (target[0] == 'p' && GetEnvironmentVariableA("ONRUSH_LOG_PSLIST", nullptr, 0) > 0) {
    FILE* fl = fopen("shaders\\ps_used.txt", "a");
    if (fl) { fprintf(fl, "%016llx\n", (unsigned long long)h); fclose(fl); }
  }
  // Debug: ONRUSH_DEBUG_SIMPLEPS=1 replaces every PS with a flat one; a comma list of hashes (or
  // @file with one hash per line) replaces only those: magenta on all targets, to see who draws what.
  static std::string s_simpleList; static int s_simpleMode = -1;
  if (s_simpleMode < 0) {
    DWORD n = GetEnvironmentVariableA("ONRUSH_DEBUG_SIMPLEPS", nullptr, 0);
    std::vector<char> v(n + 1, 0);
    if (n) GetEnvironmentVariableA("ONRUSH_DEBUG_SIMPLEPS", v.data(), n + 1);
    s_simpleMode = n == 0 ? 0 : (strcmp(v.data(), "1") == 0 ? 1 : 2);
    if (s_simpleMode == 2) {
      if (v[0] == '@') { FILE* fs = fopen(v.data() + 1, "rb"); if (fs) { char buf[4096]; size_t k; while ((k = fread(buf, 1, sizeof(buf), fs)) > 0) s_simpleList.append(buf, k); fclose(fs); } }
      else s_simpleList = v.data();
      log_line("[xlate] DEBUG simple PS list: %zu chars\r\n", s_simpleList.size());
      for (auto& c : s_simpleList) c = (char)tolower((unsigned char)c);
    }
  }
  bool simpleThis = s_simpleMode == 1;
  int simpleGroup = 0;  // list line "<hash> <digit>" picks the colour group (default 0 = green)
  if (s_simpleMode == 2) {
    char hx[20]; sprintf_s(hx, "%016llx", (unsigned long long)h);
    size_t at = s_simpleList.find(hx); simpleThis = at != std::string::npos;
    if (simpleThis && at + 17 < s_simpleList.size() && s_simpleList[at + 16] == ' ' && isdigit((unsigned char)s_simpleList[at + 17]))
      simpleGroup = s_simpleList[at + 17] - '0';
  }
  if (target[0] == 'p' && simpleThis) {
    static const char* kCol[8] = { "0,1,0", "1,0,0", "0,0,1", "1,1,0", "0,1,1", "1,0,1", "1,1,1", "1,0.5,0" };
    char kSimple[512];
    sprintf_s(kSimple, "struct O { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3; };\n"
              " O main() { float4 c = float4(%s,1); O o; o.c0 = c; o.c1 = c; o.c2 = c; o.c3 = c; return o; }\n", kCol[simpleGroup & 7]);
    EnsureD3DCompilerLoaded(); ID3DBlob* er2 = nullptr; ID3DBlob* bl2 = nullptr;
    if (g_pfnD3DCompile && SUCCEEDED(g_pfnD3DCompile(kSimple, strlen(kSimple), "simple", nullptr, nullptr, "main", target, 0, 0, &bl2, &er2)) && bl2) {
      log_line("[xlate] DEBUG simple PS for %016llx\r\n", (unsigned long long)h); g_simplePsBlob.insert(bl2); s_cache[h] = bl2; ReleaseSRWLockExclusive(&s_lock); return bl2; }
  }
  if (target[0] == 'v' && GetEnvironmentVariableA("ONRUSH_DEBUG_POSDUMP", nullptr, 0) > 0) {
    // Debug: the translated VS also stores SV_Position of vertices 0..3 into g_CtrBuf at 64 KB + slot*64 (read back at Present).
    static int s_slot = 0;
    char hp[MAX_PATH]; sprintf_s(hp, "shaders\\translated\\%016llx.hlsl", (unsigned long long)h);
    FILE* fh = fopen(hp, "rb");
    if (fh && s_slot < 64) { fseek(fh, 0, SEEK_END); long hs = ftell(fh); fseek(fh, 0, SEEK_SET); std::string src(hs > 0 ? hs : 0, 0); if (hs > 0) fread(&src[0], 1, hs, fh); fclose(fh); fh = nullptr;
      size_t at = src.find("o.pos = pos0_o;");
      if (at != std::string::npos && src.find("uint vid : SV_VertexID") != std::string::npos) {
        char ins[256]; sprintf_s(ins, "o.pos = pos0_o; g_dbg.Store4(%uu + (vid & 3u) * 16u, asuint(pos0_o));", 32768u + (unsigned)s_slot * 64u);
        src.replace(at, 15, ins); src = std::string("RWByteAddressBuffer g_dbg : register(u200, space99);\n") + src;
        EnsureD3DCompilerLoaded(); ID3DBlob* er2 = nullptr; ID3DBlob* bl2 = nullptr;
        if (g_pfnD3DCompile && SUCCEEDED(g_pfnD3DCompile(src.data(), src.size(), hp, nullptr, nullptr, "main", target, 0, 0, &bl2, &er2)) && bl2) {
          log_line("[posdump] slot %d = VS %016llx\r\n", s_slot, (unsigned long long)h); ++s_slot; s_cache[h] = bl2; ReleaseSRWLockExclusive(&s_lock); return bl2; }
        if (er2) { log_line("[posdump] compile failed %016llx: %.300s\r\n", (unsigned long long)h, (const char*)er2->GetBufferPointer()); er2->Release(); }
      } }
    if (fh) fclose(fh);
  }
  if (target[0] == 'v' && GetEnvironmentVariableA("ONRUSH_DEBUG_FULLVIS", nullptr, 0) > 0) {
    char hp[MAX_PATH]; sprintf_s(hp, "shaders\\translated\\%016llx.hlsl", (unsigned long long)h);
    FILE* fh = fopen(hp, "rb");
    if (fh) { fseek(fh, 0, SEEK_END); long hs = ftell(fh); fseek(fh, 0, SEEK_SET); std::string src(hs > 0 ? hs : 0, 0); if (hs > 0) fread(&src[0], 1, hs, fh); fclose(fh);
      size_t at = src.find("o.pos = pos0_o;");
      if (at != std::string::npos && src.find("uint vid : SV_VertexID") != std::string::npos) {
        src.replace(at, 15, "o.pos = float4(float(vid & 63) / 32.0 - 1.0, float((vid >> 6) & 63) / 32.0 - 1.0, 0.5, 1.0);");
        EnsureD3DCompilerLoaded(); ID3DBlob* er2 = nullptr; ID3DBlob* bl2 = nullptr;
        if (g_pfnD3DCompile && SUCCEEDED(g_pfnD3DCompile(src.data(), src.size(), hp, nullptr, nullptr, "main", target, 0, 0, &bl2, &er2)) && bl2) {
          log_line("[xlate] DEBUG full-vis VS %016llx\r\n", (unsigned long long)h); s_cache[h] = bl2; ReleaseSRWLockExclusive(&s_lock); return bl2; }
      } } }
  if (target[0] == 'v' && GetEnvironmentVariableA("ONRUSH_DEBUG_MESHVS", nullptr, 0) > 0) {
    char hp[MAX_PATH]; sprintf_s(hp, "shaders\\translated\\%016llx.hlsl", (unsigned long long)h);
    FILE* fh = fopen(hp, "rb");
    if (fh) { fseek(fh, 0, SEEK_END); long hs = ftell(fh); fseek(fh, 0, SEEK_SET); std::string src(hs > 0 ? hs : 0, 0); if (hs > 0) fread(&src[0], 1, hs, fh); fclose(fh);
      size_t at = src.find("o.pos = pos0_o;");
      if (src.find("in_POSITION0") != std::string::npos && at != std::string::npos) {
        src.replace(at, 15, "o.pos = float4(in_POSITION0.x * 0.0015, in_POSITION0.z * 0.0025, 0.5, 1.0);");
        EnsureD3DCompilerLoaded(); ID3DBlob* er2 = nullptr; ID3DBlob* bl2 = nullptr;
        if (g_pfnD3DCompile && SUCCEEDED(g_pfnD3DCompile(src.data(), src.size(), hp, nullptr, nullptr, "main", target, 0, 0, &bl2, &er2)) && bl2) {
          log_line("[xlate] DEBUG mesh VS %016llx (position passthrough)\r\n", (unsigned long long)h); s_cache[h] = bl2; ReleaseSRWLockExclusive(&s_lock); return bl2; }
      } } }
  {
    char cso[MAX_PATH]; sprintf_s(cso, "shaders\\translated\\%016llx.cso", (unsigned long long)h);
    FILE* fc = fopen(cso, "rb");
    if (fc) {
      EnsureD3DCompilerLoaded();
      typedef HRESULT (WINAPI *PFN_D3DCreateBlob)(SIZE_T, ID3DBlob**);
      static PFN_D3DCreateBlob pCreateBlob = nullptr;
      if (!pCreateBlob) { HMODULE hc = GetModuleHandleA("D3DCompiler_47.dll"); if (hc) pCreateBlob = (PFN_D3DCreateBlob)GetProcAddress(hc, "D3DCreateBlob"); }
      fseek(fc, 0, SEEK_END); long csz = ftell(fc); fseek(fc, 0, SEEK_SET);
      { static bool s_skipInit = false; static std::unordered_set<std::string> s_skip; if (!s_skipInit) { s_skipInit = true; char sp[MAX_PATH]; if (GetEnvironmentVariableA("ONRUSH_CS_SKIPFILE", sp, sizeof(sp)) == 0) strcpy_s(sp, "shaders\\translated\\skip.txt"); if (sp[0]) { FILE* sf = fopen(sp, "rb"); if (sf) { char ln[64]; while (fgets(ln, sizeof(ln), sf)) { std::string t(ln); while (!t.empty() && (t.back() == 10 || t.back() == 13 || t.back() == 32)) t.pop_back(); if (!t.empty()) s_skip.insert(t); } fclose(sf); } } }
        char hx[32]; sprintf_s(hx, "%016llx", (unsigned long long)h);
        if (s_skip.count(hx)) { fclose(fc); s_cache[h] = nullptr; ReleaseSRWLockExclusive(&s_lock); return nullptr; } }
      { static long s_maxCso = -1; if (s_maxCso == -1) { char mb[16]; s_maxCso = GetEnvironmentVariableA("ONRUSH_CS_MAXBYTES", mb, sizeof(mb)) > 0 ? atol(mb) : 0; }
        if (target[0] == 'c' && s_maxCso > 0 && csz > s_maxCso) { fclose(fc); log_line("[xlate] %s len=%zu hash=%016llx SKIPPED (cso %ld > max)\r\n", target, len, (unsigned long long)h, csz); s_cache[h] = nullptr; ReleaseSRWLockExclusive(&s_lock); return nullptr; } }
      if (csz > 0 && pCreateBlob && SUCCEEDED(pCreateBlob((SIZE_T)csz, &blob)) && blob) { fread(blob->GetBufferPointer(), 1, csz, fc); psodc::NoteShaderBlob(blob, h, psocache::SH_CSO); } else blob = nullptr;
      fclose(fc);
      log_line("[xlate] %s len=%zu hash=%016llx FOUND(cso) bc=%p\r\n", target, len, (unsigned long long)h, bytecode);
      s_cache[h] = blob; ReleaseSRWLockExclusive(&s_lock); return blob;
    }
  }
  sprintf_s(path, "shaders\\translated\\%016llx.hlsl", (unsigned long long)h);
  FILE* f = (target[0] == 'c') ? nullptr : fopen(path, "rb");
  log_line("[xlate] %s len=%zu hash=%016llx %s bc=%p\r\n", target, len, (unsigned long long)h, f ? "FOUND" : "missing", bytecode);
  if (!f) {  // keep the original bytecode so the offline translator (build_shader_cache.py) can pick it up
    CreateDirectoryW(L"shaders", nullptr);
    char dp[MAX_PATH]; sprintf_s(dp, "shaders\\pso_h%016llx_%c%c.dxbc", (unsigned long long)h, target[0], target[1]);
    if (GetFileAttributesA(dp) == INVALID_FILE_ATTRIBUTES) { FILE* fd = fopen(dp, "wb"); if (fd) { fwrite(bytecode, 1, len, fd); fclose(fd); log_line("[shader_dump] missing %s -> %s\r\n", target, dp); } }
  }
  if (f) {
    EnsureD3DCompilerLoaded();
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::string src; if (sz > 0) { src.resize(sz); fread(&src[0], 1, sz, f); }
    fclose(f);
    ID3DBlob* err = nullptr;
    if (g_pfnD3DCompile && !src.empty()) {
      HRESULT hr = g_pfnD3DCompile(src.data(), src.size(), path, nullptr, nullptr, "main", target, 0, 0, &blob, &err);
      if (FAILED(hr)) {
        log_line("[d3d12_x] translated shader %016llx failed to compile: %s\r\n", (unsigned long long)h,
                 err ? (const char*)err->GetBufferPointer() : "?");
        blob = nullptr;
      }
    }
    if (err) err->Release();
  }
  s_cache[h] = blob;
  ReleaseSRWLockExclusive(&s_lock);
  return blob;
}

static void EnsureReplacementShaders() {
  static bool s_inited = false;
  if (s_inited) return;
  s_inited = true;

  const char* vs_code =
      "struct VSOutput {\n"
      "    float4 pos : SV_POSITION;\n"
      "    float2 uv  : TEXCOORD0;\n"
      "};\n"
      "VSOutput main(uint vertexID : SV_VertexID) {\n"
      "    VSOutput output;\n"
      "    output.uv = float2((vertexID << 1) & 2, vertexID & 2);\n"
      "    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
      "    return output;\n"
      "}\n";

  const char* boot_ps_code =
      "Texture2D g_Textures[13] : register(t0, space7);\n"
      "SamplerState g_Samplers[9] : register(s0, space0);\n"
      "struct PSInput {\n"
      "    float4 pos : SV_POSITION;\n"
      "    float2 uv  : TEXCOORD0;\n"
      "};\n"
      "float4 main(PSInput input) : SV_TARGET {\n"
      "    float3 bg = float3(0.04f, 0.05f, 0.09f);\n"
      "    float4 t0 = g_Textures[0].Sample(g_Samplers[0], input.uv);\n"
      "    float waveIntensity = t0.r;\n"
      "    float3 waveColor = lerp(float3(0.05f, 0.35f, 0.85f), float3(0.0f, 0.85f, 0.95f), input.uv.x);\n"
      "    bg += waveColor * waveIntensity * 0.8f;\n"
      "    float4 t10 = g_Textures[10].Sample(g_Samplers[0], input.uv);\n"
      "    float logoAlpha = saturate(t10.r);\n"
      "    float3 logoColor = float3(1.0f, 1.0f, 1.0f);\n"
      "    float3 finalColor = lerp(bg, logoColor, logoAlpha);\n"
      "    return float4(finalColor, 1.0f);\n"
      "}\n";

  const char* overlay_ps_code =
      "Texture2D g_Textures[19] : register(t0, space51);\n"
      "SamplerState g_Samplers[9] : register(s0, space0);\n"
      "cbuffer Params : register(b1, space0) {\n"
      "    float4 g_params[6];\n"
      "};\n"
      "struct PSInput {\n"
      "    float4 pos : SV_POSITION;\n"
      "    float2 uv  : TEXCOORD0;\n"
      "};\n"
      "float4 main(PSInput input) : SV_TARGET {\n"
      "    float4 overlay = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
      "    bool hasOverlay = false;\n"
      "    for (int i = 0; i < 19; ++i) {\n"
      "        float4 c = g_Textures[i].Sample(g_Samplers[0], input.uv);\n"
      "        if (any(c.rgb > 0.0f) || c.a > 0.0f) {\n"
      "            float3 col = (c.g == 0.0f && c.b == 0.0f) ? c.rrr : c.rgb;\n"
      "            float a = (c.a > 0.0f) ? c.a : saturate(max(col.r, max(col.g, col.b)));\n"
      "            overlay.rgb = lerp(overlay.rgb, col, a);\n"
      "            overlay.a = max(overlay.a, a);\n"
      "            hasOverlay = true;\n"
      "        }\n"
      "    }\n"
      "    if (!hasOverlay) {\n"
      "        discard;\n"
      "    }\n"
      "    return overlay;\n"
      "}\n";

  if (CompileShaderBlob("FullScreenVS.hlsl", vs_code, "vs_5_1", &g_pBlobFullScreenVS)) {
    log_line("[d3d12_x] Compiled replacement FullScreenVS (size=%zu bytes)\r\n",
             g_pBlobFullScreenVS->GetBufferSize());
  }
  if (CompileShaderBlob("BootImagePS.hlsl", boot_ps_code, "ps_5_1", &g_pBlobBootImagePS)) {
    log_line("[d3d12_x] Compiled replacement BootImagePS (size=%zu bytes)\r\n",
             g_pBlobBootImagePS->GetBufferSize());
  }
  if (CompileShaderBlob("OverlaysPS.hlsl", overlay_ps_code, "ps_5_1", &g_pBlobOverlayPS)) {
    log_line("[d3d12_x] Compiled replacement OverlaysPS (size=%zu bytes)\r\n",
             g_pBlobOverlayPS->GetBufferSize());
  }
}

static thread_local bool t_psoPsUntranslated = false;
static HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineStateX(
    ID3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc,
    void *pArg3, void *pArg4, const IID *priid, void **ppvPipelineState) {
  IID riid = priid ? *priid : __uuidof(ID3D12PipelineState);
  log_line("[d3d12_x] HookedCreateGraphicsPipelineStateX: pDesc=%p, arg3=%p, "
           "arg4=%p, priid=%p, ppv=%p\r\n",
           pDesc, pArg3, pArg4, priid, ppvPipelineState);
  if (!ppvPipelineState)
    return E_POINTER;

  EnsureReplacementShaders();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC modifiedDesc = pDesc ? *pDesc : D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
  const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDescToUse = pDesc;

  static const bool s_noXlate = GetEnvironmentVariableA("ONRUSH_NO_TRANSLATED_SHADERS", nullptr, 0) > 0;
  const bool hasPS = pDesc && pDesc->PS.pShaderBytecode && pDesc->PS.BytecodeLength > 16;
  if (pDesc && !s_noXlate && pDesc->VS.pShaderBytecode && pDesc->VS.BytecodeLength > 16 && (hasPS || pDesc->PS.BytecodeLength == 0)) {
    ID3DBlob* tv = GetTranslatedShader(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength, "vs_5_1");
    // A VS feeding a geometry/hull stage exports pre-projection data; without that stage it covers the screen.
    if (tv && (pDesc->GS.BytecodeLength || pDesc->HS.BytecodeLength || pDesc->DS.BytecodeLength)) {
      log_line("[xlate] VS %016llx has GS=%zu HS=%zu DS=%zu -> not translated\r\n", (unsigned long long)Fnv1a64(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength),
               (size_t)pDesc->GS.BytecodeLength, (size_t)pDesc->HS.BytecodeLength, (size_t)pDesc->DS.BytecodeLength);
      // keep HS/DS/GS bytecode (named after the VS hash + own hash) for the offline translator
      { unsigned long long vh = Fnv1a64(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
        const D3D12_SHADER_BYTECODE *st[3] = { &pDesc->HS, &pDesc->DS, &pDesc->GS }; const char *nm[3] = { "hs", "ds", "gs" };
        CreateDirectoryW(L"shaders", nullptr); CreateDirectoryW(L"shaders\\tess", nullptr);
        for (int k = 0; k < 3; ++k) {
          if (!st[k]->pShaderBytecode || st[k]->BytecodeLength < 16) continue;
          char dp[MAX_PATH]; sprintf_s(dp, "shaders\\tess\\vs%016llx_%s%016llx.dxbc", vh, nm[k], (unsigned long long)Fnv1a64(st[k]->pShaderBytecode, st[k]->BytecodeLength));
          if (GetFileAttributesA(dp) == INVALID_FILE_ATTRIBUTES) { FILE* fd = fopen(dp, "wb"); if (fd) { fwrite(st[k]->pShaderBytecode, 1, st[k]->BytecodeLength, fd); fclose(fd); } }
        }
        log_line("[tess] VS %016llx topoType=%d numRT=%u rtv0=%d dsv=%d ps=%016llx\r\n", vh, (int)pDesc->PrimitiveTopologyType, pDesc->NumRenderTargets, (int)pDesc->RTVFormats[0], (int)pDesc->DSVFormat,
                 hasPS ? (unsigned long long)Fnv1a64(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength) : 0ull); }
      // tessellation bypass: LS + DS folded into one VS by tess_bypass.py (shaders\translated\tess_<vs>_<ds>.dxil.cso)
      ID3DBlob* tb = nullptr;
      { static const bool s_noTess = GetEnvironmentVariableA("ONRUSH_NO_TESS_BYPASS", nullptr, 0) > 0;
        if (!s_noTess && !pDesc->GS.BytecodeLength && pDesc->HS.BytecodeLength && pDesc->DS.BytecodeLength) {
          char tp_[MAX_PATH]; sprintf_s(tp_, "shaders\\translated\\tess_%016llx_%016llx.dxil.cso", (unsigned long long)Fnv1a64(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength),
                                        (unsigned long long)Fnv1a64(pDesc->DS.pShaderBytecode, pDesc->DS.BytecodeLength));
          FILE* ft = fopen(tp_, "rb");
          if (ft) {
            fseek(ft, 0, SEEK_END); long tsz = ftell(ft); fseek(ft, 0, SEEK_SET);
            EnsureD3DCompilerLoaded();
            typedef HRESULT(WINAPI* PFN_CB)(SIZE_T, ID3DBlob**); static PFN_CB s_cbT = nullptr;
            if (!s_cbT) { HMODULE m = GetModuleHandleA("d3dcompiler_47.dll"); if (!m) m = LoadLibraryA("d3dcompiler_47.dll"); if (m) s_cbT = (PFN_CB)GetProcAddress(m, "D3DCreateBlob"); }
            if (tsz > 0 && s_cbT && SUCCEEDED(s_cbT((SIZE_T)tsz, &tb)) && tb) fread(tb->GetBufferPointer(), 1, (size_t)tsz, ft); else tb = nullptr;
            fclose(ft);
          }
          log_line("[tess] bypass %s -> %s\r\n", tp_, tb ? "LOADED" : "missing");
        } }
      if (tb) {
        tv = tb;
        modifiedDesc.HS = D3D12_SHADER_BYTECODE{}; modifiedDesc.DS = D3D12_SHADER_BYTECODE{};
        modifiedDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      } else
      tv = nullptr;
    }
    ID3DBlob* tp = (tv && hasPS) ? GetTranslatedShader(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength, "ps_5_1") : nullptr;
    // ONRUSH_TESS_MAGENTA=1 (debug): PSOs drawn through the tessellation bypass get a flat magenta PS (shaders\translated\tess_magenta.dxil.cso)
    if (tv && hasPS && modifiedDesc.HS.BytecodeLength == 0 && pDesc->HS.BytecodeLength && GetEnvironmentVariableA("ONRUSH_TESS_MAGENTA", nullptr, 0) > 0) {
      static ID3DBlob* s_mag = nullptr; static bool s_magTried = false;
      if (!s_magTried) { s_magTried = true; FILE* fm = fopen("shaders\\translated\\tess_magenta.dxil.cso", "rb");
        if (fm) { fseek(fm, 0, SEEK_END); long ms = ftell(fm); fseek(fm, 0, SEEK_SET); EnsureD3DCompilerLoaded();
          typedef HRESULT(WINAPI* PFN_CB)(SIZE_T, ID3DBlob**); HMODULE m = GetModuleHandleA("d3dcompiler_47.dll"); PFN_CB cb = m ? (PFN_CB)GetProcAddress(m, "D3DCreateBlob") : nullptr;
          if (ms > 0 && cb && SUCCEEDED(cb((SIZE_T)ms, &s_mag)) && s_mag) fread(s_mag->GetBufferPointer(), 1, (size_t)ms, fm); else s_mag = nullptr; fclose(fm); } }
      if (s_mag) tp = s_mag;
    }
    // Depth-EQUAL passes need bit-identical positions in the depth prepass and the G-buffer pass. The
    // same VS compiled by fxc (DXBC) and by dxc (DXIL twin, picked when the PS is SM6) differs slightly ->
    // striped holes. So every graphics PSO uses the DXIL twins when they exist (ONRUSH_ALLOW_DXBC_VS=1 = old).
    { static const bool s_mixOk = GetEnvironmentVariableA("ONRUSH_ALLOW_DXBC_VS", nullptr, 0) > 0;
      if (!s_mixOk && tv && !IsDxilBlob(tv) && !(tp && g_simplePsBlob.count(tp))) {
        ID3DBlob* tv2 = LoadDxilTwin(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
        ID3DBlob* tp2 = (tp && !IsDxilBlob(tp)) ? LoadDxilTwin(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength) : tp;
        if (tv2 && (!hasPS || tp2)) { tv = tv2; if (hasPS) tp = tp2; }
      } }
    // DXBC and DXIL cannot be mixed in one PSO: an SM6 (DXIL) pixel shader needs the VS's DXIL twin (<hash>.dxil.cso)
    if (tv && tp && IsDxilBlob(tp) && !IsDxilBlob(tv)) {
      ID3DBlob* tv2 = LoadDxilTwin(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
      if (tv2) { tv = tv2; log_line("[xlate] using DXIL twin VS for SM6 PS\r\n"); }
      else { log_line("[xlate] SM6 PS without DXIL VS twin -> original shaders\r\n"); tv = nullptr; tp = nullptr; }
    } else if (tv && tp && IsDxilBlob(tv) && !IsDxilBlob(tp)) {
      // SM6 vertex shader (wave intrinsics) with an SM5.1 pixel shader: use the PS's DXIL twin.
      ID3DBlob* tp2 = LoadDxilTwin(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
      if (tp2) { tp = tp2; log_line("[xlate] using DXIL twin PS for SM6 VS\r\n"); }
      else { log_line("[xlate] SM6 VS without DXIL PS twin -> original shaders\r\n"); tv = nullptr; tp = nullptr; }
    }
    const bool vsOnly = !hasPS && tv;
    t_psoPsUntranslated = tv && hasPS && !tp;  // PS without translation -> the draw is skipped (see SkipDrawByShader)
    static int s_xlateSeq = 0; static unsigned long long s_xlateMask = 0xFFFFFFFFFFFFFFFFull;
    static bool s_maskInit = false;
    if (!s_maskInit) { s_maskInit = true; char mb[32]; if (GetEnvironmentVariableA("ONRUSH_XLATE_MASK", mb, sizeof(mb)) > 0) s_xlateMask = strtoull(mb, nullptr, 16); }
    if ((tv && (tp || vsOnly)) && ((s_xlateMask >> (s_xlateSeq++ & 63)) & 1) == 0) { tv = nullptr; tp = nullptr; }
    if (tv && (tp || vsOnly)) {
      log_line("[d3d12_x] USING TRANSLATED GCN shaders (VS %zu->%zu, PS %zu->%zu)\r\n",
               (size_t)pDesc->VS.BytecodeLength, tv->GetBufferSize(), (size_t)pDesc->PS.BytecodeLength, tp ? tp->GetBufferSize() : (size_t)0);
      log_line("[xlate]   raster: fill=%d cull=%d depthBias=%d clip=%d msaa=%d aaLine=%d forcedSamples=%u cons=%d | SO.rasterStream=%u SO.numEntries=%u | flags=%d nodeMask=%u ib=%d topoType=%d numRT=%u rtv=%d,%d,%d\r\n",
               (int)pDesc->RasterizerState.FillMode, (int)pDesc->RasterizerState.CullMode, (int)pDesc->RasterizerState.DepthBias, (int)pDesc->RasterizerState.DepthClipEnable, (int)pDesc->RasterizerState.MultisampleEnable, (int)pDesc->RasterizerState.AntialiasedLineEnable, (unsigned)pDesc->RasterizerState.ForcedSampleCount, (int)pDesc->RasterizerState.ConservativeRaster,
               (unsigned)pDesc->StreamOutput.RasterizedStream, (unsigned)pDesc->StreamOutput.NumEntries, (int)pDesc->Flags, (unsigned)pDesc->NodeMask, (int)pDesc->IBStripCutValue, (int)pDesc->PrimitiveTopologyType, (unsigned)pDesc->NumRenderTargets, (int)pDesc->RTVFormats[0], (int)pDesc->RTVFormats[1], (int)pDesc->RTVFormats[2]);
      log_line("[xlate]   blend: numRT=%u mask0=%u blendEn0=%d a2c=%d sample=%u/%08X ind=%d\r\n", pDesc->NumRenderTargets, (unsigned)pDesc->BlendState.RenderTarget[0].RenderTargetWriteMask, (int)pDesc->BlendState.RenderTarget[0].BlendEnable, (int)pDesc->BlendState.AlphaToCoverageEnable, pDesc->SampleDesc.Count, pDesc->SampleMask, (int)pDesc->BlendState.IndependentBlendEnable);
      { const auto &b0 = pDesc->BlendState.RenderTarget[0];
        log_line("[xlate]   blend0: en=%d src=%d dst=%d op=%d srcA=%d dstA=%d opA=%d mask=%u numRT=%u rtv0=%d a2c=%d vs=%016llx ps=%016llx\r\n", (int)b0.BlendEnable, (int)b0.SrcBlend, (int)b0.DestBlend, (int)b0.BlendOp, (int)b0.SrcBlendAlpha, (int)b0.DestBlendAlpha, (int)b0.BlendOpAlpha,
                 (unsigned)b0.RenderTargetWriteMask, pDesc->NumRenderTargets, (int)pDesc->RTVFormats[0], (int)pDesc->BlendState.AlphaToCoverageEnable,
                 (unsigned long long)Fnv1a64(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength), hasPS ? (unsigned long long)Fnv1a64(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength) : 0ull); }
      log_line("[xlate]   depth: enable=%d mask=%d func=%d dsv=%d stencil=%d cull=%d ccw=%d clip=%d rtv0=%d topo=%d\r\n",
               pDesc->DepthStencilState.DepthEnable, (int)pDesc->DepthStencilState.DepthWriteMask, (int)pDesc->DepthStencilState.DepthFunc, (int)pDesc->DSVFormat,
               pDesc->DepthStencilState.StencilEnable, (int)pDesc->RasterizerState.CullMode, pDesc->RasterizerState.FrontCounterClockwise, pDesc->RasterizerState.DepthClipEnable, (int)pDesc->RTVFormats[0], (int)pDesc->PrimitiveTopologyType);
      modifiedDesc.VS.pShaderBytecode = tv->GetBufferPointer();
      modifiedDesc.VS.BytecodeLength  = tv->GetBufferSize();
      { static const bool s_noDepth = GetEnvironmentVariableA("ONRUSH_XLATE_NODEPTH", nullptr, 0) > 0;
        if (s_noDepth) { modifiedDesc.DepthStencilState.DepthEnable = FALSE; modifiedDesc.DepthStencilState.StencilEnable = FALSE;  modifiedDesc.RasterizerState.DepthClipEnable = FALSE; modifiedDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; } }
      { // Depth-EQUAL passes without depth writes stay EQUAL: the G-buffer PS of alpha-tested foliage has no
        // clip() and relies on EQUAL against the prepass depth to cut the card quads (the old GREATER_EQUAL
        // workaround drew the whole card, giving dark "cardboard" panels around trees). The prepass and G-buffer
        // VS now translate to identical positions, so EQUAL no longer drops the tree G-buffer draws.
        // ONRUSH_EQ_DEPTH_BIAS=N restores the old GREATER_EQUAL + bias N workaround (debug only).
        static int s_eqBias = -2;
        if (s_eqBias == -2) { char eb[16] = {}; s_eqBias = GetEnvironmentVariableA("ONRUSH_EQ_DEPTH_BIAS", eb, sizeof(eb)) > 0 ? atoi(eb) : -1; }
        if (s_eqBias >= 0 && modifiedDesc.DepthStencilState.DepthEnable && modifiedDesc.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO &&
            modifiedDesc.DepthStencilState.DepthFunc == D3D12_COMPARISON_FUNC_EQUAL) {
          modifiedDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
          modifiedDesc.RasterizerState.DepthBias = s_eqBias;
          modifiedDesc.RasterizerState.SlopeScaledDepthBias = s_eqBias ? 1.0f : 0.0f;
        } }
      if (tp) { modifiedDesc.PS.pShaderBytecode = tp->GetBufferPointer();
      modifiedDesc.PS.BytecodeLength  = tp->GetBufferSize(); }
      pDescToUse = &modifiedDesc;
    }
  }
  if (pDesc && pDescToUse == pDesc) {
    if (pDesc->PS.pShaderBytecode && pDesc->PS.BytecodeLength == 2096 && g_pBlobBootImagePS && g_pBlobFullScreenVS) {
      log_line("[d3d12_x] REPLACING Durango GCN shaders for BootImage PSO (VS len=%zu->%zu, PS len=%zu->%zu)\r\n",
               pDesc->VS.BytecodeLength, g_pBlobFullScreenVS->GetBufferSize(),
               pDesc->PS.BytecodeLength, g_pBlobBootImagePS->GetBufferSize());
      modifiedDesc.VS.pShaderBytecode = g_pBlobFullScreenVS->GetBufferPointer();
      modifiedDesc.VS.BytecodeLength  = g_pBlobFullScreenVS->GetBufferSize();
      modifiedDesc.PS.pShaderBytecode = g_pBlobBootImagePS->GetBufferPointer();
      modifiedDesc.PS.BytecodeLength  = g_pBlobBootImagePS->GetBufferSize();
      pDescToUse = &modifiedDesc;
    } else if (pDesc->PS.pShaderBytecode && pDesc->PS.BytecodeLength == 4376 && g_pBlobOverlayPS && g_pBlobFullScreenVS) {
      log_line("[d3d12_x] REPLACING Durango GCN shaders for OverlaysOnly PSO (VS len=%zu->%zu, PS len=%zu->%zu)\r\n",
               pDesc->VS.BytecodeLength, g_pBlobFullScreenVS->GetBufferSize(),
               pDesc->PS.BytecodeLength, g_pBlobOverlayPS->GetBufferSize());
      modifiedDesc.VS.pShaderBytecode = g_pBlobFullScreenVS->GetBufferPointer();
      modifiedDesc.VS.BytecodeLength  = g_pBlobFullScreenVS->GetBufferSize();
      modifiedDesc.PS.pShaderBytecode = g_pBlobOverlayPS->GetBufferPointer();
      modifiedDesc.PS.BytecodeLength  = g_pBlobOverlayPS->GetBufferSize();
      pDescToUse = &modifiedDesc;
    }
  }

  HRESULT hr = E_FAIL;
  if (g_TrueCreateGraphicsPipelineState) {
    hr = psodc::CreateGraphics(g_TrueCreateGraphicsPipelineState, pDevice, pDescToUse, riid, ppvPipelineState);
    if (SUCCEEDED(hr) && t_psoPsUntranslated && ppvPipelineState && *ppvPipelineState) MarkPsoUntranslatedPs(*ppvPipelineState);
    t_psoPsUntranslated = false;
    if (FAILED(hr) && pDescToUse != pDesc && pDesc) {
      log_line("[xlate] translated PSO rejected (hr=0x%08lX) -> falling back to original shaders\r\n", (unsigned long)hr);
      LogD3D12Failure(pDevice, "translated CreateGraphicsPipelineState", hr);
      for (UINT ie = 0; ie < pDesc->InputLayout.NumElements; ++ie) { const D3D12_INPUT_ELEMENT_DESC& e = pDesc->InputLayout.pInputElementDescs[ie];
        log_line("[xlate]   IL[%u] %s%u fmt=%d slot=%u off=%u cls=%d\r\n", ie, e.SemanticName, e.SemanticIndex, (int)e.Format, e.InputSlot, e.AlignedByteOffset, (int)e.InputSlotClass); }
      hr = g_TrueCreateGraphicsPipelineState(pDevice, pDesc, riid, ppvPipelineState);
    }
    log_line("  -> CreateGraphicsPipelineState returned hr=0x%08lX, pso=%p\r\n",
             (unsigned long)hr, *ppvPipelineState);
    if (SUCCEEDED(hr) && *ppvPipelineState && pDesc) {
      log_line("  [pso_cache] pso=%p, VS=%p (len=%zu), PS=%p (len=%zu)\r\n",
               *ppvPipelineState,
               pDesc->VS.pShaderBytecode, (size_t)pDesc->VS.BytecodeLength,
               pDesc->PS.pShaderBytecode, (size_t)pDesc->PS.BytecodeLength);
      { static char s_ph[512] = {}; static int s_phInit = 0;  // ONRUSH_LOG_PSHASH=h1,h2,...: PS or VS hashes
        if (!s_phInit) { s_phInit = 1; GetEnvironmentVariableA("ONRUSH_LOG_PSHASH", s_ph, sizeof(s_ph)); }
        if (s_ph[0] && *ppvPipelineState) {
          char hx[24] = {}, hv[24] = {};
          if (pDesc->PS.pShaderBytecode && pDesc->PS.BytecodeLength > 16) sprintf_s(hx, "%016llx", (unsigned long long)Fnv1a64(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength));
          if (pDesc->VS.pShaderBytecode && pDesc->VS.BytecodeLength > 16) sprintf_s(hv, "%016llx", (unsigned long long)Fnv1a64(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength));
          if ((hx[0] && strstr(s_ph, hx)) || (hv[0] && strstr(s_ph, hv))) {
            LONG k = InterlockedIncrement(&g_dbgPsoN) - 1;
            if (k < 8) g_dbgPso[k] = *ppvPipelineState;
            const D3D12_RENDER_TARGET_BLEND_DESC &b = pDesc->BlendState.RenderTarget[0];
            log_line("[pshash] PSO %p ps=%s numRT=%u rtv0=%d blendEn=%d src=%d dst=%d op=%d srcA=%d dstA=%d opA=%d mask=%u logicOp=%d dsv=%d depth=%d\r\n",
                     *ppvPipelineState, hx, pDesc->NumRenderTargets, (int)pDesc->RTVFormats[0], (int)b.BlendEnable, (int)b.SrcBlend, (int)b.DestBlend, (int)b.BlendOp,
                     (int)b.SrcBlendAlpha, (int)b.DestBlendAlpha, (int)b.BlendOpAlpha, (unsigned)b.RenderTargetWriteMask, (int)b.LogicOpEnable, (int)pDesc->DSVFormat, (int)pDesc->DepthStencilState.DepthEnable);
          }
        } }
      PsoShaderBlobs blobs;
      if (pDesc->VS.pShaderBytecode && pDesc->VS.BytecodeLength > 0) {
        blobs.vsBytecode.assign(
            (const uint8_t *)pDesc->VS.pShaderBytecode,
            (const uint8_t *)pDesc->VS.pShaderBytecode + pDesc->VS.BytecodeLength);
      }
      if (pDesc->PS.pShaderBytecode && pDesc->PS.BytecodeLength > 0) {
        blobs.psBytecode.assign(
            (const uint8_t *)pDesc->PS.pShaderBytecode,
            (const uint8_t *)pDesc->PS.pShaderBytecode + pDesc->PS.BytecodeLength);
      }
      if (!blobs.vsBytecode.empty() || !blobs.psBytecode.empty()) {
        AcquireSRWLockExclusive(&g_PsoShaderLock);
        g_PsoShaderMap[*ppvPipelineState] = std::move(blobs);
        ReleaseSRWLockExclusive(&g_PsoShaderLock);
      }
    }
    if (FAILED(hr)) {
      LogD3D12Failure(pDevice, "CreateGraphicsPipelineState", hr);
    }
  }
  return hr;
}

// Validate D3D12_SAMPLER_DESC layout at compile time to ensure byte offsets and
// size match ABI expectations
static_assert(sizeof(D3D12_SAMPLER_DESC) == 52,
              "D3D12_SAMPLER_DESC unexpected size");
static_assert(offsetof(D3D12_SAMPLER_DESC, Filter) == 0,
              "Unexpected Filter offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, AddressU) == 4,
              "Unexpected AddressU offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, AddressV) == 8,
              "Unexpected AddressV offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, AddressW) == 12,
              "Unexpected AddressW offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, MipLODBias) == 16,
              "Unexpected MipLODBias offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, MaxAnisotropy) == 20,
              "Unexpected MaxAnisotropy offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, ComparisonFunc) == 24,
              "Unexpected ComparisonFunc offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, BorderColor) == 28,
              "Unexpected BorderColor offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, MinLOD) == 44,
              "Unexpected MinLOD offset");
static_assert(offsetof(D3D12_SAMPLER_DESC, MaxLOD) == 48,
              "Unexpected MaxLOD offset");

static bool IsVerboseSamplerLoggingEnabled() {
  static int s_enabled = -1;
  if (s_enabled == -1) {
    wchar_t envBuf[16] = {};
    DWORD len = GetEnvironmentVariableW(L"ONRUSH_DEBUG_SAMPLER", envBuf, 16);
    s_enabled = (len > 0 && envBuf[0] == L'1') ? 1 : 0;
  }
  return s_enabled == 1;
}

static void LogSamplerDescDetails(const D3D12_SAMPLER_DESC *pDesc,
                                  const uint32_t *raw) {
  log_line("  -> Filter=%u (0x%X), AddressU=%u (0x%X), AddressV=%u (0x%X), "
           "AddressW=%u (0x%X)\r\n",
           (UINT)pDesc->Filter, (UINT)pDesc->Filter, (UINT)pDesc->AddressU,
           (UINT)pDesc->AddressU, (UINT)pDesc->AddressV, (UINT)pDesc->AddressV,
           (UINT)pDesc->AddressW, (UINT)pDesc->AddressW);
  log_line("  -> MipLODBias=%f (0x%08X), MaxAnisotropy=%u (0x%X), "
           "ComparisonFunc=%u (0x%X)\r\n",
           pDesc->MipLODBias, raw[4], pDesc->MaxAnisotropy,
           pDesc->MaxAnisotropy, (UINT)pDesc->ComparisonFunc,
           (UINT)pDesc->ComparisonFunc);
  log_line(
      "  -> BorderColor=[%f, %f, %f, %f] (0x%08X 0x%08X 0x%08X 0x%08X)\r\n",
      pDesc->BorderColor[0], pDesc->BorderColor[1], pDesc->BorderColor[2],
      pDesc->BorderColor[3], raw[7], raw[8], raw[9], raw[10]);
  log_line("  -> MinLOD=%f (0x%08X), MaxLOD=%f (0x%08X)\r\n", pDesc->MinLOD,
           raw[11], pDesc->MaxLOD, raw[12]);

  constexpr size_t kSamplerDwords =
      sizeof(D3D12_SAMPLER_DESC) / sizeof(uint32_t);
  char rawHex[128] = {};
  size_t offset = 0;
  for (size_t i = 0; i < kSamplerDwords; ++i) {
    if (offset + 10 >= sizeof(rawHex))
      break;
    int written = _snprintf_s(rawHex + offset, sizeof(rawHex) - offset,
                              _TRUNCATE, "%08X ", raw[i]);
    if (written < 0)
      break;
    offset += static_cast<size_t>(written);
  }
  log_line("  -> RawDesc (%zu DWORDs): %s\r\n", kSamplerDwords, rawHex);
}

static void LogDesktopSamplerDesc(const D3D12_SAMPLER_DESC *pDesc) {
  if (!pDesc)
    return;
  log_line("  [Desktop D3D12_SAMPLER_DESC (52 bytes)]\r\n");
  log_line("    Filter=%u (0x%X), AddressU=%u, AddressV=%u, AddressW=%u\r\n",
           (UINT)pDesc->Filter, (UINT)pDesc->Filter, (UINT)pDesc->AddressU,
           (UINT)pDesc->AddressV, (UINT)pDesc->AddressW);
  log_line("    MipLODBias=%.4f, MaxAnisotropy=%u, ComparisonFunc=%u\r\n",
           pDesc->MipLODBias, pDesc->MaxAnisotropy,
           (UINT)pDesc->ComparisonFunc);
  log_line(
      "    BorderColor=[%.2f, %.2f, %.2f, %.2f], MinLOD=%.4f, MaxLOD=%.4f\r\n",
      pDesc->BorderColor[0], pDesc->BorderColor[1], pDesc->BorderColor[2],
      pDesc->BorderColor[3], pDesc->MinLOD, pDesc->MaxLOD);
}

static void STDMETHODCALLTYPE
HookedCreateSamplerX(ID3D12Device *pDevice, const D3D12_SAMPLER_DESC *pDesc,
                     D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
  static volatile LONG s_samplerIndex = 0;
  LONG idx = InterlockedIncrement(&s_samplerIndex);

  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva = 0;
  bool hasRva = TryGetModuleRva(hExe, retAddr, &rva);

  constexpr size_t kSamplerDwords =
      sizeof(D3D12_SAMPLER_DESC) / sizeof(uint32_t);
  uint32_t raw[kSamplerDwords] = {};
  if (pDesc) {
    memcpy(raw, pDesc, sizeof(D3D12_SAMPLER_DESC));
  }

  bool verbose = IsVerboseSamplerLoggingEnabled();

  if (hasRva) {
    log_line("[d3d12_x] HookedCreateSamplerX [#%ld]: caller=%p (game+0x%llX), "
             "pDesc=%p, Dest=0x%016llX\r\n",
             idx, retAddr, (unsigned long long)rva, pDesc,
             (unsigned long long)DestDescriptor.ptr);
  } else {
    log_line("[d3d12_x] HookedCreateSamplerX [#%ld]: caller=%p, pDesc=%p, "
             "Dest=0x%016llX\r\n",
             idx, retAddr, pDesc, (unsigned long long)DestDescriptor.ptr);
  }

  if (verbose && pDesc) {
    LogSamplerDescDetails(pDesc, raw);
  }

  if (!g_TrueCreateSampler) {
    log_line("[d3d12_x] FATAL ERROR: g_TrueCreateSampler is null! Cannot "
             "invoke native CreateSampler for sampler [#%ld]\r\n",
             idx);
    if (IsDebuggerPresent()) {
      __debugbreak();
    }
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    return;
  }

  D3D12_SAMPLER_DESC convertedDesc = {};
  SamplerTranslationStatus status =
      TranslateXboxSamplerFromPayloadPointer(pDesc, &convertedDesc);
  if (status != SamplerTranslationStatus::Success) {
    log_line(
        "[d3d12_x] FATAL: HookedCreateSamplerX [#%ld] translation failed with "
        "status %d (%s)!\r\n"
        "  -> caller=%p (game+0x%llX), pDesc=%p, DestDescriptor=0x%016llX\r\n",
        idx, (int)status, SamplerTranslationStatusToString(status), retAddr,
        (unsigned long long)rva, pDesc, (unsigned long long)DestDescriptor.ptr);
    if (pDesc) {
      LogSamplerDescDetails(pDesc, raw);
    }
    if (IsDebuggerPresent()) {
      __debugbreak();
    }
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    return;
  }

  if (verbose) {
    log_line("[d3d12_x] HookedCreateSamplerX [#%ld] Converted "
             "D3D12_SAMPLER_DESC:\r\n",
             idx);
    LogDesktopSamplerDesc(&convertedDesc);
  }

  { // ONRUSH_LOG_CMPSAMP=1: log every comparison sampler (shadow maps) with its converted filter
    static const bool s_cmp = GetEnvironmentVariableA("ONRUSH_LOG_CMPSAMP", nullptr, 0) > 0;
    static volatile LONG s_cn = 0;
    if (s_cmp && ((UINT)convertedDesc.Filter & 0x80u) && InterlockedIncrement(&s_cn) <= 40) {
      const uint32_t *w = (const uint32_t *)pDesc;
      log_line("[cmpsamp] #%ld dest=%llX filter=0x%X func=%d addr=%d/%d/%d lodbias=%g aniso=%u minlod=%g maxlod=%g raw=%08X %08X %08X %08X\r\n", idx,
               (unsigned long long)DestDescriptor.ptr, (UINT)convertedDesc.Filter, (int)convertedDesc.ComparisonFunc,
               (int)convertedDesc.AddressU, (int)convertedDesc.AddressV, (int)convertedDesc.AddressW, convertedDesc.MipLODBias,
               convertedDesc.MaxAnisotropy, convertedDesc.MinLOD, convertedDesc.MaxLOD, w ? w[0] : 0, w ? w[1] : 0, w ? w[2] : 0, w ? w[3] : 0);
    }
  }

  HRESULT preReason = S_OK;
  if (pDevice) {
    preReason = pDevice->GetDeviceRemovedReason();
  }

  UINT64 msgStart =
      g_pD3D12InfoQueue ? g_pD3D12InfoQueue->GetNumStoredMessages() : 0;

  g_TrueCreateSampler(pDevice, &convertedDesc, DestDescriptor);

  UINT64 msgEnd =
      g_pD3D12InfoQueue ? g_pD3D12InfoQueue->GetNumStoredMessages() : 0;

  if (pDevice) {
    HRESULT postReason = pDevice->GetDeviceRemovedReason();

    if (postReason != S_OK) {
      if (preReason != S_OK) {
        log_line("[d3d12_x] NOTE: Device was already removed prior to "
                 "HookedCreateSamplerX [#%ld]: Reason=0x%08lX\r\n",
                 idx, (unsigned long)preReason);
      } else {
        log_line("[d3d12_x] WARNING: Device removal state detected following "
                 "HookedCreateSamplerX [#%ld] (temporal correlation, not "
                 "proven cause): Reason=0x%08lX\r\n",
                 idx, (unsigned long)postReason);
      }

      if (!verbose && pDesc) {
        LogSamplerDescDetails(pDesc, raw);
        LogDesktopSamplerDesc(&convertedDesc);
      }
    }
  }

  if (msgEnd > msgStart) {
    LogD3D12InfoQueueRange(idx, msgStart, msgEnd);
  } else if (verbose) {
    log_line("[d3d12_x] HookedCreateSamplerX [#%ld]: No new InfoQueue messages "
             "(total stored: %llu)\r\n",
             idx, msgEnd);
  }
}

static HRESULT STDMETHODCALLTYPE HookedCreateComputePipelineStateX(
    ID3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc,
    void *pArg3, void *pArg4, const IID *priid, void **ppvPipelineState) {
  IID riid = priid ? *priid : __uuidof(ID3D12PipelineState);
  log_line("[d3d12_x] HookedCreateComputePipelineStateX: pDesc=%p, arg3=%p, "
           "arg4=%p, priid=%p, ppv=%p\r\n",
           pDesc, pArg3, pArg4, priid, ppvPipelineState);
  if (!ppvPipelineState)
    return E_POINTER;
  HRESULT hr = E_FAIL;
  D3D12_COMPUTE_PIPELINE_STATE_DESC xlDesc = pDesc ? *pDesc : D3D12_COMPUTE_PIPELINE_STATE_DESC{};
  const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDescCS = pDesc;
  static const bool s_noXlateCS = GetEnvironmentVariableA("ONRUSH_NO_TRANSLATED_CS", nullptr, 0) > 0;
  if (pDesc && !s_noXlateCS && !GetEnvironmentVariableA("ONRUSH_NO_TRANSLATED_SHADERS", nullptr, 0) && pDesc->CS.pShaderBytecode && pDesc->CS.BytecodeLength > 16) {
    ID3DBlob* tc = GetTranslatedShader(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength, "cs_5_1");
    if (tc) { xlDesc.CS.pShaderBytecode = tc->GetBufferPointer(); xlDesc.CS.BytecodeLength = tc->GetBufferSize(); pDescCS = &xlDesc;
      log_line("[d3d12_x] USING TRANSLATED CS (%zu->%zu)\r\n", (size_t)pDesc->CS.BytecodeLength, tc->GetBufferSize()); }
  }
  if (g_TrueCreateComputePipelineState) {
    hr = psodc::CreateCompute(g_TrueCreateComputePipelineState, pDevice, pDescCS, riid, ppvPipelineState);
    if (FAILED(hr) && pDescCS != pDesc) {
      log_line("[xlate] translated CS rejected (hr=0x%08lX) -> original\r\n", (unsigned long)hr);
      LogD3D12Failure(pDevice, "translated CreateComputePipelineState", hr);
      hr = g_TrueCreateComputePipelineState(pDevice, pDesc, riid, ppvPipelineState);
    }
    log_line("  -> CreateComputePipelineState returned hr=0x%08lX, pso=%p\r\n",
             (unsigned long)hr, *ppvPipelineState);
    if (SUCCEEDED(hr) && *ppvPipelineState && pDesc) {
      log_line("  [pso_cache] pso=%p, CS=%p (len=%zu)\r\n",
               *ppvPipelineState,
               pDesc->CS.pShaderBytecode, (size_t)pDesc->CS.BytecodeLength);
      PsoShaderBlobs blobs;
      if (pDesc->CS.pShaderBytecode && pDesc->CS.BytecodeLength > 0) {
        blobs.csBytecode.assign(
            (const uint8_t *)pDesc->CS.pShaderBytecode,
            (const uint8_t *)pDesc->CS.pShaderBytecode + pDesc->CS.BytecodeLength);
      }
      if (!blobs.csBytecode.empty()) {
        AcquireSRWLockExclusive(&g_PsoShaderLock);
        g_PsoShaderMap[*ppvPipelineState] = std::move(blobs);
        ReleaseSRWLockExclusive(&g_PsoShaderLock);
      }
    }
    if (FAILED(hr)) {
      LogD3D12Failure(pDevice, "CreateComputePipelineState", hr);
    }
  }
  return hr;
}

static std::unordered_map<void*, int> g_SigKind; static SRWLOCK g_SigLock = SRWLOCK_INIT;
static std::unordered_map<void*, int> g_SigDispOff;
int LookupSigDispatchOffset(void* sig) { AcquireSRWLockShared(&g_SigLock); auto it = g_SigDispOff.find(sig); int v = (it == g_SigDispOff.end()) ? -1 : it->second; ReleaseSRWLockShared(&g_SigLock); return v; }
int LookupSigKind(void* sig) { AcquireSRWLockShared(&g_SigLock); auto it = g_SigKind.find(sig); int v = (it == g_SigKind.end()) ? -1 : it->second; ReleaseSRWLockShared(&g_SigLock); return v; }
static HRESULT STDMETHODCALLTYPE HookedCreateCommandSignature(
    ID3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc,
    ID3D12RootSignature *pRootSignature, REFIID riid,
    void **ppvCommandSignature) {
  void *retAddr = _ReturnAddress();
  HMODULE hExe = GetModuleHandleW(nullptr);
  uintptr_t rva =
      (hExe && retAddr >= hExe) ? ((uintptr_t)retAddr - (uintptr_t)hExe) : 0;

  log_line(
      "\r\n[d3d12_x] === HookedCreateCommandSignature (Diagnostic) ===\r\n");
  log_line("  Caller: %p (game+0x%llX)\r\n", retAddr, (unsigned long long)rva);
  log_line("  pDevice: %p, pRootSignature: %p, ppvCommandSignature: %p\r\n",
           pDevice, pRootSignature, ppvCommandSignature);
  log_guid("riid", &riid);

  if (!pDesc) {
    log_line("  pDesc: (null)\r\n");
  } else {
    __try {
      log_line(
          "  pDesc: %p | ByteStride=%u, NumArgumentDescs=%u, NodeMask=0x%X\r\n",
          pDesc, pDesc->ByteStride, pDesc->NumArgumentDescs, pDesc->NodeMask);
      if (pDesc->pArgumentDescs && pDesc->NumArgumentDescs > 0) {
        for (UINT i = 0; i < pDesc->NumArgumentDescs && i < 16; ++i) {
          const D3D12_INDIRECT_ARGUMENT_DESC &arg = pDesc->pArgumentDescs[i];
          const char *typeName = "UNKNOWN";
          switch (arg.Type) {
          case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
            typeName = "DRAW";
            log_line("    Argument[%u]: Type=%u (%s)\r\n", i, (UINT)arg.Type,
                     typeName);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
            typeName = "DRAW_INDEXED";
            log_line("    Argument[%u]: Type=%u (%s)\r\n", i, (UINT)arg.Type,
                     typeName);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
            typeName = "DISPATCH";
            log_line("    Argument[%u]: Type=%u (%s)\r\n", i, (UINT)arg.Type,
                     typeName);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            typeName = "VERTEX_BUFFER_VIEW";
            log_line(
                "    Argument[%u]: Type=%u (%s) | VertexBuffer.Slot=%u\r\n", i,
                (UINT)arg.Type, typeName, arg.VertexBuffer.Slot);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
            typeName = "INDEX_BUFFER_VIEW";
            log_line("    Argument[%u]: Type=%u (%s)\r\n", i, (UINT)arg.Type,
                     typeName);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            typeName = "CONSTANT";
            log_line("    Argument[%u]: Type=%u (%s) | RootParam=%u, "
                     "DestOffset=%u, Num32BitValues=%u\r\n",
                     i, (UINT)arg.Type, typeName,
                     arg.Constant.RootParameterIndex,
                     arg.Constant.DestOffsetIn32BitValues,
                     arg.Constant.Num32BitValuesToSet);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            typeName = "CONSTANT_BUFFER_VIEW";
            log_line("    Argument[%u]: Type=%u (%s) | CBV.RootParam=%u\r\n", i,
                     (UINT)arg.Type, typeName,
                     arg.ConstantBufferView.RootParameterIndex);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            typeName = "SHADER_RESOURCE_VIEW";
            log_line("    Argument[%u]: Type=%u (%s) | SRV.RootParam=%u\r\n", i,
                     (UINT)arg.Type, typeName,
                     arg.ShaderResourceView.RootParameterIndex);
            break;
          case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            typeName = "UNORDERED_ACCESS_VIEW";
            log_line("    Argument[%u]: Type=%u (%s) | UAV.RootParam=%u\r\n", i,
                     (UINT)arg.Type, typeName,
                     arg.UnorderedAccessView.RootParameterIndex);
            break;
          default: {
            const uint32_t *raw = (const uint32_t *)&arg;
            log_line("    Argument[%u]: Type=0x%X (%u) [UNRECOGNIZED/INVALID "
                     "D3D12 TYPE!] RawWords: %08X %08X %08X %08X\r\n",
                     i, (UINT)arg.Type, (UINT)arg.Type, raw[0], raw[1], raw[2],
                     raw[3]);
          } break;
          }
        }
      } else {
        log_line("    pArgumentDescs: %p (NumArgumentDescs=%u)\r\n",
                 pDesc->pArgumentDescs, pDesc->NumArgumentDescs);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      log_line("    [ERROR: Inaccessible memory in pDesc %p]\r\n", pDesc);
    }
  }

  D3D12_COMMAND_SIGNATURE_DESC cleanDesc;
  D3D12_INDIRECT_ARGUMENT_DESC cleanArgs[16];
  const D3D12_COMMAND_SIGNATURE_DESC *descToUse = pDesc;

  if (pDesc && pDesc->pArgumentDescs && pDesc->NumArgumentDescs > 0 && pDesc->NumArgumentDescs <= 16) {
    cleanDesc = *pDesc;
    memcpy(cleanArgs, pDesc->pArgumentDescs, sizeof(D3D12_INDIRECT_ARGUMENT_DESC) * pDesc->NumArgumentDescs);
    bool modified = false;
    for (UINT i = 0; i < pDesc->NumArgumentDescs; ++i) {
      if ((UINT)cleanArgs[i].Type == 0x69) {
        log_line("[d3d12_x] HookedCreateCommandSignature: Sanitizing console Argument[%u].Type 0x69 -> D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH\r\n", i);
        cleanArgs[i].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        modified = true;
      }
    }
    if (modified) {
      cleanDesc.pArgumentDescs = cleanArgs;
      descToUse = &cleanDesc;
    }
  }

  HRESULT hr = E_FAIL;
  if (g_TrueCreateCommandSignature) {
    hr = g_TrueCreateCommandSignature(pDevice, descToUse, pRootSignature, riid,
                                      ppvCommandSignature);
  } else {
    log_line("  [ERROR: g_TrueCreateCommandSignature is null!]\r\n");
  }

  if (SUCCEEDED(hr) && ppvCommandSignature && *ppvCommandSignature && descToUse && descToUse->NumArgumentDescs > 0 && descToUse->pArgumentDescs) {
    // byte offset of the dispatch arguments inside one command (sum of the PC argument sizes before it), -1 if none
    int dispOff = -1, off = 0;
    for (UINT i = 0; i < descToUse->NumArgumentDescs; ++i) {
      const D3D12_INDIRECT_ARGUMENT_DESC &a = descToUse->pArgumentDescs[i];
      if (a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) { dispOff = off; break; }
      // draws: flagged with 0x10000 (the guard then checks VertexCount/IndexCount and InstanceCount instead)
      if (a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW || a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) { dispOff = 0x10000 | off; break; }
      switch (a.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: off += 16; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: off += 20; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: off += 16; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: off += 4 * (int)a.Constant.Num32BitValuesToSet; break;
        default: off += 8; break;  // CBV / SRV / UAV root descriptors
      }
    }
    AcquireSRWLockExclusive(&g_SigLock); g_SigKind[*ppvCommandSignature] = (int)pDesc->pArgumentDescs[pDesc->NumArgumentDescs - 1].Type * 1000 + (int)descToUse->ByteStride;
    g_SigDispOff[*ppvCommandSignature] = dispOff; ReleaseSRWLockExclusive(&g_SigLock); }
  void *outObj = (ppvCommandSignature ? *ppvCommandSignature : nullptr);
  log_line("  -> CreateCommandSignature returned hr=0x%08lX, "
           "ppvCommandSignature=%p (*ppv=%p)\r\n",
           (unsigned long)hr, ppvCommandSignature, outObj);
  if (FAILED(hr)) {
    LogD3D12Failure(pDevice, "CreateCommandSignature", hr);
  }
  log_line("[d3d12_x] === HookedCreateCommandSignature EXIT ===\r\n\r\n");
  return hr;
}

struct D3D12X_COMMAND_LIST_DESC {
  UINT Type;
  UINT NodeMask;
  UINT64 Flags;
};

// BridgeCommandList, forwarding methods, and specialized vtables are defined in
// bridge_cmdlist.h

static void WrapCommandList(void **ppCmdList, D3D12_COMMAND_LIST_TYPE type) {
  if (!ppCmdList || !*ppCmdList)
    return;
  ID3D12GraphicsCommandList *pReal = (ID3D12GraphicsCommandList *)*ppCmdList;
  BridgeCommandList *pBridge = CreateBridgeCommandList(pReal, type);
  *ppCmdList = (void *)pBridge;
  log_line(
      "[d3d12_x] Created BridgeCommandList %p wrapping native %p (type=%d)\r\n",
      pBridge, pReal, (int)type);
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandListX(
    ID3D12Device *pDevice, const D3D12X_COMMAND_LIST_DESC *pDesc,
    ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState,
    const IID *priid, void **ppvCommandList) {
  UINT nodeMask = pDesc ? pDesc->NodeMask : 0;
  D3D12_COMMAND_LIST_TYPE type = pDesc ? (D3D12_COMMAND_LIST_TYPE)pDesc->Type
                                       : D3D12_COMMAND_LIST_TYPE_DIRECT;
  if ((int)type == 16) {
    type = D3D12_COMMAND_LIST_TYPE_COPY;
  }
  IID riid = priid ? *priid : __uuidof(ID3D12GraphicsCommandList);

  log_line("[d3d12_x] HookedCreateCommandListX: type=%u, nodeMask=%u, "
           "alloc=%p, pso=%p, ppv=%p\r\n",
           (UINT)type, nodeMask, pAllocator, pInitialState, ppvCommandList);

  if (!ppvCommandList)
    return E_POINTER;
  *ppvCommandList = nullptr;

  ID3D12CommandAllocator *validAllocator = pAllocator;
  if ((uintptr_t)validAllocator == 0xBAADF00DBAADF00D || !validAllocator) {
    log_line("[d3d12_x] WARNING: HookedCreateCommandListX received invalid "
             "allocator %p, creating temporary fallback allocator!\r\n",
             validAllocator);
    if (g_TrueCreateCommandAllocator) {
      g_TrueCreateCommandAllocator(pDevice, type,
                                   __uuidof(ID3D12CommandAllocator),
                                   (void **)&validAllocator);
    }
  }

  HRESULT hr = E_FAIL;
  if (g_TrueCreateCommandList) {
    hr = g_TrueCreateCommandList(pDevice, nodeMask, type, validAllocator,
                                 pInitialState, riid, ppvCommandList);
    if (FAILED(hr)) {
      hr = g_TrueCreateCommandList(
          pDevice, nodeMask, type, validAllocator, pInitialState,
          __uuidof(ID3D12GraphicsCommandList), ppvCommandList);
    }
    if (FAILED(hr) && type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      hr = g_TrueCreateCommandList(
          pDevice, nodeMask, D3D12_COMMAND_LIST_TYPE_DIRECT, validAllocator,
          pInitialState, __uuidof(ID3D12GraphicsCommandList), ppvCommandList);
    }
    log_line("  -> CreateCommandList returned hr=0x%08lX, cmdList=%p\r\n",
             (unsigned long)hr, *ppvCommandList);

    if (SUCCEEDED(hr) && ppvCommandList && *ppvCommandList) {
      // D3D12CreateCommandList returns an OPEN list. Keep it open: the
      // game records its initial commands and calls Close itself. The
      // old eager Close made that first game Close return E_FAIL.
      WrapCommandList(ppvCommandList, type);
    }
  }
  return hr;
}

EXTERN_C HRESULT WINAPI OnRushD3D12XboxCreateDevice(void *pAdapter,
                                                    const void *pParameters,
                                                    const IID *riid,
                                                    void **ppDevice) {
  log_line(
      "[d3d12_x] D3D12XboxCreateDevice(adapter=%p, params=%p, ppDevice=%p)\r\n",
      pAdapter, pParameters, ppDevice);
  log_guid("riid", riid);
  if (pParameters) {
    const unsigned char *bytes =
        static_cast<const unsigned char *>(pParameters);
    char hex[3 * 64 + 1] = {};
    for (int i = 0; i < 64; ++i)
      sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", bytes[i]);
    log_line("  params[0..63] = %s\r\n", hex);
  }

  if (!riid || !ppDevice)
    return E_POINTER;

  // The latest supplied run crashed at game_release_xbox.exe+0xB6C42,
  // matching the unpatched pointer-width instruction at RVA 0xB6C2A.
  // Stop here on a mismatched executable instead of continuing into an AV.
  if (!g_bufferPointerPatchReady) {
    *ppDevice = nullptr;
    log_line("[d3d12_x] FATAL: required patch at RVA 0xB6C2A was not verified; "
             "refusing device creation. Check the game EXE build and Ghidra "
             "bytes.\r\n");
    return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  }

  // Lazy initialization of real IDXGIFactory2 outside loader lock
  EnsureDXGIFactoryGlobal(GetModuleHandleW(nullptr));

  // Enable D3D12 Debug Layer only if explicitly requested via environment variable.
  // In production compatibility layers, running D3D12SDKLayers with wrapped COM objects
  // and placed resources causes internal debug layer crashes across multithreaded recording.
  wchar_t debugEnv[16] = {};
  if (GetEnvironmentVariableW(L"ONRUSH_D3D12_DEBUG", debugEnv, 16) > 0 &&
      debugEnv[0] == L'1') {
    typedef HRESULT(WINAPI * PFN_D3D12GetDebugInterface)(REFIID riid,
                                                         void **ppvDebug);
    HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
    if (!hD3D12)
      hD3D12 = LoadLibraryW(L"d3d12.dll");
    if (hD3D12) {
      auto pfnGetDebug = (PFN_D3D12GetDebugInterface)GetProcAddress(
          hD3D12, "D3D12GetDebugInterface");
      if (pfnGetDebug) {
        ID3D12Debug *pDebug = nullptr;
        if (SUCCEEDED(pfnGetDebug(__uuidof(ID3D12Debug), (void **)&pDebug)) &&
            pDebug) {
          pDebug->EnableDebugLayer();
          if (GetEnvironmentVariableA("ONRUSH_D3D12_GBV", nullptr, 0) > 0) { ID3D12Debug1* pDbg1 = nullptr; if (SUCCEEDED(pDebug->QueryInterface(__uuidof(ID3D12Debug1), (void**)&pDbg1)) && pDbg1) { pDbg1->SetEnableGPUBasedValidation(TRUE); log_line("[d3d12_x] GPU-based validation enabled\r\n"); pDbg1->Release(); } }
          pDebug->Release();
          log_line("[d3d12_x] D3D12 Debug Layer enabled successfully via ID3D12Debug\r\n");
        }
      }
    }
  } else {
    log_line("[d3d12_x] D3D12 Debug Layer disabled for stability (set ONRUSH_D3D12_DEBUG=1 to enable)\r\n");
  }
  // ONRUSH_DRED=1: Device Removed Extended Data (auto-breadcrumbs + GPU page-fault info).
  char dredEnv[8] = {};
  if (GetEnvironmentVariableA("ONRUSH_DRED", dredEnv, sizeof(dredEnv)) > 0) {
    // 1 = breadcrumbs + page faults (breadcrumbs currently crash D3D12Core early), 2 = page faults only
    typedef HRESULT(WINAPI * PFN_GetDbg)(REFIID, void **);
    HMODULE hD3D12d = GetModuleHandleW(L"d3d12.dll");
    if (!hD3D12d) hD3D12d = LoadLibraryW(L"d3d12.dll");
    auto pfn = hD3D12d ? (PFN_GetDbg)GetProcAddress(hD3D12d, "D3D12GetDebugInterface") : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings *pDred = nullptr;
    if (pfn && SUCCEEDED(pfn(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), (void **)&pDred)) && pDred) {
      if (dredEnv[0] == '1') pDred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      pDred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      pDred->Release();
      log_line("[dred] enabled (breadcrumbs + page faults)\r\n");
    }
  }

  // Attempt to create real D3D12 device
  HRESULT hr = D3D12CreateDevice(static_cast<IUnknown *>(pAdapter),
                                 D3D_FEATURE_LEVEL_11_0, *riid, ppDevice);
  log_line("  -> D3D12CreateDevice returned hr=0x%08lX, ppDevice=%p\r\n",
           (unsigned long)hr, ppDevice ? *ppDevice : nullptr);
  if (FAILED(hr)) {
    // Try with nullptr adapter (default GPU)
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, *riid, ppDevice);
    log_line(
        "  -> D3D12CreateDevice(nullptr) returned hr=0x%08lX, ppDevice=%p\r\n",
        (unsigned long)hr, ppDevice ? *ppDevice : nullptr);
  }

  if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
    g_pMainDevice = static_cast<ID3D12Device *>(*ppDevice);
    if (g_pMainDevice) {
      HRESULT iqHr = g_pMainDevice->QueryInterface(__uuidof(ID3D12InfoQueue),
                                                   (void **)&g_pD3D12InfoQueue);
      log_line("[d3d12_x] Device QueryInterface(ID3D12InfoQueue) -> "
               "hr=0x%08lX, infoQueue=%p\r\n",
               (unsigned long)iqHr, g_pD3D12InfoQueue);
      if (g_pD3D12InfoQueue) {
        g_pD3D12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                              FALSE);
        g_pD3D12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,
                                              FALSE);
        g_pD3D12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,
                                              FALSE);
      }
    }

    void **origDeviceVtable = *(void ***)(*ppDevice);
    if (!g_TrueCreateDescriptorHeap) {
      psodc::g_origDevVtbl = origDeviceVtable;
      InitAllDeviceThunks(g_WrappedDeviceVtable,
                          std::make_integer_sequence<int, 256>());
      int validDeviceSlots = 0;
      for (int i = 0; i < 256; ++i) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(origDeviceVtable[i], &mbi, sizeof(mbi)) &&
            (mbi.Protect == PAGE_EXECUTE_READ ||
             mbi.Protect == PAGE_EXECUTE_READWRITE)) {
          g_WrappedDeviceVtable[i] = origDeviceVtable[i];
          validDeviceSlots = i + 1;
        } else {
          break;
        }
      }
      log_line("[d3d12_x] Populated %d original device vtable slots into "
               "wrapped vtable\r\n",
               validDeviceSlots);

      g_TrueCreateCommandQueue = (PFN_CreateCommandQueue)origDeviceVtable[8];
      g_TrueCreateCommandAllocator =
          (PFN_CreateCommandAllocator)origDeviceVtable[9];
      g_TrueCreateGraphicsPipelineState =
          (PFN_CreateGraphicsPipelineState)origDeviceVtable[10];
      g_TrueCreateComputePipelineState =
          (PFN_CreateComputePipelineState)origDeviceVtable[11];
      g_TrueCreateCommandList = (PFN_CreateCommandList)origDeviceVtable[12];
      g_TrueCreateDescriptorHeap =
          (PFN_CreateDescriptorHeap)origDeviceVtable[14];
      g_TrueCreateRenderTargetView =
          (PFN_CreateRenderTargetView)origDeviceVtable[20];
      g_TrueCreateSRV = (PFN_CreateSRV)origDeviceVtable[18];
      g_TrueCreateUAV = (PFN_CreateUAV)origDeviceVtable[19];
      g_TrueCreateRootSignature = (PFN_CreateRootSignature)origDeviceVtable[16];
      g_TrueCreateCBV = (PFN_CreateCBV)origDeviceVtable[17];
      g_TrueCreateDSV = (PFN_CreateDSV)origDeviceVtable[21];
      g_TrueCopyDescriptors = (PFN_CopyDescriptors)origDeviceVtable[23];
      g_TrueCopyDescriptorsSimple = (PFN_CopyDescriptorsSimple)origDeviceVtable[24];
      g_TrueCreateSampler = (PFN_CreateSampler)origDeviceVtable[22];
      g_TrueGetResourceAllocationInfo =
          (PFN_GetResourceAllocationInfo)origDeviceVtable[25];
      g_TrueGetCustomHeapProperties =
          (PFN_GetCustomHeapProperties)origDeviceVtable[26];
      g_TrueCreateCommittedResource =
          (PFN_CreateCommittedResource)origDeviceVtable[27];
      g_TrueCreateCommandSignature =
          (PFN_CreateCommandSignature)origDeviceVtable[41];

      g_WrappedDeviceVtable[9] = (void *)&HookedCreateCommandAllocatorX;
      g_WrappedDeviceVtable[14] = (void *)&HookedCreateDescriptorHeap;
      g_WrappedDeviceVtable[20] = (void *)&HookedCreateRenderTargetView;
      g_WrappedDeviceVtable[18] = (void *)&HookedCreateSRV;
      g_WrappedDeviceVtable[19] = (void *)&HookedCreateUAV;
      g_WrappedDeviceVtable[16] = (void *)&HookedCreateRootSignature;
      g_WrappedDeviceVtable[17] = (void *)&HookedCreateCBV;
      g_WrappedDeviceVtable[21] = (void *)&HookedCreateDSV;
      g_WrappedDeviceVtable[23] = (void *)&HookedCopyDescriptors;
      g_WrappedDeviceVtable[24] = (void *)&HookedCopyDescriptorsSimple;
      g_WrappedDeviceVtable[25] = (void *)&HookedGetResourceAllocationInfo;
      g_WrappedDeviceVtable[26] = (void *)&HookedGetCustomHeapProperties;
      g_WrappedDeviceVtable[27] = (void *)&HookedCreateCommittedResource;
      g_WrappedDeviceVtable[41] = (void *)&HookedCreateCommandSignature;
      g_WrappedDeviceVtable[44] = (void *)&HookedCreatePlacedResourceX;
      g_WrappedDeviceVtable[48] = (void *)&HookedCreateCommandQueueX;
      g_WrappedDeviceVtable[60] = (void *)&HookedCreateGraphicsPipelineStateX;
      g_WrappedDeviceVtable[61] = (void *)&HookedCreateSamplerX;
      g_WrappedDeviceVtable[62] = (void *)&HookedCreateComputePipelineStateX;
      g_WrappedDeviceVtable[73] = (void *)&HookedCreateCommandListX;
    }
    *(void ***)(*ppDevice) = g_WrappedDeviceVtable;
    log_line("[d3d12_x] Wrapped D3D12 device vtable (intercepted 9, 14, 20, 25, "
             "26, 41, 44, 48, 60, 61, 62, 73, real slots populated)\r\n");
  }

  return hr;
}

static ID3D12CommandAllocator* g_pBlitAllocator = nullptr;
static ID3D12GraphicsCommandList* g_pBlitCommandList = nullptr;
static ID3D12Fence* g_pBlitFence = nullptr;
static UINT64 g_BlitFenceValue = 0;
static HANDLE g_hBlitEvent = nullptr;

static bool InitBlitInfrastructure() {
  if (g_pBlitCommandList) return true;
  if (!g_pMainDevice || !g_pDirectCommandQueue) return false;

  HRESULT hr = g_pMainDevice->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      __uuidof(ID3D12CommandAllocator),
      (void**)&g_pBlitAllocator);
  if (FAILED(hr) || !g_pBlitAllocator) {
    log_line("[d3d12_x] Failed to create blit command allocator hr=0x%08lX\r\n", (unsigned long)hr);
    return false;
  }

  hr = g_pMainDevice->CreateCommandList(
      0,
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      g_pBlitAllocator,
      nullptr,
      __uuidof(ID3D12GraphicsCommandList),
      (void**)&g_pBlitCommandList);
  if (FAILED(hr) || !g_pBlitCommandList) {
    log_line("[d3d12_x] Failed to create blit command list hr=0x%08lX\r\n", (unsigned long)hr);
    return false;
  }

  hr = g_pMainDevice->CreateFence(
      0,
      D3D12_FENCE_FLAG_NONE,
      __uuidof(ID3D12Fence),
      (void**)&g_pBlitFence);
  if (FAILED(hr) || !g_pBlitFence) {
    log_line("[d3d12_x] Failed to create blit fence hr=0x%08lX\r\n", (unsigned long)hr);
    return false;
  }

  g_hBlitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_hBlitEvent) {
    log_line("[d3d12_x] Failed to create blit fence event\r\n");
    return false;
  }

  g_pBlitCommandList->Close(); // Keep closed initially until reset per-frame
  log_line("[d3d12_x] Blit infrastructure initialized successfully (alloc=%p, cmdList=%p, fence=%p)\r\n",
           g_pBlitAllocator, g_pBlitCommandList, g_pBlitFence);
  return true;
}

#pragma pack(push, 1)
struct DiagnosticBMPHeader {
  uint16_t bfType;
  uint32_t bfSize;
  uint16_t bfReserved1;
  uint16_t bfReserved2;
  uint32_t bfOffBits;
  uint32_t biSize;
  int32_t biWidth;
  int32_t biHeight;
  uint16_t biPlanes;
  uint16_t biBitCount;
  uint32_t biCompression;
  uint32_t biSizeImage;
  int32_t biXPelsPerMeter;
  int32_t biYPelsPerMeter;
  uint32_t biClrUsed;
  uint32_t biClrImportant;
};
#pragma pack(pop)

// Resets the shared blit/readback command list robustly: if a previous user left it
// open (or the reset fails), close it first and, as a last resort, recreate it.
static bool ResetBlitListRobust(HRESULT *pRHr, HRESULT *pCHr) {
  if (g_pBlitCommandList) g_pBlitCommandList->Close();  // ignore: fails harmlessly when already closed
  HRESULT rHr = g_pBlitAllocator->Reset();
  HRESULT cHr = g_pBlitCommandList->Reset(g_pBlitAllocator, nullptr);
  if (FAILED(rHr) || FAILED(cHr)) {
    // A previous recording was invalid (Close() failed with E_INVALIDARG); the list is
    // poisoned for good. Recreate allocator + list and carry on.
    if (g_pBlitCommandList) { g_pBlitCommandList->Release(); g_pBlitCommandList = nullptr; }
    if (g_pBlitAllocator) { g_pBlitAllocator->Release(); g_pBlitAllocator = nullptr; }
    HRESULT ha = g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void **)&g_pBlitAllocator);
    HRESULT hl = FAILED(ha) ? ha : g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_pBlitAllocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&g_pBlitCommandList);
    static volatile LONG s_recreated = 0;
    if (InterlockedIncrement(&s_recreated) <= 5)
      log_line("[d3d12_x] Blit list poisoned (rHr=0x%08lX cHr=0x%08lX); recreated alloc/list -> 0x%08lX 0x%08lX\r\n",
               (unsigned long)rHr, (unsigned long)cHr, (unsigned long)ha, (unsigned long)hl);
    rHr = ha; cHr = hl;  // a freshly created list is already open for recording
  }
  if (pRHr) *pRHr = rHr;
  if (pCHr) *pCHr = cHr;
  return SUCCEEDED(rHr) && SUCCEEDED(cHr);
}

static float F11ToFloat(uint32_t v, int bits);
static void DumpBackBufferDiagnostic(ID3D12Resource *pResource, UINT frame, const char *tag) {
  if (!pResource || !g_pMainDevice || !g_pDirectCommandQueue || !g_pBlitCommandList || !g_pBlitAllocator || !g_pBlitFence)
    return;

  D3D12_RESOURCE_DESC desc = {};
  if (!SafeGetResourceDesc(pResource, &desc))
    return;

  UINT width = (UINT)desc.Width;
  UINT height = desc.Height;
  if (width == 0 || height == 0)
    return;
  // The readback and BMP code assume 4 bytes per pixel. An 8-byte target (the in-game HDR scene,
  // R16G16B16A16_FLOAT) gave a footprint with a too-small RowPitch -> blit list failed to close ->
  // executing it removed the device (DXGI_ERROR_INVALID_CALL) right when the tutorial started.
  {
    const int f = (int)desc.Format;
    const bool fourBytes = (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_R8G8B8A8_SINT) ||
                           (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) ||
                           f == DXGI_FORMAT_R16G16B16A16_FLOAT;  // converted to 8-bit below
    if (!fourBytes || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) {
      static volatile LONG s_skip = 0;
      if (InterlockedIncrement(&s_skip) <= 5)
        log_line("[d3d12_x] DumpBackBuffer (%s): skipped format %d (%ux%u, samples %u)\r\n", tag, f, width, height,
                 desc.SampleDesc.Count);
      return;
    }
  }

  const bool halfRgba = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  UINT rowPitch = (width * (halfRgba ? 8 : 4) + 255) & ~255;
  UINT64 totalBytes = (UINT64)rowPitch * height;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_READBACK;

  D3D12_RESOURCE_DESC bufDesc = {};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = totalBytes;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.SampleDesc.Quality = 0;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  ID3D12Resource *pReadbackBuf = nullptr;
  HRESULT hr = g_pMainDevice->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      __uuidof(ID3D12Resource), (void **)&pReadbackBuf);
  if (FAILED(hr) || !pReadbackBuf) {
    log_line("[d3d12_x] DumpBackBuffer (%s): CreateCommittedResource failed hr=0x%08lX\r\n", tag, (unsigned long)hr);
    return;
  }

  HRESULT rHr = S_OK, cHr = S_OK;
  ResetBlitListRobust(&rHr, &cHr);
  if (FAILED(rHr) || FAILED(cHr)) {
    log_line("[d3d12_x] DumpBackBuffer (%s): Reset failed rHr=0x%08lX cHr=0x%08lX\r\n", tag, (unsigned long)rHr, (unsigned long)cHr);
    pReadbackBuf->Release();
    return;
  }

  D3D12_RESOURCE_STATES stateBefore = GetTrackedResourceState(pResource, D3D12_RESOURCE_STATE_COMMON);

  if (stateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER preBarrier = {};
    preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBarrier.Transition.pResource = pResource;
    preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preBarrier.Transition.StateBefore = stateBefore;
    preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_pBlitCommandList->ResourceBarrier(1, &preBarrier);
  }

  DXGI_FORMAT copyFormat = desc.Format;
  if (copyFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) copyFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  else if (copyFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS) copyFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource = pResource;
  srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource = pReadbackBuf;
  dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint.Offset = 0;
  dstLoc.PlacedFootprint.Footprint.Format = copyFormat;
  dstLoc.PlacedFootprint.Footprint.Width = width;
  dstLoc.PlacedFootprint.Footprint.Height = height;
  dstLoc.PlacedFootprint.Footprint.Depth = 1;
  dstLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

  g_pBlitCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

  if (stateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER postBarrier = {};
    postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarrier.Transition.pResource = pResource;
    postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    postBarrier.Transition.StateAfter = stateBefore;
    g_pBlitCommandList->ResourceBarrier(1, &postBarrier);
  }

  g_pBlitCommandList->Close();

  ID3D12CommandList *ppLists[] = { g_pBlitCommandList };
  g_pDirectCommandQueue->ExecuteCommandLists(1, ppLists);

  g_BlitFenceValue++;
  g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
  if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) {
    g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent);
    WaitForSingleObject(g_hBlitEvent, INFINITE);
  }

  uint8_t *pData = nullptr;
  D3D12_RANGE readRange = { 0, (SIZE_T)totalBytes };
  hr = pReadbackBuf->Map(0, &readRange, (void **)&pData);
  std::vector<uint8_t> halfConv;
  if (SUCCEEDED(hr) && pData && desc.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
    // R11G11B10 float target (sky capture panel, lit scene): decode, clamp to [0,1], gamma 2.2, store as RGBA8.
    halfConv.resize((size_t)width * height * 4);
    for (UINT y = 0; y < height; ++y) {
      const uint32_t *src = (const uint32_t *)(pData + (size_t)y * rowPitch);
      uint8_t *dst = halfConv.data() + (size_t)y * width * 4;
      for (UINT x = 0; x < width; ++x) {
        uint32_t v = src[x]; float c[3] = { F11ToFloat(v & 2047u, 11), F11ToFloat((v >> 11) & 2047u, 11), F11ToFloat((v >> 22) & 1023u, 10) };
        for (int k = 0; k < 3; ++k) { float f = c[k]; if (!(f > 0.0f)) f = 0.0f; if (f > 1.0f) f = 1.0f; dst[x * 4 + k] = (uint8_t)(powf(f, 1.0f / 2.2f) * 255.0f + 0.5f); }
        dst[x * 4 + 3] = 255;
      }
    }
    D3D12_RANGE noWrite = { 0, 0 }; pReadbackBuf->Unmap(0, &noWrite); pData = halfConv.data(); rowPitch = width * 4;
  } else if (SUCCEEDED(hr) && pData && halfRgba) {
    // HDR scene target: clamp to [0,1], gamma 2.2, store as RGBA8 for the stats/BMP code below.
    auto h2f = [](uint16_t h) -> float {
      uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
      if (e == 0) { if (!m) bits = s << 31; else { float v = ldexpf((float)m, -24); return s ? -v : v; } }
      else if (e == 31) bits = (s << 31) | 0x7F800000 | (m << 13);
      else bits = (s << 31) | ((e + 112) << 23) | (m << 13);
      float out; memcpy(&out, &bits, 4); return out;
    };
    halfConv.resize((size_t)width * height * 4);
    for (UINT y = 0; y < height; ++y) {
      const uint16_t *src = (const uint16_t *)(pData + (size_t)y * rowPitch);
      uint8_t *dst = halfConv.data() + (size_t)y * width * 4;
      for (UINT x = 0; x < width * 4; ++x) {
        float v = h2f(src[x]);
        if (!(v > 0.0f)) v = 0.0f; if (v > 1.0f) v = 1.0f;
        if ((x & 3) != 3) v = powf(v, 1.0f / 2.2f);
        dst[x] = (uint8_t)(v * 255.0f + 0.5f);
      }
    }
    D3D12_RANGE noWrite = { 0, 0 };
    pReadbackBuf->Unmap(0, &noWrite);
    pData = halfConv.data();
    rowPitch = width * 4;
  }
  if (SUCCEEDED(hr) && pData) {
    UINT64 nonZeroCount = 0;
    UINT64 nonBlackCount = 0;
    uint32_t centerPixel = 0;
    uint8_t minR = 255, minG = 255, minB = 255, minA = 255;
    uint8_t maxR = 0, maxG = 0, maxB = 0, maxA = 0;

    for (UINT y = 0; y < height; ++y) {
      const uint8_t *row = pData + y * rowPitch;
      for (UINT x = 0; x < width; ++x) {
        uint8_t c0 = row[x * 4 + 0];
        uint8_t c1 = row[x * 4 + 1];
        uint8_t c2 = row[x * 4 + 2];
        uint8_t c3 = row[x * 4 + 3];

        if (c0 || c1 || c2 || c3) nonZeroCount++;
        if (c0 || c1 || c2) nonBlackCount++;

        if (c0 < minR) minR = c0; if (c0 > maxR) maxR = c0;
        if (c1 < minG) minG = c1; if (c1 > maxG) maxG = c1;
        if (c2 < minB) minB = c2; if (c2 > maxB) maxB = c2;
        if (c3 < minA) minA = c3; if (c3 > maxA) maxA = c3;

        if (x == width / 2 && y == height / 2) {
          centerPixel = *(const uint32_t *)(row + x * 4);
        }
      }
    }

    log_line("[d3d12_x] Frame %u Diagnostic Dump (%s, res=%p): %ux%u fmt=%d | NonZero=%llu (%.2f%%), NonBlack(RGB)=%llu (%.2f%%), "
             "Min=(%u,%u,%u,%u), Max=(%u,%u,%u,%u), Center=0x%08X\r\n",
             frame, tag, pResource, width, height, desc.Format,
             (unsigned long long)nonZeroCount, (double)nonZeroCount * 100.0 / (width * height),
             (unsigned long long)nonBlackCount, (double)nonBlackCount * 100.0 / (width * height),
             minR, minG, minB, minA, maxR, maxG, maxB, maxA, centerPixel);

    char bmpPath[MAX_PATH];
    sprintf_s(bmpPath, sizeof(bmpPath), "dist_game\\frame_%u_%s.bmp", frame, tag);
    FILE *fp = nullptr;
    fopen_s(&fp, bmpPath, "wb");
    if (!fp) {
      sprintf_s(bmpPath, sizeof(bmpPath), "frame_%u_%s.bmp", frame, tag);
      fopen_s(&fp, bmpPath, "wb");
    }
    if (fp) {
      DiagnosticBMPHeader hdr = {};
      hdr.bfType = 0x4D42;
      hdr.bfOffBits = sizeof(DiagnosticBMPHeader);
      hdr.bfSize = hdr.bfOffBits + width * height * 4;
      hdr.biSize = 40;
      hdr.biWidth = (int32_t)width;
      hdr.biHeight = -(int32_t)height;
      hdr.biPlanes = 1;
      hdr.biBitCount = 32;
      hdr.biCompression = 0;
      hdr.biSizeImage = width * height * 4;

      fwrite(&hdr, sizeof(hdr), 1, fp);

      bool isRgba = (halfRgba || desc.Format == DXGI_FORMAT_R11G11B10_FLOAT || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
      std::vector<uint8_t> outRow(width * 4);
      for (UINT y = 0; y < height; ++y) {
        const uint8_t *srcRow = pData + y * rowPitch;
        if (isRgba) {
          for (UINT x = 0; x < width; ++x) {
            outRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
            outRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
            outRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
            outRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
          }
          fwrite(outRow.data(), 1, width * 4, fp);
        } else {
          fwrite(srcRow, 1, width * 4, fp);
        }
      }
      fclose(fp);
      log_line("  -> Saved screenshot: %s\r\n", bmpPath);
    }

    D3D12_RANGE writeRange = { 0, 0 };
    if (!halfRgba) pReadbackBuf->Unmap(0, &writeRange);
  } else {
    log_line("[d3d12_x] DumpBackBuffer (%s): Map failed hr=0x%08lX\r\n", tag, (unsigned long)hr);
  }

  pReadbackBuf->Release();
}

// Generic texture content statistics via GetCopyableFootprints + readback (works for BC/any format).
static bool DumpTexStats(ID3D12Resource *pRes, UINT &outNonZeroPct10, UINT64 &outBytes) {
  outNonZeroPct10 = 0; outBytes = 0;
  if (!pRes || !g_pMainDevice || !g_pDirectCommandQueue || !InitBlitInfrastructure()) return false;
  D3D12_RESOURCE_DESC desc = {};
  if (!SafeGetResourceDesc(pRes, &desc) || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count > 1) return false;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
  g_pMainDevice->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
  if (!total || total > (256ull << 20)) return false;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
  bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    __uuidof(ID3D12Resource), (void **)&rb)) || !rb) return false;
  HRESULT r1 = S_OK, r2 = S_OK;
  if (!ResetBlitListRobust(&r1, &r2)) { rb->Release(); return false; }
  D3D12_RESOURCE_STATES st = GetTrackedResourceState(pRes, D3D12_RESOURCE_STATE_COMMON);
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = pRes;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) g_pBlitCommandList->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
  dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = fp;
  sl.pResource = pRes; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
  g_pBlitCommandList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g_pBlitCommandList->ResourceBarrier(1, &b); }
  if (FAILED(g_pBlitCommandList->Close())) { rb->Release(); return false; }
  ID3D12CommandList *pls[] = { g_pBlitCommandList };
  g_pDirectCommandQueue->ExecuteCommandLists(1, pls);
  g_BlitFenceValue++;
  g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
  if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) {
    g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent);
    WaitForSingleObject(g_hBlitEvent, INFINITE);
  }
  uint8_t *data = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total };
  bool ok = false;
  if (SUCCEEDED(rb->Map(0, &rr, (void **)&data)) && data) {
    UINT64 nz = 0, n = 0;
    for (UINT y = 0; y < rows; ++y) {
      const uint8_t *row = data + (UINT64)y * fp.Footprint.RowPitch;
      for (UINT64 x = 0; x < rowSize; ++x) { ++n; if (row[x]) ++nz; }
    }
    outNonZeroPct10 = n ? (UINT)(nz * 1000 / n) : 0; outBytes = n; ok = true;
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr);
  }
  rb->Release();
  return ok;
}

static void DumpMipList() {
  uint8_t *hx = (uint8_t *)GetModuleHandleW(nullptr);
  __try {
    uint8_t *lst = *(uint8_t **)(hx + 0x0157E870);
    uint8_t *cur = *(uint8_t **)(hx + 0x0157EF78);
    uint64_t begin = *(uint64_t *)(lst + 8), bytes = *(uint64_t *)(lst + 0x10);
    log_line("[mips] list=%p entries=%llu cur=%p\r\n", lst, (unsigned long long)(bytes / 16), cur);
    for (uint64_t i = 0; i < bytes / 16 && i < 12; ++i) {
      uint8_t *ent = (uint8_t *)begin + i * 16;
      uint8_t *obj = *(uint8_t **)ent; uint64_t owner = *(uint64_t *)(ent + 8);
      char ln[300]; int o = _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[mips]  #%llu obj=%p owner=%llX [obj+0x1c]=%u q:", (unsigned long long)i, obj, (unsigned long long)owner, *(uint32_t *)(obj + 0x1c));
      for (int q = 0; q < 6; ++q) o += _snprintf_s(ln + o, sizeof(ln) - o, _TRUNCATE, " %llX", (unsigned long long)((uint64_t *)obj)[q]);
      log_line("%s\r\n", ln);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { log_line("[mips] exception reading list\r\n"); }
}

static float HalfToFloatHost(uint16_t h) {
  uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v;
  if (e == 0) v = m * (1.0f / 16777216.0f); else if (e == 31) v = (m ? NAN : INFINITY); else v = (1.0f + m / 1024.0f) * powf(2.0f, (float)e - 15.0f);
  return s ? -v : v;
}
// ONRUSH_TEXVAL_FRAME=N: at frame N log value statistics (min/max/mean per channel + a texel grid) of the atmosphere LUT candidates
// (RGBA16F 2D 256x64 / 64x16 and 3D 256x128x32) found in the descriptor map. Debug only.
static void DumpTexValues(ID3D12Resource *pRes, const char *tag) {
  if (!pRes || !g_pMainDevice || !g_pDirectCommandQueue || !InitBlitInfrastructure()) return;
  D3D12_RESOURCE_DESC desc = {};
  if (!SafeGetResourceDesc(pRes, &desc) || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.SampleDesc.Count > 1) return;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
  g_pMainDevice->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
  if (!total || total > (256ull << 20)) return;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
  bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void **)&rb)) || !rb) return;
  HRESULT r1 = S_OK, r2 = S_OK;
  if (!ResetBlitListRobust(&r1, &r2)) { rb->Release(); return; }
  D3D12_RESOURCE_STATES st = GetTrackedResourceState(pRes, D3D12_RESOURCE_STATE_COMMON);
  D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = pRes; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) g_pBlitCommandList->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {}; dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = fp;
  sl.pResource = pRes; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
  g_pBlitCommandList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g_pBlitCommandList->ResourceBarrier(1, &b); }
  if (FAILED(g_pBlitCommandList->Close())) { rb->Release(); return; }
  ID3D12CommandList *pls[] = { g_pBlitCommandList }; g_pDirectCommandQueue->ExecuteCommandLists(1, pls);
  g_BlitFenceValue++; g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
  if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) { g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent); WaitForSingleObject(g_hBlitEvent, INFINITE); }
  uint8_t *data = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total };
  if (SUCCEEDED(rb->Map(0, &rr, (void **)&data)) && data) {
    UINT W = (UINT)desc.Width, H = desc.Height, D = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
    UINT pitch = fp.Footprint.RowPitch; float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f}, mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f}; double sum[4] = {}; UINT64 cnt = 0;
    auto texel = [&](UINT x, UINT y, UINT z, float *o) { const uint16_t *p = (const uint16_t *)(data + (UINT64)z * pitch * H + (UINT64)y * pitch + (UINT64)x * 8); for (int c = 0; c < 4; ++c) o[c] = HalfToFloatHost(p[c]); };
    for (UINT z = 0; z < D; ++z) for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) { float v[4]; texel(x, y, z, v); for (int c = 0; c < 4; ++c) { if (v[c] < mn[c]) mn[c] = v[c]; if (v[c] > mx[c]) mx[c] = v[c]; sum[c] += v[c]; } ++cnt; }
    log_line("[texval] %s %p %ux%ux%u: min=(%g %g %g %g) max=(%g %g %g %g) mean=(%g %g %g %g)\r\n", tag, (void *)pRes, W, H, D, mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
             sum[0] / cnt, sum[1] / cnt, sum[2] / cnt, sum[3] / cnt);
    UINT zs = D / 2;
    for (UINT gy = 0; gy < 6; ++gy) {
      char ln[600]; int o = _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[texval]   y=%3u:", H * gy / 5 >= H ? H - 1 : H * gy / 5);
      for (UINT gx = 0; gx < 6; ++gx) { UINT x = W * gx / 5 >= W ? W - 1 : W * gx / 5, y = H * gy / 5 >= H ? H - 1 : H * gy / 5; float v[4]; texel(x, y, zs, v);
        o += _snprintf_s(ln + o, sizeof(ln) - o, _TRUNCATE, " (%.3f %.3f %.3f %.3f)", v[0], v[1], v[2], v[3]); }
      log_line("%s\r\n", ln);
    }
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr);
  }
  rb->Release();
}
static float F11ToFloat(uint32_t v, int bits) {   // unsigned 11/10-bit float (5-bit exponent)
  uint32_t m = v & ((1u << (bits - 5)) - 1), e = (v >> (bits - 5)) & 31; float mant = (float)m / (float)(1u << (bits - 5));
  if (e == 0) return mant * (1.0f / 16384.0f); if (e == 31) return INFINITY; return (1.0f + mant) * powf(2.0f, (float)e - 15.0f);
}
// Average colour of every slice of a 256x256x48 R11G11B10 texture array (the environment cubemaps: 8 cubes x 6 faces).
static void DumpCubeFaces(ID3D12Resource *pRes) {
  D3D12_RESOURCE_DESC desc = {};
  if (!pRes || !g_pMainDevice || !g_pDirectCommandQueue || !InitBlitInfrastructure() || !SafeGetResourceDesc(pRes, &desc)) return;
  UINT mips = desc.MipLevels ? desc.MipLevels : 1, slices = desc.DepthOrArraySize;
  for (UINT sl = 0; sl < slices; ++sl) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g_pMainDevice->GetCopyableFootprints(&desc, sl * mips, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void **)&rb)) || !rb) return;
    HRESULT r1 = S_OK, r2 = S_OK; if (!ResetBlitListRobust(&r1, &r2)) { rb->Release(); return; }
    D3D12_RESOURCE_STATES st = GetTrackedResourceState(pRes, D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = pRes; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) g_pBlitCommandList->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sloc = {}; dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = fp;
    sloc.pResource = pRes; sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sloc.SubresourceIndex = sl * mips;
    g_pBlitCommandList->CopyTextureRegion(&dl, 0, 0, 0, &sloc, nullptr);
    if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g_pBlitCommandList->ResourceBarrier(1, &b); }
    if (FAILED(g_pBlitCommandList->Close())) { rb->Release(); return; }
    ID3D12CommandList *pls[] = { g_pBlitCommandList }; g_pDirectCommandQueue->ExecuteCommandLists(1, pls);
    g_BlitFenceValue++; g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
    if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) { g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent); WaitForSingleObject(g_hBlitEvent, INFINITE); }
    uint8_t *data = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total };
    if (SUCCEEDED(rb->Map(0, &rr, (void **)&data)) && data) {
      double sum[3] = {}; UINT64 n = 0; UINT W = (UINT)desc.Width, H = desc.Height;
      for (UINT y = 0; y < H; y += 4) for (UINT x = 0; x < W; x += 4) { uint32_t v = *(const uint32_t *)(data + (UINT64)y * fp.Footprint.RowPitch + x * 4);
        sum[0] += F11ToFloat(v & 2047u, 11); sum[1] += F11ToFloat((v >> 11) & 2047u, 11); sum[2] += F11ToFloat((v >> 22) & 1023u, 10); ++n; }
      log_line("[cubeface] %p cube=%u face=%u avg=(%.3f %.3f %.3f)\r\n", (void *)pRes, sl / 6, sl % 6, sum[0] / n, sum[1] / n, sum[2] / n);
      D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr);
    }
    rb->Release();
  }
}
// Colour grid (8x8 texels spread over the whole texture) + mean of a single-slice R11G11B10 texture, e.g. the 1536x768 sky capture panel.
static void DumpR11Grid(ID3D12Resource *pRes, const char *tag) {
  D3D12_RESOURCE_DESC desc = {};
  if (!pRes || !g_pMainDevice || !g_pDirectCommandQueue || !InitBlitInfrastructure() || !SafeGetResourceDesc(pRes, &desc)) return;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
  g_pMainDevice->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
  if (!total || total > (64ull << 20)) return;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void **)&rb)) || !rb) return;
  HRESULT r1 = S_OK, r2 = S_OK; if (!ResetBlitListRobust(&r1, &r2)) { rb->Release(); return; }
  D3D12_RESOURCE_STATES st = GetTrackedResourceState(pRes, D3D12_RESOURCE_STATE_COMMON);
  D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = pRes; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) g_pBlitCommandList->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dl = {}, sloc = {}; dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = fp;
  sloc.pResource = pRes; sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sloc.SubresourceIndex = 0;
  g_pBlitCommandList->CopyTextureRegion(&dl, 0, 0, 0, &sloc, nullptr);
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g_pBlitCommandList->ResourceBarrier(1, &b); }
  if (FAILED(g_pBlitCommandList->Close())) { rb->Release(); return; }
  ID3D12CommandList *pls[] = { g_pBlitCommandList }; g_pDirectCommandQueue->ExecuteCommandLists(1, pls);
  g_BlitFenceValue++; g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
  if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) { g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent); WaitForSingleObject(g_hBlitEvent, INFINITE); }
  uint8_t *data = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total };
  if (SUCCEEDED(rb->Map(0, &rr, (void **)&data)) && data) {
    UINT W = (UINT)desc.Width, H = desc.Height; double sum[3] = {}; UINT64 n = 0;
    auto px = [&](UINT x, UINT y, float *o) { uint32_t v = *(const uint32_t *)(data + (UINT64)y * fp.Footprint.RowPitch + x * 4); o[0] = F11ToFloat(v & 2047u, 11); o[1] = F11ToFloat((v >> 11) & 2047u, 11); o[2] = F11ToFloat((v >> 22) & 1023u, 10); };
    for (UINT y = 0; y < H; y += 4) for (UINT x = 0; x < W; x += 4) { float c[3]; px(x, y, c); for (int k = 0; k < 3; ++k) sum[k] += c[k]; ++n; }
    log_line("[r11grid] %s %p %ux%u mean=(%.3f %.3f %.3f)\r\n", tag, (void *)pRes, W, H, sum[0] / n, sum[1] / n, sum[2] / n);
    for (UINT gy = 0; gy < 8; ++gy) {
      char ln[700]; int o = _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[r11grid]  y=%3u:", H * gy / 8);
      for (UINT gx = 0; gx < 8; ++gx) { float c[3]; px(W * gx / 8 + W / 16, H * gy / 8 + H / 16, c); o += _snprintf_s(ln + o, sizeof(ln) - o, _TRUNCATE, " (%.2f %.2f %.2f)", c[0], c[1], c[2]); }
      log_line("%s\r\n", ln);
    }
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr);
  }
  rb->Release();
}
// Whole-volume statistics of a small RGBA8 3D texture (baked light/AO volumes): % of non-zero bytes and mean RGB.
static bool Dump3DRgba8Stats(ID3D12Resource *pRes, double &nzPct, double *mean) {
  nzPct = 0; mean[0] = mean[1] = mean[2] = 0;
  D3D12_RESOURCE_DESC desc = {};
  if (!pRes || !g_pMainDevice || !g_pDirectCommandQueue || !InitBlitInfrastructure() || !SafeGetResourceDesc(pRes, &desc)) return false;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
  g_pMainDevice->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
  if (!total || total > (32ull << 20)) return false;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void **)&rb)) || !rb) return false;
  HRESULT r1 = S_OK, r2 = S_OK; if (!ResetBlitListRobust(&r1, &r2)) { rb->Release(); return false; }
  D3D12_RESOURCE_STATES st = GetTrackedResourceState(pRes, D3D12_RESOURCE_STATE_COMMON);
  // only resources whose tracked state is a plain SRV/COMMON state: other states are not reliable and a wrong barrier removes the device
  if (st != D3D12_RESOURCE_STATE_COMMON && st != (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && st != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) { rb->Release(); return false; }
  D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = pRes; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) g_pBlitCommandList->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dl = {}, sloc = {}; dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = fp;
  sloc.pResource = pRes; sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sloc.SubresourceIndex = 0;
  g_pBlitCommandList->CopyTextureRegion(&dl, 0, 0, 0, &sloc, nullptr);
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g_pBlitCommandList->ResourceBarrier(1, &b); }
  if (FAILED(g_pBlitCommandList->Close())) { rb->Release(); return false; }
  ID3D12CommandList *pls[] = { g_pBlitCommandList }; g_pDirectCommandQueue->ExecuteCommandLists(1, pls);
  g_BlitFenceValue++; g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
  if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) { g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent); WaitForSingleObject(g_hBlitEvent, INFINITE); }
  uint8_t *data = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total }; bool ok = false;
  if (SUCCEEDED(rb->Map(0, &rr, (void **)&data)) && data) {
    UINT W = (UINT)desc.Width, H = desc.Height, D = desc.DepthOrArraySize; UINT64 nz = 0, n = 0; double sum[3] = {};
    for (UINT z = 0; z < D; ++z) for (UINT y = 0; y < H; ++y) { const uint8_t *row = data + (UINT64)z * fp.Footprint.RowPitch * H + (UINT64)y * fp.Footprint.RowPitch;
      for (UINT x = 0; x < W; ++x) { const uint8_t *p = row + x * 4; if (p[0] | p[1] | p[2] | p[3]) ++nz; sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++n; } }
    nzPct = n ? 100.0 * nz / n : 0; for (int k = 0; k < 3; ++k) mean[k] = n ? sum[k] / n / 255.0 : 0; ok = true;
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr);
  }
  rb->Release(); return ok;
}
static void TexValReport() {
  std::vector<ID3D12Resource *> seen;
  AcquireSRWLockShared(&g_descLock);
  for (auto &kv : g_descMap) { ID3D12Resource *r = kv.second.res; if (!r) continue; bool dup = false; for (auto *q : seen) if (q == r) { dup = true; break; } if (!dup) seen.push_back(r); }
  ReleaseSRWLockShared(&g_descLock);
  for (size_t i = 0; i < seen.size() && i < 3000; ++i) {
    D3D12_RESOURCE_DESC rd = {}; if (!SafeGetResourceDesc(seen[i], &rd)) continue;
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width == 256 && rd.Height == 256 && rd.DepthOrArraySize >= 6)
      log_line("[cubecand] %p fmt=%d slices=%u mips=%u\r\n", (void *)seen[i], (int)rd.Format, (UINT)rd.DepthOrArraySize, (UINT)rd.MipLevels);
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Format == DXGI_FORMAT_R11G11B10_FLOAT && rd.Width == 256 && rd.Height == 256 && (rd.DepthOrArraySize == 48 || rd.DepthOrArraySize == 24) && GetEnvironmentVariableA("ONRUSH_CUBEDUMP", nullptr, 0) > 0) { DumpCubeFaces(seen[i]); continue; }
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Format == DXGI_FORMAT_R11G11B10_FLOAT && rd.DepthOrArraySize == 1 && rd.Width == 1536 && rd.Height == 768 && GetEnvironmentVariableA("ONRUSH_CUBEDUMP", nullptr, 0) > 0) { DumpR11Grid(seen[i], "skypanel"); continue; }
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM) && rd.Width <= 64 && GetEnvironmentVariableA("ONRUSH_VOLDUMP", nullptr, 0) > 0) {
      double nzp = 0, mean[3]; static int s_vn = 0; static int s_vzero = 0, s_vdata = 0;
      if (Dump3DRgba8Stats(seen[i], nzp, mean)) { if (nzp < 0.5) ++s_vzero; else ++s_vdata;
        if (++s_vn <= 60) log_line("[vol3d] %p %ux%ux%u fmt=%d nonzero=%.1f%% mean=(%.3f %.3f %.3f)\r\n", (void *)seen[i], (UINT)rd.Width, rd.Height, (UINT)rd.DepthOrArraySize, (int)rd.Format, nzp, mean[0], mean[1], mean[2]);
        if (s_vn % 20 == 0) log_line("[vol3d] so far: all-zero=%d with data=%d\r\n", s_vzero, s_vdata); }
      continue;
    }
    if (rd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) continue;
    bool d3 =rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D, d2 = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bool want = (d2 && ((rd.Width == 256 && rd.Height == 64) || (rd.Width == 64 && rd.Height == 16))) || (d3 && rd.Width == 256 && rd.Height == 128 && rd.DepthOrArraySize == 32);
    bool fog = d3 && rd.DepthOrArraySize == 128 && ((rd.Width == 60 && rd.Height == 34) || (rd.Width == 30 && rd.Height == 17));   // volumetric fog froxels
    if (want || fog) DumpTexValues(seen[i], fog ? "fogvol" : (d3 ? "scatter3d" : (rd.Width == 256 ? "transmittance" : "irradiance")));
  }
}
static void TexStatsReport() {
        std::vector<ID3D12Resource *> seen;
        AcquireSRWLockShared(&g_descLock);
        for (auto &kv : g_descMap) {
          ID3D12Resource *r = kv.second.res;
          if (!r || kv.second.kind != 1) continue;
          bool dup = false; for (auto *q : seen) if (q == r) { dup = true; break; }
          if (!dup) seen.push_back(r);
        }
        ReleaseSRWLockShared(&g_descLock);
        LONG zeroTex = 0, dataTex = 0, failed = 0;
        for (size_t i = 0; i < seen.size() && i < 400; ++i) {
          UINT pct = 0; UINT64 by = 0;
          bool ok = DumpTexStats(seen[i], pct, by);
          D3D12_RESOURCE_DESC rd = {}; SafeGetResourceDesc(seen[i], &rd);
          if (!ok) { ++failed; continue; }
          if (pct == 0) ++zeroTex; else ++dataTex;
          if (i < 60 || pct == 0)
            if (i < 120) log_line("[tex] %p %llux%u mips=%u fmt=%d nonzero=%u.%u%% of %llu bytes\r\n", (void *)seen[i], (unsigned long long)rd.Width, rd.Height,
                                  (UINT)rd.MipLevels, (int)rd.Format, pct / 10, pct % 10, (unsigned long long)by);
        }
        log_line("[tex] SRV textures tracked=%u: with data=%ld all-zero=%ld unreadable=%ld\r\n", (UINT)seen.size(), (long)dataTex, (long)zeroTex, (long)failed);

}

static void GpuCbvProbe() {
  LONG n = g_cbvProbeCount; if (n <= 0 || !g_pMainDevice) return;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr;
  if (FAILED(g_pMainDevice->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return;
  g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&ca);
  g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
  g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
  const UINT64 kBytes = 148 * 16;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kBytes * 24; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return;
  UINT64 sizes[24] = {};
  for (LONG i = 0; i < n && i < 24; ++i) { UINT64 c = g_cbvProbe[i].avail < kBytes ? g_cbvProbe[i].avail : kBytes; c &= ~3ull; sizes[i] = c; if (c) cl->CopyBufferRegion(rb, i * kBytes, g_cbvProbe[i].res, g_cbvProbe[i].off, c); }
  cl->Close(); ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, 1);
  HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
  uint8_t* mp = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)(kBytes * 24) };
  if (SUCCEEDED(rb->Map(0, &rr, (void**)&mp)) && mp) {
    for (LONG i = 0; i < n && i < 24; ++i) { const float* f = (const float*)(mp + i * kBytes); UINT64 nw = sizes[i] / 4; int nz = 0; for (UINT64 k = 0; k < nw; ++k) if (f[k] != 0.0f) ++nz;
      log_line("[gpucbv] #%ld draw#%u param%d res=%p off=%llu nonzero=%d/%llu first=%g %g %g %g | %g %g %g %g\r\n", (long)i, g_cbvProbe[i].draw, g_cbvProbe[i].param, (void*)g_cbvProbe[i].res, (unsigned long long)g_cbvProbe[i].off, nz, (unsigned long long)nw, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
      if (nw >= 84 * 4) for (int r : { 12, 13, 14, 15, 16, 17, 18, 19, 82, 83 }) log_line("[gpucbv]   #%ld c%d = %g %g %g %g\r\n", (long)i, r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]); }
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr); }
}
static void GpuGeoProbe() {
  LONG n = g_geoProbeCount; if (n <= 0 || !g_pMainDevice) return;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr;
  if (FAILED(g_pMainDevice->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return;
  g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&ca);
  g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
  g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
  const UINT64 kB = 4096;
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kB * 48; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return;
  UINT64 sizes[48] = {};
  for (LONG i = 0; i < n && i < 48; ++i) { UINT64 c = g_geoProbe[i].avail < kB ? g_geoProbe[i].avail : kB; c &= ~3ull; sizes[i] = c; if (c) cl->CopyBufferRegion(rb, i * kB, g_geoProbe[i].res, g_geoProbe[i].off, c); }
  cl->Close(); ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, 1);
  HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
  uint8_t* mp = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)(kB * 48) };
  if (SUCCEEDED(rb->Map(0, &rr, (void**)&mp)) && mp) {
    for (LONG i = 0; i < n && i < 48; ++i) { const uint32_t* u = (const uint32_t*)(mp + i * kB); const float* f = (const float*)u; UINT64 nw = sizes[i] / 4; int nz = 0; uint32_t mx = 0; for (UINT64 k = 0; k < nw; ++k) { if (u[k]) ++nz; if (u[k] > mx && g_geoProbe[i].kind == 0) mx = u[k]; }
      if (g_geoProbe[i].kind == 0) log_line("[gpugeo] #%ld draw#%u IB nonzero=%d/%llu first=%08X %08X %08X %08X %08X %08X max=%u\r\n", (long)i, g_geoProbe[i].draw, nz, (unsigned long long)nw, u[0], u[1], u[2], u[3], u[4], u[5], mx);
      else log_line("[gpugeo] #%ld draw#%u VB nonzero=%d/%llu stride=%u first=%g %g %g %g %g %g %g %g\r\n", (long)i, g_geoProbe[i].draw, nz, (unsigned long long)nw, g_geoProbe[i].stride, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]); }
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr); }
}
static void GpuScanPlaced() {
  LONG n = g_placedBufCount; if (n > 20000) n = 20000; if (n <= 0 || !g_pMainDevice) return; if (n > 1024) n = 1024;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr;
  if (FAILED(g_pMainDevice->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return;
  g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&ca);
  g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
  g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
  const UINT64 kB = 4096; D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kB * n; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr;
  if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return;
  std::vector<UINT64> sz(n);
  for (LONG i = 0; i < n; ++i) { UINT64 c = g_placedBufs[i].size < kB ? g_placedBufs[i].size : kB; c &= ~3ull; sz[i] = c; if (c) cl->CopyBufferRegion(rb, i * kB, g_placedBufs[i].res, 0, c); }
  cl->Close(); ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, 1);
  HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 8000); CloseHandle(ev);
  uint8_t* mp = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)(kB * n) }; int withData = 0, zero = 0; UINT64 bytesData = 0;
  if (SUCCEEDED(rb->Map(0, &rr, (void**)&mp)) && mp) {
    for (LONG i = 0; i < n; ++i) { const uint32_t* u = (const uint32_t*)(mp + i * kB); bool nz = false; for (UINT64 k = 0; k < sz[i] / 4; ++k) if (u[k]) { nz = true; break; }
      if (g_placedBufs[i].size % 144 == 0 || g_placedBufs[i].size % 24 == 0 || g_placedBufs[i].size % 36 == 0) log_line("[gpuscan] stride-candidate #%ld size=%llu (d136=%d d28=%d) data=%d first=%08X %08X %08X %08X %08X %08X %08X %08X\r\n", (long)i, (unsigned long long)g_placedBufs[i].size, (int)(g_placedBufs[i].size % 136 == 0), (int)(g_placedBufs[i].size % 28 == 0), (int)nz, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]);
      if (nz) { ++withData; bytesData += g_placedBufs[i].size; if (withData <= 40) log_line("[gpuscan] #%ld size=%llu va=%llX HAS DATA first=%08X %08X %08X %08X\r\n", (long)i, (unsigned long long)g_placedBufs[i].size, (unsigned long long)g_placedBufs[i].va, u[0], u[1], u[2], u[3]); } else ++zero; }
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr); }
  log_line("[gpuscan] placed buffers scanned=%ld withData=%d (%llu bytes) allZero=%d\r\n", (long)n, withData, (unsigned long long)bytesData, zero);
}
static void GpuXProbe() {
  LONG n = g_xProbeCount; if (n <= 0 || !g_pMainDevice) return;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr;
  if (FAILED(g_pMainDevice->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return;
  g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&ca);
  g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
  g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
  const UINT64 kB = 256; D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kB * 32; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr; if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return;
  UINT64 szA[16] = {}, szC[16] = {};
  for (LONG i = 0; i < n; ++i) { D3D12_RESOURCE_DESC rd = {}; SafeGetResourceDesc(g_xProbe[i].arg, &rd); UINT64 av = rd.Width > g_xProbe[i].aoff ? rd.Width - g_xProbe[i].aoff : 0; UINT64 c = av < kB ? av : kB; c &= ~3ull; szA[i] = c; if (c) cl->CopyBufferRegion(rb, (2 * i) * kB, g_xProbe[i].arg, g_xProbe[i].aoff, c);
    if (g_xProbe[i].cnt) { szC[i] = 16; cl->CopyBufferRegion(rb, (2 * i + 1) * kB, g_xProbe[i].cnt, g_xProbe[i].coff, 16); } }
  cl->Close(); ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, 1);
  HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
  uint8_t* mp = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)(kB * 32) };
  if (SUCCEEDED(rb->Map(0, &rr, (void**)&mp)) && mp) {
    for (LONG i = 0; i < n; ++i) { const uint32_t* u = (const uint32_t*)(mp + (2 * i) * kB); const uint32_t* c = (const uint32_t*)(mp + (2 * i + 1) * kB); int nz = 0; for (UINT64 k = 0; k < szA[i] / 4; ++k) if (u[k]) ++nz;
      log_line("[gpux] #%ld sigkind=%d max=%u arg=%p off=%llu argNZ=%d/%llu args=%08X %08X %08X %08X %08X %08X %08X %08X count=%s %08X\r\n", (long)i, g_xProbe[i].sigKind, g_xProbe[i].maxc, (void*)g_xProbe[i].arg, (unsigned long long)g_xProbe[i].aoff, nz, (unsigned long long)(szA[i] / 4), u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], g_xProbe[i].cnt ? "buf" : "none", g_xProbe[i].cnt ? c[0] : 0u); }
    D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr); }
}
static void GpuRtProbe() {
  LONG n = g_xProbeCount; if (n <= 0 || !g_pMainDevice) return;
  std::vector<ID3D12Resource*> rts; for (LONG i = 0; i < n; ++i) { ID3D12Resource* r = g_xProbe[i].rt; if (!r) continue; bool d = false; for (auto* q : rts) if (q == r) d = true; if (!d) rts.push_back(r); }
  log_line("[gpurt] probing %zu distinct RTs (xprobes=%ld)\r\n", rts.size(), (long)n);
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; ID3D12CommandQueue* q = nullptr;
  if (FAILED(g_pMainDevice->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return;
  for (ID3D12Resource* rt : rts) {
    D3D12_RESOURCE_DESC rd = {}; if (!SafeGetResourceDesc(rt, &rd) || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { log_line("[gpurt] skip rt=%p dim=%d\r\n", (void*)rt, (int)rd.Dimension); continue; }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0; g_pMainDevice->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr; if (FAILED(g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) continue;
    ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr;
    g_pMainDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&ca); g_pMainDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl); g_pMainDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
    D3D12_RESOURCE_BARRIER bar = {}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar.Transition.pResource = rt; bar.Transition.Subresource = 0; bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; cl->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp; D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; cl->ResourceBarrier(1, &bar); cl->Close();
    ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, 1); HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
    uint8_t* mp = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total }; if (SUCCEEDED(rb->Map(0, &rr, (void**)&mp)) && mp) {
      UINT64 nz = 0, npx = 0; unsigned first = 0; for (UINT y = 0; y < rows; ++y) { const uint32_t* row = (const uint32_t*)(mp + fp.Offset + (UINT64)y * fp.Footprint.RowPitch); UINT w = fp.Footprint.Width; for (UINT x = 0; x < w; ++x) { ++npx; if (row[x]) { ++nz; if (!first) first = row[x]; } } }
      log_line("[gpurt] rt=%p %llux%u fmt=%d nonzeroPixels=%llu/%llu firstNonZero=%08X\r\n", (void*)rt, (unsigned long long)rd.Width, rd.Height, (int)rd.Format, (unsigned long long)nz, (unsigned long long)npx, first); D3D12_RANGE wr = { 0, 0 }; rb->Unmap(0, &wr); }
  }
}
static void TsSetup() {
  if (g_tsHeap || !g_pMainDevice) return;
  D3D12_QUERY_HEAP_DESC qd = {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2048; g_pMainDevice->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), (void**)&g_tsHeap);
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2048 * 8; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_tsReadback);
}
static void TsReport() {
  if (!g_tsReadback || !g_pMainDevice) return;
  LONG n = g_tsNext; if (n > 1024) n = 1024; if (n <= 0) return;
  D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; ID3D12CommandQueue* q = nullptr; if (FAILED(g_pMainDevice->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return;
  UINT64 freq = 0; q->GetTimestampFrequency(&freq); if (!freq) freq = 1;
  UINT64* ts = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)n * 16 }; if (FAILED(g_tsReadback->Map(0, &rr, (void**)&ts)) || !ts) return;
  struct Row { void* pso; double us; UINT gx, gy, gz; }; std::vector<Row> rows; double total = 0;
  for (LONG i = 0; i < n; ++i) { UINT64 a = ts[2 * i], c = ts[2 * i + 1]; double us = (c > a) ? (double)(c - a) * 1e6 / (double)freq : 0.0; total += us; rows.push_back({ g_tsInfo[i].pso, us, g_tsInfo[i].gx, g_tsInfo[i].gy, g_tsInfo[i].gz }); }
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.us > y.us; });
  log_line("[ts] dispatches=%ld totalGpuUs=%.0f\r\n", (long)n, total);
  for (size_t k = 0; k < rows.size() && k < 14; ++k) log_line("[ts] #%zu pso=%p us=%.0f groups=%u,%u,%u\r\n", k, rows[k].pso, rows[k].us, rows[k].gx, rows[k].gy, rows[k].gz);
  D3D12_RANGE wr = { 0, 0 }; g_tsReadback->Unmap(0, &wr);
}
static void PsQuerySetup() {
  if (g_psQueryHeap || !g_pMainDevice) return;
  D3D12_QUERY_HEAP_DESC qd = {}; qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; qd.Count = kPsMax;
  g_pMainDevice->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), (void **)&g_psQueryHeap);
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kPsMax * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
  bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  g_pMainDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         __uuidof(ID3D12Resource), (void **)&g_psReadback);
  log_line("[psq] query heap=%p readback=%p\r\n", (void *)g_psQueryHeap, (void *)g_psReadback);
}

static void PsQueryReport() {
  if (!g_psReadback) return;
  LONG n = g_psNext; if (n > (LONG)kPsMax) n = kPsMax;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS *d = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)n * sizeof(*d) };
  if (FAILED(g_psReadback->Map(0, &rr, (void **)&d)) || !d) return;
  LONG zeroPs = 0;
  for (LONG i = 0; i < n; ++i) {
    if (d[i].PSInvocations == 0) ++zeroPs;
    if (i < (GetEnvironmentVariableA("ONRUSH_PSQ_VS", nullptr, 0) > 0 ? 1024 : 60))
      log_line("[psq] #%ld %c count=%u pso=%p vsh=%016llx psh=%016llx rc0=%08X rt=%p IAverts=%llu IAprims=%llu VS=%llu CInv=%llu CPrim=%llu PS=%llu\r\n", (long)i, g_psInfo[i].kind, g_psInfo[i].count,
               g_psInfo[i].pso, g_psInfo[i].vs, g_psInfo[i].ps, g_psInfo[i].rc0, g_psInfo[i].rt, (unsigned long long)d[i].IAVertices, (unsigned long long)d[i].IAPrimitives,
               (unsigned long long)d[i].VSInvocations, (unsigned long long)d[i].CInvocations, (unsigned long long)d[i].CPrimitives,
               (unsigned long long)d[i].PSInvocations);
  }
  { unsigned long long sumPS = 0, sumVS = 0, sumPrim = 0; LONG nx = 0, nxPS = 0, nd = 0, ndPS = 0;
    for (LONG i = 0; i < n; ++i) { if (g_psInfo[i].kind == 'X') { ++nx; sumPS += d[i].PSInvocations; sumVS += d[i].VSInvocations; sumPrim += d[i].CPrimitives; if (d[i].PSInvocations) ++nxPS; } else { ++nd; if (d[i].PSInvocations) ++ndPS; } }
    log_line("[psq] EXECUTEINDIRECT calls=%ld withPS=%ld sumVS=%llu sumPrims=%llu sumPS=%llu; direct draws=%ld withPS=%ld\r\n", (long)nx, (long)nxPS, sumVS, sumPrim, sumPS, (long)nd, (long)ndPS);
    { struct Agg { void* pso; void* rt; unsigned long long vs, prim, ps; int calls; }; std::vector<Agg> ag;
      for (LONG i = 0; i < n; ++i) { if (g_psInfo[i].kind != 'X') continue; Agg* a = nullptr; for (auto& q : ag) if (q.pso == g_psInfo[i].pso && q.rt == g_psInfo[i].rt) { a = &q; break; } if (!a) { ag.push_back({ g_psInfo[i].pso, g_psInfo[i].rt, 0, 0, 0, 0 }); a = &ag.back(); } a->vs += d[i].VSInvocations; a->prim += d[i].CPrimitives; a->ps += d[i].PSInvocations; a->calls++; }
      for (auto& q : ag) log_line("[psq] XAGG pso=%p rt=%p calls=%d VS=%llu CPrim=%llu PS=%llu\r\n", q.pso, q.rt, q.calls, q.vs, q.prim, q.ps); }
    int shown = 0; for (LONG i = 0; i < n && shown < 16; ++i) if (g_psInfo[i].kind == 'X') { ++shown; log_line("[psq] X#%ld max=%u pso=%p rt=%p VS=%llu CPrim=%llu PS=%llu\r\n", (long)i, g_psInfo[i].count, g_psInfo[i].pso, g_psInfo[i].rt, (unsigned long long)d[i].VSInvocations, (unsigned long long)d[i].CPrimitives, (unsigned long long)d[i].PSInvocations); } }
  log_line("[psq] draws=%ld withZeroPS=%ld\r\n", (long)n, (long)zeroPs);
  D3D12_RANGE wr = { 0, 0 }; g_psReadback->Unmap(0, &wr);
}

static bool NoForcePhase() {
  // Natural engine-driven flow is the default; ONRUSH_FORCE_PHASE=1 restores the old Present-driven pacing.
  static const bool v = !InputEnvFlag("ONRUSH_FORCE_PHASE");
  return v;
}

static int PF_shift() { static int s = -99999; if (s == -99999) { char mb[16]; s = GetEnvironmentVariableA("ONRUSH_PROBE_SHIFT", mb, sizeof(mb)) > 0 ? atoi(mb) : 0; } return s; }
#define PF(x) ((UINT)((int)(x) + PF_shift()))

#include <tlhelp32.h>

// ---- memory pattern scan + hardware write watch (diagnostics) ----
// ONRUSH_MEMSCAN_AT=frame ONRUSH_MEMSCAN_PAT=<hex dwords, comma separated> : at that frame every committed private/mapped
// region is scanned for the dword sequence ([memscan] lines). ONRUSH_MEMWATCH_OFF=<byte offset> also arms a hardware
// write breakpoint (DR0, 4 bytes) on match+offset in every thread; writers are logged once per RIP ([memwatch]).
static volatile LONG g_mwHits = 0; static void* g_mwRips[64]; static volatile LONG g_mwNRips = 0; static uintptr_t g_mwAddr = 0;
static LONG CALLBACK MemWatchVeh(PEXCEPTION_POINTERS ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
  ep->ContextRecord->Dr6 = 0;
  void* rip = (void*)ep->ContextRecord->Rip; bool seen = false; LONG n = g_mwNRips;
  for (LONG i = 0; i < n && i < 64; ++i) if (g_mwRips[i] == rip) seen = true;
  if (!seen && n < 64) {
    LONG id = InterlockedIncrement(&g_mwNRips) - 1; if (id < 64) g_mwRips[id] = rip;
    HMODULE hExe = GetModuleHandleW(nullptr);
    char bt[512]; int o = 0; void* frames[12] = {}; USHORT nf = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT k = 0; k < nf; ++k) { uintptr_t f = (uintptr_t)frames[k]; if (f >= (uintptr_t)hExe && f < (uintptr_t)hExe + 0x4000000) o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " exe+%llX", (unsigned long long)(f - (uintptr_t)hExe)); }
    log_line("[memwatch] access at exe+%llX (rip=%p) value now=%08X tid=%lu bt:%s\r\n", (unsigned long long)((uintptr_t)rip - (uintptr_t)hExe), rip,
             g_mwAddr ? *(volatile UINT*)g_mwAddr : 0u, GetCurrentThreadId(), bt);
  }
  InterlockedIncrement(&g_mwHits);
  return EXCEPTION_CONTINUE_EXECUTION;
}
static void MemWatchArm(uintptr_t addr) {
  g_mwAddr = addr;
  static bool s_veh = false; if (!s_veh) { s_veh = true; AddVectoredExceptionHandler(1, MemWatchVeh); }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te = { sizeof(te) }; int armed = 0; DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID); if (!h) continue;
    bool other = te.th32ThreadID != self; if (other) SuspendThread(h);
    CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    static const bool s_rw = GetEnvironmentVariableA("ONRUSH_MEMWATCH_RW", nullptr, 0) > 0;   // break on reads too (RW=11)
    if (GetThreadContext(h, &c)) { c.Dr0 = addr; c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | ((s_rw ? 3ull : 1ull) << 16) | (3ull << 18); if (SetThreadContext(h, &c)) ++armed; }
    if (other) ResumeThread(h); CloseHandle(h);
  }
  CloseHandle(snap);
  log_line("[memwatch] armed DR0=%p (write, 4 bytes) on %d threads\r\n", (void*)addr, armed);
}
static void MemScanTick(UINT frame) {
  static long s_at = -2; static UINT s_pat[16]; static int s_np = 0; static long s_woff = -1;
  if (s_at == -2) {
    char b[256] = {}; s_at = GetEnvironmentVariableA("ONRUSH_MEMSCAN_AT", b, sizeof(b)) > 0 ? atol(b) : -1;
    if (GetEnvironmentVariableA("ONRUSH_MEMSCAN_PAT", b, sizeof(b)) > 0) { char* ctx = nullptr; for (char* t = strtok_s(b, ",", &ctx); t && s_np < 16; t = strtok_s(nullptr, ",", &ctx)) s_pat[s_np++] = (UINT)strtoul(t, nullptr, 16); }
    if (GetEnvironmentVariableA("ONRUSH_MEMWATCH_OFF", b, sizeof(b)) > 0) s_woff = atol(b);
  }
  // ONRUSH_MEMWATCH_REGSIZE=<hex size> + ONRUSH_MEMWATCH_REGOFF=<hex offset>: at frame ONRUSH_MEMSCAN_AT arm the watch on
  // (base of the first private region of exactly that size) + offset, without scanning.
  { static UINT64 s_rsz = ~0ull, s_roff = 0;
    if (s_rsz == ~0ull) { char b[64] = {}; s_rsz = GetEnvironmentVariableA("ONRUSH_MEMWATCH_REGSIZE", b, sizeof(b)) > 0 ? _strtoui64(b, nullptr, 16) : 0;
      if (GetEnvironmentVariableA("ONRUSH_MEMWATCH_REGOFF", b, sizeof(b)) > 0) s_roff = _strtoui64(b, nullptr, 16); }
    // re-armed every 100 frames up to frame 3000 so threads created later (loading jobs) are covered too
    if (s_rsz && s_at >= 0 && (long)frame >= s_at && frame <= 3000 && ((long)frame - s_at) % 100 == 0) {
      MEMORY_BASIC_INFORMATION mi; uint8_t* p = nullptr;
      while (VirtualQuery(p, &mi, sizeof(mi)) == sizeof(mi)) {
        if (mi.State == MEM_COMMIT && mi.RegionSize == s_rsz) { log_line("[memscan] region of size %llX at %p\r\n", (unsigned long long)s_rsz, mi.BaseAddress); MemWatchArm((uintptr_t)mi.BaseAddress + (uintptr_t)s_roff); return; }
        p = (uint8_t*)mi.BaseAddress + mi.RegionSize; if ((uintptr_t)p >= 0x7FFFFFFF0000ull) break;
      }
      log_line("[memscan] no region of size %llX at frame %u\r\n", (unsigned long long)s_rsz, frame);
      return;
    } }
  if (s_at < 0 || (long)frame != s_at || s_np == 0) return;
  MEMORY_BASIC_INFORMATION mi; uint8_t* p = nullptr; int found = 0; uintptr_t first = 0;
  while (VirtualQuery(p, &mi, sizeof(mi)) == sizeof(mi)) {
    uint8_t* base = (uint8_t*)mi.BaseAddress; SIZE_T sz = mi.RegionSize;
    bool rd = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_READONLY));
    if (rd && mi.Type != MEM_IMAGE) {
      __try {
        const UINT* q = (const UINT*)base; size_t n = sz / 4;
        for (size_t i = 0; i + s_np <= n; ++i) {
          if (q[i] != s_pat[0]) continue;
          int k = 1; while (k < s_np && q[i + k] == s_pat[k]) ++k;
          if (k == s_np && found < 40) {
            ++found; if (!first) first = (uintptr_t)(q + i);
            log_line("[memscan] match %p region=%p size=%llX prot=%lX type=%lX\r\n", (void*)(q + i), base, (unsigned long long)sz, mi.Protect, mi.Type);
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    p = base + sz; if ((uintptr_t)p >= 0x7FFFFFFF0000ull) break;
    if (first && s_woff >= 0) break;   // watch mode: the first match is enough
  }
  log_line("[memscan] frame %u: %d match(es)\r\n", frame, found);
  // ONRUSH_MEMSCAN_STRIDE=<bytes>: survey records of that stride around the first match (fields 4 and 7 != -1)
  { char b[16] = {}; int st = GetEnvironmentVariableA("ONRUSH_MEMSCAN_STRIDE", b, sizeof(b)) > 0 ? atoi(b) : 0;
    if (first && st > 0) {
      __try {
        int valid = 0, total = 0, shown = 0;
        for (long r = -4096; r < 65536; ++r) {
          const UINT* q = (const UINT*)(first + (intptr_t)r * st);
          MEMORY_BASIC_INFORMATION m2; if (VirtualQuery(q, &m2, sizeof(m2)) != sizeof(m2) || m2.State != MEM_COMMIT) continue;
          bool empty = true; for (int k = 0; k < st / 4; ++k) if (q[k]) empty = false;
          if (r >= -2 && r < 4) log_line("[memscan] near rec %ld: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\r\n", r, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10]);
          if (empty) continue;
          ++total;
          if (q[4] != 0xFFFFFFFFu || q[7] != 0xFFFFFFFFu) {
            ++valid;
            if (shown < 24) { ++shown; log_line("[memscan] rec %ld: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\r\n", r, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10]); }
          }
        }
        log_line("[memscan] stride %d: %d of %d records have field4/field7 != -1\r\n", st, valid, total);
      } __except (EXCEPTION_EXECUTE_HANDLER) { log_line("[memscan] survey faulted\r\n"); }
    } }
  if (first && s_woff >= 0) MemWatchArm(first + (uintptr_t)s_woff);
}
static DWORD WINAPI CpuSamplerThread(LPVOID) {
  std::unordered_map<DWORD, ULONGLONG> prev;
  for (int iter = 0; iter < 60; ++iter) {
    Sleep(2000);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) continue;
    THREADENTRY32 te = {}; te.dwSize = sizeof(te); DWORD bestTid = 0; ULONGLONG bestDelta = 0;
    if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE th = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID); if (!th) continue;
      FILETIME c, e, k, u; if (GetThreadTimes(th, &c, &e, &k, &u)) { ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime); ULONGLONG d = t - prev[te.th32ThreadID]; prev[te.th32ThreadID] = t; if (d > bestDelta) { bestDelta = d; bestTid = te.th32ThreadID; } }
      CloseHandle(th); } while (Thread32Next(snap, &te));
    CloseHandle(snap); if (!bestTid) continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, bestTid); if (!th) continue;
    if (SuspendThread(th) != (DWORD)-1) {
      CONTEXT uc = {}; uc.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(th, &uc)) {
        char bt[1600]; int o = _snprintf_s(bt, sizeof(bt), _TRUNCATE, "[smp] tid=%lu busy=%.0fms", bestTid, bestDelta / 10000.0); HMODULE hExe = GetModuleHandleW(nullptr);
        for (int fr = 0; fr < 24 && uc.Rip; ++fr) { DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(uc.Rip, &ib, nullptr); char nm[64] = "?"; if (ib == (DWORD64)hExe) strcpy_s(nm, "exe"); else { char full[MAX_PATH]; if (GetModuleFileNameA((HMODULE)ib, full, MAX_PATH)) { const char* s = strrchr(full, 92); strncpy_s(nm, s ? s + 1 : full, _TRUNCATE); } }
          o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " %s+%llX", nm, (unsigned long long)(uc.Rip - ib)); if (!rf) break; void* hd = nullptr; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, uc.Rip, rf, &uc, &hd, &ef, nullptr); }
        log_line("%s\r\n", bt); }
      ResumeThread(th); }
    CloseHandle(th); }
  return 0; }

// Hang watchdog: when no frame has been presented for 6 s, log the call stack of every thread once
// (exe-relative frames). ONRUSH_NO_HANG_DUMP=1 disables it.
static volatile ULONGLONG g_lastPresentTick = 0;
static void LogThreadStack(DWORD tid, const char *tag) {
  HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!th) return;
  if (SuspendThread(th) != (DWORD)-1) {
    CONTEXT uc = {}; uc.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &uc)) {
      char bt[1800]; int o = _snprintf_s(bt, sizeof(bt), _TRUNCATE, "[%s] tid=%lu", tag, tid);
      HMODULE hExe = GetModuleHandleW(nullptr);
      for (int fr = 0; fr < 28 && uc.Rip; ++fr) {
        DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(uc.Rip, &ib, nullptr); char nm[64] = "?";
        if (ib == (DWORD64)hExe) strcpy_s(nm, "exe");
        else if (ib) { char full[MAX_PATH]; if (GetModuleFileNameA((HMODULE)ib, full, MAX_PATH)) { const char *s = strrchr(full, 92); strncpy_s(nm, s ? s + 1 : full, _TRUNCATE); } }
        o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " %s+%llX", nm, (unsigned long long)(uc.Rip - ib));
        if (!rf) break;
        void *hd = nullptr; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, uc.Rip, rf, &uc, &hd, &ef, nullptr);
      }
      log_line("%s\r\n", bt);
    }
    ResumeThread(th);
  }
  CloseHandle(th);
}
static DWORD WINAPI HangWatchdogThread(LPVOID) {
  bool dumped = false;
  for (;;) {
    Sleep(1000);
    ULONGLONG last = g_lastPresentTick;
    if (!last) continue;
    ULONGLONG idle = GetTickCount64() - last;
    if (idle < 6000) { dumped = false; continue; }
    if (dumped) continue;
    dumped = true;
    log_line("[hang] no Present for %llu ms -> thread stacks:\r\n", idle);
    {
      // exe+0xB7E50 spins until [exe+0x1538B50]->GetCompletedValue() >= target ([exe+0x1538B00] + 1).
      uint8_t *hx = (uint8_t *)GetModuleHandleW(nullptr);
      ID3D12Fence *f = *(ID3D12Fence **)(hx + 0x1538B50);
      ID3D12Fence *f2 = *(ID3D12Fence **)(hx + 0x1538B48);
      unsigned long long tgt = *(unsigned long long *)(hx + 0x1538B00);
      log_line("[hang] frame fence %p completed=%llu (target %llu) | fence2 %p completed=%llu | device removed=0x%08lX\r\n",
               f, f ? (unsigned long long)f->GetCompletedValue() : 0ull, tgt + 1, f2,
               f2 ? (unsigned long long)f2->GetCompletedValue() : 0ull,
               g_pMainDevice ? (unsigned long)g_pMainDevice->GetDeviceRemovedReason() : 0ul);
      DumpQueueFenceHistory();
      DumpBreadcrumbs();
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) continue;
    THREADENTRY32 te = {}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) do {
      if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
        LogThreadStack(te.th32ThreadID, "hang");
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
  }
}

#include "photo_capture.h"
static ID3D12Resource* PhotoCurrentBackBuffer() {
  UINT idx = 0; IDXGISwapChain3* sc3 = nullptr;
  if (g_pMainSwapChain && SUCCEEDED(g_pMainSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) && sc3) { idx = sc3->GetCurrentBackBufferIndex(); sc3->Release(); }
  return idx < g_SwapChainBufferCount ? g_pSwapChainBuffers[idx] : nullptr;
}
EXTERN_C HRESULT WINAPI OnRushDXGIXPresentArray(
    UINT SyncInterval, UINT PresentImmediateThreshold, UINT Flags,
    UINT NumSwapChains, void **ppSwapChains,
    const DXGIX_PRESENTARRAY_PARAMETERS *pPresentParameters) {
  static UINT frame_count = 0;
  frame_count++;
  g_lastPresentTick = GetTickCount64();
  BcArgDumpTick((long)frame_count);
  g_rtLogFrame = (LONG)frame_count;
  DeferReleaseTick();
  MemScanTick(frame_count);
  { static bool s_wd = false; if (!s_wd) { s_wd = true; if (GetEnvironmentVariableA("ONRUSH_NO_HANG_DUMP", nullptr, 0) == 0) CloseHandle(CreateThread(nullptr, 0, HangWatchdogThread, nullptr, 0, nullptr)); } }
  Photo_OnPresent(PhotoCurrentBackBuffer());   // F12 photo export (photo_capture.h); must run before the counter reset
  { static const bool s_rst = GetEnvironmentVariableA("ONRUSH_NO_CTR_RESET", nullptr, 0) == 0; if (s_rst) ResetCountersOnQueue(); }
  { static const bool s_pd = GetEnvironmentVariableA("ONRUSH_DEBUG_POSDUMP", nullptr, 0) > 0 || GetEnvironmentVariableA("ONRUSH_DBG_REGION", nullptr, 0) > 0; if (s_pd && (frame_count % 600) == 300) DumpDbgRegion(frame_count); }

  // Pump all window messages on presentation thread so desktop window stays responsive
  if (g_desktopHwnd && IsWindow(g_desktopHwnd)) {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        log_line("[d3d12_x] Received WM_QUIT on presentation thread\r\n");
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  if (frame_count == 1 || frame_count == 15 || frame_count == 30 || frame_count == 60) {
    if (g_desktopHwnd && IsWindow(g_desktopHwnd)) {
      if (!NoFocusMode()) {
        ShowWindow(g_desktopHwnd, SW_SHOW);
        SetWindowPos(g_desktopHwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_desktopHwnd);
        BringWindowToTop(g_desktopHwnd);
      }
      UpdateWindow(g_desktopHwnd);

      RECT wrc = {}, crc = {};
      GetWindowRect(g_desktopHwnd, &wrc);
      GetClientRect(g_desktopHwnd, &crc);
      BOOL isVis = IsWindowVisible(g_desktopHwnd);
      BOOL isIconic = IsIconic(g_desktopHwnd);
      HWND fgHwnd = GetForegroundWindow();
      log_line("[d3d12_x] Window state: hwnd=%p, vis=%d, iconic=%d, fg=%p (isFg=%d), rect={%ld,%ld,%ld,%ld}, client={%ld,%ld,%ld,%ld}\r\n",
               g_desktopHwnd, isVis, isIconic, fgHwnd, (fgHwnd == g_desktopHwnd),
               wrc.left, wrc.top, wrc.right, wrc.bottom,
               crc.left, crc.top, crc.right, crc.bottom);
    }
  }

  {  // frame rate every 5 s, with the sync interval the game asks for (30 fps = 2, 60 fps = 1)
    static ULONGLONG s_fpsT0 = 0; static long s_fpsF0 = 0;
    ULONGLONG now = GetTickCount64();
    if (!s_fpsT0) { s_fpsT0 = now; s_fpsF0 = frame_count; }
    else if (now - s_fpsT0 >= 5000) {
      log_line("[fps] %.1f fps (game flip rate %u)\r\n", (frame_count - s_fpsF0) * 1000.0 / (double)(now - s_fpsT0), SyncInterval);
      s_fpsT0 = now; s_fpsF0 = frame_count;
    }
  }
  if (frame_count <= 10 || (frame_count % 60) == 0) {
    log_line("[d3d12_x] DXGIXPresentArray frame %u (sync=%u, thresh=%u, "
             "flags=0x%X, numSwapChains=%u, ppSwapChains=%p, pParams=%p, g_pMainSwapChain=%p)\r\n",
             frame_count, SyncInterval, PresentImmediateThreshold, Flags,
             NumSwapChains, ppSwapChains, pPresentParameters, g_pMainSwapChain);
    if (pPresentParameters) {
      log_line("  pPresentParams: Disable=%d, UsePrev=%d, SourceRect={%ld,%ld,%ld,%ld}, DestUpperLeft={%ld,%ld}, Scale=(%f,%f)\r\n",
               pPresentParameters->Disable, pPresentParameters->UsePreviousBuffer,
               pPresentParameters->SourceRect.left, pPresentParameters->SourceRect.top,
               pPresentParameters->SourceRect.right, pPresentParameters->SourceRect.bottom,
               pPresentParameters->DestRectUpperLeft.x, pPresentParameters->DestRectUpperLeft.y,
               pPresentParameters->ScaleFactorHorz, pPresentParameters->ScaleFactorVert);
    }
    if (ppSwapChains && NumSwapChains > 0) {
      for (UINT i = 0; i < NumSwapChains && i < 4; ++i) {
        void *sc = ppSwapChains[i];
        log_line("  swapChain[%u] = %p\r\n", i, sc);
        if (sc && (uintptr_t)sc > 0x10000) {
          __try {
            void **vtbl = *(void ***)sc;
            log_line("    vtbl = %p (slot0=%p, slot1=%p, slot8=%p)\r\n", vtbl,
                     vtbl[0], vtbl[1], vtbl[8]);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("    (inaccessible vtable)\r\n");
          }
        }
      }
    }
  }

  // 1. If we have captured the desktop swap chain, present it directly!
  if (g_pMainSwapChain) {
    if (g_pMainDevice) {
      HRESULT preReason = g_pMainDevice->GetDeviceRemovedReason();
      if (FAILED(preReason)) {
        log_line("  !!! CRITICAL: Device ALREADY REMOVED before Present! Reason=0x%08lX\r\n",
                 (unsigned long)preReason);
      }
    }

    // Blit from last rendered target to current swapchain backbuffer if needed
    ID3D12Resource *pSrc = g_pLastRenderedTarget;
    UINT backBufferIdx = 0;
    IDXGISwapChain3 *sc3 = nullptr;
    if (g_pMainSwapChain && SUCCEEDED(g_pMainSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc3)) && sc3) {
      backBufferIdx = sc3->GetCurrentBackBufferIndex();
      sc3->Release();
    }
    ID3D12Resource *pCurrentBackBuffer = (backBufferIdx < g_SwapChainBufferCount) ? g_pSwapChainBuffers[backBufferIdx] : nullptr;
    if (!pCurrentBackBuffer && g_pMainSwapChain) {
      g_pMainSwapChain->GetBuffer(backBufferIdx, __uuidof(ID3D12Resource), (void **)&pCurrentBackBuffer);
      if (pCurrentBackBuffer) {
        pCurrentBackBuffer->Release();
      }
    }

    if (frame_count <= 10 || (frame_count % 60) == 0) {
      log_line("[d3d12_x] DXGIXPresentArray frame %u: pSrc=%p, backBufferIdx=%u, pCurrentBackBuffer=%p, g_SwapChainBufferCount=%u\r\n",
               frame_count, pSrc, backBufferIdx, pCurrentBackBuffer, g_SwapChainBufferCount);
    }

    ID3D12Resource *pSourceToBlit = nullptr;
    // The game renders straight into the swap-chain buffer. Command lists are recorded on several
    // threads, so "last rendered target" is often a 1600x900 scene target, and copying it over the
    // backbuffer blanked the top-left 1600x900 of some frames (intro video flicker). The full-size
    // composited target (title/menus, an overlay plane on Xbox) still has to be copied, so only
    // blit sources with the backbuffer's size. ONRUSH_PRESENT_BLIT=1 restores the old any-size blit.
    static int s_presentBlit = -1;
    if (s_presentBlit < 0) s_presentBlit = InputEnvFlag("ONRUSH_PRESENT_BLIT") ? 1 : 0;
    bool blitSizeOk = s_presentBlit != 0;
    if (!blitSizeOk && g_pLastRenderedTarget && pCurrentBackBuffer) {
      D3D12_RESOURCE_DESC a = {}, b = {};
      blitSizeOk = SafeGetResourceDesc(g_pLastRenderedTarget, &a) && SafeGetResourceDesc(pCurrentBackBuffer, &b) &&
                   a.Width == b.Width && a.Height == b.Height;
    }
    if (blitSizeOk && g_pLastRenderedTarget && g_pLastRenderedTarget != pCurrentBackBuffer) {
      pSourceToBlit = g_pLastRenderedTarget;
    }

    if (pSourceToBlit && pCurrentBackBuffer && g_pDirectCommandQueue && InitBlitInfrastructure()) {
      D3D12_RESOURCE_DESC srcDesc = {};
      D3D12_RESOURCE_DESC dstDesc = {};
      if (SafeGetResourceDesc(pSourceToBlit, &srcDesc) && SafeGetResourceDesc(pCurrentBackBuffer, &dstDesc)) {
        if (frame_count <= 10 || (frame_count % 60) == 0) {
          log_line("[d3d12_x] Blit pass (frame %u): src=%p (%llux%u, fmt=%d, samples=%u) -> dstBuf[%u]=%p (%llux%u, fmt=%d)\r\n",
                   frame_count, pSourceToBlit, (unsigned long long)srcDesc.Width, srcDesc.Height, srcDesc.Format, srcDesc.SampleDesc.Count,
                   backBufferIdx, pCurrentBackBuffer, (unsigned long long)dstDesc.Width, dstDesc.Height, dstDesc.Format);
        }

        HRESULT rHr = S_OK, cHr = S_OK;
        ResetBlitListRobust(&rHr, &cHr);
        if (SUCCEEDED(rHr) && SUCCEEDED(cHr)) {
          D3D12_RESOURCE_STATES srcStateBefore = GetTrackedResourceState(pSourceToBlit, D3D12_RESOURCE_STATE_RENDER_TARGET);
          D3D12_RESOURCE_STATES dstStateBefore = GetTrackedResourceState(pCurrentBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);

          // Barriers before copy
          D3D12_RESOURCE_BARRIER preBarriers[2] = {};
          UINT barrierCount = 0;

          if (srcStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            preBarriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[barrierCount].Transition.pResource = pSourceToBlit;
            preBarriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preBarriers[barrierCount].Transition.StateBefore = srcStateBefore;
            preBarriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrierCount++;
          }

          if (dstStateBefore != D3D12_RESOURCE_STATE_COPY_DEST) {
            preBarriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[barrierCount].Transition.pResource = pCurrentBackBuffer;
            preBarriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preBarriers[barrierCount].Transition.StateBefore = dstStateBefore;
            preBarriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrierCount++;
          }

          if (barrierCount > 0) {
            g_pBlitCommandList->ResourceBarrier(barrierCount, preBarriers);
          }

          // Perform copy or resolve
          if (srcDesc.SampleDesc.Count > 1) {
            g_pBlitCommandList->ResolveSubresource(pCurrentBackBuffer, 0, pSourceToBlit, 0, dstDesc.Format);
          } else if (srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height) {
            g_pBlitCommandList->CopyResource(pCurrentBackBuffer, pSourceToBlit);
          } else {
            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = pCurrentBackBuffer;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = pSourceToBlit;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;

            D3D12_BOX srcBox = {};
            srcBox.left = 0;
            srcBox.top = 0;
            srcBox.right = (UINT)min(srcDesc.Width, dstDesc.Width);
            srcBox.bottom = min(srcDesc.Height, dstDesc.Height);
            srcBox.front = 0;
            srcBox.back = 1;

            if (pPresentParameters && pPresentParameters->SourceRect.right > pPresentParameters->SourceRect.left &&
                pPresentParameters->SourceRect.bottom > pPresentParameters->SourceRect.top) {
              srcBox.left = (UINT)max(0L, pPresentParameters->SourceRect.left);
              srcBox.top = (UINT)max(0L, pPresentParameters->SourceRect.top);
              srcBox.right = (UINT)min((LONG)srcDesc.Width, pPresentParameters->SourceRect.right);
              srcBox.bottom = (UINT)min((LONG)srcDesc.Height, pPresentParameters->SourceRect.bottom);
            }

            g_pBlitCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
          }

          // Barriers after copy
          D3D12_RESOURCE_BARRIER postBarriers[2] = {};
          UINT postBarrierCount = 0;

          if (srcStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            postBarriers[postBarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarriers[postBarrierCount].Transition.pResource = pSourceToBlit;
            postBarriers[postBarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            postBarriers[postBarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            postBarriers[postBarrierCount].Transition.StateAfter = srcStateBefore;
            postBarrierCount++;
          }

          postBarriers[postBarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          postBarriers[postBarrierCount].Transition.pResource = pCurrentBackBuffer;
          postBarriers[postBarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          postBarriers[postBarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
          postBarriers[postBarrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
          postBarrierCount++;

          g_pBlitCommandList->ResourceBarrier(postBarrierCount, postBarriers);

          TrackResourceState(pSourceToBlit, srcStateBefore);
          TrackResourceState(pCurrentBackBuffer, D3D12_RESOURCE_STATE_PRESENT);

          HRESULT clsHr = g_pBlitCommandList->Close();
          if (SUCCEEDED(clsHr)) {
            ID3D12CommandList *ppLists[] = { g_pBlitCommandList };
            g_pDirectCommandQueue->ExecuteCommandLists(1, ppLists);

            g_BlitFenceValue++;
            g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
            if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) {
              g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent);
              WaitForSingleObject(g_hBlitEvent, INFINITE);
            }
          } else {
            log_line("[d3d12_x] Failed to close blit command list hr=0x%08lX\r\n", (unsigned long)clsHr);
          }
        } else {
          log_line("[d3d12_x] ERROR: Blit Reset failed! rHr=0x%08lX, cHr=0x%08lX\r\n", (unsigned long)rHr, (unsigned long)cHr);
        }
      } else {
        log_line("[d3d12_x] ERROR: SafeGetResourceDesc failed for pSourceToBlit=%p or pDst=%p\r\n", pSourceToBlit, pCurrentBackBuffer);
      }
    }

    if (g_desktopHwnd && !NoFocusMode() && (frame_count == 1 || frame_count == 10 || frame_count == 60)) {
      ShowWindow(g_desktopHwnd, SW_RESTORE);
      SetForegroundWindow(g_desktopHwnd);
    }

    // ONRUSH_DUMP_RANGE=start:count[:step] -> also dump frames start..start+count-1 (every `step`-th; flicker checks)
    static long s_drStart = -2, s_drCount = 0, s_drStep = 1;
    if (s_drStart == -2) { char rb[48] = {}; s_drStart = -1;
      if (GetEnvironmentVariableA("ONRUSH_DUMP_RANGE", rb, sizeof(rb)) > 0) sscanf_s(rb, "%ld:%ld:%ld", &s_drStart, &s_drCount, &s_drStep);
      if (s_drStep < 1) s_drStep = 1; }
    const bool inDumpRange = s_drStart >= 0 && frame_count >= s_drStart && frame_count < s_drStart + s_drCount &&
                             (frame_count - s_drStart) % s_drStep == 0;
    { // ONRUSH_LOG_FRAME_DRAWS=1: one line per frame with the draw/dispatch counts of that frame
      static int s_fd = -1; static LONG s_prev[STAT_COUNT] = {};
      if (s_fd < 0) s_fd = GetEnvironmentVariableA("ONRUSH_LOG_FRAME_DRAWS", nullptr, 0) > 0 ? 1 : 0;
      if (s_fd) {
        LONG c[STAT_COUNT]; for (int i = 0; i < STAT_COUNT; ++i) c[i] = g_cmdTotal[i];
        log_line("[fd] f=%lu draw=%ld idx=%ld disp=%ld exind=%ld rt=%ld nort=%ld\r\n", (unsigned long)frame_count,
                 (long)(c[STAT_DRAW] - s_prev[STAT_DRAW]), (long)(c[STAT_DRAWIDX] - s_prev[STAT_DRAWIDX]), (long)(c[STAT_DISPATCH] - s_prev[STAT_DISPATCH]),
                 (long)(c[STAT_EXECIND] - s_prev[STAT_EXECIND]), (long)(c[STAT_DRAW_RT] - s_prev[STAT_DRAW_RT]), (long)(c[STAT_DRAW_NORT] - s_prev[STAT_DRAW_NORT]));
        for (int i = 0; i < STAT_COUNT; ++i) s_prev[i] = c[i];
      }
    }
    { // F9 = re-arm the ONRUSH_LOG_PSHASH log (descriptor tables of the next 12 draws of that PS)
      static int s_f9on = -1; static bool s_f9prev = false;
      if (s_f9on < 0) s_f9on = GetEnvironmentVariableA("ONRUSH_RT_KEY", nullptr, 0) > 0 ? 1 : 0;
      if (s_f9on) { bool dn = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (dn && !s_f9prev) { InterlockedExchange(&g_pshN, 0); InterlockedExchange(&g_pshForce, 1); log_line("[rtkey] F9: PSHASH log re-armed at frame %lu\r\n", (unsigned long)frame_count); }
        s_f9prev = dn; }
    }
    { // F10 = dump every render target >= 1280 px wide that was drawn into since the last stats window (debug)
      static int s_f10on = -1; static bool s_f10prev = false;
      if (s_f10on < 0) s_f10on = GetEnvironmentVariableA("ONRUSH_RT_KEY", nullptr, 0) > 0 ? 1 : 0;
      if (s_f10on) {
        bool dn = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (dn && !s_f10prev) {
          int nd = 0;
          for (int ri = 0; ri < 24; ++ri) {
            ID3D12Resource *rt = (ID3D12Resource *)g_rtDrawStats[ri].res; if (!rt) continue;
            D3D12_RESOURCE_DESC rd = {};
            if (!SafeGetResourceDesc(rt, &rd) || rd.Width < 1280) continue;
            char tg[24]; sprintf_s(tg, "rt%d_f%d", ri, (int)rd.Format);
            DumpBackBufferDiagnostic(rt, frame_count, tg); ++nd;
          }
          log_line("[rtkey] F10: dumped %d render targets at frame %lu\r\n", nd, (unsigned long)frame_count);
        }
        s_f10prev = dn;
      }
    }
    // F11 = burst capture: the next 12 frames (backbuffer only) go to frame_<n>_backbuffer.bmp, for reporting
    // flicker / lighting problems while playing normally (no debug option needed). ONRUSH_NO_BURST_KEY=1 disables.
    {
      static int s_burstLeft = 0; static bool s_f11prev = false;
      static int s_burstOff = -1; if (s_burstOff < 0) s_burstOff = InputEnvFlag("ONRUSH_NO_BURST_KEY") ? 1 : 0;
      if (!s_burstOff) {
        bool dn = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        static int s_dlArm = 0;
        if (g_drawList) { InterlockedExchange(&g_drawList, 0); }            // the frame that was being logged just ended
        if (s_dlArm) { InterlockedExchange(&g_drawList, 1); s_dlArm = 0; }  // log the draws of the next frame
        { static long s_dlFrame = -2; if (s_dlFrame == -2) { char e[32]; s_dlFrame = GetEnvironmentVariableA("ONRUSH_DL_FRAME", e, sizeof(e)) > 0 ? atol(e) : -1; }
          if (s_dlFrame > 0 && (long)frame_count == s_dlFrame) { s_dlArm = 1; log_line("[burst] auto dl at frame %lu\r\n", (unsigned long)frame_count); } }
        if (dn && !s_f11prev && s_burstLeft == 0) { s_dlArm = 1; s_burstLeft = 12; log_line("[burst] F11: capturing 12 frames from frame %lu\r\n", (unsigned long)frame_count); }
        s_f11prev = dn;
        if (s_burstLeft > 0) { --s_burstLeft; if (pCurrentBackBuffer) DumpBackBufferDiagnostic(pCurrentBackBuffer, frame_count, "backbuffer"); }
      }
    }
    // Periodic frame captures (frame_*.bmp, ~8 MB each, stall the GPU): only with the launcher's debug
    // option "Capturas de quadros" (ONRUSH_FRAME_DUMPS=1).
    static int s_frameDumps = -1;
    if (s_frameDumps < 0) s_frameDumps = InputEnvFlag("ONRUSH_FRAME_DUMPS") ? 1 : 0;
    if (inDumpRange || s_frameDumps && (frame_count == 10|| frame_count == 60 || frame_count == 125 || frame_count == 150 || frame_count == 245 || frame_count == 270 || (frame_count % 300 == 0))) {
      if (pCurrentBackBuffer) {
        DumpBackBufferDiagnostic(pCurrentBackBuffer, frame_count, "backbuffer");
      }
      if (pSrc && pSrc != pCurrentBackBuffer) {
        DumpBackBufferDiagnostic(pSrc, frame_count, "src_target");
      }
      if (g_pLastOffscreenRenderTarget && g_pLastOffscreenRenderTarget != pCurrentBackBuffer) {
        DumpBackBufferDiagnostic(g_pLastOffscreenRenderTarget, frame_count, "offscreen_target");
      }
    }

    PollAltEnterToggle();
    // Ensure active gamepad is always populated in PlatformInputXbox singleton
    HMODULE hExePresent = GetModuleHandleW(nullptr);
    if (hExePresent) {
      void **ppPlatformInput = (void **)((uint8_t *)hExePresent + 0x01543480);
      if (ppPlatformInput && *ppPlatformInput) {
        void **ppActiveGamepad = (void **)((uint8_t *)*ppPlatformInput + 0x48);
        if (*ppActiveGamepad == nullptr) {
          *ppActiveGamepad = (void *)(IGamepad *)&g_mockGamepad;
          ((IGamepad *)&g_mockGamepad)->AddRef();
          log_line("[input] Present: Bound g_mockGamepad to [PlatformInputXbox+0x48] (%p)\r\n", *ppActiveGamepad);
        }
      }

      // Input-driven skip of the boot/title flow. Only honoured until the
      // MainMenu transition (step 3) so gameplay input (A/D steering, A/B/X/Y,
      // Start) can never force phase transitions.
      static int s_enginePhaseStep = 0;
      // ONRUSH_NO_FORCE_PHASE=1: never drive phase transitions from Present;
      // let the engine's own main loop do it (diagnostic / long-term goal).
      bool advanceReq = !NoForcePhase() && (s_enginePhaseStep < 3) && IsAdvanceInputHeld();
      static void *s_lateNp = nullptr, *s_lateFixed = nullptr;
      static int s_origWorker = -1; if (s_origWorker < 0) s_origWorker = InputEnvFlag("ONRUSH_ORIG_PHASE_WORKER") ? 1 : 0;
      if (!s_origWorker && MaterializePendingPhase(hExePresent)) s_lateNp = *(void **)((uint8_t *)hExePresent + 0x0167E940);
      // A phase built late here (its construct job ran too early) can find its transition
      // request already dropped: EvalPhase ran with nextPhase==NULL and cleared [exe+0x167E949].
      // Once it is ready ([exe+0x167E8C8]==1) raise the request again, as SetNextPhase did.
      // Opt-in only (ONRUSH_LATE_PHASE_FIX=1): re-raising loops the transition phase 0x142BEC8 and
      // ends in _purecall (test 06/11).
      if (s_lateNp && s_lateNp != s_lateFixed && InputEnvFlag("ONRUSH_LATE_PHASE_FIX")) {
        uint8_t *pF1 = (uint8_t *)hExePresent + 0x0167E949;
        void *npNow = *(void **)((uint8_t *)hExePresent + 0x0167E940);
        if (npNow != s_lateNp) s_lateNp = nullptr;
        else if (*((uint8_t *)hExePresent + 0x0167E8C8) && !*pF1) {
          *pF1 = 1; s_lateFixed = s_lateNp;
          log_line("[phase] late-built next phase %p is ready but its request was dropped -> re-raised f1\r\n", s_lateNp);
        }
      }
      // ONRUSH_FORCE_PHASE_READY=1: if a materialized next phase stays "not ready" ([exe+0x167E8C8]==0)
      // for 120 frames (its completion job exe+0x21BA00 never ran), run that completion once.
      { static int s_forceReadyI = -1;
        if (s_forceReadyI < 0) s_forceReadyI = InputEnvFlag("ONRUSH_FORCE_PHASE_READY") ? 1 : 0;
        const bool s_forceReady = s_forceReadyI == 1;
        static void *s_waitPhase = nullptr; static int s_waitFrames = 0; static void *s_doneFor = nullptr;
        void *np = *(void **)((uint8_t *)hExePresent + 0x0167E940);
        uint8_t ready1 = *(uint8_t *)((uint8_t *)hExePresent + 0x0167E8C8);
        if (s_forceReady && np && !ready1 && np != s_doneFor) {
          if (np != s_waitPhase) { s_waitPhase = np; s_waitFrames = 0; }
          if (++s_waitFrames >= 120) {
            log_line("[phase] FORCE_PHASE_READY: next phase %p not ready after 120 frames -> calling exe+0x21BA00\r\n", np);
            ((void (*)())((uint8_t *)hExePresent + 0x0021BA00))();
            // SetNextPhase (exe+0x21CC68) had raised the transition request flag; it gets cleared
            // while the completion job is missing, so raise it again for the engine's EvalPhase.
            *(uint8_t *)((uint8_t *)hExePresent + 0x0167E949) = 1;
            s_doneFor = np;
          }
        } }

      static long s_detF = -2; if (s_detF == -2) { char e[32]; s_detF = GetEnvironmentVariableA("ONRUSH_DETAIL_FRAME", e, sizeof(e)) > 0 ? atol(e) : -1; }   // ONRUSH_DETAIL_FRAME=n: the detail/pixel-query frame (default PF(400))
      const UINT detF = s_detF > 0 ? (UINT)s_detF : PF(400);
      if (frame_count == detF) { g_drawDetailCount = 0; g_drawDetail = 1; g_pfnLogRootCbv = LogRootCbvImpl; g_pfnLogGeom = LogGeomImpl; } else if (frame_count == detF + 1) g_drawDetail = 0;
      // One-shot content probes below read resources the bridge remembered earlier; when a frame
      // number lands on a level load those resources may already be freed (GetDesc on a freed
      // texture faulted inside D3D12Core holding a lock -> game froze on the race loading screen).
      // They are debugging aids only: ONRUSH_DIAG_PROBES=1 turns them on.
      static int s_diagProbesI = -1;   // (no dynamic static init: this function uses __try)
      if (s_diagProbesI < 0) s_diagProbesI = InputEnvFlag("ONRUSH_DIAG_PROBES") ? 1 : 0;
      const bool s_diagProbes = s_diagProbesI == 1;
      { static long s_tvFrame = -2; if (s_tvFrame == -2) { char tb[16] = {}; s_tvFrame = GetEnvironmentVariableA("ONRUSH_TEXVAL_FRAME", tb, sizeof(tb)) > 0 ? atol(tb) : -1; }
        if (s_tvFrame > 0 && (long)frame_count == s_tvFrame) TexValReport(); }
      if (s_diagProbes && (frame_count == 440 || frame_count == 2400)) TexStatsReport();
      if (s_diagProbes && (frame_count == 440 || frame_count == 1200)) DumpMipList();
      if (frame_count == 20 && InputEnvFlag("ONRUSH_SAMPLER")) CreateThread(nullptr, 0, CpuSamplerThread, nullptr, 0, nullptr);
      if (frame_count == detF - 5) { PsQuerySetup(); TsSetup(); }
      if (frame_count == detF + 10) { PsQueryReport(); TsReport(); }
      if (frame_count == PF(412)) if (InputEnvFlag("ONRUSH_GPU_PROBES")) GpuCbvProbe(); if (frame_count == PF(412)) if (InputEnvFlag("ONRUSH_GPU_PROBES")) GpuGeoProbe(); if (frame_count == PF(413)) if (InputEnvFlag("ONRUSH_GPU_PROBES")) GpuScanPlaced(); if (frame_count == PF(414)) if (InputEnvFlag("ONRUSH_GPU_PROBES")) GpuXProbe(); if (frame_count == PF(415)) if (InputEnvFlag("ONRUSH_GPU_PROBES")) GpuRtProbe();
      if (s_diagProbes && frame_count == 420 && g_bigCopySrc) {
        D3D12_RESOURCE_DESC bd_ = {}; SafeGetResourceDesc(g_bigCopySrc, &bd_); D3D12_HEAP_PROPERTIES hp_ = {}; D3D12_HEAP_FLAGS hf_ = {}; HRESULT hh_ = g_bigCopySrc->GetHeapProperties(&hp_, &hf_);
        void* mp_ = nullptr; D3D12_RANGE rr_ = { 0, 0 }; HRESULT hm_ = g_bigCopySrc->Map(0, &rr_, &mp_); size_t nz_ = 0, chk_ = (size_t)(bd_.Width < (32ull << 20) ? bd_.Width : (32ull << 20));
        if (SUCCEEDED(hm_) && mp_) { __try { for (size_t q = 0; q < chk_; q += 4) if (((const uint32_t*)mp_)[q / 4]) ++nz_; } __except (EXCEPTION_EXECUTE_HANDLER) {} D3D12_RANGE wr_ = { 0, 0 }; g_bigCopySrc->Unmap(0, &wr_); }
        log_line("[bigsrc] res=%p width=%llu heapType=%d (hr=%08lX) map=%08lX ptr=%p nonzeroDwords=%zu/%zu\r\n", (void*)g_bigCopySrc, (unsigned long long)bd_.Width, (int)hp_.Type, (unsigned long)hh_, (unsigned long)hm_, mp_, nz_, chk_ / 4);
      }
      if (s_diagProbes && frame_count == 420) {
        LONG n = g_placedBufCount; if (n > 20000) n = 20000;
        LONG withData = 0, allZero = 0, bad = 0, uploads = 0;
        UINT64 bytesData = 0;
        for (LONG i = 0; i < n; ++i) {
          PlacedBufInfo &pb = g_placedBufs[i];
          if (pb.upload) { ++uploads; continue; }
          __try {
            size_t chk = (size_t)(pb.size < (4u << 20) ? pb.size : (4u << 20)); size_t nzc = 0;
            const uint8_t *c = (const uint8_t *)pb.cpu;
            for (size_t k2 = 0; k2 < chk; ++k2) if (c[k2]) ++nzc;
            if (nzc) { ++withData; bytesData += pb.size; if (withData <= 30) log_line("[placed] CPU-side data in #%ld res=%p cpu=%p size=%llu nonzeroBytes=%zu\r\n", (long)i, (void*)pb.res, pb.cpu, (unsigned long long)pb.size, nzc); } else ++allZero;
          } __except (EXCEPTION_EXECUTE_HANDLER) { ++bad; }
        }
        log_line("[placed] buffers=%ld upload=%ld cpuMemHasData=%ld (%llu bytes) cpuMemZero=%ld unreadable=%ld\r\n",
                 (long)n, (long)uploads, (long)withData, (unsigned long long)bytesData, (long)allZero, (long)bad);
        for (LONG i = 0; i < n; ++i) {
          PlacedBufInfo &pb = g_placedBufs[i];
          if (!pb.upload) continue;
          void *mp = nullptr; D3D12_RANGE rr = { 0, 0 };
          if (SUCCEEDED(pb.res->Map(0, &rr, &mp)) && mp) {
            size_t chk = (size_t)(pb.size < 65536 ? pb.size : 65536); size_t nzb = 0; const uint8_t *c = (const uint8_t *)mp;
            for (size_t k2 = 0; k2 < chk; ++k2) if (c[k2]) ++nzb;
            log_line("[placed] UPLOAD #%ld res=%p size=%llu nonzero=%llu/%llu first16=%08X %08X %08X %08X\r\n", (long)i, (void *)pb.res,
                     (unsigned long long)pb.size, (unsigned long long)nzb, (unsigned long long)chk,
                     ((uint32_t *)mp)[0], ((uint32_t *)mp)[1], ((uint32_t *)mp)[2], ((uint32_t *)mp)[3]);
            D3D12_RANGE wr = { 0, 0 }; pb.res->Unmap(0, &wr);
          }
        }
        for (LONG i = 0; i < n && i < 12; ++i)
          log_line("[placed]   #%ld res=%p cpu=%p size=%llu va=%llX\r\n", (long)i, (void *)g_placedBufs[i].res, g_placedBufs[i].cpu,
                   (unsigned long long)g_placedBufs[i].size, (unsigned long long)g_placedBufs[i].va);
      }
      if ((frame_count % 240) == 0) {
        log_line("[stats frame %ld] draw=%ld drawIdx=%ld dispatch=%ld clearRTV=%ld copyTex=%ld execInd=%ld setPSO=%ld\r\n",
                 (long)frame_count, InterlockedExchange(&g_cmdStats[STAT_DRAW], 0), InterlockedExchange(&g_cmdStats[STAT_DRAWIDX], 0),
                 InterlockedExchange(&g_cmdStats[STAT_DISPATCH], 0), InterlockedExchange(&g_cmdStats[STAT_CLEARRTV], 0),
                 InterlockedExchange(&g_cmdStats[STAT_COPYTEX], 0), InterlockedExchange(&g_cmdStats[STAT_EXECIND], 0),
                 InterlockedExchange(&g_cmdStats[STAT_PSO], 0));
        {
          // UI selector stacks (exe+0x1651DB0: 7 selectors; page stack count at +0x600, entries stride 0x18)
          __try {
            void **sel = (void **)((uint8_t *)hExePresent + 0x01651DB0);
            for (int si = 0; si < 7; ++si) {
              uint8_t *sp = (uint8_t *)sel[si];
              if (!sp) continue;
              uint32_t cnt = *(uint32_t *)(sp + 0x600);
              char line[400]; int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "[ui] selector %d cnt=%u:", si, cnt);
              for (uint32_t k = 0; k < cnt && k < 6; ++k) {
                uint8_t *pg = *(uint8_t **)(sp + k * 0x18);
                o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " [pg=%p hash=%llX st=%08X %08X]", pg,
                                 pg ? (unsigned long long)*(uint64_t *)(pg + 0x278) : 0ULL,
                                 *(uint32_t *)(sp + k * 0x18 + 8), *(uint32_t *)(sp + k * 0x18 + 12));
              }
              log_line("%s\r\n", line);
            }
          } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        {
          char sb[900]; int so = _snprintf_s(sb, sizeof(sb), _TRUNCATE, "[stubs] unmapped/stub slot hits since start:");
          for (int si = 0; si < kDurangoCmdListSlots; ++si)
            if (g_CmdListStubHits[si] > 0) so += _snprintf_s(sb + so, sizeof(sb) - so, _TRUNCATE, " %d(0x%X)=%ld", si, si * 8, (long)g_CmdListStubHits[si]);
          log_line("%s\r\n", sb);
        }
        { // ONRUSH_BC_TESTDUMP_FRAME=N (multiple of 240): run the breadcrumb dump once on a healthy frame (checks the dump itself)
          static long s_bcTest = -2;
          if (s_bcTest == -2) { char fb[16] = {}; s_bcTest = GetEnvironmentVariableA("ONRUSH_BC_TESTDUMP_FRAME", fb, sizeof(fb)) > 0 ? atol(fb) : -1; }
          if (frame_count == s_bcTest) { DWORD t0 = GetTickCount(); DumpBreadcrumbs(); log_line("[bc] test dump took %lu ms\r\n", GetTickCount() - t0); }
        }
        log_line("[stats] drawsWithRT=%ld drawsNoRT=%ld lastRT=%p backBuf=%p srcToBlit=%p\r\n",
                 InterlockedExchange(&g_cmdStats[STAT_DRAW_RT], 0), InterlockedExchange(&g_cmdStats[STAT_DRAW_NORT], 0),
                 (void *)g_pLastRenderedTarget, (void *)pCurrentBackBuffer, (void *)pSourceToBlit);
        for (int ri = 0; ri < 24; ++ri) {
          // take the entry (and the reference CountRtDraw holds) out of the table
          ID3D12Resource *rt = (ID3D12Resource *)InterlockedExchangePointer((PVOID *)&g_rtDrawStats[ri].res, nullptr);
          if (!rt) continue;
          D3D12_RESOURCE_DESC rd = {};
          bool okd = SafeGetResourceDesc(rt, &rd);
          static long s_rtDumpAt = -2;  // ONRUSH_RTDUMP_FRAME=N (multiple of 240) -> also dump every RT at frame N
          if (s_rtDumpAt == -2) { char fb[16] = {}; s_rtDumpAt = GetEnvironmentVariableA("ONRUSH_RTDUMP_FRAME", fb, sizeof(fb)) > 0 ? atol(fb) : -1; }
          if ((frame_count == s_rtDumpAt || (s_diagProbes && (frame_count == 960 || frame_count == 2400))) && okd) { char tg[16]; sprintf_s(tg, "rt%d", ri); DumpBackBufferDiagnostic(rt, frame_count, tg); }
          log_line("[stats]   RT %p %llux%u fmt=%d draws=%ld%s\r\n", (void *)rt,
                   okd ? (unsigned long long)rd.Width : 0ULL, okd ? rd.Height : 0, okd ? (int)rd.Format : 0,
                   InterlockedExchange(&g_rtDrawStats[ri].draws, 0), (rt == pCurrentBackBuffer) ? " (BACKBUFFER)" : "");
          rt->Release();
        }
      }
      void *curPhase = *(void **)((uint8_t *)hExePresent + 0x0167E930);
      void *nextPhase = *(void **)((uint8_t *)hExePresent + 0x0167E940);
      uint8_t flag0 = *(uint8_t *)((uint8_t *)hExePresent + 0x0167E948);
      uint8_t flag1 = *(uint8_t *)((uint8_t *)hExePresent + 0x0167E949);
      uint8_t phaseReady0 = *(uint8_t *)((uint8_t *)hExePresent + 0x0167E8C7);
      uint8_t phaseReady1 = *(uint8_t *)((uint8_t *)hExePresent + 0x0167E8C8);
      uint32_t cond = *(uint32_t *)((uint8_t *)hExePresent + 0x0167E914);

      {
        static bool s_bootLogged = false;
        if (!s_bootLogged && g_bootThreadDone) {
          s_bootLogged = true;
          log_line("[boot] boot thread done seen at frame %ld (curPhase=%p) audio_language='%.15s'\r\n", frame_count, curPhase,
                   (const char *)hExePresent + 0x01675B50);
        }
      }
      if (frame_count <= 10 || (frame_count % 60) == 0) {
        void *curVtbl = curPhase ? *(void **)curPhase : nullptr;
        void *nextVtbl = nextPhase ? *(void **)nextPhase : nullptr;
        uintptr_t curRva = curVtbl ? (uintptr_t)((uint8_t *)curVtbl - (uint8_t *)hExePresent) : 0;
        uintptr_t nextRva = nextVtbl ? (uintptr_t)((uint8_t *)nextVtbl - (uint8_t *)hExePresent) : 0;
        static ULONGLONG s_pdT0 = GetTickCount64();
        log_line("[PhaseDiag frame %ld t=%.1fs] curPhase=%p (vtbl_rva=0x%llX), nextPhase=%p (vtbl_rva=0x%llX), f0=%u, f1=%u, r0=%u, r1=%u, cond=%u\r\n",
                 frame_count, (GetTickCount64() - s_pdT0) / 1000.0, curPhase, (unsigned long long)curRva, nextPhase, (unsigned long long)nextRva, flag0, flag1, phaseReady0, phaseReady1, cond);
      }

      void *curVtblNow = curPhase ? *(void **)curPhase : nullptr;
      uintptr_t curRvaNow = curVtblNow ? (uintptr_t)((uint8_t *)curVtblNow - (uint8_t *)hExePresent) : 0;

      const int stepBefore = s_enginePhaseStep;  // restored if a transition is blocked
      // Step 0: Boot (LoadingPhase) -> Request IntroVideoPhase at frame 60
      if (s_enginePhaseStep == 0 && frame_count >= 60 && curPhase == nullptr) {
        log_line("[phase] Pacing: Requesting transition to IntroVideoPhase (frame %ld)...\r\n", frame_count);
        advanceReq = true;
        s_enginePhaseStep = 1;
      }
      // Step 1: In IntroVideoPhase (0x0142C090) -> Play intro videos!
      // The engine leaves the intro by itself when the video ends; we only force it on skip
      // input, or as a safety net after a long time (the intro video is ~76 s).
      else if (s_enginePhaseStep == 1 && curRvaNow == 0x0142C090) {
        bool skipRequested = IsAdvanceInputHeld() ||
                             (GameWindowFocused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) ||
                             (frame_count >= 9000);
        if (skipRequested) {
          log_line("[phase] Pacing: Requesting transition to StartScreenPhase (frame %ld, skip=%d, nextPhase=%p)...\r\n",
                   frame_count, skipRequested, nextPhase);
          advanceReq = true;
          s_enginePhaseStep = 2;
        }
      }
      // Step 2: In StartScreenPhase (0x0142C2F0) -> 'Press Any Button' title screen
      // Transition to MainMenuPhase on user button press OR after 500 frames
      else if (s_enginePhaseStep == 2 && curRvaNow == 0x0142C2F0) {
        bool startRequested = IsAdvanceInputHeld() || (frame_count >= 500);
        if (startRequested || nextPhase != nullptr) {
          log_line("[phase] Pacing: Requesting transition to MainMenuPhase (frame %ld, start=%d, nextPhase=%p)...\r\n",
                   frame_count, startRequested, nextPhase);
          advanceReq = true;
          s_enginePhaseStep = 3;
        }
      }

      if (NoForcePhase()) advanceReq = false;
      if (advanceReq) {
        if (!nextPhase) {
          struct PhaseConfigDesc {
            void *vtable;
            uint64_t flags;
          } desc;
          uintptr_t targetVtblRva = 0x0142C0F0; // IntroVideoPhaseConfig by default
          if (curPhase) {
            void *curVtbl = *(void **)curPhase;
            uintptr_t curVtblRva = curVtbl ? (uintptr_t)((uint8_t *)curVtbl - (uint8_t *)hExePresent) : 0;
            if (curVtblRva == 0x0142C090) {
              targetVtblRva = 0x0142C350; // StartScreenPhaseConfig
            } else if (curVtblRva == 0x0142C2F0) {
              targetVtblRva = 0x0141BA40; // MainMenuPhaseConfig
            }
          }
          desc.vtable = (void *)((uint8_t *)hExePresent + targetVtblRva);
          desc.flags = 1;
          typedef void (*SetNextPhaseFn)(void *pDesc, int mode);
          SetNextPhaseFn SetNextPhase = (SetNextPhaseFn)((uint8_t *)hExePresent + 0x0021CB80);
          SetNextPhase(&desc, 0);
          MaterializePendingPhase(hExePresent);
          nextPhase = *(void **)((uint8_t *)hExePresent + 0x0167E940);
          log_line("[input] Present (frame %ld): Called SetNextPhase(targetVtblRva=0x%08llX) -> nextPhase=%p\r\n",
                   frame_count, (unsigned long long)targetVtblRva, nextPhase);
        }

        if (nextPhase != nullptr && !PhaseTransitionSafe(hExePresent, curPhase)) {
          s_enginePhaseStep = stepBefore;  // retry on a later frame
        } else if (nextPhase != nullptr) {
          uint8_t *pFlag0 = (uint8_t *)hExePresent + 0x0167E948;
          uint8_t *pFlag1 = (uint8_t *)hExePresent + 0x0167E949;
          uint8_t *pPhaseReady1 = (uint8_t *)hExePresent + 0x0167E8C8;   // (0x167E8C7 is a pointer byte, never write it)
          *pFlag0 = 1;
          *pFlag1 = 1;
          *pPhaseReady1 = 1;
          log_line("[input] Present (frame %ld): Armed flags for transition to nextPhase=%p\r\n", frame_count, nextPhase);

          log_line("[phase] Calling EvaluatePhaseTransition (0x1402116C0) directly (frame %ld)...\r\n", frame_count);
          typedef void (*EvalPhaseFn)();
          EvalPhaseFn EvalPhase = (EvalPhaseFn)((uint8_t *)hExePresent + 0x0021D1D0);
          EvalPhase();
          curPhase = *(void **)((uint8_t *)hExePresent + 0x0167E930);
          nextPhase = *(void **)((uint8_t *)hExePresent + 0x0167E940);
          void *curVtbl = curPhase ? *(void **)curPhase : nullptr;
          uintptr_t curRva = curVtbl ? (uintptr_t)((uint8_t *)curVtbl - (uint8_t *)hExePresent) : 0;
          log_line("[phase] EvaluatePhaseTransition completed: curPhase=%p (vtbl_rva=0x%llX), nextPhase=%p\r\n",
                   curPhase, (unsigned long long)curRva, nextPhase);
        }
      }

      // Check if engine itself requested a transition
      if (!NoForcePhase() && nextPhase != nullptr && (flag0 != 0 || flag1 != 0) && PhaseTransitionSafe(hExePresent, curPhase)) {
        log_line("[phase] Engine transition pending (frame %ld, f0=%u, f1=%u): calling EvaluatePhaseTransition...\r\n", frame_count, flag0, flag1);
        typedef void (*EvalPhaseFn)();
        EvalPhaseFn EvalPhase = (EvalPhaseFn)((uint8_t *)hExePresent + 0x0021D1D0);
        EvalPhase();
        curPhase = *(void **)((uint8_t *)hExePresent + 0x0167E930);
        nextPhase = *(void **)((uint8_t *)hExePresent + 0x0167E940);
        void *curVtbl = curPhase ? *(void **)curPhase : nullptr;
        uintptr_t curRva = curVtbl ? (uintptr_t)((uint8_t *)curVtbl - (uint8_t *)hExePresent) : 0;
        log_line("[phase] Engine transition completed: curPhase=%p (vtbl_rva=0x%llX), nextPhase=%p\r\n",
                 curPhase, (unsigned long long)curRva, nextPhase);
      }
    }

    // The game asks for FlipRate vblanks (2 = 30 Hz on a 60 Hz TV); pacing follows the in-game
    // "Frame rate limit" / "Vertical sync" rows instead (gfx_options.inc).
    UINT sync = gfxopt::PresentSync(g_desktopHwnd, &Flags);
    // The game renders straight into the swap-chain buffer (Durango has no states): make sure the
    // buffer is really in PRESENT state when it is presented.
    if (pCurrentBackBuffer && g_pDirectCommandQueue && InitBlitInfrastructure()) {
      D3D12_RESOURCE_STATES bbState = GetTrackedResourceState(pCurrentBackBuffer, D3D12_RESOURCE_STATE_PRESENT);
      if (bbState != D3D12_RESOURCE_STATE_PRESENT) {
        HRESULT pr = S_OK, pc = S_OK;
        if (ResetBlitListRobust(&pr, &pc)) {
          D3D12_RESOURCE_BARRIER pb = {};
          pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          pb.Transition.pResource = pCurrentBackBuffer;
          pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          pb.Transition.StateBefore = bbState;
          pb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
          g_pBlitCommandList->ResourceBarrier(1, &pb);
          if (SUCCEEDED(g_pBlitCommandList->Close())) {
            ID3D12CommandList *pls[] = { g_pBlitCommandList };
            g_pDirectCommandQueue->ExecuteCommandLists(1, pls);
            g_BlitFenceValue++;
            g_pDirectCommandQueue->Signal(g_pBlitFence, g_BlitFenceValue);
            if (g_pBlitFence->GetCompletedValue() < g_BlitFenceValue) {
              g_pBlitFence->SetEventOnCompletion(g_BlitFenceValue, g_hBlitEvent);
              WaitForSingleObject(g_hBlitEvent, INFINITE);
            }
            TrackResourceState(pCurrentBackBuffer, D3D12_RESOURCE_STATE_PRESENT);
          }
        }
      }
    }
    HRESULT hr = g_pMainSwapChain->Present(sync, Flags);
    if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) {
      if (g_pMainDevice) {
        HRESULT postReason = g_pMainDevice->GetDeviceRemovedReason();
        log_line("  -> Present(sync=%u, flags=0x%X) failed hr=0x%08lX, DeviceRemovedReason=0x%08lX\r\n",
                 sync, Flags, (unsigned long)hr, (unsigned long)postReason);
      }
      // Fallback present with default parameters if custom sync/flags failed
      hr = g_pMainSwapChain->Present(1, 0);
    }
    if (frame_count <= 10 || (frame_count % 60) == 0) {
      log_line("  -> g_pMainSwapChain->Present(sync=%u, flags=0x%X) returned 0x%08lX\r\n",
               sync, Flags, (unsigned long)hr);
    }
    g_pLastRenderedTarget = nullptr;
    g_pLastOffscreenRenderTarget = nullptr;
    return hr;
  }

  // Try standard IDXGISwapChain::Present if valid
  if (ppSwapChains && NumSwapChains > 0) {
    for (UINT i = 0; i < NumSwapChains; ++i) {
      void *sc = ppSwapChains[i];
      if (sc && (uintptr_t)sc > 0x10000) {
        __try {
          IDXGISwapChain *pSwapChain = (IDXGISwapChain *)sc;
          pSwapChain->Present(SyncInterval, Flags);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          // Not a standard IDXGISwapChain, ignore
        }
      }
    }
  }

  return S_OK;
}

EXTERN_C HRESULT WINAPI OnRushD3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob **ppBlob,
    ID3DBlob **ppErrorBlob) {
  HRESULT hr =
      D3D12SerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
  log_line("[d3d12_x] D3D12SerializeRootSignature(version=%d) -> 0x%08lX\r\n",
           (int)Version, (unsigned long)hr);
  return hr;
}

EXTERN_C HRESULT WINAPI OnRushD3DMapEsramMemory(UINT Flags,
                                                void *pVirtualAddress,
                                                UINT NumPages,
                                                const UINT *pPageList) {
  DWORD allocation_type = 0;
  if (Flags & 1)
    allocation_type = MEM_LARGE_PAGES;
  else if (Flags & 2)
    allocation_type = MEM_4MB_PAGES;
  HRESULT hr =
      MapTitleEsramPages(pVirtualAddress, NumPages, allocation_type, pPageList);
  log_line(
      "[d3d12_x] D3DMapEsramMemory(flags=0x%X, va=%p, pages=%u) -> 0x%08lX\r\n",
      Flags, pVirtualAddress, NumPages, (unsigned long)hr);
  return hr;
}

// ============================================================================
// CRT Diagnostic Hooks & Vectored Crash Handler
// ============================================================================

static void(__cdecl *g_TrueAbort)(void) = nullptr;
static void __cdecl HookedAbort(void) {
  log_line("\r\n[CRASH_DIAG] ucrtbase.dll!abort() called from %p!\r\n",
           _ReturnAddress());
  if (g_TrueAbort)
    g_TrueAbort();
  ExitProcess(0xC0000409);
}

static void(__cdecl *g_TrueTerminate)(void) = nullptr;
static void __cdecl HookedTerminate(void) {
  log_line("\r\n[CRASH_DIAG] ucrtbase.dll!terminate() called from %p!\r\n",
           _ReturnAddress());
  if (g_TrueTerminate)
    g_TrueTerminate();
  ExitProcess(0xC0000409);
}

static void(__cdecl *g_TrueStdTerminate)(void) = nullptr;
static void __cdecl HookedStdTerminate(void) {
  log_line(
      "\r\n[CRASH_DIAG] VCRUNTIME140.dll!__std_terminate() called from %p!\r\n",
      _ReturnAddress());
  if (g_TrueStdTerminate)
    g_TrueStdTerminate();
  ExitProcess(0xC0000409);
}

static void(__cdecl *g_TrueInvalidParamNoInfo)(void) = nullptr;
static void __cdecl HookedInvalidParamNoInfo(void) {
  log_line("\r\n[CRASH_DIAG] ucrtbase.dll!_invalid_parameter_noinfo() called "
           "from %p!\r\n",
           _ReturnAddress());
  if (g_TrueInvalidParamNoInfo)
    g_TrueInvalidParamNoInfo();
  ExitProcess(0xC0000409);
}

static void(__cdecl *g_TrueInvalidParamNoInfoNoReturn)(void) = nullptr;
static void __cdecl HookedInvalidParamNoInfoNoReturn(void) {
  log_line("\r\n[CRASH_DIAG] ucrtbase.dll!_invalid_parameter_noinfo_noreturn() "
           "called from %p!\r\n",
           _ReturnAddress());
  if (g_TrueInvalidParamNoInfoNoReturn)
    g_TrueInvalidParamNoInfoNoReturn();
  ExitProcess(0xC0000409);
}

static int(__cdecl *g_TruePurecall)(void) = nullptr;
static int __cdecl HookedPurecall(void) {
  log_line("\r\n[CRASH_DIAG] VCRUNTIME140.dll!_purecall() called from %p!\r\n",
           _ReturnAddress());
  if (g_TruePurecall)
    return g_TruePurecall();
  ExitProcess(0xC0000409);
  return 0;
}

static void(WINAPI *g_TrueCxxThrowException)(void *pExceptionObject,
                                             void *pThrowInfo) = nullptr;
static void WINAPI HookedCxxThrowException(void *pExceptionObject,
                                           void *pThrowInfo) {
  log_line("[CRASH_DIAG] _CxxThrowException called from %p! (obj=%p, "
           "throwInfo=%p)\r\n",
           _ReturnAddress(), pExceptionObject, pThrowInfo);
  if (g_TrueCxxThrowException)
    g_TrueCxxThrowException(pExceptionObject, pThrowInfo);
}

#include "tracer.h"

static LONG WINAPI VectoredCrashHandler(PEXCEPTION_POINTERS pExInfo) {
  if (!pExInfo || !pExInfo->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  if (HandleTraceException(pExInfo)) return EXCEPTION_CONTINUE_EXECUTION;
  DWORD code = pExInfo->ExceptionRecord->ExceptionCode;

  if (code == 0xE06D7363) // MSVC C++ Exception
  {
    return EXCEPTION_CONTINUE_SEARCH;
  } else if (code == 0xC0000409) {
    ULONG_PTR fastFail = pExInfo->ExceptionRecord->NumberParameters > 0
                             ? pExInfo->ExceptionRecord->ExceptionInformation[0]
                             : 0;
    log_line("\r\n[VECTORED] FastFail (0x%llX) at %p\r\n",
             (unsigned long long)fastFail,
             pExInfo->ExceptionRecord->ExceptionAddress);
  } else if (code == 0xC0000005 && pExInfo->ContextRecord &&
             (((uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress ==
               (uint8_t *)GetModuleHandleW(nullptr) + 0x0033B163) ||
              ((uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress ==
               (uint8_t *)GetModuleHandleW(nullptr) + 0x00331BA1))) {
    // UI screen activation (exe+0x33B150 / exe+0x331B90): `screens[idx]->vfunc()`
    // where screens[] (exe+0x18138D0) entry was never created because the phase
    // OnEnter that builds it did not run in this port's flow. Skip the virtual call
    // but keep the bookkeeping store that follows it.
    PCONTEXT c = pExInfo->ContextRecord;
    uint8_t *hx = (uint8_t *)GetModuleHandleW(nullptr);
    bool is312 = ((uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress == hx + 0x0033B163);
    static volatile LONG s_skips3 = 0;
    if (InterlockedIncrement(&s_skips3) <= 6)
      log_line("[ui] screen table entry is NULL at exe+0x%X, skipping virtual call\r\n",
               is312 ? 0x33B163 : 0x331BA1);
    c->Rip = (DWORD64)(hx + (is312 ? 0x0033B169 : 0x00331BA7));
    return EXCEPTION_CONTINUE_EXECUTION;
  } else if (code == 0xC0000005 && pExInfo->ContextRecord &&
             (uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress >= (uint8_t *)GetModuleHandleW(nullptr) + 0x105AA0 &&
             (uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress < (uint8_t *)GetModuleHandleW(nullptr) + 0x1060E0) {
    // Render item walk (exe+0x1059B0) hit an item with garbage data (bad transform/bucket/output
    // pointer). End the walk for this frame the way the loop ends normally: rsi = end, continue at
    // the loop-end check (exe+0x1060E0). rbp is the function's frame pointer here.
    PCONTEXT c = pExInfo->ContextRecord;
    uint8_t *hx = (uint8_t *)GetModuleHandleW(nullptr);
    static volatile LONG s_walkAv = 0;
    LONG n = InterlockedIncrement(&s_walkAv);
    if (n <= 10 || (n % 100) == 0)
      log_line("[race] render item walk AV at exe+0x%llX -> walk ended (%ld so far)\r\n",
               (unsigned long long)((uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress - hx), (long)n);
    c->Rsi = *(DWORD64 *)(c->Rbp + 0xC8);
    c->Rip = (DWORD64)(hx + 0x1060E0);
    return EXCEPTION_CONTINUE_EXECUTION;
  } else if (code == 0xC0000005 && pExInfo->ContextRecord &&
             (uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress ==
                 (uint8_t *)GetModuleHandleW(nullptr) + 0x00154397) {
    // exe+0x154380 is the main-thread completion of the "Load next top mip" job:
    // `dec [current_entry+0x1C]` where current_entry (global) is NULL because the
    // job (exe+0x15A920) found nothing to load and returned early. Same fix: return.
    PCONTEXT c = pExInfo->ContextRecord;
    static volatile LONG s_skips2 = 0;
    if (InterlockedIncrement(&s_skips2) <= 3)
      log_line("[job] exe+0x154380: no current mip entry, skipping completion (emulated return)\r\n");
    uint64_t *sp = (uint64_t *)c->Rsp;
    c->Rip = sp[5];       // frame: sub rsp,0x28 -> ret addr at rsp+0x28
    c->Rsp += 0x30;
    c->Rax = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  } else if (code == 0xC0000005 &&
             pExInfo->ContextRecord &&
             (uint8_t *)pExInfo->ExceptionRecord->ExceptionAddress ==
                 (uint8_t *)GetModuleHandleW(nullptr) + 0x0015A95C) {
    // exe+0x15A920 is a worker-thread job that picks the highest-priority entry
    // of a list in [exe+0x157E870]; the list is empty here (a registration that
    // the Durango build does at startup is missing), so entry==NULL is
    // dereferenced. Emulate the job returning early: restore RBX (pushed at
    // [rsp+0x20]), pop the return address, drop the 0x30-byte frame.
    PCONTEXT c = pExInfo->ContextRecord;
    static volatile LONG s_skips = 0;
    if (InterlockedIncrement(&s_skips) <= 3)
      log_line("[job] exe+0x15A920: empty priority list, skipping job (emulated return)\r\n");
    uint64_t *sp = (uint64_t *)c->Rsp;
    c->Rbx = sp[4];
    c->Rip = sp[5];
    c->Rsp += 0x30;
    c->Rax = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  } else if (code == 0xC0000005) {
    PCONTEXT ctx = pExInfo->ContextRecord;
    void *faultAddr = pExInfo->ExceptionRecord->ExceptionAddress;
    HMODULE hFaultMod = nullptr;
    char modName[MAX_PATH] = "unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)faultAddr, &hFaultMod)) {
      GetModuleFileNameA(hFaultMod, modName, sizeof(modName));
      char *pSlash = strrchr(modName, '\\');
      if (pSlash)
        memmove(modName, pSlash + 1, strlen(pSlash + 1) + 1);
    }

    log_line("\r\n[VECTORED] Access Violation (%s) at %p (%s + 0x%llX) "
             "targeting %p\r\n",
             pExInfo->ExceptionRecord->ExceptionInformation[0] == 0 ? "Read"
                                                                    : "Write",
             faultAddr, modName, (uint64_t)faultAddr - (uint64_t)hFaultMod,
             (void *)pExInfo->ExceptionRecord->ExceptionInformation[1]);
    if (ctx) {
      log_line("  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\r\n",
               ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
      log_line("  RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\r\n",
               ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
      log_line("  R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\r\n", ctx->R8,
               ctx->R9, ctx->R10, ctx->R11);
      log_line(
          "  R12=%016llX R13=%016llX R14=%016llX R15=%016llX RIP=%016llX\r\n",
          ctx->R12, ctx->R13, ctx->R14, ctx->R15, ctx->Rip);
      {
        // Real unwind (uses .pdata) so we get the actual call chain in RVAs.
        CONTEXT uc = *ctx;
        HMODULE hExeBt = GetModuleHandleW(nullptr);
        char bt[1200];
        int o = _snprintf_s(bt, sizeof(bt), _TRUNCATE, "  [bt tid=%lu]", GetCurrentThreadId());
        for (int fr = 0; fr < 28 && uc.Rip; ++fr) {
          DWORD64 imgBase = 0;
          PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(uc.Rip, &imgBase, nullptr);
          if (imgBase == (DWORD64)hExeBt)
            o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " exe+%llX", (unsigned long long)(uc.Rip - imgBase));
          else
            o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " %llX(+%llX)", (unsigned long long)uc.Rip, (unsigned long long)(uc.Rip - imgBase));
          if (!rf) break;
          void *hd = nullptr; DWORD64 ef = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgBase, uc.Rip, rf, &uc, &hd, &ef, nullptr);
        }
        log_line("%s\r\n", bt);
      }
      {
        // Diagnostic: engine "systems" table (exe+0x16962E0, 22 entries) used by exe+0x25F5C0.
        HMODULE hx = GetModuleHandleW(nullptr);
        if (hx && (uint8_t *)faultAddr >= (uint8_t *)hx &&
            (uint8_t *)faultAddr < (uint8_t *)hx + 0x1A0F000) {
          __try {
            void **t = (void **)((uint8_t *)hx + 0x016962E0);
            char tb[600]; int o = _snprintf_s(tb, sizeof(tb), _TRUNCATE, "  [systems]");
            for (int i = 0; i < 22; ++i)
              o += _snprintf_s(tb + o, sizeof(tb) - o, _TRUNCATE, " %d:%p", i, t[i]);
            log_line("%s\r\n", tb);
            void **sc = (void **)((uint8_t *)hx + 0x018138D0);
            int o2 = _snprintf_s(tb, sizeof(tb), _TRUNCATE, "  [screens]");
            for (int i = 0; i < 8; ++i) o2 += _snprintf_s(tb + o2, sizeof(tb) - o2, _TRUNCATE, " %d:%p", i, sc[i]);
            log_line("%s\r\n", tb);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
          }
        }
      }
      {
        // Diagnostic: first qwords pointed to by the main registers (vector/object layouts).
        const uint64_t rv[8] = { ctx->Rbx, ctx->Rsi, ctx->Rdi, ctx->R12, ctx->R13, ctx->R14, ctx->R15, ctx->Rbp };
        const char *rn[8] = { "rbx", "rsi", "rdi", "r12", "r13", "r14", "r15", "rbp" };
        for (int ri = 0; ri < 8; ++ri) {
          uintptr_t v = (uintptr_t)rv[ri];
          if (v < 0x10000 || v > 0x00007FFFFFFE0000ULL || (v & 7)) continue;
          // VEH runs before __try handlers, so a faulting read here would recurse: check first.
          MEMORY_BASIC_INFORMATION mbi = {};
          if (!VirtualQuery((void *)v, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
              (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
              !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            continue;
          uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
          int nq = (int)((regionEnd - v) / 8); if (nq > 10) nq = 10;
          char db[400]; int o = _snprintf_s(db, sizeof(db), _TRUNCATE, "  [mem %s=%llX]", rn[ri], (unsigned long long)v);
          for (int q = 0; q < nq; ++q) o += _snprintf_s(db + o, sizeof(db) - o, _TRUNCATE, " %llX", (unsigned long long)((uint64_t *)v)[q]);
          log_line("%s\r\n", db);
        }
      }
      uint64_t *sp = (uint64_t *)ctx->Rsp;
      __try {
        for (int i = 0; i < 8; ++i) {
          HMODULE hMod = nullptr;
          char stkMod[MAX_PATH] = "";
          if (GetModuleHandleExA(
                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                  (LPCSTR)sp[i], &hMod)) {
            GetModuleFileNameA(hMod, stkMod, sizeof(stkMod));
            char *pS = strrchr(stkMod, '\\');
            if (pS)
              memmove(stkMod, pS + 1, strlen(pS + 1) + 1);
            log_line("  Stack[%d] = %016llX (%s + 0x%llX)\r\n", i,
                     (unsigned long long)sp[i], stkMod,
                     (unsigned long long)sp[i] - (unsigned long long)hMod);
          } else {
            log_line("  Stack[%d] = %016llX\r\n", i, (unsigned long long)sp[i]);
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    if (g_pMainDevice) {
      __try {
        HRESULT remReason = g_pMainDevice->GetDeviceRemovedReason();
        log_line("[d3d12_x] Crash Handler: "
                 "g_pMainDevice->GetDeviceRemovedReason() = 0x%08lX\r\n",
                 (unsigned long)remReason);
        FlushD3D12InfoQueue("VectoredCrashHandler");
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[d3d12_x] Crash Handler: Exception querying "
                 "DeviceRemovedReason/InfoQueue\r\n");
      }
    }
    log_line("[d3d12_x] Crash Handler completed for Access Violation\r\n");
  } else if ((code & 0xC0000000) == 0xC0000000 || code == 0x80000003) {
    PCONTEXT ctx = pExInfo->ContextRecord;
    void *faultAddr = pExInfo->ExceptionRecord->ExceptionAddress;
    HMODULE hFaultMod = nullptr;
    char modName[MAX_PATH] = "unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)faultAddr, &hFaultMod) &&
        hFaultMod) {
      GetModuleFileNameA(hFaultMod, modName, sizeof(modName));
      char *pSlash = strrchr(modName, '\\');
      if (pSlash)
        memmove(modName, pSlash + 1, strlen(pSlash + 1) + 1);
    }

    log_line("\r\n[VECTORED] Fatal Exception 0x%08lX at %p (%s + 0x%llX)\r\n",
             code, faultAddr, modName,
             (uint64_t)faultAddr - (uint64_t)hFaultMod);
    if (ctx) {
      log_line(
          "  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\r\n"
          "  RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\r\n"
          "  R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\r\n"
          "  R12=%016llX R13=%016llX R14=%016llX R15=%016llX RIP=%016llX\r\n",
          ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->Rbp,
          ctx->Rsp, ctx->R8, ctx->R9, ctx->R10, ctx->R11, ctx->R12, ctx->R13,
          ctx->R14, ctx->R15, ctx->Rip);
      {
        CONTEXT uc = *ctx;
        HMODULE hExeBt = GetModuleHandleW(nullptr);
        char bt[1200];
        int o = _snprintf_s(bt, sizeof(bt), _TRUNCATE, "  [bt tid=%lu]", GetCurrentThreadId());
        for (int fr = 0; fr < 28 && uc.Rip; ++fr) {
          DWORD64 imgBase = 0;
          PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(uc.Rip, &imgBase, nullptr);
          if (imgBase == (DWORD64)hExeBt)
            o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " exe+%llX", (unsigned long long)(uc.Rip - imgBase));
          else
            o += _snprintf_s(bt + o, sizeof(bt) - o, _TRUNCATE, " %llX(+%llX)", (unsigned long long)uc.Rip, (unsigned long long)(uc.Rip - imgBase));
          if (!rf) break;
          void *hd = nullptr; DWORD64 ef = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgBase, uc.Rip, rf, &uc, &hd, &ef, nullptr);
        }
        log_line("%s\r\n", bt);
      }
    }
    if (g_pMainDevice) {
      __try {
        HRESULT remReason = g_pMainDevice->GetDeviceRemovedReason();
        log_line("[d3d12_x] Crash Handler: "
                 "g_pMainDevice->GetDeviceRemovedReason() = 0x%08lX\r\n",
                 (unsigned long)remReason);
        FlushD3D12InfoQueue("VectoredCrashHandler");
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[d3d12_x] Crash Handler: Exception querying "
                 "DeviceRemovedReason/InfoQueue\r\n");
      }
    }
    log_line("[d3d12_x] Crash Handler completed for Exception 0x%08lX\r\n",
             code);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// Bink2 Video Hooks
// ============================================================================
typedef void* HBINK;

typedef HBINK (WINAPI *PFN_BinkOpen)(const void* name, uint32_t flags);
typedef void  (WINAPI *PFN_BinkClose)(HBINK bink);
typedef int32_t (WINAPI *PFN_BinkWait)(HBINK bink);
typedef int32_t (WINAPI *PFN_BinkNextFrame)(HBINK bink);
typedef int32_t (WINAPI *PFN_BinkDoFrameAsyncMulti)(HBINK bink, void* a, void* b);
typedef int32_t (WINAPI *PFN_BinkGetFrameBuffersInfo)(HBINK bink, void* fbinfo);
typedef int32_t (WINAPI *PFN_BinkRegisterFrameBuffers)(HBINK bink, void* fbinfo);

static PFN_BinkOpen g_TrueBinkOpen = nullptr;
static PFN_BinkClose g_TrueBinkClose = nullptr;
static PFN_BinkWait g_TrueBinkWait = nullptr;
static PFN_BinkNextFrame g_TrueBinkNextFrame = nullptr;
static PFN_BinkDoFrameAsyncMulti g_TrueBinkDoFrameAsyncMulti = nullptr;
static PFN_BinkGetFrameBuffersInfo g_TrueBinkGetFrameBuffersInfo = nullptr;
static PFN_BinkRegisterFrameBuffers g_TrueBinkRegisterFrameBuffers = nullptr;

// ---- CRT file-open tracing (the game reads dat archives via fopen/_wfopen) ----
typedef FILE *(__cdecl *PFN_fopen)(const char *, const char *);
typedef FILE *(__cdecl *PFN_wfopen)(const wchar_t *, const wchar_t *);
typedef errno_t (__cdecl *PFN_fopen_s)(FILE **, const char *, const char *);
static PFN_fopen g_TrueFopen = nullptr;
static PFN_wfopen g_TrueWfopen = nullptr;
static PFN_fopen_s g_TrueFopenS = nullptr;
static volatile LONG g_fopenLogCount = 0;
static void LogFopen(const char *api, const char *path, const char *mode, bool ok) {
  if (path && strstr(path, "savedownloads")) return;  // optional DLC probes: very noisy
  LONG n = InterlockedIncrement(&g_fopenLogCount);
  if (n <= 400)
    log_line("[fs] %s('%s', '%s') -> %s\r\n", api, path ? path : "<null>", mode ? mode : "", ok ? "OK" : "FAIL");
}
static FILE *__cdecl HookedFopen(const char *path, const char *mode) {
  FILE *f = g_TrueFopen ? g_TrueFopen(path, mode) : nullptr;
  LogFopen("fopen", path, mode, f != nullptr);
  return f;
}
static FILE *__cdecl HookedWfopen(const wchar_t *path, const wchar_t *mode) {
  FILE *f = g_TrueWfopen ? g_TrueWfopen(path, mode) : nullptr;
  char pa[520] = {}, ma[16] = {};
  if (path) WideCharToMultiByte(CP_ACP, 0, path, -1, pa, sizeof(pa) - 1, nullptr, nullptr);
  if (mode) WideCharToMultiByte(CP_ACP, 0, mode, -1, ma, sizeof(ma) - 1, nullptr, nullptr);
  LogFopen("_wfopen", pa, ma, f != nullptr);
  return f;
}
static errno_t __cdecl HookedFopenS(FILE **pf, const char *path, const char *mode) {
  errno_t r = g_TrueFopenS ? g_TrueFopenS(pf, path, mode) : EINVAL;
  LogFopen("fopen_s", path, mode, r == 0);
  return r;
}

static HBINK WINAPI HookedBinkOpen(const void* name, uint32_t flags) {
  const char* str = "<null>";
  if (name && (uintptr_t)name > 0x10000) {
    if ((flags & 0x00800000) == 0) {
      str = (const char*)name;
    } else {
      str = "<memory buffer>";
    }
  }
  log_line("[bink_hook] BinkOpen('%s', flags=0x%08X)\r\n", str, flags);
  HBINK h = g_TrueBinkOpen ? g_TrueBinkOpen(name, flags) : nullptr;
  log_line("  -> BinkOpen returned handle=%p\r\n", h);
  return h;
}

static void WINAPI HookedBinkClose(HBINK bink) {
  {
    void *bt[12] = {};
    USHORT n = RtlCaptureStackBackTrace(0, 12, bt, nullptr);
    char sb[512]; int so = 0;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    for (USHORT i = 0; i < n; ++i)
      so += _snprintf_s(sb + so, sizeof(sb) - so, _TRUNCATE, " %llX", (unsigned long long)((uintptr_t)bt[i] - exe));
    uint32_t *bk = (uint32_t *)bink;
    log_line("[bink_hook] BinkClose(handle=%p) W=%u H=%u Frames=%u FrameNum=%u stack(exe-rel):%s\r\n", bink,
             bk ? bk[0] : 0, bk ? bk[1] : 0, bk ? bk[2] : 0, bk ? bk[3] : 0, sb);
  }
  if (g_TrueBinkClose) g_TrueBinkClose(bink);
}

static int32_t WINAPI HookedBinkWait(HBINK bink) {
  return g_TrueBinkWait ? g_TrueBinkWait(bink) : 0;
}

static int32_t WINAPI HookedBinkNextFrame(HBINK bink) {
  static volatile LONG s_binkFrames = 0;
  LONG f = InterlockedIncrement(&s_binkFrames);
  if (f <= 30 || (f % 60 == 0)) {
    uint32_t *bk = (uint32_t *)bink;
    log_line("[bink_hook] BinkNextFrame(handle=%p, frame=%ld) W=%u H=%u Frames=%u FrameNum=%u\r\n", bink, f,
             bk ? bk[0] : 0, bk ? bk[1] : 0, bk ? bk[2] : 0, bk ? bk[3] : 0);
  }
  return g_TrueBinkNextFrame ? g_TrueBinkNextFrame(bink) : 0;
}

static int32_t WINAPI HookedBinkDoFrameAsyncMulti(HBINK bink, void* a, void* b) {
  return g_TrueBinkDoFrameAsyncMulti ? g_TrueBinkDoFrameAsyncMulti(bink, a, b) : 0;
}

static int32_t WINAPI HookedBinkGetFrameBuffersInfo(HBINK bink, void* fbinfo) {
  int32_t res = g_TrueBinkGetFrameBuffersInfo ? g_TrueBinkGetFrameBuffersInfo(bink, fbinfo) : 0;
  log_line("[bink_hook] BinkGetFrameBuffersInfo(handle=%p, fbinfo=%p) -> res=%d\r\n", bink, fbinfo, res);
  return res;
}

static int32_t WINAPI HookedBinkRegisterFrameBuffers(HBINK bink, void* fbinfo) {
  int32_t res = g_TrueBinkRegisterFrameBuffers ? g_TrueBinkRegisterFrameBuffers(bink, fbinfo) : 0;
  log_line("[bink_hook] BinkRegisterFrameBuffers(handle=%p, fbinfo=%p) -> res=%d\r\n", bink, fbinfo, res);
  return res;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID lpReserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    log_line("[d3d12_x] loaded (OnRush diagnostic bridge & desktop translation "
             "layer)\r\n");
    PVOID hVeh = AddVectoredExceptionHandler(1, VectoredCrashHandler);
    InstallTracer(GetModuleHandleW(nullptr));
    log_line("[d3d12_x] Registered VectoredCrashHandler before execution "
             "(handle=%p)\r\n",
             hVeh);

    HMODULE hExe = GetModuleHandleW(nullptr);
    // Xbox Integrated Multiplayer has no Xbox Live backend on PC: pressing A at the intro registers
    // the local user via XimSetIntendedLocalXboxUserIds, which fails (0x89234501) and the game then
    // calls XimFailFastWithInform -> process exits with 0x675. Report success / ignore the fail-fast.
    // ONRUSH_NO_XIM_FIX=1 disables this.
    if (GetEnvironmentVariableA("ONRUSH_NO_XIM_FIX", nullptr, 0) == 0) {
      if (HookIAT(hExe, "XboxIntegratedMultiplayer.dll", "XimSetIntendedLocalXboxUserIds",
                  (void *)&HookedXimSetIntendedLocalXboxUserIds, (void **)&g_TrueXimSetIntendedLocalXboxUserIds))
        log_line("[xim] hooked XimSetIntendedLocalXboxUserIds\r\n");
      if (HookIAT(hExe, "XboxIntegratedMultiplayer.dll", "XimFailFastWithInform",
                  (void *)&HookedXimFailFastWithInform, nullptr))
        log_line("[xim] hooked XimFailFastWithInform\r\n");
    }
    if (HookIAT(hExe, "combase.dll", "RoGetActivationFactory",
                (void *)&HookedRoGetActivationFactory,
                (void **)&g_TrueRoGetActivationFactory)) {
      log_line("[d3d12_x] Hooked combase.dll!RoGetActivationFactory in game "
               "executable\r\n");
    } else {
      log_line("[d3d12_x] WARNING: Could not find RoGetActivationFactory in "
               "IAT\r\n");
    }

    if (HookIAT(hExe, "vccorlib140.dll",
                "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@"
                "PEAPEAX@Z",
                (void *)&HookedGetActivationFactoryByPCWSTR,
                (void **)&g_TrueGetActivationFactoryByPCWSTR)) {
      log_line("[d3d12_x] Hooked vccorlib140.dll!GetActivationFactoryByPCWSTR "
               "in game executable\r\n");
    } else {
      log_line("[d3d12_x] WARNING: Could not find GetActivationFactoryByPCWSTR "
               "in IAT\r\n");
    }

    HookIAT(hExe, "vccorlib140.dll",
            "?__abi_WinRTraiseClassNotRegisteredException@@YAXXZ",
            (void *)&HookedWinRTraiseClassNotRegisteredException, nullptr);
    HookIAT(hExe, "vccorlib140.dll", "?__abi_WinRTraiseCOMException@@YAXJ@Z",
            (void *)&HookedWinRTraiseCOMException, nullptr);
    HookIAT(hExe, "vccorlib140.dll", "?__abi_WinRTraiseFailureException@@YAXXZ",
            (void *)&HookedWinRTraiseFailureException, nullptr);

    HMODULE hVccorlib = GetModuleHandleW(L"vccorlib140.dll");
    if (hVccorlib) {
      HookIAT(hVccorlib, "api-ms-win-core-winrt-l1-1-0.dll",
              "RoGetActivationFactory", (void *)&HookedRoGetActivationFactory,
              nullptr);
      HookIAT(hVccorlib, "combase.dll", "RoGetActivationFactory",
              (void *)&HookedRoGetActivationFactory, nullptr);
    }

    HookIAT(hExe, "ucrtbase.dll", "abort", (void *)&HookedAbort,
            (void **)&g_TrueAbort);
    HookIAT(hExe, "ucrtbase.dll", "terminate", (void *)&HookedTerminate,
            (void **)&g_TrueTerminate);
    HookIAT(hExe, "ucrtbase.dll", "_invalid_parameter_noinfo",
            (void *)&HookedInvalidParamNoInfo,
            (void **)&g_TrueInvalidParamNoInfo);
    HookIAT(hExe, "ucrtbase.dll", "_invalid_parameter_noinfo_noreturn",
            (void *)&HookedInvalidParamNoInfoNoReturn,
            (void **)&g_TrueInvalidParamNoInfoNoReturn);
    HookIAT(hExe, "VCRUNTIME140.dll", "_purecall", (void *)&HookedPurecall,
            (void **)&g_TruePurecall);
    HookIAT(hExe, "VCRUNTIME140.dll", "__std_terminate",
            (void *)&HookedStdTerminate, (void **)&g_TrueStdTerminate);
    HookIAT(hExe, "VCRUNTIME140.dll", "_CxxThrowException",
            (void *)&HookedCxxThrowException,
            (void **)&g_TrueCxxThrowException);

    bool binkHooked = HookIAT(hExe, "Bink2XboxOne.dll", "BinkOpen", (void*)&HookedBinkOpen, (void**)&g_TrueBinkOpen);
    HookIAT(hExe, "ucrtbase.dll", "fopen", (void*)&HookedFopen, (void**)&g_TrueFopen);
    HookIAT(hExe, "ucrtbase.dll", "_wfopen", (void*)&HookedWfopen, (void**)&g_TrueWfopen);
    HookIAT(hExe, "ucrtbase.dll", "fopen_s", (void*)&HookedFopenS, (void**)&g_TrueFopenS);
    log_line("[d3d12_x] HookIAT Bink2XboxOne.dll!BinkOpen: %s\r\n", binkHooked ? "SUCCESS" : "FAILED");
    HookIAT(hExe, "Bink2XboxOne.dll", "BinkClose", (void*)&HookedBinkClose, (void**)&g_TrueBinkClose);
    HookIAT(hExe, "Bink2XboxOne.dll", "BinkWait", (void*)&HookedBinkWait, (void**)&g_TrueBinkWait);
    HookIAT(hExe, "Bink2XboxOne.dll", "BinkNextFrame", (void*)&HookedBinkNextFrame, (void**)&g_TrueBinkNextFrame);
    HookIAT(hExe, "Bink2XboxOne.dll", "BinkDoFrameAsyncMulti", (void*)&HookedBinkDoFrameAsyncMulti, (void**)&g_TrueBinkDoFrameAsyncMulti);
    HookIAT(hExe, "Bink2XboxOne.dll", "BinkGetFrameBuffersInfo", (void*)&HookedBinkGetFrameBuffersInfo, (void**)&g_TrueBinkGetFrameBuffersInfo);
    HookIAT(hExe, "Bink2XboxOne.dll", "BinkRegisterFrameBuffers", (void*)&HookedBinkRegisterFrameBuffers, (void**)&g_TrueBinkRegisterFrameBuffers);

    PatchAllocThunk(hExe);
  } else if (reason == DLL_PROCESS_DETACH) {
    log_line("[d3d12_x] DLL_PROCESS_DETACH (lpReserved=%p)\r\n", lpReserved);
    if (g_pMainDevice) {
      __try {
        HRESULT remReason = g_pMainDevice->GetDeviceRemovedReason();
        log_line("[d3d12_x] Process Detach: "
                 "g_pMainDevice->GetDeviceRemovedReason() = 0x%08lX\r\n",
                 (unsigned long)remReason);
        FlushD3D12InfoQueue("DLL_PROCESS_DETACH");
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  }
  return TRUE;
}

#pragma comment(linker,                                                        \
                "/export:D3D12XboxCreateDevice=OnRushD3D12XboxCreateDevice")
#pragma comment(linker, "/export:DXGIXPresentArray=OnRushDXGIXPresentArray")
#pragma comment(                                                               \
    linker,                                                                    \
    "/export:D3D12SerializeRootSignature=OnRushD3D12SerializeRootSignature")
#pragma comment(linker, "/export:D3DMapEsramMemory=OnRushD3DMapEsramMemory")
