// "Quit" at the end of the main menu, with a confirmation (the Xbox game has no way to close itself).
//
// The main menu page (game/ui/mainmenu/mainmenupage.cpp) keeps its entries in a 0x4A8-byte object at
// page+0x2D8, filled by the page constructor (exe+0x346972..0x3469A7 without the debug menu):
//   +0x480 the list (type 5 table), +0x488 text keys, +0x490 page names, +0x498 enabled flags (bytes),
//   +0x4A0 count (8). The same object type serves other menus.
// - rows are drawn with the text key of their index (exe+0x38CA80);
// - "enabled" is exe+0x35EF50(obj, index): flags[index] when index < count (the main menu flags at
//   exe+0x14340A8 have 8 entries; the game flips the second one for the gear page);
// - the page's event handler exe+0x370E40(page, table, event, index) runs the shared exe+0x372D00
//   (event 3 = confirm -> opens page_names[index]) and then sets the description of the focused row
//   (ID_MENU_DESC_* for the first eight, an empty key past them).
// The constructor gets copies of the key and page tables with one more entry and a count of 9; the
// enabled check is replaced by the same code that also enables our entry; the event handler shows the
// confirmation for our entry and gives it its description.
namespace mainmenu {

static const int kGameCount = 8;
static const int kQuitIndex = kGameCount;
static const char **g_keys = nullptr;   // the game's 8 text keys + ours
static const char **g_pages = nullptr;  // the game's 8 page names + a placeholder (never opened)

static bool IsMainMenu(const uint8_t *obj) { return obj && *(const char ***)(obj + 0x488) == g_keys; }

// exe+0x35EF50, same logic, plus our entry
static uint8_t __cdecl Enabled(const uint8_t *obj, int index) {
  const uint8_t *flags = *(const uint8_t *const *)(obj + 0x498);
  if (!flags) return 1;
  if (index >= *(const int32_t *)(obj + 0x4A0)) return 0;
  if (index == kQuitIndex && IsMainMenu(obj)) return 1;
  return flags[(uint32_t)index];
}

// "ID_CANCEL" (the game's own key) also makes B / back answer the dialog with that button
static const char *const kButtons[] = {"ID_RL_QUIT", "ID_CANCEL"};
static void __cdecl QuitAnswered(void *, void *, int button) {
  log_line("[menu] quit dialog -> button %d\r\n", button);
  if (button == 0) QuitGameFromWindow("main menu QUIT");
}

typedef void(__cdecl *PFN_Event)(uint8_t *page, void *table, int event, int index);
static PFN_Event g_trueEvent = nullptr;

static void __cdecl HookEvent(uint8_t *page, void *table, int event, int index) {
  uint8_t *obj = *(uint8_t **)(page + 0x2D8);
  bool ours = IsMainMenu(obj) && index == kQuitIndex;
  if (ours && event == 3 && table == *(void **)(obj + 0x480)) {
    ((void (*)())((uint8_t *)gfxopt::g_exe + 0x001D7340))();  // the confirm sound
    if (!lang::ShowGameDialog("ID_RL_QUIT_TITLE", "ID_RL_QUIT_ASK", kButtons, 2, &QuitAnswered, 1))
      log_line("[menu] WARNING: no free dialog request; quit not asked\r\n");
    return;
  }
  g_trueEvent(page, table, event, index);
  if (ours) {
    void *desc = *(void **)(page + 0x1A88);
    typedef void (*PFN_SetText)(void *el, const char *key);
    if (desc) ((PFN_SetText)((uint8_t *)gfxopt::g_exe + 0x001EFE90))(desc, "ID_RL_QUIT_DESC");
  }
}

static void Install(HMODULE hExe) {
  uint8_t *exe = (uint8_t *)hExe;
  uint8_t *leaKeys = exe + 0x00346972, *leaPages = exe + 0x00346980, *count = exe + 0x0034699C;
  uint8_t *enabled = exe + 0x0035EF50, *event = exe + 0x00370E40;
  const char **gameKeys = (const char **)(exe + 0x014341B0), **gamePages = (const char **)(exe + 0x014341F0);
  static const uint8_t kCount[] = {0x41, 0xC7, 0x80, 0xA0, 0x04, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  static const uint8_t kEnabled[] = {0x4C, 0x8B, 0x81, 0x98, 0x04, 0x00, 0x00, 0x4D, 0x85, 0xC0, 0x74, 0x13,
                                     0x3B, 0x91, 0xA0, 0x04, 0x00, 0x00, 0x7D, 0x08, 0x8B, 0xC2, 0x42, 0x0F,
                                     0xB6, 0x04, 0x00, 0xC3, 0x32, 0xC0, 0xC3, 0xB0, 0x01, 0xC3};
  static const uint8_t kEvent[] = {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                   0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
  auto leaTarget = [](uint8_t *p) { return p + 7 + *(int32_t *)(p + 3); };
  bool ok = leaKeys[0] == 0x48 && leaKeys[1] == 0x8D && leaKeys[2] == 0x05 && leaTarget(leaKeys) == (uint8_t *)gameKeys &&
            leaPages[0] == 0x48 && leaPages[1] == 0x8D && leaPages[2] == 0x05 &&
            leaTarget(leaPages) == (uint8_t *)gamePages && memcmp(count, kCount, sizeof(kCount)) == 0 &&
            memcmp(enabled, kEnabled, sizeof(kEnabled)) == 0 && strcmp(gameKeys[0], "ID_PLAY") == 0 &&
            strcmp(gameKeys[kGameCount - 1], "ID_SETTINGS") == 0 && strcmp(gamePages[kGameCount - 1], "SettingsPage") == 0;
  if (!ok) {
    log_line("[menu] WARNING: unexpected main menu code; QUIT not added\r\n");
    return;
  }
  // both tables near the code (rip-relative lea)
  uint8_t *mem = (uint8_t *)AllocateNearAddress(leaKeys, 0x100);
  if (!mem) return;
  g_keys = (const char **)mem;
  g_pages = (const char **)(mem + 0x80);
  memcpy(g_keys, gameKeys, kGameCount * sizeof(char *));
  memcpy(g_pages, gamePages, kGameCount * sizeof(char *));
  g_keys[kQuitIndex] = "ID_RL_QUIT";
  g_pages[kQuitIndex] = "RushLauncherQuit";

  if (!mouse::HookAt(event, kEvent, sizeof(kEvent), (void *)&HookEvent, (void **)&g_trueEvent)) {
    log_line("[menu] WARNING: main menu event hook failed; QUIT not added\r\n");
    return;
  }
  uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};
  void *to = (void *)&Enabled;
  memcpy(jmp + 6, &to, 8);
  int32_t dk = (int32_t)((uint8_t *)g_keys - (leaKeys + 7)), dp = (int32_t)((uint8_t *)g_pages - (leaPages + 7));
  uint8_t n = kGameCount + 1;
  ok = gfxopt::WriteCode(enabled, jmp, sizeof(jmp)) && gfxopt::WriteCode(leaKeys + 3, &dk, 4) &&
       gfxopt::WriteCode(leaPages + 3, &dp, 4) && gfxopt::WriteCode(count + 7, &n, 1);
  log_line("[menu] main menu QUIT %s\r\n", ok ? "added" : "PATCH FAILED");
}

}  // namespace mainmenu
