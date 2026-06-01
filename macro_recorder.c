#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h> 
#pragma comment(linker, "/SUBSYSTEM:windows")

#pragma pack(push, 1)
typedef struct {
    uint64_t timeUs;
    uint32_t vk;
    uint32_t mouseFlags;
    int32_t mouseData;
    int32_t x;
    int32_t y;
    uint8_t type;
    uint8_t down;
} MacroEvent;
#pragma pack(pop)

enum { EVENT_KEY = 1, EVENT_MOUSE_MOVE = 2, EVENT_MOUSE_BUTTON = 3, EVENT_MOUSE_WHEEL = 4 };

static HHOOK gKeyboardHook = NULL;
static HHOOK gMouseHook = NULL;
static volatile LONG gRecording = 0;
static volatile LONG gPlaying = 0;
static volatile LONG gStopPlayback = 0;

static MacroEvent* gEvents = NULL;
static size_t gEventCount = 0;
static size_t gEventCapacity = 0;
static uint64_t gRecordStartUs = 0;
static int gLastMoveX = 0, gLastMoveY = 0, gHasLastMove = 0;
static LARGE_INTEGER gQpcFreq;

int gLang = 1; // 1 = VI, 0 = EN
HFONT gFont = NULL;
int gLoopCount = 1; // Mặc định chạy 1 lần

// Control IDs
#define IDC_LIST 1001
#define IDC_LBL_DELAY 1002
#define IDC_TXT_DELAY 1003
#define IDC_LBL_VAL1 1004
#define IDC_TXT_VAL1 1005
#define IDC_LBL_VAL2 1006
#define IDC_TXT_VAL2 1007
#define IDC_BTN_SAVE 1008
#define IDC_BTN_DEL 1009
#define IDC_BTN_EXPORT 1010
#define IDC_BTN_IMPORT 1011
#define IDC_BTN_LANG 1012
#define IDC_LBL_HELP 1013
#define IDC_STATUS 1014
#define IDC_LBL_LOOP 1015
#define IDC_TXT_LOOP 1016

HWND gHwndMain, gHwndList, gHwndStatus, gHwndHelp;
HWND gHwndLblDelay, gHwndTxtDelay, gHwndLblVal1, gHwndTxtVal1, gHwndLblVal2, gHwndTxtVal2;
HWND gHwndBtnSave, gHwndBtnDel, gHwndBtnExport, gHwndBtnImport, gHwndBtnLang;
HWND gHwndLblLoop, gHwndTxtLoop;
int gSelectedEvent = -1;

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wParam, LPARAM lParam);

static const wchar_t* L_str(const wchar_t* en, const wchar_t* vi) { return gLang ? vi : en; }

static uint64_t now_us(void) {
    LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000ULL) / (uint64_t)gQpcFreq.QuadPart);
}
static void sleep_until_us(uint64_t targetUs) {
    for (;;) {
        uint64_t currentUs = now_us();
        if (currentUs >= targetUs) break;
        uint64_t remainingUs = targetUs - currentUs;
        if (remainingUs > 2000) Sleep((DWORD)((remainingUs - 1000) / 1000)); else SwitchToThread();
    }
}
static int append_event(const MacroEvent* event) {
    if (gEventCount == gEventCapacity) {
        size_t newC = gEventCapacity == 0 ? 256 : gEventCapacity * 2;
        MacroEvent* nE = (MacroEvent*)realloc(gEvents, newC * sizeof(MacroEvent));
        if (!nE) return 0;
        gEvents = nE; gEventCapacity = newC;
    }
    gEvents[gEventCount++] = *event;
    return 1;
}
static void clear_events(void) { free(gEvents); gEvents = NULL; gEventCount = 0; gEventCapacity = 0; }
static void stop_playback(void) { InterlockedExchange(&gStopPlayback, 1); }

void RefreshList(void) {
    SendMessage(gHwndList, LB_RESETCONTENT, 0, 0);
    wchar_t buf[512], keyName[64];
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        unsigned long long tMs = (unsigned long long)(e->timeUs / 1000ULL);
        if (e->type == EVENT_KEY) {
            UINT scan = MapVirtualKeyW(e->vk, MAPVK_VK_TO_VSC);
            if (scan == 0 || GetKeyNameTextW(scan << 16, keyName, sizeof(keyName)/sizeof(wchar_t)) == 0) swprintf(keyName, 64, L"Key %u", e->vk);
            swprintf(buf, 512, L"[%zu] %.20s | %s | %llu ms", i, keyName, e->down ? L_str(L"- Press", L"\x2193 Nhấn") : L_str(L"^ Key Up", L"\x2191 Nhả"), tMs);
        } else if (e->type == EVENT_MOUSE_MOVE) {
            swprintf(buf, 512, L"[%zu] %s (X:%d Y:%d) | %llu ms", i, L_str(L"Mouse Move", L"Di chuyển chuột"), e->x, e->y, tMs);
        } else if (e->type == EVENT_MOUSE_BUTTON) {
            const wchar_t* btn = L_str(L"Mouse Click", L"Click chuột");
            if (e->mouseFlags & MOUSEEVENTF_LEFTDOWN) btn = L_str(L"Left \x2193", L"Trái \x2193");
            else if (e->mouseFlags & MOUSEEVENTF_LEFTUP) btn = L_str(L"Left \x2191", L"Trái \x2191");
            else if (e->mouseFlags & MOUSEEVENTF_RIGHTDOWN) btn = L_str(L"Right \x2193", L"Phải \x2193");
            else if (e->mouseFlags & MOUSEEVENTF_RIGHTUP) btn = L_str(L"Right \x2191", L"Phải \x2191");
            swprintf(buf, 512, L"[%zu] %s (X:%d Y:%d) | %llu ms", i, btn, e->x, e->y, tMs);
        } else if (e->type == EVENT_MOUSE_WHEEL) {
            swprintf(buf, 512, L"[%zu] %s | %llu ms", i, L_str(L"Mouse Wheel", L"Cuộn chuột"), tMs);
        }
        LRESULT idx = SendMessageW(gHwndList, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessage(gHwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
}

static void UpdateUIStrings() {
    SetWindowTextW(gHwndMain, L_str(L"Macro Editor", L"Trình chỉnh sửa Macro"));
    SetWindowTextW(gHwndLblDelay, L_str(L"Delay (ms):", L"Thời gian trễ (ms):"));
    SetWindowTextW(gHwndBtnSave, L_str(L"Save Changes", L"Lưu thay đổi"));
    SetWindowTextW(gHwndBtnDel, L_str(L"Delete Action", L"Xóa hành động"));
    SetWindowTextW(gHwndBtnExport, L_str(L"Export JSON", L"Xuất JSON"));
    SetWindowTextW(gHwndBtnImport, L_str(L"Import JSON", L"Nhập JSON"));
    SetWindowTextW(gHwndBtnLang, L_str(L"EN / VI", L"VI / EN"));
    SetWindowTextW(gHwndLblLoop, L_str(L"Loop (0 = Infinite):", L"Số lần lặp (0 = Vô hạn):"));
    SetWindowTextW(gHwndHelp, L_str(L"Hotkeys:\nF1: Record   F2: Stop Rec\nF3: Play     F4: Stop Play", L"Phím tắt:\nF1: Ghi lệnh   F2: Dừng ghi\nF3: Phát       F4: Dừng phát"));
    
    if(gRecording) SetWindowTextW(gHwndStatus, L_str(L"Status: RECORDING (F2 to Stop)", L"Trạng thái: ĐANG GHI (F2 để dừng)"));
    else if(gPlaying) SetWindowTextW(gHwndStatus, L_str(L"Status: PLAYING (F4 to Stop)", L"Trạng thái: ĐANG PHÁT (F4 để dừng)"));
    else SetWindowTextW(gHwndStatus, L_str(L"Status: IDLE", L"Trạng thái: Sẵn sàng"));
    RefreshList();
}

static void stop_recording(void) {
    if (InterlockedExchange(&gRecording, 0) == 1) {
        UnhookWindowsHookEx(gKeyboardHook); UnhookWindowsHookEx(gMouseHook);
        gKeyboardHook = NULL; gMouseHook = NULL;
        UpdateUIStrings();
    }
}
static void start_recording(void) {
    if (InterlockedCompareExchange(&gRecording, 1, 0) != 0) return;
    if (InterlockedCompareExchange(&gPlaying, 0, 0) != 0) {
        InterlockedExchange(&gRecording, 0); return; 
    }
    stop_playback(); clear_events(); RefreshList();
    gRecordStartUs = now_us(); gHasLastMove = 0;
    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandle(NULL), 0);
    gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc, GetModuleHandle(NULL), 0);
    UpdateUIStrings();
}

static int open_file_dialog(wchar_t* outPath) {
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gHwndMain;
    ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = outPath; outPath[0] = L'\0';
    ofn.nMaxFile = 260;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&ofn);
}
static int save_file_dialog(wchar_t* outPath) {
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gHwndMain;
    ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = outPath; outPath[0] = L'\0';
    ofn.nMaxFile = 260;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn);
}

static int save_macro(const wchar_t* path) {
    FILE* file = _wfopen(path, L"w"); if (!file) return 0;
    fprintf(file, "[\n");
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        fprintf(file, "  {\n    \"timeUs\": %llu,\n    \"type\": %u,\n    \"vk\": %u,\n    \"mouseFlags\": %u,\n    \"mouseData\": %d,\n    \"x\": %d,\n    \"y\": %d,\n    \"down\": %u\n  }%s\n",
            (unsigned long long)e->timeUs, e->type, e->vk, e->mouseFlags, e->mouseData, e->x, e->y, e->down, (i == gEventCount-1) ? "" : ",");
    }
    fprintf(file, "]\n"); fclose(file); return 1;
}
static int load_macro(const wchar_t* path) {
    if (InterlockedCompareExchange(&gRecording, 0, 0) != 0 || InterlockedCompareExchange(&gPlaying, 0, 0) != 0) return 0;
    FILE* file = _wfopen(path, L"r"); if (!file) return 0;
    fseek(file, 0, SEEK_END); long len = ftell(file); fseek(file, 0, SEEK_SET);
    if (len <= 0) { fclose(file); return 0; }
    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(file); return 0; }
    fread(buf, 1, len, file); buf[len] = '\0'; fclose(file);
    clear_events();
    char* ptr = buf;
    while ((ptr = strstr(ptr, "{")) != NULL) {
        char* end = strstr(ptr, "}"); if (!end) break; *end = '\0';
        MacroEvent e; ZeroMemory(&e, sizeof(e)); char* p;
        if ((p = strstr(ptr, "\"timeUs\""))) { p=strchr(p,':'); if(p) e.timeUs=strtoull(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"type\""))) { p=strchr(p,':'); if(p) e.type=(uint8_t)strtoul(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"vk\""))) { p=strchr(p,':'); if(p) e.vk=(uint32_t)strtoul(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"mouseFlags\""))) { p=strchr(p,':'); if(p) e.mouseFlags=(uint32_t)strtoul(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"mouseData\""))) { p=strchr(p,':'); if(p) e.mouseData=(int32_t)strtol(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"x\""))) { p=strchr(p,':'); if(p) e.x=(int32_t)strtol(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"y\""))) { p=strchr(p,':'); if(p) e.y=(int32_t)strtol(p+1,NULL,10); }
        if ((p = strstr(ptr, "\"down\""))) { p=strchr(p,':'); if(p) e.down=(uint8_t)strtoul(p+1,NULL,10); }
        append_event(&e); *end = '}'; ptr = end + 1;
    }
    free(buf); RefreshList(); return 1;
}

static void send_mouse_button(DWORD flags, int x, int y, DWORD data) {
    INPUT input; ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE; input.mi.dx = x; input.mi.dy = y;
    input.mi.dwFlags = flags; input.mi.mouseData = data; SendInput(1, &input, sizeof(input));
}
static void send_key(uint32_t vk, DWORD flags) {
    INPUT input; ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD; input.ki.wVk = (WORD)vk;
    input.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = flags; SendInput(1, &input, sizeof(input));
}

static DWORD WINAPI playback_thread(LPVOID unused) {
    (void)unused;
    if (gEventCount == 0) { InterlockedExchange(&gPlaying, 0); return 0; }
    InterlockedExchange(&gStopPlayback, 0);
    
    int currentLoop = 0;
    while (gLoopCount == 0 || currentLoop < gLoopCount) {
        if (InterlockedCompareExchange(&gStopPlayback, 0, 0) != 0) break;
        uint64_t playStartUs = now_us();
        
        for (size_t i = 0; i < gEventCount; ++i) {
            if (InterlockedCompareExchange(&gStopPlayback, 0, 0) != 0) break;
            const MacroEvent* event = &gEvents[i];
            sleep_until_us(playStartUs + event->timeUs);
            if (InterlockedCompareExchange(&gStopPlayback, 0, 0) != 0) break;
            
            switch (event->type) {
                case EVENT_KEY: send_key(event->vk, event->down ? 0 : KEYEVENTF_KEYUP); break;
                case EVENT_MOUSE_MOVE: SetCursorPos(event->x, event->y); break;
                case EVENT_MOUSE_BUTTON: SetCursorPos(event->x, event->y); send_mouse_button(event->mouseFlags, event->x, event->y, (DWORD)event->mouseData); break;
                case EVENT_MOUSE_WHEEL: SetCursorPos(event->x, event->y); send_mouse_button(event->mouseFlags, event->x, event->y, (DWORD)event->mouseData); break;
            }
        }
        currentLoop++;
    }
    
    InterlockedExchange(&gPlaying, 0);
    PostMessage(gHwndMain, WM_APP + 1, 0, 0); // Trigger UI update
    return 0;
}

static void start_playback(void) {
    if (InterlockedCompareExchange(&gPlaying, 1, 0) != 0) return;
    stop_recording(); InterlockedExchange(&gStopPlayback, 0);
    
    wchar_t buf[64];
    GetWindowTextW(gHwndTxtLoop, buf, 64);
    gLoopCount = _wtoi(buf);
    if (gLoopCount < 0) gLoopCount = 1;

    UpdateUIStrings();
    HANDLE thread = CreateThread(NULL, 0, playback_thread, NULL, 0, NULL);
    if (!thread) InterlockedExchange(&gPlaying, 0);
    else CloseHandle(thread);
}

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && InterlockedCompareExchange(&gRecording, 0, 0) != 0) {
        const KBDLLHOOKSTRUCT* data = (const KBDLLHOOKSTRUCT*)lParam;
        if ((data->flags & LLKHF_INJECTED) == 0 && data->vkCode != VK_F1 && data->vkCode != VK_F2 && data->vkCode != VK_F3 && data->vkCode != VK_F4) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                MacroEvent event; ZeroMemory(&event, sizeof(event));
                event.timeUs = now_us() - gRecordStartUs;
                event.type = EVENT_KEY; event.vk = data->vkCode;
                event.down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? 1 : 0;
                append_event(&event);
            }
        }
    }
    return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
}

static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && InterlockedCompareExchange(&gRecording, 0, 0) != 0) {
        const MSLLHOOKSTRUCT* data = (const MSLLHOOKSTRUCT*)lParam;
        if ((data->flags & LLMHF_INJECTED) == 0) {
            MacroEvent event; ZeroMemory(&event, sizeof(event));
            event.timeUs = now_us() - gRecordStartUs;
            event.x = data->pt.x; event.y = data->pt.y;
            switch (wParam) {
                case WM_MOUSEMOVE:
                    if (!gHasLastMove || gLastMoveX != data->pt.x || gLastMoveY != data->pt.y) {
                        event.type = EVENT_MOUSE_MOVE;
                        if (append_event(&event)) { gLastMoveX = data->pt.x; gLastMoveY = data->pt.y; gHasLastMove = 1; }
                    }
                    break;
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                    event.type = EVENT_MOUSE_BUTTON;
                    if (wParam == WM_LBUTTONDOWN) event.mouseFlags = MOUSEEVENTF_LEFTDOWN;
                    else if (wParam == WM_LBUTTONUP) event.mouseFlags = MOUSEEVENTF_LEFTUP;
                    else if (wParam == WM_RBUTTONDOWN) event.mouseFlags = MOUSEEVENTF_RIGHTDOWN;
                    else if (wParam == WM_RBUTTONUP) event.mouseFlags = MOUSEEVENTF_RIGHTUP;
                    else if (wParam == WM_MBUTTONDOWN) event.mouseFlags = MOUSEEVENTF_MIDDLEDOWN;
                    else if (wParam == WM_MBUTTONUP) event.mouseFlags = MOUSEEVENTF_MIDDLEUP;
                    else if (wParam == WM_XBUTTONDOWN) { event.mouseFlags = MOUSEEVENTF_XDOWN; event.mouseData = (HIWORD(data->mouseData) == XBUTTON1) ? XBUTTON1 : XBUTTON2; } 
                    else { event.mouseFlags = MOUSEEVENTF_XUP; event.mouseData = (HIWORD(data->mouseData) == XBUTTON1) ? XBUTTON1 : XBUTTON2; }
                    append_event(&event); break;
                case WM_MOUSEWHEEL:
                    event.type = EVENT_MOUSE_WHEEL; event.mouseFlags = MOUSEEVENTF_WHEEL;
                    event.mouseData = (short)HIWORD(data->mouseData); append_event(&event); break;
            }
        }
    }
    return CallNextHookEx(gMouseHook, code, wParam, lParam);
}

BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam) { SendMessage(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE); return TRUE; }

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            gFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            
            gHwndStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
            gHwndList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_HASSTRINGS, 0, 0, 0, 0, hwnd, (HMENU)IDC_LIST, NULL, NULL);
            
            // Loop controls
            gHwndLblLoop = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_LOOP, NULL, NULL);
            gHwndTxtLoop = CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_LOOP, NULL, NULL);

            // Edit controls
            gHwndLblDelay = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_DELAY, NULL, NULL);
            gHwndTxtDelay = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_DELAY, NULL, NULL);
            gHwndLblVal1 = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_VAL1, NULL, NULL);
            gHwndTxtVal1 = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_VAL1, NULL, NULL);
            gHwndLblVal2 = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_VAL2, NULL, NULL);
            gHwndTxtVal2 = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_VAL2, NULL, NULL);
            
            // Buttons
            gHwndBtnSave = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_SAVE, NULL, NULL);
            gHwndBtnDel = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_DEL, NULL, NULL);
            gHwndBtnExport = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_EXPORT, NULL, NULL);
            gHwndBtnImport = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_IMPORT, NULL, NULL);
            gHwndBtnLang = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_LANG, NULL, NULL);
            gHwndHelp = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_HELP, NULL, NULL);
            
            EnumChildWindows(hwnd, SetFontProc, (LPARAM)gFont);
            UpdateUIStrings();
            break;
            
        case WM_SIZE: {
            int w = LOWORD(lParam), h = HIWORD(lParam);
            int rW = 220; 
            int m = 10;   
            int lW = w - rW - m * 3;
            if (lW < 200) lW = 200;

            MoveWindow(gHwndStatus, m, m, w - m*2, 20, TRUE);
            MoveWindow(gHwndList, m, 35, lW, h - 45, TRUE);
            
            int rx = m*2 + lW, ry = 35;
            
            // Loop section
            MoveWindow(gHwndLblLoop, rx, ry, rW, 20, TRUE); ry += 20;
            MoveWindow(gHwndTxtLoop, rx, ry, rW, 24, TRUE); ry += 35;

            // Edit section
            MoveWindow(gHwndLblDelay, rx, ry, rW, 20, TRUE); ry += 20;
            MoveWindow(gHwndTxtDelay, rx, ry, rW, 24, TRUE); ry += 30;
            MoveWindow(gHwndLblVal1, rx, ry, rW/2 - 5, 20, TRUE);
            MoveWindow(gHwndLblVal2, rx + rW/2 + 5, ry, rW/2 - 5, 20, TRUE); ry += 20;
            MoveWindow(gHwndTxtVal1, rx, ry, rW/2 - 5, 24, TRUE);
            MoveWindow(gHwndTxtVal2, rx + rW/2 + 5, ry, rW/2 - 5, 24, TRUE); ry += 35;
            
            // Action buttons
            MoveWindow(gHwndBtnSave, rx, ry, rW, 30, TRUE); ry += 35;
            MoveWindow(gHwndBtnDel, rx, ry, rW, 30, TRUE); ry += 45;
            MoveWindow(gHwndBtnExport, rx, ry, rW/2 - 5, 30, TRUE);
            MoveWindow(gHwndBtnImport, rx + rW/2 + 5, ry, rW/2 - 5, 30, TRUE); ry += 35;
            MoveWindow(gHwndBtnLang, rx, ry, rW, 30, TRUE); ry += 40;
            
            // Help
            MoveWindow(gHwndHelp, rx, ry, rW, 60, TRUE);
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
                LRESULT sel = SendMessage(gHwndList, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    gSelectedEvent = (int)SendMessage(gHwndList, LB_GETITEMDATA, sel, 0);
                    if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                        MacroEvent* e = &gEvents[gSelectedEvent];
                        wchar_t buf[64]; swprintf(buf, 64, L"%llu", (unsigned long long)(e->timeUs / 1000ULL)); 
                        SetWindowTextW(gHwndTxtDelay, buf);
                        if (e->type == EVENT_KEY) {
                            SetWindowTextW(gHwndLblVal1, L_str(L"Key Code:", L"Mã phím:"));
                            SetWindowTextW(gHwndLblVal2, L_str(L"State(1=Dn):", L"Tr.thái (1=Nhấn):"));
                            swprintf(buf, 64, L"%u", e->vk); SetWindowTextW(gHwndTxtVal1, buf);
                            swprintf(buf, 64, L"%u", e->down); SetWindowTextW(gHwndTxtVal2, buf);
                        } else {
                            SetWindowTextW(gHwndLblVal1, L"X:"); SetWindowTextW(gHwndLblVal2, L"Y:");
                            swprintf(buf, 64, L"%d", e->x); SetWindowTextW(gHwndTxtVal1, buf);
                            swprintf(buf, 64, L"%d", e->y); SetWindowTextW(gHwndTxtVal2, buf);
                        }
                    }
                }
            } else if (LOWORD(wParam) == IDC_BTN_LANG) {
                gLang = !gLang; UpdateUIStrings();
            } else if (LOWORD(wParam) == IDC_BTN_SAVE) {
                if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                    wchar_t buf[64]; MacroEvent* e = &gEvents[gSelectedEvent];
                    GetWindowTextW(gHwndTxtDelay, buf, 64); e->timeUs = wcstoull(buf, NULL, 10) * 1000ULL;
                    if (e->type == EVENT_KEY) {
                        GetWindowTextW(gHwndTxtVal1, buf, 64); e->vk = (uint32_t)_wtoi(buf);
                        GetWindowTextW(gHwndTxtVal2, buf, 64); e->down = (uint8_t)_wtoi(buf);
                    } else if (e->type == EVENT_MOUSE_MOVE || e->type == EVENT_MOUSE_BUTTON) {
                        GetWindowTextW(gHwndTxtVal1, buf, 64); e->x = _wtoi(buf);
                        GetWindowTextW(gHwndTxtVal2, buf, 64); e->y = _wtoi(buf);
                    }
                    int prevSel = (int)SendMessage(gHwndList, LB_GETCURSEL, 0, 0);
                    RefreshList(); SendMessage(gHwndList, LB_SETCURSEL, prevSel, 0);
                }
            } else if (LOWORD(wParam) == IDC_BTN_DEL) {
                if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                    for (size_t i = gSelectedEvent; i < gEventCount - 1; i++) gEvents[i] = gEvents[i+1];
                    gEventCount--; gSelectedEvent = -1; RefreshList();
                }
            } else if (LOWORD(wParam) == IDC_BTN_EXPORT) {
                wchar_t path[260]; if (save_file_dialog(path)) save_macro(path);
            } else if (LOWORD(wParam) == IDC_BTN_IMPORT) {
                wchar_t path[260]; if (open_file_dialog(path)) load_macro(path);
            }
            return 0;
        
        case WM_HOTKEY:
            switch (wParam) {
                case 1: start_recording(); break;
                case 2: stop_recording(); break;
                case 3: start_playback(); break;
                case 4: stop_playback(); UpdateUIStrings(); break;
            } return 0;
            
        case WM_APP + 1: UpdateUIStrings(); return 0;

        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)pCmdLine; (void)nCmdShow; QueryPerformanceFrequency(&gQpcFreq);
    
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc)); 
    wc.lpfnWndProc = WindowProc; 
    wc.hInstance = hInstance; 
    wc.lpszClassName = L"MacroGUI";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); 
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); 
    RegisterClassW(&wc);
    
    gHwndMain = CreateWindowW(L"MacroGUI", L"Macro Editor", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 720, 600, NULL, NULL, hInstance, NULL);
    
    RegisterHotKey(gHwndMain, 1, 0, VK_F1); RegisterHotKey(gHwndMain, 2, 0, VK_F2);
    RegisterHotKey(gHwndMain, 3, 0, VK_F3); RegisterHotKey(gHwndMain, 4, 0, VK_F4);
    
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    
    stop_recording(); stop_playback();
    UnregisterHotKey(gHwndMain, 1); UnregisterHotKey(gHwndMain, 2); 
    UnregisterHotKey(gHwndMain, 3); UnregisterHotKey(gHwndMain, 4);
    clear_events(); return 0;
}