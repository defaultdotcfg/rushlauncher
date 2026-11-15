// Output size and window mode.
//
// The game draws to a display of one size chosen at start (exe+0xAA9E0 builds it: 1920x1080 on the
// Xbox One; gfx_options.inc patches it to [Display] Width x Height). The swap chain has that size and is
// stretched over the window's client area, so the window always keeps the output's aspect ratio:
//  * Windowed: a framed window whose client area is the output size (shrunk to fit the work area);
//    resizing it keeps the aspect ratio.
//  * Borderless: a popup covering the monitor. An output of another aspect ratio than the monitor's is
//    centred with black bars (a black backdrop window right behind the game window).
//  * Fullscreen: the monitor is switched to the output size (when it has that mode), then like
//    borderless, topmost. The desktop's mode comes back while the game is in the background and when it
//    closes.
// Alt+Enter toggles between the windowed mode and the last full-screen one.
namespace display {

enum Mode { M_BORDERLESS, M_WINDOWED, M_FULLSCREEN, M_COUNT };
static const wchar_t *const kModeNames[M_COUNT] = {L"Borderless", L"Windowed", L"Fullscreen"};

static int g_w = 1920, g_h = 1080;           // output size
static volatile LONG g_mode = M_WINDOWED;
static LONG g_lastFull = M_BORDERLESS;      // where Alt+Enter goes from the windowed mode
static HWND g_backdrop = nullptr;
static RECT g_windowedRect = {};             // last framed window rect (empty: default size)
static wchar_t g_modeDevice[CCHDEVICENAME] = {};  // monitor switched by Fullscreen (empty: none)
static void (*g_onModeChanged)(LONG mode) = nullptr;  // gfx_options.inc: saves it, updates the menu row

static void Init() {
  wchar_t b[64] = {};
  int w = GetEnvironmentVariableW(L"ONRUSH_RES_WIDTH", b, 64) ? _wtoi(b) : 0;
  int h = GetEnvironmentVariableW(L"ONRUSH_RES_HEIGHT", b, 64) ? _wtoi(b) : 0;
  if (w >= 640 && h >= 360 && w <= 7680 && h <= 4320) { g_w = w & ~1; g_h = h & ~1; }
  g_mode = M_WINDOWED;
  if (GetEnvironmentVariableW(L"ONRUSH_WINDOW_MODE", b, 64))
    for (int m = 0; m < M_COUNT; ++m)
      if (_wcsicmp(b, kModeNames[m]) == 0) g_mode = m;
  if (g_mode != M_WINDOWED) g_lastFull = g_mode;
}

static bool SameRect(const RECT &a, const RECT &b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// Largest rect of the output's aspect ratio inside `area`, centred (the whole area when within 1%).
static RECT Fit(const RECT &area) {
  int aw = area.right - area.left, ah = area.bottom - area.top;
  int fw = aw, fh = (int)((long long)aw * g_h / g_w);
  if (fh > ah) { fh = ah; fw = (int)((long long)ah * g_w / g_h); }
  if (aw - fw <= aw / 100 && ah - fh <= ah / 100) return area;
  RECT r;
  r.left = area.left + (aw - fw) / 2; r.top = area.top + (ah - fh) / 2;
  r.right = r.left + fw; r.bottom = r.top + fh;
  return r;
}

static const DWORD kFramedStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE, kFramedEx = WS_EX_APPWINDOW;

// Framed window showing the output size (shrunk, same aspect, when it does not fit the work area),
// centred on the work area.
static RECT DefaultWindowedRect(const RECT &work) {
  RECT fr = {0, 0, 100, 100};
  AdjustWindowRectEx(&fr, kFramedStyle, FALSE, kFramedEx);
  int frameW = (fr.right - fr.left) - 100, frameH = (fr.bottom - fr.top) - 100;
  int availW = (work.right - work.left) - frameW, availH = (work.bottom - work.top) - frameH;
  int cw = g_w, ch = g_h;
  if (cw > availW || ch > availH) {
    double s = 0.9 * min((double)availW / cw, (double)availH / ch);
    cw = (int)(cw * s); ch = (int)(ch * s);
  }
  int w = cw + frameW, h = ch + frameH;
  RECT r;
  r.left = work.left + ((work.right - work.left) - w) / 2;
  r.top = work.top + ((work.bottom - work.top) - h) / 2;
  r.right = r.left + w; r.bottom = r.top + h;
  return r;
}

// Fullscreen: the monitor in the output size (same refresh rate when it has it).
static bool SwitchMonitor(HMONITOR mon) {
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  DEVMODEW cur = {};
  cur.dmSize = sizeof(cur);
  if (!GetMonitorInfoW(mon, &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return false;
  if ((int)cur.dmPelsWidth == g_w && (int)cur.dmPelsHeight == g_h) return true;
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  dm.dmPelsWidth = (DWORD)g_w; dm.dmPelsHeight = (DWORD)g_h; dm.dmDisplayFrequency = cur.dmDisplayFrequency;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
  LONG r = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
  if (r != DISP_CHANGE_SUCCESSFUL) {
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    r = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
  }
  log_line("[window] fullscreen: monitor %ls -> %dx%d %s\r\n", mi.szDevice, g_w, g_h,
           r == DISP_CHANGE_SUCCESSFUL ? "ok" : "not available (stays at the desktop's mode)");
  if (r != DISP_CHANGE_SUCCESSFUL) return false;
  wcscpy_s(g_modeDevice, mi.szDevice);
  return true;
}

static void RestoreMonitor() {
  if (!g_modeDevice[0]) return;
  ChangeDisplaySettingsExW(g_modeDevice, nullptr, nullptr, 0, nullptr);
  log_line("[window] monitor %ls back to the desktop's mode\r\n", g_modeDevice);
  g_modeDevice[0] = 0;
}

static bool Background() {   // test runs (ONRUSH_NO_FOCUS): never move, reorder or switch anything
  static int s = -1;
  if (s < 0) { char b[8] = {}; s = (GetEnvironmentVariableA("ONRUSH_NO_FOCUS", b, sizeof(b)) > 0 && b[0] == '1') ? 1 : 0; }
  return s == 1;
}

// Puts the window in `mode` (any thread: positions are applied by the window's own thread).
static void Place(HWND hwnd, LONG mode) {
  if (!hwnd) return;
  const UINT async = SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW;
  if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION && !IsIconic(hwnd)) GetWindowRect(hwnd, &g_windowedRect);
  if (Background()) {
    log_line("[window] mode %ls (background test run: window left as it is)\r\n", kModeNames[mode]);
    return;
  }
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (mode == M_FULLSCREEN) SwitchMonitor(mon);
  else RestoreMonitor();
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  if (mode == M_WINDOWED) {
    RECT r = g_windowedRect.right > g_windowedRect.left ? g_windowedRect : DefaultWindowedRect(mi.rcWork);
    SetWindowLongPtrW(hwnd, GWL_STYLE, kFramedStyle);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, kFramedEx);
    SetWindowPos(hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, async);
    if (g_backdrop) ShowWindowAsync(g_backdrop, SW_HIDE);
  } else {
    RECT g = Fit(mi.rcMonitor);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(hwnd, mode == M_FULLSCREEN ? HWND_TOPMOST : HWND_NOTOPMOST, g.left, g.top, g.right - g.left,
                 g.bottom - g.top, async);
    if (g_backdrop) {
      if (SameRect(g, mi.rcMonitor)) ShowWindowAsync(g_backdrop, SW_HIDE);
      else SetWindowPos(g_backdrop, hwnd, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  }
}

static void SetMode(HWND hwnd, LONG mode) {
  if (mode < 0 || mode >= M_COUNT) return;
  g_mode = mode;
  if (mode != M_WINDOWED) g_lastFull = mode;
  Place(hwnd, mode);
  if (g_onModeChanged) g_onModeChanged(mode);
  log_line("[window] mode -> %ls\r\n", kModeNames[mode]);
}

static void ToggleWindowed(HWND hwnd) { SetMode(hwnd, g_mode == M_WINDOWED ? g_lastFull : M_WINDOWED); }

static LRESULT CALLBACK BackdropProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(h, msg, wp, lp);
}

// On the window's thread, after the game window exists: the black bars window, then the start mode.
static void CreateBackdrop(HWND game) {
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = BackdropProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"OnRushBackdropClass";
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
  RegisterClassExW(&wc);
  g_backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, game,
                               nullptr, wc.hInstance, nullptr);
}

// WM_SIZING of the framed window: the client area keeps the output's aspect ratio.
static void OnSizing(HWND hwnd, WPARAM edge, RECT *r) {
  RECT fr = {0, 0, 100, 100};
  AdjustWindowRectEx(&fr, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  int frameW = (fr.right - fr.left) - 100, frameH = (fr.bottom - fr.top) - 100;
  int cw = max(160, (int)(r->right - r->left) - frameW), ch = max(90, (int)(r->bottom - r->top) - frameH);
  if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) ch = (int)((long long)cw * g_h / g_w);
  else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) cw = (int)((long long)ch * g_w / g_h);
  else if ((long long)cw * g_h > (long long)ch * g_w) ch = (int)((long long)cw * g_h / g_w);
  else cw = (int)((long long)ch * g_w / g_h);
  int w = cw + frameW, h = ch + frameH;
  if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - w;
  else r->right = r->left + w;
  if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - h;
  else r->bottom = r->top + h;
}

// Every Present: Fullscreen gives the monitor back while another window is in front (and minimizes the
// game), and takes it again when the game comes back.
static void Poll(HWND hwnd, bool foreground) {
  static bool s_away = false;
  static bool s_seen = false;   // not before the game window was in front once (the launcher may be at start)
  if (g_mode != M_FULLSCREEN || Background() || !hwnd) { s_away = false; return; }
  if (foreground) s_seen = true;
  if (!s_seen) return;
  if (!foreground && !s_away) {
    s_away = true;
    RestoreMonitor();
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    ShowWindowAsync(hwnd, SW_MINIMIZE);
  } else if (foreground && s_away && !IsIconic(hwnd)) {
    s_away = false;
    Place(hwnd, M_FULLSCREEN);
  }
}

}  // namespace display
