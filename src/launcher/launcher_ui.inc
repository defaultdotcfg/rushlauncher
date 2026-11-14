// ============================================================================
// Launcher UI (v2): game art on the left, settings panel on the right.
// All buttons/toggles/combo items are owner-drawn so the dark theme is consistent.
// ============================================================================
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
// Common Controls v6 (themed scrollbars/combos; needed for the DarkMode_* themes)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define ID_BTN_ADVANCED   1020
#define ID_BTN_SHOW_LOG   1021
#define ID_DISCORD_CHK    1022
#define ID_SKIPINTRO_CHK  1024
#include <shobjidl.h>
#include "discord_rpc.inc"
#include "update_check.inc"
#include "self_update.inc"
#define ID_BTN_UPDATE     1030
static HWND g_hBtnUpdate = nullptr;

// Layout (client area)
#define UI_W        1000
#define UI_H        680
#define UI_HERO_W   600
#define UI_PANEL_X  (UI_HERO_W + 28)
#define UI_PANEL_W  (UI_W - UI_HERO_W - 56)

static ULONG_PTR g_gdiplusToken = 0;
#include "logo_png.h"
// The logo replaces the "RUSHLAUNCHER" text in the rail. Decoded once from the embedded PNG (the stream must outlive the bitmap).
static Gdiplus::Bitmap *g_logo = nullptr;
static Gdiplus::Bitmap *LogoBitmap()
{
    if (g_logo) return g_logo;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(g_logoPng));
    if (!hg) return nullptr;
    void *p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return nullptr; }
    memcpy(p, g_logoPng, sizeof(g_logoPng));
    GlobalUnlock(hg);
    IStream *st = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { GlobalFree(hg); return nullptr; }
    Gdiplus::Bitmap *b = Gdiplus::Bitmap::FromStream(st);
    if (b && b->GetLastStatus() == Gdiplus::Ok) g_logo = b; else delete b;
    return g_logo;
}
// Logo + version at the top of the 216 px rail (same on every tab).
static void DrawRailBrand(Gdiplus::Graphics &g)
{
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fTag(&ff, 11, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush orange(GPC(COLOR_ACCENT_ORANGE)), white(GPC(COLOR_TEXT_PRIMARY));
    Gdiplus::StringFormat center; center.SetAlignment(Gdiplus::StringAlignmentCenter);
    float lh = 0.f, ly = 34.f;
    if (Gdiplus::Bitmap *logo = LogoBitmap())
    {   // centred in the rail with 8 px margins
        const int lw = 200; lh = (float)lw * (float)LOGO_H / (float)LOGO_W;
        // scaled (and, in the light theme, tinted) once per theme instead of on every paint
        static Gdiplus::Bitmap *s_scaled = nullptr;
        static int s_scaledTheme = -1;
        const int lhi = (int)(lh + 0.5f);
        if (!s_scaled || s_scaledTheme != (g_pal.dark ? 1 : 0))
        {
            delete s_scaled;
            s_scaled = new Gdiplus::Bitmap(lw, lhi, PixelFormat32bppPARGB);
            Gdiplus::Graphics sg(s_scaled);
            sg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            sg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            // the logo is white: in the light theme it is tinted with the text colour
            Gdiplus::ColorMatrix cm = { {
                { g_pal.dark ? 1.f : GetRValue(COLOR_TEXT_PRIMARY) / 255.f, 0, 0, 0, 0 }, { 0, g_pal.dark ? 1.f : GetGValue(COLOR_TEXT_PRIMARY) / 255.f, 0, 0, 0 },
                { 0, 0, g_pal.dark ? 1.f : GetBValue(COLOR_TEXT_PRIMARY) / 255.f, 0, 0 }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } } };
            Gdiplus::ImageAttributes ia; ia.SetColorMatrix(&cm);
            sg.DrawImage(logo, Gdiplus::Rect(0, 0, lw, lhi), 0, 0, (INT)logo->GetWidth(), (INT)logo->GetHeight(), Gdiplus::UnitPixel, &ia);
            s_scaledTheme = g_pal.dark ? 1 : 0;
        }
        g.DrawImage(s_scaled, (216 - lw) / 2, (int)ly, lw, lhi);
    }
    else
    {
        Gdiplus::Font fBrand(&ff, 16, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        lh = 22.f;
        g.DrawString(L"RUSHLAUNCHER", -1, &fBrand, Gdiplus::RectF(0.f, ly, 216.f, lh), &center, &white);
    }
    g.DrawString(L"v" OLAUNCHER_VERSION, -1, &fTag, Gdiplus::RectF(0.f, ly + lh + 6.f, 216.f, 16.f), &center, &orange);
}
static bool g_vsyncOn = true, g_d3dDebugOn = false, g_verboseOn = false, g_skipIntroOn = false;
static HWND g_hSkipIntroChk = nullptr;
#define ID_MINIMIZE_CHK   1023
#define ID_THEME_COMBO    1031
static bool g_minimizeOn = false, g_minimizedForGame = false;
static HWND g_hMinimizeChk = nullptr, g_hThemeCombo = nullptr;
static bool g_showAdvanced = false, g_showLog = false;
static bool g_discordOn = true;
static char g_discordAppId[64] = {};
static HWND g_hDiscordChk = nullptr;
static HWND g_hAdvBtn = nullptr, g_hLogBtn = nullptr, g_hClearLogBtn = nullptr, g_hCopyLogBtn = nullptr;
static bool g_gameDirOk = false;
static wchar_t g_gameDirShown[MAX_PATH] = {};
static HFONT g_hFontSmallCaps = nullptr, g_hFontPlay = nullptr;

// ---- Launcher language (pt / en / es) -------------------------------------------------------
#define ID_UILANG_COMBO    1025
#define ID_TEXTLANG_COMBO  1026
#define ID_VOICELANG_COMBO 1027
#define ID_PLAYER_EDIT     1028
#define ID_PHOTOFMT_COMBO  1029
static HWND g_hPhotoFmtCombo = nullptr;
static HWND g_hPlayerEdit = nullptr;
static HWND g_hUiLangCombo = nullptr, g_hTextLangCombo = nullptr, g_hVoiceLangCombo = nullptr;
static HWND g_hBtnOpenGame = nullptr, g_hBtnOpenSave = nullptr;
static int g_uiLang = 0;  // 0 = Português, 1 = English, 2 = Español
static const wchar_t *kUiLangCodes[] = { L"pt", L"en", L"es" };
static const wchar_t *kUiLangNames[] = { L"Português", L"English", L"Español" };

enum UiStr {
    S_SETTINGS, S_RES, S_MODE, S_TEXTLANG, S_VOICELANG, S_GAMEDIR, S_SAVEDIR, S_VSYNC, S_SKIP, S_ADV, S_D3DDBG,
    S_VERBOSE, S_PLAY, S_CLOSE, S_PREP, S_BTN_GAME, S_BTN_SAVES, S_SHOWLOG, S_HIDELOG, S_CLEAR, S_COPY,
    S_RUNNING, S_READY, S_NOTFOUND, S_VERSION, S_MODE_BORDERLESS, S_MODE_WINDOW, S_MODE_FULL, S_LANG_SYSTEM,
    S_LANG_SAME, S_PICK_GAME, S_BAD_DIR, S_NO_GAME, S_PICK_SAVE, S_PLAYER, S_PHOTO, S_PHOTO_PNG, S_PHOTO_JPG,
    S_UPD_CHECK, S_UPD_CHECKING, S_UPD_AVAIL, S_UPD_LATEST, S_UPD_FAIL, S_UPD_NONE, S_UPD_ASK,
    S_SETUP_ASK, S_SETUP_FAIL, S_SETUP_NOPORT, S_UPD_ASK_INSTALL, S_UPD_DL, S_UPD_GAMERUN, S_UPD_DLFAIL,
    S_HOME_TITLE, S_HOME_HINT, S_INSTALL_TITLE, S_INSTALL_HINT, S_INSTALL_READY, S_INSTALL_MISSING,
    S_PATCH_ASK, S_PICK_PATCH, S_BAD_PATCH, S_PATCH_CONFIRM, S_PATCH_PROGRESS, S_PATCH_FAIL, S_HOME_PROFILE, S_HOME_PLAYTIME,
    S_MINIMIZE, S_THEME, S_THEME_DARK, S_THEME_LIGHT, S_TAB_SETTINGS, S_OLD_GAME, S_LOST_COPY, S_COUNT
};
static const wchar_t *kUiStr[S_COUNT][3] = {
    { L"CONFIGURAÇÕES", L"SETTINGS", L"CONFIGURACIÓN" },
    { L"Resolução", L"Resolution", L"Resolución" },
    { L"Modo de tela", L"Display mode", L"Modo de pantalla" },
    { L"Idioma do texto", L"Text language", L"Idioma del texto" },
    { L"Idioma da voz", L"Voice language", L"Idioma de las voces" },
    { L"Pasta do jogo", L"Game folder", L"Carpeta del juego" },
    { L"Pasta de saves", L"Save folder", L"Carpeta de partidas" },
    { L"Sincronização vertical (VSync)", L"Vertical sync (VSync)", L"Sincronización vertical (VSync)" },
    { L"Pular introdução (tela legal e logos)", L"Skip intro (legal screen and logos)", L"Omitir introducción (aviso legal y logos)" },
    { L"Opções avançadas", L"Advanced options", L"Opciones avanzadas" },
    { L"Camada de depuração do Direct3D 12", L"Direct3D 12 debug layer", L"Capa de depuración de Direct3D 12" },
    { L"Modo de depuração (log completo e capturas BMP)", L"Debug mode (full log and BMP captures)", L"Modo de depuración (registro completo y capturas BMP)" },
    { L"JOGAR", L"PLAY", L"JUGAR" },
    { L"FECHAR JOGO", L"CLOSE GAME", L"CERRAR JUEGO" },
    { L"PREPARANDO…", L"PREPARING…", L"PREPARANDO…" },
    { L"Pasta do jogo", L"Game folder", L"Carpeta" },
    { L"Saves", L"Saves", L"Partidas" },
    { L"Ver log", L"Show log", L"Ver registro" },
    { L"Ocultar log", L"Hide log", L"Ocultar registro" },
    { L"Limpar", L"Clear", L"Limpiar" },
    { L"Copiar", L"Copy", L"Copiar" },
    { L"Jogo em execução", L"Game running", L"Juego en ejecución" },
    { L"Pronto para jogar", L"Ready to play", L"Listo para jugar" },
    { L"Pasta do jogo não encontrada", L"Game folder not found", L"Carpeta del juego no encontrada" },
    { L"Versão 1.0.64980.34  ·  Windows x64 · Direct3D 12", L"Version 1.0.64980.34  ·  Windows x64 · Direct3D 12",
      L"Versión 1.0.64980.34  ·  Windows x64 · Direct3D 12" },
    { L"Sem bordas (recomendado)", L"Borderless (recommended)", L"Sin bordes (recomendado)" },
    { L"Janela", L"Windowed", L"Ventana" },
    { L"Tela cheia", L"Fullscreen", L"Pantalla completa" },
    { L"Automático (sistema)", L"Automatic (system)", L"Automático (sistema)" },
    { L"Igual ao texto", L"Same as text", L"Igual que el texto" },
    { L"Selecione a sua cópia do ONRUSH (a pasta do pacote extraído, com Mount e EmbeddedXvd)",
      L"Select your copy of ONRUSH (the extracted package folder, with Mount and EmbeddedXvd)",
      L"Selecciona tu copia de ONRUSH (la carpeta del paquete extraído, con Mount y EmbeddedXvd)" },
    { L"Essa pasta não tem o ONRUSH (game_release_xbox.exe, direto ou em Mount\\).\nEscolher outra pasta?",
      L"This folder does not contain ONRUSH (game_release_xbox.exe, directly or in Mount\\).\nChoose another folder?",
      L"Esta carpeta no contiene ONRUSH (game_release_xbox.exe, directo o en Mount\\).\n¿Elegir otra carpeta?" },
    { L"O jogo ainda não está preparado.\nSelecione a pasta da sua cópia do ONRUSH (o pacote extraído, com Mount e EmbeddedXvd).",
      L"The game is not set up yet.\nSelect the folder of your copy of ONRUSH (the extracted package, with Mount and EmbeddedXvd).",
      L"El juego aún no está preparado.\nSelecciona la carpeta de tu copia de ONRUSH (el paquete extraído, con Mount y EmbeddedXvd)." },
    { L"Selecione a pasta para os saves do ONRUSH", L"Select the folder for ONRUSH saves", L"Selecciona la carpeta para las partidas de ONRUSH" },
    { L"Jogador (save próprio)", L"Player (own save)", L"Jugador (partida propia)" },
    { L"Fotos do modo foto (F12)", L"Photo mode shots (F12)", L"Fotos del modo foto (F12)" },
    { L"PNG (maior qualidade)", L"PNG (best quality)", L"PNG (máxima calidad)" },
    { L"JPG (mais leve)", L"JPG (smaller files)", L"JPG (más ligero)" },
    { L"Verificar atualizações", L"Check for updates", L"Buscar actualizaciones" },
    { L"Verificando…", L"Checking…", L"Buscando…" },
    { L"Atualização: %ls", L"Update: %ls", L"Actualización: %ls" },
    { L"Você já está na versão mais recente do launcher (v%ls).", L"You already have the latest launcher version (v%ls).",
      L"Ya tienes la versión más reciente del launcher (v%ls)." },
    { L"Não foi possível verificar atualizações (%ls).\nConfira sua conexão ou abra a página de versões no GitHub.",
      L"Could not check for updates (%ls).\nCheck your connection or open the releases page on GitHub.",
      L"No se pudo buscar actualizaciones (%ls).\nRevisa tu conexión o abre la página de versiones en GitHub." },
    { L"Ainda não há versões publicadas no GitHub.", L"There are no releases on GitHub yet.", L"Todavía no hay versiones publicadas en GitHub." },
    { L"Nova versão do launcher disponível: %ls (atual: v%ls).\n\nAbrir a página de download no GitHub?",
      L"A new launcher version is available: %ls (current: v%ls).\n\nOpen the download page on GitHub?",
      L"Hay una nueva versión del launcher: %ls (actual: v%ls).\n\n¿Abrir la página de descarga en GitHub?" },
    { L"Encontrei o ONRUSH em:\n%ls\n\nPreparar o jogo na pasta do launcher?\n%ls\n\nOs arquivos do jogo são copiados (~60 MB), as pastas grandes (dat e EmbeddedXvd) viram atalhos para a sua cópia e é criado o onrush-pc.exe. A sua cópia não é alterada.",
      L"Found ONRUSH in:\n%ls\n\nSet the game up in the launcher folder?\n%ls\n\nThe game files are copied (~60 MB), the big folders (dat and EmbeddedXvd) become links to your copy and onrush-pc.exe is created. Your copy is not modified.",
      L"Se encontró ONRUSH en:\n%ls\n\n¿Preparar el juego en la carpeta del launcher?\n%ls\n\nLos archivos del juego se copian (~60 MB), las carpetas grandes (dat y EmbeddedXvd) se enlazan a tu copia y se crea onrush-pc.exe. Tu copia no se modifica." },
    { L"Não foi possível preparar o jogo: %ls.", L"Could not set the game up: %ls.", L"No se pudo preparar el juego: %ls." },
    { L"A pasta do launcher não tem os arquivos do port (d3d12_x.dll e outros).\nExtraia o zip da release inteiro e abra o launcher de dentro dele.",
      L"The launcher folder is missing the port files (d3d12_x.dll and others).\nExtract the whole release zip and run the launcher from it.",
      L"La carpeta del launcher no tiene los archivos del port (d3d12_x.dll y otros).\nExtrae el zip completo de la release y abre el launcher desde ahí." },
    { L"Nova versão do launcher disponível: %ls (atual: v%ls).\n\nBaixar e instalar agora? O launcher fecha, atualiza a pasta dele e abre de novo sozinho. Suas configurações, saves e shaders são mantidos.",
      L"A new launcher version is available: %ls (current: v%ls).\n\nDownload and install it now? The launcher closes, updates its folder and opens again by itself. Your settings, saves and shaders are kept.",
      L"Hay una nueva versión del launcher: %ls (actual: v%ls).\n\n¿Descargar e instalar ahora? El launcher se cierra, actualiza su carpeta y se abre de nuevo solo. Tu configuración, partidas guardadas y shaders se mantienen." },
    { L"Baixando… %d%%", L"Downloading… %d%%", L"Descargando… %d%%" },
    { L"Feche o jogo antes de atualizar o launcher.", L"Close the game before updating the launcher.", L"Cierra el juego antes de actualizar el launcher." },
    { L"Não foi possível baixar a atualização (%ls).\n\nAbrir a página da versão no GitHub?",
      L"Could not download the update (%ls).\n\nOpen the release page on GitHub?",
      L"No se pudo descargar la actualización (%ls).\n\n¿Abrir la página de la versión en GitHub?" },
    { L"Pronto para correr?", L"Ready to race?", L"¿Listo para correr?" },
    { L"Selecione JOGAR para iniciar o jogo.", L"Select PLAY to start the game.", L"Selecciona JUGAR para iniciar el juego." },
    { L"Instalação do jogo", L"Game installation", L"Instalación del juego" },
    { L"Use … para escolher a sua cópia do ONRUSH (o pacote extraído). Se ela não estiver na versão final 1.80.100.100, o launcher pede em seguida a pasta do patch da atualização.",
      L"Use … to choose your ONRUSH copy (the extracted package). If it is not the final version 1.80.100.100, the launcher then asks for the update patch folder.",
      L"Usa … para elegir tu copia de ONRUSH (el paquete extraído). Si no es la versión final 1.80.100.100, el launcher pide después la carpeta del parche." },
    { L"Status: pronto", L"Status: ready", L"Estado: listo" },
    { L"Status: selecione a pasta do jogo", L"Status: select the game folder", L"Estado: selecciona la carpeta del juego" },
    { L"Esta cópia do ONRUSH é a versão %ls.\nO port funciona com a versão final, %ls.\n\nVocê tem o patch da atualização (a pasta extraída com os arquivos .gdiff e os pacotes .dat novos)? Escolha a pasta dele para atualizar a cópia.",
      L"This ONRUSH copy is version %ls.\nThe port works with the final version, %ls.\n\nDo you have the update patch (the extracted folder with the .gdiff files and the new .dat packs)? Choose its folder to update the copy.",
      L"Esta copia de ONRUSH es la versión %ls.\nEl port funciona con la versión final, %ls.\n\n¿Tienes el parche de la actualización (la carpeta extraída con los archivos .gdiff y los paquetes .dat nuevos)? Elige su carpeta para actualizar la copia." },
    { L"Selecione a pasta do patch da atualização (1.80.100.100)", L"Select the update patch folder (1.80.100.100)", L"Selecciona la carpeta del parche (1.80.100.100)" },
    { L"Essa pasta não parece ser o patch do ONRUSH (falta Mount\\game_release_xbox.exe.gdiff).\nTentar outra pasta?",
      L"This folder does not look like the ONRUSH patch (Mount\\game_release_xbox.exe.gdiff is missing).\nTry another folder?",
      L"Esta carpeta no parece el parche de ONRUSH (falta Mount\\game_release_xbox.exe.gdiff).\n¿Probar otra carpeta?" },
    { L"Aplicar o patch na sua cópia do jogo?\n%ls\n\nOs arquivos alterados são atualizados e os pacotes novos são MOVIDOS da pasta do patch para a cópia (~13 GB; no mesmo disco é instantâneo e não ocupa espaço extra). Depois disso a pasta do patch fica vazia.",
      L"Apply the patch to your game copy?\n%ls\n\nChanged files are updated and the new packs are MOVED from the patch folder into the copy (~13 GB; instant and no extra space on the same disk). Afterwards the patch folder is left empty.",
      L"¿Aplicar el parche a tu copia del juego?\n%ls\n\nLos archivos modificados se actualizan y los paquetes nuevos se MUEVEN de la carpeta del parche a la copia (~13 GB; instantáneo y sin espacio extra en el mismo disco). Después la carpeta del parche queda vacía." },
    { L"Atualizando o jogo… %d%%", L"Updating the game… %d%%", L"Actualizando el juego… %d%%" },
    { L"Não foi possível aplicar o patch: %ls.", L"Could not apply the patch: %ls.", L"No se pudo aplicar el parche: %ls." },
    { L"Perfil", L"Profile", L"Perfil" },
    { L"Tempo de jogo", L"Play time", L"Tiempo de juego" },
    { L"Minimizar o launcher ao jogar", L"Minimize the launcher while playing", L"Minimizar el launcher al jugar" },
    { L"Tema do launcher", L"Launcher theme", L"Tema del launcher" },
    { L"Escuro", L"Dark", L"Oscuro" },
    { L"Claro", L"Light", L"Claro" },
    { L"Configurações", L"Settings", L"Configuración" },
    { L"O jogo nesta pasta é a versão %ls, mas o port funciona só com a versão final, %ls.\n\nEscolha a sua cópia do ONRUSH (o pacote extraído) para o launcher atualizá-la com o patch e preparar o jogo de novo.",
      L"The game in this folder is version %ls, but the port only works with the final version, %ls.\n\nChoose your ONRUSH copy (the extracted package) so the launcher can update it with the patch and set the game up again.",
      L"El juego en esta carpeta es la versión %ls, pero el port solo funciona con la versión final, %ls.\n\nElige tu copia de ONRUSH (el paquete extraído) para que el launcher la actualice con el parche y prepare el juego de nuevo." },
    { L"A pasta do jogo foi preparada a partir da cópia do ONRUSH em\n%ls\nmas essa pasta não existe mais (foi movida, renomeada ou apagada).\n\nEscolha onde está a sua cópia do ONRUSH (o pacote extraído) para o launcher ligar a pasta do jogo a ela de novo.",
      L"The game folder was set up from the ONRUSH copy at\n%ls\nbut that folder no longer exists (it was moved, renamed or deleted).\n\nChoose where your ONRUSH copy (the extracted package) is now so the launcher can point the game folder at it again.",
      L"La carpeta del juego se preparó a partir de la copia de ONRUSH en\n%ls\npero esa carpeta ya no existe (se movió, renombró o borró).\n\nElige dónde está tu copia de ONRUSH (el paquete extraído) para que el launcher vuelva a enlazar la carpeta del juego con ella." },
};
static const wchar_t *T(UiStr s) { return kUiStr[s][g_uiLang]; }

// Game languages (names in their own language) and the locale passed to the game.
static const wchar_t *kGameLangNames[] = { L"English", L"Português (Brasil)", L"Español", L"Français", L"Deutsch", L"Italiano", L"Polski" };
static const wchar_t *kGameLangLocales[] = { L"en-US", L"pt-BR", L"es-ES", L"fr-FR", L"de-DE", L"it-IT", L"pl-PL" };
static const int kGameLangCount = 7;

static void FillGameLangCombo(HWND combo, const wchar_t *firstItem, const wchar_t *current)
{
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)firstItem);
    int sel = 0;
    for (int i = 0; i < kGameLangCount; ++i)
    {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)kGameLangNames[i]);
        if (current && current[0] && _wcsnicmp(current, kGameLangLocales[i], 2) == 0) sel = i + 1;
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}
static void ReadGameLangCombo(HWND combo, wchar_t *out, size_t n)
{
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel >= 1 && sel <= kGameLangCount) wcscpy_s(out, n, kGameLangLocales[sel - 1]);
    else out[0] = L'\0';
}

static bool GameDirLooksValid(const wchar_t *dir)
{
    if (!dir || !dir[0]) return false;
    const wchar_t *exes[] = { L"onrush-pc.exe", L"game_release_xbox.exe" };
    for (const wchar_t *e : exes)
    {
        wchar_t p[MAX_PATH];
        swprintf_s(p, MAX_PATH, L"%ls\\%ls", dir, e);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

static bool GameDirIsReady(const wchar_t *dir);
static void RefreshGameDirStatus(HWND hwnd)
{
    wchar_t edit[MAX_PATH] = {};
    if (g_hGamePathEdit) GetWindowTextW(g_hGamePathEdit, edit, MAX_PATH);
    LauncherConfig tmp = g_config;
    wcscpy_s(tmp.gamePath, MAX_PATH, edit);
    GetEffectiveGameDir(&tmp, g_gameDirShown, MAX_PATH);
    g_gameDirOk = GameDirIsReady(g_gameDirShown);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void SaveIni()
{
    wchar_t iniPath[MAX_PATH];
    swprintf_s(iniPath, MAX_PATH, L"%ls\\onrush.ini", g_exeDir);
    SaveConfigFile(iniPath, &g_config);
    WritePrivateProfileStringW(L"Discord", L"Enabled", g_discordOn ? L"1" : L"0", iniPath);
    wchar_t id[64]; MultiByteToWideChar(CP_UTF8, 0, g_discordAppId, -1, id, 64);
    WritePrivateProfileStringW(L"Discord", L"AppId", id, iniPath);
}

static void LoadDiscordIni()
{
    wchar_t iniPath[MAX_PATH], id[64] = {};
    swprintf_s(iniPath, MAX_PATH, L"%ls\\onrush.ini", g_exeDir);
    g_discordOn = GetPrivateProfileIntW(L"Discord", L"Enabled", 1, iniPath) != 0;
    GetPrivateProfileStringW(L"Discord", L"AppId", L"", id, 64, iniPath);
    if (id[0]) WideCharToMultiByte(CP_UTF8, 0, id, -1, g_discordAppId, 64, nullptr, nullptr);
    else strcpy_s(g_discordAppId, kDiscordDefaultAppId);
}

// Modern folder picker; returns false when cancelled.
static bool PickFolder(HWND hwnd, const wchar_t *title, wchar_t *out)
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0; dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(hwnd)))
    {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { wcscpy_s(out, MAX_PATH, path); CoTaskMemFree(path); ok = true; }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

#include "game_setup.inc"
// Asks for the game folder until a valid one is chosen (or the user cancels). A copy of the game
// that is not set up yet (the extracted package) is set up in the launcher's folder.
static bool AskGameFolder(HWND hwnd)
{
    wchar_t dir[MAX_PATH], mount[MAX_PATH], root[MAX_PATH];
    while (PickFolder(hwnd, T(S_PICK_GAME), dir))
    {
        if (GameDirIsReady(dir)) { SetWindowTextW(g_hGamePathEdit, dir); return true; }
        if (FindGameCopy(dir, mount, root))
        {
            if (!OfferGameSetup(hwnd, dir)) return false;
            SetWindowTextW(g_hGamePathEdit, g_exeDir);
            return true;
        }
        if (MessageBoxW(hwnd, T(S_BAD_DIR), L"ONRUSH", MB_ICONWARNING | MB_YESNO) != IDYES) break;
    }
    return false;
}

// Only the final version runs on the port. Checked at every launch, not just at setup: the folder may
// hold a copy prepared before the version check existed (or by an older launcher) that was never patched.
// An old copy is patched and the files copied from it are refreshed. True when the game can start.
static void ReadUiIntoConfig();

// The game folder's dat junction leads nowhere (the copy it was set up from was moved, renamed or deleted):
// asks for the copy, updates it to the final version if needed and points the folder at it again.
static bool RelinkGameCopy(HWND hwnd, const wchar_t *gameDir)
{
    wchar_t dat[MAX_PATH], msg[1024], dir[MAX_PATH], mount[MAX_PATH], root[MAX_PATH];
    swprintf_s(dat, MAX_PATH, L"%ls\\dat", gameDir);
    std::wstring was = JunctionTarget(dat);
    LogPrintf(L"[Setup] pacotes do jogo inacessíveis em %ls (dat -> %ls)\r\n", gameDir, was.empty() ? L"?" : was.c_str());
    swprintf_s(msg, 1024, T(S_LOST_COPY), was.empty() ? dat : was.c_str());
    if (MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONWARNING | MB_OKCANCEL) != IDOK) return false;
    for (;;)
    {
        if (!PickFolder(hwnd, T(S_PICK_GAME), dir)) return false;
        if (FindGameCopy(dir, mount, root) && _wcsicmp(mount, gameDir) != 0 && GameAssetsReachable(mount)) break;
        if (MessageBoxW(hwnd, T(S_BAD_DIR), L"ONRUSH", MB_ICONWARNING | MB_YESNO) != IDYES) return false;
    }
    if (GameCopyVersion(mount) != kFinalVersion && !OfferGamePatch(hwnd, mount, root)) return false;
    std::wstring err;
    if (!SetupGameFolder(mount, root, gameDir, err))
    {
        swprintf_s(msg, 1024, T(S_SETUP_FAIL), err.c_str());
        MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONERROR);
        return false;
    }
    LogPrintf(L"[Setup] %ls ligado de novo à cópia em %ls\r\n", gameDir, root);
    return GameAssetsReachable(gameDir);
}

static bool EnsureFinalGameVersion(HWND hwnd, const wchar_t *gameDir)
{
    wchar_t mount[MAX_PATH], root[MAX_PATH], msg[1024];
    bool haveSrc = SourceCopyOf(gameDir, mount, root);
    std::wstring ver = GameCopyVersion(gameDir);
    if (ver.empty() && haveSrc) ver = GameCopyVersion(mount);
    if (ver == kFinalVersion) return true;
    if (ver.empty())
    {   // no appxmanifest.xml anywhere: nothing to compare with
        LogPrintf(L"[Setup] versão do jogo desconhecida (sem appxmanifest.xml em %ls)\r\n", gameDir);
        return true;
    }
    LogPrintf(L"[Setup] o jogo em %ls é a versão %ls; o port precisa da %ls\r\n", gameDir, ver.c_str(), kFinalVersion);
    if (!haveSrc)
    {
        swprintf_s(msg, 1024, T(S_OLD_GAME), ver.c_str(), kFinalVersion);
        if (MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONWARNING | MB_OKCANCEL) != IDOK || !AskGameFolder(hwnd)) return false;
        ReadUiIntoConfig(); SaveIni();
        RefreshGameDirStatus(hwnd);
        return GameCopyVersion(g_gameDirShown) == kFinalVersion;
    }
    if (GameCopyVersion(mount) != kFinalVersion && !OfferGamePatch(hwnd, mount, root)) return false;
    std::wstring err;
    if (!SetupGameFolder(mount, root, gameDir, err))
    {
        swprintf_s(msg, 1024, T(S_SETUP_FAIL), err.c_str());
        MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONERROR);
        return false;
    }
    ver = GameCopyVersion(gameDir);
    LogPrintf(L"[Setup] jogo em %ls agora na versão %ls\r\n", gameDir, ver.c_str());
    return ver == kFinalVersion;
}

static void FillRound(HDC hdc, RECT r, int rad, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(b); DeleteObject(p);
}

static void LayoutAdvanced(HWND hwnd)
{
    ShowWindow(g_hD3DDebugChk, g_showAdvanced ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hVerboseChk, g_showAdvanced ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_hAdvBtn, nullptr, TRUE);
}

static void LayoutLog(HWND hwnd)
{
    int sw = g_showLog ? SW_SHOW : SW_HIDE;
    ShowWindow(g_hLogEdit, sw); ShowWindow(g_hClearLogBtn, sw); ShowWindow(g_hCopyLogBtn, sw);
    ShowWindow(g_hBtnUpdate, g_showLog ? SW_HIDE : SW_SHOW);
    SetWindowTextW(g_hLogBtn, g_showLog ? T(S_HIDELOG) : T(S_SHOWLOG));
    RECT rc = { 0, 0, UI_HERO_W, UI_H };
    InvalidateRect(hwnd, &rc, FALSE);
}

#include "playlist_ui.inc"
#include "profiles_ui.inc"
#include "photos_ui.inc"
#include "controls_ui.inc"

// Backdrop of the content column (Jogar, Configurações, Perfis): drawn by the launcher instead of the game's
// splash art. Dark gradient, a warm glow and slanted "speed" bands in the accent colour, then the game name.
// Rendered once per theme/language into a bitmap (every toggle repaints the window).
static void DrawBackdrop(Gdiplus::Graphics &g)
{
    static Gdiplus::Bitmap *s_bmp = nullptr;
    static int s_key = -1;
    const int x0 = 216, w = UI_HERO_W - 216, h = UI_H;
    const int key = (g_pal.dark ? 1 : 0) | (g_uiLang << 1);
    if (!s_bmp || s_key != key)
    {
        delete s_bmp;
        s_bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
        Gdiplus::Graphics bg(s_bmp);
        bg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        bg.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        const COLORREF acc = COLOR_ACCENT_ORANGE;
        // base: slightly lifted at the top, the main background at the bottom
        Gdiplus::LinearGradientBrush base(Gdiplus::Point(0, -1), Gdiplus::Point(0, h + 1), GPC(g_pal.homeCard), GPC(COLOR_BG_MAIN));
        bg.FillRectangle(&base, 0, 0, w, h);
        // warm glow in the lower right
        {
            Gdiplus::GraphicsPath gp;
            gp.AddEllipse(Gdiplus::RectF(w * 0.05f, h * 0.38f, w * 1.5f, h * 0.9f));
            Gdiplus::PathGradientBrush pg(&gp);
            pg.SetCenterPoint(Gdiplus::PointF(w * 0.85f, h * 0.80f));
            pg.SetCenterColor(GPCA(g_pal.dark ? 110 : 70, acc));
            Gdiplus::Color edge = GPCA(0, acc); int cnt = 1;
            pg.SetSurroundColors(&edge, &cnt);
            bg.FillPath(&pg, &gp);
        }
        // speed bands: parallelograms leaning right, brighter towards the bottom
        struct Band { float x, wd; int a; };
        const Band bands[] = { { 4, 5, 110 }, { 46, 46, 34 }, { 120, 9, 150 }, { 150, 3, 200 },
                               { 196, 70, 26 }, { 290, 14, 110 }, { 318, 4, 170 }, { 352, 30, 55 } };
        const float slant = 300.f, top = h * 0.30f;
        for (const Band &bd : bands)
        {
            Gdiplus::PointF pts[4] = { { bd.x, (float)h }, { bd.x + bd.wd, (float)h },
                                       { bd.x + bd.wd + slant, top }, { bd.x + slant, top } };
            Gdiplus::LinearGradientBrush lb(Gdiplus::PointF(0, top), Gdiplus::PointF(0, (float)h),
                                            GPCA(0, acc), GPCA(g_pal.dark ? bd.a : bd.a * 3 / 4, acc));
            lb.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
            bg.FillPolygon(&lb, pts, 4);
        }
        // game name and tag line
        Gdiplus::FontFamily ffb(L"Segoe UI Black"), ff(L"Segoe UI");
        Gdiplus::FontFamily *title = ffb.IsAvailable() ? &ffb : &ff;
        Gdiplus::Font fTitle(title, 58, Gdiplus::FontStyleBoldItalic, Gdiplus::UnitPixel);
        Gdiplus::Font fTag(&ff, 13, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush txt(GPC(COLOR_TEXT_PRIMARY)), accB(GPC(acc)), muted(GPC(COLOR_TEXT_MUTED));
        bg.DrawString(L"ONRUSH", -1, &fTitle, Gdiplus::PointF(26, 70), &txt);
        bg.FillRectangle(&accB, 34, 150, 44, 4);
        static const wchar_t *kTag[] = { L"PORT NATIVO PARA PC", L"NATIVE PC PORT", L"PORT NATIVO PARA PC" };
        bg.DrawString(kTag[g_uiLang], -1, &fTag, Gdiplus::PointF(32, 162), &muted);
        s_key = key;
    }
    g.DrawImage(s_bmp, x0, 0, w, h);
}

static void DrawHero(HDC hdc)
{
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::SolidBrush bg(GPC(COLOR_BG_MAIN));
    g.FillRectangle(&bg, 0, 0, UI_HERO_W, UI_H);
    // Playlist, Fotos and Controles cover the column completely: skip the backdrop there.
    if (g_tab != 1 && g_tab != 4 && g_tab != 5) DrawBackdrop(g);
    Gdiplus::SolidBrush railBase(GPC(COLOR_BG_MAIN));
    g.FillRectangle(&railBase, 0, 0, 216, UI_H);
    if (g_tab == 1) { PlDrawHero(g); return; }
    if (g_tab == 4) { PhDrawHero(g); DrawRailBrand(g); return; }
    if (g_tab == 5) { KbDrawHero(g); DrawRailBrand(g); return; }
    if (g_showLog)
    {
        Gdiplus::SolidBrush dim(GPCA(235, COLOR_BG_LOG));
        g.FillRectangle(&dim, 0, 0, UI_HERO_W, UI_H);
        return;
    }

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fStatus(&ff, 20, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fSmall(&ff, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush orange(GPC(COLOR_ACCENT_ORANGE)), white(GPC(COLOR_TEXT_PRIMARY)),
        muted(GPC(COLOR_TEXT_MUTED)), green(GPC(COLOR_ACCENT_GREEN)), red(GPC(COLOR_ACCENT_RED));
    Gdiplus::SolidBrush rail(GPC(COLOR_BG_MAIN));
    g.FillRectangle(&rail, 0, 0, 216, UI_H);
    DrawRailBrand(g);
    g.DrawString(T(S_GAMEDIR), -1, &fSmall, Gdiplus::PointF(24, 470), &muted);
    const wchar_t *status = g_gameRunning ? T(S_RUNNING) : (g_gameDirOk ? T(S_READY) : T(S_NOTFOUND));
    Gdiplus::SolidBrush *dot = g_gameRunning ? &orange : (g_gameDirOk ? &green : &red);
    g.FillEllipse(dot, 24, 498, 8, 8);
    g.DrawString(status, -1, &fSmall, Gdiplus::PointF(40, 492), &white);
}

static void DrawPanel(HDC hdc)
{
    RECT rp = { UI_HERO_W, 0, UI_W, UI_H };
    FillRect(hdc, &rp, g_hBrushCard);
    RECT edge = { UI_HERO_W, 0, UI_HERO_W + 1, UI_H };
    HBRUSH eb = CreateSolidBrush(COLOR_BORDER); FillRect(hdc, &edge, eb); DeleteObject(eb);

    SetBkMode(hdc, TRANSPARENT);
    if (g_tab == 1) { PlDrawPanel(hdc); return; }

    if (g_tab == 0)
    {
        RECT card = { UI_PANEL_X, 64, UI_PANEL_X + UI_PANEL_W, 250 };
        FillRound(hdc, card, 14, g_pal.homeCard, COLOR_BORDER);
        SelectObject(hdc, g_hFontUIBold); SetTextColor(hdc, COLOR_TEXT_PRIMARY);
        const wchar_t *st = g_gameRunning ? T(S_RUNNING) : (g_gameDirOk ? T(S_READY) : T(S_NOTFOUND));
        TextOutW(hdc, UI_PANEL_X + 22, 90, st, (int)wcslen(st));
        SelectObject(hdc, g_hFontUI); SetTextColor(hdc, COLOR_TEXT_MUTED);
        const wchar_t *homeHint = T(S_HOME_HINT);
        TextOutW(hdc, UI_PANEL_X + 22, 120, homeHint, (int)wcslen(homeHint));
        {   // profile in use + play time (Perfis tab)
            wchar_t line[160];
            ULONGLONG secs = ProfPlaySeconds(g_config.playerName);
            if (g_gameRunning && g_playStartTick) secs += (GetTickCount64() - g_playStartTick) / 1000;
            SetTextColor(hdc, COLOR_TEXT_PRIMARY);
            swprintf_s(line, L"%ls: %ls", T(S_HOME_PROFILE), g_config.playerName);
            TextOutW(hdc, UI_PANEL_X + 22, 172, line, (int)wcslen(line));
            swprintf_s(line, L"%ls: %ls", T(S_HOME_PLAYTIME), ProfFmtPlay(secs).c_str());
            TextOutW(hdc, UI_PANEL_X + 22, 198, line, (int)wcslen(line));
            SetTextColor(hdc, COLOR_TEXT_MUTED);
        }
        // game folder (was the "Instalação" tab): "…" picks the copy and, when it is not final, the patch
        SetTextColor(hdc, COLOR_TEXT_PRIMARY);
        TextOutW(hdc, UI_PANEL_X, 320, T(S_GAMEDIR), (int)wcslen(T(S_GAMEDIR)));
        RECT gp = { UI_PANEL_X, 340, UI_PANEL_X + UI_PANEL_W - 48, 372 };
        FillRound(hdc, gp, 8, COLOR_BG_INPUT, g_gameDirOk ? COLOR_BORDER : COLOR_ACCENT_RED);
        SetTextColor(hdc, COLOR_TEXT_MUTED);
        RECT gh = { UI_PANEL_X, 380, UI_PANEL_X + UI_PANEL_W, 520 };
        DrawTextW(hdc, T(S_INSTALL_HINT), -1, &gh, DT_LEFT | DT_WORDBREAK);
        return;
    }
    if (g_tab == 3) { ProfDrawPanel(hdc); return; }
    if (g_tab == 4) { PhDrawPanel(hdc); return; }
    if (g_tab == 5) { KbDrawPanel(hdc); return; }

    SelectObject(hdc, g_hFontPlay);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    TextOutW(hdc, UI_PANEL_X, 20, T(S_SETTINGS), (int)wcslen(T(S_SETTINGS)));
    SelectObject(hdc, g_hFontUI);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    const int half = (UI_PANEL_W - 12) / 2, X2 = UI_PANEL_X + half + 12;
    struct { int x, y; const wchar_t *t; } labels[] = {
        { UI_PANEL_X, 58, T(S_RES) }, { X2, 58, T(S_MODE) }, { UI_PANEL_X, 120, T(S_TEXTLANG) }, { X2, 120, T(S_VOICELANG) },
        { UI_PANEL_X, 256, T(S_PHOTO) }, { X2, 256, T(S_THEME) }, { UI_PANEL_X, 320, T(S_SAVEDIR) } };
    for (auto &l : labels) TextOutW(hdc, l.x, l.y, l.t, (int)wcslen(l.t));

    // input boxes behind the borderless edits
    RECT r1 = { UI_PANEL_X, 340, UI_PANEL_X + UI_PANEL_W - 48, 372 };
    FillRound(hdc, r1, 8, COLOR_BG_INPUT, COLOR_BORDER);
}

static void DrawOwnerButton(const DRAWITEMSTRUCT *di)
{
    HDC hdc = di->hDC;
    RECT r = di->rcItem;
    int id = (int)di->CtlID;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    wchar_t text[128] = {};
    GetWindowTextW(di->hwndItem, text, 128);
    bool isNav = id == ID_TAB_HOME || id == ID_TAB_SETTINGS || id == ID_TAB_PLAYLIST || id == ID_TAB_SETUP || id == ID_TAB_PHOTOS || id == ID_TAB_CONTROLS;
    HBRUSH ownerBg = isNav ? CreateSolidBrush(COLOR_BG_MAIN) : g_hBrushCard;
    FillRect(hdc, &r, ownerBg);
    if (isNav) DeleteObject(ownerBg);
    SetBkMode(hdc, TRANSPARENT);

    if (isNav)
    {
        bool active = (id == ID_TAB_HOME && g_tab == 0) || (id == ID_TAB_PLAYLIST && g_tab == 1) ||
                      (id == ID_TAB_SETTINGS && g_tab == 2) || (id == ID_TAB_SETUP && g_tab == 3) || (id == ID_TAB_PHOTOS && g_tab == 4) ||
                      (id == ID_TAB_CONTROLS && g_tab == 5);
        FillRound(hdc, r, 8, active ? g_pal.navActive : COLOR_BG_MAIN, active ? COLOR_BORDER : COLOR_BG_MAIN);
        SelectObject(hdc, active ? g_hFontUIBold : g_hFontUI);
        SetTextColor(hdc, active ? COLOR_TEXT_PRIMARY : COLOR_TEXT_MUTED);
        RECT rt = r; rt.left += 16; rt.bottom -= 2;
        DrawTextW(hdc, text, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (active)
        {
            RECT u = { r.left, r.top + 8, r.left + 3, r.bottom - 8 };
            HBRUSH ub = CreateSolidBrush(COLOR_ACCENT_ORANGE); FillRect(hdc, &u, ub); DeleteObject(ub);
        }
        return;
    }
    bool isToggle = (id == ID_PL_ENABLE || id == ID_PL_SHUFFLE || id == ID_VSYNC_CHK || id == ID_DISCORD_CHK || id == ID_D3DDEBUG_CHK || id == ID_VERBOSE_CHK || id == ID_SKIPINTRO_CHK || id == ID_BTN_ADVANCED || id == ID_PROF_UNLOCK || id == ID_PROF_UNLOCKALL || id == ID_MINIMIZE_CHK);
    if (isToggle)
    {
        bool on = id == ID_PL_ENABLE ? g_config.musicEnabled != 0 : id == ID_PL_SHUFFLE ? g_config.musicShuffle != 0 :
                  id == ID_VSYNC_CHK ? g_vsyncOn : id == ID_DISCORD_CHK ? g_discordOn : id == ID_D3DDEBUG_CHK ? g_d3dDebugOn :
                  id == ID_VERBOSE_CHK ? g_verboseOn : id == ID_SKIPINTRO_CHK ? g_skipIntroOn : id == ID_MINIMIZE_CHK ? g_minimizeOn : (id == ID_PROF_UNLOCK || id == ID_PROF_UNLOCKALL) ? ProfUnlockSwitchOn(id) : g_showAdvanced;
        if (id == ID_BTN_ADVANCED)
        {   // disclosure row
            SelectObject(hdc, g_hFontUI);
            SetTextColor(hdc, COLOR_TEXT_MUTED);
            wchar_t t[160]; swprintf_s(t, L"%ls  %ls", on ? L"▾" : L"▸", text);
            RECT rt = r; DrawTextW(hdc, t, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        // switch: track 36x20 on the right, label on the left
        RECT tr = { r.right - 38, r.top + (r.bottom - r.top - 20) / 2, r.right - 2, 0 };
        tr.bottom = tr.top + 20;
        FillRound(hdc, tr, 20, on ? COLOR_ACCENT_ORANGE : g_pal.toggleOff, on ? COLOR_ACCENT_ORANGE : g_pal.toggleOff);
        RECT knob = on ? RECT{ tr.right - 18, tr.top + 2, tr.right - 2, tr.bottom - 2 }
                       : RECT{ tr.left + 2, tr.top + 2, tr.left + 18, tr.bottom - 2 };
        FillRound(hdc, knob, 16, g_pal.knob, g_pal.knob);
        SelectObject(hdc, g_hFontUI);
        SetTextColor(hdc, COLOR_TEXT_PRIMARY);
        RECT rt = r; rt.right = tr.left - 8;
        DrawTextW(hdc, text, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    bool primary = (id == ID_BTN_LAUNCH);
    COLORREF fill, border, fg = COLOR_TEXT_PRIMARY;
    if (primary)
    {
        fg = g_pal.onAccent;
        fill = g_gameRunning ? COLOR_ACCENT_RED : COLOR_ACCENT_ORANGE;
        if (pressed) fill = g_gameRunning ? g_pal.closePressed : g_pal.launchPressed;
        if (disabled) { fill = g_pal.disabledFill; fg = COLOR_TEXT_MUTED; }
        border = fill;
    }
    else
    {
        fill = pressed ? g_pal.btnPressed : g_pal.btnFill;
        border = COLOR_BORDER;
        if (disabled) fg = COLOR_TEXT_MUTED;
        if (id == ID_BTN_UPDATE && g_updateKnown && g_updateLast.status == UPD_NEWER) { border = COLOR_ACCENT_ORANGE; fg = g_pal.accentText; }
    }
    FillRound(hdc, r, primary ? 12 : 8, fill, border);
    SelectObject(hdc, primary ? g_hFontPlay : g_hFontUI);
    SetTextColor(hdc, fg);
    DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawComboItem(const DRAWITEMSTRUCT *di)
{
    if (di->itemID == (UINT)-1) return;
    wchar_t text[128] = {};
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    bool sel = (di->itemState & ODS_SELECTED) && !(di->itemState & ODS_COMBOBOXEDIT);
    HBRUSH b = CreateSolidBrush(sel ? g_pal.comboSel : COLOR_BG_INPUT);
    FillRect(di->hDC, &di->rcItem, b); DeleteObject(b);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, sel ? g_pal.accentText : COLOR_TEXT_PRIMARY);
    SelectObject(di->hDC, g_hFontUI);
    RECT r = di->rcItem; r.left += 10;
    DrawTextW(di->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void SetLaunchButtonState()
{
    SetWindowTextW(g_hBtnLaunch, g_gameRunning ? T(S_CLOSE) : T(S_PLAY));
    InvalidateRect(g_hBtnLaunch, nullptr, TRUE);
    RECT rc = { 0, 0, UI_HERO_W, UI_H };
    InvalidateRect(g_hMainWnd, &rc, FALSE);
}

static void BrowseFolder(HWND hwnd, HWND edit, const wchar_t *title)
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t selected[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, selected)) SetWindowTextW(edit, selected);
    CoTaskMemFree(pidl);
}

static void ReadUiIntoConfig()
{
    int sel = (int)SendMessageW(g_hResCombo, CB_GETCURSEL, 0, 0);
    if (sel == 1) { g_config.width = 2560; g_config.height = 1440; }
    else if (sel == 2) { g_config.width = 3840; g_config.height = 2160; }
    else if (sel == 3) { g_config.width = 1280; g_config.height = 720; }
    else { g_config.width = 1920; g_config.height = 1080; }
    int modeSel = (int)SendMessageW(g_hModeCombo, CB_GETCURSEL, 0, 0);
    wcscpy_s(g_config.windowMode, 32, modeSel == 1 ? L"Windowed" : modeSel == 2 ? L"Fullscreen" : L"Borderless");
    g_config.vsync = g_vsyncOn ? 1 : 0;
    g_config.d3d12Debug = g_d3dDebugOn ? 1 : 0;
    g_config.verboseLog = g_verboseOn ? 1 : 0;
    g_config.skipIntro = g_skipIntroOn ? 1 : 0;
    g_config.minimizeOnPlay = g_minimizeOn ? 1 : 0;
    wcscpy_s(g_config.theme, 8, g_pal.dark ? L"dark" : L"light");
    wcscpy_s(g_config.photoFormat, 8, SendMessageW(g_hPhotoFmtCombo, CB_GETCURSEL, 0, 0) == 1 ? L"jpg" : L"png");
    wcscpy_s(g_config.uiLang, 8, kUiLangCodes[g_uiLang]);
    ReadGameLangCombo(g_hTextLangCombo, g_config.textLang, 16);
    ReadGameLangCombo(g_hVoiceLangCombo, g_config.voiceLang, 16);
    GetWindowTextW(g_hGamePathEdit, g_config.gamePath, MAX_PATH);
    GetWindowTextW(g_hSaveEdit, g_config.savePath, MAX_PATH);
    // player name: trimmed, no characters Windows can't use in a folder name
    wchar_t name[32] = {}, clean[32] = {};
    GetWindowTextW(g_hPlayerEdit, name, 32);
    size_t n = 0;
    for (const wchar_t *p = name; *p && n < 31; ++p)
        if (*p >= 32 && !wcschr(L"<>:\"/\\|?*", *p)) clean[n++] = *p;
    while (n > 0 && (clean[n - 1] == L' ' || clean[n - 1] == L'.')) clean[--n] = 0;
    const wchar_t *s = clean; while (*s == L' ') ++s;
    wcscpy_s(g_config.playerName, 32, *s ? s : L"Player 1");
}

// Puts every launcher text in the current language (g_uiLang); keeps the selections.
static void UpdateButtonText()
{
    if (!g_hBtnUpdate) return;
    wchar_t t[96];
    if (g_updateDownloading) swprintf_s(t, 96, T(S_UPD_DL), g_updatePercent);
    else if (g_updateChecking) wcscpy_s(t, T(S_UPD_CHECKING));
    else if (g_updateKnown && g_updateLast.status == UPD_NEWER) swprintf_s(t, 96, T(S_UPD_AVAIL), g_updateLast.tag);
    else wcscpy_s(t, T(S_UPD_CHECK));
    SetWindowTextW(g_hBtnUpdate, t);
    InvalidateRect(g_hBtnUpdate, nullptr, TRUE);
}

static void ApplyUiLanguage(HWND hwnd)
{
    UpdateButtonText();
    wchar_t textLang[16], voiceLang[16];
    ReadGameLangCombo(g_hTextLangCombo, textLang, 16);
    ReadGameLangCombo(g_hVoiceLangCombo, voiceLang, 16);
    if (SendMessageW(g_hTextLangCombo, CB_GETCOUNT, 0, 0) == 0) { wcscpy_s(textLang, g_config.textLang); wcscpy_s(voiceLang, g_config.voiceLang); }
    FillGameLangCombo(g_hTextLangCombo, T(S_LANG_SYSTEM), textLang);
    FillGameLangCombo(g_hVoiceLangCombo, T(S_LANG_SAME), voiceLang);

    int modeSel = (int)SendMessageW(g_hModeCombo, CB_GETCURSEL, 0, 0);
    if (modeSel < 0) modeSel = _wcsicmp(g_config.windowMode, L"Windowed") == 0 ? 1 : _wcsicmp(g_config.windowMode, L"Fullscreen") == 0 ? 2 : 0;
    SendMessageW(g_hModeCombo, CB_RESETCONTENT, 0, 0);
    const UiStr modes[] = { S_MODE_BORDERLESS, S_MODE_WINDOW, S_MODE_FULL };
    for (auto s : modes) SendMessageW(g_hModeCombo, CB_ADDSTRING, 0, (LPARAM)T(s));
    SendMessageW(g_hModeCombo, CB_SETCURSEL, modeSel, 0);

    int photoSel = (int)SendMessageW(g_hPhotoFmtCombo, CB_GETCURSEL, 0, 0);
    if (photoSel < 0) photoSel = _wcsicmp(g_config.photoFormat, L"jpg") == 0 ? 1 : 0;
    SendMessageW(g_hPhotoFmtCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hPhotoFmtCombo, CB_ADDSTRING, 0, (LPARAM)T(S_PHOTO_PNG));
    SendMessageW(g_hPhotoFmtCombo, CB_ADDSTRING, 0, (LPARAM)T(S_PHOTO_JPG));
    SendMessageW(g_hPhotoFmtCombo, CB_SETCURSEL, photoSel, 0);

    SendMessageW(g_hThemeCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hThemeCombo, CB_ADDSTRING, 0, (LPARAM)T(S_THEME_DARK));
    SendMessageW(g_hThemeCombo, CB_ADDSTRING, 0, (LPARAM)T(S_THEME_LIGHT));
    SendMessageW(g_hThemeCombo, CB_SETCURSEL, g_pal.dark ? 0 : 1, 0);
    SetWindowTextW(g_hMinimizeChk, T(S_MINIMIZE));

    SetWindowTextW(g_hVsyncChk, T(S_VSYNC));
    SetWindowTextW(g_hSkipIntroChk, T(S_SKIP));
    SetWindowTextW(g_hAdvBtn, T(S_ADV));
    SetWindowTextW(g_hD3DDebugChk, T(S_D3DDBG));
    SetWindowTextW(g_hVerboseChk, T(S_VERBOSE));
    SetWindowTextW(g_hBtnOpenGame, T(S_BTN_GAME));
    SetWindowTextW(g_hBtnOpenSave, T(S_BTN_SAVES));
    SetWindowTextW(g_hClearLogBtn, T(S_CLEAR));
    SetWindowTextW(g_hCopyLogBtn, T(S_COPY));
    SetWindowTextW(g_hLogBtn, g_showLog ? T(S_HIDELOG) : T(S_SHOWLOG));
    SetWindowTextW(g_hBtnLaunch, !IsWindowEnabled(g_hBtnLaunch) ? T(S_PREP) : g_gameRunning ? T(S_CLOSE) : T(S_PLAY));
    if (g_hTabSettings) PlApplyLanguage();
    if (g_hProfList) ProfApplyLanguage();
    if (g_hPhList) PhApplyLanguage();
    if (g_hKbList) KbApplyLanguage();
    InvalidateRect(hwnd, nullptr, TRUE);
}

#include "shader_prep.inc"

#define TIMER_PLAYTIME    0x7702
#define TIMER_DISCORD     0x7703
static long long g_discordStart = 0;   // game start (epoch seconds) shown as "elapsed" in Discord
static bool g_discordShown = false;     // activity accepted by Discord for this game session
static std::string g_discordLastErr;    // last failure already logged (logged once per change)

// Discord Rich Presence while the game runs: (re)connects every 15 s until Discord accepts the activity, so it also works
// when Discord is opened (or restarted) after the game. Every failure reason is written to the launcher log once.
static void DiscordUpdate()
{
    if (!g_discordOn || !g_gameRunning) return;
    if (g_discordShown && g_discord.Alive()) return;
    g_discordShown = false;
    bool ok = g_discord.Connect(g_discordAppId) && g_discord.SetPlaying(g_discordStart);
    if (ok) { g_discordShown = true; g_discordLastErr.clear(); LogPrintf(L"[Discord] Rich Presence ativo\r\n"); return; }
    if (g_discord.lastError != g_discordLastErr)
    {
        g_discordLastErr = g_discord.lastError;
        wchar_t w[512] = {}; MultiByteToWideChar(CP_UTF8, 0, g_discordLastErr.c_str(), -1, w, 511);
        LogPrintf(L"[Discord] Rich Presence: %ls (tentando de novo a cada 15 s)\r\n", w);
    }
}
static bool g_prepLaunch = true;      // start the game when the preparation finishes

// preparation (packs -> translate + compile -> pipelines, shader_prep.inc); WM_SHADER_PREP_DONE starts the game if launch is set
static void BeginShaderPrep(HWND hwnd, bool launch)
{
    g_prepLaunch = launch;
    EnableWindow(g_hBtnLaunch, FALSE);
    SetWindowTextW(g_hBtnLaunch, T(S_PREP));
    StartShaderPrep(hwnd, g_gameDirShown);
}

static void CreateThemeBrushes()
{
    HBRUSH old[] = { g_hBrushMain, g_hBrushCard, g_hBrushInput, g_hBrushLog };
    g_hBrushMain = CreateSolidBrush(COLOR_BG_MAIN);
    g_hBrushCard = CreateSolidBrush(COLOR_BG_CARD);
    g_hBrushInput = CreateSolidBrush(COLOR_BG_INPUT);
    g_hBrushLog = CreateSolidBrush(COLOR_BG_LOG);
    for (HBRUSH b : old) if (b) DeleteObject(b);
}

// Switches the launcher between the dark and the light palette while it runs.
static void ApplyTheme(HWND hwnd, bool dark)
{
    g_pal = dark ? kPaletteDark : kPaletteLight;
    CreateThemeBrushes();
    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    HWND combos[] = { g_hUiLangCombo, g_hResCombo, g_hModeCombo, g_hTextLangCombo, g_hVoiceLangCombo, g_hPhotoFmtCombo, g_hThemeCombo, g_hPlCombo };
    for (HWND c : combos) if (c) SetWindowTheme(c, dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    HWND lists[] = { g_hLogEdit, g_hProfList, g_hPhList, g_hKbList, g_hPlList };
    for (HWND l : lists) if (l) SetWindowTheme(l, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    PhApplyThemeColors();
    KbApplyThemeColors();
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NOTIFY && PhHandleNotify(hwnd, (NMHDR *)lParam)) return 0;
    if (msg == WM_NOTIFY && KbHandleNotify(hwnd, (NMHDR *)lParam)) return 0;
    if (g_hTabSettings) { LRESULT plr = 0; if (PlHandleMessage(hwnd, msg, wParam, lParam, &plr)) return plr; }
    switch (msg)
    {
    case WM_CREATE:
    {
        BOOL dark = g_pal.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        auto mk = [](int h, int w, const wchar_t *face) {
            return CreateFontW(-h, 0, 0, 0, w, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face); };
        g_hFontUI = mk(14, FW_NORMAL, L"Segoe UI");
        g_hFontUIBold = mk(14, FW_SEMIBOLD, L"Segoe UI");
        g_hFontSmallCaps = mk(11, FW_BOLD, L"Segoe UI");
        g_hFontPlay = mk(18, FW_BOLD, L"Segoe UI");
        g_hFontLog = mk(12, FW_NORMAL, L"Consolas");

        wchar_t initialGameDir[MAX_PATH];
        GetEffectiveGameDir(&g_config, initialGameDir, MAX_PATH);
        if (g_config.gamePath[0] == L'\0') wcscpy_s(g_config.gamePath, MAX_PATH, initialGameDir);
        LoadDiscordIni();
        g_vsyncOn = g_config.vsync != 0; g_d3dDebugOn = g_config.d3d12Debug != 0; g_verboseOn = g_config.verboseLog != 0; g_skipIntroOn = g_config.skipIntro != 0;
        g_minimizeOn = g_config.minimizeOnPlay != 0;

        g_uiLang = 0;
        for (int i = 0; i < 3; ++i) if (_wcsicmp(g_config.uiLang, kUiLangCodes[i]) == 0) g_uiLang = i;

        const int X = UI_PANEL_X, W = UI_PANEL_W, HALF = (UI_PANEL_W - 12) / 2, X2 = UI_PANEL_X + HALF + 12;
        DWORD comboStyle = WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL;
        // launcher language, top right of the panel
        g_hUiLangCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X + W - 124, 20, 124, 200, hwnd, (HMENU)ID_UILANG_COMBO, nullptr, nullptr);
        for (auto s : kUiLangNames) SendMessageW(g_hUiLangCombo, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_hUiLangCombo, CB_SETCURSEL, g_uiLang, 0);

        g_hResCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X, 78, HALF, 300, hwnd, (HMENU)ID_RES_COMBO, nullptr, nullptr);
        const wchar_t *res[] = { L"1920 × 1080 (FHD)", L"2560 × 1440 (QHD)", L"3840 × 2160 (4K)", L"1280 × 720 (HD)" };
        for (auto s : res) SendMessageW(g_hResCombo, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_hResCombo, CB_SETCURSEL, g_config.width == 2560 ? 1 : g_config.width == 3840 ? 2 : g_config.width == 1280 ? 3 : 0, 0);

        g_hModeCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X2, 78, HALF, 300, hwnd, (HMENU)ID_MODE_COMBO, nullptr, nullptr);
        // game text / voice language (filled by ApplyUiLanguage)
        g_hTextLangCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X, 140, HALF, 300, hwnd, (HMENU)ID_TEXTLANG_COMBO, nullptr, nullptr);
        g_hVoiceLangCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X2, 140, HALF, 300, hwnd, (HMENU)ID_VOICELANG_COMBO, nullptr, nullptr);
        g_hPhotoFmtCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X, 276, HALF, 200, hwnd, (HMENU)ID_PHOTOFMT_COMBO, nullptr, nullptr);
        g_hThemeCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X2, 276, HALF, 200, hwnd, (HMENU)ID_THEME_COMBO, nullptr, nullptr);
        HWND combos[] = { g_hUiLangCombo, g_hResCombo, g_hModeCombo, g_hTextLangCombo, g_hVoiceLangCombo, g_hPhotoFmtCombo, g_hThemeCombo };
        for (HWND c : combos) SetWindowTheme(c, g_pal.dark ? L"DarkMode_CFD" : L"CFD", nullptr);

        DWORD od = WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_OWNERDRAW;
        g_hVsyncChk = CreateWindowW(L"BUTTON", L"", od, X, 178, W, 24, hwnd, (HMENU)ID_VSYNC_CHK, nullptr, nullptr);
        g_hDiscordChk = CreateWindowW(L"BUTTON", L"Discord Rich Presence", od, X, 202, W, 24, hwnd, (HMENU)ID_DISCORD_CHK, nullptr, nullptr);
        g_hSkipIntroChk = CreateWindowW(L"BUTTON", L"", od, X, 226, W, 24, hwnd, (HMENU)ID_SKIPINTRO_CHK, nullptr, nullptr);
        g_hMinimizeChk = CreateWindowW(L"BUTTON", L"", od, X, 388, W, 24, hwnd, (HMENU)ID_MINIMIZE_CHK, nullptr, nullptr);

        DWORD es = WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL;
        g_hPlayerEdit = CreateWindowW(L"EDIT", g_config.playerName, es & ~WS_VISIBLE, X + 10, 283, HALF - 20, 20, hwnd, (HMENU)ID_PLAYER_EDIT, nullptr, nullptr);
        SendMessageW(g_hPlayerEdit, EM_LIMITTEXT, 20, 0);
        g_hGamePathEdit = CreateWindowW(L"EDIT", g_config.gamePath, es, X + 10, 347, W - 68, 20, hwnd, (HMENU)ID_GAME_PATH_EDIT, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"…", od, X + W - 40, 340, 40, 32, hwnd, (HMENU)ID_GAME_PATH_BROWSE, nullptr, nullptr);
        g_hSaveEdit = CreateWindowW(L"EDIT", g_config.savePath, es, X + 10, 347, W - 68, 20, hwnd, (HMENU)ID_SAVE_EDIT, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"…", od, X + W - 40, 340, 40, 32, hwnd, (HMENU)ID_SAVE_BROWSE, nullptr, nullptr);

        g_hAdvBtn = CreateWindowW(L"BUTTON", L"", od, X, 450, W, 24, hwnd, (HMENU)ID_BTN_ADVANCED, nullptr, nullptr);
        g_hD3DDebugChk = CreateWindowW(L"BUTTON", L"", od & ~WS_VISIBLE, X, 476, W, 24, hwnd, (HMENU)ID_D3DDEBUG_CHK, nullptr, nullptr);
        g_hVerboseChk = CreateWindowW(L"BUTTON", L"", od & ~WS_VISIBLE, X, 502, W, 24, hwnd, (HMENU)ID_VERBOSE_CHK, nullptr, nullptr);

        g_hBtnLaunch = CreateWindowW(L"BUTTON", L"", od, X, UI_H - 112, W, 56, hwnd, (HMENU)ID_BTN_LAUNCH, nullptr, nullptr);
        int bw = (W - 16) / 3;
        g_hBtnOpenGame = CreateWindowW(L"BUTTON", L"", od, X, UI_H - 46, bw, 30, hwnd, (HMENU)ID_BTN_OPEN_GAME, nullptr, nullptr);
        g_hBtnOpenSave = CreateWindowW(L"BUTTON", L"", od, X + bw + 8, UI_H - 46, bw, 30, hwnd, (HMENU)ID_BTN_OPEN_SAVE, nullptr, nullptr);
        g_hLogBtn = CreateWindowW(L"BUTTON", L"", od, X + 2 * (bw + 8), UI_H - 46, bw, 30, hwnd, (HMENU)ID_BTN_SHOW_LOG, nullptr, nullptr);

        // log overlay (over the art), hidden until "Ver log"
        g_hLogEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                   24, 24, UI_HERO_W - 48, UI_H - 92, hwnd, (HMENU)ID_LOG_EDIT, nullptr, nullptr);
        SetWindowTheme(g_hLogEdit, g_pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        g_hClearLogBtn = CreateWindowW(L"BUTTON", L"", od & ~WS_VISIBLE, 24, UI_H - 56, 90, 30, hwnd, (HMENU)ID_BTN_CLEAR_LOG, nullptr, nullptr);
        g_hCopyLogBtn = CreateWindowW(L"BUTTON", L"", od & ~WS_VISIBLE, 122, UI_H - 56, 90, 30, hwnd, (HMENU)ID_BTN_COPY_LOG, nullptr, nullptr);
        g_hBtnKill = nullptr;
        g_hBtnUpdate = CreateWindowW(L"BUTTON", L"", od, 16, UI_H - 48, 184, 30, hwnd, (HMENU)ID_BTN_UPDATE, nullptr, nullptr);
        PlCreateControls(hwnd);
        ProfCreateControls(hwnd);
        PhCreateControls(hwnd);
        KbCreateControls(hwnd);

        EnumChildWindows(hwnd, [](HWND h, LPARAM) -> BOOL {
            SendMessageW(h, WM_SETFONT, (WPARAM)(GetDlgCtrlID(h) == ID_LOG_EDIT ? g_hFontLog : g_hFontUI), TRUE);
            return TRUE; }, 0);
        for (HWND c : combos) SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, 24);
        ApplyUiLanguage(hwnd);
        PlUpdateVisibility(hwnd);

        // "OnRushLauncher.exe -setup <copy of the game>": set the game up in the launcher folder without asking
        {
            int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            for (int i = 1; argv && i + 1 < argc; ++i)
                if (!_wcsicmp(argv[i], L"-patch")) wcscpy_s(g_cmdPatchDir, MAX_PATH, argv[i + 1]);
            for (int i = 1; argv && i + 1 < argc; ++i)
            {
                if (_wcsicmp(argv[i], L"-setup") != 0) continue;
                wchar_t sm[MAX_PATH], sr[MAX_PATH]; std::wstring err;
                if (!FindGameCopy(argv[i + 1], sm, sr)) LogPrintf(L"[Setup] cópia do jogo não encontrada em %ls\r\n", argv[i + 1]);
                else if (GameCopyVersion(sm) != kFinalVersion && !OfferGamePatch(hwnd, sm, sr)) LogPrintf(L"[Setup] cópia não está na versão %ls\r\n", kFinalVersion);
                else if (!SetupGameFolder(sm, sr, g_exeDir, err)) LogPrintf(L"[Setup] falhou: %ls\r\n", err.c_str());
                else
                {
                    SetWindowTextW(g_hGamePathEdit, g_exeDir);
                    wcscpy_s(g_config.gamePath, MAX_PATH, g_exeDir); wcscpy_s(initialGameDir, MAX_PATH, g_exeDir);
                    SaveIni();
                }
                break;
            }
            if (argv) LocalFree(argv);
        }
        RefreshGameDirStatus(hwnd);

        SysSpecs specs = GetHostSpecs();
        LogPrintf(L"ONRUSH — PC Native Port (launcher)\r\n");
        LogPrintf(L"CPU: %u núcleos | RAM: %llu MB (%llu MB livres) | AVX2: %ls\r\n",
                  specs.cores, specs.ramTotalMB, specs.ramAvailMB, specs.hasAVX2 ? L"sim" : L"não");
        LogPrintf(L"Pasta do jogo: %ls\r\n\r\n", initialGameDir);
        for (const auto &p : CheckPrerequisites(initialGameDir))
            LogPrintf(L"  [%ls] %ls (%ls)\r\n", p.found ? L" OK " : L"FALTA", p.name, p.file);
        // "OnRushLauncher.exe -prepshaders": run only the shader/pipeline preparation and exit
        // "OnRushLauncher.exe -autolaunch": press JOGAR on start (automated tests of the real launcher path)
        if (wcsstr(GetCommandLineW(), L"-prepshaders") || wcsstr(GetCommandLineW(), L"-autolaunch"))
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_BTN_LAUNCH, 0), 0);
        else if (!wcsstr(GetCommandLineW(), L"-noupdatecheck"))
        {
            if (wcsstr(GetCommandLineW(), L"-updated"))
            {   // just installed by the updater: say so and remove its temp folder
                LogPrintf(L"[Atualização] launcher atualizado para v%ls\r\n", OLAUNCHER_VERSION);
                CloseHandle(CreateThread(nullptr, 0, UpdateCleanupThread, nullptr, 0, nullptr));
            }
            StartUpdateCheck(hwnd, false);  // silent: only highlights the button when a newer release exists
            UpdateButtonText();
        }
        return 0;
    }

    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lParam;
        if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = 24; return TRUE; }
        if (mi->CtlType == ODT_LISTBOX) { mi->itemHeight = mi->CtlID == ID_PROF_LIST ? 34 : 48; return TRUE; }
        break;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lParam;
        if (di->CtlType == ODT_COMBOBOX) DrawComboItem(di);
        else if (di->CtlType == ODT_LISTBOX && di->CtlID == ID_PROF_LIST) ProfDrawListItem(di);
        else if (di->CtlType == ODT_LISTBOX) PlDrawListItem(di);
        else if (di->CtlType == ODT_BUTTON) DrawOwnerButton(di);
        return TRUE;
    }

    case WM_APPEND_LOG:
    {
        wchar_t *text = (wchar_t *)lParam;
        if (text) { AppendLogText(text); free(text); }
        return 0;
    }

    case WM_GAME_STARTED:
        g_gameRunning = true;
        SetLaunchButtonState();
        g_playStartTick = GetTickCount64(); g_playProfile = g_config.playerName;
        SetTimer(hwnd, TIMER_PLAYTIME, 60000, nullptr);  // keeps the play time on the home card current
        if (g_minimizeOn && !IsIconic(hwnd)) { g_minimizedForGame = true; ShowWindow(hwnd, SW_MINIMIZE); }
        g_discordStart = (long long)time(nullptr); g_discordShown = false; g_discordLastErr.clear();
        DiscordUpdate();
        SetTimer(hwnd, TIMER_DISCORD, 15000, nullptr);
        return 0;

    case WM_SHADER_PREP_DONE:
        EnableWindow(g_hBtnLaunch, TRUE);
        SetLaunchButtonState();
        if (wcsstr(GetCommandLineW(), L"-prepshaders")) { DestroyWindow(hwnd); return 0; }  // prepare only, don't start the game
        if (!g_prepLaunch) return 0;  // translation after the game closed: nothing else to do
        g_hWorkerThread = CreateThread(nullptr, 0, GameSupervisorThread, &g_config, 0, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == KB_TIMER_ID) { KbOnTimer(hwnd); return 0; }
        if (wParam == TIMER_DISCORD) { DiscordUpdate(); return 0; }
        if (wParam == TIMER_PLAYTIME) { if (g_tab == 0) InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        break;

    case WM_UPDATE_RESULT:
    {
        UpdateResult *r = (UpdateResult *)lParam;
        g_updateChecking = false;
        g_updateLast = *r; g_updateKnown = true;
        UpdateButtonText();
        wchar_t msgText[512];
        if (r->status != UPD_NEWER)
            LogPrintf(L"[Atualização] Launcher v%ls: %ls%ls\r\n", OLAUNCHER_VERSION,
                      r->status == UPD_LATEST ? L"versão mais recente" : r->status == UPD_NO_RELEASE ? L"nenhuma versão publicada no GitHub" : L"falha na verificação — ",
                      r->status == UPD_ERROR ? r->error : L"");
        if (r->status == UPD_NEWER)
            LogPrintf(L"[Atualização] Nova versão do launcher: %ls (atual v%ls) — %ls\r\n", r->tag, OLAUNCHER_VERSION, r->url);
        if (r->manual)
        {
            if (r->status == UPD_NEWER && r->asset[0] && r->sha256[0])
            {   // automatic update: download, close, install, reopen
                swprintf_s(msgText, 512, T(S_UPD_ASK_INSTALL), r->tag, OLAUNCHER_VERSION);
                if (MessageBoxW(hwnd, msgText, L"RushLauncher", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                {
                    if (g_gameRunning || g_hPrepWnd) MessageBoxW(hwnd, T(S_UPD_GAMERUN), L"RushLauncher", MB_ICONINFORMATION);
                    else if (StartUpdateDownload(hwnd, *r)) LogPrintf(L"[Atualização] baixando %ls…\r\n", r->asset);
                    UpdateButtonText();
                }
            }
            else if (r->status == UPD_NEWER)
            {
                swprintf_s(msgText, 512, T(S_UPD_ASK), r->tag, OLAUNCHER_VERSION);
                if (MessageBoxW(hwnd, msgText, L"RushLauncher", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                    ShellExecuteW(hwnd, L"open", r->url, nullptr, nullptr, SW_SHOWNORMAL);
            }
            else if (r->status == UPD_LATEST) { swprintf_s(msgText, 512, T(S_UPD_LATEST), OLAUNCHER_VERSION); MessageBoxW(hwnd, msgText, L"RushLauncher", MB_ICONINFORMATION); }
            else if (r->status == UPD_NO_RELEASE) MessageBoxW(hwnd, T(S_UPD_NONE), L"RushLauncher", MB_ICONINFORMATION);
            else { swprintf_s(msgText, 512, T(S_UPD_FAIL), r->error); MessageBoxW(hwnd, msgText, L"RushLauncher", MB_ICONWARNING); }
        }
        delete r;
        return 0;
    }

    case WM_UPDATE_PROGRESS:
        g_updatePercent = (int)wParam;
        UpdateButtonText();
        return 0;

    case WM_UPDATE_READY:
    {
        UpdateJob *job = (UpdateJob *)lParam;
        g_updateDownloading = false;
        UpdateButtonText();
        wchar_t msgText[512], err[160] = {};
        if (job->ok && (g_gameRunning || g_hPrepWnd))
            MessageBoxW(hwnd, T(S_UPD_GAMERUN), L"RushLauncher", MB_ICONINFORMATION);
        else if (job->ok && LaunchUpdateInstaller(job, err, 160))
        {
            LogPrintf(L"[Atualização] %ls baixada e conferida; o launcher fecha para instalar\r\n", job->tag);
            ReadUiIntoConfig();
            SaveIni();
            delete job;
            DestroyWindow(hwnd);
            return 0;
        }
        else
        {
            const wchar_t *why = job->ok ? err : job->error;
            LogPrintf(L"[Atualização] falhou: %ls\r\n", why);
            swprintf_s(msgText, 512, T(S_UPD_DLFAIL), why);
            if (MessageBoxW(hwnd, msgText, L"RushLauncher", MB_YESNO | MB_ICONWARNING) == IDYES)
                ShellExecuteW(hwnd, L"open", g_updateLast.url[0] ? g_updateLast.url : OLAUNCHER_RELEASES L"/latest", nullptr, nullptr, SW_SHOWNORMAL);
        }
        delete job;
        return 0;
    }

    case WM_GAME_EXITED:
        g_gameRunning = false;
        {   // options changed in the game's GRAPHICS page were written to onrush.ini: take them back
            wchar_t ini[MAX_PATH];
            swprintf_s(ini, MAX_PATH, L"%ls\\onrush.ini", g_exeDir);
            g_config.vsync = GetPrivateProfileIntW(L"Display", L"VSync", g_config.vsync, ini);
            g_config.gfxPreset = GetPrivateProfileIntW(L"Graphics", L"Preset", g_config.gfxPreset, ini);
            g_config.fpsLimit = GetPrivateProfileIntW(L"Graphics", L"FpsLimit", g_config.fpsLimit, ini);
            g_vsyncOn = g_config.vsync != 0;
            // ... and so were the languages picked in the game's first options page
            GetPrivateProfileStringW(L"Language", L"Text", g_config.textLang, g_config.textLang, 16, ini);
            GetPrivateProfileStringW(L"Language", L"Voice", g_config.voiceLang, g_config.voiceLang, 16, ini);
            if (g_hTextLangCombo) FillGameLangCombo(g_hTextLangCombo, T(S_LANG_SYSTEM), g_config.textLang);
            if (g_hVoiceLangCombo) FillGameLangCombo(g_hVoiceLangCombo, T(S_LANG_SAME), g_config.voiceLang);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (g_minimizedForGame)
        {   // minimized by "Minimizar o launcher ao jogar": back when the game closes
            g_minimizedForGame = false;
            if (IsIconic(hwnd)) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        }
        if (g_playStartTick)
        {
            ULONGLONG secs = (GetTickCount64() - g_playStartTick) / 1000;
            ProfAddPlaySeconds(g_playProfile, secs);
            LogPrintf(L"[Perfis] sessão de %ls (%ls); total: %ls\r\n", ProfFmtPlay(secs).c_str(), g_playProfile.c_str(), ProfFmtPlay(ProfPlaySeconds(g_playProfile)).c_str());
            g_playStartTick = 0;
            KillTimer(hwnd, TIMER_PLAYTIME);
        }
        SetLaunchButtonState();
        KillTimer(hwnd, TIMER_DISCORD);
        g_discordShown = false;
        g_discord.Clear();
        g_discord.Close();
        // a shader the packs did not have (the bridge dumps any it meets) gets translated now, so the next JOGAR starts right away
        {
            wchar_t tr[MAX_PATH];
            if (!g_hPrepWnd && PrepTranslatorExe(tr) && PrepTranslationNeeded(g_gameDirShown) && !wcsstr(GetCommandLineW(), L"-autolaunch"))
            {
                LogPrintf(L"[Shaders] o jogo gravou shaders novos: traduzindo para a próxima vez\r\n");
                BeginShaderPrep(hwnd, false);
            }
        }
        return 0;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (ProfHandleCommand(hwnd, id, code)) return 0;
        if (PhHandleCommand(hwnd, id, code)) return 0;
        if (KbHandleCommand(hwnd, id, code)) return 0;
        switch (id)
        {
        case ID_BTN_LAUNCH:
            if (g_gameRunning)
            {
                if (g_hGameProcess) TerminateProcess(g_hGameProcess, 0);
                return 0;
            }
            PlPreviewStop();
            ReadUiIntoConfig();
            SaveIni();
            if (!GameDirIsReady(g_gameDirShown))
            {
                // the configured folder may be the player's copy itself: offer to set it up first
                wchar_t gm[MAX_PATH], gr[MAX_PATH];
                if (FindGameCopy(g_gameDirShown, gm, gr) && OfferGameSetup(hwnd, g_gameDirShown)) SetWindowTextW(g_hGamePathEdit, g_exeDir);
                else
                {
                    MessageBoxW(hwnd, T(S_NO_GAME), L"ONRUSH", MB_ICONINFORMATION);
                    if (!AskGameFolder(hwnd)) return 0;
                }
                ReadUiIntoConfig(); SaveIni();
                RefreshGameDirStatus(hwnd);
            }
            if (!GameAssetsReachable(g_gameDirShown) && !RelinkGameCopy(hwnd, g_gameDirShown)) return 0;
            if (!EnsureFinalGameVersion(hwnd, g_gameDirShown)) return 0;
            if (FileExistsIn(g_gameDirShown, L"game_release_xbox.exe") &&
                (!FileExistsIn(g_gameDirShown, L"onrush-pc.exe") || GameExeOutdated(g_gameDirShown) || GameExeIconStale(g_gameDirShown)) && BrandGameExe(g_gameDirShown))
                LogPrintf(L"[Setup] onrush-pc.exe criado a partir de game_release_xbox.exe\r\n");
            if (g_hPrepWnd) return 0;  // already preparing
            // shader/pipeline preparation first (popup with progress, the game is not opened for it); the game starts on WM_SHADER_PREP_DONE
            BeginShaderPrep(hwnd, true);
            return 0;
        case ID_VSYNC_CHK: g_vsyncOn = !g_vsyncOn; InvalidateRect((HWND)lParam, nullptr, TRUE); return 0;
        case ID_DISCORD_CHK:
            g_discordOn = !g_discordOn; InvalidateRect((HWND)lParam, nullptr, TRUE); SaveIni();
            if (!g_discordOn) { g_discord.Clear(); g_discord.Close(); g_discordShown = false; }
            else { g_discordLastErr.clear(); DiscordUpdate(); }
            return 0;
        case ID_D3DDEBUG_CHK: g_d3dDebugOn = !g_d3dDebugOn; InvalidateRect((HWND)lParam, nullptr, TRUE); return 0;
        case ID_VERBOSE_CHK: g_verboseOn = !g_verboseOn; InvalidateRect((HWND)lParam, nullptr, TRUE); return 0;
        case ID_SKIPINTRO_CHK: g_skipIntroOn = !g_skipIntroOn; InvalidateRect((HWND)lParam, nullptr, TRUE); return 0;
        case ID_MINIMIZE_CHK: g_minimizeOn = !g_minimizeOn; InvalidateRect((HWND)lParam, nullptr, TRUE); ReadUiIntoConfig(); SaveIni(); return 0;
        case ID_THEME_COMBO:
            if (code == CBN_SELCHANGE)
            {
                bool dark = SendMessageW(g_hThemeCombo, CB_GETCURSEL, 0, 0) != 1;
                if (dark != g_pal.dark) { ApplyTheme(hwnd, dark); ReadUiIntoConfig(); SaveIni(); }
            }
            return 0;
        case ID_BTN_UPDATE:
            if (g_updateKnown && g_updateLast.status == UPD_NEWER && !g_updateChecking)
            {
                // a newer release is already known: go straight to the question
                UpdateResult *r = new UpdateResult(g_updateLast); r->manual = true;
                PostMessageW(hwnd, WM_UPDATE_RESULT, 0, (LPARAM)r);
                g_updateChecking = true;
                return 0;
            }
            StartUpdateCheck(hwnd, true);
            UpdateButtonText();
            return 0;
        case ID_BTN_ADVANCED: g_showAdvanced = !g_showAdvanced; LayoutAdvanced(hwnd); return 0;
        case ID_BTN_SHOW_LOG: if (g_tab != 0) PlSetTab(hwnd, 0); g_showLog = !g_showLog; LayoutLog(hwnd); return 0;
        case ID_BTN_CLEAR_LOG: SetWindowTextW(g_hLogEdit, L""); return 0;
        case ID_BTN_COPY_LOG:
        {
            int len = GetWindowTextLengthW(g_hLogEdit);
            if (len <= 0 || !OpenClipboard(hwnd)) return 0;
            EmptyClipboard();
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
            if (hg) { GetWindowTextW(g_hLogEdit, (wchar_t *)GlobalLock(hg), len + 1); GlobalUnlock(hg); SetClipboardData(CF_UNICODETEXT, hg); }
            CloseClipboard();
            return 0;
        }
        case ID_BTN_OPEN_GAME:
            RefreshGameDirStatus(hwnd);
            ShellExecuteW(hwnd, L"open", g_gameDirShown, nullptr, nullptr, SW_SHOW);
            return 0;
        case ID_BTN_OPEN_SAVE:
        {
            wchar_t savePath[MAX_PATH], full[MAX_PATH];
            GetWindowTextW(g_hSaveEdit, savePath, MAX_PATH);
            if (PathIsRelativeW(savePath)) { RefreshGameDirStatus(hwnd); PathCombineW(full, g_gameDirShown, savePath); }
            else wcscpy_s(full, MAX_PATH, savePath);
            CreateDirectoryW(full, nullptr);
            ShellExecuteW(hwnd, L"open", full, nullptr, nullptr, SW_SHOW);
            return 0;
        }
        case ID_GAME_PATH_BROWSE:
            AskGameFolder(hwnd);
            ReadUiIntoConfig(); SaveIni();
            RefreshGameDirStatus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case ID_SAVE_BROWSE:
            { wchar_t d[MAX_PATH]; if (PickFolder(hwnd, T(S_PICK_SAVE), d)) SetWindowTextW(g_hSaveEdit, d); }
            ReadUiIntoConfig(); SaveIni();
            return 0;
        case ID_PLAYER_EDIT:
            if (code == EN_KILLFOCUS) { ReadUiIntoConfig(); SetWindowTextW(g_hPlayerEdit, g_config.playerName); SaveIni(); }
            return 0;
        case ID_GAME_PATH_EDIT:
            if (code == EN_CHANGE) { RefreshGameDirStatus(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case ID_UILANG_COMBO:
            if (code == CBN_SELCHANGE)
            {
                int sel = (int)SendMessageW(g_hUiLangCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < 3 && sel != g_uiLang) { g_uiLang = sel; ApplyUiLanguage(hwnd); ReadUiIntoConfig(); SaveIni(); }
            }
            return 0;
        case ID_TEXTLANG_COMBO:
        case ID_VOICELANG_COMBO:
        case ID_PHOTOFMT_COMBO:
            if (code == CBN_SELCHANGE) { ReadUiIntoConfig(); SaveIni(); }
            return 0;
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // double buffer the whole client area
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, UI_W, UI_H);
        HGDIOBJ old = SelectObject(mem, bmp);
        DrawHero(mem);
        DrawPanel(mem);
        BitBlt(hdc, 0, 0, UI_W, UI_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        HDC h = (HDC)wParam;
        if ((HWND)lParam == g_hLogEdit)
        {
            SetTextColor(h, COLOR_LOG_TEXT);
            SetBkColor(h, COLOR_BG_LOG);
            return (INT_PTR)g_hBrushLog;
        }
        SetTextColor(h, COLOR_TEXT_PRIMARY);
        SetBkColor(h, COLOR_BG_INPUT);
        return (INT_PTR)g_hBrushInput;
    }

    case WM_CTLCOLORLISTBOX:
    {
        HDC h = (HDC)wParam;
        SetTextColor(h, COLOR_TEXT_PRIMARY);
        SetBkColor(h, COLOR_BG_INPUT);
        return (INT_PTR)g_hBrushInput;
    }

    case WM_CTLCOLORBTN:
        return (INT_PTR)g_hBrushCard;

    case WM_DESTROY:
    {
        ReadUiIntoConfig();
        SaveIni();
        PlShutdown();
        HFONT fonts[] = { g_hFontUI, g_hFontUIBold, g_hFontSmallCaps, g_hFontPlay, g_hFontLog };
        for (HFONT f : fonts) if (f) DeleteObject(f);
        HBRUSH brushes[] = { g_hBrushMain, g_hBrushCard, g_hBrushInput, g_hBrushLog };
        for (HBRUSH b : brushes) if (b) DeleteObject(b);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Application Entrypoint
// ============================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    if (wcsstr(GetCommandLineW(), L"-applyupdate")) return RunApplyUpdate();  // the updater copy (self_update.inc)
    GetModuleFileNameW(nullptr, g_exeDir, MAX_PATH);
    wchar_t *lastSlash = wcsrchr(g_exeDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    SetDefaultConfig(&g_config);
    wchar_t iniPath[MAX_PATH];
    swprintf_s(iniPath, MAX_PATH, L"%ls\\onrush.ini", g_exeDir);
    LoadConfigFile(iniPath, &g_config);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icex);
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, nullptr);

    g_pal = _wcsicmp(g_config.theme, L"light") == 0 ? kPaletteLight : kPaletteDark;
    CreateThemeBrushes();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OnRushLauncherV2";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT wr = { 0, 0, UI_W, UI_H };
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_APPWINDOW);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    g_hMainWnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"RushLauncher", style,
                                 (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                                 ww, wh, nullptr, nullptr, hInstance, nullptr);
    if (!g_hMainWnd) return 1;
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (IsDialogMessageW(g_hMainWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_discord.Clear();
    g_discord.Close();
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return (int)msg.wParam;
}
