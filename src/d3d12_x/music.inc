// The player's own playlist in place of the game's music.
//
// RushLauncher passes the chosen playlist (a UTF-8 .m3u8 from its Playlist tab):
//   ONRUSH_MUSIC_PLAYLIST = full path of the .m3u8 (one file per line; "#ONRUSH:<title>\t<artist>" names the
//                           next file, "#ONRUSH-OFF" marks it as switched off)
//   ONRUSH_MUSIC_ENABLED  = 0 to start with the game's own music (default 1)
//   ONRUSH_MUSIC_SHUFFLE  = 1 to shuffle
//
// The tracks replace the game music where it would play, following the game:
//  * The game's music manager sets a music state for every screen (exe+0x332010 SetMusicState(mgr, state),
//    names Music_State_* at exe+0x1431860): start page, menus, race and its phases, replay, credits play
//    music; the logo, loading screens and "no music" (16) do not. A track plays while the state has music
//    and pauses otherwise; after a stop the next track starts (a new screen gets a new song, like the game).
//  * The pause menu ducks the music ("Music_Duck" through the audio system, exe+0x1F5590): the track pauses
//    and resumes where it was.
//  * The game's music volume (option slider, RTPC Music_Volume) sets the track volume; the game's own music
//    is held at 0 while the playlist plays (DetourSetRtpc) and comes back when it is switched off.
// Playback: Media Foundation media engine (audio only; mp3, m4a/aac, wma, wav, flac, ... what Windows decodes).
//
// Options: the settings page gets a PLAYLIST category (main menu) and the pause menu's options list gets the
// same rows at its end: MUSIC (the game's / playlist), ORDER (in order / shuffle) and one ON/OFF row per song.
// The order goes to onrush.ini [Music] Shuffle, the music choice to [Music] Enabled, a song's switch to the
// .m3u8 ("#ONRUSH-OFF"), so RushLauncher shows the same.
#include <mfapi.h>
#include <mfmediaengine.h>
#include <random>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static void ResendMusicVolume(float gameValue);  // onrush_d3d12_x.cpp

namespace music {

static uint8_t *g_exe = nullptr;

// ---- playlist -----------------------------------------------------------------------------------
struct Track {
  std::wstring path;
  std::string row;     // "ID_RL_TX_<TITLE>": the localiser hook returns the title (gfx_options.inc)
  bool exists = false; // file found (rows and playback only for these)
  volatile bool on = true;
};
static std::vector<Track> g_tracks;        // in file order; fixed after Install
static wchar_t g_m3u[MAX_PATH] = {};
static volatile LONG g_enabled = 1;        // 1 = the playlist replaces the game music
static volatile LONG g_shuffle = 0;
static SRWLOCK g_fileLock = SRWLOCK_INIT;

static std::string Utf8(const std::wstring &w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
static std::wstring Wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n > 0 ? n : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

static bool ReadLines(const wchar_t *path, std::vector<std::string> &lines) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(f, nullptr), got = 0;
  std::string data(size, '\0');
  ReadFile(f, &data[0], size, &got, nullptr);
  CloseHandle(f);
  data.resize(got);
  size_t pos = data.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (pos < data.size()) {
    size_t e = data.find('\n', pos);
    std::string line = data.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? data.size() : e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    lines.push_back(line);
  }
  return true;
}

static void LoadPlaylist() {
  std::vector<std::string> lines;
  if (!ReadLines(g_m3u, lines)) return;
  std::wstring title;
  bool off = false;
  for (const std::string &line : lines) {
    if (line.empty()) continue;
    if (line == "#ONRUSH-OFF") { off = true; continue; }
    if (line.rfind("#ONRUSH:", 0) == 0) { std::wstring m = Wide(line.substr(8)); title = m.substr(0, m.find(L'\t')); continue; }
    if (line[0] == '#') continue;
    Track t;
    t.path = Wide(line);
    t.exists = GetFileAttributesW(t.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    t.on = !off;
    if (title.empty()) {  // no metadata line: the file name without extension
      size_t s = t.path.find_last_of(L"\\/"), d = t.path.find_last_of(L'.');
      title = t.path.substr(s == std::wstring::npos ? 0 : s + 1);
      if (d != std::wstring::npos && d > (s == std::wstring::npos ? 0 : s)) title.resize(title.size() - (t.path.size() - d));
    }
    // upper case like the game's own texts
    std::wstring up(title.size() + 1, L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, title.c_str(), (int)title.size(), &up[0], (int)up.size(), nullptr, nullptr, 0);
    up.resize(n > 0 ? n : 0);
    t.row = "ID_RL_TX_" + Utf8(n > 0 ? up : title);
    g_tracks.push_back(t);
    title.clear();
    off = false;
  }
}

// Writes the songs' switches back into the .m3u8 (the other lines stay as they are).
static void SaveSwitches() {
  AcquireSRWLockExclusive(&g_fileLock);
  std::vector<std::string> lines;
  if (ReadLines(g_m3u, lines)) {
    std::string out;
    size_t k = 0;
    for (const std::string &line : lines) {
      if (line == "#ONRUSH-OFF") continue;
      if (!line.empty() && line[0] != '#') {
        if (k < g_tracks.size() && !g_tracks[k].on) out += "#ONRUSH-OFF\r\n";
        ++k;
      }
      out += line + "\r\n";
    }
    HANDLE f = CreateFileW(g_m3u, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD wr = 0;
      WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
      CloseHandle(f);
    }
  }
  ReleaseSRWLockExclusive(&g_fileLock);
}

static int PlayableCount() {
  int n = 0;
  for (const Track &t : g_tracks) n += t.exists && t.on;
  return n;
}

// ---- what the game is doing -------------------------------------------------------------------
enum { kStateCount = 17, kStateNone = 16 };
// Music states with music. 0 logo, 1 start page, 2 menu, 3 first-play loading, 5 first-play race, 6 loading,
// 8 race, 14 replay, 15 credits; 4, 7 and 9..13 are race phases without a Wwise state of their own (race
// intro, round end, rush, takedown, ...); 16 = no music.
static const bool kStateMusic[kStateCount] = {false, true, true, false, true, true, false, true, true,
                                              true, true, true, true, true, true, true, false};
static volatile LONG g_state = kStateNone;
static volatile LONG g_ducked = 0;
static volatile float g_gameVolume = 1.0f;   // the game's Music_Volume (0..1)
static HANDLE g_wake = nullptr;              // player thread: something changed
static ULONGLONG g_t0 = 0;

static bool Active() { return g_enabled && PlayableCount() > 0; }
static bool Wanted() {
  LONG s = g_state;
  return Active() && s >= 0 && s < kStateCount && kStateMusic[s] && !g_ducked;
}

static float OnGameMusicVolume(float value) {
  g_gameVolume = value;
  if (g_wake) SetEvent(g_wake);
  return Active() ? 0.0f : value;
}

// The playlist took over or handed back the music: send the game music volume again.
static void ApplyGameMusic() {
  ResendMusicVolume(g_gameVolume);
  if (g_wake) SetEvent(g_wake);
}

typedef void (*PFN_SetState)(uint8_t *mgr, int state);
static PFN_SetState g_trueSetState = nullptr;
static void HookSetState(uint8_t *mgr, int state) {
  if (state != g_state) {
    log_line("[music] game music state %d (%.1f s)\r\n", state, (GetTickCount64() - g_t0) / 1000.0);
    g_state = state;
    if (g_wake) SetEvent(g_wake);
  }
  g_trueSetState(mgr, state);
}

static void SetPaused(LONG on, const char *why) {
  if (InterlockedExchange(&g_ducked, on) != on) {
    log_line("[music] %s: %s (%.1f s)\r\n", why, on ? "paused" : "resumed", (GetTickCount64() - g_t0) / 1000.0);
    if (g_wake) SetEvent(g_wake);
  }
}

// The audio calls below are the game's thin wrappers around its audio system [exe+0x1675920]; they are
// rewritten whole here (the originals load the system with mov rcx,[rip+x], which cannot be relocated).
static void *AudioSys() { return *(void **)(g_exe + 0x01675920); }

// exe+0x1F5590(name, arg): slot 0x140, a named mix. Pages ducking the music call it with "Music_Duck" and a
// callback when they open, with 0 when they close.
static void HookAudioNamed(const char *name, void *arg) {
  if (name && strcmp(name, "Music_Duck") == 0) SetPaused(arg != nullptr, "music ducked");
  typedef void (*PFN)(void *, const char *, void *);
  void *sys = AudioSys();
  ((PFN)(*(void ***)sys)[0x140 / 8])(sys, name, arg);
}

// Sound events: exe+0x1F3D00 GetEvent(gameObj, name) -> handle (slot 0x50) and exe+0x1F51D0
// PostEvent(handle, flags*) (slot 0xD8). The pause menu posts "Pause_Enter" / "Pause_Exit" (replays their
// "Replay_" twins); the others are logged to follow the race's end.
enum { E_PAUSE_ENTER, E_PAUSE_EXIT, E_REPLAY_PAUSE_ENTER, E_REPLAY_PAUSE_EXIT, E_RESULTS, E_END_MATCH, E_EXIT_RACE, E_START_RACE,
       E_COUNT };
static const char *const kEventNames[E_COUNT] = {"Pause_Enter", "Pause_Exit", "Replay_Pause_Enter", "Replay_Pause_Exit",
                                                 "Post_Race_Results_Enter", "End_of_Match", "Exit_Race", "Start_Race"};
static volatile uint32_t g_eventHandle[E_COUNT][4];   // handles learnt from GetEvent (one per game object)

static uint32_t HookGetEvent(int gameObj, const char *name) {
  typedef uint32_t (*PFN)(void *, int, const char *);
  void *sys = AudioSys();
  uint32_t h = ((PFN)(*(void ***)sys)[0x50 / 8])(sys, gameObj, name);
  if (name)
    for (int e = 0; e < E_COUNT; ++e)
      if (strcmp(name, kEventNames[e]) == 0)
        for (volatile uint32_t &slot : g_eventHandle[e]) {
          if (slot == h) break;
          if (slot == 0xFFFFFFFFu) { slot = h; break; }
        }
  return h;
}

static uint64_t HookPostEvent(uint32_t handle, const uint8_t *flags) {
  for (int e = 0; e < E_COUNT; ++e)
    for (uint32_t slot : g_eventHandle[e]) {
      if (slot != handle || slot == 0xFFFFFFFFu) continue;
      if (e <= E_REPLAY_PAUSE_EXIT) SetPaused(e == E_PAUSE_ENTER || e == E_REPLAY_PAUSE_ENTER, "game paused");
      else log_line("[music] game event %s (%.1f s)\r\n", kEventNames[e], (GetTickCount64() - g_t0) / 1000.0);
      e = E_COUNT;
      break;
    }
  uint8_t copy = *flags;   // what the original passes: a copy of the flag byte
  typedef uint64_t (*PFN)(void *, uint32_t, uint8_t *);
  void *sys = AudioSys();
  return ((PFN)(*(void ***)sys)[0xD8 / 8])(sys, handle, &copy);
}

// Replaces a wrapper that starts with `head` (and loads the audio system from exe+0x1675920 at `ripAt`).
static bool ReplaceWrapper(uint32_t rva, const uint8_t *head, size_t len, size_t ripAt, void *to) {
  uint8_t *p = g_exe + rva;
  if (memcmp(p, head, len) != 0 || p + ripAt + 4 + *(int32_t *)(p + ripAt) != g_exe + 0x01675920) return false;
  uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};
  memcpy(jmp + 6, &to, 8);
  return gfxopt::WriteCode(p, jmp, sizeof(jmp));
}

// ---- player -------------------------------------------------------------------------------------
struct EngineNotify : IMFMediaEngineNotify {
  volatile LONG refs = 1;
  volatile LONG ended = 0, failed = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **pp) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(IMFMediaEngineNotify)) { *pp = this; AddRef(); return S_OK; }
    *pp = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE EventNotify(DWORD ev, DWORD_PTR, DWORD) override {
    if (ev == MF_MEDIA_ENGINE_EVENT_ENDED) ended = 1;
    else if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) failed = 1;
    else return S_OK;
    if (g_wake) SetEvent(g_wake);
    return S_OK;
  }
};

// Play order: a list of track indices; shuffle = a new random order every time the list is used up.
static std::vector<int> g_order;
static int g_pos = -1;
static int g_current = -1;                   // track in the engine (-1 none)
static LONG g_orderShuffle = -1;
static std::mt19937 g_rng((unsigned)GetTickCount64());

static void BuildOrder(bool keepCurrent) {
  g_order.clear();
  for (int i = 0; i < (int)g_tracks.size(); ++i) g_order.push_back(i);
  g_orderShuffle = g_shuffle;
  if (g_shuffle) std::shuffle(g_order.begin(), g_order.end(), g_rng);
  g_pos = -1;
  if (keepCurrent && g_current >= 0) {
    for (int k = 0; k < (int)g_order.size(); ++k)
      if (g_order[k] == g_current) {
        if (g_shuffle) std::swap(g_order[k], g_order[0]), g_pos = 0;  // continue from the current one
        else g_pos = k;
      }
  }
}

// Next playable track after the current position (-1 if none is switched on).
static int NextTrack() {
  if (g_orderShuffle != g_shuffle || g_order.size() != g_tracks.size()) BuildOrder(true);
  for (size_t step = 0; step < g_order.size(); ++step) {
    if (++g_pos >= (int)g_order.size()) {
      if (g_shuffle) { std::shuffle(g_order.begin(), g_order.end(), g_rng); if (g_order.size() > 1 && g_order[0] == g_current) std::swap(g_order[0], g_order.back()); }
      g_pos = 0;
    }
    const Track &t = g_tracks[g_order[g_pos]];
    if (t.exists && t.on) return g_order[g_pos];
  }
  return -1;
}

static DWORD WINAPI PlayerThread(void *) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { log_line("[music] MFStartup failed\r\n"); return 0; }
  IMFMediaEngineClassFactory *factory = nullptr;
  IMFAttributes *attr = nullptr;
  IMFMediaEngine *engine = nullptr;
  EngineNotify *notify = new EngineNotify();
  HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 2);
  if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
  if (SUCCEEDED(hr)) attr->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, 7 /* AudioCategory_GameMedia */);
  if (SUCCEEDED(hr)) hr = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attr, &engine);
  if (FAILED(hr) || !engine) { log_line("[music] media engine creation failed (0x%08X)\r\n", (unsigned)hr); return 0; }

  const DWORD kTick = 20;
  const float kFadeIn = 0.6f, kFadeOut = 0.35f;   // seconds
  float level = 0.0f;                             // current engine volume
  bool playing = false;                           // engine running (Play issued, not paused)
  bool freshNext = true;                          // the next start begins a new track
  int failsInRow = 0;
  for (;;) {
    WaitForSingleObject(g_wake, kTick);
    const bool want = Wanted();
    float gain = g_gameVolume;   // the game's music volume, as the original music would play
    if (gain < 0.0f) gain = 0.0f; else if (gain > 1.0f) gain = 1.0f;

    // the track in the engine ended, failed or was switched off: on to the next one
    bool next = false;
    if (InterlockedExchange(&notify->ended, 0)) { next = true; failsInRow = 0; }
    if (InterlockedExchange(&notify->failed, 0)) {
      if (g_current >= 0) log_line("[music] could not play %ls\r\n", g_tracks[g_current].path.c_str());
      next = true;
      if (++failsInRow > (int)g_tracks.size()) { log_line("[music] no playable track\r\n"); failsInRow = 0; Sleep(2000); }
    }
    if (g_current >= 0 && !g_tracks[g_current].on) next = true;
    if (next) { engine->Pause(); engine->SetVolume(0.0); playing = false; g_current = -1; level = 0.0f; }

    if (want) {
      if (g_current < 0 || (!playing && freshNext)) {
        int t = NextTrack();
        if (t >= 0) {
          BSTR url = SysAllocString(g_tracks[t].path.c_str());
          engine->SetSource(url);
          SysFreeString(url);
          g_current = t;
          level = 0.0f;
          engine->SetVolume(0.0);
          log_line("[music] playing %ls\r\n", g_tracks[t].path.c_str());
        }
        freshNext = false;
      }
      if (g_current >= 0 && !playing) { engine->Play(); playing = true; }
      float step = gain * kTick / (1000.0f * kFadeIn);
      level = level < gain ? (std::min)(gain, level + (std::max)(step, 0.002f)) : gain;
      engine->SetVolume(level);
    } else if (playing) {
      float step = (std::max)(level, 0.05f) * kTick / (1000.0f * kFadeOut);
      level = (std::max)(0.0f, level - step);
      engine->SetVolume(level);
      if (level <= 0.0f) {
        engine->Pause();
        playing = false;
        log_line("[music] track paused\r\n");
        // a pause keeps the track; leaving the music states (loading, results, ...) starts a new one next time
        if (!g_ducked) freshNext = true;
      }
    }
  }
}

// ---- options rows -------------------------------------------------------------------------------
using gfxopt::MenuItem;
static const char *const kMusicKeys[] = {"ID_RL_PL_GAME", "ID_RL_PL_MINE"};
static const char *const kOrderKeys[] = {"ID_RL_PL_SEQ", "ID_RL_PL_SHUF"};
static const char *const kSongKeys[] = {"ID_RL_OFF_F", "ID_RL_ON_F"};
enum : uint32_t { kIdMusic = 0x70, kIdOrder = 0x71, kIdSong0 = 0x80 };
static const int kMaxRows = 300;
static uint8_t *g_songThunks = nullptr;      // per-song callbacks: mov r8d, index ; jmp OnSong
static uint8_t *volatile g_pages[8] = {};
static volatile LONG g_pageNext = 0;

static void SetRowValue(uint32_t id, float value) {  // every page our rows were added to (both menus)
  void *invoker = g_exe + 0x0035E960;
  for (uint8_t *page : g_pages) {
    if (!page) continue;
    uint8_t *data = *(uint8_t **)(page + 8);
    size_t n = *(size_t *)(page + 0x10) / sizeof(MenuItem);
    for (size_t i = 0; i < n; ++i) {
      MenuItem *it = (MenuItem *)(data + i * sizeof(MenuItem));
      if (it->id == id && it->invoker == invoker) it->value = value;
    }
  }
}

static void OnMusic(float value, int) {
  LONG v = gfxopt::ListIndex(value, 2);
  bool wasActive = Active();
  g_enabled = v;
  SetRowValue(kIdMusic, (float)v);
  if (gfxopt::g_ini[0]) WritePrivateProfileStringW(L"Music", L"Enabled", v ? L"1" : L"0", gfxopt::g_ini);
  log_line("[music] %s music\r\n", v ? "playlist" : "game");
  if (wasActive != Active()) ApplyGameMusic();
}
static void OnOrder(float value, int) {
  LONG v = gfxopt::ListIndex(value, 2);
  g_shuffle = v;
  SetRowValue(kIdOrder, (float)v);
  if (gfxopt::g_ini[0]) WritePrivateProfileStringW(L"Music", L"Shuffle", v ? L"1" : L"0", gfxopt::g_ini);
  if (g_wake) SetEvent(g_wake);
}
static void OnSong(float value, int, int index) {
  if (index < 0 || index >= (int)g_tracks.size()) return;
  bool on = gfxopt::ListIndex(value, 2) != 0;
  bool wasActive = Active();
  g_tracks[index].on = on;
  SetRowValue(kIdSong0 + index, on ? 1.0f : 0.0f);
  SaveSwitches();
  if (wasActive != Active()) ApplyGameMusic();
  else if (g_wake) SetEvent(g_wake);
}

static void AddRows(uint8_t *page) {
  bool known = false;
  for (uint8_t *p : g_pages) known |= p == page;
  if (!known) g_pages[(InterlockedIncrement(&g_pageNext) - 1) & 7] = page;
  typedef void (*PFN_AddItem)(void *page, const MenuItem *item);
  PFN_AddItem add = (PFN_AddItem)(g_exe + 0x00340DB0);
  MenuItem it = {};
  it.type = 6;
  it.invoker = g_exe + 0x0035E960;
  it.count = 2;
  it.name = "ID_RL_PL_MUSIC"; it.callback = (void *)&OnMusic; it.id = kIdMusic; it.values = kMusicKeys; it.value = (float)g_enabled;
  add(page, &it);
  it.name = "ID_RL_PL_ORDER"; it.callback = (void *)&OnOrder; it.id = kIdOrder; it.values = kOrderKeys; it.value = (float)g_shuffle;
  add(page, &it);
  int rows = 0;
  for (int i = 0; i < (int)g_tracks.size() && i < kMaxRows; ++i) {
    if (!g_tracks[i].exists) continue;
    it.name = g_tracks[i].row.c_str(); it.callback = g_songThunks + i * 32; it.id = kIdSong0 + i;
    it.values = kSongKeys; it.value = g_tracks[i].on ? 1.0f : 0.0f;
    add(page, &it);
    ++rows;
  }
  log_line("[music] playlist rows added (%d songs)\r\n", rows);
}

// ---- settings page: the PLAYLIST category -------------------------------------------------------
// Categories are 0x38-byte entries in the vector at page+0x2D8 (data +0x2E0, byte size +0x2E8; an entry is
// the category's item vector with its id at +0x30), made by the SettingsPage constructor (exe+0x348120):
// 0 gameplay, 1 controller hints, 3 audio, 4 graphics, 6 change user, 5 credits, then 7 (not listed: the
// list shows size - 1 entries; the pause menu's single list), and 8 "voices and text" (language.inc). The
// list names a category through a key table indexed by id, read at exe+0x38CA2D; language.inc already
// points it at a copy with key 8, and this one copies that table again with key 9 added.
enum { kCatPause = 7, kCatVoiceText = 8, kCatPlaylist = 9 };
static const char **g_catTable = nullptr;    // near the exe (the game reads it with lea [rip+x])

static uint8_t *FindCategory(uint8_t *page, int id) {
  uint8_t *data = *(uint8_t **)(page + 0x2E0);
  size_t n = *(size_t *)(page + 0x2E8) / 0x38;
  for (size_t i = 0; data && i < n; ++i)
    if (*(int32_t *)(data + i * 0x38 + 0x30) == id) return data + i * 0x38;
  return nullptr;
}

// Appends the category with the game's own vector code, then moves it after "voices and text" (or after
// graphics), keeping the hidden pause list last.
static void InsertCategory(uint8_t *page) {
  if (!g_catTable || FindCategory(page, kCatPlaylist)) return;
  uint8_t *data = *(uint8_t **)(page + 0x2E0);
  size_t n = *(size_t *)(page + 0x2E8) / 0x38;
  if (!data || n < 2 || *(int32_t *)(data + (n - 1) * 0x38 + 0x30) != kCatPause) {
    log_line("[music] WARNING: unexpected settings categories; no PLAYLIST category\r\n");
    return;
  }
  alignas(16) uint8_t tmp[0x38] = {};
  ((void (*)(void *, void *))(g_exe + 0x0007C990))(tmp, *(void **)(data + 0x20));  // empty item vector, same allocator
  *(int32_t *)(tmp + 0x30) = kCatPlaylist;
  ((void (*)(void *, void *))(g_exe + 0x00340E40))(page + 0x2D8, tmp);           // append (moves the items out of tmp)
  ((void (*)(void *))(g_exe + 0x0007CBC0))(tmp);
  data = *(uint8_t **)(page + 0x2E0);
  size_t at = n - 1;
  for (size_t i = 0; i < n; ++i) {
    const int32_t id = *(int32_t *)(data + i * 0x38 + 0x30);
    if (id == kCatVoiceText || (id == 4 && at == n - 1)) at = i + 1;
  }
  uint8_t mine[0x38];
  memcpy(mine, data + n * 0x38, 0x38);
  memmove(data + (at + 1) * 0x38, data + at * 0x38, (n - at) * 0x38);
  memcpy(data + at * 0x38, mine, 0x38);
}

// Options builder exe+0x354CB0(page): fills every category's rows. The playlist rows go to the new category
// and to the end of the pause menu's list.
typedef void (*PFN_Build)(uint8_t *page);
static PFN_Build g_trueBuild = nullptr;
static void HookBuild(uint8_t *page) {
  InsertCategory(page);
  g_trueBuild(page);
  if (uint8_t *c = FindCategory(page, kCatPlaylist)) AddRows(c);
  if (uint8_t *c = FindCategory(page, kCatPause)) AddRows(c);
}

// ---- install ------------------------------------------------------------------------------------
static void Install(HMODULE hExe) {
  g_exe = (uint8_t *)hExe;
  g_t0 = GetTickCount64();
  DWORD n = GetEnvironmentVariableW(L"ONRUSH_MUSIC_PLAYLIST", g_m3u, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) { g_m3u[0] = 0; return; }   // no playlist chosen: the game's music, no rows
  wchar_t v[16] = {};
  if (GetEnvironmentVariableW(L"ONRUSH_MUSIC_ENABLED", v, 16)) g_enabled = v[0] != L'0';
  g_shuffle = GetEnvironmentVariableW(L"ONRUSH_MUSIC_SHUFFLE", v, 16) && v[0] == L'1';
  LoadPlaylist();
  int found = 0;
  for (const Track &t : g_tracks) found += t.exists;
  log_line("[music] playlist '%ls': %d songs (%d found, %d on), %s, %s\r\n", g_m3u, (int)g_tracks.size(), found,
           PlayableCount(), g_enabled ? "replaces the game music" : "game music chosen", g_shuffle ? "shuffle" : "in order");
  if (found == 0) return;

  // game music state and pause duck
  static const uint8_t kSetStateHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};
  if (!mouse::HookAt(g_exe + 0x00332010, kSetStateHead, sizeof(kSetStateHead), (void *)&HookSetState, (void **)&g_trueSetState)) {
    log_line("[music] WARNING: unexpected music state code; playlist off\r\n");
    return;
  }
  for (auto &e : g_eventHandle) for (volatile uint32_t &h : e) h = 0xFFFFFFFFu;
  // mov r9,rcx; mov r8,rdx; mov rcx,[rip+x]; ... jmp [rax+0x140]
  static const uint8_t kNamed[] = {0x4C, 0x8B, 0xC9, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0x0D};
  // mov r9d,ecx; mov r8,rdx; mov rcx,[rip+x]; ... jmp [rax+0x50]
  static const uint8_t kGetEvent[] = {0x44, 0x8B, 0xC9, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0x0D};
  // sub rsp,28h; movzx eax,byte [rdx]; lea r8,[rsp+38h]; mov r9d,ecx; mov [rsp+38h],al; mov rcx,[rip+x]; ... call [rax+0xD8]
  static const uint8_t kPostEvent[] = {0x48, 0x83, 0xEC, 0x28, 0x0F, 0xB6, 0x02, 0x4C, 0x8D, 0x44, 0x24, 0x38, 0x44, 0x8B, 0xC9,
                                       0x88, 0x44, 0x24, 0x38, 0x48, 0x8B, 0x0D};
  if (!ReplaceWrapper(0x001F5590, kNamed, sizeof(kNamed), 9, (void *)&HookAudioNamed) ||
      !ReplaceWrapper(0x001F3D00, kGetEvent, sizeof(kGetEvent), 9, (void *)&HookGetEvent) ||
      !ReplaceWrapper(0x001F51D0, kPostEvent, sizeof(kPostEvent), 22, (void *)&HookPostEvent))
    log_line("[music] WARNING: unexpected audio call code; the playlist may not pause with the game\r\n");

  // options: per-song callbacks, the PLAYLIST category and the rows
  g_songThunks = (uint8_t *)AllocateNearAddress(g_exe + 0x00300000, kMaxRows * 32);
  DWORD old = 0;
  if (g_songThunks && !VirtualProtect(g_songThunks, kMaxRows * 32, PAGE_EXECUTE_READWRITE, &old)) g_songThunks = nullptr;
  if (!g_songThunks) log_line("[music] WARNING: no memory for the option rows\r\n");
  if (g_songThunks) {
    for (int i = 0; i < kMaxRows; ++i) {
      uint8_t *q = g_songThunks + i * 32;
      *q++ = 0x41; *q++ = 0xB8; memcpy(q, &i, 4); q += 4;                       // mov r8d, i
      gfxopt::EmitJmpAbs(q, (void *)&OnSong);
    }
    FlushInstructionCache(GetCurrentProcess(), g_songThunks, kMaxRows * 32);
    // category names: the table the game reads now (language.inc's copy, or the game's) plus ours
    uint8_t *lea = g_exe + 0x0038CA2D;   // lea rcx, [rip+x]
    if (lea[0] == 0x48 && lea[1] == 0x8D && lea[2] == 0x0D) {
      const char *const *cur = (const char *const *)(lea + 7 + *(int32_t *)(lea + 3));
      g_catTable = (const char **)AllocateNearAddress(lea, (kCatPlaylist + 1) * sizeof(char *));
      if (g_catTable) {
        memcpy(g_catTable, cur, kCatPlaylist * sizeof(char *));
        g_catTable[kCatPlaylist] = "ID_RL_PLAYLIST";
        int32_t d = (int32_t)((uint8_t *)g_catTable - (lea + 7));
        if (!gfxopt::WriteCode(lea + 3, &d, 4)) g_catTable = nullptr;
      }
    }
    if (!g_catTable) log_line("[music] WARNING: unexpected options list code; no PLAYLIST category\r\n");
    static const uint8_t kBuildHead[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
    if (!mouse::HookAt(g_exe + 0x00354CB0, kBuildHead, sizeof(kBuildHead), (void *)&HookBuild, (void **)&g_trueBuild))
      log_line("[music] WARNING: unexpected options builder code; no playlist rows\r\n");
  }

  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE h = CreateThread(nullptr, 0, PlayerThread, nullptr, 0, nullptr);
  if (h) CloseHandle(h);
  log_line("[music] installed\r\n");
}

}  // namespace music
