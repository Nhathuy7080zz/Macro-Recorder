#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

HWND gHwndMain, gHwndList, gHwndStatus, gHwndHelp;
HWND gHwndLblDelay, gHwndTxtDelay, gHwndLblVal1, gHwndTxtVal1, gHwndLblVal2, gHwndTxtVal2;
HWND gHwndBtnSave, gHwndBtnDel, gHwndBtnExport, gHwndBtnImport, gHwndBtnLang;
int gSelectedEvent = -1;

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wParam, LPARAM lParam);

static const char* L(const char* en, const char* vi) { return gLang ? vi : en; }

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
    char buf[512], keyName[64];
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        unsigned long long tMs = (unsigned long long)(e->timeUs / 1000ULL);
        char actStr[64];
        if (e->type == EVENT_KEY) {
            UINT scan = MapVirtualKeyA(e->vk, MAPVK_VK_TO_VSC);
            if (scan == 0 || GetKeyNameTextA(scan << 16, keyName, sizeof(keyName)) == 0) sprintf(keyName, "Key %u", e->vk);
            sprintf(actStr, "%.20s | %s", keyName, e->down ? L("- Press", "\xe2\x86\x93 Nh\xef\xbf\xbdn") : L("^ Key Up", "\xe2\x86\x91 Nh\xef\xbf\xbd"));
            sprintf(buf, "[%zu] %s | %llu ms", i, actStr, tMs);
        } else if (e->type == EVENT_MOUSE_MOVE) {
            sprintf(buf, "[%zu] %s (X:%d Y:%d) | %llu ms", i, L("Mouse Move", "Di chu\xee\xbd\xbf\xbdn chu\xee\xbd\xbf\xbd"), e->x, e->y, tMs);
        } else if (e->type == EVENT_MOUSE_BUTTON) {
            const char* btn = L("Mouse Click", "Click chu\xee\xbd\xbf\xbd");
            if (e->mouseFlags & MOUSEEVENTF_LEFTDOWN) btn = L("Left \xe2\x86\x93", "Tr\xef\xbf\xbd \xe2\x86\x93");
            else if (e->mouseFlags & MOUSEEVENTF_LEFTUP) btn = L("Left \xe2\x86\x91", "Tr\xef\xbf\xbd \xe2\x86\x91");
            else if (e->mouseFlags & MOUSEEVENTF_RIGHTDOWN) btn = L("Right \xe2\x86\x93", "Ph\xef\xbf\xbd \xe2\x86\x93");
            else if (e->mouseFlags & MOUSEEVENTF_RIGHTUP) btn = L("Right \xe2\x86\x91", "Ph\xef\xbf\xbd \xe2\x86\x91");
            sprintf(buf, "[%zu] %s (X:%d Y:%d) | %llu ms", i, btn, e->x, e->y, tMs);
        } else if (e->type == EVENT_MOUSE_WHEEL) {
            sprintf(buf, "[%zu] %s | %llu ms", i, L("Mouse Wheel", "Cu\xee\xbd\xbf\xbdn chu\xee\xbd\xbf\xbd"), tMs);
        }
        LRESULT idx = SendMessageA(gHwndList, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessage(gHwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
}

static void UpdateUIStrings() {
    SetWindowTextA(gHwndMain, L("Macro Editor", "Tr\xef\xbf\xbdnh ch\xef\xbf\xbdnh s\xef\xbf\xbd Macro"));
    SetWindowTextA(gHwndLblDelay, L("Delay (ms):", "Th\xef\xbf\xbd gian tr\xef\xbf\xbd (ms):"));
    SetWindowTextA(gHwndBtnSave, L("Save Changes", "L\xef\xbf\xbd thay \xef\xbf\xbd\xef\xbf\xbd"));
    SetWindowTextA(gHwndBtnDel, L("Delete Action", "X\xef\xbf\xbd h\xef\xbf\xbdnh \xef\xbf\xbd\xef\xbf\xbdng"));
    SetWindowTextA(gHwndBtnExport, L("Export JSON", "Xu\xef\xbf\xbd file JSON"));
    SetWindowTextA(gHwndBtnImport, L("Import JSON", "Nh\xef\xbf\xbdp file JSON"));
    SetWindowTextA(gHwndBtnLang, L("EN / \xef\xbf\xbd", "VI / EN"));
    SetWindowTextA(gHwndHelp, L("Hotkeys:\nF1: Record   F2: Stop Rec\nF3: Play     F4: Stop Play", "Ph\xef\xbf\xbd t\xef\xbf\xbd:\nF1: Ghi l\xef\xbf\xbdn   F2: D\xef\xbf\xbdng ghi\nF3: Ph\xef\xbf\xbd     F4: D\xef\xbf\xbdng ph\xef\xbf\xbd"));
    if(gRecording) SetWindowTextA(gHwndStatus, L("Status: RECORDING (F2 to Stop)", "Tr\xef\xbf\xbdng th\xef\xbf\xbd: \xef\xbf\xbdNG GHI (F2 \xef\xbf\xbd d\xef\xbf\xbdng)"));
    else if(gPlaying) SetWindowTextA(gHwndStatus, L("Status: PLAYING (F4 to Stop)", "Tr\xef\xbf\xbdng th\xef\xbf\xbd: \xef\xbf\xbdNG PH\xef\xbf\xbd (F4 \xef\xbf\xbd d\xef\xbf\xbdng)"));
    else SetWindowTextA(gHwndStatus, L("Status: IDLE", "Tr\xef\xbf\xbdng th\xef\xbf\xbd: S\xef\xbf\xbdn s\xef\xbf\xbdng (IDLE)"));
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
    gKeyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandleA(NULL), 0);
    gMouseHook = SetWindowsHookExA(WH_MOUSE_LL, mouse_hook_proc, GetModuleHandleA(NULL), 0);
    UpdateUIStrings();
}

static int open_file_dialog(char* outPath) {
    OPENFILENAMEA ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gHwndMain;
    ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = outPath; outPath[0] = '\0';
    ofn.nMaxFile = 260;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    return GetOpenFileNameA(&ofn);
}
static int save_file_dialog(char* outPath) {
    OPENFILENAMEA ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gHwndMain;
    ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = outPath; outPath[0] = '\0';
    ofn.nMaxFile = 260;
    ofn.lpstrDefExt = "json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameA(&ofn);
}

static int save_macro(const char* path) {
    FILE* file = fopen(path, "w"); if (!file) return 0;
    fprintf(file, "[\n");
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        fprintf(file, "  {\n    \"timeUs\": %llu,\n    \"type\": %u,\n    \"vk\": %u,\n    \"mouseFlags\": %u,\n    \"mouseData\": %d,\n    \"x\": %d,\n    \"y\": %d,\n    \"down\": %u\n  }%s\n",
            (unsigned long long)e->timeUs, e->type, e->vk, e->mouseFlags, e->mouseData, e->x, e->y, e->down, (i == gEventCount-1) ? "" : ",");
    }
    fprintf(file, "]\n"); fclose(file); return 1;
}
static int load_macro(const char* path) {
    if (InterlockedCompareExchange(&gRecording, 0, 0) != 0 || InterlockedCompareExchange(&gPlaying, 0, 0) != 0) return 0;
    FILE* file = fopen(path, "r"); if (!file) return 0;
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
    input.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = flags; SendInput(1, &input, sizeof(input));
}

static DWORD WINAPI playback_thread(LPVOID unused) {
    (void)unused;
    if (gEventCount == 0) { InterlockedExchange(&gPlaying, 0); return 0; }
    InterlockedExchange(&gStopPlayback, 0);
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
    InterlockedExchange(&gPlaying, 0);
    PostMessage(gHwndMain, WM_APP + 1, 0, 0); // trigger UI update
    return 0;
}
static void start_playback(void) {
    if (InterlockedCompareExchange(&gPlaying, 1, 0) != 0) return;
    stop_recording(); InterlockedExchange(&gStopPlayback, 0);
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
            gFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            if (!gFont) gFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");
            gHwndStatus = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
            gHwndList = CreateWindowA("LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_HASSTRINGS, 0, 0, 0, 0, hwnd, (HMENU)IDC_LIST, NULL, NULL);
            gHwndLblDelay = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_DELAY, NULL, NULL);
            gHwndTxtDelay = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_DELAY, NULL, NULL);
            gHwndLblVal1 = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_VAL1, NULL, NULL);
            gHwndTxtVal1 = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_VAL1, NULL, NULL);
            gHwndLblVal2 = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_VAL2, NULL, NULL);
            gHwndTxtVal2 = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, hwnd, (HMENU)IDC_TXT_VAL2, NULL, NULL);
            gHwndBtnSave = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_SAVE, NULL, NULL);
            gHwndBtnDel = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_DEL, NULL, NULL);
            gHwndBtnExport = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_EXPORT, NULL, NULL);
            gHwndBtnImport = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_IMPORT, NULL, NULL);
            gHwndBtnLang = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_LANG, NULL, NULL);
            gHwndHelp = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_HELP, NULL, NULL);
            EnumChildWindows(hwnd, SetFontProc, (LPARAM)gFont);
            UpdateUIStrings();
            break;
            
        case WM_SIZE: {
            int w = LOWORD(lParam), h = HIWORD(lParam);
            int rW = 220; // right panel width
            int m = 10;   // margin
            int lW = w - rW - m * 3;
            if (lW < 200) lW = 200;
            MoveWindow(gHwndStatus, m, m, w - m*2, 20, TRUE);
            MoveWindow(gHwndList, m, 35, lW, h - 45, TRUE);
            int rx = m*2 + lW, ry = 35;
            MoveWindow(gHwndLblDelay, rx, ry, rW, 20, TRUE); ry += 20;
            MoveWindow(gHwndTxtDelay, rx, ry, rW, 24, TRUE); ry += 30;
            MoveWindow(gHwndLblVal1, rx, ry, rW/2 - 5, 20, TRUE);
            MoveWindow(gHwndLblVal2, rx + rW/2 + 5, ry, rW/2 - 5, 20, TRUE); ry += 20;
            MoveWindow(gHwndTxtVal1, rx, ry, rW/2 - 5, 24, TRUE);
            MoveWindow(gHwndTxtVal2, rx + rW/2 + 5, ry, rW/2 - 5, 24, TRUE); ry += 30;
            MoveWindow(gHwndBtnSave, rx, ry, rW, 30, TRUE); ry += 35;
            MoveWindow(gHwndBtnDel, rx, ry, rW, 30, TRUE); ry += 45;
            MoveWindow(gHwndBtnExport, rx, ry, rW/2 - 5, 30, TRUE);
            MoveWindow(gHwndBtnImport, rx + rW/2 + 5, ry, rW/2 - 5, 30, TRUE); ry += 35;
            MoveWindow(gHwndBtnLang, rx, ry, rW, 30, TRUE); ry += 40;
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
                        char buf[64]; sprintf(buf, "%llu", (unsigned long long)(e->timeUs / 1000ULL)); 
                        SetWindowTextA(gHwndTxtDelay, buf);
                        if (e->type == EVENT_KEY) {
                            SetWindowTextA(gHwndLblVal1, L("Key Code:", "M\xef\xbf\xbd ph\xef\xbf\xbd:"));
                            SetWindowTextA(gHwndLblVal2, L("State (1=Dn):", "H. \xef\xbf\xbd\xef\xbf\xbdng (1=Nh\xef\xbf\xbdn):"));
                            sprintf(buf, "%u", e->vk); SetWindowTextA(gHwndTxtVal1, buf);
                            sprintf(buf, "%u", e->down); SetWindowTextA(gHwndTxtVal2, buf);
                        } else {
                            SetWindowTextA(gHwndLblVal1, "X:"); SetWindowTextA(gHwndLblVal2, "Y:");
                            sprintf(buf, "%d", e->x); SetWindowTextA(gHwndTxtVal1, buf);
                            sprintf(buf, "%d", e->y); SetWindowTextA(gHwndTxtVal2, buf);
                        }
                    }
                }
            } else if (LOWORD(wParam) == IDC_BTN_LANG) {
                gLang = !gLang; UpdateUIStrings();
            } else if (LOWORD(wParam) == IDC_BTN_SAVE) {
                if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                    char buf[64]; MacroEvent* e = &gEvents[gSelectedEvent];
                    GetWindowTextA(gHwndTxtDelay, buf, sizeof(buf)); e->timeUs = strtoull(buf, NULL, 10) * 1000ULL;
                    if (e->type == EVENT_KEY) {
                        GetWindowTextA(gHwndTxtVal1, buf, sizeof(buf)); e->vk = (uint32_t)atoi(buf);
                        GetWindowTextA(gHwndTxtVal2, buf, sizeof(buf)); e->down = (uint8_t)atoi(buf);
                    } else if (e->type == EVENT_MOUSE_MOVE || e->type == EVENT_MOUSE_BUTTON) {
                        GetWindowTextA(gHwndTxtVal1, buf, sizeof(buf)); e->x = atoi(buf);
                        GetWindowTextA(gHwndTxtVal2, buf, sizeof(buf)); e->y = atoi(buf);
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
                char path[260]; if (save_file_dialog(path)) save_macro(path);
            } else if (LOWORD(wParam) == IDC_BTN_IMPORT) {
                char path[260]; if (open_file_dialog(path)) load_macro(path);
            }
            return 0;
        
        case WM_HOTKEY:
            switch (wParam) {
                case 1: start_recording(); break;
                case 2: stop_recording(); break;
                case 3: start_playback(); break;
                case 4: stop_playback(); UpdateUIStrings(); break;
            } return 0;
            
        case WM_APP + 1: UpdateUIStrings(); return 0; // Playback finished

        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)pCmdLine; (void)nCmdShow; QueryPerformanceFrequency(&gQpcFreq);
    WNDCLASSA wc; ZeroMemory(&wc, sizeof(wc)); wc.lpfnWndProc = WindowProc; wc.hInstance = hInstance; wc.lpszClassName = "MacroGUI";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc);
    gHwndMain = CreateWindowA("MacroGUI", "Macro Editor", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 720, 600, NULL, NULL, hInstance, NULL);
    RegisterHotKey(gHwndMain, 1, 0, VK_F1); RegisterHotKey(gHwndMain, 2, 0, VK_F2);
    RegisterHotKey(gHwndMain, 3, 0, VK_F3); RegisterHotKey(gHwndMain, 4, 0, VK_F4);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    stop_recording(); stop_playback();
    UnregisterHotKey(gHwndMain, 1); UnregisterHotKey(gHwndMain, 2); UnregisterHotKey(gHwndMain, 3); UnregisterHotKey(gHwndMain, 4);
    clear_events(); return 0;
}
