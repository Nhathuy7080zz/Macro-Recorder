#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t count;
} MacroHeader;

typedef struct {
    uint64_t timeUs;
    uint32_t vk;
    uint32_t mouseFlags;
    int32_t mouseData;
    int32_t x;
    int32_t y;
    uint8_t type;
    uint8_t down;
    uint16_t reserved;
} MacroEvent;
#pragma pack(pop)

enum {
    EVENT_KEY = 1,
    EVENT_MOUSE_MOVE = 2,
    EVENT_MOUSE_BUTTON = 3,
    EVENT_MOUSE_WHEEL = 4
};

static HHOOK gKeyboardHook = NULL;
static HHOOK gMouseHook = NULL;
static volatile LONG gRecording = 0;
static volatile LONG gPlaying = 0;
static volatile LONG gStopPlayback = 0;

static MacroEvent* gEvents = NULL;
static size_t gEventCount = 0;
static size_t gEventCapacity = 0;
static uint64_t gRecordStartUs = 0;
static int gLastMoveX = 0;
static int gLastMoveY = 0;
static int gHasLastMove = 0;

static LARGE_INTEGER gQpcFreq;

// GUI Globals
HWND gHwndMain, gHwndList, gHwndTime, gHwndP1, gHwndP2, gHwndUpdate, gHwndDelete;
int gSelectedEvent = -1;

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wParam, LPARAM lParam);

static uint64_t now_us(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000ULL) / (uint64_t)gQpcFreq.QuadPart);
}

static void sleep_until_us(uint64_t targetUs) {
    for (;;) {
        uint64_t currentUs = now_us();
        if (currentUs >= targetUs) {
            break;
        }
        uint64_t remainingUs = targetUs - currentUs;
        if (remainingUs > 2000) {
            Sleep((DWORD)((remainingUs - 1000) / 1000));
        } else {
            SwitchToThread();
        }
    }
}

static int append_event(const MacroEvent* event) {
    if (gEventCount == gEventCapacity) {
        size_t newCapacity = gEventCapacity == 0 ? 256 : gEventCapacity * 2;
        MacroEvent* newEvents = (MacroEvent*)realloc(gEvents, newCapacity * sizeof(MacroEvent));
        if (!newEvents) {
            return 0;
        }
        gEvents = newEvents;
        gEventCapacity = newCapacity;
    }
    gEvents[gEventCount++] = *event;
    return 1;
}

static void clear_events(void) {
    free(gEvents);
    gEvents = NULL;
    gEventCount = 0;
    gEventCapacity = 0;
}

static void stop_playback(void) {
    InterlockedExchange(&gStopPlayback, 1);
}

void RefreshList(void) {
    SendMessage(gHwndList, LB_RESETCONTENT, 0, 0);
    char buf[256];
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        if (e->type == EVENT_KEY) {
            sprintf(buf, "[%zu] %llu ms - KEY vk:%u %s", i, (unsigned long long)(e->timeUs / 1000ULL), e->vk, e->down ? "DOWN" : "UP");
        } else if (e->type == EVENT_MOUSE_MOVE) {
            sprintf(buf, "[%zu] %llu ms - MOVE x:%d y:%d", i, (unsigned long long)(e->timeUs / 1000ULL), e->x, e->y);
        } else if (e->type == EVENT_MOUSE_BUTTON) {
            sprintf(buf, "[%zu] %llu ms - MBTN flag:%u data:%d", i, (unsigned long long)(e->timeUs / 1000ULL), e->mouseFlags, e->mouseData);
        } else if (e->type == EVENT_MOUSE_WHEEL) {
            sprintf(buf, "[%zu] %llu ms - WHL flag:%u data:%d", i, (unsigned long long)(e->timeUs / 1000ULL), e->mouseFlags, e->mouseData);
        } else {
            sprintf(buf, "[%zu] %llu ms - UNKNOWN", i, (unsigned long long)(e->timeUs / 1000ULL));
        }

        LRESULT idx = SendMessageA(gHwndList, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessage(gHwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
}

static void stop_recording(void) {
    if (InterlockedExchange(&gRecording, 0) == 1) {
        UnhookWindowsHookEx(gKeyboardHook);
        UnhookWindowsHookEx(gMouseHook);
        gKeyboardHook = NULL;
        gMouseHook = NULL;
        RefreshList();
        MessageBoxA(gHwndMain, "Recording stopped.", "Info", MB_OK);
    }
}

static void start_recording(void) {
    if (InterlockedCompareExchange(&gRecording, 1, 0) != 0) return;
    if (InterlockedCompareExchange(&gPlaying, 0, 0) != 0) {
        InterlockedExchange(&gRecording, 0);
        MessageBoxA(gHwndMain, "Please stop playback before recording.", "Warning", MB_OK | MB_ICONWARNING);
        return; 
    }

    stop_playback();
    clear_events();
    RefreshList();
    gRecordStartUs = now_us();
    gHasLastMove = 0;

    gKeyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandleA(NULL), 0);
    gMouseHook = SetWindowsHookExA(WH_MOUSE_LL, mouse_hook_proc, GetModuleHandleA(NULL), 0);

    if (!gKeyboardHook || !gMouseHook) {
        if (gKeyboardHook) UnhookWindowsHookEx(gKeyboardHook);
        if (gMouseHook) UnhookWindowsHookEx(gMouseHook);
        gKeyboardHook = NULL;
        gMouseHook = NULL;
        InterlockedExchange(&gRecording, 0);
        MessageBoxA(gHwndMain, "Failed to start recording hook.", "Error", MB_OK | MB_ICONERROR);
    }
}

static int save_macro(const char* path) {
    FILE* file = fopen(path, "wb");
    if (!file) return 0;

    MacroHeader header = { {'M','C','R','1'}, 1, (uint32_t)gEventCount };
    int ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
             (gEventCount == 0 || fwrite(gEvents, sizeof(MacroEvent), gEventCount, file) == gEventCount);
    fclose(file);
    return ok;
}

static int load_macro(const char* path) {
    if (InterlockedCompareExchange(&gRecording, 0, 0) != 0 || InterlockedCompareExchange(&gPlaying, 0, 0) != 0) {
        MessageBoxA(gHwndMain, "Stop recording/playback first.", "Warning", MB_OK | MB_ICONWARNING);
        return 0;
    }

    FILE* file = fopen(path, "rb");
    if (!file) return 0;

    MacroHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1 || 
        header.magic[0] != 'M' || header.magic[1] != 'C' || 
        header.magic[2] != 'R' || header.magic[3] != '1' || header.version != 1) {
        fclose(file); return 0;
    }

    MacroEvent* loaded = NULL;
    if (header.count > 0) {
        loaded = (MacroEvent*)malloc((size_t)header.count * sizeof(MacroEvent));
        if (!loaded || fread(loaded, sizeof(MacroEvent), header.count, file) != header.count) {
            free(loaded); fclose(file); return 0;
        }
    }

    fclose(file);
    clear_events();
    gEvents = loaded;
    gEventCount = header.count;
    gEventCapacity = header.count;
    RefreshList();
    return 1;
}

static void send_mouse_button(DWORD flags, int x, int y, DWORD data) {
    INPUT input; ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = x; input.mi.dy = y;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    SendInput(1, &input, sizeof(input));
}

static void send_key(uint32_t vk, DWORD flags) {
    INPUT input; ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)vk;
    input.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = flags;
    SendInput(1, &input, sizeof(input));
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
    return 0;
}

static void start_playback(void) {
    if (InterlockedCompareExchange(&gPlaying, 1, 0) != 0) return;
    stop_recording();
    InterlockedExchange(&gStopPlayback, 0);

    HANDLE thread = CreateThread(NULL, 0, playback_thread, NULL, 0, NULL);
    if (!thread) InterlockedExchange(&gPlaying, 0);
    else CloseHandle(thread);
}

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && InterlockedCompareExchange(&gRecording, 0, 0) != 0) {
        const KBDLLHOOKSTRUCT* data = (const KBDLLHOOKSTRUCT*)lParam;
        if ((data->flags & LLKHF_INJECTED) == 0 &&
            data->vkCode != VK_F1 && data->vkCode != VK_F2 && data->vkCode != VK_F3 && data->vkCode != VK_F4 &&
            data->vkCode != VK_F5 && data->vkCode != VK_F6) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                MacroEvent event; ZeroMemory(&event, sizeof(event));
                event.timeUs = now_us() - gRecordStartUs;
                event.type = EVENT_KEY;
                event.vk = data->vkCode;
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
                        if (append_event(&event)) {
                            gLastMoveX = data->pt.x; gLastMoveY = data->pt.y; gHasLastMove = 1;
                        }
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
                    append_event(&event);
                    break;
                case WM_MOUSEWHEEL:
                    event.type = EVENT_MOUSE_WHEEL;
                    event.mouseFlags = MOUSEEVENTF_WHEEL;
                    event.mouseData = (short)HIWORD(data->mouseData);
                    append_event(&event);
                    break;
            }
        }
    }
    return CallNextHookEx(gMouseHook, code, wParam, lParam);
}

BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam) {
    SendMessage(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == 1001 && HIWORD(wParam) == LBN_SELCHANGE) {
                LRESULT sel = SendMessage(gHwndList, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    gSelectedEvent = (int)SendMessage(gHwndList, LB_GETITEMDATA, sel, 0);
                    if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                        MacroEvent* e = &gEvents[gSelectedEvent];
                        char buf[64];
                        sprintf(buf, "%llu", (unsigned long long)(e->timeUs / 1000ULL)); SetWindowTextA(gHwndTime, buf);
                        if (e->type == EVENT_KEY) {
                            sprintf(buf, "%u", e->vk); SetWindowTextA(gHwndP1, buf);
                            sprintf(buf, "%u", e->down); SetWindowTextA(gHwndP2, buf);
                        } else if (e->type == EVENT_MOUSE_MOVE) {
                            sprintf(buf, "%d", e->x); SetWindowTextA(gHwndP1, buf);
                            sprintf(buf, "%d", e->y); SetWindowTextA(gHwndP2, buf);
                        } else {
                            sprintf(buf, "%u", e->mouseFlags); SetWindowTextA(gHwndP1, buf);
                            sprintf(buf, "%d", e->mouseData); SetWindowTextA(gHwndP2, buf);
                        }
                    }
                }
            } else if (LOWORD(wParam) == 1005) { // Update
                if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                    char buf[64];
                    MacroEvent* e = &gEvents[gSelectedEvent];
                    GetWindowTextA(gHwndTime, buf, sizeof(buf)); e->timeUs = strtoull(buf, NULL, 10) * 1000ULL;
                    GetWindowTextA(gHwndP1, buf, sizeof(buf)); int p1 = atoi(buf);
                    GetWindowTextA(gHwndP2, buf, sizeof(buf)); int p2 = atoi(buf);

                    if (e->type == EVENT_KEY) { e->vk = p1; e->down = p2; }
                    else if (e->type == EVENT_MOUSE_MOVE) { e->x = p1; e->y = p2; }
                    else { e->mouseFlags = p1; e->mouseData = p2; }

                    int prevSel = (int)SendMessage(gHwndList, LB_GETCURSEL, 0, 0);
                    RefreshList();
                    SendMessage(gHwndList, LB_SETCURSEL, prevSel, 0);
                }
            } else if (LOWORD(wParam) == 1006) { // Delete
                if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                    for (size_t i = gSelectedEvent; i < gEventCount - 1; i++) {
                        gEvents[i] = gEvents[i+1];
                    }
                    gEventCount--;
                    gSelectedEvent = -1;
                    RefreshList();
                    SetWindowTextA(gHwndTime, "");
                    SetWindowTextA(gHwndP1, "");
                    SetWindowTextA(gHwndP2, "");
                }
            }
            return 0;
        
        case WM_HOTKEY:
            switch (wParam) {
                case 1: start_recording(); break;
                case 2: stop_recording(); break;
                case 3: start_playback(); break;
                case 4: stop_playback(); break;
                case 5: if (save_macro("macro.bin")) MessageBoxA(hwnd, "Saved to macro.bin", "Info", MB_OK); break;
                case 6: 
                    if (load_macro("macro.bin")) {
                        MessageBoxA(hwnd, "Loaded macro.bin", "Info", MB_OK);
                    } else {
                        MessageBoxA(hwnd, "Failed to load macro.bin", "Error", MB_OK | MB_ICONERROR);
                    }
                    break;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)pCmdLine; (void)nCmdShow;
    QueryPerformanceFrequency(&gQpcFreq);

    WNDCLASSA wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MacroGUI";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    gHwndMain = CreateWindowA("MacroGUI", "Macro Editor", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 480, NULL, NULL, hInstance, NULL);

    gHwndList = CreateWindowA("LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_HASSTRINGS,
        10, 10, 380, 420, gHwndMain, (HMENU)1001, hInstance, NULL);

    CreateWindowA("STATIC", "Time (ms):", WS_CHILD | WS_VISIBLE, 400, 10, 190, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndTime = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 400, 30, 190, 24, gHwndMain, NULL, hInstance, NULL);

    CreateWindowA("STATIC", "Param 1 (VK / X / Flags):", WS_CHILD | WS_VISIBLE, 400, 70, 190, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndP1 = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 400, 90, 190, 24, gHwndMain, NULL, hInstance, NULL);

    CreateWindowA("STATIC", "Param 2 (Down / Y / Data):", WS_CHILD | WS_VISIBLE, 400, 130, 190, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndP2 = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 400, 150, 190, 24, gHwndMain, NULL, hInstance, NULL);

    gHwndUpdate = CreateWindowA("BUTTON", "Update Selected", WS_CHILD | WS_VISIBLE, 400, 190, 190, 30, gHwndMain, (HMENU)1005, hInstance, NULL);
    gHwndDelete = CreateWindowA("BUTTON", "Delete Selected", WS_CHILD | WS_VISIBLE, 400, 230, 190, 30, gHwndMain, (HMENU)1006, hInstance, NULL);

    CreateWindowA("STATIC", "Hotkeys:\nF1: Start Recording\nF2: Stop Recording\nF3: Play Macro\nF4: Stop Playback\nF5: Save macro.bin\nF6: Load macro.bin", 
        WS_CHILD | WS_VISIBLE, 400, 280, 190, 120, gHwndMain, NULL, hInstance, NULL);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(gHwndMain, SetFontProc, (LPARAM)hFont);

    RegisterHotKey(gHwndMain, 1, 0, VK_F1);
    RegisterHotKey(gHwndMain, 2, 0, VK_F2);
    RegisterHotKey(gHwndMain, 3, 0, VK_F3);
    RegisterHotKey(gHwndMain, 4, 0, VK_F4);
    RegisterHotKey(gHwndMain, 5, 0, VK_F5);
    RegisterHotKey(gHwndMain, 6, 0, VK_F6);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    stop_recording();
    stop_playback();
    UnregisterHotKey(gHwndMain, 1);
    UnregisterHotKey(gHwndMain, 2);
    UnregisterHotKey(gHwndMain, 3);
    UnregisterHotKey(gHwndMain, 4);
    UnregisterHotKey(gHwndMain, 5);
    UnregisterHotKey(gHwndMain, 6);
    clear_events();
    return 0;
}
