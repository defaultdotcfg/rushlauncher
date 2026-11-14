// Mouse support: camera in a race, pointer in the menus.
//
// Race: the race cameras read the DrivingInput axes "look_around_xaxis" / "look_around_yaxis" (right
// stick: an angle offset from behind the car, the camera swings back when the stick is released)
// through the input context getter exe+0xC3AD0(context, name) -> float (exe+0x4050DA, exe+0x4064D6).
// That getter is hooked: for those two names the mouse offset is added to the pad's value. Mouse
// movement comes from raw input (WM_INPUT on the game window). While a race camera is reading the axes
// and the game window has focus, the cursor is hidden and kept inside the window; after a moment
// without mouse movement the camera swings back behind the car, like the stick.
//
// Menus: the UI keeps a stack of pages per layer (manager +0x600 = count, 0x18-byte entries). A page
// lists its focusable elements at +0x2A8 (base) / +0x2B0 (bytes), 0x40 bytes each (element first), and
// the focused one at +0x2D0. The generic navigation (exe+0x1DE860, run for each layer until one takes
// the input) moves that focus; list elements (type 5, uiTableDrawCommand) also have a selected row
// (+0x954), drawn by repositioning one row template per row (exe+0x1EABED). Pointing at an element or
// a row moves the focus there exactly the way the pad navigation does (same events and sound); the
// left button then acts as A (confirm) on it, the right button as B (back), the wheel as d-pad up/down.
//
// onrush.ini [Controls]: MouseLook=0 turns the race camera off, MouseSensitivity=1.0 scales it,
// MenuMouse=0 turns the menu pointer off.
namespace mouse {

typedef float (*PFN_AxisGet)(void *ctx, const char *name);
static PFN_AxisGet g_trueAxisGet = nullptr;
static const char *g_nameX = nullptr, *g_nameY = nullptr;   // the strings the race cameras pass
static volatile LONG64 g_lookTick = 0;                      // last time a race camera read the axes
static volatile LONG g_dx = 0, g_dy = 0;                    // raw mouse motion not consumed yet
static float g_x = 0.0f, g_y = 0.0f;                        // mouse offset added to the axes
static bool g_enabled = true;                               // race camera
static float g_sens = 1.0f;
static volatile LONG g_hide = 0;                            // cursor hidden (WM_SETCURSOR)
static volatile LONG g_racing = 0;
static bool g_rawRegistered = false;

static float Clamp1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

static float HookAxisGet(void *ctx, const char *name) {
  float v = g_trueAxisGet(ctx, name);
  if (!g_enabled || (name != g_nameX && name != g_nameY)) return v;
  InterlockedExchange64(&g_lookTick, (LONG64)GetTickCount64());
  return Clamp1(v + (name == g_nameX ? g_x : g_y));
}

// ---------------------------------------------------------------- menus
static uint8_t *g_exe = nullptr;
static bool g_menuEnabled = true;
static volatile LONG g_menuHit = 0;      // the pointer is over a focusable element / row (set each UI frame)
static volatile LONG g_clickDir = 0;     // what a click there means: 0 confirm, -1 / +1 left / right (option arrows)
static volatile LONG g_wheel = 0;        // wheel notches not consumed yet (+ = up)
static volatile LONG g_frame = 0;        // input polls (one UI navigation pass per poll)
static volatile LONG64 g_menuTick = 0;   // last time a page with focusable elements took the input (pause menu over a race too)

typedef void (*PFN_Size)(void *el, float *w, float *h);
typedef void *(*PFN_Local)(void *el, float *m16);
typedef int (*PFN_Type)(void *el);
typedef bool (*PFN_PageActive)(void *page);
typedef void (*PFN_PageLeave)(void *page, void *el, int kind, int idx);
typedef void (*PFN_SendEvent)(void *el, uint64_t ev, void *args);
typedef void (*PFN_NavEvent)(void *page, void *el, int kind, int idx);   // exe+0x1F1F90
typedef void (*PFN_NavSound)(int id, uint8_t *flag);                    // exe+0x1F51D0
typedef void (*PFN_ListCount)(void *ctx, int *count);
typedef bool (*PFN_ListEnabled)(void *ctx, int idx);

static uint8_t *VT(void *o) { return *(uint8_t **)o; }
static int ElType(uint8_t *el) { return (*(PFN_Type *)(VT(el) + 0x18))(el); }

static void MatMul(const float *a, const float *b, float *o) {   // row vectors: o = a * b
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

struct Rect { float x0, y0, x1, y1; };   // UI space: origin at the screen centre, y up, 1920x1080
static bool Inside(const Rect &r, float x, float y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }

// screen rectangle of an element: its local transform times all its parents' (the draw uses the same)
static Rect ElRect(uint8_t *el) {
  static const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  alignas(16) float m[16], l[16], o[16];
  memcpy(m, I, sizeof(I));
  uint8_t *c = el;
  for (int d = 0; c && d < 32; ++d, c = *(uint8_t **)(c + 0x20)) {
    (*(PFN_Local *)(VT(c) + 0x100))(c, l);
    l[3] = l[7] = l[11] = 0.0f; l[15] = 1.0f;   // affine (the translation row comes with w = 0)
    MatMul(m, l, o);
    memcpy(m, o, sizeof(o));
  }
  float w = 0, h = 0;
  (*(PFN_Size *)(VT(el) + 0xa0))(el, &w, &h);
  const float hw = 0.5f * w * fabsf(m[0]), hh = 0.5f * h * fabsf(m[5]);
  return {m[12] - hw, m[13] - hh, m[12] + hw, m[13] + hh};
}

// rows of the lists, recorded while they are drawn (draw thread)
struct RowRec { uint8_t *table; int row; Rect r; ULONGLONG t; };
static RowRec g_rows[128];
static int g_rowN = 0;
static SRWLOCK g_rowLock = SRWLOCK_INIT;

static void RowDrawn(uint8_t *table, int row, uint8_t *tmpl) {
  if (!g_menuEnabled) return;
  __try {
    const Rect r = ElRect(tmpl), tr = ElRect(table);
    // rows scrolled outside the list are drawn clipped: only keep the ones inside it
    const float cy = 0.5f * (r.y0 + r.y1), cx = 0.5f * (r.x0 + r.x1);
    if (cy < tr.y0 || cy > tr.y1 || cx < tr.x0 || cx > tr.x1) return;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_rowLock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < g_rowN; ++i) {
      if (g_rows[i].table == table && g_rows[i].row == row) { slot = i; break; }
      if (g_rows[i].t < g_rows[oldest].t) oldest = i;
    }
    if (slot < 0) slot = g_rowN < 128 ? g_rowN++ : oldest;
    g_rows[slot] = {table, row, r, now};
    ReleaseSRWLockExclusive(&g_rowLock);
  } __except (1) {}
}

static bool ListRowEnabled(uint8_t *el, int row) {
  int count = 0;
  if (*(void **)(el + 0x998) || *(void **)(el + 0x990)) (*(PFN_ListCount *)(el + 0x990))(*(void **)(el + 0x998), &count);
  if (row < 0 || row >= count) return false;
  if (*(void **)(el + 0x9b0) || *(void **)(el + 0x9a8)) return (*(PFN_ListEnabled *)(el + 0x9a8))(*(void **)(el + 0x9b0), row);
  return true;
}

// focus another element of the page: the pad navigation's steps (exe+0x1DEB0D..0x1DEC2C)
static void FocusEntry(uint8_t *page, uint8_t *entry) {
  uint8_t *focus = *(uint8_t **)(page + 0x2d0);
  uint8_t *oldEl = focus ? *(uint8_t **)focus : nullptr;
  uint8_t *newEl = *(uint8_t **)entry;
  const uint64_t evOff = *(uint64_t *)(g_exe + 0x01411940), evLeave = *(uint64_t *)(g_exe + 0x01411880);
  if (oldEl) {
    const int t = ElType(oldEl);
    if (t == 5) {
      for (int i = 0; i < 0x80; ++i) { *(uint64_t *)(oldEl + 0x108 + i * 16) = evOff; *(uint64_t *)(oldEl + 0x110 + i * 16) = 0; }
    } else if (t == 6) {
      const uint32_t n = *(uint32_t *)(oldEl + 0x150) * *(uint32_t *)(oldEl + 0x154);
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t *cell = (*(uint8_t ***)(oldEl + 0x108))[i];
        uint64_t arg = *(uint64_t *)(g_exe + 0x01648558);
        (*(PFN_SendEvent *)(VT(cell) + 0x120))(cell, evOff, &arg);
      }
    } else {
      uint64_t arg = *(uint64_t *)(g_exe + 0x01648558);
      (*(PFN_SendEvent *)(VT(oldEl) + 0x120))(oldEl, evLeave, &arg);
    }
  }
  (*(PFN_PageLeave *)(VT(page) + 0x30))(page, oldEl, 1, -1);
  ((PFN_NavEvent)(g_exe + 0x001F1F90))(page, newEl, 0, -1);
  uint8_t z = 0;
  ((PFN_NavSound)(g_exe + 0x001F51D0))(*(int *)(g_exe + 0x014116E4), &z);
  *(uint8_t **)(page + 0x2d0) = entry;
}

static bool GridCellEnabled(uint8_t *el, int cell) {
  int count = 0;
  if (*(void **)(el + 0x188) || *(void **)(el + 0x180)) (*(PFN_ListCount *)(el + 0x180))(*(void **)(el + 0x188), &count);
  return cell >= 0 && cell < count;
}

// select another cell of a grid: the pad navigation's steps (exe+0x1DE7FB..0x1DE835)
static void SelectCell(uint8_t *page, uint8_t *el, int cell) {
  const int old = *(int *)(el + 0x15c);
  PFN_NavEvent ev = (PFN_NavEvent)(g_exe + 0x001F1F90);
  ev(page, el, 1, old);
  ev(page, el, 0, cell);
  uint8_t z = 0;
  ((PFN_NavSound)(g_exe + 0x001F51D0))(*(int *)(g_exe + 0x014116E4), &z);
  *(int *)(el + 0x15c) = cell;
}

// select another row of a list: the pad navigation's steps (exe+0x1DF185..0x1DF1D2)
static void SelectRow(uint8_t *page, uint8_t *el, int row) {
  const int old = *(int *)(el + 0x954);
  PFN_NavEvent ev = (PFN_NavEvent)(g_exe + 0x001F1F90);
  ev(page, el, 1, old);
  ev(page, el, 0, row);
  uint8_t z = 0;
  ((PFN_NavSound)(g_exe + 0x001F51D0))(*(int *)(g_exe + 0x014116E4), &z);
  *(int *)(el + 0x958) += row - old;
  *(int *)(el + 0x954) = row;
}

static bool CursorUI(float *ux, float *uy, bool *inside) {
  HWND w = g_desktopHwnd;
  POINT p;
  RECT rc;
  if (!w || !GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &rc)) return false;
  const float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
  if (cw < 1 || ch < 1) return false;
  *inside = p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom;
  // the UI canvas (1920x1080) fills the window
  *ux = ((float)p.x / cw - 0.5f) * 1920.0f;
  *uy = (0.5f - (float)p.y / ch) * 1080.0f;
  return true;
}

// one UI layer's navigation pass (game thread), before the game's own
static void MenuPointer(uint8_t *mgr) {
  static LONG s_doneFrame = -1;
  static float s_lastX = 1e9f, s_lastY = 1e9f;
  static bool s_lastDown = false;
  const uint32_t n = *(uint32_t *)(mgr + 0x600);
  if (!n || s_doneFrame == g_frame) return;
  uint8_t *page = *(uint8_t **)(mgr + (size_t)(n - 1) * 0x18);
  if (!page || !(*(PFN_PageActive *)(VT(page) + 0x40))(page)) return;
  s_doneFrame = g_frame;   // this layer takes the input this frame
  uint8_t *base = *(uint8_t **)(page + 0x2a8);
  const size_t count = *(size_t *)(page + 0x2b0) / 0x40;
  auto focusable = [](uint8_t *el) {
    const uint32_t fl = el ? *(uint32_t *)(el + 0xe8) : 0;
    return ((fl >> 1) & 1) && ((fl >> 4) & 1);   // the pad navigation's own filter
  };
  bool any = false;
  for (size_t i = 0; i < count && i < 256 && !any; ++i) any = focusable(*(uint8_t **)(base + i * 0x40));
  if (!any) { InterlockedExchange(&g_menuHit, 0); return; }   // HUD only: the mouse drives the camera
  InterlockedExchange64(&g_menuTick, (LONG64)GetTickCount64());
  float ux, uy;
  bool inside = false;
  if (GetForegroundWindow() != g_desktopHwnd || !CursorUI(&ux, &uy, &inside) || !inside) {
    InterlockedExchange(&g_menuHit, 0);
    return;
  }
  const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  const bool moved = fabsf(ux - s_lastX) > 0.5f || fabsf(uy - s_lastY) > 0.5f;
  const bool pressed = down && !s_lastDown;
  s_lastX = ux; s_lastY = uy; s_lastDown = down;

  // what is under the pointer: the smallest focusable element (or list row) containing it
  uint8_t *hitEntry = nullptr;
  int hitRow = -1;
  float hitArea = 1e30f;
  Rect hitRect = {};
  const ULONGLONG now = GetTickCount64();
  for (size_t i = 0; i < count && i < 256; ++i) {
    uint8_t *entry = base + i * 0x40;
    uint8_t *el = *(uint8_t **)entry;
    if (!focusable(el)) continue;
    const int type = ElType(el);
    if (type == 6) {   // grid: its cells are elements of their own
      const uint32_t cells = *(uint32_t *)(el + 0x150) * *(uint32_t *)(el + 0x154);
      uint8_t **cell = *(uint8_t ***)(el + 0x108);
      for (uint32_t k = 0; cell && k < cells && k < 256; ++k) {
        if (!cell[k]) continue;
        const Rect r = ElRect(cell[k]);
        const float a = (r.x1 - r.x0) * (r.y1 - r.y0);
        if (Inside(r, ux, uy) && a > 1.0f && a < hitArea) { hitArea = a; hitEntry = entry; hitRow = (int)k; hitRect = r; }
      }
    } else if (type == 5) {
      AcquireSRWLockShared(&g_rowLock);
      for (int k = 0; k < g_rowN; ++k) {
        const RowRec &rr = g_rows[k];
        if (rr.table != el || now - rr.t > 200 || !Inside(rr.r, ux, uy)) continue;
        const float a = (rr.r.x1 - rr.r.x0) * (rr.r.y1 - rr.r.y0);
        if (a < hitArea) { hitArea = a; hitEntry = entry; hitRow = rr.row; hitRect = rr.r; }
      }
      ReleaseSRWLockShared(&g_rowLock);
    } else {
      const Rect r = ElRect(el);
      const float a = (r.x1 - r.x0) * (r.y1 - r.y0);
      if (Inside(r, ux, uy) && a > 1.0f && a < hitArea) { hitArea = a; hitEntry = entry; hitRow = -1; hitRect = r; }
    }
  }
  const int hitType = hitEntry ? ElType(*(uint8_t **)hitEntry) : 0;
  if (hitEntry && hitRow >= 0 &&
      !(hitType == 6 ? GridCellEnabled(*(uint8_t **)hitEntry, hitRow) : ListRowEnabled(*(uint8_t **)hitEntry, hitRow)))
    hitEntry = nullptr;   // greyed-out row / empty cell
  // the game's option rows (1072x64: label on the left half, value on the right half between a "<" just
  // past the middle and a ">" near the right end, or a toggle / slider): a click on the "<" or ">" part
  // changes the value like the d-pad left / right, anywhere else it confirms (flips a toggle)
  LONG dir = 0;
  if (hitEntry) {
    const float w = hitRect.x1 - hitRect.x0, h = hitRect.y1 - hitRect.y0;
    if (w > 900.0f && w > 8.0f * h) {
      const float f = (ux - hitRect.x0) / w;
      dir = f >= 0.45f && f < 0.6f ? -1 : (f > 0.82f ? 1 : 0);
    }
  }
  InterlockedExchange(&g_clickDir, dir);
  InterlockedExchange(&g_menuHit, hitEntry ? 1 : 0);
  if (!hitEntry || !(moved || pressed)) return;
  if (*(uint8_t **)(page + 0x2d0) != hitEntry) FocusEntry(page, hitEntry);
  uint8_t *el = *(uint8_t **)hitEntry;
  if (hitRow >= 0 && hitType == 6 && *(int *)(el + 0x15c) != hitRow) SelectCell(page, el, hitRow);
  else if (hitRow >= 0 && hitType == 5 && !el[0x95c] && *(int *)(el + 0x954) != hitRow) SelectRow(page, el, hitRow);
}

typedef uint64_t (*PFN_UINav)(uint8_t *mgr, void *dt, void *input);
static PFN_UINav g_trueUINav = nullptr;
static uint64_t HookUINav(uint8_t *mgr, void *dt, void *input) {
  if (g_menuEnabled) {
    __try { MenuPointer(mgr); } __except (1) {}
  }
  return g_trueUINav(mgr, dt, input);
}

// Pad buttons the mouse presses in the menus (PollInput thread).
static DWORD Buttons(bool focused) {
  if (!g_menuEnabled || g_racing || !focused || GetForegroundWindow() != g_desktopHwnd) { InterlockedExchange(&g_wheel, 0); return 0; }
  DWORD m = 0;
  static bool s_lPrev = false, s_lArmed = false;
  static DWORD s_lButton = 0;
  const bool l = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, r = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  if (l && !s_lPrev) {   // a click on an element: A (or d-pad left/right on an option's arrows) while the button is held
    s_lArmed = g_menuHit != 0;
    const LONG dir = g_clickDir;
    s_lButton = dir < 0 ? 0x400 : (dir > 0 ? 0x800 : 0x10);
  }
  if (!l) s_lArmed = false;
  s_lPrev = l;
  if (s_lArmed) m |= s_lButton;
  static bool s_rArmed = false;
  float ux, uy; bool inside = false;
  static bool s_rPrev = false;
  if (r && !s_rPrev) s_rArmed = CursorUI(&ux, &uy, &inside) && inside;
  if (!r) s_rArmed = false;
  s_rPrev = r;
  if (s_rArmed) m |= 0x20;   // B
  // wheel: one d-pad tap per notch (pressed for a poll, released for a poll)
  static int s_phase = 0, s_dir = 0;
  if (s_phase == 0) {
    const LONG w = g_wheel;
    if (w) { s_dir = w > 0 ? 1 : -1; InterlockedExchangeAdd(&g_wheel, -s_dir); s_phase = 2; }
  }
  if (s_phase == 2) { m |= s_dir > 0 ? 0x100 : 0x200; s_phase = 1; }
  else if (s_phase == 1) s_phase = 0;
  return m;
}

// Called on every input poll (game thread): turns the raw motion into the camera offset.
static void Update(bool focused) {
  InterlockedIncrement(&g_frame);
  if (!g_enabled) return;
  static ULONGLONG s_last = 0, s_lastMove = 0;
  const ULONGLONG now = GetTickCount64();
  const float dt = s_last ? (float)(now - s_last) / 1000.0f : 0.0f;
  s_last = now;
  // driving: a race camera reads the look axes and no menu (pause, vehicle choice) takes the input
  const bool racing = focused && now - (ULONGLONG)g_lookTick < 250 && !(g_menuEnabled && now - (ULONGLONG)g_menuTick < 250);
  InterlockedExchange(&g_racing, racing ? 1 : 0);
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
    case WM_MOUSEWHEEL: {
      static int s_acc = 0;
      s_acc += GET_WHEEL_DELTA_WPARAM(wParam);
      while (s_acc >= WHEEL_DELTA) { InterlockedIncrement(&g_wheel); s_acc -= WHEEL_DELTA; }
      while (s_acc <= -WHEEL_DELTA) { InterlockedDecrement(&g_wheel); s_acc += WHEEL_DELTA; }
      *res = 0;
      return true;
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

static bool HookAt(uint8_t *site, const uint8_t *head, size_t len, void *hook, void **trampoline) {
  if (memcmp(site, head, len) != 0) return false;
  uint8_t *mem = (uint8_t *)AllocateNearAddress(site, 0x40);
  if (!mem) return false;
  DWORD old = 0;
  VirtualProtect(mem, 0x40, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = mem;
  *trampoline = q;
  memcpy(q, head, len); q += len;
  q = gfxopt::EmitJmpAbs(q, site + len);
  uint8_t *stub = q;
  q = gfxopt::EmitJmpAbs(q, hook);
  FlushInstructionCache(GetCurrentProcess(), mem, (size_t)(q - mem));
  return gfxopt::WriteJmpRel(site, stub, len);
}

static void InstallMenus(uint8_t *exe) {
  g_exe = exe;
  if (gfxopt::g_ini[0]) g_menuEnabled = GetPrivateProfileIntW(L"Controls", L"MenuMouse", 1, gfxopt::g_ini) != 0;
  if (!g_menuEnabled) { log_line("[mouse] menu pointer off (onrush.ini)\r\n"); return; }
  // the list row loop: the row template gets its position, then is drawn (exe+0x1EABD3..0x1EABF0)
  //   mov rax,[rdi]; vaddss xmm2,xmm11,[r12+964h]; vaddss xmm1,xmm10,[r12+960h]; mov rcx,rdi; call [rax+38h]
  static const uint8_t kRow[] = {0x48, 0x8B, 0x07, 0xC4, 0xC1, 0x22, 0x58, 0x94, 0x24, 0x64, 0x09, 0x00, 0x00, 0xC4, 0xC1,
                                 0x2A, 0x58, 0x8C, 0x24, 0x60, 0x09, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0xFF, 0x50, 0x38};
  static const uint8_t kNav[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50};
  uint8_t *row = exe + 0x001EABD3, *nav = exe + 0x001DE860;
  if (memcmp(row, kRow, sizeof(kRow)) != 0 || memcmp(nav, kNav, sizeof(kNav)) != 0) {
    log_line("[mouse] WARNING: unexpected UI code; menu pointer not installed\r\n");
    g_menuEnabled = false;
    return;
  }
  uint8_t *mem = (uint8_t *)AllocateNearAddress(row, 0x80);
  if (!mem) { g_menuEnabled = false; return; }
  DWORD old = 0;
  VirtualProtect(mem, 0x80, PAGE_EXECUTE_READWRITE, &old);
  uint8_t *q = mem;
  memcpy(q, kRow, sizeof(kRow)); q += sizeof(kRow);   // the original instructions (no RIP-relative operand)
  // RowDrawn(table = r12, row = r14d, template = rdi); volatile registers are dead after the call
  static const uint8_t kArgs[] = {0x4C, 0x89, 0xE1, 0x44, 0x89, 0xF2, 0x49, 0x89, 0xF8};   // mov rcx,r12; mov edx,r14d; mov r8,rdi
  memcpy(q, kArgs, sizeof(kArgs)); q += sizeof(kArgs);
  *q++ = 0x48; *q++ = 0xB8; *(uint64_t *)q = (uint64_t)(uintptr_t)&RowDrawn; q += 8;   // mov rax, RowDrawn
  *q++ = 0xFF; *q++ = 0xD0;                                                            // call rax
  q = gfxopt::EmitJmpAbs(q, row + sizeof(kRow));
  FlushInstructionCache(GetCurrentProcess(), mem, (size_t)(q - mem));
  const bool okRow = gfxopt::WriteJmpRel(row, mem, sizeof(kRow));
  const bool okNav = okRow && HookAt(nav, kNav, sizeof(kNav), (void *)&HookUINav, (void **)&g_trueUINav);
  if (okRow && okNav) log_line("[mouse] menu pointer installed\r\n");
  else { log_line("[mouse] WARNING: menu pointer hooks failed\r\n"); g_menuEnabled = false; }
}

static void Install(HMODULE hExe) {
  uint8_t *exe = (uint8_t *)hExe;
  InstallMenus(exe);
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
  if (!g_nameX || !g_nameY || strcmp(g_nameX, "look_around_xaxis") != 0 || strcmp(g_nameY, "look_around_yaxis") != 0 ||
      leaTarget(0x004064D6) != g_nameX || !HookAt(get, kHead, sizeof(kHead), (void *)&HookAxisGet, (void **)&g_trueAxisGet)) {
    log_line("[mouse] WARNING: unexpected input code; mouse look not installed\r\n");
    g_enabled = false;
    return;
  }
  log_line("[mouse] mouse look installed (sensitivity %.2f)\r\n", g_sens);
}

}  // namespace mouse
