// Mouse in a race: the mouse turns the chase camera.
//
// The race cameras read the DrivingInput axes "look_around_xaxis" / "look_around_yaxis" (right stick:
// an angle offset from behind the car, the camera swings back when the stick is released) through the
// input context getter exe+0xC3AD0(context, name) -> float (exe+0x4050DA, exe+0x4064D6). That getter
// is hooked: for those two names the mouse offset is added to the pad's value. Mouse movement comes
// from raw input (WM_INPUT on the game window). While a race camera is reading the axes and the game
// window has focus, the cursor is hidden and kept inside the window; after a moment without mouse
// movement the camera swings back behind the car, like the stick.
// onrush.ini [Controls] MouseLook=0 turns it off, MouseSensitivity=1.0 scales it.
namespace mouse {

typedef float (*PFN_AxisGet)(void *ctx, const char *name);
static PFN_AxisGet g_trueAxisGet = nullptr;
static const char *g_nameX = nullptr, *g_nameY = nullptr;   // the strings the race cameras pass
static volatile LONG64 g_lookTick = 0;                      // last time a race camera read the axes
static volatile LONG g_dx = 0, g_dy = 0;                    // raw mouse motion not consumed yet
static float g_x = 0.0f, g_y = 0.0f;                        // mouse offset added to the axes
static bool g_enabled = true;
static float g_sens = 1.0f;
static volatile LONG g_hide = 0;                            // cursor hidden (WM_SETCURSOR)
static bool g_rawRegistered = false;

static float Clamp1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

static float HookAxisGet(void *ctx, const char *name) {
  float v = g_trueAxisGet(ctx, name);
  if (!g_enabled || (name != g_nameX && name != g_nameY)) return v;
  InterlockedExchange64(&g_lookTick, (LONG64)GetTickCount64());
  return Clamp1(v + (name == g_nameX ? g_x : g_y));
}

// Called on every input poll (game thread): turns the raw motion into the camera offset.
static void Update(bool focused) {
  if (!g_enabled) return;
  static ULONGLONG s_last = 0, s_lastMove = 0;
  const ULONGLONG now = GetTickCount64();
  const float dt = s_last ? (float)(now - s_last) / 1000.0f : 0.0f;
  s_last = now;
  const bool racing = focused && now - (ULONGLONG)g_lookTick < 250;
  const LONG dx = InterlockedExchange(&g_dx, 0), dy = InterlockedExchange(&g_dy, 0);
  if (racing) {
    if (dx || dy) s_lastMove = now;
    const float k = 0.0015f * g_sens;   // ~650 counts (a few cm at 800 dpi) = full stick
    g_x = Clamp1(g_x + dx * k);
    g_y = Clamp1(g_y - dy * k);
    if (now - s_lastMove > 900) {       // idle: swing back behind the car
      const float f = dt > 0.0f ? expf(-dt / 0.18f) : 1.0f;
      g_x *= f; g_y *= f;
      if (fabsf(g_x) < 0.002f) g_x = 0.0f;
      if (fabsf(g_y) < 0.002f) g_y = 0.0f;
    }
  } else {
    g_x = g_y = 0.0f;
  }
  // keep the cursor hidden and inside the window while driving
  const LONG hide = racing ? 1 : 0;
  if (InterlockedExchange(&g_hide, hide) != hide) {
    HWND w = g_desktopHwnd;
    if (hide && w) {
      RECT r; GetClientRect(w, &r);
      POINT a = {r.left, r.top}, b = {r.right, r.bottom};
      ClientToScreen(w, &a); ClientToScreen(w, &b);
      RECT c = {a.x, a.y, b.x, b.y};
      ClipCursor(&c);
      SetCursorPos((a.x + b.x) / 2, (a.y + b.y) / 2);
    } else {
      ClipCursor(nullptr);
    }
    if (w) PostMessageW(w, WM_SETCURSOR, (WPARAM)w, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
  }
}

// Game window messages (frame thread). Returns true when the message was handled.
static bool OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *res) {
  if (!g_rawRegistered && g_enabled) {
    RAWINPUTDEVICE rid = {0x01, 0x02, 0, hwnd};   // generic desktop / mouse, to this window
    g_rawRegistered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
    if (!g_rawRegistered) { g_rawRegistered = true; log_line("[mouse] raw input unavailable (%lu)\r\n", GetLastError()); }
  }
  switch (msg) {
    case WM_INPUT: {
      RAWINPUT ri; UINT sz = sizeof(ri);
      if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
          ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        InterlockedExchangeAdd(&g_dx, ri.data.mouse.lLastX);
        InterlockedExchangeAdd(&g_dy, ri.data.mouse.lLastY);
      }
      return false;   // DefWindowProc cleans the input up
    }
    case WM_SETCURSOR:
      if (g_hide && LOWORD(lParam) == HTCLIENT) { SetCursor(nullptr); *res = TRUE; return true; }
      return false;
    case WM_KILLFOCUS:
      ClipCursor(nullptr);
      return false;
  }
  return false;
}

static void Install(HMODULE hExe) {
  uint8_t *exe = (uint8_t *)hExe;
  if (gfxopt::g_ini[0]) {
    g_enabled = GetPrivateProfileIntW(L"Controls", L"MouseLook", 1, gfxopt::g_ini) != 0;
    wchar_t b[16] = {};
    GetPrivateProfileStringW(L"Controls", L"MouseSensitivity", L"1.0", b, 16, gfxopt::g_ini);
    float s = (float)_wtof(b);
    if (s > 0.05f && s < 20.0f) g_sens = s;
  }
  if (!g_enabled) { log_line("[mouse] mouse look off (onrush.ini)\r\n"); return; }
  uint8_t *get = exe + 0x000C3AD0;
  static const uint8_t kHead[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};   // push rbx; sub rsp,20h; mov rbx,rcx
  // the race cameras: lea rdx,[rip+x] -> "look_around_xaxis" / "look_around_yaxis"
  auto leaTarget = [&](uint32_t rva) -> const char * {
    const uint8_t *p = exe + rva;
    if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) return nullptr;
    return (const char *)(p + 7 + *(const int32_t *)(p + 3));
  };
  g_nameX = leaTarget(0x004050DA);
  g_nameY = leaTarget(0x004050EC);
  if (memcmp(get, kHead, sizeof(kHead)) != 0 || !g_nameX || !g_nameY || strcmp(g_nameX, "look_around_xaxis") != 0 ||
      strcmp(g_nameY, "look_around_yaxis") != 0 || leaTarget(0x004064D6) != g_nameX) {
    log_line("[mouse] WARNING: unexpected input code; mouse look not installed\r\n");
    g_enabled = false;
    return;
  }
  uint8_t *mem = (uint8_t *)AllocateNearAddress(get, 0x40);
  if (!mem) { g_enabled = false; return; }
  DWORD old = 0;
  VirtualProtect(mem, 0x40, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = mem;
  g_trueAxisGet = (PFN_AxisGet)q;
  memcpy(q, kHead, sizeof(kHead)); q += sizeof(kHead);
  q = gfxopt::EmitJmpAbs(q, get + sizeof(kHead));
  uint8_t *stub = q;
  q = gfxopt::EmitJmpAbs(q, (void *)&HookAxisGet);
  FlushInstructionCache(GetCurrentProcess(), mem, (size_t)(q - mem));
  if (gfxopt::WriteJmpRel(get, stub, sizeof(kHead))) log_line("[mouse] mouse look installed (sensitivity %.2f)\r\n", g_sens);
  else g_enabled = false;
}

}  // namespace mouse
