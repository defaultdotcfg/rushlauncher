// Graphics options as native rows of the game's own Options > GRAPHICS page (no overlay).
//
// The options menu is built in code (exe+0x354E00..0x3559AE): each row is a 0x40-byte item
// {name key, widget type, value, invoker, callback, id, value-key table, value count} appended to the
// page's item vector by exe+0x340DB0. Row and value names are localisation keys looked up by
// exe+0x22DFA0, so new keys ("ID_RL_*") get their text from a hook on that lookup.
//
// Rows (inserted where the console builds add "Visuals" on Xbox One X):
//  * Graphics quality: Console (the Xbox One profile) or the PC profiles the developers shipped in
//    code:graphics/viewquality.cfg (QualityConfig-PC-UltraLow .. -UltraHigh: render scale, shadow
//    maps, LOD bias, tessellation). The game applies a quality profile through exe+0x3DCD60 (profile
//    load, photo mode exit, ...); its base-console names are redirected to the chosen profile, so the
//    choice holds everywhere and is applied at once through the game's own reload.
//  * Frame rate limit (30 .. 240, unlimited): time-based limiter in front of Present, independent
//    of the monitor's refresh rate (the game's FlipRate 2 only meant "every other vblank").
//  * Detail rows (render resolution, shadows, world/vehicle detail, tessellation, fog, light effects,
//    debris): AUTO keeps the preset's value, anything else overrides the matching viewquality.cfg
//    keys at the game's config lookup (exe+0x156FC0), see "detailed settings" below.
//  * Vertical sync: on = Present waits for vblank; off = tearing allowed (no extra latency).
// Choices persist in the launcher's onrush.ini (ONRUSH_INI) and come back through the environment.

#include <dxgi1_5.h>

namespace gfxopt {

static const char *const kPresetSuffix[] = {"XBoxOne-Base", "PC-UltraLow", "PC-Low", "PC-Medium", "PC-High", "PC-UltraHigh"};
static const int kPresetCount = 6;
static const int kFpsChoices[] = {30, 60, 90, 120, 144, 165, 240, 0};  // 0 = unlimited
static const int kFpsCount = 8;

static volatile LONG g_preset = 0;
static volatile LONG g_fpsIdx = 1;
static volatile LONG g_vsync = 1;
static volatile LONG g_upscaler = 0;   // 0 = the game's TAA, 1 = FSR 2 (upscaler.inc)
static wchar_t g_ini[MAX_PATH] = {};
static HMODULE g_exe = nullptr;
static bool g_tearingSupported = false;

// Profile names the game reads (near the exe so `lea reg,[rip+x]` can reach them).
static char *g_qcName = nullptr;     // "QualityConfig-<suffix>"
static char *g_qcPhoto = nullptr;    // "QualityConfig-PhotoMode-<suffix>"

static int EnvInt(const char *name, int def) {
  char v[16] = {};
  DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
  return (n > 0 && n < sizeof(v)) ? atoi(v) : def;
}

static void UpdateProfileNames() {
  if (!g_qcName) return;
  int p = g_preset;
  if (p < 0 || p >= kPresetCount) p = 0;
  _snprintf_s(g_qcName, 64, _TRUNCATE, "QualityConfig-%s", kPresetSuffix[p]);
  _snprintf_s(g_qcPhoto, 64, _TRUNCATE, "QualityConfig-PhotoMode-%s", kPresetSuffix[p]);
}

static void SaveIni() {
  if (!g_ini[0]) return;
  wchar_t b[16];
  swprintf_s(b, L"%ld", (long)g_preset);
  WritePrivateProfileStringW(L"Graphics", L"Preset", b, g_ini);
  swprintf_s(b, L"%d", kFpsChoices[g_fpsIdx]);
  WritePrivateProfileStringW(L"Graphics", L"FpsLimit", b, g_ini);
  swprintf_s(b, L"%ld", (long)g_vsync);
  WritePrivateProfileStringW(L"Display", L"VSync", b, g_ini);
  WritePrivateProfileStringW(L"Graphics", L"Upscaler", g_upscaler ? L"FSR2" : L"TAA", g_ini);
}

// ---- texts --------------------------------------------------------------------------------------
enum { L_EN, L_PT, L_FR, L_ES, L_DE, L_IT, L_PL, L_COUNT };

static volatile LONG s_lang = -1;  // reset when the language is changed in game
static int TextLang() {
  if (s_lang >= 0) return s_lang;
  wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {};
  DWORD n = GetEnvironmentVariableW(L"ONRUSH_TEXT_LANG", loc, LOCALE_NAME_MAX_LENGTH);
  if (n == 0 || n >= LOCALE_NAME_MAX_LENGTH) GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
  static const struct { const wchar_t *pre; int lang; } map[] = {
    {L"pt", L_PT}, {L"fr", L_FR}, {L"es", L_ES}, {L"de", L_DE}, {L"it", L_IT}, {L"pl", L_PL}};
  int lang = L_EN;
  for (const auto &m : map)
    if (_wcsnicmp(loc, m.pre, 2) == 0) lang = m.lang;
  s_lang = lang;
  return lang;
}

struct Text { const char *key; const char *t[L_COUNT]; };  // UTF-8, upper case like the game's own; en, pt, fr, es, de, it, pl.
// List rows clip (and scroll) row names past ~17 characters: keep them short.
static const Text kTexts[] = {
  {"ID_RL_GFX_QUALITY", {"GRAPHICS QUALITY", "QUALIDADE GRÁFICA", "QUALITÉ GRAPHIQUE", "CALIDAD GRÁFICA",
                         "GRAFIKQUALITÄT", "QUALITÀ GRAFICA", "JAKOŚĆ GRAFIKI"}},
  {"ID_RL_Q_CONSOLE", {"CONSOLE", "CONSOLE", "CONSOLE", "CONSOLA", "KONSOLE", "CONSOLE", "KONSOLA"}},
  {"ID_RL_Q_ULTRALOW", {"ULTRA LOW", "ULTRA BAIXA", "ULTRA BASSE", "ULTRABAJA", "ULTRANIEDRIG", "ULTRA BASSA", "BARDZO NISKA"}},
  {"ID_RL_Q_LOW", {"LOW", "BAIXA", "BASSE", "BAJA", "NIEDRIG", "BASSA", "NISKA"}},
  {"ID_RL_Q_MEDIUM", {"MEDIUM", "MÉDIA", "MOYENNE", "MEDIA", "MITTEL", "MEDIA", "ŚREDNIA"}},
  {"ID_RL_Q_HIGH", {"HIGH", "ALTA", "HAUTE", "ALTA", "HOCH", "ALTA", "WYSOKA"}},
  {"ID_RL_Q_ULTRA", {"ULTRA", "ULTRA", "ULTRA", "ULTRA", "ULTRA", "ULTRA", "ULTRA"}},
  {"ID_RL_FPS_LIMIT", {"FRAME RATE LIMIT", "LIMITE DE QUADROS", "LIMITE D'IMAGES", "LÍMITE DE FPS",
                       "FPS-LIMIT", "LIMITE FPS", "LIMIT KLATEK"}},
  {"ID_RL_FPS_UNLIMITED", {"UNLIMITED", "SEM LIMITE", "ILLIMITÉ", "SIN LÍMITE", "UNBEGRENZT", "ILLIMITATO", "BEZ LIMITU"}},
  {"ID_RL_VSYNC", {"VERTICAL SYNC", "V-SYNC", "SYNCHRO VERTICALE", "V-SYNC", "V-SYNC", "V-SYNC", "V-SYNC"}},
  {"ID_RL_OFF", {"OFF", "DESLIGADO", "DÉSACTIVÉE", "DESACTIVADO", "AUS", "DISATTIVATO", "WYŁĄCZONE"}},
  {"ID_RL_ON", {"ON", "LIGADO", "ACTIVÉE", "ACTIVADO", "AN", "ATTIVATO", "WŁĄCZONE"}},
  // detail rows
  {"ID_RL_AUTO", {"AUTO", "AUTO", "AUTO", "AUTO", "AUTO", "AUTO", "AUTO"}},
  {"ID_RL_RES", {"RENDER RESOLUTION", "RESOLUÇÃO INTERNA", "RÉSOLUTION RENDU", "RESOLUCIÓN",
                 "RENDERAUFLÖSUNG", "RISOLUZIONE", "ROZDZIELCZOŚĆ"}},
  {"ID_RL_SHADOWS", {"SHADOWS", "SOMBRAS", "OMBRES", "SOMBRAS", "SCHATTEN", "OMBRE", "CIENIE"}},
  {"ID_RL_WORLD", {"WORLD DETAIL", "DETALHE CENÁRIO", "DÉTAILS DU DÉCOR", "DETALLE ESCENARIO",
                   "WELTDETAILS", "DETTAGLI SCENARIO", "SZCZEGÓŁY ŚWIATA"}},
  {"ID_RL_VEHICLES", {"VEHICLE DETAIL", "DETALHE VEÍCULOS", "DÉTAILS VÉHICULES", "DETALLE VEHÍCULOS",
                      "FAHRZEUGDETAILS", "DETTAGLI VEICOLI", "SZCZEGÓŁY AUT"}},
  {"ID_RL_TESS", {"TESSELLATION", "TESSELAÇÃO", "TESSELLATION", "TESELACIÓN", "TESSELLATION",
                  "TASSELLAZIONE", "TESELACJA"}},
  {"ID_RL_FOG", {"FOG", "NÉVOA", "BROUILLARD", "NIEBLA", "NEBEL", "NEBBIA", "MGŁA"}},
  {"ID_RL_LIGHTFX", {"LIGHT EFFECTS", "EFEITOS DE LUZ", "EFFETS LUMINEUX", "EFECTOS DE LUZ",
                     "LICHTEFFEKTE", "EFFETTI LUCE", "EFEKTY ŚWIATŁA"}},
  {"ID_RL_DEBRIS", {"DEBRIS", "DETRITOS", "DÉBRIS", "ESCOMBROS", "TRÜMMER", "DETRITI", "ODŁAMKI"}},
  {"ID_RL_OFF_F", {"OFF", "DESLIGADA", "DÉSACTIVÉE", "DESACTIVADA", "AUS", "DISATTIVATA", "WYŁĄCZONA"}},
  {"ID_RL_OFF_MP", {"OFF", "DESLIGADOS", "DÉSACTIVÉS", "DESACTIVADOS", "AUS", "DISATTIVATI", "WYŁĄCZONE"}},
  {"ID_RL_ON_MP", {"ON", "LIGADOS", "ACTIVÉS", "ACTIVADOS", "AN", "ATTIVATI", "WŁĄCZONE"}},
  {"ID_RL_ON_F", {"ON", "LIGADA", "ACTIVÉE", "ACTIVADA", "AN", "ATTIVATA", "WŁĄCZONA"}},
  {"ID_RL_M_LOW", {"LOW", "BAIXO", "BAS", "BAJO", "NIEDRIG", "BASSO", "NISKI"}},
  {"ID_RL_M_MEDIUM", {"MEDIUM", "MÉDIO", "MOYEN", "MEDIO", "MITTEL", "MEDIO", "ŚREDNI"}},
  {"ID_RL_M_HIGH", {"HIGH", "ALTO", "ÉLEVÉ", "ALTO", "HOCH", "ALTO", "WYSOKI"}},
  {"ID_RL_FEW", {"FEW", "POUCOS", "PEU", "POCOS", "WENIG", "POCHI", "MAŁO"}},
  {"ID_RL_MANY", {"MANY", "MUITOS", "BEAUCOUP", "MUCHOS", "VIELE", "MOLTI", "DUŻO"}},
  {"ID_RL_UPSCALER", {"UPSCALER", "UPSCALER", "MISE À L'ÉCHELLE", "ESCALADO", "UPSCALER", "UPSCALER", "SKALOWANIE"}},
  {"ID_RL_UPS_TAA", {"TAA", "TAA", "TAA", "TAA", "TAA", "TAA", "TAA"}},
  {"ID_RL_UPS_FSR2", {"FSR 2", "FSR 2", "FSR 2", "FSR 2", "FSR 2", "FSR 2", "FSR 2"}},
  // voices and text page (language.inc)
  {"ID_RL_VOICE_TEXT", {"VOICES AND TEXT", "VOZES E TEXTO", "VOIX ET TEXTE", "VOCES Y TEXTO", "STIMMEN UND TEXT",
                        "VOCI E TESTO", "GŁOSY I TEKST"}},
  {"ID_RL_LANG", {"TEXT", "TEXTO", "TEXTE", "TEXTO", "TEXT", "TESTO", "TEKST"}},
  {"ID_RL_VOICE", {"VOICES", "VOZES", "VOIX", "VOCES", "STIMMEN", "VOCI", "GŁOSY"}},
  {"ID_RL_VOICE_NOTICE", {"The game's voices will change the next time you start ONRUSH.",
                          "A voz do jogo será alterada na próxima vez que você iniciar ONRUSH.",
                          "Les voix du jeu changeront au prochain lancement d'ONRUSH.",
                          "Las voces del juego cambiarán la próxima vez que inicies ONRUSH.",
                          "Die Stimmen im Spiel ändern sich beim nächsten Start von ONRUSH.",
                          "Le voci del gioco cambieranno al prossimo avvio di ONRUSH.",
                          "Głosy w grze zmienią się przy następnym uruchomieniu ONRUSH."}},
  {"ID_RL_SAME_AS_TEXT", {"SAME AS TEXT", "IGUAL AO TEXTO", "COMME LE TEXTE", "IGUAL AL TEXTO", "WIE TEXT",
                          "COME IL TESTO", "JAK TEKST"}},
  {"ID_RL_LANG_EN", {"ENGLISH", "ENGLISH", "ENGLISH", "ENGLISH", "ENGLISH", "ENGLISH", "ENGLISH"}},
  {"ID_RL_LANG_PT", {"PORTUGUÊS (BR)", "PORTUGUÊS (BR)", "PORTUGUÊS (BR)", "PORTUGUÊS (BR)", "PORTUGUÊS (BR)",
                     "PORTUGUÊS (BR)", "PORTUGUÊS (BR)"}},
  {"ID_RL_LANG_ES", {"ESPAÑOL", "ESPAÑOL", "ESPAÑOL", "ESPAÑOL", "ESPAÑOL", "ESPAÑOL", "ESPAÑOL"}},
  {"ID_RL_LANG_FR", {"FRANÇAIS", "FRANÇAIS", "FRANÇAIS", "FRANÇAIS", "FRANÇAIS", "FRANÇAIS", "FRANÇAIS"}},
  {"ID_RL_LANG_DE", {"DEUTSCH", "DEUTSCH", "DEUTSCH", "DEUTSCH", "DEUTSCH", "DEUTSCH", "DEUTSCH"}},
  {"ID_RL_LANG_IT", {"ITALIANO", "ITALIANO", "ITALIANO", "ITALIANO", "ITALIANO", "ITALIANO", "ITALIANO"}},
  {"ID_RL_LANG_PL", {"POLSKI", "POLSKI", "POLSKI", "POLSKI", "POLSKI", "POLSKI", "POLSKI"}},
};
// Percentages (same in every language).
static const char *const kPctKeys[] = {"ID_RL_PCT_50", "ID_RL_PCT_67", "ID_RL_PCT_75", "ID_RL_PCT_85",
                                       "ID_RL_PCT_100", "ID_RL_PCT_125", "ID_RL_PCT_150", "ID_RL_PCT_200"};
static const char *const kPctLabels[] = {"50%", "67%", "75%", "85%", "100%", "125%", "150%", "200%"};
static const float kPctScale[] = {0.5f, 0.6667f, 0.75f, 0.85f, 1.0f, 1.25f, 1.5f, 2.0f};
static const char *const kFpsKeys[] = {"ID_RL_FPS_30", "ID_RL_FPS_60", "ID_RL_FPS_90", "ID_RL_FPS_120",
                                       "ID_RL_FPS_144", "ID_RL_FPS_165", "ID_RL_FPS_240", "ID_RL_FPS_UNLIMITED"};
static const char *const kFpsLabels[] = {"30 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS"};

typedef const char *(*PFN_Localise)(const char *key);
static PFN_Localise g_trueLocalise = nullptr;

static const char *HookLocalise(const char *key) {
  if (key && key[0] == 'I' && strncmp(key, "ID_RL_", 6) == 0) {
    for (int i = 0; i < 7; ++i)
      if (strcmp(key, kFpsKeys[i]) == 0) return kFpsLabels[i];
    for (int i = 0; i < (int)_countof(kPctKeys); ++i)
      if (strcmp(key, kPctKeys[i]) == 0) return kPctLabels[i];
    for (const Text &t : kTexts)
      if (strcmp(key, t.key) == 0) return t.t[TextLang()];
  }
  return g_trueLocalise(key);
}

// ---- detailed settings: overrides of config values ----------------------------------------------
// Every graphics setting the renderer reads from viewquality.cfg goes through
// exe+0x156FC0(db = [exe+0x157E808], "Path/key", flag) -> entry* {u32 type (1 int, 2 float, 3 bool),
// u32 value, ...} or null. Views read their block when they are created ("EventRenderViewConfig/..."
// = the race view, so race settings take effect at the next race), the "Memory/..." and global keys
// when a quality profile is (re)applied. The detail rows below override chosen keys on top of the
// quality preset; "AUTO" leaves the preset's value.
enum CfgType : uint32_t { CT_INT = 1, CT_FLOAT = 2, CT_BOOL = 3 };
enum RuleMode : uint8_t { RM_SET, RM_MUL };
struct Rule { const char *pattern; uint32_t type; RuleMode mode; float f; };  // f: value or factor

// '*' matches any run of characters except '/'.
static bool KeyMatch(const char *p, const char *k) {
  for (;;) {
    if (*p == '*') {
      ++p;
      for (const char *s = k;; ++s) {
        if (KeyMatch(p, s)) return true;
        if (!*s || *s == '/') return false;
      }
    }
    if (*p != *k) return false;
    if (!*p) return true;
    ++p; ++k;
  }
}

// No reflections row: with screen-space reflections off the race view's final composite smears (it
// keeps reading the reflection buffer); the game never turns them off in a race either.
enum Detail { D_RES, D_SHADOWS, D_WORLD, D_VEHICLES, D_TESS, D_FOG, D_LIGHTFX, D_DEBRIS, D_COUNT };
struct DetailRow {
  const char *name;            // localisation key of the row
  const wchar_t *ini;          // onrush.ini [Graphics] key
  const char *const *values;   // value keys (index 0 = AUTO)
  uint32_t count;
};
static const char *const kResVals[] = {"ID_RL_AUTO", "ID_RL_PCT_50", "ID_RL_PCT_67", "ID_RL_PCT_75", "ID_RL_PCT_85",
                                       "ID_RL_PCT_100", "ID_RL_PCT_125", "ID_RL_PCT_150", "ID_RL_PCT_200"};
// No "off": the race view is not built for running without shadows (the game never does it; forcing
// Shadows/enabled = false there crashes at exe+0x128CC0).
static const char *const kShadowVals[] = {"ID_RL_AUTO", "ID_RL_Q_LOW", "ID_RL_Q_MEDIUM", "ID_RL_Q_HIGH", "ID_RL_Q_ULTRA"};
static const char *const kLodVals[] = {"ID_RL_AUTO", "ID_RL_M_LOW", "ID_RL_M_MEDIUM", "ID_RL_M_HIGH", "ID_RL_Q_ULTRA"};
static const char *const kTessVals[] = {"ID_RL_AUTO", "ID_RL_OFF_F", "ID_RL_ON_F"};
static const char *const kOnOffMpVals[] = {"ID_RL_AUTO", "ID_RL_OFF_MP", "ID_RL_ON_MP"};
static const char *const kFogVals[] = {"ID_RL_AUTO", "ID_RL_OFF_F", "ID_RL_Q_LOW", "ID_RL_Q_HIGH"};
static const char *const kDebrisVals[] = {"ID_RL_AUTO", "ID_RL_FEW", "ID_RL_MANY"};
static const DetailRow kDetail[D_COUNT] = {
  {"ID_RL_RES", L"RenderScale", kResVals, _countof(kResVals)},
  {"ID_RL_SHADOWS", L"Shadows", kShadowVals, _countof(kShadowVals)},
  {"ID_RL_WORLD", L"WorldDetail", kLodVals, _countof(kLodVals)},
  {"ID_RL_VEHICLES", L"VehicleDetail", kLodVals, _countof(kLodVals)},
  {"ID_RL_TESS", L"Tessellation", kTessVals, _countof(kTessVals)},
  {"ID_RL_FOG", L"Fog", kFogVals, _countof(kFogVals)},
  {"ID_RL_LIGHTFX", L"LightEffects", kOnOffMpVals, _countof(kOnOffMpVals)},
  {"ID_RL_DEBRIS", L"Debris", kDebrisVals, _countof(kDebrisVals)},
};
static volatile LONG g_detail[D_COUNT] = {};

enum { kMaxRules = 96 };
static Rule g_rules[kMaxRules];
static volatile LONG g_ruleCount = 0;
static SRWLOCK g_ruleLock = SRWLOCK_INIT;

static void AddRule(Rule *r, LONG &n, const char *pattern, uint32_t type, RuleMode mode, float f) {
  if (n < kMaxRules) r[n++] = Rule{pattern, type, mode, f};
}

// LOD bias (positive = coarser) and lowest LOD index allowed, for LOW / MEDIUM / HIGH / ULTRA.
static const float kLodBias[] = {0.5f, 0.0f, -0.5f, -1.0f};
static const float kMinLod[] = {1.0f, 0.0f, 0.0f, 0.0f};

static void BuildRules() {
  Rule r[kMaxRules];
  LONG n = 0;
  LONG v = g_detail[D_RES];
  if (v > 0) {
    // Render size = display size x scale, capped by MaxRender*: the cap is raised to exactly what the
    // chosen scale needs at the window's resolution (render targets are sized from it, so a larger
    // cap only wastes GPU memory). The TAA history keeps the preset's size.
    float s = kPctScale[v - 1];
    int w = EnvInt("ONRUSH_RES_WIDTH", 1920), h = EnvInt("ONRUSH_RES_HEIGHT", 1080);
    if (w < 640 || h < 360) { w = 1920; h = 1080; }
    AddRule(r, n, "Memory/DisplayToRenderScale", CT_FLOAT, RM_SET, s);
    AddRule(r, n, "Memory/MaxRenderWidth", CT_INT, RM_SET, (float)(((int)(w * s) + 7) & ~7));
    AddRule(r, n, "Memory/MaxRenderHeight", CT_INT, RM_SET, (float)(((int)(h * s) + 7) & ~7));
  }
  v = g_detail[D_SHADOWS];
  if (v > 0) {  // LOW / MEDIUM / HIGH / ULTRA: filter taps and cascade reach
    static const float kLookups[] = {4, 8, 12, 12}, kDist[] = {0.6f, 1.0f, 1.3f, 1.7f};
    AddRule(r, n, "EventRenderViewConfig/ShadowCascade:*/numLookups", CT_INT, RM_SET, kLookups[v - 1]);
    AddRule(r, n, "EventRenderViewConfig/ShadowCascade:*/distance", CT_FLOAT, RM_MUL, kDist[v - 1]);
  }
  static const char *const kWorldCats[] = {"CategoryLodBias/Default", "*/CategoryLodBias/Default",
                                           "CategoryLodBias/Fodder", "*/CategoryLodBias/Fodder",
                                           "CategoryLodBias/ProceduralPlacement", "*/CategoryLodBias/ProceduralPlacement"};
  static const char *const kWorldMin[] = {"CategoryMinLod/Fodder", "*/CategoryMinLod/Fodder"};
  static const char *const kVehCats[] = {"CategoryLodBias/ViewedVehicle", "*/CategoryLodBias/ViewedVehicle",
                                         "CategoryLodBias/HubViewedVehicle", "*/CategoryLodBias/HubViewedVehicle",
                                         "CategoryLodBias/PlayerVehicle", "*/CategoryLodBias/PlayerVehicle",
                                         "CategoryLodBias/AIVehicle", "*/CategoryLodBias/AIVehicle",
                                         "CategoryLodBias/Character", "*/CategoryLodBias/Character",
                                         "CategoryLodBias/HubCharacter", "*/CategoryLodBias/HubCharacter"};
  static const char *const kVehMin[] = {"CategoryMinLod/ViewedVehicle", "*/CategoryMinLod/ViewedVehicle",
                                        "CategoryMinLod/PlayerVehicle", "*/CategoryMinLod/PlayerVehicle",
                                        "CategoryMinLod/AIVehicle", "*/CategoryMinLod/AIVehicle",
                                        "CategoryMinLod/Character", "*/CategoryMinLod/Character"};
  v = g_detail[D_WORLD];
  if (v > 0) {
    for (const char *k : kWorldCats) AddRule(r, n, k, CT_FLOAT, RM_SET, kLodBias[v - 1]);
    for (const char *k : kWorldMin) AddRule(r, n, k, CT_INT, RM_SET, kMinLod[v - 1]);
  }
  v = g_detail[D_VEHICLES];
  if (v > 0) {
    for (const char *k : kVehCats) AddRule(r, n, k, CT_FLOAT, RM_SET, kLodBias[v - 1]);
    for (const char *k : kVehMin) AddRule(r, n, k, CT_INT, RM_SET, kMinLod[v - 1]);
  }
  v = g_detail[D_TESS];
  if (v == 1) {
    AddRule(r, n, "Tessellation/enabled", CT_BOOL, RM_SET, 0);
    AddRule(r, n, "Tessellation/castShadowsFromTessellatedTerrain", CT_BOOL, RM_SET, 0);
  } else if (v == 2) {  // viewquality.cfg DefaultTessellation
    AddRule(r, n, "Tessellation/enabled", CT_BOOL, RM_SET, 1);
    AddRule(r, n, "Tessellation/maxTessellationFactor", CT_FLOAT, RM_SET, 16.0f);
    AddRule(r, n, "Tessellation/deformationScale", CT_FLOAT, RM_SET, 0.25f);
    AddRule(r, n, "Tessellation/snowDeformationScale", CT_FLOAT, RM_SET, 0.25f);
    AddRule(r, n, "Tessellation/tessellationNearDistance", CT_FLOAT, RM_SET, 40.0f);
    AddRule(r, n, "Tessellation/tessellationFarDistance", CT_FLOAT, RM_SET, 50.0f);
    AddRule(r, n, "Tessellation/deformationNearDistance", CT_FLOAT, RM_SET, 25.0f);
    AddRule(r, n, "Tessellation/deformationFarDistance", CT_FLOAT, RM_SET, 250.0f);
    AddRule(r, n, "Tessellation/castShadowsFromTessellatedTerrain", CT_BOOL, RM_SET, 1);
  }
  v = g_detail[D_FOG];
  if (v == 1) AddRule(r, n, "EventRenderViewConfig/Atmospherics/enabled", CT_BOOL, RM_SET, 0);
  else if (v >= 2) AddRule(r, n, "Memory/VolumetricFogResolutionScale", CT_FLOAT, RM_SET, v == 2 ? 0.5f : 1.0f);
  v = g_detail[D_LIGHTFX];
  if (v > 0) {
    float on = v == 2 ? 1.0f : 0.0f;
    AddRule(r, n, "EventRenderViewConfig/Sky/godRays", CT_BOOL, RM_SET, on);
    AddRule(r, n, "EventRenderViewConfig/LensFlare/enabled", CT_BOOL, RM_SET, on);
    AddRule(r, n, "EventRenderViewConfig/LensDust/enabled", CT_BOOL, RM_SET, on);
  }
  v = g_detail[D_DEBRIS];
  if (v > 0) AddRule(r, n, "Debris/*", CT_INT, RM_MUL, v == 1 ? 0.5f : 2.0f);

  AcquireSRWLockExclusive(&g_ruleLock);
  memcpy(g_rules, r, sizeof(Rule) * n);
  g_ruleCount = n;
  ReleaseSRWLockExclusive(&g_ruleLock);
}

typedef uint8_t *(*PFN_CfgGet)(void *db, const char *key, uint8_t flag);
static PFN_CfgGet g_trueCfgGet = nullptr;

// Overridden entries live in a small table keyed by the key's hash (a key always maps to the same
// slot, so the pointer handed back stays valid).
struct CfgSlot { volatile LONG hash; uint32_t e[4]; };
static CfgSlot g_slots[1024];

static uint32_t KeyHash(const char *k) {
  uint32_t h = 2166136261u;
  for (; *k; ++k) h = (h ^ (uint8_t)*k) * 16777619u;
  return h | 1;
}

// ONRUSH_LOG_CFGKEYS=1: every key once, with the game's entry (type, value).
static void LogKey(const char *key, uint32_t h, const uint8_t *e, uint8_t flag) {
  enum { N = 4096 };
  static volatile LONG s_seen[N];
  for (uint32_t i = 0; i < 64; ++i) {
    LONG cur = InterlockedCompareExchange(&s_seen[(h + i) & (N - 1)], (LONG)h, 0);
    if (cur == (LONG)h) return;
    if (cur == 0) break;
  }
  if (e) log_line("%s[cfg] %s = %08X %08X (f%u)\r\n", "", key, ((const uint32_t *)e)[0], ((const uint32_t *)e)[1], flag);
  else log_line("%s[cfg] %s = (none) (f%u)\r\n", "", key, flag);
}

static uint8_t *HookCfgGet(void *db, const char *key, uint8_t flag) {
  uint8_t *e = g_trueCfgGet(db, key, flag);
  if (!key) return e;
  static const bool s_log = GetEnvironmentVariableA("ONRUSH_LOG_CFGKEYS", nullptr, 0) > 0;
  uint32_t h = 0;
  if (s_log) { h = KeyHash(key); LogKey(key, h, e, flag); }
  if (!g_ruleCount) return e;
  Rule rule = {};
  bool hit = false;
  AcquireSRWLockShared(&g_ruleLock);
  for (LONG i = 0; i < g_ruleCount; ++i)
    if (KeyMatch(g_rules[i].pattern, key)) { rule = g_rules[i]; hit = true; break; }
  ReleaseSRWLockShared(&g_ruleLock);
  if (!hit) return e;
  uint32_t type = e ? *(uint32_t *)e : rule.type;
  if (type != CT_INT && type != CT_FLOAT && type != CT_BOOL) return e;
  uint32_t value;
  if (rule.mode == RM_MUL) {
    if (!e) return e;  // nothing to scale
    if (type == CT_FLOAT) { float f = *(float *)(e + 4) * rule.f; memcpy(&value, &f, 4); }
    else { int32_t i = (int32_t)(*(int32_t *)(e + 4) * rule.f + 0.5f); value = (uint32_t)(i < 1 ? 1 : i); }
  } else if (type == CT_FLOAT) {
    memcpy(&value, &rule.f, 4);
  } else {
    value = (uint32_t)(int32_t)rule.f;
  }
  if (!h) h = KeyHash(key);
  for (uint32_t i = 0; i < _countof(g_slots); ++i) {
    CfgSlot &s = g_slots[(h + i) & (_countof(g_slots) - 1)];
    LONG cur = InterlockedCompareExchange(&s.hash, (LONG)h, 0);
    if (cur != 0 && cur != (LONG)h) continue;
    if (e) memcpy(s.e, e, sizeof(s.e));
    else memset(s.e, 0, sizeof(s.e));
    s.e[0] = type;
    s.e[1] = value;
    return (uint8_t *)s.e;
  }
  return e;
}

// ---- menu rows ----------------------------------------------------------------------------------
struct MenuItem {           // layout used by exe+0x340DB0 (AddItem) and the builder
  const char *name;         // +0x00 localisation key
  uint32_t type;            // +0x08 widget: 6 = list of `count` values
  float value;              // +0x0C current value (list index)
  void *invoker;            // +0x10 exe+0x35E960: calls `callback(value, ...)`
  void *callback;           // +0x18
  uint64_t unused20;        // +0x20
  uint32_t id;              // +0x28
  uint32_t unused2c;        // +0x2C
  const char *const *values;// +0x30 value keys
  uint32_t count;           // +0x38
  uint32_t unused3c;        // +0x3C
};
static_assert(sizeof(MenuItem) == 0x40, "menu item layout");

static const char *const kPresetKeys[] = {"ID_RL_Q_CONSOLE", "ID_RL_Q_ULTRALOW", "ID_RL_Q_LOW",
                                          "ID_RL_Q_MEDIUM", "ID_RL_Q_HIGH", "ID_RL_Q_ULTRA"};
static const char *const kOnOffKeys[] = {"ID_RL_OFF", "ID_RL_ON"};
enum : uint32_t { kIdQuality = 0x52, kIdFps = 0x53, kIdVsync = 0x54, kIdDetail0 = 0x55, kIdUpscaler = 0x5D };

static int ListIndex(float v, int count) {
  int i = (int)(v + 0.5f);
  return i < 0 ? 0 : i >= count ? count - 1 : i;
}

// Re-applies the game's quality profile through its own path (what the Xbox One X "Visuals" row does,
// exe+0x3B42C0): profile owner = exe+0x3CD710(exe+0x3CD100()), apply = exe+0x3DCD60(owner, flag).
static void ApplyQualityProfile() {
  typedef void *(*PFN_Get0)();
  typedef void *(*PFN_Get1)(void *);
  typedef void (*PFN_Apply)(void *, uint8_t);
  void *owner = ((PFN_Get1)((uint8_t *)g_exe + 0x003CD710))(((PFN_Get0)((uint8_t *)g_exe + 0x003CD100))());
  if (owner) ((PFN_Apply)((uint8_t *)g_exe + 0x003DCD60))(owner, *((uint8_t *)owner + 0x20));
}

static void OnQuality(float value, int) {
  LONG p = ListIndex(value, kPresetCount);
  if (p == g_preset) return;
  g_preset = p;
  UpdateProfileNames();
  ApplyQualityProfile();
  SaveIni();
  log_line("[gfx] graphics quality -> %s\r\n", g_qcName);
}
static void OnFps(float value, int) {
  g_fpsIdx = ListIndex(value, kFpsCount);
  SaveIni();
  log_line("[gfx] frame rate limit -> %d\r\n", kFpsChoices[g_fpsIdx]);
}
static const char *const kUpscalerKeys[] = {"ID_RL_UPS_TAA", "ID_RL_UPS_FSR2"};
static void OnUpscaler(float value, int) {
  g_upscaler = ListIndex(value, 2);
  SaveIni();
  log_line("[gfx] upscaler -> %s\r\n", g_upscaler ? "FSR 2" : "TAA");
}
static void OnVsync(float value, int) {
  g_vsync = ListIndex(value, 2);
  SaveIni();
  log_line("[gfx] vertical sync -> %ld\r\n", (long)g_vsync);
}

// Detail rows: index 0 = AUTO (the preset's value). Profile-level keys (render scale, tessellation,
// fog resolution, global LOD) are re-read by re-applying the profile; race-view keys at the next race.
static void SaveDetail(int d) {
  if (!g_ini[0]) return;
  wchar_t b[16];
  swprintf_s(b, L"%ld", (long)g_detail[d]);
  WritePrivateProfileStringW(L"Graphics", kDetail[d].ini, b, g_ini);
}
static void OnDetail(int d, float value) {
  LONG v = ListIndex(value, (int)kDetail[d].count);
  if (v == g_detail[d]) return;
  g_detail[d] = v;
  BuildRules();
  ApplyQualityProfile();
  SaveDetail(d);
  log_line("[gfx] %s -> %s\r\n", kDetail[d].name, kDetail[d].values[v]);
}
template <int D> static void OnDetailRow(float value, int) { OnDetail(D, value); }
static void *const kDetailCallbacks[D_COUNT] = {
  (void *)&OnDetailRow<0>, (void *)&OnDetailRow<1>, (void *)&OnDetailRow<2>, (void *)&OnDetailRow<3>,
  (void *)&OnDetailRow<4>, (void *)&OnDetailRow<5>, (void *)&OnDetailRow<6>, (void *)&OnDetailRow<7>};
static_assert(D_COUNT == 8, "one callback per detail row");

static void AddRows(void *page) {
  typedef void (*PFN_AddItem)(void *page, const MenuItem *item);
  PFN_AddItem add = (PFN_AddItem)((uint8_t *)g_exe + 0x00340DB0);
  void *invoker = (uint8_t *)g_exe + 0x0035E960;
  MenuItem it = {};
  it.type = 6;
  it.invoker = invoker;

  it.name = "ID_RL_GFX_QUALITY"; it.callback = (void *)&OnQuality; it.id = kIdQuality;
  it.values = kPresetKeys; it.count = kPresetCount; it.value = (float)g_preset;
  add(page, &it);
  for (int d = 0; d < D_COUNT; ++d) {
    it.name = kDetail[d].name; it.callback = kDetailCallbacks[d]; it.id = kIdDetail0 + d;
    it.values = kDetail[d].values; it.count = kDetail[d].count; it.value = (float)g_detail[d];
    add(page, &it);
    if (d == D_RES) {
      it.name = "ID_RL_UPSCALER"; it.callback = (void *)&OnUpscaler; it.id = kIdUpscaler;
      it.values = kUpscalerKeys; it.count = 2; it.value = (float)g_upscaler;
      add(page, &it);
    }
  }
  it.name = "ID_RL_FPS_LIMIT"; it.callback = (void *)&OnFps; it.id = kIdFps;
  it.values = kFpsKeys; it.count = kFpsCount; it.value = (float)g_fpsIdx;
  add(page, &it);
  it.name = "ID_RL_VSYNC"; it.callback = (void *)&OnVsync; it.id = kIdVsync;
  it.values = kOnOffKeys; it.count = 2; it.value = (float)g_vsync;
  add(page, &it);
  static volatile LONG s_n = 0;
  log_line("[gfx] graphics rows added to the options page (build #%ld)\r\n", InterlockedIncrement(&s_n));
}

// ---- patching helpers ---------------------------------------------------------------------------
static bool WriteCode(uint8_t *p, const void *src, size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(p, src, n);
  VirtualProtect(p, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, n);
  return true;
}

// `lea reg, [rip+disp32]` (7 bytes) at rva -> points at `target` instead of `expect`.
static bool RetargetLea(uint32_t rva, const char *expect, const void *target) {
  uint8_t *p = (uint8_t *)g_exe + rva;
  if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) return false;
  const char *cur = (const char *)(p + 7 + *(int32_t *)(p + 3));
  if (strcmp(cur, expect) != 0) return false;
  int64_t d = (int64_t)((const uint8_t *)target - (p + 7));
  if (d < INT32_MIN || d > INT32_MAX) return false;
  int32_t d32 = (int32_t)d;
  return WriteCode(p + 3, &d32, 4);
}

// Stub: mov rax, imm64 ; jmp rax
static uint8_t *EmitJmpAbs(uint8_t *q, const void *to) {
  *q++ = 0x48; *q++ = 0xB8; memcpy(q, &to, 8); q += 8;
  *q++ = 0xFF; *q++ = 0xE0;
  return q;
}

static bool WriteJmpRel(uint8_t *site, const uint8_t *to, size_t patchLen) {
  int64_t d = (int64_t)(to - (site + 5));
  if (d < INT32_MIN || d > INT32_MAX) return false;
  uint8_t b[64];
  if (patchLen < 5 || patchLen > sizeof(b)) return false;
  b[0] = 0xE9; int32_t d32 = (int32_t)d; memcpy(b + 1, &d32, 4);
  for (size_t i = 5; i < patchLen; ++i) b[i] = 0x90;
  return WriteCode(site, b, patchLen);
}

static void Install(HMODULE hExe) {
  g_exe = hExe;
  {
    wchar_t ini[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ONRUSH_INI", ini, MAX_PATH);
    if (n > 0 && n < MAX_PATH) wcscpy_s(g_ini, ini);
  }
  int p = EnvInt("ONRUSH_GFX_PRESET", 0);
  g_preset = (p >= 0 && p < kPresetCount) ? p : 0;
  int fps = EnvInt("ONRUSH_FPS_LIMIT", 60);
  g_fpsIdx = 1;
  for (int i = 0; i < kFpsCount; ++i)
    if (kFpsChoices[i] == fps) g_fpsIdx = i;
  g_vsync = EnvInt("ONRUSH_VSYNC", 1) ? 1 : 0;
  {
    // ONRUSH_UPSCALER overrides onrush.ini [Graphics] Upscaler (TAA / FSR2)
    wchar_t u[16] = {};
    if (!GetEnvironmentVariableW(L"ONRUSH_UPSCALER", u, 16) && g_ini[0]) GetPrivateProfileStringW(L"Graphics", L"Upscaler", L"TAA", u, 16, g_ini);
    g_upscaler = _wcsicmp(u, L"FSR2") == 0 ? 1 : 0;
  }
  if (g_ini[0])
    for (int d = 0; d < D_COUNT; ++d) {
      UINT v = GetPrivateProfileIntW(L"Graphics", kDetail[d].ini, 0, g_ini);
      g_detail[d] = v < kDetail[d].count ? (LONG)v : 0;
    }
  BuildRules();

  uint8_t *exe = (uint8_t *)hExe;
  uint8_t *mem = (uint8_t *)AllocateNearAddress(exe + 0x00300000, 0x1000);
  if (!mem) { log_line("[gfx] WARNING: no memory near the exe; graphics rows not installed\r\n"); return; }
  DWORD old = 0;
  VirtualProtect(mem, 0x1000, PAGE_EXECUTE_READWRITE, &old);
  g_qcName = (char *)mem; g_qcPhoto = (char *)mem + 128;
  UpdateProfileNames();
  uint8_t *code = mem + 256;

  // 1. Quality profile names: exe+0x3DCD60 (base console: quality / 60 Hz) and photo mode entry.
  //    "QualityConfig-XBoxOne-Base-60Hz" is not in viewquality.cfg: the redirect also stops the
  //    base console from asking for a profile that does not exist. The boot profile (table at
  //    exe+0xEE1428) stays the console's: the shadow map sizes are only read at boot, and the PC
  //    profiles' 8192x8192 shadow map does not fit the boot-time page list (exe+0xB5C78). The chosen
  //    profile is applied when the player profile loads (exe+0x384381 -> 0x3DCD60), like a menu change.
  bool okQ = RetargetLea(0x003DCD86, "QualityConfig-XBoxOne-Base", g_qcName) &&
             RetargetLea(0x003DCD8D, "QualityConfig-XBoxOne-Base-60Hz", g_qcName);
  bool okP = RetargetLea(0x00366FDE, "QualityConfig-PhotoMode-XBoxOne-Base", g_qcPhoto);
  log_line("[gfx] quality profile -> %s (apply %d, photo %d)\r\n", g_qcName, okQ, okP);

  // 2. Localisation lookup exe+0x22DFA0: `mov [rsp+8],rbx ; mov [rsp+18h],rsi` (10 bytes) moved to a
  //    trampoline, entry jumps to HookLocalise.
  uint8_t *loc = exe + 0x0022DFA0;
  static const uint8_t kLocHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
  // 3. Options builder exe+0x3550D6: `cmp byte [Rendering/FPSVsQualityOption], r13b ; ...` -> the
  //    Visuals block (up to 0x35514C) is replaced by AddRows(rbx = display page). r15 is set to the
  //    Visuals callback as the skipped block did (the second half of the builder reads it).
  uint8_t *bld = exe + 0x003550D6;
  static const uint8_t kBldHead[] = {0x44, 0x38, 0x2D};
  if (memcmp(loc, kLocHead, sizeof(kLocHead)) != 0 || memcmp(bld, kBldHead, sizeof(kBldHead)) != 0 ||
      memcmp(exe + 0x0035514C, "\x48\x8B\x53\x10", 4) != 0) {
    log_line("[gfx] WARNING: unexpected code at exe+0x22DFA0/0x3550D6; graphics rows not installed\r\n");
    return;
  }
  uint8_t *q = code;
  g_trueLocalise = (PFN_Localise)q;                 // trampoline: original 10 bytes, back to +10
  memcpy(q, loc, 10); q += 10;
  q = EmitJmpAbs(q, loc + 10);
  uint8_t *locStub = q;                             // entry stub -> HookLocalise
  q = EmitJmpAbs(q, (void *)&HookLocalise);
  uint8_t *bldStub = q;
  *q++ = 0x48; *q++ = 0x89; *q++ = 0xD9;            // mov rcx, rbx
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x20; // sub rsp, 20h (rsp stays as at the game's own calls)
  *q++ = 0x48; *q++ = 0xB8; { void *f = (void *)&AddRows; memcpy(q, &f, 8); q += 8; }  // mov rax, AddRows
  *q++ = 0xFF; *q++ = 0xD0;                         // call rax
  *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x20; // add rsp, 20h
  *q++ = 0x49; *q++ = 0xBF; { void *cb = exe + 0x003B42C0; memcpy(q, &cb, 8); q += 8; }  // mov r15, Visuals cb
  q = EmitJmpAbs(q, exe + 0x0035514C);
  // 4. Config lookup exe+0x156FC0: `mov rax,rsp ; push rdi ; sub rsp,110h` (11 bytes) moved to a
  //    trampoline, entry jumps to HookCfgGet.
  uint8_t *cfg = exe + 0x00156FC0;
  static const uint8_t kCfgHead[] = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00};
  uint8_t *cfgStub = nullptr;
  if (memcmp(cfg, kCfgHead, sizeof(kCfgHead)) == 0) {
    g_trueCfgGet = (PFN_CfgGet)q;
    memcpy(q, cfg, sizeof(kCfgHead)); q += sizeof(kCfgHead);
    // jmp [rip+0] ; dq target (rax already holds the caller's rsp here)
    *q++ = 0xFF; *q++ = 0x25; *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;
    { const uint8_t *back = cfg + sizeof(kCfgHead); memcpy(q, &back, 8); q += 8; }
    cfgStub = q;
    q = EmitJmpAbs(q, (void *)&HookCfgGet);
  }
  FlushInstructionCache(GetCurrentProcess(), code, (size_t)(q - code));
  bool okC = cfgStub && WriteJmpRel(cfg, cfgStub, 5);
  if (!okC) log_line("[gfx] WARNING: config lookup not hooked; detailed graphics options disabled\r\n");

  bool okL = WriteJmpRel(loc, locStub, 5);
  bool okB = okL && WriteJmpRel(bld, bldStub, 7);
  log_line("[gfx] graphics rows %s (texts %d, builder %d); fps limit %d, vsync %ld\r\n",
           okB ? "installed" : "NOT installed", okL, okB, kFpsChoices[g_fpsIdx], (long)g_vsync);
}

// ---- presentation -------------------------------------------------------------------------------
static void DetectTearing(IDXGIFactory2 *f) {
  IDXGIFactory5 *f5 = nullptr;
  BOOL allow = FALSE;
  if (f && SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory5), (void **)&f5)) && f5) {
    if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) allow = FALSE;
    f5->Release();
  }
  g_tearingSupported = allow != FALSE;
}
static bool TearingFlag(IDXGIFactory2 *f) { DetectTearing(f); return g_tearingSupported; }

static double MonitorHz(HWND hwnd) {
  static double s_hz = 60.0;
  static ULONGLONG s_t = 0;
  ULONGLONG now = GetTickCount64();
  if (s_t && now - s_t < 2000) return s_hz;
  s_t = now;
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
  if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
      dm.dmDisplayFrequency > 1)
    s_hz = dm.dmDisplayFrequency == 59 ? 59.94 : (double)dm.dmDisplayFrequency;
  return s_hz;
}

// Waits until the next frame slot of a `fps` cap (high-resolution timer, then a short spin).
static void Limit(int fps) {
  static LARGE_INTEGER s_freq = {};
  static LONGLONG s_next = 0;
  static HANDLE s_timer = nullptr;
  if (!s_freq.QuadPart) {
    QueryPerformanceFrequency(&s_freq);
    s_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  }
  LONGLONG period = s_freq.QuadPart / fps;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!s_next || now.QuadPart - s_next > period * 4 || s_next - now.QuadPart > period * 4) s_next = now.QuadPart;
  LONGLONG wait = s_next - now.QuadPart;
  LONGLONG spin = s_freq.QuadPart / 1000;  // last ~1 ms spins
  if (wait > spin && s_timer) {
    LARGE_INTEGER due;
    due.QuadPart = -((wait - spin) * 10000000LL / s_freq.QuadPart);
    if (SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(s_timer, 100);
  }
  do { QueryPerformanceCounter(&now); YieldProcessor(); } while (now.QuadPart < s_next);
  s_next += period;
}

// Sync interval and present flags for this frame; runs the frame limiter when needed.
static UINT PresentSync(HWND hwnd, UINT *flags) {
  int cap = kFpsChoices[g_fpsIdx];
  if (g_vsync) {
    double hz = MonitorHz(hwnd);
    if (cap == 0 || cap >= hz * 0.97) return 1;
    int n = (int)(hz / cap + 0.5);
    if (n >= 1 && n <= 4 && fabs(hz / n - cap) <= cap * 0.03) return (UINT)n;  // whole vblanks: smoothest
    Limit(cap);
    return 1;
  }
  if (cap) Limit(cap);
  if (g_tearingSupported) *flags |= DXGI_PRESENT_ALLOW_TEARING;
  return 0;
}

}  // namespace gfxopt
