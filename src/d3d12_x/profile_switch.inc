// Options > CHANGE USER switches to another RushLauncher profile.
//
// The settings page (SettingsPage, vtbl exe+0x1436250) lists its categories in the table at page+0x308;
// the table's events go to exe+0x3616B0(page, table, event, index). Category entries are 0x38 bytes at
// [page+0x2E0] with the category id at +0x30 (0 gameplay ... 5 credits, 6 change user). On confirm
// (event 3) the game asks the console for its account picker (exe+0xCBDF0 -> vtbl+0x20), which does
// nothing on PC. Here confirm on id 6 opens the game's dialog with one button per other profile.
//
// A profile is a folder in the save folder (see CsRoot); its unlock switches are launch arguments, so
// the switch is a restart: the chosen name goes to onrush.ini [Player] SwitchTo, the game ends with
// kExitProfileSwitch, and RushLauncher starts it again with that profile (ONRUSH_PROFILE_SWITCHED=1),
// which shows a notice once the main menu is up.
namespace profsw {

static const UINT kExitProfileSwitch = 0x52530050;  // same value in src/launcher/OnRushLauncher.cpp
static const int kMaxProfiles = 6;                  // buttons in the dialog besides CANCEL

typedef void(__cdecl *PFN_Event)(uint8_t *page, void *table, int event, int index);
static PFN_Event g_trueEvent = nullptr;

static std::string g_btnText[kMaxProfiles];   // "ID_RL_PN_<name>" (the localiser returns the name)
static const char *g_btnKeys[kMaxProfiles + 1];
static std::wstring g_names[kMaxProfiles];
static int g_count = 0;
static std::string g_doneKey;                  // "ID_RL_TX_<sentence>"

static std::string Utf8(const std::wstring &w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

// the save folder itself (CsRoot without the profile part)
static std::wstring SaveRoot() {
  std::wstring r = CsRoot(true);  // <save>\_machine
  size_t s = r.find_last_of(L'\\');
  return s == std::wstring::npos ? r : r.substr(0, s);
}

static void __cdecl Chosen(void *, void *, int button) {
  if (button < 0 || button >= g_count) { log_line("[profile] change user cancelled\r\n"); return; }
  const std::wstring &name = g_names[button];
  if (!gfxopt::g_ini[0] || !WritePrivateProfileStringW(L"Player", L"SwitchTo", name.c_str(), gfxopt::g_ini)) {
    log_line("[profile] WARNING: could not write onrush.ini; profile not switched\r\n");
    return;
  }
  log_line("[profile] switching to profile '%s': restarting through RushLauncher\r\n", Utf8(name).c_str());
  display::RestoreMonitor();
  TerminateProcess(GetCurrentProcess(), kExitProfileSwitch);
}

static void OpenPicker() {
  const bool launcher = GetEnvironmentVariableW(L"ONRUSH_LAUNCHER", nullptr, 0) > 0;   // set by RushLauncher
  log_line("[profile] change user chosen (launcher %s)\r\n", launcher ? "present" : "absent");
  if (!launcher) {
    lang::ShowNotice("ID_CHANGE_USER", "ID_RL_PROF_NOLAUNCHER");
    return;
  }
  std::vector<std::wstring> names;
  std::wstring cur = OrPlayerName();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((SaveRoot() + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
      std::wstring n = CsDecode(fd.cFileName);
      if (_wcsicmp(n.c_str(), cur.c_str()) != 0) names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(names.begin(), names.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  if (names.empty()) {
    lang::ShowNotice("ID_CHANGE_USER", "ID_RL_PROF_NONE");
    return;
  }
  if ((int)names.size() > kMaxProfiles) log_line("[profile] %d profiles, the dialog shows the first %d\r\n", (int)names.size(), kMaxProfiles);
  g_count = (int)std::min<size_t>(names.size(), kMaxProfiles);
  for (int i = 0; i < g_count; ++i) {
    g_names[i] = names[i];
    g_btnText[i] = "ID_RL_PN_" + Utf8(names[i]);
    g_btnKeys[i] = g_btnText[i].c_str();
  }
  g_btnKeys[g_count] = "ID_CANCEL";  // B / back answers with this one
  if (!lang::ShowGameDialog("ID_CHANGE_USER", "ID_RL_PROF_ASK", g_btnKeys, g_count + 1, &Chosen, 0))
    log_line("[profile] WARNING: no free dialog request; profile list not shown\r\n");
}

static void __cdecl HookEvent(uint8_t *page, void *table, int event, int index) {
  if (event == 3 && index >= 0 && table == *(void **)(page + 0x308)) {
    const uint8_t *items = *(const uint8_t *const *)(page + 0x2E0);
    if (items && *(const int32_t *)(items + (size_t)index * 0x38 + 0x30) == 6) {
      ((void (*)())((uint8_t *)gfxopt::g_exe + 0x001D7340))();  // the confirm sound
      OpenPicker();
      return;
    }
  }
  g_trueEvent(page, table, event, index);
}

// Main menu up after a switch: say which profile is in use (once).
static void OnMainMenu() {
  static volatile LONG s_done = 0;
  if (s_done || GetEnvironmentVariableW(L"ONRUSH_PROFILE_SWITCHED", nullptr, 0) == 0) return;
  if (InterlockedExchange(&s_done, 1)) return;
  const char *fmt = gfxopt::HookLocalise("ID_RL_PROF_DONE");
  char text[256];
  _snprintf_s(text, _TRUNCATE, fmt ? fmt : "%s", Utf8(OrPlayerName()).c_str());
  g_doneKey = std::string("ID_RL_TX_") + text;
  if (!lang::ShowGameDialog("ID_CHANGE_USER", g_doneKey.c_str(), lang::kOkButton, 1, nullptr))
    log_line("[profile] WARNING: no free dialog request; switch notice not shown\r\n");
}

static void Install(HMODULE hExe) {
  uint8_t *ev = (uint8_t *)hExe + 0x003616B0;
  // mov [rsp+8],rbx; push rdi; sub rsp,20h; movsxd rdi,r9d
  static const uint8_t kHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x63, 0xF9};
  bool ok = mouse::HookAt(ev, kHead, sizeof(kHead), (void *)&HookEvent, (void **)&g_trueEvent);
  log_line("[profile] change user -> RushLauncher profiles %s\r\n", ok ? "installed" : "NOT INSTALLED (unexpected code)");
}

}  // namespace profsw
