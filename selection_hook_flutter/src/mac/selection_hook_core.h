/**
 * Text Selection Hook Core for macOS
 *
 * Ported from upstream selection-hook (Node N-API → C-ABI for Dart FFI).
 * Strips all Napi::* types; replaces ThreadSafeFunction with a plain C function pointer.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Ported under the MIT License
 */

#ifndef SELECTION_HOOK_CORE_H
#define SELECTION_HOOK_CORE_H

#include <atomic>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>

#include "../bridge/c_api.h"

enum class FilterMode
{
    Default = 0,
    IncludeList = 1,
    ExcludeList = 2
};

struct TextSelectionInfo
{
    std::string text;
    std::string programName;

    CGPoint startTop;
    CGPoint startBottom;
    CGPoint endTop;
    CGPoint endBottom;
    CGPoint mousePosStart;
    CGPoint mousePosEnd;

    SHSelectionMethod method;
    SHPositionLevel posLevel;
    bool isFullscreen;

    TextSelectionInfo()
        : method(SH_METHOD_NONE), posLevel(SH_POS_LEVEL_NONE), isFullscreen(false)
    {
        startTop = CGPointZero;
        startBottom = CGPointZero;
        endTop = CGPointZero;
        endBottom = CGPointZero;
        mousePosStart = CGPointZero;
        mousePosEnd = CGPointZero;
    }

    void clear()
    {
        text.clear();
        programName.clear();
        startTop = CGPointZero;
        startBottom = CGPointZero;
        endTop = CGPointZero;
        endBottom = CGPointZero;
        mousePosStart = CGPointZero;
        mousePosEnd = CGPointZero;
        method = SH_METHOD_NONE;
        posLevel = SH_POS_LEVEL_NONE;
        isFullscreen = false;
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

    static bool isProcessTrusted();
    static bool requestProcessTrust();

  private:
    void startEventThread();
    void stopEventThread();
    void eventThreadProc();

    bool getSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);
    bool getTextViaAXAPI(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);
    bool getSelectedTextFromElement(AXUIElementRef element, std::string &text);
    bool setTextRangeCoordinates(AXUIElementRef element, TextSelectionInfo &selectionInfo);

    static CGEventRef mouseEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
    static CGEventRef keyboardEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
    void processMouseEvent(CGEventType type, CGPoint pos, int64_t button, int64_t flag, CGEventFlags evFlags);
    void dispatchSelection(const TextSelectionInfo &info);
    void fillSHSelectionData(const TextSelectionInfo &info, SHSelectionData &out) const;

    std::atomic<bool> running{false};
    SHSelectionCallback callback{nullptr};

    pid_t running_pid{0};
    mutable std::string last_error;

    // String buffers for callback data — heap-allocated (SelectionHookCore is heap object).
    // dispatchSelection copies selectionInfo strings here before calling callback,
    // ensuring c_str() pointers survive Dart FFI marshalling.
    std::string cached_text;
    std::string cached_program;
    SHSelectionData cached_sel_data{};

    CFRunLoopRef eventRunLoop{nullptr};
    CFMachPortRef mouseEventTap{nullptr};
    CFMachPortRef keyboardEventTap{nullptr};
    CFRunLoopSourceRef mouseRunLoopSource{nullptr};
    CFRunLoopSourceRef keyboardRunLoopSource{nullptr};
    std::thread event_thread;

    std::atomic<bool> is_processing{false};
    bool is_triggered_by_user{false};
    bool is_selection_passive_mode{false};

    bool is_enabled_clipboard{true};
    int64_t clipboard_sequence{0};
    FilterMode clipboard_filter_mode{FilterMode::Default};
    std::vector<std::string> clipboard_filter_list;
    FilterMode global_filter_mode{FilterMode::Default};
    std::vector<std::string> global_filter_list;

    static SelectionHookCore *currentInstance;
};

#endif // SELECTION_HOOK_CORE_H
