#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wctype.h>
#include <string.h>

enum {
    WM_TRAYICON = WM_USER + 1,
    IDM_TRAY_EXIT = 1001,
};

static HHOOK g_keyboard_hook = NULL;
static NOTIFYICONDATAW g_nid = {0};
static HMENU g_tray_menu = NULL;
static HICON g_app_icon_small = NULL;
static HICON g_app_icon_big = NULL;
static HANDLE g_single_instance_mutex = NULL;

// ---------- Wrong-layout autocorrect (EN/RU) ----------

#define TOKEN_MAX_CHARS 64

static wchar_t g_token[TOKEN_MAX_CHARS + 1];
static size_t g_token_len = 0;
static DWORD g_swallow_vk_keyup = 0;
static BOOL g_swallow_keyup = FALSE;

typedef struct {
    BOOL active;
    ULONGLONG ts_ms;
    wchar_t original[TOKEN_MAX_CHARS + 1];
    wchar_t corrected[TOKEN_MAX_CHARS + 1];
    size_t original_len;
    size_t corrected_len;
    wchar_t boundary;
    BOOL had_boundary;
    BOOL corrected_to_english; // TRUE if we mapped RU->EN
    BOOL corrected_applied;    // TRUE if current text is corrected+boundary
} LastFix;

static LastFix g_last_fix = {0};

static BOOL IsLatinLetter(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

static BOOL IsCyrillicLetter(wchar_t ch)
{
    return (ch >= 0x0400 && ch <= 0x04FF) || (ch >= 0x0500 && ch <= 0x052F);
}

static BOOL IsWordChar(wchar_t ch)
{
    // Word basis: only letters/digits. Hyphens/apostrophes end the token for simplicity.
    return iswalnum((wint_t)ch) != 0;
}

static wchar_t ToLowerInvariant(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z') return (wchar_t)(ch - L'A' + L'a');
    // Cyrillic case fold: towlower handles Unicode on Windows.
    return (wchar_t)towlower((wint_t)ch);
}

static int FindBigramScore(const wchar_t* token, const wchar_t* const* commonPairs, size_t commonCount)
{
    // Returns number of bigrams found in the small "common bigrams" list.
    const size_t n = wcslen(token);
    if (n < 2) return 0;

    int hits = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        wchar_t bg[3] = { token[i], token[i + 1], 0 };
        for (size_t j = 0; j < commonCount; j++) {
            if (bg[0] == commonPairs[j][0] && bg[1] == commonPairs[j][1]) {
                hits++;
                break;
            }
        }
    }
    return hits;
}

static int CountBadBigrams(const wchar_t* token, const wchar_t* const* badPairs, size_t badCount)
{
    const size_t n = wcslen(token);
    if (n < 2) return 0;
    int hits = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        wchar_t bg0 = token[i];
        wchar_t bg1 = token[i + 1];
        for (size_t j = 0; j < badCount; j++) {
            if (bg0 == badPairs[j][0] && bg1 == badPairs[j][1]) {
                hits++;
                break;
            }
        }
    }
    return hits;
}

static double VowelRatioEn(const wchar_t* token)
{
    const wchar_t* vowels = L"aeiouy";
    int v = 0, l = 0;
    for (const wchar_t* p = token; *p; p++) {
        if (!IsLatinLetter(*p)) continue;
        l++;
        if (wcschr(vowels, *p)) v++;
    }
    if (l == 0) return 0.0;
    return (double)v / (double)l;
}

static double VowelRatioRu(const wchar_t* token)
{
    const wchar_t* vowels = L"\u0430\u0435\u0451\u0438\u043e\u0443\u044b\u044d\u044e\u044f";
    int v = 0, l = 0;
    for (const wchar_t* p = token; *p; p++) {
        if (!IsCyrillicLetter(*p)) continue;
        l++;
        if (wcschr(vowels, *p)) v++;
    }
    if (l == 0) return 0.0;
    return (double)v / (double)l;
}

static int ScoreEnglish(const wchar_t* tokenLower)
{
    // Lightweight "not gibberish" score: common bigrams + vowel ratio sanity.
    static const wchar_t* const bigrams[] = {
        L"th", L"he", L"in", L"er", L"an", L"re", L"on", L"at", L"en", L"nd",
        L"ti", L"es", L"or", L"te", L"of", L"ed", L"is", L"it", L"al", L"ar",
        L"st", L"to", L"nt", L"ng", L"se", L"ha", L"as", L"ou", L"io", L"le",
        // Short-word helpers
        L"oo", L"ck", L"ok", L"bo", L"ee",
    };

    int latin = 0, nonLatinLetters = 0;
    for (const wchar_t* p = tokenLower; *p; p++) {
        if (IsLatinLetter(*p)) latin++;
        else if (iswalpha((wint_t)*p)) nonLatinLetters++;
    }
    if (latin == 0) return -1000;
    if (nonLatinLetters > 0) return -500;

    const int hits = FindBigramScore(tokenLower, bigrams, ARRAYSIZE(bigrams));
    const size_t n = wcslen(tokenLower);
    const double vr = VowelRatioEn(tokenLower);

    int score = 0;
    score += hits * 3;
    // Prefer some vowels but allow short words like "nth" to pass if bigrams look okay.
    if (n >= 4 && vr < 0.20) score -= 6;
    if (vr > 0.75) score -= 3;
    // Penalize long runs without vowels.
    if (n >= 6 && vr < 0.15) score -= 10;
    // Slight length bonus.
    score += (int)(n);
    return score;
}

static int ScoreRussian(const wchar_t* tokenLower)
{
    static const wchar_t* const bigrams[] = {
        L"\u0441\u0442", L"\u043d\u043e", L"\u0442\u043e", L"\u043d\u0430", L"\u0435\u043d", L"\u043e\u0432", L"\u043d\u0438", L"\u0440\u0430", L"\u0432\u043e", L"\u043a\u043e",
        L"\u043f\u0440", L"\u043f\u043e", L"\u0435\u0440", L"\u0440\u043e", L"\u043e\u0441", L"\u0430\u043b", L"\u0442\u0430", L"\u0432\u0430", L"\u043d\u0435", L"\u043b\u0438",
        L"\u0440\u0435",
    };
    static const wchar_t* const badBigrams[] = {
        L"\u0449\u0449", // щщ
        L"\u044a\u044a", // ъъ
        L"\u044b\u044b", // ыы
        L"\u0439\u0439", // йй
        L"\u044c\u044a", // ьъ
        L"\u044a\u044c", // ъь
        L"\u0436\u044b", // жы (should be жи)
        L"\u0448\u044b", // шы (should be ши)
    };

    int cyr = 0, nonCyrLetters = 0;
    for (const wchar_t* p = tokenLower; *p; p++) {
        if (IsCyrillicLetter(*p)) cyr++;
        else if (iswalpha((wint_t)*p)) nonCyrLetters++;
    }
    if (cyr == 0) return -1000;
    if (nonCyrLetters > 0) return -500;

    const int hits = FindBigramScore(tokenLower, bigrams, ARRAYSIZE(bigrams));
    const int badHits = CountBadBigrams(tokenLower, badBigrams, ARRAYSIZE(badBigrams));
    const size_t n = wcslen(tokenLower);
    const double vr = VowelRatioRu(tokenLower);

    int score = 0;
    score += hits * 3;
    score -= badHits * 8;
    if (n >= 4 && vr < 0.20) score -= 6;
    if (vr > 0.80) score -= 3;
    if (n >= 6 && vr < 0.15) score -= 10;
    score += (int)(n);
    return score;
}

typedef struct {
    wchar_t from;
    wchar_t to;
} CharMap;

// Physical-keyboard mapping for QWERTY <-> ЙЦУКЕН (lowercase).
static const CharMap kRuToEn[] = {
    {L'\u0439', L'q'},{L'\u0446', L'w'},{L'\u0443', L'e'},{L'\u043a', L'r'},{L'\u0435', L't'},{L'\u043d', L'y'},{L'\u0433', L'u'},{L'\u0448', L'i'},{L'\u0449', L'o'},{L'\u0437', L'p'},{L'\u0445', L'['},{L'\u044a', L']'},
    {L'\u0444', L'a'},{L'\u044b', L's'},{L'\u0432', L'd'},{L'\u0430', L'f'},{L'\u043f', L'g'},{L'\u0440', L'h'},{L'\u043e', L'j'},{L'\u043b', L'k'},{L'\u0434', L'l'},{L'\u0436', L';'},{L'\u044d', L'\''},
    {L'\u044f', L'z'},{L'\u0447', L'x'},{L'\u0441', L'c'},{L'\u043c', L'v'},{L'\u0438', L'b'},{L'\u0442', L'n'},{L'\u044c', L'm'},{L'\u0431', L','},{L'\u044e', L'.'},
    {L'\u0451', L'`'},
};

static const CharMap kEnToRu[] = {
    {L'q', L'\u0439'},{L'w', L'\u0446'},{L'e', L'\u0443'},{L'r', L'\u043a'},{L't', L'\u0435'},{L'y', L'\u043d'},{L'u', L'\u0433'},{L'i', L'\u0448'},{L'o', L'\u0449'},{L'p', L'\u0437'},{L'[', L'\u0445'},{L']', L'\u044a'},
    {L'a', L'\u0444'},{L's', L'\u044b'},{L'd', L'\u0432'},{L'f', L'\u0430'},{L'g', L'\u043f'},{L'h', L'\u0440'},{L'j', L'\u043e'},{L'k', L'\u043b'},{L'l', L'\u0434'},{L';', L'\u0436'},{L'\'', L'\u044d'},
    {L'z', L'\u044f'},{L'x', L'\u0447'},{L'c', L'\u0441'},{L'v', L'\u043c'},{L'b', L'\u0438'},{L'n', L'\u0442'},{L'm', L'\u044c'},{L',', L'\u0431'},{L'.', L'\u044e'},
    {L'`', L'\u0451'},
};

static wchar_t MapChar(const CharMap* map, size_t mapCount, wchar_t ch)
{
    for (size_t i = 0; i < mapCount; i++) {
        if (map[i].from == ch) return map[i].to;
    }
    return 0;
}

static void MapRuToEn(const wchar_t* in, wchar_t* out, size_t outCap)
{
    size_t n = 0;
    for (const wchar_t* p = in; *p && n + 1 < outCap; p++) {
        wchar_t ch = *p;
        const BOOL upper = (ch != ToLowerInvariant(ch));
        wchar_t lower = ToLowerInvariant(ch);
        wchar_t mapped = MapChar(kRuToEn, ARRAYSIZE(kRuToEn), lower);
        if (!mapped) mapped = lower;
        if (upper && mapped >= L'a' && mapped <= L'z') mapped = (wchar_t)(mapped - L'a' + L'A');
        out[n++] = mapped;
    }
    out[n] = 0;
}

static void MapEnToRu(const wchar_t* in, wchar_t* out, size_t outCap)
{
    size_t n = 0;
    for (const wchar_t* p = in; *p && n + 1 < outCap; p++) {
        wchar_t ch = *p;
        const BOOL upper = (ch >= L'A' && ch <= L'Z');
        wchar_t lower = (ch >= L'A' && ch <= L'Z') ? (wchar_t)(ch - L'A' + L'a') : ch;
        wchar_t mapped = MapChar(kEnToRu, ARRAYSIZE(kEnToRu), lower);
        if (!mapped) mapped = lower;
        if (upper) mapped = (wchar_t)towupper((wint_t)mapped);
        out[n++] = mapped;
    }
    out[n] = 0;
}

static HKL FindLayoutByPrimaryLang(WORD primaryLang)
{
    HKL layouts[32];
    const int n = GetKeyboardLayoutList((int)ARRAYSIZE(layouts), layouts);
    for (int i = 0; i < n; i++) {
        const LANGID lid = LOWORD((UINT_PTR)layouts[i]);
        if (PRIMARYLANGID(lid) == primaryLang) return layouts[i];
    }
    return NULL;
}

static HKL GetForegroundKeyboardLayout(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return GetKeyboardLayout(0);
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    if (!tid) return GetKeyboardLayout(0);
    return GetKeyboardLayout(tid);
}

static void RequestLayoutSwitch(HKL target)
{
    if (!target) return;
    HWND fg = GetForegroundWindow();
    if (fg) {
        PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)target);
    }
}

static void SendBackspacesAndText(size_t backspaces, const wchar_t* text)
{
    INPUT inputs[256];
    UINT count = 0;

    for (size_t i = 0; i < backspaces && count + 2 < ARRAYSIZE(inputs); i++) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_BACK;
        inputs[count].ki.wScan = 0;
        inputs[count].ki.dwFlags = 0;
        inputs[count].ki.time = 0;
        inputs[count].ki.dwExtraInfo = 0;
        count++;

        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_BACK;
        inputs[count].ki.wScan = 0;
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[count].ki.time = 0;
        inputs[count].ki.dwExtraInfo = 0;
        count++;
    }

    for (const wchar_t* p = text; *p && count + 2 < ARRAYSIZE(inputs); p++) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = 0;
        inputs[count].ki.wScan = *p;
        inputs[count].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[count].ki.time = 0;
        inputs[count].ki.dwExtraInfo = 0;
        count++;

        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = 0;
        inputs[count].ki.wScan = *p;
        inputs[count].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs[count].ki.time = 0;
        inputs[count].ki.dwExtraInfo = 0;
        count++;
    }

    if (count) {
        SendInput(count, inputs, sizeof(INPUT));
    }
}

static void InvalidateLastFix(void)
{
    g_last_fix.active = FALSE;
}

static BOOL ToggleLastFixIfPossible(void)
{
    if (!g_last_fix.active) return FALSE;

    const ULONGLONG now = GetTickCount64();
    if (now - g_last_fix.ts_ms > 30000) { // 30s window
        g_last_fix.active = FALSE;
        return FALSE;
    }
    if (!g_last_fix.had_boundary) {
        g_last_fix.active = FALSE;
        return FALSE;
    }

    const BOOL want_corrected = g_last_fix.corrected_applied ? FALSE : TRUE;

    const wchar_t* targetText = want_corrected ? g_last_fix.corrected : g_last_fix.original;
    const size_t targetLen = want_corrected ? g_last_fix.corrected_len : g_last_fix.original_len;
    const size_t currentLen = want_corrected ? g_last_fix.original_len : g_last_fix.corrected_len;

    // Switch layout to match the target.
    const BOOL targetIsEnglish = want_corrected ? g_last_fix.corrected_to_english : !g_last_fix.corrected_to_english;
    const HKL targetLayout = FindLayoutByPrimaryLang(targetIsEnglish ? LANG_ENGLISH : LANG_RUSSIAN);
    RequestLayoutSwitch(targetLayout);

    // Cursor is after: current + boundary. Replace with: target + boundary.
    wchar_t out[TOKEN_MAX_CHARS + 2];
    if (targetLen + 1 >= ARRAYSIZE(out)) {
        g_last_fix.active = FALSE;
        return FALSE;
    }
    memcpy(out, targetText, (targetLen + 1) * sizeof(wchar_t));
    out[targetLen] = g_last_fix.boundary;
    out[targetLen + 1] = 0;

    SendBackspacesAndText(currentLen + 1, out);

    g_last_fix.corrected_applied = want_corrected ? TRUE : FALSE;
    g_last_fix.ts_ms = now; // extend window while toggling
    return TRUE;
}

static BOOL TryAutocorrectToken(const wchar_t* token, wchar_t boundaryChar, BOOL includeBoundary)
{
    const size_t n = wcslen(token);
    if (n < 3) return FALSE;
    if (n > TOKEN_MAX_CHARS) return FALSE;

    wchar_t lower[TOKEN_MAX_CHARS + 1];
    for (size_t i = 0; i < n; i++) lower[i] = ToLowerInvariant(token[i]);
    lower[n] = 0;

    int latin = 0, cyr = 0, otherLetters = 0;
    for (size_t i = 0; i < n; i++) {
        const wchar_t ch = lower[i];
        if (IsLatinLetter(ch)) latin++;
        else if (IsCyrillicLetter(ch)) cyr++;
        else if (iswalpha((wint_t)ch)) otherLetters++;
    }
    if (otherLetters > 0) return FALSE;

    const BOOL mixedScripts = (latin > 0 && cyr > 0);
    // Avoid "fixing" likely IDs like "C3PO", "R2D2", etc.
    // If it contains digits, be conservative.
    int digits = 0;
    for (size_t i = 0; i < n; i++) if (iswdigit((wint_t)lower[i])) digits++;
    if (digits > 0) return FALSE;

    const int scoreEn = ScoreEnglish(lower);
    const int scoreRu = ScoreRussian(lower);

    wchar_t mapped[TOKEN_MAX_CHARS + 1];
    int mappedScore = -1000;
    BOOL toEnglish = FALSE;

    if (cyr > 0) {
        MapRuToEn(token, mapped, ARRAYSIZE(mapped));
        wchar_t mappedLower[TOKEN_MAX_CHARS + 1];
        size_t ml = wcslen(mapped);
        for (size_t i = 0; i < ml; i++) mappedLower[i] = ToLowerInvariant(mapped[i]);
        mappedLower[ml] = 0;
        mappedScore = ScoreEnglish(mappedLower);
        toEnglish = TRUE;
    } else if (latin > 0) {
        MapEnToRu(token, mapped, ARRAYSIZE(mapped));
        wchar_t mappedLower[TOKEN_MAX_CHARS + 1];
        size_t ml = wcslen(mapped);
        for (size_t i = 0; i < ml; i++) mappedLower[i] = ToLowerInvariant(mapped[i]);
        mappedLower[ml] = 0;
        mappedScore = ScoreRussian(mappedLower);
        toEnglish = FALSE;
    } else {
        return FALSE;
    }

    // Decision thresholds: dynamic based on length; tuned to fix cases like "руддщ" -> "hello".
    const int base = (cyr > 0) ? scoreRu : scoreEn;
    const int diff = mappedScore - base;

    int minMapped = (n <= 4) ? 6 : 8;
    int minDiff = (n <= 5) ? 4 : 6;
    if (base <= 6) minDiff = 3;
    if (mixedScripts) minDiff = 2;

    if (mappedScore >= minMapped && diff >= minDiff) {
        wchar_t dbg[256];
        StringCchPrintfW(dbg, ARRAYSIZE(dbg),
                         L"[DiSwitcher] autocorrect '%s' -> '%s' base=%d mapped=%d diff=%d\r\n",
                         token, mapped, base, mappedScore, diff);
        OutputDebugStringW(dbg);

        // Save last fix for Pause-to-revert.
        ZeroMemory(&g_last_fix, sizeof(g_last_fix));
        g_last_fix.active = TRUE;
        g_last_fix.ts_ms = GetTickCount64();
        StringCchCopyW(g_last_fix.original, ARRAYSIZE(g_last_fix.original), token);
        StringCchCopyW(g_last_fix.corrected, ARRAYSIZE(g_last_fix.corrected), mapped);
        g_last_fix.original_len = n;
        g_last_fix.corrected_len = wcslen(mapped);
        g_last_fix.boundary = boundaryChar;
        g_last_fix.had_boundary = includeBoundary ? TRUE : FALSE;
        g_last_fix.corrected_to_english = toEnglish ? TRUE : FALSE;
        g_last_fix.corrected_applied = TRUE;

        const HKL target = FindLayoutByPrimaryLang(toEnglish ? LANG_ENGLISH : LANG_RUSSIAN);
        RequestLayoutSwitch(target);
        if (includeBoundary) {
            wchar_t withBoundary[TOKEN_MAX_CHARS + 2];
            size_t ml = wcslen(mapped);
            if (ml + 1 < ARRAYSIZE(withBoundary)) {
                memcpy(withBoundary, mapped, (ml + 1) * sizeof(wchar_t));
                withBoundary[ml] = boundaryChar;
                withBoundary[ml + 1] = 0;
                SendBackspacesAndText(n, withBoundary);
            } else {
                SendBackspacesAndText(n, mapped);
            }
        } else {
            SendBackspacesAndText(n, mapped);
        }
        return TRUE;
    }
    return FALSE;
}

static void DebugPrintVkEvent(const wchar_t* prefix, DWORD vkCode, DWORD scanCode, DWORD flags)
{
    wchar_t buf[256];
    StringCchPrintfW(buf, 256, L"[DiSwitcher] %s vk=0x%02X sc=0x%02X flags=0x%08lX\r\n",
                     prefix, (unsigned)vkCode, (unsigned)scanCode, (unsigned long)flags);
    OutputDebugStringW(buf);
}

static HICON CreateTrayIconS(int sizePx)
{
    if (sizePx < 16) sizePx = 16;
    if (sizePx > 128) sizePx = 128;

    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sizePx;
    bi.bV5Height = -sizePx; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = NULL;
    HDC hdc = GetDC(NULL);
    if (!hdc) return NULL;

    HBITMAP colorBmp = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!colorBmp || !bits) {
        if (colorBmp) DeleteObject(colorBmp);
        return NULL;
    }

    HDC memdc = CreateCompatibleDC(NULL);
    if (!memdc) {
        DeleteObject(colorBmp);
        return NULL;
    }

    HGDIOBJ old = SelectObject(memdc, colorBmp);

    // Clear fully transparent.
    const size_t pixelCount = (size_t)sizePx * (size_t)sizePx;
    ZeroMemory(bits, pixelCount * 4);

    // Draw: red filled circle + white "S"
    const COLORREF red = RGB(220, 0, 0);
    const COLORREF white = RGB(255, 255, 255);
    const int penW = (sizePx / 10) > 2 ? (sizePx / 10) : 2;
    HPEN pen = CreatePen(PS_SOLID, penW, red);
    HGDIOBJ oldPen = SelectObject(memdc, pen);
    HBRUSH brush = CreateSolidBrush(red);
    HGDIOBJ oldBrush = SelectObject(memdc, brush);

    const int pad = (sizePx / 12) > 2 ? (sizePx / 12) : 2;
    Ellipse(memdc, pad, pad, sizePx - pad, sizePx - pad);

    SetBkMode(memdc, TRANSPARENT);
    SetTextColor(memdc, white);

    const int fontSize = (int)(sizePx * 0.70);
    HFONT font = CreateFontW(
        -fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = NULL;
    if (font) oldFont = SelectObject(memdc, font);

    RECT rc = {0, 0, sizePx, sizePx};
    DrawTextW(memdc, L"S", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (oldFont) SelectObject(memdc, oldFont);
    if (font) DeleteObject(font);

    SelectObject(memdc, oldBrush);
    SelectObject(memdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    // Ensure alpha is set for any drawn pixel (GDI often leaves alpha at 0).
    DWORD* px = (DWORD*)bits; // BGRA (little-endian DWORD: 0xAARRGGBB with our masks)
    for (size_t i = 0; i < pixelCount; i++) {
        if ((px[i] & 0x00FFFFFFu) != 0) {
            px[i] |= 0xFF000000u;
        }
    }

    SelectObject(memdc, old);
    DeleteDC(memdc);

    // Build 1bpp mask: 1 = transparent.
    const size_t strideBytes = (((sizePx + 31) / 32) * 4);
    const size_t maskBytes = strideBytes * (size_t)sizePx;
    BYTE* mask = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, maskBytes);
    if (!mask) {
        DeleteObject(colorBmp);
        return NULL;
    }

    for (int y = 0; y < sizePx; y++) {
        for (int x = 0; x < sizePx; x++) {
            const DWORD c = px[(size_t)y * (size_t)sizePx + (size_t)x];
            const BOOL transparent = ((c & 0xFF000000u) == 0);
            if (transparent) {
                const size_t byteIndex = (size_t)y * strideBytes + (size_t)(x / 8);
                const BYTE bit = (BYTE)(0x80 >> (x % 8));
                mask[byteIndex] |= bit;
            }
        }
    }

    HBITMAP maskBmp = CreateBitmap(sizePx, sizePx, 1, 1, mask);
    HeapFree(GetProcessHeap(), 0, mask);
    if (!maskBmp) {
        DeleteObject(colorBmp);
        return NULL;
    }

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;

    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(maskBmp);
    DeleteObject(colorBmp);
    return icon;
}

static void AllowExplorerMessages(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    typedef BOOL(WINAPI * ChangeWindowMessageFilterExFn)(HWND, UINT, DWORD, PCHANGEFILTERSTRUCT);
    const ChangeWindowMessageFilterExFn fn =
        (ChangeWindowMessageFilterExFn)GetProcAddress(user32, "ChangeWindowMessageFilterEx");
    if (!fn) return;

    CHANGEFILTERSTRUCT cfs;
    ZeroMemory(&cfs, sizeof(cfs));
    cfs.cbSize = sizeof(cfs);

    // Allow tray callback even if we're elevated (Explorer is medium integrity).
    // MSGFLT_ALLOW = 1
    fn(hwnd, WM_TRAYICON, 1, &cfs);
}

static void ShowWin32ErrorBox(HWND hwnd, const wchar_t* context)
{
    const DWORD err = GetLastError();
    wchar_t sysMsg[512] = {0};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, sysMsg, (DWORD)ARRAYSIZE(sysMsg), NULL);

    wchar_t buf[768];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s\n\nWin32 error %lu: %s", context, (unsigned long)err, sysMsg);
    MessageBoxW(hwnd, buf, L"DiSwitcher", MB_ICONERROR | MB_OK);
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        if (k->flags & LLKHF_INJECTED) {
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        if (g_swallow_keyup && (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) && k->vkCode == g_swallow_vk_keyup) {
            g_swallow_keyup = FALSE;
            g_swallow_vk_keyup = 0;
            return 1;
        }

        switch (wParam) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            DebugPrintVkEvent(L"down", k->vkCode, k->scanCode, k->flags);
            // Emergency exit hotkey: Ctrl+Alt+Shift+Q
            if (k->vkCode == 'Q') {
                const SHORT ctrl = GetAsyncKeyState(VK_CONTROL);
                const SHORT alt = GetAsyncKeyState(VK_MENU);
                const SHORT shift = GetAsyncKeyState(VK_SHIFT);
                if ((ctrl & 0x8000) && (alt & 0x8000) && (shift & 0x8000)) {
                    PostQuitMessage(0);
                    return 1;
                }
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            DebugPrintVkEvent(L"up", k->vkCode, k->scanCode, k->flags);
            break;
        default:
            break;
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // Global hotkey: Pause to revert the last auto-correction (within a short window).
            if (k->vkCode == VK_PAUSE) {
                if (ToggleLastFixIfPossible()) return 1;
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            // Ignore shortcuts/modifiers.
            const BOOL ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const BOOL alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if (ctrl || alt) {
                InvalidateLastFix();
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            if (k->vkCode == VK_BACK) {
                InvalidateLastFix();
                if (g_token_len > 0) {
                    g_token_len--;
                    g_token[g_token_len] = 0;
                }
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            if (k->vkCode == VK_ESCAPE) {
                InvalidateLastFix();
                g_token_len = 0;
                g_token[0] = 0;
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            // Convert this keystroke to the actual character produced in the foreground layout.
            HKL hkl = GetForegroundKeyboardLayout();
            BYTE ks[256];
            ZeroMemory(ks, sizeof(ks));
            GetKeyboardState(ks);

            wchar_t out[8];
            const UINT vk = (UINT)k->vkCode;
            const UINT sc = (UINT)k->scanCode;
            int rc = ToUnicodeEx(vk, sc, ks, out, (int)ARRAYSIZE(out), 0, hkl);
            if (rc == 1) {
                const wchar_t ch = out[0];
                if (IsWordChar(ch)) {
                    InvalidateLastFix();
                    if (g_token_len < TOKEN_MAX_CHARS) {
                        g_token[g_token_len++] = ch;
                        g_token[g_token_len] = 0;
                    }
                } else {
                    if (g_token_len >= 3) {
                        // If we correct on a printable boundary, swallow the boundary keystroke
                        // and re-inject it after correction to keep order stable.
                        if (TryAutocorrectToken(g_token, ch, TRUE)) {
                            g_token_len = 0;
                            g_token[0] = 0;
                            g_swallow_vk_keyup = k->vkCode;
                            g_swallow_keyup = TRUE;
                            return 1;
                        }
                    }
                    InvalidateLastFix();
                    g_token_len = 0;
                    g_token[0] = 0;
                }
            } else {
                // Non-text key ends current token.
                if (g_token_len >= 3) {
                    (void)TryAutocorrectToken(g_token, 0, FALSE);
                }
                InvalidateLastFix();
                g_token_len = 0;
                g_token[0] = 0;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static BOOL InstallKeyboardHook(void)
{
    if (g_keyboard_hook) return TRUE;
    g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(NULL), 0);
    if (!g_keyboard_hook) {
        OutputDebugStringW(L"[DiSwitcher] Failed to install keyboard hook.\r\n");
        return FALSE;
    }
    return TRUE;
}

static void UninstallKeyboardHook(void)
{
    if (!g_keyboard_hook) return;
    UnhookWindowsHookEx(g_keyboard_hook);
    g_keyboard_hook = NULL;
}

static void TrayRemove(void)
{
    if (g_nid.cbSize) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        ZeroMemory(&g_nid, sizeof(g_nid));
    }
}

static BOOL TrayAdd(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    // Prefer full size on modern Windows; fallback to V2 if needed.
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    if (!g_app_icon_small) g_app_icon_small = CreateTrayIconS(32);
    g_nid.hIcon = g_app_icon_small ? g_app_icon_small : LoadIconW(NULL, IDI_APPLICATION);
    StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"DiSwitcher (PID %lu)", GetCurrentProcessId());

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) {
            g_nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
            if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
                OutputDebugStringW(L"[DiSwitcher] Failed to add tray icon (full+v2).\r\n");
                ZeroMemory(&g_nid, sizeof(g_nid));
                return FALSE;
            }
        } else {
            OutputDebugStringW(L"[DiSwitcher] Failed to add tray icon.\r\n");
            ZeroMemory(&g_nid, sizeof(g_nid));
            return FALSE;
        }
    }

    // Version 4 improves behavior on modern Windows; ignore failures.
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    return TRUE;
}

static void ForceForegroundForMenu(HWND hwnd)
{
    const HWND fg = GetForegroundWindow();
    DWORD fgTid = 0;
    if (fg) fgTid = GetWindowThreadProcessId(fg, NULL);
    const DWORD curTid = GetCurrentThreadId();

    if (fgTid && fgTid != curTid) {
        AttachThreadInput(curTid, fgTid, TRUE);
        SetForegroundWindow(hwnd);
        AttachThreadInput(curTid, fgTid, FALSE);
    } else {
        SetForegroundWindow(hwnd);
    }
}

static void TrayShowMenu(HWND hwnd)
{
    if (!g_tray_menu) return;

    POINT pt;
    GetCursorPos(&pt);
    ForceForegroundForMenu(hwnd);
    UINT cmd = TrackPopupMenu(
        g_tray_menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RETURNCMD,
        pt.x, pt.y, 0, hwnd, NULL);
    if (cmd != 0) {
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
    SendMessageW(hwnd, WM_NULL, 0, 0);
}

static void Cleanup(HWND hwnd)
{
    (void)hwnd;
    UninstallKeyboardHook();
    TrayRemove();
    if (g_tray_menu) {
        DestroyMenu(g_tray_menu);
        g_tray_menu = NULL;
    }
    if (g_app_icon_small) {
        DestroyIcon(g_app_icon_small);
        g_app_icon_small = NULL;
    }
    if (g_app_icon_big) {
        DestroyIcon(g_app_icon_big);
        g_app_icon_big = NULL;
    }
    if (g_single_instance_mutex) {
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = NULL;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_tray_menu = CreatePopupMenu();
        if (g_tray_menu) {
            AppendMenuW(g_tray_menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
        }
        AllowExplorerMessages(hwnd);
        if (!TrayAdd(hwnd)) {
            ShowWin32ErrorBox(hwnd, L"Failed to create tray icon.");
            PostQuitMessage(1);
            return -1;
        }
        if (!InstallKeyboardHook()) {
            ShowWin32ErrorBox(hwnd, L"Failed to install keyboard hook.");
        }
        return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_TRAYICON: {
        wchar_t buf[128];
        StringCchPrintfW(buf, 128, L"[DiSwitcher] tray wParam=%llu lParam=0x%llX\r\n",
                         (unsigned long long)wParam, (unsigned long long)lParam);
        OutputDebugStringW(buf);

        // Some shells pack additional data into the high word; only the low word is the mouse msg.
        const UINT uMsg = (UINT)LOWORD(lParam);
        switch (uMsg) {
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            MessageBeep(MB_OK);
            TrayShowMenu(hwnd);
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_DESTROY:
        Cleanup(hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Single-instance guard.
    g_single_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\DiSwitcher_SingleInstance_9B2C9A8A_05D2_4E37_BF3F_0CF6DF6C4F5C");
    if (!g_single_instance_mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = NULL;
        return 0;
    }

    const wchar_t* kClassName = L"DiSwitcherHiddenWindow";

    if (!g_app_icon_small) g_app_icon_small = CreateTrayIconS(16);
    if (!g_app_icon_big) g_app_icon_big = CreateTrayIconS(32);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_app_icon_big;
    wc.hIconSm = g_app_icon_small;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"RegisterClassEx failed.", L"DiSwitcher", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kClassName,
        L"DiSwitcher",
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        MessageBoxW(NULL, L"CreateWindowEx failed.", L"DiSwitcher", MB_ICONERROR);
        return 1;
    }

    // Keep it invisible, but as a top-level tool window (not message-only) so it can own menus.
    ShowWindow(hwnd, SW_HIDE);

    if (g_app_icon_small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_app_icon_small);
    if (g_app_icon_big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_app_icon_big);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
