// "Gráficos" card of the Configurações tab, in the content column: the same options as the rows the bridge adds to
// the game's Options > GRAPHICS page (src/d3d12_x/gfx_options.inc). They share onrush.ini: [Graphics] Preset and
// FpsLimit reach the game through the environment, Upscaler and the detail rows (real levels; choosing a quality
// preset moves every row to that preset's levels) are read by the bridge from the ini; changes made in the game are
// read back when it closes.

#define ID_GFX_COMBO0     1120   // .. ID_GFX_COMBO0 + GR_COUNT - 1

enum GfxRow { GR_PRESET, GR_FPS, GR_RES, GR_UPSCALER, GR_SHADOWS, GR_FOG, GR_WORLD, GR_VEHICLES, GR_TESS, GR_LIGHTFX,
              GR_DEBRIS, GR_COUNT };

struct GfxText { const wchar_t *t[3]; };   // pt, en, es
struct GfxRowDef { GfxText label; const GfxText *vals; int count; int detail; };   // detail: index in gfxDetail, -1 none

static const GfxText kGfxPresetVals[] = {
    { { L"Console (Xbox One)", L"Console (Xbox One)", L"Consola (Xbox One)" } },
    { { L"Ultra baixa", L"Ultra low", L"Ultra baja" } }, { { L"Baixa", L"Low", L"Baja" } },
    { { L"Média", L"Medium", L"Media" } }, { { L"Alta", L"High", L"Alta" } }, { { L"Ultra", L"Ultra", L"Ultra" } } };
static const int kGfxFps[] = { 30, 60, 90, 120, 144, 165, 240, 0 };   // 0 = unlimited (bridge kFpsChoices)
static const GfxText kGfxFpsVals[] = {
    { { L"30 FPS", L"30 FPS", L"30 FPS" } }, { { L"60 FPS", L"60 FPS", L"60 FPS" } }, { { L"90 FPS", L"90 FPS", L"90 FPS" } },
    { { L"120 FPS", L"120 FPS", L"120 FPS" } }, { { L"144 FPS", L"144 FPS", L"144 FPS" } },
    { { L"165 FPS", L"165 FPS", L"165 FPS" } }, { { L"240 FPS", L"240 FPS", L"240 FPS" } },
    { { L"Sem limite", L"Unlimited", L"Sin límite" } } };
static const GfxText kGfxResVals[] = {
    { { L"50%", L"50%", L"50%" } }, { { L"67%", L"67%", L"67%" } }, { { L"75%", L"75%", L"75%" } },
    { { L"83%", L"83%", L"83%" } }, { { L"100%", L"100%", L"100%" } }, { { L"125%", L"125%", L"125%" } },
    { { L"150%", L"150%", L"150%" } }, { { L"200%", L"200%", L"200%" } } };
static const GfxText kGfxUpscalerVals[] = { { { L"TAA", L"TAA", L"TAA" } }, { { L"FSR 2", L"FSR 2", L"FSR 2" } } };
static const GfxText kGfxShadowVals[] = {
    { { L"Baixa", L"Low", L"Baja" } }, { { L"Média", L"Medium", L"Media" } }, { { L"Alta", L"High", L"Alta" } },
    { { L"Ultra", L"Ultra", L"Ultra" } } };
static const GfxText kGfxLodVals[] = {
    { { L"Baixo", L"Low", L"Bajo" } }, { { L"Médio", L"Medium", L"Medio" } }, { { L"Alto", L"High", L"Alto" } },
    { { L"Ultra", L"Ultra", L"Ultra" } } };
static const GfxText kGfxTessVals[] = {
    { { L"Desligada", L"Off", L"Desactivada" } }, { { L"Ligada", L"On", L"Activada" } } };
static const GfxText kGfxFogVals[] = {
    { { L"Desligada", L"Off", L"Desactivada" } }, { { L"Média", L"Medium", L"Media" } }, { { L"Alta", L"High", L"Alta" } } };
static const GfxText kGfxLightVals[] = {
    { { L"Desligados", L"Off", L"Desactivados" } }, { { L"Ligados", L"On", L"Activados" } } };
static const GfxText kGfxDebrisVals[] = {
    { { L"Poucos", L"Few", L"Pocos" } }, { { L"Normal", L"Normal", L"Normal" } }, { { L"Muitos", L"Many", L"Muchos" } } };

// Rows in screen order (two columns); detail indexes follow kGfxDetailKeys.
static const GfxRowDef kGfxRows[GR_COUNT] = {
    { { { L"Qualidade gráfica", L"Graphics quality", L"Calidad gráfica" } }, kGfxPresetVals, 6, -1 },
    { { { L"Limite de quadros", L"Frame rate limit", L"Límite de FPS" } }, kGfxFpsVals, 8, -1 },
    { { { L"Resolução interna", L"Render resolution", L"Resolución interna" } }, kGfxResVals, 8, 0 },
    { { { L"Upscaler", L"Upscaler", L"Escalado" } }, kGfxUpscalerVals, 2, -1 },
    { { { L"Sombras", L"Shadows", L"Sombras" } }, kGfxShadowVals, 4, 1 },
    { { { L"Névoa", L"Fog", L"Niebla" } }, kGfxFogVals, 3, 5 },
    { { { L"Detalhe do cenário", L"World detail", L"Detalle del escenario" } }, kGfxLodVals, 4, 2 },
    { { { L"Detalhe dos veículos", L"Vehicle detail", L"Detalle de vehículos" } }, kGfxLodVals, 4, 3 },
    { { { L"Tesselação", L"Tessellation", L"Teselación" } }, kGfxTessVals, 2, 4 },
    { { { L"Efeitos de luz", L"Light effects", L"Efectos de luz" } }, kGfxLightVals, 2, 6 },
    { { { L"Detritos", L"Debris", L"Escombros" } }, kGfxDebrisVals, 3, 7 },
};
static const GfxText kGfxTitle = { { L"GRÁFICOS", L"GRAPHICS", L"GRÁFICOS" } };
static const GfxText kGfxHint = { {
    L"Escolher uma qualidade gráfica ajusta todas as opções para ela. As mesmas opções também ficam no jogo, em Opções > Gráficos.",
    L"Choosing a graphics quality sets every option to match it. The same options are also in the game, under Options > Graphics.",
    L"Elegir una calidad gráfica ajusta todas las opciones a ella. Las mismas opciones también están en el juego, en Opciones > Gráficos." } };

static HWND g_hGfxCombo[GR_COUNT] = {};

// Card geometry inside the content column (x 216 .. UI_HERO_W).
static const int kGfxCardL = 232, kGfxCardR = UI_HERO_W - 16, kGfxCardT = 58;
static const int kGfxColW = (kGfxCardR - kGfxCardL - 32 - 12) / 2;
static const int kGfxRowY0 = kGfxCardT + 16, kGfxRowStep = 60;
static int GfxColX(int i) { return kGfxCardL + 16 + (i % 2) * (kGfxColW + 12); }
static int GfxRowY(int i) { return kGfxRowY0 + (i / 2) * kGfxRowStep; }   // label; the combo is 20 px below
static const int kGfxCardB = kGfxRowY0 + ((GR_COUNT + 1) / 2) * kGfxRowStep + 74;

static int GfxIndexFromConfig(int row)
{
    const GfxRowDef &r = kGfxRows[row];
    int v = 0;
    if (row == GR_PRESET) v = g_config.gfxPreset;
    else if (row == GR_UPSCALER) v = g_config.upscaler;
    else if (row == GR_FPS) { v = 1; for (int i = 0; i < (int)_countof(kGfxFps); ++i) if (kGfxFps[i] == g_config.fpsLimit) v = i; }
    else if (r.detail >= 0) v = g_config.gfxDetail[r.detail];
    return v >= 0 && v < r.count ? v : 0;
}

static void GfxCreateControls(HWND hwnd)
{
    DWORD style = WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL;
    for (int i = 0; i < GR_COUNT; ++i)
    {
        g_hGfxCombo[i] = CreateWindowW(WC_COMBOBOXW, L"", style, GfxColX(i), GfxRowY(i) + 20, kGfxColW, 300, hwnd,
                                       (HMENU)(INT_PTR)(ID_GFX_COMBO0 + i), nullptr, nullptr);
        SetWindowTheme(g_hGfxCombo[i], g_pal.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    }
}

// (Re)fills the combos in the launcher language; fromConfig = select the values in g_config, else keep the selection.
static void GfxFill(bool fromConfig)
{
    for (int i = 0; i < GR_COUNT; ++i)
    {
        HWND c = g_hGfxCombo[i];
        if (!c) continue;
        int sel = fromConfig ? -1 : (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
        if (sel < 0) sel = GfxIndexFromConfig(i);
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        for (int v = 0; v < kGfxRows[i].count; ++v) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)kGfxRows[i].vals[v].t[g_uiLang]);
        SendMessageW(c, CB_SETCURSEL, sel, 0);
        SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, 24);
    }
}

static void GfxReadIntoConfig()
{
    for (int i = 0; i < GR_COUNT; ++i)
    {
        if (!g_hGfxCombo[i]) return;
        int v = (int)SendMessageW(g_hGfxCombo[i], CB_GETCURSEL, 0, 0);
        if (v < 0 || v >= kGfxRows[i].count) continue;
        if (i == GR_PRESET) g_config.gfxPreset = v;
        else if (i == GR_FPS) g_config.fpsLimit = kGfxFps[v];
        else if (i == GR_UPSCALER) g_config.upscaler = v;
        else if (kGfxRows[i].detail >= 0) g_config.gfxDetail[kGfxRows[i].detail] = v;
    }
}

static void GfxUpdateVisibility(bool show)
{
    for (HWND c : g_hGfxCombo) if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
}

static void GfxApplyTheme(bool dark)
{
    for (HWND c : g_hGfxCombo) if (c) SetWindowTheme(c, dark ? L"DarkMode_CFD" : L"CFD", nullptr);
}

// Title, card and row labels (Configurações tab); the combos draw themselves.
static void GfxDrawColumn(HDC hdc)
{
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_hFontPlay);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    TextOutW(hdc, kGfxCardL, 20, kGfxTitle.t[g_uiLang], (int)wcslen(kGfxTitle.t[g_uiLang]));
    RECT card = { kGfxCardL, kGfxCardT, kGfxCardR, kGfxCardB };
    FillRound(hdc, card, 14, g_pal.homeCard, COLOR_BORDER);
    SelectObject(hdc, g_hFontUI);
    for (int i = 0; i < GR_COUNT; ++i)
    {
        const wchar_t *l = kGfxRows[i].label.t[g_uiLang];
        TextOutW(hdc, GfxColX(i), GfxRowY(i), l, (int)wcslen(l));
    }
    SetTextColor(hdc, COLOR_TEXT_MUTED);
    RECT h = { kGfxCardL + 16, kGfxCardB - 72, kGfxCardR - 16, kGfxCardB - 8 };
    DrawTextW(hdc, kGfxHint.t[g_uiLang], -1, &h, DT_LEFT | DT_WORDBREAK);
}
