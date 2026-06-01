#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
HWND gHwndMain, gHwndList, gHwndStatus;
HWND gHwndTime, gHwndX, gHwndY, gHwndKey, gHwndDown;
HWND gHwndUpdate, gHwndDelete;
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
        if (currentUs >= targetUs) break;
        uint64_t remainingUs = targetUs - currentUs;
        if (remainingUs > 2000) Sleep((DWORD)((remainingUs - 1000) / 1000));
        else SwitchToThread();
    }
}

static int append_event(const MacroEvent* event) {
    if (gEventCount == gEventCapacity) {
        size_t newCapacity = gEventCapacity == 0 ? 256 : gEventCapacity * 2;
        MacroEvent* newEvents = (MacroEvent*)realloc(gEvents, newCapacity * sizeof(MacroEvent));
        if (!newEvents) return 0;
        gEvents = newEvents;
        gEventCapacity = newCapacity;
    }
    gEvents[gEventCount++] = *event;
    return 1;
}

static void clear_events(void) {
    free(gEvents); gEvents = NULL;
    gEventCount = 0; gEventCapacity = 0;
}

static void stop_playback(void) {
    InterlockedExchange(&gStopPlayback, 1);
}

void RefreshList(void) {
    SendMessage(gHwndList, LB_RESETCONTENT, 0, 0);
    char buf[256];
    char keyName[64];
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        unsigned long long tMs = (unsigned long long)(e->timeUs / 1000ULL);
        
        if (e->type == EVENT_KEY) {
            UINT scan = MapVirtualKeyA(e->vk, MAPVK_VK_TO_VSC);
            if (scan == 0 || GetKeyNameTextA(scan << 16, keyName, sizeof(keyName)) == 0) {
                sprintf(keyName, "Phím %u", e->vk);
            }
            sprintf(buf, "[%zu] %llu ms | BÀN PHÍM | %s (%s)", i, tMs, keyName, e->down ? "Nhấn xuống" : "Nhả ra");
        } else if (e->type == EVENT_MOUSE_MOVE) {
            sprintf(buf, "[%zu] %llu ms | CHUỘT | Di chuyển tới (X: %d, Y: %d)", i, tMs, e->x, e->y);
        } else if (e->type == EVENT_MOUSE_BUTTON) {
            sprintf(buf, "[%zu] %llu ms | CHUỘT | Click tại (X: %d, Y: %d)", i, tMs, e->x, e->y);
        } else if (e->type == EVENT_MOUSE_WHEEL) {
            sprintf(buf, "[%zu] %llu ms | CHUỘT | Cuộn chuột", i, tMs);
        } else {
            sprintf(buf, "[%zu] %llu ms | KHÔNG RÕ", i, tMs);
        }
        LRESULT idx = SendMessageA(gHwndList, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessage(gHwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
}

static void stop_recording(void) {
    if (InterlockedExchange(&gRecording, 0) == 1) {
        UnhookWindowsHookEx(gKeyboardHook);
        UnhookWindowsHookEx(gMouseHook);
        gKeyboardHook = NULL; gMouseHook = NULL;
        RefreshList();
        SetWindowTextA(gHwndStatus, "Trạng thái: 🟢 Đã dừng ghi (Sẵn sàng)");
        MessageBoxA(gHwndMain, "Đã dừng ghi macro.", "Thông báo", MB_OK);
    }
}

static void start_recording(void) {
    if (InterlockedCompareExchange(&gRecording, 1, 0) != 0) return;
    if (InterlockedCompareExchange(&gPlaying, 0, 0) != 0) {
        InterlockedExchange(&gRecording, 0);
        MessageBoxA(gHwndMain, "Vui lòng dừng phát macro trước khi ghi.", "Cảnh báo", MB_OK | MB_ICONWARNING);
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
        gKeyboardHook = NULL; gMouseHook = NULL;
        InterlockedExchange(&gRecording, 0);
        MessageBoxA(gHwndMain, "Lỗi chạy hook.", "Lỗi", MB_OK | MB_ICONERROR);
    } else {
        SetWindowTextA(gHwndStatus, "Trạng thái: 🔴 ĐANG GHI MACRO (Bấm F2 để dừng) ...");
    }
}

static int save_macro(const char* path) {
    FILE* file = fopen(path, "w");
    if (!file) return 0;
    
    fprintf(file, "[\n");
    for (size_t i = 0; i < gEventCount; ++i) {
        MacroEvent* e = &gEvents[i];
        fprintf(file, "  {\n");
        fprintf(file, "    \"timeUs\": %llu,\n", (unsigned long long)e->timeUs);
        fprintf(file, "    \"type\": %u,\n", e->type);
        fprintf(file, "    \"vk\": %u,\n", e->vk);
        fprintf(file, "    \"mouseFlags\": %u,\n", e->mouseFlags);
        fprintf(file, "    \"mouseData\": %d,\n", e->mouseData);
        fprintf(file, "    \"x\": %d,\n", e->x);
        fprintf(file, "    \"y\": %d,\n", e->y);
        fprintf(file, "    \"down\": %u\n", e->down);
        fprintf(file, "  }%s\n", (i == gEventCount - 1) ? "" : ",");
    }
    fprintf(file, "]\n");
    fclose(file);
    return 1;
}

static int load_macro(const char* path) {
    if (InterlockedCompareExchange(&gRecording, 0, 0) != 0 || InterlockedCompareExchange(&gPlaying, 0, 0) != 0) {
        MessageBoxA(gHwndMain, "Dừng ghi/phát trước khi Load.", "Cảnh báo", MB_OK | MB_ICONWARNING);
        return 0;
    }
    FILE* file = fopen(path, "r");
    if (!file) return 0;

    fseek(file, 0, SEEK_END);
    long len = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (len <= 0) { fclose(file); return 0; }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(file); return 0; }
    fread(buf, 1, len, file);
    buf[len] = '\0';
    fclose(file);

    clear_events();

    char* ptr = buf;
    while ((ptr = strstr(ptr, "{")) != NULL) {
        char* end = strstr(ptr, "}");
        if (!end) break;
        *end = '\0';

        MacroEvent e; ZeroMemory(&e, sizeof(e));
        char* p;
        if ((p = strstr(ptr, "\"timeUs\""))) { p = strchr(p, ':'); if (p) e.timeUs = strtoull(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"type\""))) { p = strchr(p, ':'); if (p) e.type = (uint8_t)strtoul(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"vk\""))) { p = strchr(p, ':'); if (p) e.vk = (uint32_t)strtoul(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"mouseFlags\""))) { p = strchr(p, ':'); if (p) e.mouseFlags = (uint32_t)strtoul(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"mouseData\""))) { p = strchr(p, ':'); if (p) e.mouseData = (int32_t)strtol(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"x\""))) { p = strchr(p, ':'); if (p) e.x = (int32_t)strtol(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"y\""))) { p = strchr(p, ':'); if (p) e.y = (int32_t)strtol(p+1, NULL, 10); }
        if ((p = strstr(ptr, "\"down\""))) { p = strchr(p, ':'); if (p) e.down = (uint8_t)strtoul(p+1, NULL, 10); }
        append_event(&e);
        *end = '}';
        ptr = end + 1;
    }
    
    free(buf);
    RefreshList();
    return 1;
}

static void send_mouse_button(DWORD flags, int x, int y, DWORD data) {
    INPUT input; ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE; input.mi.dx = x; input.mi.dy = y;
    input.mi.dwFlags = flags; input.mi.mouseData = data;
    SendInput(1, &input, sizeof(input));
}

static void send_key(uint32_t vk, DWORD flags) {
    INPUT input; ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD; input.ki.wVk = (WORD)vk;
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
    SetWindowTextA(gHwndStatus, "Trạng thái: 🟢 IDLE (Đã chạy xong)");
    return 0;
}

static void start_playback(void) {
    if (InterlockedCompareExchange(&gPlaying, 1, 0) != 0) return;
    stop_recording();
    InterlockedExchange(&gStopPlayback, 0);
    SetWindowTextA(gHwndStatus, "Trạng thái: ⏳ ĐANG PHÁT MACRO (F4 để dừng)");

    HANDLE thread = CreateThread(NULL, 0, playback_thread, NULL, 0, NULL);
    if (!thread) {
        InterlockedExchange(&gPlaying, 0);
        SetWindowTextA(gHwndStatus, "Trạng thái: Lỗi không thể phát.");
    } else CloseHandle(thread);
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
                    else if (wParam == WM_XBUTTONDOWN) { 
                        event.mouseFlags = MOUSEEVENTF_XDOWN; 
                        event.mouseData = (HIWORD(data->mouseData) == XBUTTON1) ? XBUTTON1 : XBUTTON2; 
                    } else { 
                        event.mouseFlags = MOUSEEVENTF_XUP; 
                        event.mouseData = (HIWORD(data->mouseData) == XBUTTON1) ? XBUTTON1 : XBUTTON2; 
                    }
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
                        
                        sprintf(buf, "%llu", (unsigned long long)(e->timeUs / 1000ULL)); 
                        SetWindowTextA(gHwndTime, buf);
                        
                        if (e->type == EVENT_KEY) {
                            // Bật chỉnh sửa Bàn Phím, Tắt Chuột
                            EnableWindow(gHwndKey, TRUE); EnableWindow(gHwndDown, TRUE);
                            EnableWindow(gHwndX, FALSE); EnableWindow(gHwndY, FALSE);
                            
                            sprintf(buf, "%u", e->vk); SetWindowTextA(gHwndKey, buf);
                            sprintf(buf, "%u", e->down); SetWindowTextA(gHwndDown, buf);
                            SetWindowTextA(gHwndX, "-"); SetWindowTextA(gHwndY, "-");
                        } else {
                            // Bật chỉnh sửa Chuột, Tắt Bàn Phím
                            EnableWindow(gHwndX, TRUE); EnableWindow(gHwndY, TRUE);
                            EnableWindow(gHwndKey, FALSE); EnableWindow(gHwndDown, FALSE);
                            
                            sprintf(buf, "%d", e->x); SetWindowTextA(gHwndX, buf);
                            sprintf(buf, "%d", e->y); SetWindowTextA(gHwndY, buf);
                            SetWindowTextA(gHwndKey, "-"); SetWindowTextA(gHwndDown, "-");
                        }
                    }
                }
            } else if (LOWORD(wParam) == 1005) { // Update
                if (gSelectedEvent >= 0 && gSelectedEvent < (int)gEventCount) {
                    char buf[64];
                    MacroEvent* e = &gEvents[gSelectedEvent];
                    
                    GetWindowTextA(gHwndTime, buf, sizeof(buf)); 
                    e->timeUs = strtoull(buf, NULL, 10) * 1000ULL;
                    
                    if (e->type == EVENT_KEY) {
                        GetWindowTextA(gHwndKey, buf, sizeof(buf)); e->vk = (uint32_t)atoi(buf);
                        GetWindowTextA(gHwndDown, buf, sizeof(buf)); e->down = (uint8_t)atoi(buf);
                    } else if (e->type == EVENT_MOUSE_MOVE || e->type == EVENT_MOUSE_BUTTON) {
                        GetWindowTextA(gHwndX, buf, sizeof(buf)); e->x = atoi(buf);
                        GetWindowTextA(gHwndY, buf, sizeof(buf)); e->y = atoi(buf);
                    }

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
                    SetWindowTextA(gHwndX, ""); SetWindowTextA(gHwndY, "");
                    SetWindowTextA(gHwndKey, ""); SetWindowTextA(gHwndDown, "");
                }
            }
            return 0;
        
        case WM_HOTKEY:
            switch (wParam) {
                case 1: start_recording(); break;
                case 2: stop_recording(); break;
                case 3: start_playback(); break;
                case 4: 
                    stop_playback(); 
                    SetWindowTextA(gHwndStatus, "Trạng thái: 🟢 Đã ngắt phát (IDLE)");
                    break;
                case 5: if (save_macro("macro.json")) MessageBoxA(hwnd, "Lưu JSON thành công (macro.json)!", "Xong", MB_OK); break;
                case 6: 
                    if (load_macro("macro.json")) {
                        MessageBoxA(hwnd, "Đã nạp file macro.json", "Xong", MB_OK);
                    } else {
                        MessageBoxA(hwnd, "Không thể đọc macro.json hoặc sai form", "Lỗi", MB_OK | MB_ICONERROR);
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

    gHwndMain = CreateWindowA("MacroGUI", "Macro Editor (Phiên bản Đơn giản)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 600, NULL, NULL, hInstance, NULL);

    gHwndStatus = CreateWindowA("STATIC", "Trạng thái: 🟢 Khởi động xong (IDLE)", WS_CHILD | WS_VISIBLE, 
        10, 10, 680, 20, gHwndMain, NULL, hInstance, NULL);

    gHwndList = CreateWindowA("LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_HASSTRINGS,
        10, 40, 420, 500, gHwndMain, (HMENU)1001, hInstance, NULL);

    // ================= KHU VỰC CHỈNH SỬA BÊN PHẢI ================= //
    int rightX = 440;
    
    // Thuộc tính chung
    CreateWindowA("STATIC", "1. THỜI GIAN TRỄ", WS_CHILD | WS_VISIBLE, rightX, 40, 230, 20, gHwndMain, NULL, hInstance, NULL);
    CreateWindowA("STATIC", "Thời gian (mili-giây):", WS_CHILD | WS_VISIBLE, rightX, 60, 230, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndTime = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, rightX, 80, 230, 24, gHwndMain, NULL, hInstance, NULL);

    // Thuộc tính Chuột
    CreateWindowA("STATIC", "2. KHI LÀ LỆNH CHUỘT", WS_CHILD | WS_VISIBLE, rightX, 120, 230, 20, gHwndMain, NULL, hInstance, NULL);
    CreateWindowA("STATIC", "Tọa độ X:", WS_CHILD | WS_VISIBLE, rightX, 140, 110, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndX = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, rightX, 160, 110, 24, gHwndMain, NULL, hInstance, NULL);
    CreateWindowA("STATIC", "Tọa độ Y:", WS_CHILD | WS_VISIBLE, rightX + 120, 140, 110, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndY = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, rightX + 120, 160, 110, 24, gHwndMain, NULL, hInstance, NULL);

    // Thuộc tính Phím
    CreateWindowA("STATIC", "3. KHI LÀ LỆNH BÀN PHÍM", WS_CHILD | WS_VISIBLE, rightX, 200, 230, 20, gHwndMain, NULL, hInstance, NULL);
    CreateWindowA("STATIC", "Mã phím (Ví dụ 65=A, 13=Enter):", WS_CHILD | WS_VISIBLE, rightX, 220, 230, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndKey = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, rightX, 240, 230, 24, gHwndMain, NULL, hInstance, NULL);
    CreateWindowA("STATIC", "Hành động (1 = Nhấn, 0 = Nhả):", WS_CHILD | WS_VISIBLE, rightX, 270, 230, 20, gHwndMain, NULL, hInstance, NULL);
    gHwndDown = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, rightX, 290, 230, 24, gHwndMain, NULL, hInstance, NULL);

    // Nút Lưu/Xóa
    gHwndUpdate = CreateWindowA("BUTTON", "Lưu thay đổi dòng này", WS_CHILD | WS_VISIBLE, rightX, 330, 230, 35, gHwndMain, (HMENU)1005, hInstance, NULL);
    gHwndDelete = CreateWindowA("BUTTON", "Xóa dòng này khỏi danh sách", WS_CHILD | WS_VISIBLE, rightX, 375, 230, 35, gHwndMain, (HMENU)1006, hInstance, NULL);

    // Hướng dẫn
    CreateWindowA("STATIC", "--- PHÍM TẮT ĐIỀU KHIỂN ---\n\nF1: Ghi lén    F2: Dừng ghi\nF3: Phát       F4: Dừng phát\n\nF5: Lưu json   F6: Mở json", 
        WS_CHILD | WS_VISIBLE, rightX, 430, 230, 100, gHwndMain, NULL, hInstance, NULL);

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
    UnregisterHotKey(gHwndMain, 1); UnregisterHotKey(gHwndMain, 2);
    UnregisterHotKey(gHwndMain, 3); UnregisterHotKey(gHwndMain, 4);
    UnregisterHotKey(gHwndMain, 5); UnregisterHotKey(gHwndMain, 6);
    clear_events();
    return 0;
}
