/**
 * Text Selection Hook Core for macOS
 *
 * Ported from upstream selection-hook (Node N-API → C-ABI for Dart FFI).
 * Strips all Napi::* types; replaces ThreadSafeFunction with a plain C function pointer.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Ported under the MIT License
 */

#include "selection_hook_core.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#import "lib/clipboard.h"
#import "lib/keyboard.h"
#import "lib/utils.h"

// Mouse&Keyboard hook constants
constexpr int DEFAULT_MOUSE_EVENT_QUEUE_SIZE = 512;
constexpr int DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE = 128;

// Mouse interaction constants
constexpr int MIN_DRAG_DISTANCE = 8;
constexpr uint64_t MAX_DRAG_TIME_MS = 15000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static uint64_t DOUBLE_CLICK_TIME_MS = 500;

// Text selection detection type enum
enum class SelectionDetectType
{
    None = 0,
    Drag = 1,
    DoubleClick = 2,
    ShiftClick = 3
};

// Mouse button enum
enum class MouseButton
{
    None = -1,
    Unknown = 99,
    Left = 0,
    Middle = 1,
    Right = 2,
    Back = 3,
    Forward = 4,
    WheelVertical = 0,
    WheelHorizontal = 1
};

// Mouse tracking state (ported from upstream static locals in ProcessMouseEvent)
static CGPoint s_lastLastMouseUpPos = CGPointZero;
static CGPoint s_lastMouseUpPos = CGPointZero;
static uint64_t s_lastMouseUpTime = 0;
static CGPoint s_lastMouseDownPos = CGPointZero;
static uint64_t s_lastMouseDownTime = 0;
static bool s_isLastValidClick = false;
static bool s_isLastMouseDownValidCursor = false;

// Run loop synchronization
static std::promise<CFRunLoopRef> s_eventRunLoopPromise;
static std::future<CFRunLoopRef> s_eventRunLoopFuture;

// Static pointer for callbacks
SelectionHookCore *SelectionHookCore::currentInstance = nullptr;

SelectionHookCore::SelectionHookCore()
{
    currentInstance = this;
    running_pid = getpid();
}

SelectionHookCore::~SelectionHookCore()
{
    stop();

    if (currentInstance == this)
    {
        currentInstance = nullptr;
    }
}

void SelectionHookCore::setCallback(SHSelectionCallback cb)
{
    callback = cb;
}

bool SelectionHookCore::start()
{
    if (running)
    {
        return false;
    }

    if (!requestProcessTrust())
    {
        last_error = "Process is not trusted for accessibility";
        return false;
    }

    running = true;
    startEventThread();
    return true;
}

bool SelectionHookCore::stop()
{
    if (!running && !event_thread.joinable())
    {
        return false;
    }

    running = false;
    stopEventThread();
    return true;
}

bool SelectionHookCore::isRunning() const
{
    return running.load();
}

TextSelectionInfo *SelectionHookCore::getCurrentSelection()
{
    NSRunningApplication *frontApp = GetFrontApp();
    if (!frontApp)
    {
        return nullptr;
    }

    TextSelectionInfo *result = new TextSelectionInfo();
    is_triggered_by_user = true;
    if (!getSelectedText(frontApp, *result) || IsTrimmedEmpty(result->text))
    {
        is_triggered_by_user = false;
        delete result;
        return nullptr;
    }

    is_triggered_by_user = false;
    return result;
}

const char *SelectionHookCore::lastError() const
{
    return last_error.c_str();
}

bool SelectionHookCore::isProcessTrusted()
{
    return AXIsProcessTrusted();
}

bool SelectionHookCore::requestProcessTrust()
{
    CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, (const void **)&kAXTrustedCheckOptionPrompt,
                                                  (const void **)&kCFBooleanTrue, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    bool isTrusted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    return isTrusted;
}

void SelectionHookCore::startEventThread()
{
    if (event_thread.joinable())
    {
        return;
    }

    s_eventRunLoopPromise = std::promise<CFRunLoopRef>();
    s_eventRunLoopFuture = s_eventRunLoopPromise.get_future();

    event_thread = std::thread(&SelectionHookCore::eventThreadProc, this);
}

void SelectionHookCore::stopEventThread()
{
    if (!event_thread.joinable())
    {
        eventRunLoop = nullptr;
        mouseEventTap = nullptr;
        keyboardEventTap = nullptr;
        mouseRunLoopSource = nullptr;
        keyboardRunLoopSource = nullptr;
        return;
    }

    if (s_eventRunLoopFuture.valid())
    {
        CFRunLoopRef rl = s_eventRunLoopFuture.get();
        if (rl)
        {
            CFRunLoopStop(rl);
        }
    }
    else if (eventRunLoop)
    {
        CFRunLoopStop(eventRunLoop);
    }

    event_thread.join();
}

void SelectionHookCore::eventThreadProc()
{
    eventRunLoop = CFRunLoopGetCurrent();
    s_eventRunLoopPromise.set_value(eventRunLoop);

    CGEventMask mouseMask = CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
                            CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDown) |
                            CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventRightMouseDragged) |
                            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
                            CGEventMaskBit(kCGEventOtherMouseDragged) | CGEventMaskBit(kCGEventMouseMoved) |
                            CGEventMaskBit(kCGEventScrollWheel);

    mouseEventTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly, mouseMask,
                                     mouseEventCallback, this);

    if (mouseEventTap)
    {
        mouseRunLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, mouseEventTap, 0);
        CFRunLoopAddSource(eventRunLoop, mouseRunLoopSource, kCFRunLoopDefaultMode);
        CGEventTapEnable(mouseEventTap, true);
    }
    else
    {
        NSLog(@"SelectionHook: CGEventTapCreate failed — check Input Monitoring permission or disable sandbox");
    }

    CGEventMask keyboardMask =
        CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);

    keyboardEventTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly,
                                        keyboardMask, keyboardEventCallback, this);

    if (keyboardEventTap)
    {
        keyboardRunLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, keyboardEventTap, 0);
        CFRunLoopAddSource(eventRunLoop, keyboardRunLoopSource, kCFRunLoopDefaultMode);
        CGEventTapEnable(keyboardEventTap, true);
    }

    running_pid = getpid();

    CFRunLoopRun();

    if (mouseEventTap)
    {
        CGEventTapEnable(mouseEventTap, false);
        CFRunLoopRemoveSource(eventRunLoop, mouseRunLoopSource, kCFRunLoopDefaultMode);
        CFRelease(mouseEventTap);
        CFRelease(mouseRunLoopSource);
        mouseEventTap = nullptr;
        mouseRunLoopSource = nullptr;
    }

    if (keyboardEventTap)
    {
        CGEventTapEnable(keyboardEventTap, false);
        CFRunLoopRemoveSource(eventRunLoop, keyboardRunLoopSource, kCFRunLoopDefaultMode);
        CFRelease(keyboardEventTap);
        CFRelease(keyboardRunLoopSource);
        keyboardEventTap = nullptr;
        keyboardRunLoopSource = nullptr;
    }

    eventRunLoop = nullptr;
}

CGEventRef SelectionHookCore::mouseEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    SelectionHookCore *hook = static_cast<SelectionHookCore *>(refcon);
    if (!hook || !hook->running)
        return event;

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
    {
        CGEventTapEnable(hook->mouseEventTap, true);
        return event;
    }

    if (type == kCGEventMouseMoved)
        return event;

    int64_t button = -1;
    int64_t flag = 0;

    switch (type)
    {
        case kCGEventScrollWheel:
        {
            int64_t deltaY = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
            int64_t deltaX = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2);

            if (deltaY != 0)
            {
                button = 0;
                flag = (deltaY > 0) ? 1 : -1;
            }
            else if (deltaX != 0)
            {
                button = 1;
                flag = (deltaX > 0) ? 1 : -1;
            }
            else
            {
                return event;
            }
            break;
        }
        case kCGEventLeftMouseDown:
        case kCGEventRightMouseDown:
        case kCGEventOtherMouseDown:
        case kCGEventLeftMouseUp:
        case kCGEventRightMouseUp:
        case kCGEventOtherMouseUp:
            button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            break;

        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged:
            button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            break;

        default:
            break;
    }

    CGPoint pos = CGEventGetLocation(event);
    CGEventFlags flags = CGEventGetFlags(event);
    dispatch_async(dispatch_get_main_queue(), ^{
        hook->processMouseEvent(type, pos, button, flag, flags);
    });

    return event;
}

CGEventRef SelectionHookCore::keyboardEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                                                     void *refcon)
{
    SelectionHookCore *hook = static_cast<SelectionHookCore *>(refcon);
    if (!hook || !hook->running)
        return event;

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
    {
        CGEventTapEnable(hook->keyboardEventTap, true);
        return event;
    }

    return event;
}

void SelectionHookCore::processMouseEvent(CGEventType type, CGPoint pos, int64_t button, int64_t flag,
                                           CGEventFlags evFlags)
{
    if (!currentInstance)
    {
        return;
    }

    auto currentTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    CGPoint currentPos = pos;
    auto mouseType = type;

    bool shouldDetectSelection = false;
    auto detectionType = SelectionDetectType::None;

    switch (mouseType)
    {
        case kCGEventLeftMouseDown:
        {
            s_lastMouseDownTime = currentTime;
            s_lastMouseDownPos = currentPos;
            s_isLastMouseDownValidCursor = IsIBeamCursor([NSCursor currentSystemCursor]);
            clipboard_sequence = GetClipboardSequence();
            break;
        }
        case kCGEventLeftMouseUp:
        {
            if (!is_selection_passive_mode)
            {
                double dx = currentPos.x - s_lastMouseDownPos.x;
                double dy = currentPos.y - s_lastMouseDownPos.y;
                double distance = sqrt(dx * dx + dy * dy);

                bool isCurrentValidClick = (currentTime - s_lastMouseDownTime) <= DOUBLE_CLICK_TIME_MS;
                bool isValidCursor = s_isLastMouseDownValidCursor || IsIBeamCursor([NSCursor currentSystemCursor]);

                if ((currentTime - s_lastMouseDownTime) > MAX_DRAG_TIME_MS)
                {
                    shouldDetectSelection = false;
                }
                else if (distance >= MIN_DRAG_DISTANCE)
                {
                    if (isValidCursor)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::Drag;
                    }
                }
                else if (s_isLastValidClick && isCurrentValidClick && distance <= DOUBLE_CLICK_MAX_DISTANCE)
                {
                    double dx2 = currentPos.x - s_lastMouseUpPos.x;
                    double dy2 = currentPos.y - s_lastMouseUpPos.y;
                    double distance2 = sqrt(dx2 * dx2 + dy2 * dy2);

                    if (distance2 <= DOUBLE_CLICK_MAX_DISTANCE &&
                        (s_lastMouseDownTime - s_lastMouseUpTime) <= DOUBLE_CLICK_TIME_MS)
                    {
                        if (isValidCursor)
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::DoubleClick;
                        }
                    }
                }

                if (!shouldDetectSelection)
                {
                    CGEventFlags flags = evFlags;
                    bool isShiftPressed = (flags & kCGEventFlagMaskShift) != 0;
                    bool isCtrlPressed = (flags & kCGEventFlagMaskControl) != 0;
                    bool isCmdPressed = (flags & kCGEventFlagMaskCommand) != 0;
                    bool isOptionPressed = (flags & kCGEventFlagMaskAlternate) != 0;

                    if (isShiftPressed && !isCtrlPressed && !isCmdPressed && !isOptionPressed)
                    {
                        if (isValidCursor)
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::ShiftClick;
                        }
                    }
                }
                s_isLastValidClick = isCurrentValidClick;
            }

            s_lastLastMouseUpPos = s_lastMouseUpPos;
            s_lastMouseUpTime = currentTime;
            s_lastMouseUpPos = currentPos;
            break;
        }

        case kCGEventRightMouseDown:
        case kCGEventOtherMouseDown:
        case kCGEventRightMouseUp:
        case kCGEventOtherMouseUp:
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged:
        case kCGEventScrollWheel:
        default:
            break;
    }

    if (shouldDetectSelection)
    {
        TextSelectionInfo selectionInfo;
        NSRunningApplication *frontApp = GetFrontApp();

        if (getSelectedText(frontApp, selectionInfo) && !IsTrimmedEmpty(selectionInfo.text))
        {
            switch (detectionType)
            {
                case SelectionDetectType::Drag:
                {
                    selectionInfo.mousePosStart = s_lastMouseDownPos;
                    selectionInfo.mousePosEnd = s_lastMouseUpPos;

                    if (selectionInfo.posLevel == SH_POS_LEVEL_NONE)
                        selectionInfo.posLevel = SH_POS_LEVEL_MOUSE_DUAL;
                    break;
                }
                case SelectionDetectType::DoubleClick:
                {
                    selectionInfo.mousePosStart = s_lastMouseUpPos;
                    selectionInfo.mousePosEnd = s_lastMouseUpPos;

                    if (selectionInfo.posLevel == SH_POS_LEVEL_NONE)
                        selectionInfo.posLevel = SH_POS_LEVEL_MOUSE_SINGLE;
                    break;
                }
                case SelectionDetectType::ShiftClick:
                {
                    selectionInfo.mousePosStart = s_lastLastMouseUpPos;
                    selectionInfo.mousePosEnd = s_lastMouseUpPos;

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

void SelectionHookCore::dispatchSelection(const TextSelectionInfo &info)
{
    if (!callback || !running) {
        return;
    }

    // Copy strings to heap storage before calling callback.
    // Stack-local c_str() from selectionInfo can become invalid during Dart FFI marshalling.
    cached_text = info.text;
    cached_program = info.programName;
    cached_sel_data.text = cached_text.c_str();
    cached_sel_data.program_name = cached_program.c_str();

    callback(&cached_sel_data);
}

void SelectionHookCore::fillSHSelectionData(const TextSelectionInfo &info, SHSelectionData &out) const
{
    out.text = info.text.c_str();
    out.program_name = info.programName.c_str();
    out.start_top.x = static_cast<int32_t>(info.startTop.x);
    out.start_top.y = static_cast<int32_t>(info.startTop.y);
    out.start_bottom.x = static_cast<int32_t>(info.startBottom.x);
    out.start_bottom.y = static_cast<int32_t>(info.startBottom.y);
    out.end_top.x = static_cast<int32_t>(info.endTop.x);
    out.end_top.y = static_cast<int32_t>(info.endTop.y);
    out.end_bottom.x = static_cast<int32_t>(info.endBottom.x);
    out.end_bottom.y = static_cast<int32_t>(info.endBottom.y);
    out.mouse_start.x = static_cast<int32_t>(info.mousePosStart.x);
    out.mouse_start.y = static_cast<int32_t>(info.mousePosStart.y);
    out.mouse_end.x = static_cast<int32_t>(info.mousePosEnd.x);
    out.mouse_end.y = static_cast<int32_t>(info.mousePosEnd.y);
    out.method = static_cast<int32_t>(info.method);
    out.pos_level = static_cast<int32_t>(info.posLevel);
    out.is_fullscreen = info.isFullscreen ? 1 : 0;
}

bool SelectionHookCore::getSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (!frontApp)
        return false;

    if (is_processing.load())
        return false;
    else
        is_processing.store(true);

    selectionInfo.clear();

    if (!GetProgramNameFromFrontApp(frontApp, selectionInfo.programName))
    {
        is_processing.store(false);
        return false;
    }
    else if (global_filter_mode != FilterMode::Default)
    {
        std::string lowerProgramName = selectionInfo.programName;
        std::transform(lowerProgramName.begin(), lowerProgramName.end(), lowerProgramName.begin(), ::tolower);

        bool isIn = false;
        for (const auto &filterItem : global_filter_list)
        {
            if (lowerProgramName.find(filterItem) != std::string::npos)
            {
                isIn = true;
                break;
            }
        }

        if ((global_filter_mode == FilterMode::IncludeList && !isIn) ||
            (global_filter_mode == FilterMode::ExcludeList && isIn))
        {
            is_processing.store(false);
            return false;
        }
    }

    bool result = false;
    if (getTextViaAXAPI(frontApp, selectionInfo))
    {
        selectionInfo.method = SH_METHOD_AXAPI;
        result = true;
    }

    if (!result && shouldProcessViaClipboard(selectionInfo.programName) &&
        getTextViaClipboard(frontApp, selectionInfo))
    {
        selectionInfo.method = SH_METHOD_CLIPBOARD;
        result = true;
    }

    is_processing.store(false);

    if (result)
        selectionInfo.isFullscreen = IsWindowFullscreen(frontApp);

    return result;
}

bool SelectionHookCore::getTextViaAXAPI(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (!frontApp)
    {
        return false;
    }

    AXUIElementRef appElement = GetAppElementFromFrontApp(frontApp);
    AXUIElementRef focusedElement = GetFocusedElementFromAppElement(appElement);

    if (!focusedElement)
    {
        focusedElement = GetFrontWindowElementFromAppElement(appElement);

        if (!focusedElement)
        {
            CFRelease(appElement);
            return false;
        }
    }

    bool result = false;

    std::string selectedText;
    if (getSelectedTextFromElement(focusedElement, selectedText))
    {
        if (!selectedText.empty() && !IsTrimmedEmpty(selectedText))
        {
            selectionInfo.text = selectedText;

            if (!setTextRangeCoordinates(focusedElement, selectionInfo))
            {
                selectionInfo.posLevel = SH_POS_LEVEL_NONE;
            }

            result = true;
        }
    }

    if (!result)
    {
        CFArrayRef children = nullptr;
        AXError error = AXUIElementCopyAttributeValue(focusedElement, kAXChildrenAttribute, (CFTypeRef *)&children);

        if (error == kAXErrorSuccess && children)
        {
            CFIndex childCount = CFArrayGetCount(children);

            for (CFIndex i = 0; i < childCount && !result; i++)
            {
                AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                if (child)
                {
                    std::string childText;
                    if (getSelectedTextFromElement(child, childText))
                    {
                        if (!childText.empty() && !IsTrimmedEmpty(childText))
                        {
                            selectionInfo.text = childText;

                            if (!setTextRangeCoordinates(child, selectionInfo))
                            {
                                selectionInfo.posLevel = SH_POS_LEVEL_NONE;
                            }

                            result = true;
                        }
                    }
                }
            }

            CFRelease(children);
        }
    }

    if (!result)
    {
        AXUIElementSetAttributeValue(appElement, CFSTR("AXEnhancedUserInterface"), kCFBooleanTrue);
        AXUIElementSetAttributeValue(appElement, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
    }

    CFRelease(focusedElement);
    CFRelease(appElement);

    return result;
}

bool SelectionHookCore::getSelectedTextFromElement(AXUIElementRef element, std::string &text)
{
    if (!element)
    {
        return false;
    }

    CFTypeRef selectedTextRef = nullptr;
    AXError error = AXUIElementCopyAttributeValue(element, kAXSelectedTextAttribute, &selectedTextRef);

    if (error == kAXErrorSuccess && selectedTextRef)
    {
        CFTypeID typeID = CFGetTypeID(selectedTextRef);

        if (typeID == CFStringGetTypeID())
        {
            CFStringRef selectedText = (CFStringRef)selectedTextRef;
            CFIndex length = CFStringGetLength(selectedText);
            if (length > 0)
            {
                CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
                char *buffer = new char[maxSize];

                if (CFStringGetCString(selectedText, buffer, maxSize, kCFStringEncodingUTF8))
                {
                    text = std::string(buffer);
                    delete[] buffer;
                    CFRelease(selectedTextRef);
                    return !text.empty();
                }

                delete[] buffer;
            }
        }
        else if (typeID == CFNumberGetTypeID())
        {
            CFNumberRef number = (CFNumberRef)selectedTextRef;

            if (CFNumberIsFloatType(number))
            {
                double doubleValue;
                if (CFNumberGetValue(number, kCFNumberDoubleType, &doubleValue))
                {
                    text = std::to_string(doubleValue);
                    CFRelease(selectedTextRef);
                    return !text.empty();
                }
            }
            else
            {
                long longValue;
                if (CFNumberGetValue(number, kCFNumberLongType, &longValue))
                {
                    text = std::to_string(longValue);
                    CFRelease(selectedTextRef);
                    return !text.empty();
                }
            }
        }
        CFRelease(selectedTextRef);
    }

    CFTypeRef valueRef = nullptr;
    error = AXUIElementCopyAttributeValue(element, kAXValueAttribute, &valueRef);

    if (error == kAXErrorSuccess && valueRef)
    {
        CFTypeID valueTypeID = CFGetTypeID(valueRef);

        if (valueTypeID == CFStringGetTypeID())
        {
            CFStringRef value = (CFStringRef)valueRef;
            CFRange selectedRange = {0, 0};
            AXValueRef rangeValue = nullptr;
            error = AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, (CFTypeRef *)&rangeValue);

            if (error == kAXErrorSuccess && rangeValue)
            {
                if (AXValueGetValue(rangeValue, kAXValueTypeCFRange, &selectedRange))
                {
                    CFIndex valueLength = CFStringGetLength(value);

                    if (valueLength <= 0)
                    {
                        CFRelease(rangeValue);
                        CFRelease(valueRef);
                        return false;
                    }

                    if (selectedRange.location < 0)
                    {
                        selectedRange.location = 0;
                    }
                    else if (selectedRange.location >= valueLength)
                    {
                        selectedRange.location = valueLength - 1;
                    }

                    if (selectedRange.length <= 0)
                    {
                        CFRelease(rangeValue);
                        CFRelease(valueRef);
                        return false;
                    }

                    if (selectedRange.location + selectedRange.length > valueLength)
                    {
                        selectedRange.length = valueLength - selectedRange.location;
                    }

                    if (selectedRange.length <= 0)
                    {
                        CFRelease(rangeValue);
                        CFRelease(valueRef);
                        return false;
                    }

                    CFStringRef selectedSubstring =
                        CFStringCreateWithSubstring(kCFAllocatorDefault, value, selectedRange);
                    if (selectedSubstring)
                    {
                        CFIndex length = CFStringGetLength(selectedSubstring);
                        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
                        char *buffer = new char[maxSize];

                        if (CFStringGetCString(selectedSubstring, buffer, maxSize, kCFStringEncodingUTF8))
                        {
                            text = std::string(buffer);
                            delete[] buffer;
                            CFRelease(selectedSubstring);
                            CFRelease(rangeValue);
                            CFRelease(valueRef);
                            return !text.empty();
                        }

                        delete[] buffer;
                        CFRelease(selectedSubstring);
                    }
                }
                CFRelease(rangeValue);
            }

            CFRelease(valueRef);
        }
        else
        {
            CFRelease(valueRef);
        }
    }

    return false;
}

bool SelectionHookCore::setTextRangeCoordinates(AXUIElementRef element, TextSelectionInfo &selectionInfo)
{
    if (!element)
        return false;

    AXValueRef selectedRangeValue = nullptr;

    AXError error =
        AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, (CFTypeRef *)&selectedRangeValue);
    if (error == kAXErrorSuccess && selectedRangeValue)
    {
        CFRange selectedRange = {0, 0};
        if (AXValueGetValue(selectedRangeValue, kAXValueTypeCFRange, &selectedRange))
        {
            if (selectedRange.length > 0)
            {
                CFRange firstCharRange = {selectedRange.location, 1};
                CFRange lastCharRange = {selectedRange.location + selectedRange.length - 1, 1};

                AXValueRef firstCharRangeValue = AXValueCreate(kAXValueTypeCFRange, &firstCharRange);
                AXValueRef lastCharRangeValue = AXValueCreate(kAXValueTypeCFRange, &lastCharRange);

                if (firstCharRangeValue && lastCharRangeValue)
                {
                    AXValueRef firstCharBoundsValue = nullptr;
                    AXValueRef lastCharBoundsValue = nullptr;

                    AXError firstCharBoundsError = AXUIElementCopyParameterizedAttributeValue(
                        element, kAXBoundsForRangeParameterizedAttribute, firstCharRangeValue,
                        (CFTypeRef *)&firstCharBoundsValue);
                    AXError lastCharBoundsError = AXUIElementCopyParameterizedAttributeValue(
                        element, kAXBoundsForRangeParameterizedAttribute, lastCharRangeValue,
                        (CFTypeRef *)&lastCharBoundsValue);

                    if (firstCharBoundsError == kAXErrorSuccess && firstCharBoundsValue &&
                        lastCharBoundsError == kAXErrorSuccess && lastCharBoundsValue)
                    {
                        CGRect firstCharRect = CGRectZero;
                        CGRect lastCharRect = CGRectZero;

                        if (AXValueGetValue(firstCharBoundsValue, kAXValueTypeCGRect, &firstCharRect) &&
                            AXValueGetValue(lastCharBoundsValue, kAXValueTypeCGRect, &lastCharRect))
                        {
                            if (firstCharRect.size.width > 1.0 && firstCharRect.size.height > 0 &&
                                firstCharRect.size.height < 100.0 && lastCharRect.size.width > 1.0 &&
                                lastCharRect.size.height > 0 && lastCharRect.size.height < 100.0 &&
                                firstCharRect.origin.x >= 0 && firstCharRect.origin.y >= 0 &&
                                lastCharRect.origin.x >= 0 && lastCharRect.origin.y >= 0 &&
                                firstCharRect.origin.x < 10000 && firstCharRect.origin.y < 10000 &&
                                lastCharRect.origin.x < 10000 && lastCharRect.origin.y < 10000)
                            {
                                selectionInfo.startTop = CGPointMake(firstCharRect.origin.x, firstCharRect.origin.y);
                                selectionInfo.startBottom = CGPointMake(
                                    firstCharRect.origin.x, firstCharRect.origin.y + firstCharRect.size.height);
                                selectionInfo.endTop =
                                    CGPointMake(lastCharRect.origin.x + lastCharRect.size.width, lastCharRect.origin.y);
                                selectionInfo.endBottom = CGPointMake(lastCharRect.origin.x + lastCharRect.size.width,
                                                                      lastCharRect.origin.y + lastCharRect.size.height);

                                selectionInfo.posLevel = SH_POS_LEVEL_SEL_FULL;

                                CFRelease(firstCharBoundsValue);
                                CFRelease(lastCharBoundsValue);
                                CFRelease(firstCharRangeValue);
                                CFRelease(lastCharRangeValue);
                                CFRelease(selectedRangeValue);

                                return true;
                            }
                        }

                        if (firstCharBoundsValue)
                            CFRelease(firstCharBoundsValue);
                        if (lastCharBoundsValue)
                            CFRelease(lastCharBoundsValue);
                    }

                    if (firstCharRangeValue)
                        CFRelease(firstCharRangeValue);
                    if (lastCharRangeValue)
                        CFRelease(lastCharRangeValue);
                }
                else
                {
                    if (firstCharRangeValue)
                        CFRelease(firstCharRangeValue);
                    if (lastCharRangeValue)
                        CFRelease(lastCharRangeValue);
                }
            }
        }
        CFRelease(selectedRangeValue);
        selectedRangeValue = nullptr;
    }

    error = AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, (CFTypeRef *)&selectedRangeValue);
    if (error == kAXErrorSuccess && selectedRangeValue)
    {
        CFRange selectedRange = {0, 0};
        if (AXValueGetValue(selectedRangeValue, kAXValueTypeCFRange, &selectedRange))
        {
            if (selectedRange.length > 0)
            {
                AXValueRef boundsValue = nullptr;
                error = AXUIElementCopyParameterizedAttributeValue(element, kAXBoundsForRangeParameterizedAttribute,
                                                                   selectedRangeValue, (CFTypeRef *)&boundsValue);
                if (error == kAXErrorSuccess && boundsValue)
                {
                    CGRect rect = CGRectZero;
                    if (AXValueGetValue(boundsValue, kAXValueTypeCGRect, &rect))
                    {
                        if (rect.size.width > 0 && rect.size.height > 0 && rect.origin.x >= 0 && rect.origin.y >= 0 &&
                            rect.origin.x < 10000 && rect.origin.y < 10000)
                        {
                            selectionInfo.startTop = CGPointMake(rect.origin.x, rect.origin.y);
                            selectionInfo.startBottom = CGPointMake(rect.origin.x, rect.origin.y + rect.size.height);
                            selectionInfo.endTop = CGPointMake(rect.origin.x + rect.size.width, rect.origin.y);
                            selectionInfo.endBottom =
                                CGPointMake(rect.origin.x + rect.size.width, rect.origin.y + rect.size.height);
                            selectionInfo.posLevel = SH_POS_LEVEL_SEL_FULL;

                            CFRelease(boundsValue);
                            CFRelease(selectedRangeValue);
                            return true;
                        }
                    }
                    CFRelease(boundsValue);
                }
            }
        }
        CFRelease(selectedRangeValue);
    }

    return false;
}

bool SelectionHookCore::getTextViaClipboard(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (frontApp.processIdentifier == running_pid)
        return false;

    int64_t newClipboardSequence = GetClipboardSequence();

    if (newClipboardSequence != clipboard_sequence)
    {
        std::string newContent;
        if (ReadClipboard(newContent))
        {
            selectionInfo.text = newContent;
            return true;
        }
    }

    clipboard_sequence = newClipboardSequence;
    ClipboardBackup clipboardBackup = BackupClipboard();

    if (!sendCopyKey(frontApp.processIdentifier))
        return false;

    bool clipboardChanged = false;
    for (int i = 0; i < 10; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        newClipboardSequence = GetClipboardSequence();
        if (newClipboardSequence != clipboard_sequence)
        {
            clipboardChanged = true;
            break;
        }
    }

    if (!clipboardChanged)
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboard_sequence = GetClipboardSequence();
        }
        return false;
    }

    std::string newContent;
    if (!ReadClipboard(newContent) || IsTrimmedEmpty(newContent))
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboard_sequence = GetClipboardSequence();
        }
        return false;
    }

    selectionInfo.text = newContent;

    if (clipboardBackup.HasData())
    {
        RestoreClipboard(clipboardBackup);
        clipboard_sequence = GetClipboardSequence();
    }

    return true;
}

bool SelectionHookCore::shouldProcessViaClipboard(const std::string &programName)
{
    if (!is_enabled_clipboard)
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

    return true;
}

bool SelectionHookCore::sendCopyKey(pid_t pid)
{
    CGEventRef keyDownEvent = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_C, true);
    if (!keyDownEvent) return false;

    CGEventSetFlags(keyDownEvent, kCGEventFlagMaskCommand);

    CGEventRef keyUpEvent = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_C, false);
    if (!keyUpEvent)
    {
        CFRelease(keyDownEvent);
        return false;
    }

    CGEventSetFlags(keyUpEvent, kCGEventFlagMaskCommand);

    if (pid != 0)
    {
        CGEventPostToPid(pid, keyDownEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CGEventPostToPid(pid, keyUpEvent);
    }
    else
    {
        CGEventPost(kCGHIDEventTap, keyDownEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CGEventPost(kCGHIDEventTap, keyUpEvent);
    }

    CFRelease(keyDownEvent);
    CFRelease(keyUpEvent);

    return true;
}

bool SelectionHookCore::isInFilterList(const std::string &programName, const std::vector<std::string> &filterList)
{
    if (filterList.empty())
        return false;

    std::string lowerProgramName = programName;
    std::transform(lowerProgramName.begin(), lowerProgramName.end(), lowerProgramName.begin(), ::tolower);

    for (const auto &filterItem : filterList)
    {
        if (lowerProgramName.find(filterItem) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}
