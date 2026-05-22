/**
 * Text Selection Hook Core for Windows
 *
 * Ported from upstream selection-hook (Node N-API -> C-ABI for Dart FFI).
 * All Napi::* removed. ThreadSafeFunction replaced with SHSelectionCallback etc.
 * Hook callbacks post messages to hook thread's message loop for processing.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Ported under the MIT License
 */

#include "selection_hook_core.h"

#include <ShellScalingApi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "lib/clipboard.h"
#include "lib/keyboard.h"
#include "lib/string_pool.h"
#include "lib/utils.h"

#pragma comment(lib, "Oleacc.lib")
#pragma comment(lib, "UIAutomationCore.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

#ifndef UIA_IsSelectionActivePropertyId
#define UIA_IsSelectionActivePropertyId 30034
#endif

enum class SelectionDetectType
{
    None = 0,
    Drag = 1,
    DoubleClick = 2,
    ShiftClick = 3
};

enum class CopyKeyType
{
    CtrlInsert = 0,
    CtrlC = 1
};

constexpr int MIN_DRAG_DISTANCE = 8;
constexpr DWORD MAX_DRAG_TIME_MS = 8000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static DWORD DOUBLE_CLICK_TIME_MS = 500;

#define WM_SH_MOUSE_EVENT (WM_USER + 1)
#define WM_SH_KEYBOARD_EVENT (WM_USER + 2)
#define WM_SH_GET_SELECTION (WM_USER + 3)

struct MouseEventMsg
{
    WPARAM event;
    LONG ptX;
    LONG ptY;
    DWORD mouseData;
};

struct KeyboardEventMsg
{
    WPARAM event;
    DWORD vkCode;
    DWORD scanCode;
    DWORD flags;
};

std::atomic<SelectionHookCore *> SelectionHookCore::currentInstance{nullptr};

SelectionHookCore::SelectionHookCore()
{
    DOUBLE_CLICK_TIME_MS = GetDoubleClickTime();
    enableDpiAwareness();
}

SelectionHookCore::~SelectionHookCore()
{
    if (currentInstance.load() == this)
    {
        currentInstance.store(nullptr);
    }

    stop();
}

void SelectionHookCore::setCallback(SHSelectionCallback cb)
{
    callback.store(cb);
}

void SelectionHookCore::setMouseCallback(SHMouseCallback cb)
{
    mouseCallback.store(cb);
}

void SelectionHookCore::setKeyboardCallback(SHKeyboardCallback cb)
{
    keyboardCallback.store(cb);
}

void SelectionHookCore::setPassiveMode(bool passive)
{
    is_selection_passive_mode.store(passive);
}

void SelectionHookCore::setClipboardEnabled(bool enabled)
{
    is_enabled_clipboard.store(enabled);
}

void SelectionHookCore::setClipboardMode(FilterMode mode, const std::vector<std::string> &programList)
{
    clipboard_filter_mode = mode;
    clipboard_filter_list = programList;
}

void SelectionHookCore::setGlobalFilterMode(FilterMode mode, const std::vector<std::string> &programList)
{
    global_filter_mode = mode;
    global_filter_list = programList;
}

void SelectionHookCore::setDebugEnabled(bool enabled)
{
    debug_enabled.store(enabled);
}

void SelectionHookCore::enableMouseMove()
{
    is_enabled_mouse_move.store(true);
}

void SelectionHookCore::disableMouseMove()
{
    is_enabled_mouse_move.store(false);
}

bool SelectionHookCore::start()
{
    if (running.load())
        return false;

    auto *cur = currentInstance.load();
    if (cur && cur != this)
    {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            last_error = "Another SelectionHookCore instance is already active";
        }
        return false;
    }

    currentInstance.store(this);
    running.store(true);

    hook_thread = CreateThread(NULL, 0, HookThreadProc, this, CREATE_SUSPENDED, &hook_thread_id);
    if (!hook_thread)
    {
        running.store(false);
        currentInstance.store(nullptr);
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            last_error = "Failed to create hook thread";
        }
        return false;
    }

    ResumeThread(hook_thread);
    return true;
}

bool SelectionHookCore::stop()
{
    if (!running.load())
        return true;

    running.store(false);

    if (hook_thread_id != 0)
    {
        PostThreadMessageW(hook_thread_id, WM_USER, 0, 0);
    }

    if (hook_thread)
    {
        WaitForSingleObject(hook_thread, 3000);
        CloseHandle(hook_thread);
        hook_thread = NULL;
        hook_thread_id = 0;
    }

    currentInstance.store(nullptr);
    return true;
}

bool SelectionHookCore::isRunning() const
{
    return running.load();
}

const char *SelectionHookCore::lastError() const
{
    std::lock_guard<std::mutex> lock(error_mutex);
    if (last_error.empty())
        return nullptr;
    return last_error.c_str();
}

static bool InitComOnCurrentThread()
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    return SUCCEEDED(hr);
}

static IUIAutomation *CreateUIA()
{
    IUIAutomation *pUIA = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
                                  __uuidof(IUIAutomation), (void **)&pUIA);
    if (FAILED(hr))
        return nullptr;
    return pUIA;
}

static void ReleaseUIA(IUIAutomation *pUIA)
{
    if (pUIA)
    {
        pUIA->Release();
    }
}

TextSelectionInfo *SelectionHookCore::getCurrentSelection()
{
    if (!running.load() || !shouldProcessGetSelection())
        return nullptr;

    HWND hwnd = GetWindowUnderMouse();
    if (!hwnd)
        return nullptr;

    bool comInitOk = InitComOnCurrentThread();
    if (!comInitOk)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = "Failed to initialize COM for getCurrentSelection";
        return nullptr;
    }

    IUIAutomation *localUIA = CreateUIA();
    if (!localUIA)
    {
        CoUninitialize();
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = "Failed to initialize UI Automation for getCurrentSelection";
        return nullptr;
    }

    auto *info = new TextSelectionInfo();
    is_triggered_by_user.store(true);

    bool ok = getTextViaUIAutomation(localUIA, hwnd, *info);
    if (!ok || IsTrimmedEmpty(info->text))
    {
        ok = getTextViaAccessible(hwnd, *info);
    }
    if (!ok || IsTrimmedEmpty(info->text))
    {
        ok = getTextViaClipboard(hwnd, *info);
    }
    if (!ok || IsTrimmedEmpty(info->text))
    {
        is_triggered_by_user.store(false);
        ReleaseUIA(localUIA);
        CoUninitialize();
        delete info;
        return nullptr;
    }

    is_triggered_by_user.store(false);
    ReleaseUIA(localUIA);
    CoUninitialize();

    if (!GetProgramNameFromHwnd(hwnd, info->programName))
    {
        info->programName = L"";
    }

    return info;
}

DWORD WINAPI SelectionHookCore::HookThreadProc(LPVOID lpParam)
{
    auto *instance = static_cast<SelectionHookCore *>(lpParam);

    InitComOnCurrentThread();
    instance->pUIAHook = CreateUIA();

    if (!instance->pUIAHook)
    {
        fprintf(stderr, "[selection-hook] Failed to create UI Automation on hook thread\n");
        instance->running.store(false);
        instance->currentInstance.store(nullptr);
        return 1;
    }

    HHOOK mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, NULL, 0);
    HHOOK keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookCallback, NULL, 0);

    if (!mouseHook)
    {
        fprintf(stderr, "[selection-hook] SetWindowsHookEx(WH_MOUSE_LL) failed: %lu\n", GetLastError());
    }
    if (!keyboardHook)
    {
        fprintf(stderr, "[selection-hook] SetWindowsHookEx(WH_KEYBOARD_LL) failed: %lu\n", GetLastError());
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        if (msg.message == WM_USER)
        {
            if (mouseHook)
            {
                UnhookWindowsHookEx(mouseHook);
                mouseHook = NULL;
            }
            if (keyboardHook)
            {
                UnhookWindowsHookEx(keyboardHook);
                keyboardHook = NULL;
            }

            MSG drainMsg;
            while (PeekMessage(&drainMsg, NULL, WM_SH_MOUSE_EVENT, WM_SH_GET_SELECTION, PM_REMOVE))
            {
                if (drainMsg.message == WM_SH_MOUSE_EVENT)
                    delete reinterpret_cast<MouseEventMsg *>(drainMsg.wParam);
                else if (drainMsg.message == WM_SH_KEYBOARD_EVENT)
                    delete reinterpret_cast<KeyboardEventMsg *>(drainMsg.wParam);
            }
            break;
        }

        if (msg.message == WM_SH_MOUSE_EVENT)
        {
            auto *mem = reinterpret_cast<MouseEventMsg *>(msg.wParam);
            auto *cur = currentInstance.load();
            if (mem && cur)
            {
                cur->processMouseEvent(mem->event, mem->ptX, mem->ptY, mem->mouseData);
            }
            delete mem;
            continue;
        }

        if (msg.message == WM_SH_KEYBOARD_EVENT)
        {
            auto *mem = reinterpret_cast<KeyboardEventMsg *>(msg.wParam);
            auto *cur = currentInstance.load();
            if (mem && cur)
            {
                cur->processKeyboardEvent(mem->event, mem->vkCode, mem->scanCode, mem->flags);
            }
            delete mem;
            continue;
        }
    }

    ReleaseUIA(instance->pUIAHook);
    instance->pUIAHook = nullptr;

    return 0;
}

LRESULT CALLBACK SelectionHookCore::MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    auto *inst = currentInstance.load();
    if (nCode == HC_ACTION && inst && !inst->is_processing.load() &&
        !(wParam == WM_MOUSEMOVE && !inst->is_enabled_mouse_move.load()))
    {
        auto *pMouseInfo = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
        auto *mem = new MouseEventMsg();
        mem->event = wParam;
        mem->ptX = pMouseInfo->pt.x;
        mem->ptY = pMouseInfo->pt.y;
        mem->mouseData = pMouseInfo->mouseData;
        if (!PostThreadMessage(inst->hook_thread_id, WM_SH_MOUSE_EVENT, reinterpret_cast<WPARAM>(mem), 0))
            delete mem;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK SelectionHookCore::KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    auto *inst = currentInstance.load();
    if (nCode == HC_ACTION && inst && !inst->is_processing.load())
    {
        auto *pKeyInfo = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        auto *mem = new KeyboardEventMsg();
        mem->event = wParam;
        mem->vkCode = pKeyInfo->vkCode;
        mem->scanCode = pKeyInfo->scanCode;
        mem->flags = pKeyInfo->flags;
        if (!PostThreadMessage(inst->hook_thread_id, WM_SH_KEYBOARD_EVENT, reinterpret_cast<WPARAM>(mem), 0))
            delete mem;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void SelectionHookCore::processMouseEvent(WPARAM mEvent, LONG ptX, LONG ptY, DWORD mouseData)
{
    if (!shouldProcessGetSelection())
        return;

    POINT currentPos = {ptX, ptY};
    auto nMouseData = mouseData;

    static POINT lastLastMouseUpPos = {0, 0};
    static POINT lastMouseUpPos = {0, 0};
    static DWORD lastMouseUpTime = 0;
    static POINT lastMouseDownPos = {0, 0};
    static DWORD lastMouseDownTime = 0;

    static bool isLastValidClick = false;

    static HWND lastWindowHandler = nullptr;
    static RECT lastWindowRect = {0};

    static DWORD mouseButtonPressingFlag = 0;

    bool shouldDetectSelection = false;
    auto detectionType = SelectionDetectType::None;
    int32_t mouseEventType = -1;
    int32_t mouseButton = -1;
    int32_t mouseFlag = 0;

    switch (mEvent)
    {
        case WM_LBUTTONDOWN:
        {
            mouseEventType = 0;
            mouseButton = 0;
            mouseButtonPressingFlag |= 0x01;

            lastMouseDownTime = GetTickCount();
            lastMouseDownPos = currentPos;

            lastWindowHandler = GetWindowUnderMouse();
            if (lastWindowHandler)
            {
                GetWindowRect(lastWindowHandler, &lastWindowRect);
            }

            if (is_enabled_clipboard.load())
            {
                CURSORINFO ci = {sizeof(CURSORINFO)};
                GetCursorInfo(&ci);
                mouse_down_cursor = ci.hCursor;
            }

            clipboard_sequence = GetClipboardSequenceNumber();
            break;
        }
        case WM_LBUTTONUP:
        {
            mouseEventType = 1;
            mouseButton = 0;
            mouseButtonPressingFlag &= ~0x01;

            DWORD currentTime = GetTickCount();

            if (!is_selection_passive_mode.load())
            {
                int dx = currentPos.x - lastMouseDownPos.x;
                int dy = currentPos.y - lastMouseDownPos.y;
                double distance = sqrt(static_cast<double>(dx * dx + dy * dy));

                bool isCurrentValidClick = (currentTime - lastMouseDownTime) <= DOUBLE_CLICK_TIME_MS;

                if ((currentTime - lastMouseDownTime) > MAX_DRAG_TIME_MS)
                {
                    shouldDetectSelection = false;
                }
                else if (distance >= MIN_DRAG_DISTANCE)
                {
                    HWND hwnd = GetWindowUnderMouse();
                    if (hwnd && hwnd == lastWindowHandler)
                    {
                        RECT currentWindowRect;
                        GetWindowRect(hwnd, &currentWindowRect);
                        if (!HasWindowMoved(currentWindowRect, lastWindowRect))
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::Drag;
                        }
                    }
                }
                else if (isLastValidClick && isCurrentValidClick && distance <= DOUBLE_CLICK_MAX_DISTANCE)
                {
                    int d2x = currentPos.x - lastMouseUpPos.x;
                    int d2y = currentPos.y - lastMouseUpPos.y;
                    double d2 = sqrt(static_cast<double>(d2x * d2x + d2y * d2y));

                    if (d2 <= DOUBLE_CLICK_MAX_DISTANCE &&
                        (lastMouseDownTime - lastMouseUpTime) <= DOUBLE_CLICK_TIME_MS)
                    {
                        HWND hwnd = GetWindowUnderMouse();
                        if (hwnd && hwnd == lastWindowHandler)
                        {
                            RECT currentWindowRect;
                            GetWindowRect(hwnd, &currentWindowRect);
                            if (!HasWindowMoved(currentWindowRect, lastWindowRect))
                            {
                                shouldDetectSelection = true;
                                detectionType = SelectionDetectType::DoubleClick;
                            }
                        }
                    }
                }

                if (!shouldDetectSelection)
                {
                    bool isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    bool isCtrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    bool isAltPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    if (isShiftPressed && !isCtrlPressed && !isAltPressed)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::ShiftClick;
                    }
                }

                if (shouldDetectSelection && is_enabled_clipboard.load())
                {
                    CURSORINFO ci = {sizeof(CURSORINFO)};
                    GetCursorInfo(&ci);
                    mouse_up_cursor = ci.hCursor;
                }

                isLastValidClick = isCurrentValidClick;
            }

            lastLastMouseUpPos = lastMouseUpPos;
            lastMouseUpTime = currentTime;
            lastMouseUpPos = currentPos;
            break;
        }

        case WM_MOUSEMOVE:
        {
            mouseEventType = 2;
            if (mouseButtonPressingFlag & 0x01)
                mouseButton = 0;
            else if (mouseButtonPressingFlag & 0x02)
                mouseButton = 2;
            else if (mouseButtonPressingFlag & 0x04)
                mouseButton = 1;
            else if (mouseButtonPressingFlag & 0x08)
                mouseButton = 3;
            else if (mouseButtonPressingFlag & 0x10)
                mouseButton = 4;
            else
                mouseButton = -1;
            break;
        }

        case WM_RBUTTONDOWN:
            mouseEventType = 0;
            mouseButton = 2;
            mouseButtonPressingFlag |= 0x02;
            break;

        case WM_RBUTTONUP:
            mouseEventType = 1;
            mouseButton = 2;
            mouseButtonPressingFlag &= ~0x02;
            break;

        case WM_MBUTTONDOWN:
            mouseEventType = 0;
            mouseButton = 1;
            mouseButtonPressingFlag |= 0x04;
            break;

        case WM_MBUTTONUP:
            mouseEventType = 1;
            mouseButton = 1;
            mouseButtonPressingFlag &= ~0x04;
            break;

        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        {
            mouseEventType = (mEvent == WM_XBUTTONUP) ? 1 : 0;
            bool isXButton1 = (HIWORD(nMouseData) == XBUTTON1);
            mouseButton = isXButton1 ? 3 : 4;
            DWORD buttonBit = isXButton1 ? 0x08 : 0x10;
            if (mEvent == WM_XBUTTONDOWN)
                mouseButtonPressingFlag |= buttonBit;
            else
                mouseButtonPressingFlag &= ~buttonBit;
            break;
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            mouseEventType = 3;
            mouseButton = (mEvent == WM_MOUSEWHEEL) ? 0 : 1;
            mouseFlag = GET_WHEEL_DELTA_WPARAM(nMouseData) > 0 ? 1 : -1;
            break;
        }

        default:
            mouseEventType = -1;
            mouseButton = 99;
            break;
    }

    if (shouldDetectSelection)
    {
        TextSelectionInfo selectionInfo;
        HWND hwnd = GetForegroundWindow();

        if (hwnd)
        {
            if (getSelectedText(hwnd, selectionInfo) && !IsTrimmedEmpty(selectionInfo.text))
            {
                switch (detectionType)
                {
                    case SelectionDetectType::Drag:
                    {
                        selectionInfo.mousePosStart = lastMouseDownPos;
                        selectionInfo.mousePosEnd = lastMouseUpPos;
                        if (selectionInfo.posLevel == SH_POS_LEVEL_NONE)
                            selectionInfo.posLevel = SH_POS_LEVEL_MOUSE_DUAL;
                        break;
                    }
                    case SelectionDetectType::DoubleClick:
                    {
                        selectionInfo.mousePosStart = lastMouseUpPos;
                        selectionInfo.mousePosEnd = lastMouseUpPos;
                        if (selectionInfo.posLevel == SH_POS_LEVEL_NONE)
                            selectionInfo.posLevel = SH_POS_LEVEL_MOUSE_SINGLE;
                        break;
                    }
                    case SelectionDetectType::ShiftClick:
                    {
                        selectionInfo.mousePosStart = lastLastMouseUpPos;
                        selectionInfo.mousePosEnd = lastMouseUpPos;
                        if (selectionInfo.posLevel == SH_POS_LEVEL_NONE)
                            selectionInfo.posLevel = SH_POS_LEVEL_MOUSE_DUAL;
                        break;
                    }
                    default:
                        break;
                }

                dispatchSelection(selectionInfo);
            }
        }
    }

    if (mouseEventType >= 0)
    {
        dispatchMouseEvent(static_cast<int32_t>(currentPos.x), static_cast<int32_t>(currentPos.y), mouseButton,
                           mouseEventType, mouseFlag);
    }
}

void SelectionHookCore::processKeyboardEvent(WPARAM kEvent, DWORD vkCode, DWORD scanCode, DWORD flags)
{
    if (!shouldProcessGetSelection())
        return;

    int32_t eventType = -1;
    auto isSysKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                    (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);

    switch (kEvent)
    {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            eventType = 0;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            eventType = 1;
            break;
        default:
            break;
    }

    if (eventType >= 0)
    {
        std::string uniKey = convertKeyCodeToUniKey(vkCode, scanCode, flags);
        dispatchKeyboardEvent(uniKey, static_cast<int32_t>(vkCode), isSysKey ? 1 : 0, static_cast<int32_t>(flags));
    }
}

void SelectionHookCore::dispatchSelection(const TextSelectionInfo &info)
{
    auto cb = callback.load();
    if (!cb)
        return;

    cached_text = StringPool::WideToUtf8(info.text);
    cached_program = StringPool::WideToUtf8(info.programName);

    cached_sel_data.text = cached_text.c_str();
    cached_sel_data.program_name = cached_program.c_str();
    cached_sel_data.start_top = {static_cast<int32_t>(info.startTop.x), static_cast<int32_t>(info.startTop.y)};
    cached_sel_data.start_bottom = {static_cast<int32_t>(info.startBottom.x), static_cast<int32_t>(info.startBottom.y)};
    cached_sel_data.end_top = {static_cast<int32_t>(info.endTop.x), static_cast<int32_t>(info.endTop.y)};
    cached_sel_data.end_bottom = {static_cast<int32_t>(info.endBottom.x), static_cast<int32_t>(info.endBottom.y)};
    cached_sel_data.mouse_start = {static_cast<int32_t>(info.mousePosStart.x), static_cast<int32_t>(info.mousePosStart.y)};
    cached_sel_data.mouse_end = {static_cast<int32_t>(info.mousePosEnd.x), static_cast<int32_t>(info.mousePosEnd.y)};
    cached_sel_data.method = info.method;
    cached_sel_data.pos_level = info.posLevel;
    cached_sel_data.is_fullscreen = 0;

    if (!running.load())
        return;
    cb(&cached_sel_data);
}

void SelectionHookCore::dispatchMouseEvent(int32_t x, int32_t y, int32_t button, int32_t event_type, int32_t flag)
{
    auto cb = mouseCallback.load();
    if (!cb)
        return;

    cached_mouse_event.x = x;
    cached_mouse_event.y = y;
    cached_mouse_event.button = button;
    cached_mouse_event.event_type = event_type;
    cached_mouse_event.flag = flag;

    if (!running.load())
        return;
    cb(&cached_mouse_event);
}

void SelectionHookCore::dispatchKeyboardEvent(const std::string &uniKey, int32_t vkCode, int32_t sys, int32_t flags)
{
    auto cb = keyboardCallback.load();
    if (!cb)
        return;

    cached_uni_key = uniKey;
    cached_keyboard_event.uni_key = cached_uni_key.c_str();
    cached_keyboard_event.vk_code = vkCode;
    cached_keyboard_event.sys = sys;
    cached_keyboard_event.flags = flags;

    if (!running.load())
        return;
    cb(&cached_keyboard_event);
}

bool SelectionHookCore::getSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    if (is_processing.load())
        return false;
    is_processing.store(true);

    selectionInfo.clear();

    if (!GetProgramNameFromHwnd(hwnd, selectionInfo.programName))
    {
        selectionInfo.programName = L"";
        if (global_filter_mode == FilterMode::IncludeList)
        {
            is_processing.store(false);
            return false;
        }
    }
    else if (global_filter_mode != FilterMode::Default)
    {
        bool isIn = isInFilterList(selectionInfo.programName, global_filter_list);
        if ((global_filter_mode == FilterMode::IncludeList && !isIn) ||
            (global_filter_mode == FilterMode::ExcludeList && isIn))
        {
            is_processing.store(false);
            return false;
        }
    }

    if (pUIAHook && getTextViaUIAutomation(pUIAHook, hwnd, selectionInfo))
    {
        selectionInfo.method = SH_METHOD_UIA;
        is_processing.store(false);
        return true;
    }

    if (getTextViaAccessible(hwnd, selectionInfo))
    {
        selectionInfo.method = SH_METHOD_ACCESSIBLE;
        is_processing.store(false);
        return true;
    }

    if (shouldProcessViaClipboard(hwnd, selectionInfo.programName) && getTextViaClipboard(hwnd, selectionInfo))
    {
        selectionInfo.method = SH_METHOD_CLIPBOARD;
        is_processing.store(false);
        return true;
    }

    is_processing.store(false);
    return false;
}

bool SelectionHookCore::shouldProcessGetSelection()
{
    static std::atomic<bool> lastResult{true};
    static std::atomic<DWORD> lastCheckTime{0};

    if (GetTickCount() - lastCheckTime.load() < 10000)
    {
        return lastResult.load();
    }

    QUERY_USER_NOTIFICATION_STATE state;
    HRESULT hr = SHQueryUserNotificationState(&state);

    lastCheckTime.store(GetTickCount());

    if (FAILED(hr))
    {
        lastResult.store(true);
        return true;
    }

    bool r = state != QUNS_RUNNING_D3D_FULL_SCREEN && state != QUNS_PRESENTATION_MODE;
    lastResult.store(r);
    return r;
}

bool SelectionHookCore::shouldProcessViaClipboard(HWND hwnd, std::wstring &programName)
{
    if (!is_enabled_clipboard.load())
        return false;

    bool result = false;
    switch (clipboard_filter_mode)
    {
        case FilterMode::Default:
            result = true;
            break;
        case FilterMode::IncludeList:
            result = isInFilterList(programName, clipboard_filter_list);
            break;
        case FilterMode::ExcludeList:
            result = !isInFilterList(programName, clipboard_filter_list);
            break;
    }

    if (!result)
        return false;

    if (!is_triggered_by_user.load())
    {
        HCURSOR arrowCursor = LoadCursor(NULL, IDC_ARROW);
        HCURSOR beamCursor = LoadCursor(NULL, IDC_IBEAM);
        HCURSOR handCursor = LoadCursor(NULL, IDC_HAND);

        if (mouse_down_cursor != beamCursor && mouse_up_cursor != beamCursor)
        {
            if (mouse_up_cursor != arrowCursor && mouse_up_cursor != handCursor)
            {
                if (isInFilterList(programName, ftl_exclude_clipboard_cursor_detect))
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
            else if (uia_control_type != UIA_GroupControlTypeId && uia_control_type != UIA_DocumentControlTypeId &&
                     uia_control_type != UIA_TextControlTypeId)
            {
                return false;
            }
        }
    }

    return true;
}

bool SelectionHookCore::getTextViaUIAutomation(IUIAutomation *pUIA, HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!pUIA || !hwnd)
        return false;

    bool result = false;
    uia_control_type = UIA_WindowControlTypeId;

    IUIAutomationElement *pElement = nullptr;
    HRESULT hr = pUIA->ElementFromHandle(hwnd, &pElement);
    if (FAILED(hr) || !pElement)
        return false;

    {
        IUIAutomationElement *pFocusedElement = nullptr;
        hr = pUIA->GetFocusedElement(&pFocusedElement);
        pElement->Release();
        pElement = nullptr;

        if (FAILED(hr) || !pFocusedElement)
            return false;

        CONTROLTYPEID controlType;
        hr = pFocusedElement->get_CurrentControlType(&controlType);
        if (SUCCEEDED(hr))
        {
            uia_control_type = controlType;
        }

        {
            IUIAutomationTextPattern *pTextPattern = nullptr;
            hr = pFocusedElement->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                                      (void **)&pTextPattern);

            if (SUCCEEDED(hr) && pTextPattern)
            {
                IUIAutomationTextRangeArray *pRanges = nullptr;
                hr = pTextPattern->GetSelection(&pRanges);

                if (SUCCEEDED(hr) && pRanges)
                {
                    int count = 0;
                    hr = pRanges->get_Length(&count);

                    if (SUCCEEDED(hr) && count > 0)
                    {
                        for (int i = 0; i < count && !result; i++)
                        {
                            IUIAutomationTextRange *pRange = nullptr;
                            hr = pRanges->GetElement(i, &pRange);
                            if (SUCCEEDED(hr) && pRange)
                            {
                                BSTR bstr = nullptr;
                                hr = pRange->GetText(-1, &bstr);
                                if (SUCCEEDED(hr) && bstr)
                                {
                                    selectionInfo.text = std::wstring(bstr);
                                    if (!selectionInfo.text.empty())
                                    {
                                        result = setTextRangeCoordinates(pRange, selectionInfo);
                                    }
                                    SysFreeString(bstr);
                                }
                                pRange->Release();
                            }
                        }
                    }
                    pRanges->Release();
                }

                if (!result)
                {
                    IUIAutomationTextRange *pDocRange = nullptr;
                    hr = pTextPattern->get_DocumentRange(&pDocRange);
                    if (SUCCEEDED(hr) && pDocRange)
                    {
                        bool hasSelection = false;

                        {
                            VARIANT varSel;
                            VariantInit(&varSel);
                            BSTR bstr = nullptr;

                            HRESULT attrHr = pDocRange->GetAttributeValue(UIA_IsSelectionActivePropertyId, &varSel);
                            hr = pDocRange->GetText(-1, &bstr);

                            if (SUCCEEDED(hr) && SUCCEEDED(attrHr) && bstr && (varSel.vt == VT_BOOL) &&
                                (varSel.boolVal == VARIANT_TRUE))
                            {
                                std::wstring selectedText(bstr);
                                if (!selectedText.empty())
                                {
                                    selectionInfo.text = selectedText;
                                    if (setTextRangeCoordinates(pDocRange, selectionInfo))
                                    {
                                        result = true;
                                        hasSelection = true;
                                    }
                                }
                            }

                            if (bstr)
                                SysFreeString(bstr);
                            VariantClear(&varSel);
                        }

                        if (!hasSelection)
                        {
                            hr = pDocRange->ExpandToEnclosingUnit(TextUnit_Document);
                            if (SUCCEEDED(hr))
                            {
                                BSTR bstr = nullptr;
                                hr = pDocRange->GetText(-1, &bstr);
                                if (SUCCEEDED(hr) && bstr)
                                {
                                    VARIANT varSel;
                                    VariantInit(&varSel);
                                    hr = pDocRange->GetAttributeValue(UIA_IsSelectionActivePropertyId, &varSel);

                                    if (SUCCEEDED(hr) && (varSel.vt == VT_BOOL) && (varSel.boolVal == VARIANT_TRUE))
                                    {
                                        std::wstring docText(bstr);
                                        if (!docText.empty())
                                        {
                                            selectionInfo.text = docText;
                                            if (setTextRangeCoordinates(pDocRange, selectionInfo))
                                            {
                                                result = true;
                                            }
                                        }
                                    }

                                    VariantClear(&varSel);
                                    SysFreeString(bstr);
                                }
                            }
                        }
                        pDocRange->Release();
                    }
                }
                pTextPattern->Release();
            }
        }

        if (!result)
        {
            IUIAutomationLegacyIAccessiblePattern *pLegacyPattern = nullptr;
            hr = pFocusedElement->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                                      __uuidof(IUIAutomationLegacyIAccessiblePattern),
                                                      (void **)&pLegacyPattern);

            if (SUCCEEDED(hr) && pLegacyPattern)
            {
                VARIANT varSelf;
                VariantInit(&varSelf);
                varSelf.vt = VT_I4;
                varSelf.lVal = CHILDID_SELF;

                IAccessible *pAcc = nullptr;
                hr = pLegacyPattern->GetIAccessible(&pAcc);

                if (SUCCEEDED(hr) && pAcc)
                {
                    VARIANT varSel;
                    VariantInit(&varSel);
                    hr = pAcc->get_accSelection(&varSel);

                    if (SUCCEEDED(hr) && varSel.vt != VT_EMPTY)
                    {
                        if (varSel.vt == VT_BSTR && varSel.bstrVal)
                        {
                            selectionInfo.text = std::wstring(varSel.bstrVal);
                            if (!selectionInfo.text.empty())
                            {
                                result = true;
                            }
                        }
                        else if (varSel.vt == VT_DISPATCH && varSel.pdispVal)
                        {
                            IAccessible *pSelAcc = nullptr;
                            HRESULT dispHr = varSel.pdispVal->QueryInterface(IID_IAccessible, (void **)&pSelAcc);
                            if (SUCCEEDED(dispHr) && pSelAcc)
                            {
                                VARIANT childSelf;
                                VariantInit(&childSelf);
                                childSelf.vt = VT_I4;
                                childSelf.lVal = CHILDID_SELF;

                                BSTR bstr = nullptr;
                                if (SUCCEEDED(pSelAcc->get_accName(childSelf, &bstr)) && bstr &&
                                    SysStringLen(bstr) > 0)
                                {
                                    selectionInfo.text = std::wstring(bstr);
                                    result = !selectionInfo.text.empty();
                                }
                                else
                                {
                                    if (bstr)
                                        SysFreeString(bstr);
                                    if (SUCCEEDED(pSelAcc->get_accValue(childSelf, &bstr)) && bstr)
                                    {
                                        selectionInfo.text = std::wstring(bstr);
                                        result = !selectionInfo.text.empty();
                                    }
                                }

                                if (bstr)
                                    SysFreeString(bstr);
                                VariantClear(&childSelf);
                                pSelAcc->Release();
                            }
                        }
                        else if ((varSel.vt & VT_ARRAY) && varSel.parray)
                        {
                            SAFEARRAY *pArray = varSel.parray;
                            LONG lLower, lUpper;
                            if (SUCCEEDED(SafeArrayGetLBound(pArray, 1, &lLower)) &&
                                SUCCEEDED(SafeArrayGetUBound(pArray, 1, &lUpper)) && lLower <= lUpper)
                            {
                                VARIANT varItem;
                                VariantInit(&varItem);
                                if (SUCCEEDED(SafeArrayGetElement(pArray, &lLower, &varItem)))
                                {
                                    if (varItem.vt == VT_DISPATCH && varItem.pdispVal)
                                    {
                                        IAccessible *pItemAcc = nullptr;
                                        if (SUCCEEDED(varItem.pdispVal->QueryInterface(IID_IAccessible, (void **)&pItemAcc)))
                                        {
                                            VARIANT itemChild;
                                            VariantInit(&itemChild);
                                            itemChild.vt = VT_I4;
                                            itemChild.lVal = CHILDID_SELF;

                                            BSTR bstr = nullptr;
                                            if (SUCCEEDED(pItemAcc->get_accValue(itemChild, &bstr)) && bstr)
                                            {
                                                selectionInfo.text = std::wstring(bstr);
                                                result = !selectionInfo.text.empty();
                                                SysFreeString(bstr);
                                            }

                                            VariantClear(&itemChild);
                                            pItemAcc->Release();
                                        }
                                    }
                                    VariantClear(&varItem);
                                }
                            }
                        }
                    }
                    VariantClear(&varSel);
                    pAcc->Release();
                }
                VariantClear(&varSelf);
                pLegacyPattern->Release();
            }
        }

        pFocusedElement->Release();
    }

    return result;
}

bool SelectionHookCore::getTextViaAccessible(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    IAccessible *pAcc = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible, (void **)&pAcc);
    if (FAILED(hr) || !pAcc)
        return false;

    VARIANT varChild;
    VariantInit(&varChild);
    varChild.vt = VT_I4;
    varChild.lVal = CHILDID_SELF;

    VARIANT varSel;
    VariantInit(&varSel);
    hr = pAcc->get_accSelection(&varSel);

    bool result = false;

    if (SUCCEEDED(hr) && varSel.vt != VT_EMPTY)
    {
        if (varSel.vt == VT_DISPATCH && varSel.pdispVal)
        {
            IAccessible *pSelAcc = nullptr;
            hr = varSel.pdispVal->QueryInterface(IID_IAccessible, (void **)&pSelAcc);
            if (SUCCEEDED(hr) && pSelAcc)
            {
                VARIANT varSelChild;
                VariantInit(&varSelChild);
                varSelChild.vt = VT_I4;
                varSelChild.lVal = CHILDID_SELF;

                BSTR bstr = nullptr;
                hr = pSelAcc->get_accName(varSelChild, &bstr);
                if (SUCCEEDED(hr) && bstr && SysStringLen(bstr) > 0)
                {
                    selectionInfo.text = std::wstring(bstr);
                    SysFreeString(bstr);
                    bstr = nullptr;
                }
                else
                {
                    if (bstr)
                    {
                        SysFreeString(bstr);
                        bstr = nullptr;
                    }
                    hr = pSelAcc->get_accValue(varSelChild, &bstr);
                    if (SUCCEEDED(hr) && bstr)
                    {
                        selectionInfo.text = std::wstring(bstr);
                        SysFreeString(bstr);
                    }
                }

                if (!selectionInfo.text.empty())
                {
                    result = true;
                    LONG x = 0, y = 0, width = 0, height = 0;
                    if (SUCCEEDED(pSelAcc->accLocation(&x, &y, &width, &height, varSelChild)))
                    {
                        selectionInfo.startTop.x = x;
                        selectionInfo.startTop.y = y;
                        selectionInfo.startBottom.x = x;
                        selectionInfo.startBottom.y = y + height;
                        selectionInfo.endTop.x = x + width;
                        selectionInfo.endTop.y = y;
                        selectionInfo.endBottom.x = x + width;
                        selectionInfo.endBottom.y = y + height;
                        selectionInfo.posLevel = SH_POS_LEVEL_SEL_FULL;
                    }
                }

                pSelAcc->Release();
            }
        }
        else if (varSel.vt == (VT_ARRAY | VT_VARIANT) || varSel.vt == (VT_ARRAY | VT_I4))
        {
            SAFEARRAY *pArray = varSel.parray;
            if (pArray)
            {
                LONG lLower, lUpper;
                if (SUCCEEDED(SafeArrayGetLBound(pArray, 1, &lLower)) &&
                    SUCCEEDED(SafeArrayGetUBound(pArray, 1, &lUpper)))
                {
                    if (lLower <= lUpper)
                    {
                        VARIANT varItem;
                        VariantInit(&varItem);
                        if (SUCCEEDED(SafeArrayGetElement(pArray, &lLower, &varItem)))
                        {
                            if (varItem.vt == VT_DISPATCH && varItem.pdispVal)
                            {
                                IAccessible *pItemAcc = nullptr;
                                if (SUCCEEDED(varItem.pdispVal->QueryInterface(IID_IAccessible, (void **)&pItemAcc)))
                                {
                                    VARIANT varItemChild;
                                    VariantInit(&varItemChild);
                                    varItemChild.vt = VT_I4;
                                    varItemChild.lVal = CHILDID_SELF;

                                    BSTR bstr = nullptr;
                                    hr = pItemAcc->get_accValue(varItemChild, &bstr);
                                    if (SUCCEEDED(hr) && bstr)
                                    {
                                        selectionInfo.text = std::wstring(bstr);
                                        SysFreeString(bstr);
                                        result = !selectionInfo.text.empty();
                                    }

                                    pItemAcc->Release();
                                }
                            }
                            VariantClear(&varItem);
                        }
                    }
                }
            }
        }

        VariantClear(&varSel);
    }

    pAcc->Release();
    return result;
}

bool SelectionHookCore::getTextViaClipboard(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    constexpr DWORD SLEEP_INTERVAL = 5;

    if (!is_triggered_by_user.load())
    {
        bool isCtrlPressed = false;
        bool isCPressed = false;
        bool isXPressed = false;
        bool isVPressed = false;
        int checkCount = 0;
        const int maxChecks = 5;

        while (checkCount < maxChecks)
        {
            if (GetClipboardSequenceNumber() != clipboard_sequence)
            {
                if (!ReadClipboard(selectionInfo.text) || selectionInfo.text.empty())
                {
                    return false;
                }
                return true;
            }

            bool isCtrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool isCPressing = (GetAsyncKeyState('C') & 0x8000) != 0;
            bool isXPressing = (GetAsyncKeyState('X') & 0x8000) != 0;
            bool isVPressing = (GetAsyncKeyState('V') & 0x8000) != 0;

            if (!isCtrlPressing && !isCPressing && !isXPressing && !isVPressing)
            {
                break;
            }

            if (isCtrlPressing)
                isCtrlPressed = true;
            if (isCPressing)
                isCPressed = true;
            if (isXPressing)
                isXPressed = true;
            if (isVPressing)
                isVPressed = true;

            checkCount++;
            Sleep(40);
        }

        if (checkCount >= maxChecks)
        {
            return false;
        }

        if (isCtrlPressed && (isCPressed || isXPressed || isVPressed))
        {
            return false;
        }
    }

    ClipboardBackup clipboardBackup;
    if (OpenClipboard(nullptr))
    {
        BackupClipboard(clipboardBackup, true);
        EmptyClipboard();
        CloseClipboard();
    }
    else
    {
        return false;
    }

    bool isInDelayReadList =
        !selectionInfo.programName.empty() && isInFilterList(selectionInfo.programName, ftl_include_clipboard_delay_read);

    if (!isInDelayReadList)
    {
        if (shouldKeyInterruptViaClipboard())
        {
            if (clipboardBackup.HasData())
            {
                RestoreClipboard(clipboardBackup);
                clipboard_sequence = GetClipboardSequenceNumber();
            }
            return false;
        }

        clipboard_sequence = GetClipboardSequenceNumber();
        sendCopyKey(static_cast<int>(CopyKeyType::CtrlInsert));

        bool hasNewContent = false;
        for (int i = 0; i < 20; i++)
        {
            if (GetClipboardSequenceNumber() != clipboard_sequence)
            {
                hasNewContent = true;
                break;
            }
            Sleep(SLEEP_INTERVAL);
        }

        if (hasNewContent)
        {
            Sleep(10);
            bool readSuccess = ReadClipboard(selectionInfo.text);

            if (clipboardBackup.HasData())
            {
                RestoreClipboard(clipboardBackup);
                clipboard_sequence = GetClipboardSequenceNumber();
            }

            if (!readSuccess || selectionInfo.text.empty())
                return false;
            return true;
        }
    }

    if (shouldKeyInterruptViaClipboard())
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboard_sequence = GetClipboardSequenceNumber();
        }
        return false;
    }

    clipboard_sequence = GetClipboardSequenceNumber();
    sendCopyKey(static_cast<int>(CopyKeyType::CtrlC));

    bool hasNewContent = false;
    for (int i = 0; i < 36; i++)
    {
        if (GetClipboardSequenceNumber() != clipboard_sequence)
        {
            hasNewContent = true;
            break;
        }
        Sleep(SLEEP_INTERVAL);
    }

    if (!hasNewContent)
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboard_sequence = GetClipboardSequenceNumber();
        }
        return false;
    }

    if (isInDelayReadList)
    {
        Sleep(135);
    }
    Sleep(10);

    if (shouldKeyInterruptViaClipboard())
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboard_sequence = GetClipboardSequenceNumber();
        }
        return false;
    }

    bool readSuccess = ReadClipboard(selectionInfo.text);

    if (clipboardBackup.HasData())
    {
        RestoreClipboard(clipboardBackup);
        clipboard_sequence = GetClipboardSequenceNumber();
    }

    if (!readSuccess || selectionInfo.text.empty())
        return false;

    return true;
}

void SelectionHookCore::sendCopyKey(int type)
{
    bool isCPressing = (GetAsyncKeyState('C') & 0x8000) != 0;
    bool isCtrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool isAltPressing = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool isShiftPressing = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if (isCtrlPressing && isCPressing)
        return;

    WORD keyCode = (type == static_cast<int>(CopyKeyType::CtrlInsert)) ? VK_INSERT : 'C';

    std::vector<INPUT> inputs;
    INPUT input = {};

    if (isAltPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_MENU;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }

    if (isShiftPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_SHIFT;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }

    if (!isCtrlPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_RCONTROL;
        input.ki.dwFlags = 0;
        inputs.push_back(input);
    }

    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keyCode;
    input.ki.dwFlags = 0;
    inputs.push_back(input);

    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keyCode;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(input);

    if (!isCtrlPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_RCONTROL;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

bool SelectionHookCore::shouldKeyInterruptViaClipboard()
{
    bool isCtrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!is_triggered_by_user.load() && isCtrlPressing)
    {
        return true;
    }
    return false;
}

bool SelectionHookCore::setTextRangeCoordinates(IUIAutomationTextRange *pRange, TextSelectionInfo &selectionInfo)
{
    if (!pRange)
        return false;

    SAFEARRAY *pRectArray = nullptr;
    HRESULT hr = pRange->GetBoundingRectangles(&pRectArray);

    if (SUCCEEDED(hr) && pRectArray)
    {
        double *pRects = nullptr;
        HRESULT accessHr = SafeArrayAccessData(pRectArray, (void **)&pRects);

        if (FAILED(accessHr) || !pRects)
        {
            SafeArrayDestroy(pRectArray);
            return false;
        }

        LONG lowerBound, upperBound;
        SafeArrayGetLBound(pRectArray, 1, &lowerBound);
        SafeArrayGetUBound(pRectArray, 1, &upperBound);

        int rectCount = (upperBound - lowerBound + 1) / 4;

        int firstValidRectIndex = -1;
        for (int i = 0; i < rectCount; i++)
        {
            int rectIndex = i * 4;
            double width = pRects[rectIndex + 2];
            double height = pRects[rectIndex + 3];
            if (width > 1.0 && height < 100.0)
            {
                firstValidRectIndex = rectIndex;
                break;
            }
        }

        int lastValidRectIndex = -1;
        for (int i = rectCount - 1; i >= 0; i--)
        {
            int rectIndex = i * 4;
            double width = pRects[rectIndex + 2];
            double height = pRects[rectIndex + 3];
            if (width > 1.0 && height < 100.0)
            {
                lastValidRectIndex = rectIndex;
                break;
            }
        }

        bool found = false;

        if (firstValidRectIndex >= 0 && lastValidRectIndex >= 0)
        {
            selectionInfo.startTop.x = static_cast<LONG>(pRects[firstValidRectIndex]);
            selectionInfo.startTop.y = static_cast<LONG>(pRects[firstValidRectIndex + 1]);
            selectionInfo.startBottom.x = static_cast<LONG>(pRects[firstValidRectIndex]);
            selectionInfo.startBottom.y = static_cast<LONG>(pRects[firstValidRectIndex + 1] +
                                                            pRects[firstValidRectIndex + 3]);
            selectionInfo.endBottom.x = static_cast<LONG>(pRects[lastValidRectIndex] + pRects[lastValidRectIndex + 2]);
            selectionInfo.endBottom.y = static_cast<LONG>(pRects[lastValidRectIndex + 1] +
                                                          pRects[lastValidRectIndex + 3]);
            selectionInfo.endTop.x = static_cast<LONG>(pRects[lastValidRectIndex] + pRects[lastValidRectIndex + 2]);
            selectionInfo.endTop.y = static_cast<LONG>(pRects[lastValidRectIndex + 1]);
            selectionInfo.posLevel = SH_POS_LEVEL_SEL_FULL;
            found = true;
        }

        SafeArrayUnaccessData(pRectArray);
        SafeArrayDestroy(pRectArray);
        return found;
    }

    return false;
}

bool SelectionHookCore::isInFilterList(const std::wstring &programName, const std::vector<std::string> &filterList)
{
    if (filterList.empty())
        return false;

    std::wstring lowerProgramName = programName;
    std::transform(lowerProgramName.begin(), lowerProgramName.end(), lowerProgramName.begin(), towlower);
    std::string utf8ProgramName = StringPool::WideToUtf8(lowerProgramName);

    for (const auto &filterItem : filterList)
    {
        if (utf8ProgramName.find(filterItem) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

void SelectionHookCore::enableDpiAwareness()
{
    typedef HRESULT(WINAPI * SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);

    HMODULE shcore = LoadLibraryA("Shcore.dll");
    if (shcore)
    {
        auto func = reinterpret_cast<SetProcessDpiAwareness_t>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (func)
        {
            func(PROCESS_PER_MONITOR_DPI_AWARE);
        }
        else
        {
            SetProcessDPIAware();
        }
        FreeLibrary(shcore);
    }
    else
    {
        SetProcessDPIAware();
    }
}
