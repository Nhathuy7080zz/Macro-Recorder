#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int gLang = 1;

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

static void stop_recording(void) {
    if (InterlockedExchange(&gRecording, 0) == 1) {
        UnhookWindowsHookEx(gKeyboardHook);
        UnhookWindowsHookEx(gMouseHook);
        gKeyboardHook = NULL;
        gMouseHook = NULL;
        printf("Recording stopped. %zu events captured.\n", gEventCount);
    }
}

static void start_recording(void) {
    if (InterlockedCompareExchange(&gRecording, 1, 0) != 0) {
        return;
    }

    if (InterlockedCompareExchange(&gPlaying, 0, 0) != 0) {
        InterlockedExchange(&gRecording, 0);
        printf("Stop playback first.\n");
        return;
    }

    stop_playback();
    clear_events();
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
        printf("Failed to start recording.\n");
        return;
    }

    printf("Recording started.\n");
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
        printf("Stop recording/playback first.\n");
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
    printf(gLang ? "Da doc %zu su kien tu %s\n" : "Loaded %zu events from %s\n", gEventCount, path);
    return 1;
}

static void send_mouse_button(DWORD flags, int x, int y, DWORD data) {
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    input.mi.dwExtraInfo = 0;
    SendInput(1, &input, sizeof(input));
}

static void send_key(uint32_t vk, DWORD flags) {
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)vk;
    input.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = flags;
    SendInput(1, &input, sizeof(input));
}

static DWORD WINAPI playback_thread(LPVOID unused) {
    (void)unused;

    if (gEventCount == 0) {
        printf("No macro loaded.\n");
        InterlockedExchange(&gPlaying, 0);
        return 0;
    }

    printf("Playback started.\n");
    InterlockedExchange(&gStopPlayback, 0);

    uint64_t playStartUs = now_us();

    for (size_t i = 0; i < gEventCount; ++i) {
        if (InterlockedCompareExchange(&gStopPlayback, 0, 0) != 0) {
            break;
        }

        const MacroEvent* event = &gEvents[i];
        sleep_until_us(playStartUs + event->timeUs);

        if (InterlockedCompareExchange(&gStopPlayback, 0, 0) != 0) {
            break;
        }

        switch (event->type) {
            case EVENT_KEY:
                send_key(event->vk, event->down ? 0 : KEYEVENTF_KEYUP);
                break;
            case EVENT_MOUSE_MOVE:
                SetCursorPos(event->x, event->y);
                break;
            case EVENT_MOUSE_BUTTON:
                SetCursorPos(event->x, event->y);
                send_mouse_button(event->mouseFlags, event->x, event->y, (DWORD)event->mouseData);
                break;
            case EVENT_MOUSE_WHEEL:
                SetCursorPos(event->x, event->y);
                send_mouse_button(event->mouseFlags, event->x, event->y, (DWORD)event->mouseData);
                break;
            default:
                break;
        }
    }

    if (InterlockedCompareExchange(&gStopPlayback, 0, 0) != 0) {
        printf("Playback stopped.\n");
    } else {
        printf("Playback finished.\n");
    }

    InterlockedExchange(&gPlaying, 0);
    return 0;
}

static void start_playback(void) {
    if (InterlockedCompareExchange(&gPlaying, 1, 0) != 0) {
        return;
    }

    stop_recording();
    InterlockedExchange(&gStopPlayback, 0);

    HANDLE thread = CreateThread(NULL, 0, playback_thread, NULL, 0, NULL);
    if (!thread) {
        InterlockedExchange(&gPlaying, 0);
        printf("Failed to start playback.\n");
        return;
    }

    CloseHandle(thread);
}

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && InterlockedCompareExchange(&gRecording, 0, 0) != 0) {
        const KBDLLHOOKSTRUCT* data = (const KBDLLHOOKSTRUCT*)lParam;
        if ((data->flags & LLKHF_INJECTED) == 0 &&
            data->vkCode != VK_F1 && data->vkCode != VK_F2 && data->vkCode != VK_F3 && data->vkCode != VK_F4 &&
            data->vkCode != VK_F5 && data->vkCode != VK_F6) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                MacroEvent event;
                ZeroMemory(&event, sizeof(event));
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
            MacroEvent event;
            ZeroMemory(&event, sizeof(event));
            event.timeUs = now_us() - gRecordStartUs;
            event.x = data->pt.x;
            event.y = data->pt.y;

            switch (wParam) {
                case WM_MOUSEMOVE:
                    if (!gHasLastMove || gLastMoveX != data->pt.x || gLastMoveY != data->pt.y) {
                        event.type = EVENT_MOUSE_MOVE;
                        if (append_event(&event)) {
                            gLastMoveX = data->pt.x;
                            gLastMoveY = data->pt.y;
                            gHasLastMove = 1;
                        }
                    }
                    break;
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONUP:
                case WM_MBUTTONDOWN:
                case WM_MBUTTONUP:
                case WM_XBUTTONDOWN:
                case WM_XBUTTONUP:
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
                default:
                    break;
            }
        }
    }

    return CallNextHookEx(gMouseHook, code, wParam, lParam);
}

static void print_help(void) {
    if (gLang) {
        printf("MACRO RECORDER CLI\nF1: Ghi lén, F2: Dừng ghi\nF3: Phát, F4: Dừng phát\nF5: Lưu JSON, F6: Đọc JSON\n");
    } else {
        printf("MACRO RECORDER CLI\nF1: Record, F2: Stop Rec\nF3: Play, F4: Stop Play\nF5: Save JSON, F6: Load JSON\n");
    }
}


int main(int argc, char* argv[]) {
    QueryPerformanceFrequency(&gQpcFreq);
    if (argc > 1 && strcmp(argv[1], "-en") == 0) { gLang = 0; }
print_help();

    RegisterHotKey(NULL, 1, 0, VK_F1);
    RegisterHotKey(NULL, 2, 0, VK_F2);
    RegisterHotKey(NULL, 3, 0, VK_F3);
    RegisterHotKey(NULL, 4, 0, VK_F4);
    RegisterHotKey(NULL, 5, 0, VK_F5);
    RegisterHotKey(NULL, 6, 0, VK_F6);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            switch (msg.wParam) {
                case 1:
                    start_recording();
                    break;
                case 2:
                    stop_recording();
                    break;
                case 3:
                    start_playback();
                    break;
                case 4:
                    stop_playback();
                    break;
                case 5:
                    if (save_macro("macro.json")) {
                        printf("Saved macro.json (%zu events).\n", gEventCount);
                    } else {
                        printf("Failed to save macro.json.\n");
                    }
                    break;
                case 6:
                    if (!load_macro("macro.json")) {
                        printf("Failed to load macro.json.\n");
                    }
                    break;
                default:
                    break;
            }
        }
    }

    stop_recording();
    stop_playback();
    UnregisterHotKey(NULL, 1);
    UnregisterHotKey(NULL, 2);
    UnregisterHotKey(NULL, 3);
    UnregisterHotKey(NULL, 4);
    UnregisterHotKey(NULL, 5);
    UnregisterHotKey(NULL, 6);
    clear_events();
    return 0;
}




