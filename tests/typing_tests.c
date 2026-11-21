/* Exercise the application's actual keyboard callback with mocked Windows input.
   Never installs hooks, types into a real window, or opens the user's database. */
#include <windows.h>
#include <shellapi.h>
#include <assert.h>
#include <stdio.h>

static wchar_t next_char=0;
static int sent_calls=0, fail_input=0, control_down=0;
static UINT sent_count=0;
static INPUT sent_inputs[300];
static HWND fake_foreground=(HWND)1;
static HKL fake_layout=(HKL)0x0409;
static UINT WINAPI FakeSendInput(UINT count, LPINPUT input, int size)
{
    (void)size; ++sent_calls; sent_count=count;
    assert(count<=300); memcpy(sent_inputs,input,count*sizeof(INPUT));
    return fail_input?0:count;
}
static HWND WINAPI FakeForeground(void) { return fake_foreground; }
static DWORD WINAPI FakeWindowThread(HWND hwnd, LPDWORD pid)
{ (void)hwnd; if (pid) *pid=0; return 1; }
static BOOL WINAPI FakeGuiInfo(DWORD tid, PGUITHREADINFO info)
{ (void)tid; info->hwndFocus=NULL; return TRUE; }
static int WINAPI FakeLayouts(int count, HKL* out)
{ assert(count>=2); out[0]=(HKL)0x0409; out[1]=(HKL)0x0419; return 2; }
static HKL WINAPI FakeLayout(DWORD tid) { (void)tid; return fake_layout; }
static SHORT WINAPI FakeAsync(int key) { return (key==VK_CONTROL && control_down)?-32768:0; }
static BOOL WINAPI FakeKeyboard(PBYTE state) { memset(state,0,256); return TRUE; }
static int WINAPI FakeUnicode(UINT vk,UINT scan,const BYTE* state,LPWSTR out,int size,UINT flags,HKL hkl)
{ (void)vk;(void)scan;(void)state;(void)size;(void)flags;(void)hkl; out[0]=next_char; return next_char?1:0; }
static BOOL WINAPI FakePost(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{ (void)hwnd;(void)msg;(void)wp;(void)lp; return TRUE; }
static LRESULT WINAPI FakeNext(HHOOK hook,int code,WPARAM wp,LPARAM lp)
{ (void)hook;(void)code;(void)wp;(void)lp; return 0; }

static int tray_adds=0, tray_versions=0, tray_deletes=0, tray_fail=0, tray_timer=0;
static BOOL WINAPI FakeNotifyIcon(DWORD message, PNOTIFYICONDATAW icon)
{
    assert(icon->hWnd==(HWND)42 && icon->uID==1);
    if (message==NIM_ADD) {
        ++tray_adds;
        if (tray_fail) { SetLastError(ERROR_NOT_READY); return FALSE; }
    } else if (message==NIM_SETVERSION) {
        assert(icon->uVersion==NOTIFYICON_VERSION_4); ++tray_versions;
    } else if (message==NIM_DELETE) ++tray_deletes;
    return TRUE;
}
static UINT_PTR WINAPI FakeSetTimer(HWND hwnd,UINT_PTR id,UINT interval,TIMERPROC callback)
{ (void)hwnd;(void)interval;(void)callback; tray_timer=1; return id; }
static BOOL WINAPI FakeKillTimer(HWND hwnd,UINT_PTR id)
{ (void)hwnd;(void)id; tray_timer=0; return TRUE; }

#define SendInput FakeSendInput
#define GetForegroundWindow FakeForeground
#define GetWindowThreadProcessId FakeWindowThread
#define GetGUIThreadInfo FakeGuiInfo
#define GetKeyboardLayoutList FakeLayouts
#define GetKeyboardLayout FakeLayout
#define GetAsyncKeyState FakeAsync
#define GetKeyboardState FakeKeyboard
#define ToUnicodeEx FakeUnicode
#define PostMessageW FakePost
#define CallNextHookEx FakeNext
#define Shell_NotifyIconW FakeNotifyIcon
#define SetTimer FakeSetTimer
#define KillTimer FakeKillTimer
#include "../src/main.c"

static void test_tray_recovery(void)
{
    g_taskbar_created=0xC123;
    g_app_icon_small=(HICON)1;
    assert(TrayAdd((HWND)42));
    WndProc((HWND)42,g_taskbar_created,0,0);
    assert(tray_adds==2 && tray_deletes==1 && tray_versions==2 && !tray_timer);
    tray_fail=1;
    WndProc((HWND)42,g_taskbar_created,0,0);
    assert(!g_nid.cbSize && tray_timer);
    WndProc((HWND)42,WM_TIMER,IDT_TRAY_RETRY,0);
    assert(!g_nid.cbSize && tray_timer && tray_adds==4);
    tray_fail=0;
    WndProc((HWND)42,WM_TIMER,IDT_TRAY_RETRY,0);
    assert(g_nid.cbSize && !tray_timer && tray_versions==3);
    // A queued timer after success must not add a duplicate icon.
    WndProc((HWND)42,WM_TIMER,IDT_TRAY_RETRY,0);
    assert(tray_adds==5);
    TrayRemove(); g_app_icon_small=NULL;
}

static LRESULT key(DWORD vk,wchar_t ch)
{
    next_char=ch;
    KBDLLHOOKSTRUCT event={0}; event.vkCode=vk;
    return LowLevelKeyboardProc(HC_ACTION,WM_KEYDOWN,(LPARAM)&event);
}
static void type(const wchar_t* text)
{
    for (const wchar_t* p=text;*p;++p) key((DWORD)*p,*p);
}
static void fresh(void)
{
    ResetTypingState(); sent_calls=0; fail_input=0; control_down=0;
    fake_foreground=(HWND)1; g_target_window=fake_foreground; g_target_focus=NULL;
    g_exclusion_window=fake_foreground; g_excluded_window=FALSE; fake_layout=(HKL)0x0409;
    g_swallow_keyup=FALSE;
}

int main(int argc,char** argv)
{
    if (argc!=2) return 2;
    test_tray_recovery();
    FILE* f=NULL; fopen_s(&f,argv[1],"rb"); assert(f);
    assert(DSLoad(&g_language_model,f)); fclose(f);
    InitializeCriticalSection(&g_model_cs); g_model_cs_init=TRUE;
    InitializeCriticalSection(&g_ml_q_cs); g_ml_q_cs_init=TRUE;
    g_ml_event=CreateEventW(NULL,TRUE,FALSE,NULL); assert(g_ml_event);

    wchar_t capital[65];
    MapEnToRu(L"Ghbdtn",capital,65); assert(!wcscmp(capital,L"Привет"));
    MapRuToEn(L"Руддщ",capital,65); assert(!wcscmp(capital,L"Hello"));

    fresh(); type(L"ghbdtn"); key(VK_SHIFT,0);
    assert(g_token_len==6 && !sent_calls);
    assert(key(VK_SPACE,L' ')==1 && sent_calls==1 && g_last_fix.active);
    key(VK_PAUSE,0);
    assert(sent_calls==2 && !g_last_fix.corrected_applied);
    assert(g_ml_queue[0].label==0);
    key(VK_PAUSE,0); assert(sent_calls==3 && g_last_fix.corrected_applied);
    assert(g_ml_queue[1].label==1);

    fresh(); type(L"ghbdtn"); key(VK_LEFT,0); key(VK_SPACE,L' ');
    assert(!sent_calls && !g_last_fix.active);
    fresh(); type(L"ghbdtn"); key(VK_RETURN,L'\r'); key(VK_PAUSE,0);
    assert(!sent_calls && !g_last_word_len);
    fresh(); type(L"ghbdtn"); LowLevelMouseProc(HC_ACTION,WM_LBUTTONDOWN,0); key(VK_SPACE,L' ');
    assert(!sent_calls);
    fresh(); type(L"g"); TypingFocusChanged(NULL,EVENT_OBJECT_FOCUS,(HWND)99,0,0,0,0);
    type(L"hbdtn"); key(VK_SPACE,L' ');
    assert(sent_calls==1 && !wcscmp(g_last_fix.original,L"ghbdtn"));
    fresh(); fake_layout=(HKL)0x0419; type(L"р");
    TypingFocusChanged(NULL,EVENT_OBJECT_FOCUS,(HWND)99,0,0,0,0);
    type(L"уддщ"); key(VK_SPACE,L' ');
    assert(sent_calls==1 && !wcscmp(g_last_fix.corrected,L"hello"));
    fresh(); type(L"ghbdtn"); fake_foreground=(HWND)2;
    TypingFocusChanged(NULL,EVENT_SYSTEM_FOREGROUND,NULL,0,0,0,0); key(VK_SPACE,L' ');
    assert(!sent_calls);
    fresh(); type(L"ghbdtn"); fake_foreground=(HWND)2; key(VK_SPACE,L' ');
    assert(!sent_calls);
    fresh(); type(L"ghbdtn"); control_down=1; key(VK_PAUSE,0);
    assert(!sent_calls && !g_token_len);
    fresh(); type(L"https://ghbdtn "); assert(!sent_calls);
    fresh(); type(L"some_ghbdtn "); assert(!sent_calls);
    fresh(); g_excluded_window=TRUE; type(L"ghbdtn "); key(VK_PAUSE,0); assert(!sent_calls);
    fresh(); g_enabled=FALSE; type(L"ghbdtn "); key(VK_PAUSE,0); assert(!sent_calls); g_enabled=TRUE;
    fresh(); type(L"ghbdtn  "); sent_calls=0; key(VK_PAUSE,0); assert(!sent_calls);
    fresh(); for (int i=0;i<70;++i) key('A',L'a');
    key(VK_PAUSE,0); key(VK_SPACE,L' '); assert(!sent_calls);

    fresh(); type(L"ghbdtn"); fail_input=1; key(VK_SPACE,L' ');
    assert(sent_calls==1 && !g_last_fix.active);

    fresh(); type(L"ghbdtn"); key(VK_PAUSE,0);
    assert(!wcscmp(g_token,L"привет"));
    key(VK_PAUSE,0); assert(!wcscmp(g_token,L"ghbdtn"));
    key(VK_BACK,0); assert(!wcscmp(g_token,L"ghbdt"));

    fresh(); wchar_t longest[66];
    for (int i=0;i<65;++i) longest[i]=L'a'; longest[65]=0;
    assert(SendBackspacesAndText(65,longest));
    assert(sent_count==260); // No silent truncation of the final character.

    // Personal learning survives restart without touching the real user's store.
    wchar_t temporary[MAX_PATH]; assert(GetTempPathW(MAX_PATH,temporary));
    assert(GetTempFileNameW(temporary,L"dst",0,g_db_path));
    StringCchPrintfW(g_data_dir,MAX_PATH,L"%sDiSwitcher-test-no-exclusions",temporary);
    LocalModelStart(); assert(g_language_model.ready);
    DbInitIfNeeded();
    LiteRTSample sample={0};
    StringCchCopyW(sample.original,65,L"ghbdtn");
    StringCchCopyW(sample.corrected,65,L"привет"); sample.label=0;
    SaveLocalFeedback(&sample);
    LocalModelStart();
    DSDecision decision=DSDecide(&g_language_model,L"ghbdtn",L"привет");
    assert(!decision.apply && !wcscmp(decision.reason,L"personal_keep"));
    sample.label=1; SaveLocalFeedback(&sample); LocalModelStart();
    decision=DSDecide(&g_language_model,L"ghbdtn",L"привет");
    assert(decision.apply && g_language_model.feedback_count==1);
    DbInsertModelEvent(L"2026-11-02T00:00:00Z",L"local_feedback",1,FALSE,0,0,0,0,0,0,L"ghbdtn",L"привет");
    wchar_t stats[12000]; BuildLearningStats(stats,ARRAYSIZE(stats));
    assert(wcsstr(stats,L"Personal word preferences: 1 / 1024"));
    assert(wcsstr(stats,L"Feedback actions (local AI): 1"));
    assert(wcsstr(stats,L"2026-11-02T00:00:00Z"));
    assert(wcsstr(stats,L"ghbdtn  ->  привет"));
    assert(DeleteFileW(g_db_path));

    for (int i=0;i<1100;++i) {
        wchar_t original[65]=L"zzzaaa", target[65];
        original[3]=(wchar_t)(L'a'+i/(26*26)); original[4]=(wchar_t)(L'a'+(i/26)%26);
        original[5]=(wchar_t)(L'a'+i%26); MapEnToRu(original,target,65);
        DSRemember(&g_language_model,original,target,i%2);
    }
    DSRefit(&g_language_model);
    assert(g_language_model.feedback_count==1024);
    for (int i=0;i<8;++i) assert(fabsf(g_language_model.weights[i]-g_language_model.baseline[i])<=.25001f);
    assert(fabsf(g_language_model.bias-g_language_model.base_bias)<=.25001f);

    CloseHandle(g_ml_event); DeleteCriticalSection(&g_model_cs); DeleteCriticalSection(&g_ml_q_cs);
    free(g_language_model.words);
    puts("Tray recovery, keyboard state, undo, feedback, exclusions, overflow and injection tests passed.");
    return 0;
}
