#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <wctype.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <commdlg.h>
#include <math.h>
#include "sqlite3.h"
#include "local_model.h"

enum {
    WM_TRAYICON = WM_USER + 1,
    IDM_TRAY_EXIT = 1001,
    IDM_TRAY_EXPORT_DB = 1002,
    IDM_TRAY_EXPORT_MODEL_LOG = 1003,
    IDM_TRAY_DIAGNOSTICS = 1004,
    IDM_TRAY_OPEN_DATA = 1005,
    IDM_TRAY_ENABLED = 1006,
    IDM_TRAY_STATS = 1007,
    WM_LOG_PAUSE = WM_APP + 2,
    WM_LOG_DECISION = WM_APP + 3,
    IDT_TRAY_RETRY = 1,
};

static HHOOK g_keyboard_hook = NULL;
static HHOOK g_mouse_hook = NULL;
static HWINEVENTHOOK g_focus_hook = NULL;
static HWINEVENTHOOK g_foreground_hook = NULL;
static HWND g_target_window = NULL, g_target_focus = NULL;
static BOOL g_enabled = TRUE, g_diagnostics = FALSE, g_token_overflow = FALSE;
static BOOL g_skip_until_boundary = FALSE;
static DSModel g_language_model = {0};
static ULONGLONG g_session_candidates=0, g_session_corrections=0;
static wchar_t g_exclusions[64][MAX_PATH];
static int g_exclusion_count = 0;
static HWND g_exclusion_window = NULL;
static BOOL g_excluded_window = FALSE;
static NOTIFYICONDATAW g_nid = {0};
static UINT g_taskbar_created = 0;
static HMENU g_tray_menu = NULL;
static HICON g_app_icon_small = NULL;
static HICON g_app_icon_big = NULL;
static HANDLE g_single_instance_mutex = NULL;
static HWND g_main_hwnd = NULL;

static wchar_t g_db_path[MAX_PATH] = {0};
static wchar_t g_data_dir[MAX_PATH] = {0};
static wchar_t g_model_path[MAX_PATH] = {0};
static wchar_t g_ml_samples_path[MAX_PATH] = {0};

// ---------- Wrong-layout autocorrect (EN/RU) ----------

#define TOKEN_MAX_CHARS 64
#define MIN_TOKEN_LEN 3

static wchar_t g_token[TOKEN_MAX_CHARS + 1];
static size_t g_token_len = 0;
static DWORD g_swallow_vk_keyup = 0;
static BOOL g_swallow_keyup = FALSE;

static wchar_t g_last_word[TOKEN_MAX_CHARS + 1];
static size_t g_last_word_len = 0;
static wchar_t g_last_word_boundary = 0;
static BOOL g_last_word_had_boundary = FALSE;
static ULONGLONG g_last_word_ts_ms = 0;

typedef struct {
    wchar_t action[32];
    wchar_t original[TOKEN_MAX_CHARS + 1];
    wchar_t corrected[TOKEN_MAX_CHARS + 1];
    wchar_t window_title[256];
    wchar_t process_path[MAX_PATH];
    wchar_t layout_hex[32];
    wchar_t ts_utc[40];
} PauseLogItem;

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

typedef struct {
    wchar_t original[65], target[65], reason[40], timestamp[40];
    DSDecision decision;
    uint32_t model_id;
} DecisionLog;

static void ResetTypingState(void)
{
    g_token_len=0; g_token[0]=0; g_token_overflow=FALSE;
    g_skip_until_boundary=FALSE;
    g_last_word_len=0; g_last_word[0]=0; g_last_fix.active=FALSE;
}

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
    return IsLatinLetter(ch) || IsCyrillicLetter(ch) || (ch>=L'0' && ch<=L'9');
}

static BOOL EndsWith(const wchar_t* s, const wchar_t* suffix)
{
    const size_t ls = wcslen(s);
    const size_t lsf = wcslen(suffix);
    if (lsf > ls) return FALSE;
    return wcsncmp(s + (ls - lsf), suffix, lsf) == 0;
}

static void GetAppDir(wchar_t* outDir, size_t outCap)
{
    if (!outDir || outCap == 0) return;
    outDir[0] = 0;

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return;

    for (DWORD i = n; i > 0; i--) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i - 1] = 0;
            break;
        }
    }
    StringCchCopyW(outDir, outCap, path);
}

static BOOL FileExistsW_(const wchar_t* path)
{
    if (!path || !path[0]) return FALSE;
    const DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return FALSE;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static BOOL DirExistsW_(const wchar_t* path)
{
    if (!path || !path[0]) return FALSE;
    const DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return FALSE;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static BOOL EnsureDirW_(const wchar_t* path)
{
    if (!path || !path[0]) return FALSE;
    if (DirExistsW_(path)) return TRUE;
    if (CreateDirectoryW(path, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static void BuildDataDir(void)
{
    g_data_dir[0] = 0;
    wchar_t localApp[MAX_PATH];
    localApp[0] = 0;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, localApp)) &&
        localApp[0] != 0) {
        wchar_t dataDir[MAX_PATH];
        dataDir[0] = 0;
        if (SUCCEEDED(StringCchPrintfW(dataDir, ARRAYSIZE(dataDir), L"%s\\DiSwitcher", localApp)) &&
            EnsureDirW_(dataDir)) {
            StringCchCopyW(g_data_dir, ARRAYSIZE(g_data_dir), dataDir);
        }
    }
}

static void BuildDbPath(void)
{
    g_db_path[0] = 0;

    // Legacy location: next to the EXE (may be non-writable, e.g. Program Files).
    wchar_t appDir[MAX_PATH];
    appDir[0] = 0;
    GetAppDir(appDir, ARRAYSIZE(appDir));
    wchar_t legacyPath[MAX_PATH];
    legacyPath[0] = 0;
    if (appDir[0] != 0) {
        (void)StringCchPrintfW(legacyPath, ARRAYSIZE(legacyPath), L"%s\\diswitcher.sqlite3", appDir);
    }

    // Preferred location: per-user LocalAppData\DiSwitcher\diswitcher.sqlite3
    wchar_t localApp[MAX_PATH];
    localApp[0] = 0;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, localApp)) &&
        localApp[0] != 0) {
        wchar_t dataDir[MAX_PATH];
        dataDir[0] = 0;
        if (SUCCEEDED(StringCchPrintfW(dataDir, ARRAYSIZE(dataDir), L"%s\\DiSwitcher", localApp)) &&
            EnsureDirW_(dataDir)) {
            StringCchCopyW(g_data_dir, ARRAYSIZE(g_data_dir), dataDir);
            wchar_t newPath[MAX_PATH];
            newPath[0] = 0;
            if (SUCCEEDED(StringCchPrintfW(newPath, ARRAYSIZE(newPath), L"%s\\diswitcher.sqlite3", dataDir))) {
                // Best-effort migration: if legacy DB exists and new DB doesn't, copy it over.
                if (!FileExistsW_(newPath) && FileExistsW_(legacyPath)) {
                    (void)CopyFileW(legacyPath, newPath, TRUE);
                }
                StringCchCopyW(g_db_path, ARRAYSIZE(g_db_path), newPath);
                return;
            }
        }
    }

    // Fallback: legacy location.
    if (legacyPath[0] != 0) {
        StringCchCopyW(g_db_path, ARRAYSIZE(g_db_path), legacyPath);
    }
}

static void BuildMlPaths(void)
{
    g_model_path[0] = 0;
    g_ml_samples_path[0] = 0;
    if (g_data_dir[0] == 0) BuildDataDir();
    if (g_data_dir[0] == 0) return;
    (void)StringCchPrintfW(g_model_path, ARRAYSIZE(g_model_path), L"%s\\litert_model.bin", g_data_dir);
    (void)StringCchPrintfW(g_ml_samples_path, ARRAYSIZE(g_ml_samples_path), L"%s\\ml_samples.jsonl", g_data_dir);
}

static void UtcNowIso(wchar_t* out, size_t cap)
{
    if (!out || cap == 0) return;
    SYSTEMTIME st;
    GetSystemTime(&st);
    StringCchPrintfW(out, cap, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// ---------- LiteRT (tiny online model) ----------

enum {
    LITERT_FEATURES = 12,
};

typedef struct {
    float w[LITERT_FEATURES];
    float b;
    float lr;
    float l2;
    float threshold;
    unsigned int steps; // number of online training updates applied
} LiteRTModel;

static LiteRTModel g_model = {0};
static CRITICAL_SECTION g_model_cs;
static BOOL g_model_cs_init = FALSE;
static HANDLE g_ml_event = NULL;
static HANDLE g_ml_thread = NULL;
static volatile LONG g_ml_stop = 0;

typedef struct {
    wchar_t ts_utc[40];
    wchar_t original[TOKEN_MAX_CHARS + 1];
    wchar_t corrected[TOKEN_MAX_CHARS + 1];
    BOOL toEnglish;
    int label; // 1 = should correct, 0 = should not
    float x[LITERT_FEATURES];
} LiteRTSample;

static LiteRTSample g_ml_queue[64];
static LONG g_ml_q_head = 0;
static LONG g_ml_q_tail = 0;
static CRITICAL_SECTION g_ml_q_cs;
static BOOL g_ml_q_cs_init = FALSE;
static LONG g_ml_q_dropped = 0;

static float Sigmoidf(float z)
{
    if (z >= 20.0f) return 1.0f;
    if (z <= -20.0f) return 0.0f;
    return 1.0f / (1.0f + expf(-z));
}

static void LiteRT_InitDefaults(LiteRTModel* m)
{
    if (!m) return;
    ZeroMemory(m->w, sizeof(m->w));
    m->lr = 0.03f;
    m->l2 = 0.0005f;
    m->threshold = 0.55f;
    m->steps = 0;

    // Seed weights to behave similar to the current heuristic: prefer high mapped score and high diff,
    // and penalize "good looking" base tokens.
    m->b = -3.0f;
    m->w[0] = 0.5f;   // diff
    m->w[1] = 0.2f;   // mappedScore
    m->w[2] = -0.15f; // baseScore
    m->w[3] = 0.6f;   // badHitsBase
    m->w[4] = 0.25f;  // len
}

static BOOL LiteRT_Load(LiteRTModel* m, const wchar_t* path)
{
    if (!m || !path || !path[0]) return FALSE;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    DWORD read = 0;
    unsigned int magic = 0;
    unsigned int ver = 0;
    unsigned int n = 0;
    if (!ReadFile(h, &magic, (DWORD)sizeof(magic), &read, NULL) || read != sizeof(magic) || magic != 0x54524C4Au) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, &ver, (DWORD)sizeof(ver), &read, NULL) || read != sizeof(ver) || (ver != 1 && ver != 2)) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, &n, (DWORD)sizeof(n), &read, NULL) || read != sizeof(n) || n != LITERT_FEATURES) {
        CloseHandle(h);
        return FALSE;
    }

    LiteRTModel tmp = {0};
    LiteRT_InitDefaults(&tmp);

    if (!ReadFile(h, tmp.w, (DWORD)(sizeof(float) * LITERT_FEATURES), &read, NULL) || read != sizeof(float) * LITERT_FEATURES) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, &tmp.b, (DWORD)sizeof(float), &read, NULL) || read != sizeof(float)) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, &tmp.lr, (DWORD)sizeof(float), &read, NULL) || read != sizeof(float)) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, &tmp.l2, (DWORD)sizeof(float), &read, NULL) || read != sizeof(float)) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, &tmp.threshold, (DWORD)sizeof(float), &read, NULL) || read != sizeof(float)) {
        CloseHandle(h);
        return FALSE;
    }

    if (ver == 2) {
        if (!ReadFile(h, &tmp.steps, (DWORD)sizeof(unsigned int), &read, NULL) || read != sizeof(unsigned int)) {
            CloseHandle(h);
            return FALSE;
        }
    } else {
        tmp.steps = 0;
    }

    CloseHandle(h);

    // Copy learned parameters only (no synchronization primitives).
    memcpy(m->w, tmp.w, sizeof(tmp.w));
    m->b = tmp.b;
    m->lr = tmp.lr;
    m->l2 = tmp.l2;
    m->threshold = tmp.threshold;
    m->steps = tmp.steps;
    return TRUE;
}

static void LiteRT_SaveLocked(const LiteRTModel* m, const wchar_t* path)
{
    if (!m || !path || !path[0]) return;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    const unsigned int magic = 0x54524C4Au; // 'J' 'L' 'R' 'T'
    const unsigned int ver = 2;
    const unsigned int n = LITERT_FEATURES;
    DWORD wrote = 0;
    (void)WriteFile(h, &magic, (DWORD)sizeof(magic), &wrote, NULL);
    (void)WriteFile(h, &ver, (DWORD)sizeof(ver), &wrote, NULL);
    (void)WriteFile(h, &n, (DWORD)sizeof(n), &wrote, NULL);
    (void)WriteFile(h, m->w, (DWORD)(sizeof(float) * LITERT_FEATURES), &wrote, NULL);
    (void)WriteFile(h, &m->b, (DWORD)sizeof(float), &wrote, NULL);
    (void)WriteFile(h, &m->lr, (DWORD)sizeof(float), &wrote, NULL);
    (void)WriteFile(h, &m->l2, (DWORD)sizeof(float), &wrote, NULL);
    (void)WriteFile(h, &m->threshold, (DWORD)sizeof(float), &wrote, NULL);
    (void)WriteFile(h, &m->steps, (DWORD)sizeof(unsigned int), &wrote, NULL);
    CloseHandle(h);
}

static float LiteRT_PredictLocked(const LiteRTModel* m, const float* x)
{
    float z = m->b;
    for (int i = 0; i < LITERT_FEATURES; i++) z += m->w[i] * x[i];
    return Sigmoidf(z);
}

static void LiteRT_TrainLocked(LiteRTModel* m, const float* x, int label)
{
    if (!m || !x) return;
    const float y = (label != 0) ? 1.0f : 0.0f;
    const float p = LiteRT_PredictLocked(m, x);
    const float err = (y - p);

    // SGD step for logistic regression with L2.
    for (int i = 0; i < LITERT_FEATURES; i++) {
        m->w[i] += m->lr * (err * x[i] - m->l2 * m->w[i]);
        if (m->w[i] > 10.0f) m->w[i] = 10.0f;
        if (m->w[i] < -10.0f) m->w[i] = -10.0f;
    }
    m->b += m->lr * err;
    if (m->b > 10.0f) m->b = 10.0f;
    if (m->b < -10.0f) m->b = -10.0f;

    if (m->steps < 0xFFFFFFFFu) m->steps++;
}

static BOOL LiteRT_ShouldAutocorrect(const float* x, float* outProb)
{
    if (!x) return FALSE;
    if (!g_model_cs_init) return FALSE;
    EnterCriticalSection(&g_model_cs);
    const float p = LiteRT_PredictLocked(&g_model, x);
    const unsigned int steps = g_model.steps;
    const float threshold = g_model.threshold;
    LeaveCriticalSection(&g_model_cs);
    if (outProb) *outProb = p;

    // Bootstrap: before we have enough online feedback, behave like the (already strict) heuristic gate
    // and don't let an untrained model block all corrections.
    if (steps < 10) return TRUE;

    return p >= threshold;
}

static void GetForegroundInfo(wchar_t* outTitle, size_t titleCap, wchar_t* outProc, size_t procCap, wchar_t* outLayoutHex, size_t layoutCap)
{
    if (outTitle && titleCap) outTitle[0] = 0;
    if (outProc && procCap) outProc[0] = 0;
    if (outLayoutHex && layoutCap) outLayoutHex[0] = 0;

    HWND fg = GetForegroundWindow();
    if (!fg) return;

    if (outTitle && titleCap) GetWindowTextW(fg, outTitle, (int)titleCap);

    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(fg, &pid);
    HKL hkl = tid ? GetKeyboardLayout(tid) : GetKeyboardLayout(0);
    if (outLayoutHex && layoutCap) {
        StringCchPrintfW(outLayoutHex, layoutCap, L"0x%p", hkl);
    }

    if (outProc && procCap && pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            DWORD sz = (DWORD)procCap;
            QueryFullProcessImageNameW(h, 0, outProc, &sz);
            CloseHandle(h);
        }
    }
}

static void DbInitIfNeeded(void)
{
    if (g_db_path[0] == 0) return;
    sqlite3* db = NULL;
    if (sqlite3_open16(g_db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    (void)sqlite3_exec(
        db,
        "CREATE TABLE IF NOT EXISTS pause_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts_utc TEXT NOT NULL,"
        "action TEXT NOT NULL,"
        "original TEXT,"
        "corrected TEXT,"
        "layout TEXT"
        ");",
        NULL, NULL, NULL);

    (void)sqlite3_exec(
        db,
        "CREATE TABLE IF NOT EXISTS model_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts_utc TEXT NOT NULL,"
        "event TEXT NOT NULL,"
        "label INTEGER,"
        "to_english INTEGER,"
        "p_before REAL,"
        "p_after REAL,"
        "delta_l1 REAL,"
        "lr REAL,"
        "l2 REAL,"
        "threshold REAL,"
        "original TEXT,"
        "corrected TEXT"
        ");",
        NULL, NULL, NULL);

    // If older builds created extra columns, migrate to the current schema.
    {
        BOOL hasWindowTitle = FALSE;
        BOOL hasProcessPath = FALSE;

        sqlite3_stmt* st = NULL;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(pause_events);", -1, &st, NULL) == SQLITE_OK && st) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char* name = (const char*)sqlite3_column_text(st, 1); // column name
                if (!name) continue;
                if (strcmp(name, "window_title") == 0) hasWindowTitle = TRUE;
                else if (strcmp(name, "process_path") == 0) hasProcessPath = TRUE;
            }
            sqlite3_finalize(st);
        } else if (st) {
            sqlite3_finalize(st);
        }

        if (hasWindowTitle || hasProcessPath) {
            char* err = NULL;
            BOOL ok = TRUE;

            if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err) != SQLITE_OK) ok = FALSE;
            if (err) {
                sqlite3_free(err);
                err = NULL;
            }

            if (ok) {
                if (sqlite3_exec(db, "ALTER TABLE pause_events RENAME TO pause_events_old;", NULL, NULL, &err) != SQLITE_OK) ok = FALSE;
                if (err) {
                    sqlite3_free(err);
                    err = NULL;
                }
            }

            if (ok) {
                if (sqlite3_exec(
                        db,
                        "CREATE TABLE pause_events ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "ts_utc TEXT NOT NULL,"
                        "action TEXT NOT NULL,"
                        "original TEXT,"
                        "corrected TEXT,"
                        "layout TEXT"
                        ");",
                        NULL, NULL, &err) != SQLITE_OK)
                    ok = FALSE;
                if (err) {
                    sqlite3_free(err);
                    err = NULL;
                }
            }

            if (ok) {
                if (sqlite3_exec(
                        db,
                        "INSERT INTO pause_events(id, ts_utc, action, original, corrected, layout) "
                        "SELECT id, ts_utc, action, original, corrected, layout FROM pause_events_old;",
                        NULL, NULL, &err) != SQLITE_OK)
                    ok = FALSE;
                if (err) {
                    sqlite3_free(err);
                    err = NULL;
                }
            }

            if (ok) {
                if (sqlite3_exec(db, "DROP TABLE pause_events_old;", NULL, NULL, &err) != SQLITE_OK) ok = FALSE;
                if (err) {
                    sqlite3_free(err);
                    err = NULL;
                }
            }

            if (ok) {
                (void)sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
            } else {
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            }
        }
    }

    sqlite3_close(db);
}

static void DbInsertModelEvent(const wchar_t* tsUtc,
                               const wchar_t* eventName,
                               int label,
                               BOOL toEnglish,
                               float pBefore,
                               float pAfter,
                               float deltaL1,
                               float lr,
                               float l2,
                               float threshold,
                               const wchar_t* original,
                               const wchar_t* corrected)
{
    if (g_db_path[0] == 0) return;
    if (!tsUtc || !eventName) return;

    sqlite3* db = NULL;
    if (sqlite3_open16(g_db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 500);

    sqlite3_stmt* st = NULL;
    const char* sql =
        "INSERT INTO model_events(ts_utc, event, label, to_english, p_before, p_after, delta_l1, lr, l2, threshold, original, corrected) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_text16(st, 1, tsUtc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 2, eventName, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, label);
    sqlite3_bind_int(st, 4, toEnglish ? 1 : 0);
    sqlite3_bind_double(st, 5, (double)pBefore);
    sqlite3_bind_double(st, 6, (double)pAfter);
    sqlite3_bind_double(st, 7, (double)deltaL1);
    sqlite3_bind_double(st, 8, (double)lr);
    sqlite3_bind_double(st, 9, (double)l2);
    sqlite3_bind_double(st, 10, (double)threshold);
    sqlite3_bind_text16(st, 11, original ? original : L"", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 12, corrected ? corrected : L"", -1, SQLITE_TRANSIENT);
    (void)sqlite3_step(st);

    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void DbInsertPauseEvent(const PauseLogItem* item)
{
    if (!item || g_db_path[0] == 0) return;

    sqlite3* db = NULL;
    if (sqlite3_open16(g_db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_stmt* st = NULL;
    const char* sql =
        "INSERT INTO pause_events(ts_utc, action, original, corrected, layout) "
        "VALUES(?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_text16(st, 1, item->ts_utc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 2, item->action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 3, item->original, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 4, item->corrected, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 5, item->layout_hex, -1, SQLITE_TRANSIENT);
    (void)sqlite3_step(st);

    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void PostPauseLogEvent(const wchar_t* action, const wchar_t* original, const wchar_t* corrected)
{
    if (!action || !action[0]) return;

    PauseLogItem* item = (PauseLogItem*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PauseLogItem));
    if (!item) return;

    UtcNowIso(item->ts_utc, ARRAYSIZE(item->ts_utc));
    GetForegroundInfo(item->window_title, ARRAYSIZE(item->window_title),
                      item->process_path, ARRAYSIZE(item->process_path),
                      item->layout_hex, ARRAYSIZE(item->layout_hex));

    StringCchCopyW(item->action, ARRAYSIZE(item->action), action);
    if (original) StringCchCopyW(item->original, ARRAYSIZE(item->original), original);
    if (corrected) StringCchCopyW(item->corrected, ARRAYSIZE(item->corrected), corrected);

    if (g_main_hwnd) {
        PostMessageW(g_main_hwnd, WM_LOG_PAUSE, 0, (LPARAM)item);
    } else {
        HeapFree(GetProcessHeap(), 0, item);
    }
}

static void ShowSqliteErrorBox(HWND hwnd, const wchar_t* context, sqlite3* db)
{
    const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error";
    wchar_t wmsg[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg, (int)ARRAYSIZE(wmsg));
    if (n <= 0) StringCchCopyW(wmsg, ARRAYSIZE(wmsg), L"(failed to decode sqlite error)");

    wchar_t buf[768];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s\n\n%s", context, wmsg);
    MessageBoxW(hwnd, buf, L"SQLite error", MB_OK | MB_ICONERROR);
}

static void ShowSqliteErrorBoxWithPath(HWND hwnd, const wchar_t* context, const wchar_t* path, sqlite3* db)
{
    wchar_t ctx[768];
    StringCchPrintfW(ctx, ARRAYSIZE(ctx), L"%s\n\nPath:\n%s", context, (path && path[0]) ? path : L"(empty)");
    ShowSqliteErrorBox(hwnd, ctx, db);
}

static void JsonEscapeToFile(FILE* f, const wchar_t* ws)
{
    if (!ws) return;
    for (const wchar_t* p = ws; *p; p++) {
        const wchar_t ch = *p;
        switch (ch) {
        case L'\\': fputws(L"\\\\", f); break;
        case L'"': fputws(L"\\\"", f); break;
        case L'\r': fputws(L"\\r", f); break;
        case L'\n': fputws(L"\\n", f); break;
        case L'\t': fputws(L"\\t", f); break;
        default:
            if (ch < 0x20) fwprintf(f, L"\\u%04x", (unsigned)ch);
            else fputwc(ch, f);
            break;
        }
    }
}

static void WriteUtf8(FILE* f, const wchar_t* ws)
{
    if (!f || !ws) return;
    const int wlen = (int)wcslen(ws);
    if (wlen <= 0) return;

    char buf[512];
    const wchar_t* p = ws;
    int remaining = wlen;
    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > 128) chunk = 128;
        int need = WideCharToMultiByte(CP_UTF8, 0, p, chunk, NULL, 0, NULL, NULL);
        if (need <= 0) return;
        if (need >= (int)sizeof(buf)) {
            char* dyn = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need);
            if (!dyn) return;
            WideCharToMultiByte(CP_UTF8, 0, p, chunk, dyn, need, NULL, NULL);
            fwrite(dyn, 1, (size_t)need, f);
            HeapFree(GetProcessHeap(), 0, dyn);
        } else {
            WideCharToMultiByte(CP_UTF8, 0, p, chunk, buf, (int)sizeof(buf), NULL, NULL);
            fwrite(buf, 1, (size_t)need, f);
        }
        p += chunk;
        remaining -= chunk;
    }
}

static void JsonEscapeToUtf8File(FILE* f, const wchar_t* ws)
{
    if (!f) return;
    if (!ws) ws = L"";

    for (const wchar_t* p = ws; *p; p++) {
        const wchar_t ch = *p;
        switch (ch) {
        case L'\\': fputs("\\\\", f); break;
        case L'"': fputs("\\\"", f); break;
        case L'\r': fputs("\\r", f); break;
        case L'\n': fputs("\\n", f); break;
        case L'\t': fputs("\\t", f); break;
        default:
            if (ch < 0x20) {
                char tmp[16];
                _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "\\u%04x", (unsigned)ch);
                fputs(tmp, f);
            } else {
                wchar_t one[2] = { ch, 0 };
                WriteUtf8(f, one);
            }
            break;
        }
    }
}

static void LocalModelStart(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetAppDir(dir, ARRAYSIZE(dir));
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\language_model.bin", dir);
    FILE* f=NULL; _wfopen_s(&f,path,L"rb");
    if (f) { (void)DSLoad(&g_language_model,f); fclose(f); }
    // The shipped baseline stays beside the EXE. Personal labels stay in LocalAppData.
    sqlite3* db=NULL;
    if (sqlite3_open16(g_db_path,&db)==SQLITE_OK) {
        sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS local_feedback (original TEXT PRIMARY KEY, target TEXT NOT NULL, label INTEGER NOT NULL CHECK(label IN (0,1)), updated INTEGER NOT NULL);",NULL,NULL,NULL);
        sqlite3_stmt* st=NULL;
        const char* sql="SELECT original,target,label FROM (SELECT * FROM local_feedback ORDER BY updated DESC,rowid DESC LIMIT 1024) ORDER BY updated,original;";
        if (sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK) {
            while (sqlite3_step(st)==SQLITE_ROW) {
                const wchar_t* a=(const wchar_t*)sqlite3_column_text16(st,0);
                const wchar_t* b=(const wchar_t*)sqlite3_column_text16(st,1);
                if (a && b) DSRemember(&g_language_model,a,b,sqlite3_column_int(st,2));
            }
        }
        sqlite3_finalize(st);
    }
    if (db) sqlite3_close(db);
    DSRefit(&g_language_model);
    StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\excluded_apps.txt",g_data_dir);
    _wfopen_s(&f,path,L"rt, ccs=UTF-8");
    if (f) {
        wchar_t line[MAX_PATH];
        while (g_exclusion_count<64 && fgetws(line,ARRAYSIZE(line),f)) {
            line[wcscspn(line,L"\r\n")]=0;
            if (line[0] && line[0]!=L'#') StringCchCopyW(g_exclusions[g_exclusion_count++],MAX_PATH,line);
        }
        fclose(f);
    }
}

static void SaveLocalFeedback(const LiteRTSample* sample)
{
    wchar_t a[65],b[65]; DSNormalize(sample->original,a); DSNormalize(sample->corrected,b);
    sqlite3* db=NULL;
    if (sqlite3_open16(g_db_path,&db)!=SQLITE_OK) { if (db) sqlite3_close(db); return; }
    sqlite3_busy_timeout(db,500);
    sqlite3_stmt* st=NULL;
    const char* sql="INSERT INTO local_feedback(original,target,label,updated) VALUES(?,?,?,?) ON CONFLICT(original) DO UPDATE SET target=excluded.target,label=excluded.label,updated=excluded.updated;";
    if (sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK) {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER ticks; ticks.LowPart=ft.dwLowDateTime; ticks.HighPart=ft.dwHighDateTime;
        sqlite3_bind_text16(st,1,a,-1,SQLITE_TRANSIENT); sqlite3_bind_text16(st,2,b,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(st,3,sample->label); sqlite3_bind_int64(st,4,(sqlite3_int64)ticks.QuadPart);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_exec(db,"DELETE FROM local_feedback WHERE original NOT IN (SELECT original FROM local_feedback ORDER BY updated DESC,rowid DESC LIMIT 1024);",NULL,NULL,NULL);
    sqlite3_close(db);
}

static void QueueDecisionLog(const wchar_t* original, const wchar_t* target, const DSDecision* decision)
{
    if (!g_diagnostics || !g_main_hwnd) return;
    DecisionLog* item=(DecisionLog*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(DecisionLog));
    if (!item) return;
    StringCchCopyW(item->original,65,original); StringCchCopyW(item->target,65,target);
    StringCchCopyW(item->reason,40,decision->reason); UtcNowIso(item->timestamp,40);
    item->decision=*decision; item->model_id=g_language_model.fingerprint;
    if (!PostMessageW(g_main_hwnd,WM_LOG_DECISION,0,(LPARAM)item)) HeapFree(GetProcessHeap(),0,item);
}

static void WriteDecisionLog(const DecisionLog* item)
{
    wchar_t path[MAX_PATH],previous[MAX_PATH];
    StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\decisions.jsonl",g_data_dir);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path,GetFileExInfoStandard,&info) && (info.nFileSizeHigh || info.nFileSizeLow>5*1024*1024)) {
        StringCchPrintfW(previous,ARRAYSIZE(previous),L"%s\\decisions.previous.jsonl",g_data_dir);
        MoveFileExW(path,previous,MOVEFILE_REPLACE_EXISTING);
    }
    FILE* f=NULL; _wfopen_s(&f,path,L"ab"); if (!f) return;
    fputs("{\"ts\":\"",f); JsonEscapeToUtf8File(f,item->timestamp);
    fprintf(f,"\",\"model\":\"%08x\",\"original\":\"",item->model_id);
    JsonEscapeToUtf8File(f,item->original); fputs("\",\"candidate\":\"",f);
    JsonEscapeToUtf8File(f,item->target); fputs("\",\"reason\":\"",f);
    JsonEscapeToUtf8File(f,item->reason);
    fprintf(f,"\",\"apply\":%d,\"p\":%.6f,\"original_frequency\":%.4f,\"target_frequency\":%.4f,\"original_score\":%.4f,\"target_score\":%.4f,\"label\":null}\n",
        item->decision.apply,item->decision.probability,item->decision.original_frequency,
        item->decision.target_frequency,item->decision.original_score,item->decision.target_score);
    fclose(f);
}

static void AppendMlSampleJsonl(const LiteRTSample* s)
{
    if (!s || g_ml_samples_path[0] == 0) return;
    FILE* f = NULL;
    _wfopen_s(&f, g_ml_samples_path, L"ab");
    if (!f) return;

    fputs("{\"ts\":\"", f);
    JsonEscapeToUtf8File(f, s->ts_utc);
    fputs("\",\"label\":", f);
    fprintf(f, "%d", s->label);
    fputs(",\"to_english\":", f);
    fprintf(f, "%d", s->toEnglish ? 1 : 0);
    fputs(",\"original\":\"", f);
    JsonEscapeToUtf8File(f, s->original);
    fputs("\",\"corrected\":\"", f);
    JsonEscapeToUtf8File(f, s->corrected);
    fputs("\",\"x\":[", f);
    for (int i = 0; i < LITERT_FEATURES; i++) {
        if (i) fputc(',', f);
        fprintf(f, "%.6g", (double)s->x[i]);
    }
    fprintf(f,"],\"engine\":\"DSLM0001\",\"baseline_id\":\"%08x\"",g_language_model.fingerprint);
    if (g_language_model.ready && DSSourceLanguage(s->original)>=0 && DSLanguage(s->corrected)>=0) {
        wchar_t a[65],b[65]; float localX[8];
        DSNormalize(s->original,a); DSNormalize(s->corrected,b);
        DSFeatures(&g_language_model,a,b,localX);
        fputs(",\"local_x\":[",f);
        for (int i=0;i<8;++i) { if (i) fputc(',',f); fprintf(f,"%.6g",localX[i]); }
        fputc(']',f);
    }
    fputs("}\n", f);

    fclose(f);
}

static DWORD WINAPI LiteRTWorkerThread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        DWORD w = WaitForSingleObject(g_ml_event, 1000);
        if (w == WAIT_FAILED) break;

        for (;;) {
            LiteRTSample s={0};
            BOOL have = FALSE;
            if (g_ml_q_cs_init) {
                EnterCriticalSection(&g_ml_q_cs);
                LONG tail = g_ml_q_tail;
                if (tail != g_ml_q_head) {
                    s = g_ml_queue[tail];
                    g_ml_q_tail = (tail + 1) % (LONG)ARRAYSIZE(g_ml_queue);
                    have = TRUE;
                }
                LeaveCriticalSection(&g_ml_q_cs);
            }
            if (!have) break;

            if (g_language_model.ready && g_model_cs_init) {
                EnterCriticalSection(&g_model_cs);
                DSRemember(&g_language_model,s.original,s.corrected,s.label);
                DSRefit(&g_language_model);
                LeaveCriticalSection(&g_model_cs);
                SaveLocalFeedback(&s);
                DbInsertModelEvent(s.ts_utc,L"local_feedback",s.label,s.toEnglish,
                                   0,0,0,0.01f,0.1f,g_language_model.threshold,s.original,s.corrected);
            } else if (g_model_cs_init) {
                float pBefore = 0.0f;
                float pAfter = 0.0f;
                float deltaL1 = 0.0f;
                float lr = 0.0f, l2 = 0.0f, thr = 0.0f;

                EnterCriticalSection(&g_model_cs);
                pBefore = LiteRT_PredictLocked(&g_model, s.x);
                float wBefore[LITERT_FEATURES];
                memcpy(wBefore, g_model.w, sizeof(wBefore));
                const float bBefore = g_model.b;
                lr = g_model.lr;
                l2 = g_model.l2;
                thr = g_model.threshold;
                // Legacy model retained only for compatibility; no repeated amplification.
                int repeats = 1;
                for (int i = 0; i < repeats; i++) {
                    LiteRT_TrainLocked(&g_model, s.x, s.label);
                }
                pAfter = LiteRT_PredictLocked(&g_model, s.x);
                for (int i = 0; i < LITERT_FEATURES; i++) {
                    deltaL1 += (float)fabs((double)(g_model.w[i] - wBefore[i]));
                }
                deltaL1 += (float)fabs((double)(g_model.b - bBefore));
                LiteRT_SaveLocked(&g_model, g_model_path);
                LeaveCriticalSection(&g_model_cs);

                DbInsertModelEvent(s.ts_utc,
                                   L"train_step",
                                   s.label,
                                   s.toEnglish,
                                   pBefore,
                                   pAfter,
                                   deltaL1,
                                   lr,
                                   l2,
                                   thr,
                                   s.original,
                                   s.corrected);
            }
            AppendMlSampleJsonl(&s);
        }

        {
            const LONG dropped = InterlockedExchange(&g_ml_q_dropped, 0);
            if (dropped > 0) {
                wchar_t ts[40];
                UtcNowIso(ts, ARRAYSIZE(ts));
                DbInsertModelEvent(ts,
                                   L"queue_drop",
                                   (int)dropped,
                                   FALSE,
                                   0.0f,
                                   0.0f,
                                   0.0f,
                                   g_model.lr,
                                   g_model.l2,
                                   g_model.threshold,
                                   L"",
                                   L"");
            }
        }

        if (InterlockedCompareExchange(&g_ml_stop, 0, 0)) break;
        ResetEvent(g_ml_event);
    }
    return 0;
}

static void LiteRT_Start(void)
{
    if (g_ml_event) return;

    if (!g_model_cs_init) {
        InitializeCriticalSection(&g_model_cs);
        g_model_cs_init = TRUE;
    }
    if (!g_ml_q_cs_init) {
        InitializeCriticalSection(&g_ml_q_cs);
        g_ml_q_cs_init = TRUE;
    }

    LiteRT_InitDefaults(&g_model);
    if (g_model_path[0] != 0) {
        // If no user model exists yet, allow bundling a pre-trained model next to the EXE.
        if (!FileExistsW_(g_model_path)) {
            wchar_t appDir[MAX_PATH];
            GetAppDir(appDir, ARRAYSIZE(appDir));
            if (appDir[0] != 0) {
                wchar_t bundled[MAX_PATH];
                bundled[0] = 0;
                if (SUCCEEDED(StringCchPrintfW(bundled, ARRAYSIZE(bundled), L"%s\\litert_model.bin", appDir)) &&
                    FileExistsW_(bundled)) {
                    (void)CopyFileW(bundled, g_model_path, TRUE);
                }
            }
        }
        (void)LiteRT_Load(&g_model, g_model_path);
    }

    g_ml_q_head = 0;
    g_ml_q_tail = 0;
    g_ml_q_dropped = 0;

    LocalModelStart();

    // Ensure a model file exists even before the first feedback arrives.
    if (g_model_path[0] != 0) {
        EnterCriticalSection(&g_model_cs);
        LiteRT_SaveLocked(&g_model, g_model_path);
        LeaveCriticalSection(&g_model_cs);
    }

    g_ml_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ml_event) return;

    g_ml_stop = 0;
    DWORD tid = 0;
    g_ml_thread = CreateThread(NULL, 0, LiteRTWorkerThread, NULL, 0, &tid);
}

static void LiteRT_Stop(void)
{
    if (!g_ml_event) return;
    InterlockedExchange(&g_ml_stop, 1);
    SetEvent(g_ml_event);
    if (g_ml_thread) {
        WaitForSingleObject(g_ml_thread, INFINITE);
        CloseHandle(g_ml_thread);
        g_ml_thread = NULL;
    }
    CloseHandle(g_ml_event);
    g_ml_event = NULL;

    if (g_model_cs_init) {
        DeleteCriticalSection(&g_model_cs);
        g_model_cs_init = FALSE;
    }
    if (g_ml_q_cs_init) {
        DeleteCriticalSection(&g_ml_q_cs);
        g_ml_q_cs_init = FALSE;
    }
}

static void ExportDbToTextFile(HWND hwnd)
{
    if (g_db_path[0] == 0) {
        MessageBoxW(hwnd, L"DB path is empty (restart the app).", L"Export DB", MB_OK | MB_ICONERROR);
        return;
    }

    // Ensure schema exists (in case DB file was created but init failed earlier).
    DbInitIfNeeded();

    wchar_t initialDir[MAX_PATH];
    GetAppDir(initialDir, ARRAYSIZE(initialDir));

    wchar_t outPath[MAX_PATH];
    ZeroMemory(outPath, sizeof(outPath));
    StringCchCopyW(outPath, ARRAYSIZE(outPath), L"pause_events.jsonl");

    // Save As dialog
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = outPath;
    ofn.nMaxFile = ARRAYSIZE(outPath);
    ofn.lpstrInitialDir = (initialDir[0] != 0) ? initialDir : NULL;
    ofn.lpstrTitle = L"Export pause events";
    ofn.lpstrFilter = L"JSON Lines (*.jsonl)\0*.jsonl\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"jsonl";

    if (!GetSaveFileNameW(&ofn)) {
        // User canceled or dialog failed; don't show an error for cancel.
        const DWORD err = CommDlgExtendedError();
        if (err != 0) {
            wchar_t msg[256];
            StringCchPrintfW(msg, ARRAYSIZE(msg), L"Save dialog failed (0x%08lX).", (unsigned long)err);
            MessageBoxW(hwnd, msg, L"Export DB", MB_OK | MB_ICONERROR);
        }
        return;
    }

    sqlite3* db = NULL;
    if (sqlite3_open16(g_db_path, &db) != SQLITE_OK) {
        ShowSqliteErrorBoxWithPath(hwnd, L"Failed to open DB.", g_db_path, db);
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_stmt* st = NULL;
    const char* sql =
        "SELECT id, ts_utc, action, original, corrected, layout "
        "FROM pause_events ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        ShowSqliteErrorBoxWithPath(hwnd, L"Failed to query DB (prepare).", g_db_path, db);
        sqlite3_close(db);
        return;
    }

    FILE* f = NULL;
    _wfopen_s(&f, outPath, L"wb");
    if (!f) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        MessageBoxW(hwnd, outPath, L"Failed to create file", MB_OK | MB_ICONERROR);
        return;
    }

    // UTF-8 BOM for Notepad friendliness.
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, 3, f);

    int rowCount = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        rowCount++;
        const int id = sqlite3_column_int(st, 0);
        const wchar_t* ts = (const wchar_t*)sqlite3_column_text16(st, 1);
        const wchar_t* action = (const wchar_t*)sqlite3_column_text16(st, 2);
        const wchar_t* orig = (const wchar_t*)sqlite3_column_text16(st, 3);
        const wchar_t* corr = (const wchar_t*)sqlite3_column_text16(st, 4);
        const wchar_t* layout = (const wchar_t*)sqlite3_column_text16(st, 5);

        char head[64];
        _snprintf_s(head, sizeof(head), _TRUNCATE, "{\"id\":%d,\"ts\":\"", id);
        fputs(head, f);
        JsonEscapeToUtf8File(f, ts);
        fputs("\",\"action\":\"", f);
        JsonEscapeToUtf8File(f, action);
        fputs("\",\"original\":\"", f);
        JsonEscapeToUtf8File(f, orig);
        fputs("\",\"corrected\":\"", f);
        JsonEscapeToUtf8File(f, corr);
        fputs("\",\"layout\":\"", f);
        JsonEscapeToUtf8File(f, layout);
        fputs("\"}\n", f);
    }

    fclose(f);
    sqlite3_finalize(st);
    sqlite3_close(db);

    {
        wchar_t msg[512];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Wrote %d rows to:\n%s", rowCount, outPath);
        MessageBoxW(hwnd, msg, L"Export DB", MB_OK | MB_ICONINFORMATION);
    }
}

static void ExportModelEventsToTextFile(HWND hwnd)
{
    if (g_db_path[0] == 0) {
        MessageBoxW(hwnd, L"DB path is empty (restart the app).", L"Export Model Log", MB_OK | MB_ICONERROR);
        return;
    }

    // Ensure schema exists (in case DB file was created but init failed earlier).
    DbInitIfNeeded();

    wchar_t initialDir[MAX_PATH];
    GetAppDir(initialDir, ARRAYSIZE(initialDir));

    wchar_t outPath[MAX_PATH];
    ZeroMemory(outPath, sizeof(outPath));
    StringCchCopyW(outPath, ARRAYSIZE(outPath), L"model_events.jsonl");

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = outPath;
    ofn.nMaxFile = ARRAYSIZE(outPath);
    ofn.lpstrInitialDir = (initialDir[0] != 0) ? initialDir : NULL;
    ofn.lpstrTitle = L"Export model events";
    ofn.lpstrFilter = L"JSON Lines (*.jsonl)\0*.jsonl\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"jsonl";

    if (!GetSaveFileNameW(&ofn)) {
        const DWORD err = CommDlgExtendedError();
        if (err != 0) {
            wchar_t msg[256];
            StringCchPrintfW(msg, ARRAYSIZE(msg), L"Save dialog failed (0x%08lX).", (unsigned long)err);
            MessageBoxW(hwnd, msg, L"Export Model Log", MB_OK | MB_ICONERROR);
        }
        return;
    }

    sqlite3* db = NULL;
    if (sqlite3_open16(g_db_path, &db) != SQLITE_OK) {
        ShowSqliteErrorBoxWithPath(hwnd, L"Failed to open DB.", g_db_path, db);
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_stmt* st = NULL;
    const char* sql =
        "SELECT id, ts_utc, event, label, to_english, p_before, p_after, delta_l1, lr, l2, threshold, original, corrected "
        "FROM model_events ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        ShowSqliteErrorBoxWithPath(hwnd, L"Failed to query DB (prepare).", g_db_path, db);
        sqlite3_close(db);
        return;
    }

    FILE* f = NULL;
    _wfopen_s(&f, outPath, L"wb");
    if (!f) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        MessageBoxW(hwnd, outPath, L"Failed to create file", MB_OK | MB_ICONERROR);
        return;
    }

    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, 3, f);

    int rowCount = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        rowCount++;
        const int id = sqlite3_column_int(st, 0);
        const wchar_t* ts = (const wchar_t*)sqlite3_column_text16(st, 1);
        const wchar_t* ev = (const wchar_t*)sqlite3_column_text16(st, 2);
        const int label = sqlite3_column_int(st, 3);
        const int toEn = sqlite3_column_int(st, 4);
        const double pBefore = sqlite3_column_double(st, 5);
        const double pAfter = sqlite3_column_double(st, 6);
        const double dL1 = sqlite3_column_double(st, 7);
        const double lr = sqlite3_column_double(st, 8);
        const double l2 = sqlite3_column_double(st, 9);
        const double thr = sqlite3_column_double(st, 10);
        const wchar_t* orig = (const wchar_t*)sqlite3_column_text16(st, 11);
        const wchar_t* corr = (const wchar_t*)sqlite3_column_text16(st, 12);

        char head[128];
        _snprintf_s(head, sizeof(head), _TRUNCATE, "{\"id\":%d,\"ts\":\"", id);
        fputs(head, f);
        JsonEscapeToUtf8File(f, ts);
        fputs("\",\"event\":\"", f);
        JsonEscapeToUtf8File(f, ev);
        fputs("\",\"label\":", f);
        fprintf(f, "%d", label);
        fputs(",\"to_english\":", f);
        fprintf(f, "%d", toEn);
        fputs(",\"p_before\":", f);
        fprintf(f, "%.6g", pBefore);
        fputs(",\"p_after\":", f);
        fprintf(f, "%.6g", pAfter);
        fputs(",\"delta_l1\":", f);
        fprintf(f, "%.6g", dL1);
        fputs(",\"lr\":", f);
        fprintf(f, "%.6g", lr);
        fputs(",\"l2\":", f);
        fprintf(f, "%.6g", l2);
        fputs(",\"threshold\":", f);
        fprintf(f, "%.6g", thr);
        fputs(",\"original\":\"", f);
        JsonEscapeToUtf8File(f, orig);
        fputs("\",\"corrected\":\"", f);
        JsonEscapeToUtf8File(f, corr);
        fputs("\"}\n", f);
    }

    fclose(f);
    sqlite3_finalize(st);
    sqlite3_close(db);

    {
        wchar_t msg[512];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Wrote %d rows to:\n%s", rowCount, outPath);
        MessageBoxW(hwnd, msg, L"Export Model Log", MB_OK | MB_ICONINFORMATION);
    }
}

static wchar_t ToLowerInvariant(wchar_t ch)
{
    return DSLower(ch);
}

static const wchar_t* const kEnCommonBigrams[] = {
    L"th", L"he", L"in", L"er", L"an", L"re", L"on", L"at", L"en", L"nd",
    L"ti", L"es", L"or", L"te", L"of", L"ed", L"is", L"it", L"al", L"ar",
    L"st", L"to", L"nt", L"ng", L"se", L"ha", L"as", L"ou", L"io", L"le",
    // Short-word helpers
    L"oo", L"ck", L"ok", L"bo", L"ee",
    // Common patterns (helps prevent false EN->RU on normal English words)
    L"ly", L"gh", L"ht", L"ig", L"sl", L"li",
};

static const wchar_t* const kRuCommonBigrams[] = {
    L"\u0441\u0442", L"\u043d\u043e", L"\u0442\u043e", L"\u043d\u0430", L"\u0435\u043d", L"\u043e\u0432", L"\u043d\u0438", L"\u0440\u0430", L"\u0432\u043e", L"\u043a\u043e",
    L"\u043f\u0440", L"\u043f\u043e", L"\u0435\u0440", L"\u0440\u043e", L"\u043e\u0441", L"\u0430\u043b", L"\u0442\u0430", L"\u0432\u0430", L"\u043d\u0435", L"\u043b\u0438",
    L"\u0440\u0435",
};

static const wchar_t* const kRuBadBigrams[] = {
    L"\u0449\u0449", // щщ
    L"\u044a\u044a", // ъъ
    L"\u044b\u044b", // ыы
    L"\u0439\u0439", // йй
    L"\u044c\u044a", // ьъ
    L"\u044a\u044c", // ъь
    L"\u0436\u044b", // жы (should be жи)
    L"\u0448\u044b", // шы (should be ши)
    // Soft sign is typically before vowels (ья/ье/ё/ю/и) or word end; consonants after ь are usually gibberish.
    L"\u044c\u0448", // ьш
    L"\u044c\u043f", // ьп
    L"\u044c\u0440", // ьр
    L"\u044c\u0442", // ьт
    L"\u044c\u043d", // ьн
    L"\u044c\u0441", // ьс
    L"\u044c\u043a", // ьк
    L"\u044c\u0433", // ьг
    L"\u044c\u043c", // ьм
    L"\u044c\u0434", // ьд
    L"\u044c\u0436", // ьж
    L"\u044c\u0437", // ьз
    L"\u044c\u0445", // ьх
    L"\u044c\u0446", // ьц
    L"\u044c\u0447", // ьч
    L"\u044c\u0449", // ьщ
    L"\u044c\u0444", // ьф
    L"\u044c\u043b", // ьл
    L"\u044c\u0432", // ьв
    L"\u044c\u0431", // ьб

    // Very rare sequences that show up frequently when mapping English to Russian.
    L"\u044b\u0434", // ыд
    L"\u0434\u0448", // дш
};

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
    int latin = 0, nonLatinLetters = 0;
    for (const wchar_t* p = tokenLower; *p; p++) {
        if (IsLatinLetter(*p)) latin++;
        else if (iswalpha((wint_t)*p)) nonLatinLetters++;
    }
    if (latin == 0) return -1000;
    if (nonLatinLetters > 0) return -500;

    const int hits = FindBigramScore(tokenLower, kEnCommonBigrams, ARRAYSIZE(kEnCommonBigrams));
    const size_t n = wcslen(tokenLower);
    const double vr = VowelRatioEn(tokenLower);

    int score = 0;
    score += hits * 3;
    // Common suffixes boost (reduces false EN->RU like "slightly" -> "ыдшпредн").
    if (EndsWith(tokenLower, L"ly")) score += 8;
    if (EndsWith(tokenLower, L"ing")) score += 8;
    if (EndsWith(tokenLower, L"ed")) score += 5;
    if (EndsWith(tokenLower, L"ion")) score += 5;
    if (EndsWith(tokenLower, L"tion")) score += 6;
    if (EndsWith(tokenLower, L"ment")) score += 6;
    if (EndsWith(tokenLower, L"ness")) score += 6;
    if (EndsWith(tokenLower, L"able")) score += 6;
    if (EndsWith(tokenLower, L"less")) score += 6;
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
    int cyr = 0, nonCyrLetters = 0;
    for (const wchar_t* p = tokenLower; *p; p++) {
        if (IsCyrillicLetter(*p)) cyr++;
        else if (iswalpha((wint_t)*p)) nonCyrLetters++;
    }
    if (cyr == 0) return -1000;
    if (nonCyrLetters > 0) return -500;

    const int hits = FindBigramScore(tokenLower, kRuCommonBigrams, ARRAYSIZE(kRuCommonBigrams));
    const int badHits = CountBadBigrams(tokenLower, kRuBadBigrams, ARRAYSIZE(kRuBadBigrams));
    const size_t n = wcslen(tokenLower);
    const double vr = VowelRatioRu(tokenLower);

    int score = 0;
    score += hits * 3;
    score -= badHits * 8;
    // Common Russian endings (small boost to prevent false EN mapping).
    if (EndsWith(tokenLower, L"\u0443\u0442")) score += 6;            // ут
    else if (EndsWith(tokenLower, L"\u044e\u0442")) score += 6;       // ют
    else if (EndsWith(tokenLower, L"\u0435\u0442")) score += 5;       // ет
    else if (EndsWith(tokenLower, L"\u0438\u0442")) score += 5;       // ит
    else if (EndsWith(tokenLower, L"\u0442\u044c")) score += 4;       // ть
    else if (EndsWith(tokenLower, L"\u044b\u0439")) score += 4;       // ый
    else if (EndsWith(tokenLower, L"\u0430\u044f")) score += 4;       // ая
    else if (EndsWith(tokenLower, L"\u043e\u0435")) score += 4;       // ое
    else if (EndsWith(tokenLower, L"\u044b\u0435")) score += 4;       // ые
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
        if (upper && mapped>=L'а' && mapped<=L'я') mapped=(wchar_t)(mapped-32);
        else if (upper && mapped==L'ё') mapped=L'Ё';
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

static BOOL SendBackspacesAndText(size_t backspaces, const wchar_t* text)
{
    INPUT inputs[4*TOKEN_MAX_CHARS+8];
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
        return SendInput(count, inputs, sizeof(INPUT))==count;
    }
    return FALSE;
}

static void InvalidateLastFix(void)
{
    g_last_fix.active = FALSE;
}

static void RememberLastWord(const wchar_t* token, size_t tokenLen, wchar_t boundaryChar, BOOL hadBoundary)
{
    if (!token || tokenLen == 0) return;
    if (tokenLen > TOKEN_MAX_CHARS) tokenLen = TOKEN_MAX_CHARS;

    wmemcpy(g_last_word, token, tokenLen);
    g_last_word[tokenLen] = 0;
    g_last_word_len = tokenLen;
    g_last_word_boundary = boundaryChar;
    g_last_word_had_boundary = hadBoundary ? TRUE : FALSE;
    g_last_word_ts_ms = GetTickCount64();
}

static void ExtractLiteRTFeatures(const wchar_t* token, const wchar_t* mapped, BOOL toEnglish, float outX[LITERT_FEATURES])
{
    if (!outX) return;
    for (int i = 0; i < LITERT_FEATURES; i++) outX[i] = 0.0f;

    if (!token || !mapped) return;

    const size_t n = wcslen(token);
    if (n == 0) return;

    wchar_t lower[TOKEN_MAX_CHARS + 1];
    const size_t ln = (n > TOKEN_MAX_CHARS) ? TOKEN_MAX_CHARS : n;
    for (size_t i = 0; i < ln; i++) lower[i] = ToLowerInvariant(token[i]);
    lower[ln] = 0;

    wchar_t mappedLower[TOKEN_MAX_CHARS + 1];
    const size_t mn = wcslen(mapped);
    const size_t mln = (mn > TOKEN_MAX_CHARS) ? TOKEN_MAX_CHARS : mn;
    for (size_t i = 0; i < mln; i++) mappedLower[i] = ToLowerInvariant(mapped[i]);
    mappedLower[mln] = 0;

    int latin = 0, cyr = 0, digits = 0;
    for (size_t i = 0; i < ln; i++) {
        if (IsLatinLetter(lower[i])) latin++;
        else if (IsCyrillicLetter(lower[i])) cyr++;
        else if (iswdigit((wint_t)lower[i])) digits++;
    }

    const int scoreEn = ScoreEnglish(lower);
    const int scoreRu = ScoreRussian(lower);
    const int baseScore = (cyr > 0) ? scoreRu : scoreEn;

    const int mappedScore = toEnglish ? ScoreEnglish(mappedLower) : ScoreRussian(mappedLower);
    const int diff = mappedScore - baseScore;

    const double vrBase = (cyr > 0) ? VowelRatioRu(lower) : VowelRatioEn(lower);
    const double vrMapped = toEnglish ? VowelRatioEn(mappedLower) : VowelRatioRu(mappedLower);

    const int badHitsBase = (cyr > 0) ? CountBadBigrams(lower, kRuBadBigrams, ARRAYSIZE(kRuBadBigrams)) : 0;
    const int badHitsMapped = (!toEnglish) ? CountBadBigrams(mappedLower, kRuBadBigrams, ARRAYSIZE(kRuBadBigrams)) : 0;

    // Feature scaling: keep values in roughly [-2..2].
    outX[0] = (float)diff / 10.0f;
    outX[1] = (float)mappedScore / 20.0f;
    outX[2] = (float)baseScore / 20.0f;
    outX[3] = (float)badHitsBase;
    outX[4] = (float)ln / 10.0f;
    outX[5] = (float)vrBase;
    outX[6] = (float)vrMapped;
    outX[7] = (float)latin / 10.0f;
    outX[8] = (float)cyr / 10.0f;
    outX[9] = (float)badHitsMapped;
    outX[10] = toEnglish ? 1.0f : 0.0f;
    outX[11] = (digits > 0) ? 1.0f : 0.0f;
}

static BOOL ToggleLastFixIfPossible(void)
{
    if (!g_last_fix.active) return FALSE;

    const ULONGLONG now = GetTickCount64();
    if (now - g_last_fix.ts_ms > 30000) { // 30s window
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
    if (!targetLayout) return FALSE;

    if (g_last_fix.had_boundary) {
        // Cursor is after: current + boundary. Replace with: target + boundary.
        wchar_t out[TOKEN_MAX_CHARS + 2];
        if (targetLen + 1 >= ARRAYSIZE(out)) {
            g_last_fix.active = FALSE;
            return FALSE;
        }
        memcpy(out, targetText, (targetLen + 1) * sizeof(wchar_t));
        out[targetLen] = g_last_fix.boundary;
        out[targetLen + 1] = 0;

        if (!SendBackspacesAndText(currentLen + 1, out)) { ResetTypingState(); return FALSE; }
    } else {
        // Cursor is after: current. Replace with: target.
        if (!SendBackspacesAndText(currentLen, targetText)) { ResetTypingState(); return FALSE; }
    }

    RequestLayoutSwitch(targetLayout);
    g_last_fix.corrected_applied = want_corrected ? TRUE : FALSE;
    g_last_fix.ts_ms = now; // extend window while toggling
    return TRUE;
}

static BOOL MapWordToOtherLayout(const wchar_t* word, wchar_t* outMapped, size_t outCap, BOOL* outToEnglish)
{
    if (!word || !outMapped || outCap == 0) return FALSE;

    const size_t n = wcslen(word);
    if (n == 0 || n > TOKEN_MAX_CHARS) return FALSE;

    int latin = 0, cyr = 0, otherLetters = 0;
    for (size_t i = 0; i < n; i++) {
        const wchar_t ch = word[i];
        if (IsLatinLetter(ch)) latin++;
        else if (IsCyrillicLetter(ch)) cyr++;
        else if (iswalpha((wint_t)ch)) otherLetters++;
    }
    if (otherLetters > 0) return FALSE;
    if (latin > 0 && cyr > 0) return FALSE;

    if (cyr > 0) {
        MapRuToEn(word, outMapped, outCap);
        if (outToEnglish) *outToEnglish = TRUE;
        return TRUE;
    }
    if (latin > 0) {
        MapEnToRu(word, outMapped, outCap);
        if (outToEnglish) *outToEnglish = FALSE;
        return TRUE;
    }
    return FALSE;
}

static BOOL FlipWordIfPossible(const wchar_t* word, wchar_t boundaryChar, BOOL includeBoundary)
{
    if (!word || !word[0]) return FALSE;

    wchar_t mapped[TOKEN_MAX_CHARS + 1];
    BOOL toEnglish = FALSE;
    if (!MapWordToOtherLayout(word, mapped, ARRAYSIZE(mapped), &toEnglish)) return FALSE;
    if (wcscmp(mapped, word) == 0) return FALSE;

    const size_t wordLen = wcslen(word);
    const size_t mappedLen = wcslen(mapped);

    const HKL target = FindLayoutByPrimaryLang(toEnglish ? LANG_ENGLISH : LANG_RUSSIAN);
    if (!target) return FALSE;

    if (includeBoundary) {
        wchar_t out[TOKEN_MAX_CHARS + 2];
        if (mappedLen + 1 >= ARRAYSIZE(out)) return FALSE;
        memcpy(out, mapped, (mappedLen + 1) * sizeof(wchar_t));
        out[mappedLen] = boundaryChar;
        out[mappedLen + 1] = 0;
        if (!SendBackspacesAndText(wordLen + 1, out)) { ResetTypingState(); return FALSE; }
    } else {
        if (!SendBackspacesAndText(wordLen, mapped)) { ResetTypingState(); return FALSE; }
    }

    RequestLayoutSwitch(target);
    // Save as the last fix so Pause can toggle back/forth.
    ZeroMemory(&g_last_fix, sizeof(g_last_fix));
    g_last_fix.active = TRUE;
    g_last_fix.ts_ms = GetTickCount64();
    StringCchCopyW(g_last_fix.original, ARRAYSIZE(g_last_fix.original), word);
    StringCchCopyW(g_last_fix.corrected, ARRAYSIZE(g_last_fix.corrected), mapped);
    g_last_fix.original_len = wordLen;
    g_last_fix.corrected_len = mappedLen;
    g_last_fix.boundary = boundaryChar;
    g_last_fix.had_boundary = includeBoundary ? TRUE : FALSE;
    g_last_fix.corrected_to_english = toEnglish ? TRUE : FALSE;
    g_last_fix.corrected_applied = TRUE;

    return TRUE;
}

static BOOL LiteRT_QueueSample(int label, const wchar_t* original, const wchar_t* corrected, BOOL toEnglish)
{
    if (!original || !corrected) return FALSE;
    if (!g_ml_event) return FALSE;

    LiteRTSample s;
    ZeroMemory(&s, sizeof(s));
    UtcNowIso(s.ts_utc, ARRAYSIZE(s.ts_utc));
    StringCchCopyW(s.original, ARRAYSIZE(s.original), original);
    StringCchCopyW(s.corrected, ARRAYSIZE(s.corrected), corrected);
    s.toEnglish = toEnglish ? TRUE : FALSE;
    s.label = label ? 1 : 0;
    ExtractLiteRTFeatures(original, corrected, toEnglish, s.x);

    if (!g_ml_q_cs_init) return FALSE;
    EnterCriticalSection(&g_ml_q_cs);
    LONG head = g_ml_q_head;
    LONG next = (head + 1) % (LONG)ARRAYSIZE(g_ml_queue);
    if (next == g_ml_q_tail) {
        InterlockedIncrement(&g_ml_q_dropped);
        LeaveCriticalSection(&g_ml_q_cs);
        return FALSE;
    }
    g_ml_queue[head] = s;
    g_ml_q_head = next;
    LeaveCriticalSection(&g_ml_q_cs);

    SetEvent(g_ml_event);
    return TRUE;
}

static BOOL TryAutocorrectToken(const wchar_t* token, wchar_t boundaryChar, BOOL includeBoundary)
{
    const size_t n = wcslen(token);
    if (n < MIN_TOKEN_LEN || n > TOKEN_MAX_CHARS) return FALSE;
    ++g_session_candidates;
    wchar_t mapped[TOKEN_MAX_CHARS + 1] = {0};
    BOOL toEnglish = FALSE;
    DSDecision decision = {0};
    decision.reason = L"unsupported";
    if (!MapWordToOtherLayout(token, mapped, ARRAYSIZE(mapped), &toEnglish)) {
        QueueDecisionLog(token, mapped, &decision);
        return FALSE;
    }
    EnterCriticalSection(&g_model_cs);
    decision = DSDecide(&g_language_model, token, mapped);
    LeaveCriticalSection(&g_model_cs);
    const HKL target = FindLayoutByPrimaryLang(toEnglish ? LANG_ENGLISH : LANG_RUSSIAN);
    if (!target) { decision.apply=0; decision.reason=L"layout_missing"; }
    QueueDecisionLog(token, mapped, &decision);
    if (decision.apply) {
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

        BOOL sent=FALSE;
        if (includeBoundary) {
            wchar_t withBoundary[TOKEN_MAX_CHARS + 2];
            size_t ml = wcslen(mapped);
            if (ml + 1 < ARRAYSIZE(withBoundary)) {
                memcpy(withBoundary, mapped, (ml + 1) * sizeof(wchar_t));
                withBoundary[ml] = boundaryChar;
                withBoundary[ml + 1] = 0;
                sent=SendBackspacesAndText(n, withBoundary);
            } else {
                sent=SendBackspacesAndText(n, mapped);
            }
        } else {
            sent=SendBackspacesAndText(n, mapped);
        }
        if (!sent) {
            decision.apply=0; decision.reason=L"injection_failed";
            QueueDecisionLog(token,mapped,&decision); ResetTypingState(); return FALSE;
        }
        RequestLayoutSwitch(target);
        ++g_session_corrections;
        PostPauseLogEvent(toEnglish ? L"autocorrect_ru_to_en" : L"autocorrect_en_to_ru", token, mapped);
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
    if (g_taskbar_created) fn(hwnd, g_taskbar_created, 1, &cfs);
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

static void CALLBACK TypingFocusChanged(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                       LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)hwnd; (void)object; (void)child; (void)thread; (void)time;
    // Accessibility focus notifications can be repeated or come from background
    // controls. They are not evidence that the user's typing target changed.
    HWND foreground=GetForegroundWindow();
    DWORD tid=GetWindowThreadProcessId(foreground,NULL);
    GUITHREADINFO gui={0}; gui.cbSize=sizeof(gui);
    if (!tid || !GetGUIThreadInfo(tid,&gui)) {
        ResetTypingState(); g_target_window=NULL; g_target_focus=NULL;
        g_exclusion_window=NULL; return;
    }
    if (foreground!=g_target_window || gui.hwndFocus!=g_target_focus) {
        ResetTypingState(); g_target_window=foreground; g_target_focus=gui.hwndFocus;
        g_exclusion_window=NULL;
    }
}

static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM message, LPARAM data)
{
    if (code==HC_ACTION && (message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN ||
        message==WM_MBUTTONDOWN || message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)) ResetTypingState();
    return CallNextHookEx(NULL,code,message,data);
}

static BOOL IsInputExcluded(HWND window, HWND focus)
{
    if (window!=g_exclusion_window) {
        g_exclusion_window=window; g_excluded_window=FALSE;
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        if (pid==GetCurrentProcessId()) { g_excluded_window=TRUE; return TRUE; }
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if (process) {
            wchar_t path[MAX_PATH]; DWORD length=ARRAYSIZE(path);
            if (QueryFullProcessImageNameW(process,0,path,&length)) {
                const wchar_t* name=wcsrchr(path,L'\\'); name=name?name+1:path;
                for (int i=0;i<g_exclusion_count;++i)
                    if (!_wcsicmp(name,g_exclusions[i])) { g_excluded_window=TRUE; break; }
            }
            CloseHandle(process);
        }
    }
    if (g_excluded_window) return TRUE;
    // Native password edits are detectable without reading their contents.
    // Browser/custom controls require an application exclusion or manual pause.
    if (focus) {
        wchar_t className[80]={0}; GetClassNameW(focus,className,ARRAYSIZE(className));
        if (!_wcsicmp(className,L"Edit") || wcsstr(className,L"RichEdit") || wcsstr(className,L"RICHEDIT")) {
            if (GetWindowLongPtrW(focus,GWL_STYLE)&ES_PASSWORD) return TRUE;
            DWORD_PTR passwordChar=0;
            if (SendMessageTimeoutW(focus,EM_GETPASSWORDCHAR,0,0,SMTO_ABORTIFHUNG,10,&passwordChar) && passwordChar) return TRUE;
        }
    }
    return FALSE;
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

            break;
        default:
            break;
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            HWND foreground=GetForegroundWindow();
            GUITHREADINFO gui={0}; gui.cbSize=sizeof(gui);
            DWORD tid=GetWindowThreadProcessId(foreground,NULL);
            BOOL haveGui=tid && GetGUIThreadInfo(tid,&gui);
            HWND focus=haveGui?gui.hwndFocus:NULL;
            if (foreground!=g_target_window || focus!=g_target_focus) {
                ResetTypingState(); g_target_window=foreground; g_target_focus=focus;
            }
            if (!g_enabled || !haveGui || IsInputExcluded(foreground,focus)) {
                ResetTypingState(); return CallNextHookEx(NULL,nCode,wParam,lParam);
            }
            // Modifier key-down is not a word boundary (e.g. Shift for a capital).
            if (k->vkCode==VK_SHIFT || k->vkCode==VK_LSHIFT || k->vkCode==VK_RSHIFT) {
                return CallNextHookEx(NULL,nCode,wParam,lParam);
            }
            // Global hotkey: Pause to revert the last auto-correction (within a short window).
            if (k->vkCode==VK_PAUSE && (g_token_overflow || g_skip_until_boundary)) {
                return CallNextHookEx(NULL,nCode,wParam,lParam);
            }
            if (k->vkCode == VK_PAUSE && !(GetAsyncKeyState(VK_CONTROL)&0x8000) &&
                !(GetAsyncKeyState(VK_MENU)&0x8000) && !(GetAsyncKeyState(VK_LWIN)&0x8000) &&
                !(GetAsyncKeyState(VK_RWIN)&0x8000)) {
                const BOOL hadLastFix = g_last_fix.active;
                const BOOL wasCorrectedApplied = g_last_fix.corrected_applied ? TRUE : FALSE;
                const BOOL lastFixToEnglish = g_last_fix.corrected_to_english ? TRUE : FALSE;
                wchar_t lastFixOrig[TOKEN_MAX_CHARS + 1];
                wchar_t lastFixCorr[TOKEN_MAX_CHARS + 1];
                lastFixOrig[0] = 0;
                lastFixCorr[0] = 0;
                if (hadLastFix) {
                    StringCchCopyW(lastFixOrig, ARRAYSIZE(lastFixOrig), g_last_fix.original);
                    StringCchCopyW(lastFixCorr, ARRAYSIZE(lastFixCorr), g_last_fix.corrected);
                }

                BOOL didToggle = ToggleLastFixIfPossible();
                BOOL didFlip = FALSE;
                const BOOL usedToken = (g_token_len > 0);

                if (!didToggle) {
                    // No autocorrect fix to toggle: flip the last typed word to the other layout.
                    const ULONGLONG now = GetTickCount64();
                    if (usedToken) {
                        didFlip = FlipWordIfPossible(g_token, 0, FALSE);
                        if (didFlip) {
                            StringCchCopyW(g_token, ARRAYSIZE(g_token), g_last_fix.corrected);
                            g_token_len = g_last_fix.corrected_len;
                        }
                    } else if (g_last_word_len > 0 && g_last_word_had_boundary && (now - g_last_word_ts_ms) <= 30000) {
                        didFlip = FlipWordIfPossible(g_last_word, g_last_word_boundary, TRUE);
                    }
                }

                // Feed user feedback to the model (async).
                if (didToggle && hadLastFix) {
                    // If the user toggled from corrected->original, autocorrect was wrong (label=0).
                    // If the user toggled from original->corrected, autocorrect was desired (label=1).
                    (void)LiteRT_QueueSample(wasCorrectedApplied ? 0 : 1, lastFixOrig, lastFixCorr, lastFixToEnglish);
                } else if (didFlip && g_last_fix.active) {
                    (void)LiteRT_QueueSample(1, g_last_fix.original, g_last_fix.corrected, g_last_fix.corrected_to_english);
                }

                if (didToggle && !g_last_fix.had_boundary) {
                    const wchar_t* current=g_last_fix.corrected_applied?g_last_fix.corrected:g_last_fix.original;
                    StringCchCopyW(g_token,ARRAYSIZE(g_token),current); g_token_len=wcslen(g_token);
                }

                PauseLogItem* item = (PauseLogItem*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PauseLogItem));
                if (item) {
                    UtcNowIso(item->ts_utc, ARRAYSIZE(item->ts_utc));
                    GetForegroundInfo(item->window_title, ARRAYSIZE(item->window_title),
                                      item->process_path, ARRAYSIZE(item->process_path),
                                      item->layout_hex, ARRAYSIZE(item->layout_hex));

                    if (didToggle) {
                        if (hadLastFix) {
                            StringCchCopyW(item->original, ARRAYSIZE(item->original), g_last_fix.original);
                            StringCchCopyW(item->corrected, ARRAYSIZE(item->corrected), g_last_fix.corrected);
                            StringCchCopyW(item->action, ARRAYSIZE(item->action),
                                           wasCorrectedApplied ? L"pause_corrected_to_original" : L"pause_original_to_corrected");
                        } else {
                            StringCchCopyW(item->action, ARRAYSIZE(item->action), L"pause_toggle_no_fix");
                        }
                    } else if (didFlip) {
                        StringCchCopyW(item->original, ARRAYSIZE(item->original), g_last_fix.original);
                        StringCchCopyW(item->corrected, ARRAYSIZE(item->corrected), g_last_fix.corrected);
                        StringCchCopyW(item->action, ARRAYSIZE(item->action),
                                       usedToken ? L"pause_flip_current_word" : L"pause_flip_last_word");
                    } else {
                        StringCchCopyW(item->action, ARRAYSIZE(item->action), L"pause_no_word");
                    }

                    if (g_main_hwnd) {
                        PostMessageW(g_main_hwnd, WM_LOG_PAUSE, 0, (LPARAM)item);
                    } else {
                        HeapFree(GetProcessHeap(), 0, item);
                    }
                }

                if (didToggle || didFlip) return 1;
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            // Ignore shortcuts/modifiers.
            const BOOL ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const BOOL alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            const BOOL win = (GetAsyncKeyState(VK_LWIN)&0x8000) || (GetAsyncKeyState(VK_RWIN)&0x8000);
            if (ctrl || alt || win || k->vkCode==VK_CONTROL || k->vkCode==VK_LCONTROL ||
                k->vkCode==VK_RCONTROL || k->vkCode==VK_MENU || k->vkCode==VK_LMENU ||
                k->vkCode==VK_RMENU || k->vkCode==VK_LWIN || k->vkCode==VK_RWIN) {
                ResetTypingState();
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            if (k->vkCode == VK_BACK) {
                InvalidateLastFix();
                g_last_word_len=0;
                if (g_token_len > 0) {
                    g_token_len--;
                    g_token[g_token_len] = 0;
                }
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            if (k->vkCode == VK_ESCAPE) {
                ResetTypingState();
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }

            // Convert this keystroke to the actual character produced in the foreground layout.
            HKL hkl = GetForegroundKeyboardLayout();
            BYTE ks[256];
            ZeroMemory(ks, sizeof(ks));
            GetKeyboardState(ks);
            ks[VK_SHIFT]=(BYTE)((ks[VK_SHIFT]&1) | ((GetAsyncKeyState(VK_SHIFT)&0x8000)?0x80:0));
            ks[k->vkCode & 255] |= 0x80;

            wchar_t out[8];
            const UINT vk = (UINT)k->vkCode;
            const UINT sc = (UINT)k->scanCode;
            int rc = ToUnicodeEx(vk, sc, ks, out, (int)ARRAYSIZE(out), 4, hkl);
            if (rc == 1) {
                const wchar_t ch = out[0];
                if (g_skip_until_boundary) {
                    if (ch==L' ' || ch==L'\r' || ch==L'\n' || ch==L'\t') ResetTypingState();
                    return CallNextHookEx(NULL,nCode,wParam,lParam);
                }
                const BOOL layoutPunctuation=(PRIMARYLANGID(LOWORD((UINT_PTR)hkl))==LANG_ENGLISH &&
                                               ch && wcschr(L"[];',.`",ch)!=NULL);
                if (IsWordChar(ch) || layoutPunctuation) {
                    InvalidateLastFix();
                    g_last_word_len=0;
                    if (g_token_len < TOKEN_MAX_CHARS) {
                        g_token[g_token_len++] = ch;
                        g_token[g_token_len] = 0;
                    } else {
                        g_token_overflow=TRUE;
                    }
                } else {
                    if (ch!=L' ') {
                        ResetTypingState();
                        // Do not treat URL/path/identifier segments as standalone words.
                        if (ch!=L'\r' && ch!=L'\n' && ch!=L'\t') g_skip_until_boundary=TRUE;
                        return CallNextHookEx(NULL,nCode,wParam,lParam);
                    }
                    g_last_word_len=0;
                    if (g_token_len > 0 && !g_token_overflow && ch==L' ') {
                        RememberLastWord(g_token, g_token_len, ch, TRUE);
                    }
                    if (!g_token_overflow && g_token_len >= MIN_TOKEN_LEN && ch==L' ') {
                        // If we correct on a printable boundary, swallow the boundary keystroke
                        // and re-inject it after correction to keep order stable.
                        if (TryAutocorrectToken(g_token, ch, TRUE)) {
                            g_token_len = 0;
                            g_token[0] = 0;
                            g_token_overflow=FALSE;
                            g_swallow_vk_keyup = k->vkCode;
                            g_swallow_keyup = TRUE;
                            return 1;
                        }
                    }
                    InvalidateLastFix();
                    g_token_len = 0;
                    g_token[0] = 0;
                    g_token_overflow=FALSE;
                }
            } else {
                // Navigation, Enter and Tab must never edit text at a moved caret
                // or submit a rewritten command. Automatic correction is Space-only.
                ResetTypingState();
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
    g_mouse_hook=SetWindowsHookExW(WH_MOUSE_LL,LowLevelMouseProc,GetModuleHandleW(NULL),0);
    g_focus_hook=SetWinEventHook(EVENT_OBJECT_FOCUS,EVENT_OBJECT_FOCUS,NULL,TypingFocusChanged,0,0,WINEVENT_OUTOFCONTEXT);
    g_foreground_hook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,NULL,TypingFocusChanged,0,0,WINEVENT_OUTOFCONTEXT);
    if (!g_mouse_hook || !g_focus_hook || !g_foreground_hook) {
        // Fail closed if caret/focus tracking cannot be installed.
        g_enabled=FALSE;
    }
    return TRUE;
}

static void UninstallKeyboardHook(void)
{
    if (g_mouse_hook) { UnhookWindowsHookEx(g_mouse_hook); g_mouse_hook=NULL; }
    if (g_focus_hook) { UnhookWinEvent(g_focus_hook); g_focus_hook=NULL; }
    if (g_foreground_hook) { UnhookWinEvent(g_foreground_hook); g_foreground_hook=NULL; }
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

static void TrayRestore(HWND hwnd)
{
    if (TrayAdd(hwnd)) {
        KillTimer(hwnd, IDT_TRAY_RETRY);
    } else {
        // Explorer can broadcast before its notification area is ready.
        SetTimer(hwnd, IDT_TRAY_RETRY, 2000, NULL);
    }
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
    CheckMenuItem(g_tray_menu,IDM_TRAY_ENABLED,MF_BYCOMMAND | (g_enabled?MF_CHECKED:MF_UNCHECKED));

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

#include "learning_stats.h"

static void Cleanup(HWND hwnd)
{
    KillTimer(hwnd, IDT_TRAY_RETRY);
    if (g_stats_window) DestroyWindow(g_stats_window);
    UninstallKeyboardHook();
    LiteRT_Stop();
    free(g_language_model.words); g_language_model.words=NULL;
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
    if (g_taskbar_created && msg == g_taskbar_created) {
        // Explorer loses all notification icons when it restarts.
        TrayRemove();
        TrayRestore(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wParam == IDT_TRAY_RETRY) {
            if (!g_nid.cbSize) TrayRestore(hwnd);
            return 0;
        }
        break;
    case WM_CREATE: {
        g_main_hwnd = hwnd;
        g_tray_menu = CreatePopupMenu();
        if (g_tray_menu) {
            AppendMenuW(g_tray_menu, MF_STRING | MF_CHECKED, IDM_TRAY_ENABLED, L"Switching enabled");
            AppendMenuW(g_tray_menu, MF_STRING, IDM_TRAY_DIAGNOSTICS, L"Record local diagnostics (contains typed words)");
            AppendMenuW(g_tray_menu, MF_STRING, IDM_TRAY_OPEN_DATA, L"Open local learning folder");
            AppendMenuW(g_tray_menu, MF_STRING, IDM_TRAY_STATS, L"AI learning statistics");
            AppendMenuW(g_tray_menu, MF_STRING | MF_GRAYED, 0,
                        g_language_model.ready?L"Local AI ready":L"AI model missing: manual conversion only");
            AppendMenuW(g_tray_menu, MF_STRING, IDM_TRAY_EXPORT_DB, L"Export DB");
            AppendMenuW(g_tray_menu, MF_STRING, IDM_TRAY_EXPORT_MODEL_LOG, L"Export Model Log");
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
        if (id==IDM_TRAY_STATS) { ShowLearningStats(); return 0; }
        if (id==IDM_TRAY_ENABLED) {
            if (!g_enabled && (!g_mouse_hook || !g_focus_hook || !g_foreground_hook)) return 0;
            g_enabled=!g_enabled; ResetTypingState();
            CheckMenuItem(g_tray_menu,id,MF_BYCOMMAND | (g_enabled?MF_CHECKED:MF_UNCHECKED));
            return 0;
        }
        if (id==IDM_TRAY_DIAGNOSTICS) {
            g_diagnostics=!g_diagnostics;
            CheckMenuItem(g_tray_menu,id,MF_BYCOMMAND | (g_diagnostics?MF_CHECKED:MF_UNCHECKED));
            return 0;
        }
        if (id==IDM_TRAY_OPEN_DATA) {
            ShellExecuteW(hwnd,L"open",g_data_dir,NULL,NULL,SW_SHOWNORMAL); return 0;
        }
        if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDM_TRAY_EXPORT_DB) {
            ExportDbToTextFile(hwnd);
            return 0;
        }
        if (id == IDM_TRAY_EXPORT_MODEL_LOG) {
            ExportModelEventsToTextFile(hwnd);
            return 0;
        }
        break;
    }
    case WM_LOG_DECISION: {
        DecisionLog* item=(DecisionLog*)lParam;
        if (item) { WriteDecisionLog(item); HeapFree(GetProcessHeap(),0,item); }
        return 0;
    }
    case WM_LOG_PAUSE: {
        PauseLogItem* item = (PauseLogItem*)lParam;
        if (item) {
            DbInsertPauseEvent(item);
            HeapFree(GetProcessHeap(), 0, item);
        }
        return 0;
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

    BuildDbPath();
    if (g_data_dir[0] == 0) BuildDataDir();
    BuildMlPaths();
    DbInitIfNeeded();
    LiteRT_Start();

    const wchar_t* kClassName = L"DiSwitcherHiddenWindow";

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_taskbar_created) {
        ShowWin32ErrorBox(NULL, L"Failed to register taskbar recovery message.");
        return 1;
    }

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
    if (lpCmdLine && !_wcsicmp(lpCmdLine,L"--stats")) ShowLearningStats();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_stats_window && IsDialogMessageW(g_stats_window,&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
