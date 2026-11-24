// Graphics options as native rows of the game's own Options > GRAPHICS page (no overlay).
//
// The options menu is built in code (exe+0x354E00..0x3559AE): each row is a 0x40-byte item
// {name key, widget type, value, invoker, callback, id, value-key table, value count} appended to the
// page's item vector by exe+0x340DB0. Row and value names are localisation keys looked up by
// exe+0x22DFA0, so new keys ("ID_RL_*") get their text from a hook on that lookup.
//
// Rows (inserted where the console builds add "Visuals" on Xbox One X):
//  * Display mode (borderless / windowed / fullscreen, at once, display.inc), aspect ratio and
//    resolution (the output size: the game's display is created once at start, so a new size is saved
//    for the next start).
//  * Graphics quality: Console (the Xbox One profile) or the PC profiles the developers shipped in
//    code:graphics/viewquality.cfg (QualityConfig-PC-UltraLow .. -UltraHigh: render scale, shadow
//    maps, LOD bias, tessellation). The game applies a quality profile through exe+0x3DCD60 (profile
//    load, photo mode exit, ...); its base-console names are redirected to the chosen profile, so the
//    choice holds everywhere and is applied at once through the game's own reload.
//  * Frame rate limit (30 .. 240, unlimited): time-based limiter in front of Present, independent
//    of the monitor's refresh rate (the game's FlipRate 2 only meant "every other vblank").
//  * Detail rows (render resolution, shadows, world/vehicle detail, tessellation, fog, light effects,
//    debris): real levels; choosing a quality preset moves them all to that preset's levels, and a
//    row that differs overrides the matching viewquality.cfg keys at the game's config lookup
//    (exe+0x156FC0), see "detailed settings" below.
//  * Vertical sync: on = Present waits for vblank; off = tearing allowed (no extra latency).
// Choices persist in the launcher's onrush.ini (ONRUSH_INI) and come back through the environment.

#include <dxgi1_5.h>

namespace lang { static void ShowNotice(const char *title, const char *text); }  // language.inc

namespace ups { static const char *FsrName(); static bool DlssAvailable(); }   // upscaler.inc

namespace gfxopt {

static const char *const kPresetSuffix[] = {"XBoxOne-Base", "PC-UltraLow", "PC-Low", "PC-Medium", "PC-High", "PC-UltraHigh"};
static const int kPresetCount = 6;
static const int kFpsChoices[] = {30, 60, 90, 120, 144, 165, 240, 0};  // 0 = unlimited
static const int kFpsCount = 8;

static volatile LONG g_preset = 0;
static volatile LONG g_fpsIdx = 1;
static volatile LONG g_vsync = 1;
static volatile LONG g_upscaler = 0;   // 0 = the game's TAA, 1 = AMD FSR 3.1 / 4, 2 = NVIDIA DLSS (upscaler.inc)
static const wchar_t *const kUpscalerIni[] = {L"TAA", L"FSR", L"DLSS"};
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
  WritePrivateProfileStringW(L"Graphics", L"Upscaler", kUpscalerIni[g_upscaler >= 0 && g_upscaler < 3 ? g_upscaler : 0], g_ini);
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
  {"ID_RL_RES", {"RENDER RESOLUTION", "RESOLUÇÃO INTERNA", "RÉSOLUTION RENDU", "RES. INTERNA",
                 "RENDERAUFLÖSUNG", "RIS. INTERNA", "ROZDZ. RENDERU"}},
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
  {"ID_RL_NORMAL", {"NORMAL", "NORMAL", "NORMAL", "NORMAL", "NORMAL", "NORMALE", "NORMALNIE"}},
  // display rows
  {"ID_RL_WINMODE", {"DISPLAY MODE", "MODO DE TELA", "MODE D'AFFICHAGE", "MODO DE PANTALLA", "ANZEIGEMODUS",
                     "MODALITÀ SCHERMO", "TRYB EKRANU"}},
  {"ID_RL_WM_BORDERLESS", {"BORDERLESS", "SEM BORDAS", "SANS BORDURE", "SIN BORDES", "RAHMENLOS", "SENZA BORDI",
                           "BEZ RAMEK"}},
  {"ID_RL_WM_WINDOWED", {"WINDOWED", "JANELA", "FENÊTRÉ", "VENTANA", "FENSTER", "FINESTRA", "OKNO"}},
  {"ID_RL_WM_FULLSCREEN", {"FULLSCREEN", "TELA CHEIA", "PLEIN ÉCRAN", "PANTALLA COMPLETA", "VOLLBILD",
                           "SCHERMO INTERO", "PEŁNY EKRAN"}},
  {"ID_RL_ASPECT", {"ASPECT RATIO", "PROPORÇÃO DE TELA", "FORMAT D'IMAGE", "RELACIÓN ASPECTO", "SEITENVERHÄLTNIS",
                    "PROPORZIONI", "PROPORCJE OBRAZU"}},
  {"ID_RL_DISPLAY_RES", {"RESOLUTION", "RESOLUÇÃO", "RÉSOLUTION", "RESOLUCIÓN", "AUFLÖSUNG", "RISOLUZIONE",
                         "ROZDZIELCZOŚĆ"}},
  {"ID_RL_RES_NOTICE", {"The new resolution will be used the next time you start ONRUSH.",
                        "A nova resolução será usada na próxima vez que você iniciar ONRUSH.",
                        "La nouvelle résolution sera utilisée au prochain lancement d'ONRUSH.",
                        "La nueva resolución se usará la próxima vez que inicies ONRUSH.",
                        "Die neue Auflösung wird beim nächsten Start von ONRUSH verwendet.",
                        "La nuova risoluzione verrà usata al prossimo avvio di ONRUSH.",
                        "Nowa rozdzielczość zostanie użyta przy następnym uruchomieniu ONRUSH."}},
  {"ID_RL_UPSCALER", {"UPSCALER", "UPSCALER", "MISE À L'ÉCHELLE", "ESCALADO", "UPSCALER", "UPSCALER", "SKALOWANIE"}},
  {"ID_RL_UPS_TAA", {"TAA", "TAA", "TAA", "TAA", "TAA", "TAA", "TAA"}},
  // FSR and DLSS: the name comes from the upscaler module (FSR 3.1 or FSR 4, by what the GPU runs)
  {"ID_RL_UPS_DLSS_NA", {"DLSS (UNSUPPORTED)", "DLSS (SEM SUPORTE)", "DLSS (NON PRIS EN CHARGE)", "DLSS (NO COMPATIBLE)",
                         "DLSS (NICHT UNTERSTÜTZT)", "DLSS (NON SUPPORTATO)", "DLSS (NIEOBSŁUGIWANE)"}},
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
  // main menu QUIT (main_menu.inc)
  {"ID_RL_QUIT", {"QUIT", "SAIR", "QUITTER", "SALIR", "BEENDEN", "ESCI", "WYJDŹ"}},
  {"ID_RL_QUIT_TITLE", {"QUIT GAME", "SAIR DO JOGO", "QUITTER LE JEU", "SALIR DEL JUEGO", "SPIEL BEENDEN",
                        "ESCI DAL GIOCO", "WYJDŹ Z GRY"}},
  {"ID_RL_QUIT_DESC", {"Close the game and return to the desktop.", "Fechar o jogo e voltar para a área de trabalho.",
                       "Fermer le jeu et revenir au bureau.", "Cerrar el juego y volver al escritorio.",
                       "Das Spiel schließen und zum Desktop zurückkehren.", "Chiudi il gioco e torna al desktop.",
                       "Zamknij grę i wróć na pulpit."}},
  {"ID_RL_QUIT_ASK", {"Are you sure you want to return to the desktop?",
                      "Tem certeza de que deseja voltar para a área de trabalho?",
                      "Voulez-vous vraiment revenir au bureau ?", "¿Seguro que quieres volver al escritorio?",
                      "Möchtest du wirklich zum Desktop zurückkehren?", "Vuoi davvero tornare al desktop?",
                      "Czy na pewno chcesz wrócić na pulpit?"}},
  // Options > CHANGE USER (profile_switch.inc)
  {"ID_RL_PROF_ASK", {"Choose a profile. The game restarts with the chosen profile.",
                      "Escolha um perfil. O jogo reinicia com o perfil escolhido.",
                      "Choisissez un profil. Le jeu redémarre avec le profil choisi.",
                      "Elige un perfil. El juego se reinicia con el perfil elegido.",
                      "Wähle ein Profil. Das Spiel startet mit dem gewählten Profil neu.",
                      "Scegli un profilo. Il gioco si riavvia con il profilo scelto.",
                      "Wybierz profil. Gra uruchomi się ponownie z wybranym profilem."}},
  {"ID_RL_PROF_NONE", {"There are no other profiles. Create one in the Profiles tab of RushLauncher.",
                       "Não há outros perfis. Crie um na aba Perfis do RushLauncher.",
                       "Il n'y a pas d'autre profil. Créez-en un dans l'onglet Profils de RushLauncher.",
                       "No hay otros perfiles. Crea uno en la pestaña Perfiles de RushLauncher.",
                       "Es gibt keine anderen Profile. Erstelle eines im Tab Profile von RushLauncher.",
                       "Non ci sono altri profili. Creane uno nella scheda Profili di RushLauncher.",
                       "Brak innych profili. Utwórz profil w karcie Profile w RushLauncher."}},
  {"ID_RL_PROF_NOLAUNCHER", {"Start the game through RushLauncher to change profiles.",
                             "Abra o jogo pelo RushLauncher para trocar de perfil.",
                             "Lancez le jeu via RushLauncher pour changer de profil.",
                             "Abre el juego desde RushLauncher para cambiar de perfil.",
                             "Starte das Spiel über RushLauncher, um das Profil zu wechseln.",
                             "Avvia il gioco da RushLauncher per cambiare profilo.",
                             "Uruchom grę przez RushLauncher, aby zmienić profil."}},
  {"ID_RL_PROF_DONE", {"Profile changed to %s.", "Perfil trocado para %s.", "Profil changé : %s.",
                       "Perfil cambiado a %s.", "Profil gewechselt zu %s.", "Profilo cambiato in %s.",
                       "Zmieniono profil na %s."}},
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
  // playlist (music.inc)
  {"ID_RL_PLAYLIST", {"PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLISTA"}},
  {"ID_RL_PL_MUSIC", {"MUSIC", "MÚSICA", "MUSIQUE", "MÚSICA", "MUSIK", "MUSICA", "MUZYKA"}},
  {"ID_RL_PL_GAME", {"GAME", "DO JOGO", "DU JEU", "DEL JUEGO", "DES SPIELS", "DEL GIOCO", "Z GRY"}},
  {"ID_RL_PL_MINE", {"PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLIST", "PLAYLISTA"}},
  {"ID_RL_PL_ORDER", {"ORDER", "ORDEM", "ORDRE", "ORDEN", "REIHENFOLGE", "ORDINE", "KOLEJNOŚĆ"}},
  {"ID_RL_PL_SEQ", {"IN ORDER", "SEQUENCIAL", "DANS L'ORDRE", "SECUENCIAL", "DER REIHE NACH", "SEQUENZIALE", "PO KOLEI"}},
  {"ID_RL_PL_SHUF", {"SHUFFLE", "ALEATÓRIA", "ALÉATOIRE", "ALEATORIO", "ZUFÄLLIG", "CASUALE", "LOSOWO"}},
};
// Percentages (same in every language).
// 83% = the Xbox One console profile (1600x900 at 1080p).
static const char *const kPctKeys[] = {"ID_RL_PCT_50", "ID_RL_PCT_67", "ID_RL_PCT_75", "ID_RL_PCT_83",
                                       "ID_RL_PCT_100", "ID_RL_PCT_125", "ID_RL_PCT_150", "ID_RL_PCT_200"};
static const char *const kPctLabels[] = {"50%", "67%", "75%", "83%", "100%", "125%", "150%", "200%"};
static const float kPctScale[] = {0.5f, 0.6667f, 0.75f, 0.8333f, 1.0f, 1.25f, 1.5f, 2.0f};
static const char *const kFpsKeys[] = {"ID_RL_FPS_30", "ID_RL_FPS_60", "ID_RL_FPS_90", "ID_RL_FPS_120",
                                       "ID_RL_FPS_144", "ID_RL_FPS_165", "ID_RL_FPS_240", "ID_RL_FPS_UNLIMITED"};
static const char *const kFpsLabels[] = {"30 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS"};

typedef const char *(*PFN_Localise)(const char *key);
static PFN_Localise g_trueLocalise = nullptr;

static const char *HookLocalise(const char *key) {
  if (key && key[0] == 'I' && strncmp(key, "ID_RL_", 6) == 0) {
    if (strncmp(key + 6, "UPS_", 4) == 0) {
      if (strcmp(key + 10, "FSR") == 0) return ups::FsrName();
      if (strcmp(key + 10, "DLSS") == 0) {
        if (ups::DlssAvailable()) return "DLSS";
        key = "ID_RL_UPS_DLSS_NA";
      }
    }
    for (int i = 0; i < 7; ++i)
      if (strcmp(key, kFpsKeys[i]) == 0) return kFpsLabels[i];
    for (int i = 0; i < (int)_countof(kPctKeys); ++i)
      if (strcmp(key, kPctKeys[i]) == 0) return kPctLabels[i];
    // aspect ratios and resolutions: the text is the key's tail ("ID_RL_AR_16:9", "ID_RL_DR_2560x1440")
    // profile names and composed sentences too ("ID_RL_PN_<name>", "ID_RL_TX_<text>", profile_switch.inc)
    if ((key[6] == 'A' && key[7] == 'R' || key[6] == 'D' && key[7] == 'R' || key[6] == 'P' && key[7] == 'N' ||
         key[6] == 'T' && key[7] == 'X') && key[8] == '_')
      return key + 9;
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
// quality preset; a row at its preset level leaves the preset's values.
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
  const char *const *values;   // value keys (real levels; the quality preset picks one per row)
  uint32_t count;
};
// No "off": the race view is not built for running without shadows (the game never does it; forcing
// Shadows/enabled = false there crashes at exe+0x128CC0).
static const char *const kShadowVals[] = {"ID_RL_Q_LOW", "ID_RL_Q_MEDIUM", "ID_RL_Q_HIGH", "ID_RL_Q_ULTRA"};
static const char *const kLodVals[] = {"ID_RL_M_LOW", "ID_RL_M_MEDIUM", "ID_RL_M_HIGH", "ID_RL_Q_ULTRA"};
static const char *const kTessVals[] = {"ID_RL_OFF_F", "ID_RL_ON_F"};
static const char *const kOnOffMpVals[] = {"ID_RL_OFF_MP", "ID_RL_ON_MP"};
static const char *const kFogVals[] = {"ID_RL_OFF_F", "ID_RL_Q_MEDIUM", "ID_RL_Q_HIGH"};
static const char *const kDebrisVals[] = {"ID_RL_FEW", "ID_RL_NORMAL", "ID_RL_MANY"};
static const DetailRow kDetail[D_COUNT] = {
  {"ID_RL_RES", L"RenderScale", kPctKeys, _countof(kPctKeys)},
  {"ID_RL_SHADOWS", L"Shadows", kShadowVals, _countof(kShadowVals)},
  {"ID_RL_WORLD", L"WorldDetail", kLodVals, _countof(kLodVals)},
  {"ID_RL_VEHICLES", L"VehicleDetail", kLodVals, _countof(kLodVals)},
  {"ID_RL_TESS", L"Tessellation", kTessVals, _countof(kTessVals)},
  {"ID_RL_FOG", L"Fog", kFogVals, _countof(kFogVals)},
  {"ID_RL_LIGHTFX", L"LightEffects", kOnOffMpVals, _countof(kOnOffMpVals)},
  {"ID_RL_DEBRIS", L"Debris", kDebrisVals, _countof(kDebrisVals)},
};
static volatile LONG g_detail[D_COUNT] = {};

// The level of every row that matches each quality preset exactly (viewquality.cfg; order Console,
// UltraLow, Low, Medium, High, Ultra). Choosing a preset moves every row to these levels; a row only
// overrides the profile when it differs from its preset level.
//   render scale: Console 0.8333, UltraLow 0.5, Low 0.71 capped at 1280x720 (= 67% at 1080p),
//                 Medium/High 1.0, Ultra 1.5      LODs: LowQualityLods / DefaultLods / HighQualityLods
//   tessellation: NoTessellation / DefaultTessellation   shadows, fog, light effects, debris: no preset
//   changes them (DefaultShadows = MEDIUM, VolumetricFogResolutionScale 0.5 = MEDIUM, on, normal).
static const uint8_t kPresetLevel[D_COUNT][6] = {
  {3, 0, 1, 4, 4, 6},  // D_RES (kPctScale index)
  {1, 1, 1, 1, 1, 1},  // D_SHADOWS
  {0, 0, 0, 1, 2, 2},  // D_WORLD
  {0, 0, 0, 1, 2, 2},  // D_VEHICLES
  {0, 0, 0, 1, 1, 1},  // D_TESS
  {1, 1, 1, 1, 1, 1},  // D_FOG
  {1, 1, 1, 1, 1, 1},  // D_LIGHTFX
  {1, 1, 1, 1, 1, 1},  // D_DEBRIS
};
static LONG PresetLevel(int d, LONG p) { return kPresetLevel[d][p >= 0 && p < kPresetCount ? p : 0]; }
// Row d differs from the current preset (an override rule is needed).
static bool Custom(int d) { return g_detail[d] != PresetLevel(d, g_preset); }

enum { kMaxRules = 96 };
static Rule g_rules[kMaxRules];
static volatile LONG g_ruleCount = 0;
static SRWLOCK g_ruleLock = SRWLOCK_INIT;

static void AddRule(Rule *r, LONG &n, const char *pattern, uint32_t type, RuleMode mode, float f) {
  if (n < kMaxRules) r[n++] = Rule{pattern, type, mode, f};
}

// LOD levels LOW / MEDIUM / HIGH / ULTRA: LOW = LowQualityLods, MEDIUM = DefaultLods, HIGH = HighQualityLods.
static const float kWorldBias[] = {0.25f, 0.0f, -0.5f, -1.0f};
static const float kVehBias[] = {0.25f, 0.0f, -0.5f, -1.0f};
static const float kViewedBias[] = {0.25f, -0.25f, -0.5f, -1.0f};
static const float kCharBias[] = {0.5f, 0.0f, 0.0f, -0.5f};
static const float kMinLod[] = {1.0f, 0.0f, 0.0f, 0.0f};

static void BuildRules() {
  Rule r[kMaxRules];
  LONG n = 0;
  LONG v = g_detail[D_RES];
  {
    // Render size = output size x scale, capped by MaxRender*: the cap is set to exactly what the chosen
    // scale needs at the output size (render targets are sized from it, so a larger cap only wastes GPU
    // memory). Always set: the profiles' caps are made for a 1920x1080 output (at 1920x1080 the rule
    // gives each preset's own render size).
    float s = kPctScale[v];
    int w = display::g_w, h = display::g_h;
    AddRule(r, n, "Memory/DisplayToRenderScale", CT_FLOAT, RM_SET, s);
    AddRule(r, n, "Memory/MaxRenderWidth", CT_INT, RM_SET, (float)(((int)(w * s) + 7) & ~7));
    AddRule(r, n, "Memory/MaxRenderHeight", CT_INT, RM_SET, (float)(((int)(h * s) + 7) & ~7));
  }
  v = g_detail[D_SHADOWS];
  if (Custom(D_SHADOWS)) {  // LOW / MEDIUM (= DefaultShadows) / HIGH / ULTRA: filter taps and cascade reach
    static const float kLookups[] = {4, 8, 12, 12}, kDist[] = {0.6f, 1.0f, 1.3f, 1.7f};
    AddRule(r, n, "EventRenderViewConfig/ShadowCascade:*/numLookups", CT_INT, RM_SET, kLookups[v]);
    AddRule(r, n, "EventRenderViewConfig/ShadowCascade:*/distance", CT_FLOAT, RM_MUL, kDist[v]);
  }
  static const char *const kWorldCats[] = {"CategoryLodBias/Default", "*/CategoryLodBias/Default",
                                           "CategoryLodBias/Fodder", "*/CategoryLodBias/Fodder",
                                           "CategoryLodBias/ProceduralPlacement", "*/CategoryLodBias/ProceduralPlacement"};
  static const char *const kVehCats[] = {"CategoryLodBias/PlayerVehicle", "*/CategoryLodBias/PlayerVehicle",
                                         "CategoryLodBias/AIVehicle", "*/CategoryLodBias/AIVehicle"};
  static const char *const kViewedCats[] = {"CategoryLodBias/ViewedVehicle", "*/CategoryLodBias/ViewedVehicle",
                                            "CategoryLodBias/HubViewedVehicle", "*/CategoryLodBias/HubViewedVehicle"};
  static const char *const kCharCats[] = {"CategoryLodBias/Character", "*/CategoryLodBias/Character",
                                          "CategoryLodBias/HubCharacter", "*/CategoryLodBias/HubCharacter"};
  static const char *const kVehMin[] = {"CategoryMinLod/ViewedVehicle", "*/CategoryMinLod/ViewedVehicle",
                                        "CategoryMinLod/PlayerVehicle", "*/CategoryMinLod/PlayerVehicle",
                                        "CategoryMinLod/AIVehicle", "*/CategoryMinLod/AIVehicle",
                                        "CategoryMinLod/Character", "*/CategoryMinLod/Character"};
  v = g_detail[D_WORLD];
  if (Custom(D_WORLD))
    for (const char *k : kWorldCats) AddRule(r, n, k, CT_FLOAT, RM_SET, kWorldBias[v]);
  v = g_detail[D_VEHICLES];
  if (Custom(D_VEHICLES)) {
    for (const char *k : kVehCats) AddRule(r, n, k, CT_FLOAT, RM_SET, kVehBias[v]);
    for (const char *k : kViewedCats) AddRule(r, n, k, CT_FLOAT, RM_SET, kViewedBias[v]);
    for (const char *k : kCharCats) AddRule(r, n, k, CT_FLOAT, RM_SET, kCharBias[v]);
    for (const char *k : kVehMin) AddRule(r, n, k, CT_INT, RM_SET, kMinLod[v]);
  }
  v = g_detail[D_TESS];
  if (Custom(D_TESS) && v == 0) {
    AddRule(r, n, "Tessellation/enabled", CT_BOOL, RM_SET, 0);
    AddRule(r, n, "Tessellation/castShadowsFromTessellatedTerrain", CT_BOOL, RM_SET, 0);
  } else if (Custom(D_TESS)) {  // viewquality.cfg DefaultTessellation
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
  v = g_detail[D_FOG];  // OFF / MEDIUM (the game's half-resolution volumetric fog) / HIGH (full resolution)
  if (Custom(D_FOG)) {
    if (v == 0) AddRule(r, n, "EventRenderViewConfig/Atmospherics/enabled", CT_BOOL, RM_SET, 0);
    else AddRule(r, n, "Memory/VolumetricFogResolutionScale", CT_FLOAT, RM_SET, v == 1 ? 0.5f : 1.0f);
  }
  v = g_detail[D_LIGHTFX];
  if (Custom(D_LIGHTFX)) {
    float on = v == 1 ? 1.0f : 0.0f;
    AddRule(r, n, "EventRenderViewConfig/Sky/godRays", CT_BOOL, RM_SET, on);
    AddRule(r, n, "EventRenderViewConfig/LensFlare/enabled", CT_BOOL, RM_SET, on);
    AddRule(r, n, "EventRenderViewConfig/LensDust/enabled", CT_BOOL, RM_SET, on);
  }
  v = g_detail[D_DEBRIS];  // FEW / NORMAL / MANY
  if (Custom(D_DEBRIS)) AddRule(r, n, "Debris/*", CT_INT, RM_MUL, v == 0 ? 0.5f : v == 2 ? 2.0f : 1.0f);

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
enum : uint32_t { kIdQuality = 0x52, kIdFps = 0x53, kIdVsync = 0x54, kIdDetail0 = 0x55, kIdUpscaler = 0x5D,
                  kIdWinMode = 0x5E, kIdAspect = 0x5F, kIdDisplayRes = 0x60 };

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

// Detail rows: real levels; choosing a quality preset moves every row to that preset's level
// (kPresetLevel). Profile-level keys (render scale, tessellation, fog resolution, global LOD) are
// re-read by re-applying the profile; race-view keys at the next race.
static void SaveDetail(int d) {
  if (!g_ini[0]) return;
  wchar_t b[16];
  swprintf_s(b, L"%ld", (long)g_detail[d]);
  WritePrivateProfileStringW(L"Graphics", kDetail[d].ini, b, g_ini);
  WritePrivateProfileStringW(L"Graphics", L"DetailLevels", L"1", g_ini);
}

// Option pages our rows were added to (the game builds the options menu more than once; each page's item
// vector: data at +8, byte size at +0x10, see exe+0x340DB0). The rows read their value (+0x0C) every
// frame, so when the preset moves the detail rows their values are rewritten in every such page whose
// items are still ours (same name key and invoker).
static uint8_t *volatile g_pages[8] = {};
static volatile LONG g_pageNext = 0;
static void RememberPage(uint8_t *page) {
  for (uint8_t *p : g_pages) if (p == page) return;
  g_pages[(InterlockedIncrement(&g_pageNext) - 1) & 7] = page;
}
// Rewrites row `id` (named `name`) in every remembered page: its value and, when `values` is given, its
// value list.
static void SetRow(uint32_t id, const char *name, float value, const char *const *values = nullptr, uint32_t count = 0) {
  void *invoker = (uint8_t *)g_exe + 0x0035E960;
  for (int k = 0; k < 8; ++k) {
    uint8_t *page = g_pages[k];
    if (!page) continue;
    __try {
      MenuItem *items = *(MenuItem **)(page + 8);
      size_t bytes = *(size_t *)(page + 0x10), n = bytes / sizeof(MenuItem);
      if (!items || n > 256 || n * sizeof(MenuItem) != bytes) { g_pages[k] = nullptr; continue; }
      for (size_t i = 0; i < n; ++i) {
        MenuItem &it = items[i];
        if (it.id != id || it.type != 6 || it.name != name || it.invoker != invoker) continue;
        if (values) { it.values = values; it.count = count; }
        it.value = value;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_pages[k] = nullptr;
    }
  }
}
static void ShowDetailValues() {
  for (int d = 0; d < D_COUNT; ++d) SetRow(kIdDetail0 + d, kDetail[d].name, (float)g_detail[d]);
}

// ---- display rows -------------------------------------------------------------------------------
// Display mode: at once (display.inc). Aspect ratio and resolution: the output size, saved to
// [Display] Width / Height for the next start (the game creates its display once, at start).
static const char *const kWinModeKeys[display::M_COUNT] = {"ID_RL_WM_BORDERLESS", "ID_RL_WM_WINDOWED", "ID_RL_WM_FULLSCREEN"};
static const char *const kAspectKeys[] = {"ID_RL_AR_16:9", "ID_RL_AR_16:10", "ID_RL_AR_21:9", "ID_RL_AR_32:9"};
static const char *const kRes16x9[] = {"ID_RL_DR_1280x720", "ID_RL_DR_1600x900", "ID_RL_DR_1920x1080",
                                       "ID_RL_DR_2560x1440", "ID_RL_DR_3840x2160"};
static const char *const kRes16x10[] = {"ID_RL_DR_1280x800", "ID_RL_DR_1440x900", "ID_RL_DR_1680x1050",
                                        "ID_RL_DR_1920x1200", "ID_RL_DR_2560x1600"};
static const char *const kRes21x9[] = {"ID_RL_DR_2560x1080", "ID_RL_DR_3440x1440", "ID_RL_DR_3840x1600", "ID_RL_DR_5120x2160"};
static const char *const kRes32x9[] = {"ID_RL_DR_3840x1080", "ID_RL_DR_5120x1440"};
static const struct { const char *const *res; uint32_t count; uint32_t def; } kAspectRes[] = {
  {kRes16x9, _countof(kRes16x9), 2}, {kRes16x10, _countof(kRes16x10), 3},
  {kRes21x9, _countof(kRes21x9), 0}, {kRes32x9, _countof(kRes32x9), 0}};
static const int kAspectCount = (int)_countof(kAspectKeys);
static_assert(_countof(kAspectKeys) == _countof(kAspectRes), "one resolution list per aspect ratio");

static volatile LONG g_aspect = 0, g_dispRes = 2;   // the chosen output (may differ from the running one)
static bool g_resNoticeShown = false;

static void ResSize(int aspect, int idx, int *w, int *h) {
  *w = 1920; *h = 1080;
  sscanf_s(kAspectRes[aspect].res[idx] + 9, "%dx%d", w, h);
}

// The list entry of the running output size (else the nearest aspect ratio and width).
static void PickDisplayRow() {
  int bestA = 0, bestI = 0;
  double bestD = 1e9;
  for (int a = 0; a < kAspectCount; ++a)
    for (uint32_t i = 0; i < kAspectRes[a].count; ++i) {
      int w, h;
      ResSize(a, (int)i, &w, &h);
      double d = fabs((double)w / h - (double)display::g_w / display::g_h) * 100000.0 + abs(w - display::g_w);
      if (d < bestD) { bestD = d; bestA = a; bestI = (int)i; }
    }
  g_aspect = bestA; g_dispRes = bestI;
}

static void SaveDisplaySize() {
  int w, h;
  ResSize(g_aspect, g_dispRes, &w, &h);
  if (g_ini[0]) {
    wchar_t b[16];
    swprintf_s(b, L"%d", w); WritePrivateProfileStringW(L"Display", L"Width", b, g_ini);
    swprintf_s(b, L"%d", h); WritePrivateProfileStringW(L"Display", L"Height", b, g_ini);
  }
  bool next = w != display::g_w || h != display::g_h;
  log_line("[gfx] resolution -> %dx%d%s\r\n", w, h, next ? " (next start)" : " (running)");
  if (next && !g_resNoticeShown) {
    g_resNoticeShown = true;
    lang::ShowNotice("ID_RL_DISPLAY_RES", "ID_RL_RES_NOTICE");
  }
}

static void OnWinMode(float value, int) {
  LONG m = ListIndex(value, display::M_COUNT);
  if (m != display::g_mode) display::SetMode(g_desktopHwnd, m);
}
// display.inc, after any mode change (menu row or Alt+Enter)
static void OnModeChanged(LONG mode) {
  if (g_ini[0]) WritePrivateProfileStringW(L"Display", L"WindowMode", display::kModeNames[mode], g_ini);
  SetRow(kIdWinMode, "ID_RL_WINMODE", (float)mode);
}
static void OnAspect(float value, int) {
  LONG a = ListIndex(value, kAspectCount);
  if (a == g_aspect) return;
  g_aspect = a;
  g_dispRes = (LONG)kAspectRes[a].def;
  SetRow(kIdDisplayRes, "ID_RL_DISPLAY_RES", (float)g_dispRes, kAspectRes[a].res, kAspectRes[a].count);
  SaveDisplaySize();
}
static void OnDisplayRes(float value, int) {
  LONG i = ListIndex(value, (int)kAspectRes[g_aspect].count);
  if (i == g_dispRes) return;
  g_dispRes = i;
  SaveDisplaySize();
}

static void OnQuality(float value, int) {
  LONG p = ListIndex(value, kPresetCount);
  if (p == g_preset) return;
  g_preset = p;
  for (int d = 0; d < D_COUNT; ++d) { g_detail[d] = PresetLevel(d, p); SaveDetail(d); }
  BuildRules();
  ShowDetailValues();
  UpdateProfileNames();
  ApplyQualityProfile();
  SaveIni();
  log_line("[gfx] graphics quality -> %s (detail rows moved to its levels)\r\n", g_qcName);
}
static void OnFps(float value, int) {
  g_fpsIdx = ListIndex(value, kFpsCount);
  SaveIni();
  log_line("[gfx] frame rate limit -> %d\r\n", kFpsChoices[g_fpsIdx]);
}
static const char *const kUpscalerKeys[] = {"ID_RL_UPS_TAA", "ID_RL_UPS_FSR", "ID_RL_UPS_DLSS"};
static void OnUpscaler(float value, int) {
  g_upscaler = ListIndex(value, 3);
  SaveIni();
  log_line("[gfx] upscaler -> %ls\r\n", kUpscalerIni[g_upscaler]);
}
static void OnVsync(float value, int) {
  g_vsync = ListIndex(value, 2);
  SaveIni();
  log_line("[gfx] vertical sync -> %ld\r\n", (long)g_vsync);
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
// Test hook (no menu needed): ONRUSH_GFX_SWITCH="40:5,70:0" picks quality preset 5 at 40 s and 0 at 70 s after
// the first gamepad poll, through the same callback as the menu row (game thread).
static void DiagTick() {
  static int s_n = -1; static double s_t[8]; static int s_v[8]; static ULONGLONG s_t0 = 0;
  if (s_n < 0) {
    s_n = 0; s_t0 = GetTickCount64();
    char b[128] = {};
    if (GetEnvironmentVariableA("ONRUSH_GFX_SWITCH", b, sizeof(b)) > 0) {
      char *ctx = nullptr;
      for (char *tok = strtok_s(b, ",", &ctx); tok && s_n < 8; tok = strtok_s(nullptr, ",", &ctx)) {
        char *c = strchr(tok, ':');
        if (c) { s_t[s_n] = atof(tok); s_v[s_n] = atoi(c + 1); ++s_n; }
      }
    }
  }
  double now = (GetTickCount64() - s_t0) / 1000.0;
  for (int i = 0; i < s_n; ++i)
    if (s_t[i] >= 0 && now >= s_t[i]) { s_t[i] = -1; log_line("[gfx] test switch -> preset %d\r\n", s_v[i]); OnQuality((float)s_v[i], 0); }
}
template <int D> static void OnDetailRow(float value, int) { OnDetail(D, value); }
static void *const kDetailCallbacks[D_COUNT] = {
  (void *)&OnDetailRow<0>, (void *)&OnDetailRow<1>, (void *)&OnDetailRow<2>, (void *)&OnDetailRow<3>,
  (void *)&OnDetailRow<4>, (void *)&OnDetailRow<5>, (void *)&OnDetailRow<6>, (void *)&OnDetailRow<7>};
static_assert(D_COUNT == 8, "one callback per detail row");

static void AddRows(void *page) {
  RememberPage((uint8_t *)page);
  typedef void (*PFN_AddItem)(void *page, const MenuItem *item);
  PFN_AddItem add = (PFN_AddItem)((uint8_t *)g_exe + 0x00340DB0);
  void *invoker = (uint8_t *)g_exe + 0x0035E960;
  MenuItem it = {};
  it.type = 6;
  it.invoker = invoker;

  it.name = "ID_RL_WINMODE"; it.callback = (void *)&OnWinMode; it.id = kIdWinMode;
  it.values = kWinModeKeys; it.count = display::M_COUNT; it.value = (float)display::g_mode;
  add(page, &it);
  it.name = "ID_RL_ASPECT"; it.callback = (void *)&OnAspect; it.id = kIdAspect;
  it.values = kAspectKeys; it.count = kAspectCount; it.value = (float)g_aspect;
  add(page, &it);
  it.name = "ID_RL_DISPLAY_RES"; it.callback = (void *)&OnDisplayRes; it.id = kIdDisplayRes;
  it.values = kAspectRes[g_aspect].res; it.count = kAspectRes[g_aspect].count; it.value = (float)g_dispRes;
  add(page, &it);

  it.name = "ID_RL_GFX_QUALITY"; it.callback = (void *)&OnQuality; it.id = kIdQuality;
  it.values = kPresetKeys; it.count = kPresetCount; it.value = (float)g_preset;
  add(page, &it);
  for (int d = 0; d < D_COUNT; ++d) {
    it.name = kDetail[d].name; it.callback = kDetailCallbacks[d]; it.id = kIdDetail0 + d;
    it.values = kDetail[d].values; it.count = kDetail[d].count; it.value = (float)g_detail[d];
    add(page, &it);
    if (d == D_RES) {
      it.name = "ID_RL_UPSCALER"; it.callback = (void *)&OnUpscaler; it.id = kIdUpscaler;
      it.values = kUpscalerKeys; it.count = 3; it.value = (float)g_upscaler;
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

// GPU memory block node layout after step 5a of Install.
static const uint32_t kNodeStride = 0x55C0;       // was 0xAB8
static const uint32_t kNodeMaxPages = 2048;       // page pointers at +0xA0 (was 256)
static const uint32_t kNodePageIds = 0x40A0;      // u16 page ids (was +0x8A0)
static const uint32_t kNodeListCount = 0x50A0;    // u32 list count (was +0xAA0)
static const uint32_t kNodeList = 0x50A8;         // list entries (was +0xAA8)

// Bounded copy of a texture's memory-alias list (see Install, step 5).
static void CopyAliasList(const uint8_t *node, uint64_t *slots, uint32_t slotCount) {
  uint32_t n = *(const uint32_t *)(node + kNodeListCount);
  if (n > slotCount) {
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 8)
      log_line("[gfx] texture alias list %u > %u slots: extra entries dropped\r\n", n, slotCount);
    n = slotCount;
  }
  const uint64_t *src = (const uint64_t *)(node + kNodeList);
  for (uint32_t i = 0; i < n; ++i) slots[i] = src[i];
}

static void Install(HMODULE hExe) {
  g_exe = hExe;
  display::Init();
  display::g_onModeChanged = &OnModeChanged;
  PickDisplayRow();
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
    // ONRUSH_UPSCALER overrides onrush.ini [Graphics] Upscaler (TAA / FSR / DLSS; "FSR2" from older versions = FSR)
    wchar_t u[16] = {};
    if (!GetEnvironmentVariableW(L"ONRUSH_UPSCALER", u, 16) && g_ini[0]) GetPrivateProfileStringW(L"Graphics", L"Upscaler", L"TAA", u, 16, g_ini);
    g_upscaler = (_wcsicmp(u, L"FSR") == 0 || _wcsicmp(u, L"FSR2") == 0) ? 1 : _wcsicmp(u, L"DLSS") == 0 ? 2 : 0;
  }
  // Detail rows: [Graphics] DetailLevels=1 = level indexes; older inis had 0 = AUTO (-> the preset's
  // level) and every level one higher (debris: 1 few, 2 many). Missing keys take the preset's level.
  {
    bool levels = g_ini[0] && GetPrivateProfileIntW(L"Graphics", L"DetailLevels", 0, g_ini) >= 1;
    for (int d = 0; d < D_COUNT; ++d) {
      int v = g_ini[0] ? (int)GetPrivateProfileIntW(L"Graphics", kDetail[d].ini, -1, g_ini) : -1;
      if (!levels) v = v >= 1 ? ((d == D_DEBRIS && v == 2) ? 2 : v - 1) : -1;
      g_detail[d] = v >= 0 && v < (int)kDetail[d].count ? (LONG)v : PresetLevel(d, g_preset);
    }
    if (g_ini[0] && !levels)
      for (int d = 0; d < D_COUNT; ++d) SaveDetail(d);
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
  // 3b. Second half of the builder: the single options page of the pause menu (name tags .. speaker type,
  //     exe+0x355556..) ends with the same Visuals row ("Elementos visuais", exe+0x355933: `cmp byte
  //     [Rendering/FPSVsQualityOption], r13b ; je 0x355988`). It is replaced by our rows, so the pause
  //     menu has the same graphics options as Options > GRAPHICS; 0x355988 is the builder's epilogue.
  uint8_t *pau = exe + 0x00355933;
  uint8_t *pauStub = nullptr;
  if (memcmp(pau, "\x44\x38\x2D", 3) == 0 && pau[7] == 0x74 && memcmp(exe + 0x00355988, "\x4C\x8D\x9C\x24", 4) == 0) {
    pauStub = q;
    *q++ = 0x48; *q++ = 0x89; *q++ = 0xD9;              // mov rcx, rbx (the pause options page)
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x20; // sub rsp, 20h
    *q++ = 0x48; *q++ = 0xB8; { void *f = (void *)&AddRows; memcpy(q, &f, 8); q += 8; }  // mov rax, AddRows
    *q++ = 0xFF; *q++ = 0xD0;                           // call rax
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x20; // add rsp, 20h
    q = EmitJmpAbs(q, exe + 0x00355988);
  }
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
  // 5. Texture memory aliasing list (exe+0xB5B70, called from the texture create path exe+0xB3830): the
  //    list of resources sharing memory with the new texture ([node+0xAA0] count, entries at +0xAA8) is
  //    copied into the texture's own slots, sized by the caller (2) and zero-filled with that count, but
  //    the copy loop at exe+0xB5C62 is not bounded by it. A profile switched in game (e.g. Console ->
  //    Ultra: bigger shadow maps in a heap already in use) gives longer lists and the copy ran over the
  //    texture object (crash at exe+0xB5C78 or later). The loop now stops at the caller's size.
  uint8_t *ali = exe + 0x000B5C62;
  static const uint8_t kAliHead[] = {0x48, 0x8B, 0x7E, 0x08, 0x33, 0xD2, 0x39, 0x97, 0xA0, 0x0A, 0x00, 0x00};
  uint8_t *aliStub = nullptr;
  if (memcmp(ali, kAliHead, sizeof(kAliHead)) == 0 && memcmp(exe + 0x000B5C86, "\x48\x8B\x74\x24\x48", 5) == 0) {
    aliStub = q;
    *q++ = 0x48; *q++ = 0x8B; *q++ = 0x4E; *q++ = 0x08;          // mov rcx, [rsi+8]   (graph node)
    *q++ = 0x48; *q++ = 0x89; *q++ = 0xDA;                        // mov rdx, rbx       (texture slots)
    *q++ = 0x44; *q++ = 0x8B; *q++ = 0x44; *q++ = 0x24; *q++ = 0x68;  // mov r8d, [rsp+68h] (slot count)
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x20;          // sub rsp, 20h (rsp is 16-aligned here)
    *q++ = 0x48; *q++ = 0xB8; { void *f = (void *)&CopyAliasList; memcpy(q, &f, 8); q += 8; }
    *q++ = 0xFF; *q++ = 0xD0;                                     // call rax
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x20;          // add rsp, 20h
    q = EmitJmpAbs(q, exe + 0x000B5C86);
  }
  FlushInstructionCache(GetCurrentProcess(), code, (size_t)(q - code));
  bool okA = aliStub && WriteJmpRel(ali, aliStub, 5);
  if (!okA) log_line("[gfx] WARNING: texture alias copy not bounded (unexpected code at exe+0xB5C62)\r\n");
  // 5a. GPU memory block nodes (pool of 48 nodes of 0xAB8 bytes, exe+0xA9450 from 0xAABF3): page
  //     pointers at +0xA0 and their u16 ids at +0x8A0 (room for 256 pages of 64 KB = 16 MB), then a
  //     list at +0xAA8 ([+0xAA0] count, room for 2). Nothing bounds them: a render target over 16 MB
  //     (Ultra applied in game: 2880x1620 and up) wrote its page pointers over the ids, the count and
  //     the next node (crashes at exe+0xB5C78 / 0xC122B, or later). The node grows to 0x55C0 bytes and
  //     its fields move: 2048 pages (128 MB), ids at +0x40A0, list count +0x50A0, list +0x50A8 (163).
  //     Patched: pool size and node address (imul), node index (x * magic >> (64 + 10): 8x stride ->
  //     shift 13), node delete size, the two memsets of pages+ids, every field displacement. The alias
  //     copy loop at exe+0xB5C68..0xB5C7E is replaced by the stub above, so it is only patched with it.
  if (okA) {
    struct Patch { uint32_t rva; uint8_t len; uint8_t expect[8]; uint8_t at; uint32_t val; uint8_t vlen; };
    static const Patch kNode[] = {
      {0x000A949F, 7, {0x48, 0x69, 0xD5, 0xB8, 0x0A, 0x00, 0x00}, 3, kNodeStride, 4},        // imul rdx,rbp,size
      {0x000AE2CF, 7, {0x48, 0x69, 0xF9, 0xB8, 0x0A, 0x00, 0x00}, 3, kNodeStride, 4},        // imul rdi,rcx,size
      {0x000ADCA0, 5, {0xBA, 0xB8, 0x0A, 0x00, 0x00}, 1, kNodeStride, 4},                    // mov edx,size
      {0x000B3EED, 4, {0x48, 0xC1, 0xFE, 0x0A}, 3, 0x0D, 1},                                 // sar rsi,10
      {0x000B6568, 4, {0x48, 0xC1, 0xFD, 0x0A}, 3, 0x0D, 1},                                 // sar rbp,10
      {0x000AE2F7, 6, {0x41, 0xB8, 0x00, 0x0A, 0x00, 0x00}, 2, kNodeListCount - 0xA0, 4},    // memset pages+ids
      {0x000C1297, 6, {0x41, 0xB8, 0x00, 0x0A, 0x00, 0x00}, 2, kNodeListCount - 0xA0, 4},
      {0x000BF46B, 7, {0x48, 0x81, 0xC2, 0xA0, 0x08, 0x00, 0x00}, 3, kNodePageIds, 4},       // page ids
      {0x000BF478, 8, {0x48, 0x8D, 0x8C, 0x46, 0xA0, 0x08, 0x00, 0x00}, 4, kNodePageIds, 4},
      {0x000BF50D, 8, {0x0F, 0xB7, 0x84, 0x5E, 0xA0, 0x08, 0x00, 0x00}, 4, kNodePageIds, 4},
      {0x000C118B, 8, {0x66, 0x89, 0x8C, 0x43, 0xA0, 0x08, 0x00, 0x00}, 4, kNodePageIds, 4},
      {0x000C1198, 8, {0x0F, 0xB7, 0x8C, 0x43, 0xA0, 0x08, 0x00, 0x00}, 4, kNodePageIds, 4},
      {0x000C1223, 8, {0x0F, 0xB7, 0x84, 0x7B, 0xA0, 0x08, 0x00, 0x00}, 4, kNodePageIds, 4},
      {0x000ADC95, 6, {0x89, 0x81, 0xA0, 0x0A, 0x00, 0x00}, 2, kNodeListCount, 4},           // list count
      {0x000AE306, 7, {0x44, 0x89, 0xAF, 0xA0, 0x0A, 0x00, 0x00}, 3, kNodeListCount, 4},
      {0x000AE360, 6, {0x8B, 0x87, 0xA0, 0x0A, 0x00, 0x00}, 2, kNodeListCount, 4},
      {0x000AE37E, 6, {0xFF, 0x87, 0xA0, 0x0A, 0x00, 0x00}, 2, kNodeListCount, 4},
      {0x000AE3DD, 6, {0x8B, 0x81, 0xA0, 0x0A, 0x00, 0x00}, 2, kNodeListCount, 4},
      {0x000AE3F7, 6, {0xFF, 0x81, 0xA0, 0x0A, 0x00, 0x00}, 2, kNodeListCount, 4},
      {0x000BF3A9, 6, {0x8B, 0x86, 0xA0, 0x0A, 0x00, 0x00}, 2, kNodeListCount, 4},
      {0x000BF5C4, 7, {0x48, 0x8D, 0x93, 0xA0, 0x0A, 0x00, 0x00}, 3, kNodeListCount, 4},
      {0x000C11CA, 7, {0x83, 0xBA, 0xA0, 0x0A, 0x00, 0x00, 0x00}, 2, kNodeListCount, 4},
      {0x000AE366, 8, {0x48, 0x8D, 0x94, 0xC7, 0xA8, 0x0A, 0x00, 0x00}, 4, kNodeList, 4},    // list entries
      {0x000AE3E3, 8, {0x48, 0x8D, 0x94, 0xC1, 0xA8, 0x0A, 0x00, 0x00}, 4, kNodeList, 4},
      {0x000BF39B, 7, {0x4C, 0x8D, 0xB6, 0xA8, 0x0A, 0x00, 0x00}, 3, kNodeList, 4},
      {0x000BF3B2, 8, {0x48, 0x8D, 0x8C, 0xC6, 0xA8, 0x0A, 0x00, 0x00}, 4, kNodeList, 4},
    };
    static_assert(kNodePageIds == 0xA0 + kNodeMaxPages * 8 && kNodeListCount == kNodePageIds + kNodeMaxPages * 2, "node layout");
    bool all = true;
    for (const Patch &p : kNode)
      if (memcmp(exe + p.rva, p.expect, p.len) != 0) { all = false; log_line("[gfx] node patch: unexpected code at exe+0x%X\r\n", p.rva); }
    if (all)
      for (const Patch &p : kNode) WriteCode(exe + p.rva + p.at, &p.val, p.vlen);
    log_line("[gfx] GPU memory nodes: %s\r\n", all ? "2048 pages each" : "NOT enlarged");
  }
  // 6. Output size: exe+0xAA9E0 creates the game's display (its size at +0x2C/+0x30, the swap chain and
  //    every full-screen target follow it) as 1920x1080 (`mov eax,780h ; mov ecx,438h`, Xbox One) or
  //    3840x2160 (Xbox One X branch). Both get [Display] Width x Height. The UI keeps its 16:9 canvas,
  //    fitted and centred; the 3D view fills the output (wider or taller field of view).
  {
    uint8_t *one = exe + 0x000AAAC7, *onex = exe + 0x000AAABB;
    bool okS = memcmp(one, "\xB8\x80\x07\x00\x00\xB9\x38\x04\x00\x00", 10) == 0 &&
               memcmp(onex, "\xB8\x00\x0F\x00\x00\xB9\x70\x08\x00\x00", 10) == 0;
    if (okS) {
      int w = display::g_w, h = display::g_h;
      okS = WriteCode(one + 1, &w, 4) && WriteCode(one + 6, &h, 4) && WriteCode(onex + 1, &w, 4) && WriteCode(onex + 6, &h, 4);
    }
    log_line("[gfx] output size %dx%d%s\r\n", display::g_w, display::g_h, okS ? "" : " NOT set (unexpected code at exe+0xAAABB)");
  }
  bool okC = cfgStub && WriteJmpRel(cfg, cfgStub, 5);
  if (!okC) log_line("[gfx] WARNING: config lookup not hooked; detailed graphics options disabled\r\n");

  bool okL = WriteJmpRel(loc, locStub, 5);
  bool okB = okL && WriteJmpRel(bld, bldStub, 7);
  bool okPause = okL && pauStub && WriteJmpRel(pau, pauStub, 9);
  log_line("[gfx] graphics rows %s (texts %d, builder %d, pause menu %d); fps limit %d, vsync %ld\r\n",
           okB ? "installed" : "NOT installed", okL, okB, okPause, kFpsChoices[g_fpsIdx], (long)g_vsync);
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
