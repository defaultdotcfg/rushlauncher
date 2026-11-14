// Options page "Voices and text" (text language switched live, voice language from the next start).
//
// The game loads its texts once at boot (exe+0x2ED9CF): exe+0x22E4B0 loads
// code:localisation/xbox/<lang>.loc through exe+0x22E140(asset registry [exe+0x1681D38]), the language
// coming from GetUserDefaultLocaleName (kernelx honours ONRUSH_TEXT_LANG), and stores the database in
// [exe+0x1681D30], which every lookup (exe+0x22DFA0) reads. Right after, it caches the decimal separator
// (exe+0x22E430) and the time formats (exe+0x22E420 date, exe+0x22E4F0 the eight ID_TIME_* strings).
// Changing the language redoes exactly those steps with the new locale; the previous database stays
// loaded (texts already handed out keep pointing into it), screens built afterwards use the new one.
namespace lang {

static const wchar_t *const kLocale[] = {L"en-US", L"pt-BR", L"es-ES", L"fr-FR", L"de-DE", L"it-IT", L"pl-PL"};
static const char *const kTextKeys[] = {"ID_RL_LANG_EN", "ID_RL_LANG_PT", "ID_RL_LANG_ES", "ID_RL_LANG_FR",
                                        "ID_RL_LANG_DE", "ID_RL_LANG_IT", "ID_RL_LANG_PL"};
static const char *const kVoiceKeys[] = {"ID_RL_SAME_AS_TEXT", "ID_RL_LANG_EN", "ID_RL_LANG_PT", "ID_RL_LANG_ES",
                                         "ID_RL_LANG_FR", "ID_RL_LANG_DE", "ID_RL_LANG_IT", "ID_RL_LANG_PL"};
static const int kCount = 7;
enum : uint32_t { kIdText = 0x60, kIdVoice = 0x61 };

static volatile LONG g_text = 0;   // index into kLocale
static volatile LONG g_voice = 0;  // 0 = same as text, else kLocale[g_voice - 1]

static int LocaleIndex(const wchar_t *loc) {
  for (int i = 0; i < kCount; ++i)
    if (_wcsnicmp(loc, kLocale[i], 2) == 0) return i;
  return -1;
}

static void LoadChoices() {
  wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {};
  DWORD n = GetEnvironmentVariableW(L"ONRUSH_TEXT_LANG", loc, LOCALE_NAME_MAX_LENGTH);
  if (n == 0 || n >= LOCALE_NAME_MAX_LENGTH) GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
  int i = LocaleIndex(loc);
  g_text = i < 0 ? 0 : i;
  n = GetEnvironmentVariableW(L"ONRUSH_VOICE_LANG", loc, LOCALE_NAME_MAX_LENGTH);
  i = (n > 0 && n < LOCALE_NAME_MAX_LENGTH) ? LocaleIndex(loc) : -1;
  g_voice = i < 0 ? 0 : i + 1;
}

// Reloads the texts for the current GetUserDefaultLocaleName (see the header).
static bool ReloadTexts() {
  uint8_t *exe = (uint8_t *)gfxopt::g_exe;
  void *registry = *(void **)(exe + 0x01681D38);
  if (!registry) return false;
  typedef void *(*PFN_Load)(void *);
  void *db = ((PFN_Load)(exe + 0x0022E140))(registry);
  if (!db) return false;
  *(void *volatile *)(exe + 0x01681D30) = db;

  typedef const char *(*PFN_Loc)(const char *);
  PFN_Loc L = (PFN_Loc)(exe + 0x0022DFA0);
  ((void (*)())(exe + 0x0022E430))();                                  // decimal separator
  ((void (*)(const char *))(exe + 0x0022E420))(L("ID_TIME_DATE"));
  typedef void (*PFN_Times)(const char *, const char *, const char *, const char *, const char *, const char *,
                            const char *, const char *);
  ((PFN_Times)(exe + 0x0022E4F0))(L("ID_TIME_DAYS_HOURS"), L("ID_TIME_DAYS_HOURS_MINUTES"),
                                  L("ID_TIME_HRS_MINS_SEC"), L("ID_TIME_HRS_MINS"), L("ID_TIME_MINS_SEC"),
                                  L("ID_TIME_SEC"), L("ID_TIME_SEC_TENTH"), L("ID_TIME_MIN_SEC_TENTH"));
  return true;
}

static void OnText(float value, int) {
  LONG v = gfxopt::ListIndex(value, kCount);
  if (v == g_text) return;
  g_text = v;
  SetEnvironmentVariableW(L"ONRUSH_TEXT_LANG", kLocale[v]);
  gfxopt::s_lang = -1;
  bool ok = ReloadTexts();
  if (gfxopt::g_ini[0]) WritePrivateProfileStringW(L"Language", L"Text", kLocale[v], gfxopt::g_ini);
  log_line("[lang] text language -> %ls (%s)\r\n", kLocale[v], ok ? "reloaded" : "RELOAD FAILED");
}

// The game's dialog box (what "opponent left" uses): requests are 0x58-byte entries from the pool at
// [exe+0x1814B38] ({+0 entries, +0x10 free list data, +0x18 free list bytes}) shown by
// exe+0x1E5660(5, page name [exe+0x1432B80], request). Request: +0 button keys, +8 button count,
// +0x0C focused button, +0x10 callback(ctx, ?, button), +0x18 ctx, +0x28/+0x30 button enabled callback/ctx,
// +0x40 title key, +0x48 text key, +0x50 [exe+0x1814B40], +0x54 1. The page (exe+0x3650B0) lets B/back pick
// the button whose key is "ID_CANCEL" (exe+0x36FE50); without one, back does nothing.
typedef void(__cdecl *PFN_DialogDone)(void *ctx, void *, int button);
static void __cdecl DialogClosed(void *, void *, int) {}

// Opens the game's dialog box; `buttons` are text keys, `done` gets the index of the chosen one.
static bool ShowGameDialog(const char *title, const char *text, const char *const *buttons, int count,
                           PFN_DialogDone done, int focus = 0) {
  uint8_t *exe = (uint8_t *)gfxopt::g_exe;
  uint8_t *pool = *(uint8_t **)(exe + 0x01814B38);
  if (!pool || !*(uint8_t **)pool) return false;
  uint8_t *freeList = *(uint8_t **)(pool + 0x10);
  uint64_t &freeBytes = *(uint64_t *)(pool + 0x18);
  if (!freeList || freeBytes < 4) return false;
  uint32_t idx = *(uint32_t *)(freeList + freeBytes - 4);
  freeBytes -= 4;
  uint8_t *req = *(uint8_t **)pool + (uint64_t)idx * 0x58;
  memset(req, 0, 0x58);
  *(const char *const **)(req + 0x00) = buttons;
  *(uint32_t *)(req + 0x08) = (uint32_t)count;
  *(uint32_t *)(req + 0x0C) = (uint32_t)focus;
  *(void **)(req + 0x10) = (void *)(done ? done : &DialogClosed);
  *(const char **)(req + 0x40) = title;
  *(const char **)(req + 0x48) = text;
  *(uint32_t *)(req + 0x50) = *(uint32_t *)(exe + 0x01814B40);
  *(uint8_t *)(req + 0x54) = 1;
  typedef void (*PFN_Show)(int, void *, void *);
  ((PFN_Show)(exe + 0x001E5660))(5, *(void **)(exe + 0x01432B80), req);
  return true;
}

static const char *const kOkButton[] = {"ID_OK"};
static void ShowVoiceNotice() { ShowGameDialog("ID_RL_VOICE", "ID_RL_VOICE_NOTICE", kOkButton, 1, nullptr); }

static void OnVoice(float value, int) {
  LONG v = gfxopt::ListIndex(value, kCount + 1);
  if (v == g_voice) return;
  g_voice = v;
  if (gfxopt::g_ini[0]) WritePrivateProfileStringW(L"Language", L"Voice", v ? kLocale[v - 1] : L"", gfxopt::g_ini);
  ShowVoiceNotice();
  log_line("[lang] voice language -> %ls (next start)\r\n", v ? kLocale[v - 1] : L"same as text");
}

// ---- the "Voices and text" page --------------------------------------------------------------
// The options screen keeps its pages in a vector at +0x2D8 ({vtbl, data, byte size, allocator if,
// allocator, capacity}); each page is the same vector (its rows, 0x40 bytes each) plus an id at +0x30:
// 0 gameplay, 1 controller hints, 3 audio, 4 graphics, 6 change user, 5 credits, then 7 (not listed:
// the list shows size - 1 entries). The left list names a page with the key table at exe+0x1433008
// indexed by id (exe+0x38CA2D). A new page (id 8) is inserted after graphics before the builder runs
// (exe+0x39470E), its name comes from a copy of that table with one more key, and its rows are added
// with the gameplay rows (exe+0x3550A4).
static const uint32_t kPageId = 8;
static const size_t kPageSize = 0x38;
static const char *g_pageNames[9] = {};

static uint8_t *FindPage(uint8_t *screen, uint32_t id) {
  uint8_t *d = *(uint8_t **)(screen + 0x2E0);
  uint64_t n = *(uint64_t *)(screen + 0x2E8) / kPageSize;
  for (uint64_t i = 0; d && i < n; ++i)
    if (*(uint32_t *)(d + i * kPageSize + 0x30) == id) return d + i * kPageSize;
  return nullptr;
}

static void __cdecl InsertPage(uint8_t *screen) {
  if (FindPage(screen, kPageId)) return;
  uint8_t *vec = screen + 0x2D8;
  uint64_t size = *(uint64_t *)(vec + 0x10);
  if (!*(uint8_t **)(vec + 8) || size < kPageSize || size % kPageSize) return;
  typedef void (*PFN_Reserve)(void *vec, uint64_t bytes, uint8_t exact);
  ((PFN_Reserve)((uint8_t *)gfxopt::g_exe + 0x0007D020))(vec, size + kPageSize, 0);
  uint8_t *d = *(uint8_t **)(vec + 8);
  if (!d || *(uint64_t *)(vec + 0x28) < size + kPageSize) return;
  uint64_t n = size / kPageSize, at = n - 1;  // before the hidden last page by default
  for (uint64_t i = 0; i < n; ++i)
    if (*(uint32_t *)(d + i * kPageSize + 0x30) == 4) at = i + 1;  // right after graphics
  memmove(d + (at + 1) * kPageSize, d + at * kPageSize, (size_t)((n - at) * kPageSize));
  uint8_t *pg = d + at * kPageSize;
  memcpy(pg, d, kPageSize);           // same vector type and allocator as the first page
  *(uint64_t *)(pg + 0x08) = 0;       // no rows yet
  *(uint64_t *)(pg + 0x10) = 0;
  *(uint64_t *)(pg + 0x28) = 0;
  *(uint64_t *)(pg + 0x30) = kPageId;
  *(uint64_t *)(vec + 0x10) = size + kPageSize;
}

static void __cdecl AddRows(void *, uint8_t *screen) {
  uint8_t *page = FindPage(screen, kPageId);
  if (!page) return;
  typedef void (*PFN_AddItem)(void *page, const gfxopt::MenuItem *item);
  PFN_AddItem add = (PFN_AddItem)((uint8_t *)gfxopt::g_exe + 0x00340DB0);
  gfxopt::MenuItem it = {};
  it.type = 6;
  it.invoker = (uint8_t *)gfxopt::g_exe + 0x0035E960;
  it.name = "ID_RL_LANG"; it.callback = (void *)&OnText; it.id = kIdText;
  it.values = kTextKeys; it.count = kCount; it.value = (float)g_text;
  add(page, &it);
  it.name = "ID_RL_VOICE"; it.callback = (void *)&OnVoice; it.id = kIdVoice;
  it.values = kVoiceKeys; it.count = kCount + 1; it.value = (float)g_voice;
  add(page, &it);
}

static void Install(HMODULE hExe) {
  uint8_t *exe = (uint8_t *)hExe;
  uint8_t *site = exe + 0x003550A4;   // builder, after the gameplay rows: mov rbx, [rsi+0x2E0]
  uint8_t *entry = exe + 0x0039470E;  // jmp builder (exe+0x354CB0), rbx = rcx = options screen
  uint8_t *name = exe + 0x0038CA2D;   // lea rcx, [page name keys]
  static const uint8_t kSite[] = {0x48, 0x8B, 0x9E, 0xE0, 0x02, 0x00, 0x00};
  uint8_t *names = exe + 0x01433008;
  bool ok = memcmp(site, kSite, sizeof(kSite)) == 0 && entry[0] == 0xE9 &&
            entry + 5 + *(int32_t *)(entry + 1) == exe + 0x00354CB0 && name[0] == 0x48 && name[1] == 0x8D &&
            name[2] == 0x0D && name + 7 + *(int32_t *)(name + 3) == names;
  if (!ok) {
    log_line("[lang] WARNING: unexpected options screen code; voices and text page not added\r\n");
    return;
  }
  LoadChoices();
  uint8_t *mem = (uint8_t *)AllocateNearAddress(site, 0x200);
  if (!mem) return;
  DWORD old = 0;
  VirtualProtect(mem, 0x200, PAGE_EXECUTE_READWRITE, &old);

  // page name keys: the game's eight + ours
  const char **tbl = (const char **)(mem + 0x180);
  memcpy(tbl, names, 8 * sizeof(char *));
  tbl[kPageId] = "ID_RL_VOICE_TEXT";

  uint8_t *q = mem;
  // rows: sub rsp,20h ; mov rcx,rbx ; mov rdx,rsi ; call AddRows ; add rsp,20h ; mov rbx,[rsi+2E0h] ; jmp back
  uint8_t *stubRows = q;
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x20;
  *q++ = 0x48; *q++ = 0x89; *q++ = 0xD9;
  *q++ = 0x48; *q++ = 0x89; *q++ = 0xF2;
  *q++ = 0x48; *q++ = 0xB8; { void *f = (void *)&AddRows; memcpy(q, &f, 8); q += 8; }
  *q++ = 0xFF; *q++ = 0xD0;
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x20;
  memcpy(q, kSite, sizeof(kSite)); q += sizeof(kSite);
  q = gfxopt::EmitJmpAbs(q, site + sizeof(kSite));
  // entry (stack as at a call target): push rcx ; sub rsp,20h ; call InsertPage ; add rsp,20h ; pop rcx ; jmp builder
  uint8_t *stubEntry = q;
  *q++ = 0x51;
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x20;
  *q++ = 0x48; *q++ = 0xB8; { void *f = (void *)&InsertPage; memcpy(q, &f, 8); q += 8; }
  *q++ = 0xFF; *q++ = 0xD0;
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x20;
  *q++ = 0x59;
  q = gfxopt::EmitJmpAbs(q, exe + 0x00354CB0);
  FlushInstructionCache(GetCurrentProcess(), mem, 0x200);

  int32_t rel = (int32_t)((uint8_t *)tbl - (name + 7));
  bool done = gfxopt::WriteJmpRel(site, stubRows, sizeof(kSite)) && gfxopt::WriteJmpRel(entry, stubEntry, 5) &&
              gfxopt::WriteCode(name + 3, &rel, 4);
  if (done)
    log_line("[lang] voices and text page added (text %ls, voice %ls)\r\n", kLocale[g_text],
             g_voice ? kLocale[g_voice - 1] : L"same as text");
}

}  // namespace lang
