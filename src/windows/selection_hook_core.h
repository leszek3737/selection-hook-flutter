/**
 * Text Selection Hook Core for Windows
 *
 * Ported from upstream selection-hook (Node N-API -> C-ABI for Dart FFI).
 * Strips all Napi::* types; replaces ThreadSafeFunction with plain C function
 * pointers (SHSelectionCallback, SHMouseCallback, SHKeyboardCallback).
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Ported under the MIT License
 */

#ifndef SELECTION_HOOK_CORE_WINDOWS_H
#define SELECTION_HOOK_CORE_WINDOWS_H

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include <objbase.h>
#include <UIAutomation.h>
#include <oleacc.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../bridge/c_api.h"

enum class FilterMode
{
    Default = 0,
    IncludeList = 1,
    ExcludeList = 2
};

struct TextSelectionInfo
{
    std::wstring text;
    std::wstring programName;

    POINT startTop;
    POINT startBottom;
    POINT endTop;
    POINT endBottom;
    POINT mousePosStart;
    POINT mousePosEnd;

    SHSelectionMethod method;
    SHPositionLevel posLevel;

    TextSelectionInfo() : method(SH_METHOD_NONE), posLevel(SH_POS_LEVEL_NONE)
    {
        startTop = startBottom = endTop = endBottom = {0, 0};
        mousePosStart = mousePosEnd = {0, 0};
    }

    void clear()
    {
        text.clear();
        programName.clear();
        startTop = startBottom = endTop = endBottom = {0, 0};
        mousePosStart = mousePosEnd = {0, 0};
        method = SH_METHOD_NONE;
        posLevel = SH_POS_LEVEL_NONE;
    }
};

class SelectionHookCore
{
  public:
    SelectionHookCore();
    ~SelectionHookCore();

    void setCallback(SHSelectionCallback callback);

    bool start();
    bool stop();
    bool isRunning() const;

    TextSelectionInfo *getCurrentSelection();

    const char *lastError() const;

    void setMouseCallback(SHMouseCallback callback);
    void setKeyboardCallback(SHKeyboardCallback callback);

    void setPassiveMode(bool passive);
    void setClipboardEnabled(bool enabled);
    void setClipboardMode(FilterMode mode, const std::vector<std::string> &programList);
    void setGlobalFilterMode(FilterMode mode, const std::vector<std::string> &programList);
    void setDebugEnabled(bool enabled);
    void enableMouseMove();
    void disableMouseMove();

  private:
    static DWORD WINAPI HookThreadProc(LPVOID lpParam);
    static LRESULT CALLBACK MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam);

    void processMouseEvent(WPARAM event, LONG ptX, LONG ptY, DWORD mouseData);
    void processKeyboardEvent(WPARAM event, DWORD vkCode, DWORD scanCode, DWORD flags);

    void dispatchSelection(const TextSelectionInfo &info);
    void dispatchMouseEvent(int32_t x, int32_t y, int32_t button, int32_t event_type, int32_t flag);
    void dispatchKeyboardEvent(const std::string &uniKey, int32_t vkCode, int32_t sys, int32_t flags);

    bool getSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool getTextViaUIAutomation(IUIAutomation *pUIA, HWND hwnd, TextSelectionInfo &selectionInfo);
    bool getTextViaAccessible(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool getTextViaClipboard(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool shouldProcessGetSelection();
    bool shouldProcessViaClipboard(HWND hwnd, std::wstring &programName);
    bool setTextRangeCoordinates(IUIAutomationTextRange *pRange, TextSelectionInfo &selectionInfo);

    bool isInFilterList(const std::wstring &programName, const std::vector<std::string> &filterList);
    void sendCopyKey(int type);
    bool shouldKeyInterruptViaClipboard();
    void enableDpiAwareness();

    std::atomic<bool> running{false};
    std::atomic<SHSelectionCallback> callback{nullptr};
    std::atomic<SHMouseCallback> mouseCallback{nullptr};
    std::atomic<SHKeyboardCallback> keyboardCallback{nullptr};

    HANDLE hook_thread{NULL};
    DWORD hook_thread_id{0};

    IUIAutomation *pUIAHook{nullptr};
    CONTROLTYPEID uia_control_type{UIA_WindowControlTypeId};

    std::atomic<bool> is_processing{false};
    std::atomic<bool> is_triggered_by_user{false};
    std::atomic<bool> is_selection_passive_mode{false};
    std::atomic<bool> is_enabled_clipboard{true};
    std::atomic<bool> is_enabled_mouse_move{false};
    std::atomic<bool> debug_enabled{false};

    DWORD clipboard_sequence{0};
    HCURSOR mouse_down_cursor{NULL};
    HCURSOR mouse_up_cursor{NULL};

    FilterMode clipboard_filter_mode{FilterMode::Default};
    std::vector<std::string> clipboard_filter_list;
    FilterMode global_filter_mode{FilterMode::Default};
    std::vector<std::string> global_filter_list;
    std::vector<std::string> ftl_exclude_clipboard_cursor_detect;
    std::vector<std::string> ftl_include_clipboard_delay_read;

    std::string cached_text;
    std::string cached_program;
    SHSelectionData cached_sel_data{};
    SHMouseEventData cached_mouse_event{};
    SHKeyboardEventData cached_keyboard_event{};
    std::string cached_uni_key;

    mutable std::mutex error_mutex;
    mutable std::string last_error;

    static std::atomic<SelectionHookCore *> currentInstance;
};

#endif // SELECTION_HOOK_CORE_WINDOWS_H
