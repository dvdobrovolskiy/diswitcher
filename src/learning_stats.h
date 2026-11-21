/* Included by main.c after the model/SQLite helpers. All queries are local. */
static HWND g_stats_window=NULL, g_stats_text=NULL;
static HFONT g_stats_font=NULL, g_stats_heading_font=NULL;

static void BuildLearningStats(wchar_t* text, size_t capacity)
{
    wchar_t recent[4096]=L"", line[320];
    unsigned int words=0, preferences=0, keep=0, convert=0, modelId=0;
    float threshold=0, adjustment=0; BOOL ready=FALSE;
    if (g_model_cs_init) {
        EnterCriticalSection(&g_model_cs);
        ready=g_language_model.ready; words=g_language_model.count;
        preferences=g_language_model.feedback_count; modelId=g_language_model.fingerprint;
        threshold=g_language_model.threshold;
        adjustment=fabsf(g_language_model.bias-g_language_model.base_bias);
        for (int i=0;i<DS_FEATURES;++i) {
            float delta=fabsf(g_language_model.weights[i]-g_language_model.baseline[i]);
            if (delta>adjustment) adjustment=delta;
        }
        for (unsigned int i=0;i<preferences;++i) {
            if (g_language_model.feedback[i].label) ++convert; else ++keep;
        }
        for (unsigned int i=0;i<preferences && i<10;++i) {
            const DSFeedback* item=&g_language_model.feedback[preferences-1-i];
            if (item->label) StringCchPrintfW(line,ARRAYSIZE(line),L"  Convert  %s  ->  %s\r\n",item->original,item->target);
            else StringCchPrintfW(line,ARRAYSIZE(line),L"  Keep     %s\r\n",item->original);
            StringCchCatW(recent,ARRAYSIZE(recent),line);
        }
        LeaveCriticalSection(&g_model_cs);
    }
    sqlite3* db=NULL; sqlite3_stmt* st=NULL;
    sqlite3_int64 events=0, keepEvents=0, convertEvents=0, corrections=0;
    wchar_t last[64]=L"No feedback recorded yet"; BOOL history=FALSE;
    char pathUtf8[MAX_PATH*4];
    if (WideCharToMultiByte(CP_UTF8,0,g_db_path,-1,pathUtf8,sizeof(pathUtf8),NULL,NULL) &&
        sqlite3_open_v2(pathUtf8,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK) {
        sqlite3_busy_timeout(db,50);
        const char* sql=
            "SELECT COUNT(*),COALESCE(SUM(label=0),0),COALESCE(SUM(label=1),0),MAX(ts_utc),"
            "(SELECT COUNT(*) FROM pause_events WHERE action IN ('autocorrect_ru_to_en','autocorrect_en_to_ru')) "
            "FROM model_events WHERE event='local_feedback';";
        if (sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW) {
            history=TRUE; events=sqlite3_column_int64(st,0); keepEvents=sqlite3_column_int64(st,1);
            convertEvents=sqlite3_column_int64(st,2); corrections=sqlite3_column_int64(st,4);
            const wchar_t* stamp=(const wchar_t*)sqlite3_column_text16(st,3);
            if (stamp) StringCchCopyW(last,ARRAYSIZE(last),stamp);
        }
    }
    sqlite3_finalize(st); if (db) sqlite3_close(db);
    wchar_t stored[1024];
    if (history) StringCchPrintfW(stored,ARRAYSIZE(stored),
        L"LOCAL HISTORY\r\n"
        L"  Feedback actions (local AI): %lld\r\n"
        L"  Keep / convert feedback:    %lld / %lld\r\n"
        L"  Last feedback (UTC):        %s\r\n"
        L"  Recorded auto corrections:  %lld (all versions)\r\n",
        events,keepEvents,convertEvents,last,corrections);
    else StringCchCopyW(stored,ARRAYSIZE(stored),L"LOCAL HISTORY\r\n  History temporarily unavailable. Try Refresh.\r\n");

    StringCchPrintfW(text,capacity,
        L"LOCAL MODEL\r\n"
        L"  Status:                     %s\r\n"
        L"  Model ID:                   %08x\r\n"
        L"  Vocabulary entries:         %u\r\n"
        L"  Decision threshold:         %.3f (not an accuracy rating)\r\n\r\n"
        L"YOUR LEARNING\r\n"
        L"  Personal word preferences: %u / %u\r\n"
        L"  Keep original / convert:    %u / %u\r\n"
        L"  Largest model adjustment:  %.4f / 0.2500 limit\r\n"
        L"  Repeated feedback replaces a word's previous preference.\r\n\r\n"
        L"THIS SESSION (resets on restart)\r\n"
        L"  Candidates evaluated:       %llu\r\n"
        L"  Automatic corrections:      %llu\r\n"
        L"  Left unchanged / failed:    %llu\r\n"
        L"  Switching: %s    Diagnostics: %s\r\n\r\n"
        L"%s\r\n"
        L"RECENT WORD PREFERENCES (newest first)\r\n%s\r\n"
        L"HOW LEARNING WORKS\r\n"
        L"  Pause undo teaches Keep; conversion / redo teaches Convert.\r\n"
        L"  Your preferences persist across restarts and model updates.\r\n"
        L"  No undo does not mean a correction was correct. These counts\r\n"
        L"  cannot measure accuracy without separately reviewed examples.\r\n"
        L"  All learning stays on this PC. Nothing is uploaded.\r\n",
        ready?L"Ready - offline":L"Unavailable - manual conversion only",modelId,words,threshold,
        preferences,DS_PERSONAL_MAX,keep,convert,adjustment,
        g_session_candidates,g_session_corrections,g_session_candidates-g_session_corrections,
        g_enabled?L"on":L"off",g_diagnostics?L"on":L"off",stored,
        recent[0]?recent:L"  No personal preferences yet. Use Pause to teach the app.\r\n");
}

static void RefreshLearningStats(void)
{
    if (!g_stats_text) return;
    wchar_t text[12000],previous[12000]; BuildLearningStats(text,ARRAYSIZE(text));
    GetWindowTextW(g_stats_text,previous,ARRAYSIZE(previous));
    if (wcscmp(text,previous)) {
        LRESULT scroll=SendMessageW(g_stats_text,EM_GETFIRSTVISIBLELINE,0,0);
        DWORD start=0,end=0; SendMessageW(g_stats_text,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
        SetWindowTextW(g_stats_text,text);
        SendMessageW(g_stats_text,EM_SETSEL,start,end);
        SendMessageW(g_stats_text,EM_LINESCROLL,0,scroll);
    }
}

static LRESULT CALLBACK LearningStatsProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    switch (message) {
    case WM_CREATE: {
        HINSTANCE instance=GetModuleHandleW(NULL);
        HDC dc=GetDC(hwnd); int dpi=GetDeviceCaps(dc,LOGPIXELSY); ReleaseDC(hwnd,dc);
        g_stats_font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        g_stats_heading_font=CreateFontW(-MulDiv(18,dpi,72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        HWND heading=CreateWindowW(L"STATIC",L"AI learning statistics",WS_CHILD|WS_VISIBLE,20,16,500,36,hwnd,(HMENU)10,instance,NULL);
        SendMessageW(heading,WM_SETFONT,(WPARAM)g_stats_heading_font,TRUE);
        HWND subtitle=CreateWindowW(L"STATIC",L"Private to this PC  |  Refreshes every 5 seconds",WS_CHILD|WS_VISIBLE,22,55,640,25,hwnd,(HMENU)11,instance,NULL);
        SendMessageW(subtitle,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
        g_stats_text=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|WS_HSCROLL|
            ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL,20,88,700,500,hwnd,(HMENU)12,instance,NULL);
        SendMessageW(g_stats_text,WM_SETFONT,(WPARAM)g_stats_font,TRUE);
        HWND refresh=CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|WS_TABSTOP,20,600,100,32,hwnd,(HMENU)13,instance,NULL);
        HWND folder=CreateWindowW(L"BUTTON",L"Open learning folder",WS_CHILD|WS_VISIBLE|WS_TABSTOP,132,600,180,32,hwnd,(HMENU)14,instance,NULL);
        SendMessageW(refresh,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
        SendMessageW(folder,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
        RefreshLearningStats(); SetTimer(hwnd,1,5000,NULL); return 0;
    }
    case WM_SIZE: {
        int width=LOWORD(lp),height=HIWORD(lp);
        if (wp!=SIZE_MINIMIZED) {
            MoveWindow(g_stats_text,20,88,width-40,height-148,TRUE);
            MoveWindow(GetDlgItem(hwnd,13),20,height-48,100,30,TRUE);
            MoveWindow(GetDlgItem(hwnd,14),132,height-48,180,30,TRUE);
        }
        return 0;
    }
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize.x=620; ((MINMAXINFO*)lp)->ptMinTrackSize.y=440; return 0;
    case WM_TIMER: if (!IsIconic(hwnd)) RefreshLearningStats(); return 0;
    case WM_COMMAND:
        if (LOWORD(wp)==13) { RefreshLearningStats(); return 0; }
        if (LOWORD(wp)==14) { ShellExecuteW(hwnd,L"open",g_data_dir,NULL,NULL,SW_SHOWNORMAL); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(hwnd,1); g_stats_text=NULL; g_stats_window=NULL;
        DeleteObject(g_stats_font); DeleteObject(g_stats_heading_font);
        g_stats_font=NULL; g_stats_heading_font=NULL; return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

static void ShowLearningStats(void)
{
    if (!g_stats_window) {
        WNDCLASSW cls={0}; cls.lpfnWndProc=LearningStatsProc; cls.hInstance=GetModuleHandleW(NULL);
        cls.lpszClassName=L"DiSwitcherLearningStatistics"; cls.hCursor=LoadCursorW(NULL,IDC_ARROW);
        cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); cls.hIcon=g_app_icon_big;
        if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return;
        g_stats_window=CreateWindowExW(0,cls.lpszClassName,L"DiSwitcher - AI learning statistics",
            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,820,800,NULL,NULL,cls.hInstance,NULL);
    }
    if (g_stats_window) {
        ShowWindow(g_stats_window,IsIconic(g_stats_window)?SW_RESTORE:SW_SHOW);
        RefreshLearningStats(); SetForegroundWindow(g_stats_window);
    }
}
