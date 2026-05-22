#include "selection_hook_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <string>

#include <X11/Xlib.h>

#ifdef None
#undef None
#endif

#include <linux/input.h>

#include "lib/keyboard.h"
#include "lib/utils.h"

constexpr int MIN_DRAG_DISTANCE = 8;
constexpr uint64_t MAX_DRAG_TIME_MS = 15000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static uint64_t DOUBLE_CLICK_TIME_MS = 500;
constexpr uint64_t CORRELATION_WINDOW_MS = 500;

std::atomic<SelectionHookCore *> SelectionHookCore::currentInstance{nullptr};

SelectionHookCore::SelectionHookCore()
{
    currentInstance.store(this);

    XInitThreads();

    protocol = CreateX11Protocol();
    if (!protocol)
    {
        last_error = "Failed to create X11 protocol";
        return;
    }

    if (!protocol->Initialize())
    {
        last_error = "Failed to initialize X11 protocol";
        return;
    }

    DOUBLE_CLICK_TIME_MS = 500;
    current_mouse_pos = Point();
}

SelectionHookCore::~SelectionHookCore()
{
    stop();

    if (protocol)
    {
        protocol->Cleanup();
    }

    if (currentInstance.load() == this)
    {
        currentInstance.store(nullptr);
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

    if (!protocol)
    {
        last_error = "Protocol not initialized";
        return false;
    }

    if (!protocol->InitializeInputMonitoring(
            &SelectionHookCore::onMouseEventCallback,
            &SelectionHookCore::onKeyboardEventCallback,
            &SelectionHookCore::onSelectionEventCallback,
            this))
    {
        last_error = "Failed to initialize input monitoring";
        return false;
    }

    try
    {
        if (!protocol->StartInputMonitoring())
        {
            last_error = "Failed to start input monitoring";
            protocol->CleanupInputMonitoring();
            return false;
        }
    }
    catch (const std::exception &e)
    {
        last_error = std::string("Failed to start input monitoring: ") + e.what();
        protocol->CleanupInputMonitoring();
        return false;
    }

    running = true;
    return true;
}

bool SelectionHookCore::stop()
{
    if (!running)
    {
        return false;
    }

    running = false;

    if (protocol)
    {
        protocol->CleanupInputMonitoring();
    }

    last_selection_event_time.store(0);
    is_gesture_button_down.store(false);
    had_selection_during_drag.store(false);

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_gesture.active = false;
    }

    return true;
}

bool SelectionHookCore::isRunning() const
{
    return running.load();
}

TextSelectionInfo *SelectionHookCore::getCurrentSelection()
{
    if (!protocol)
        return nullptr;

    uint64_t activeWindow = protocol->GetActiveWindow();
    if (!activeWindow)
        return nullptr;

    TextSelectionInfo *result = new TextSelectionInfo();
    is_triggered_by_user = true;

    if (!getSelectedText(activeWindow, *result) || IsTrimmedEmpty(result->text))
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

void SelectionHookCore::setMouseCallback(SHMouseCallback cb)
{
    mouseCallback = cb;
}

void SelectionHookCore::setKeyboardCallback(SHKeyboardCallback cb)
{
    keyboardCallback = cb;
}

void SelectionHookCore::setPassiveMode(bool passive)
{
    is_selection_passive_mode.store(passive);
}

void SelectionHookCore::setClipboardEnabled(bool enabled)
{
    is_enabled_clipboard = enabled;
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

bool SelectionHookCore::getSelectedText(uint64_t window, TextSelectionInfo &selectionInfo)
{
    if (!window)
        return false;

    if (is_processing.exchange(true))
        return false;

    selectionInfo.clear();

    if (!protocol->GetProgramNameFromWindow(window, selectionInfo.programName))
    {
        selectionInfo.programName = "";

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

    if (getTextViaPrimary(selectionInfo))
    {
        selectionInfo.method = SelectionMethod::Primary;
        is_processing.store(false);
        return true;
    }

    is_processing.store(false);
    return false;
}

bool SelectionHookCore::getTextViaPrimary(TextSelectionInfo &selectionInfo)
{
    std::string selectedText;
    if (protocol->GetTextViaPrimary(selectedText) && !IsTrimmedEmpty(selectedText))
    {
        selectionInfo.text = selectedText;
        return true;
    }

    return false;
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

void SelectionHookCore::onMouseEventCallback(void *context, MouseEventContext *mouseEvent)
{
    SelectionHookCore *instance = static_cast<SelectionHookCore *>(context);
    if (!instance || !mouseEvent || !instance->running.load())
    {
        delete mouseEvent;
        return;
    }

    instance->current_mouse_pos = mouseEvent->pos;

    if (mouseEvent->code == BTN_LEFT)
    {
        instance->is_gesture_button_down.store(mouseEvent->value == 1);
    }

    instance->processMouseEvent(mouseEvent);

    delete mouseEvent;
}

void SelectionHookCore::onKeyboardEventCallback(void *context, KeyboardEventContext *keyboardEvent)
{
    SelectionHookCore *instance = static_cast<SelectionHookCore *>(context);
    if (!instance || !keyboardEvent || !instance->running.load())
    {
        delete keyboardEvent;
        return;
    }

    if (instance->keyboardCallback)
    {
        auto keyCode = keyboardEvent->code;
        auto keyFlags = keyboardEvent->flags;

        bool isSysKey = (keyFlags & MODIFIER_CTRL) || (keyFlags & MODIFIER_ALT) || (keyFlags & MODIFIER_META);

        std::string uniKey = convertKeyCodeToUniKey(keyCode, keyFlags);

        instance->cached_uni_key = uniKey;
        instance->cached_keyboard_event.uni_key = instance->cached_uni_key.c_str();
        instance->cached_keyboard_event.vk_code = static_cast<int32_t>(keyCode);
        instance->cached_keyboard_event.sys = isSysKey ? 1 : 0;
        instance->cached_keyboard_event.flags = static_cast<int32_t>(keyFlags);

        instance->keyboardCallback(&instance->cached_keyboard_event);
    }

    delete keyboardEvent;
}

void SelectionHookCore::onSelectionEventCallback(void *context, SelectionChangeContext *event)
{
    SelectionHookCore *instance = static_cast<SelectionHookCore *>(context);
    if (!instance || !event)
    {
        delete event;
        return;
    }

    instance->last_selection_event_time.store(event->timestamp_ms);

    if (instance->is_gesture_button_down.load())
    {
        instance->had_selection_during_drag.store(true);
        delete event;
        return;
    }

    if (instance->running.load())
    {
        instance->processSelectionEvent(event);
    }

    delete event;
}

void SelectionHookCore::processMouseEvent(MouseEventContext *mouseEvent)
{
    if (!running.load())
        return;

    auto currentTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    Point currentPos = mouseEvent->pos;
    int mouseCode = mouseEvent->code;
    int mouseValue = mouseEvent->value;
    MouseButton mouseButton = static_cast<MouseButton>(mouseEvent->button);

    std::string mouseTypeStr;
    int mouseFlagValue = 0;

    switch (mouseCode)
    {
        case BTN_LEFT:
        {
            if (mouseValue == 1)
            {
                mouseTypeStr = "mouse-down";
                mouseButton = MouseButton::Left;

                last_mouse_down_time = currentTime;
                last_mouse_down_pos = currentPos;

                {
                    std::lock_guard<std::mutex> lock(pending_mutex);
                    pending_gesture.active = false;
                }
                had_selection_during_drag.store(false);

                last_window_handler = protocol->GetActiveWindow();
                if (last_window_handler)
                {
                    protocol->GetWindowRect(last_window_handler, last_window_rect);
                }
            }
            else if (mouseValue == 0)
            {
                mouseTypeStr = "mouse-up";
                mouseButton = MouseButton::Left;

                Point prevUp = last_mouse_up_pos;
                uint64_t prevUpTime = last_mouse_up_time;
                last_mouse_up_time = currentTime;
                last_mouse_up_pos = currentPos;
                last_mouse_up_modifier_flags = protocol->GetModifierFlags();

                if (!is_selection_passive_mode.load())
                {
                    auto detectionType = SelectionDetectType::None;

                    double dx = currentPos.x - last_mouse_down_pos.x;
                    double dy = currentPos.y - last_mouse_down_pos.y;
                    double distance = sqrt(dx * dx + dy * dy);

                    bool isCurrentValidClick =
                        (currentTime - last_mouse_down_time) <= DOUBLE_CLICK_TIME_MS;

                    if ((currentTime - last_mouse_down_time) > MAX_DRAG_TIME_MS)
                    {
                    }
                    else if (distance >= MIN_DRAG_DISTANCE)
                    {
                        uint64_t upWindow = protocol->GetActiveWindow();
                        if (upWindow && upWindow == last_window_handler)
                        {
                            WindowRect currentWindowRect;
                            protocol->GetWindowRect(upWindow, currentWindowRect);
                            if (!HasWindowMoved(currentWindowRect, last_window_rect))
                            {
                                detectionType = SelectionDetectType::Drag;
                            }
                        }
                        else if (upWindow && upWindow != last_window_handler)
                        {
                            detectionType = SelectionDetectType::Drag;
                        }
                    }
                    else if (is_last_valid_click && isCurrentValidClick &&
                             distance <= DOUBLE_CLICK_MAX_DISTANCE)
                    {
                        double dx2 = currentPos.x - prevUp.x;
                        double dy2 = currentPos.y - prevUp.y;
                        double distance2 = sqrt(dx2 * dx2 + dy2 * dy2);

                        if (distance2 <= DOUBLE_CLICK_MAX_DISTANCE &&
                            (last_mouse_down_time - prevUpTime) <= DOUBLE_CLICK_TIME_MS)
                        {
                            uint64_t upWindow = protocol->GetActiveWindow();
                            if (upWindow && upWindow == last_window_handler)
                            {
                                WindowRect currentWindowRect;
                                protocol->GetWindowRect(upWindow, currentWindowRect);
                                if (!HasWindowMoved(currentWindowRect, last_window_rect))
                                {
                                    detectionType = SelectionDetectType::DoubleClick;
                                }
                            }
                        }
                    }

                    if (detectionType == SelectionDetectType::None)
                    {
                        int modFlags = last_mouse_up_modifier_flags;
                        bool isShiftPressed = (modFlags & MODIFIER_SHIFT) != 0;
                        bool isCtrlPressed = (modFlags & MODIFIER_CTRL) != 0;
                        bool isAltPressed = (modFlags & MODIFIER_ALT) != 0;

                        if (isShiftPressed && !isCtrlPressed && !isAltPressed)
                        {
                            detectionType = SelectionDetectType::ShiftClick;
                        }
                    }

                    if (detectionType != SelectionDetectType::None)
                    {
                        uint64_t lastSelectionEvent = last_selection_event_time.load();

                        Point gestureStart, gestureEnd;
                        switch (detectionType)
                        {
                            case SelectionDetectType::Drag:
                                gestureStart = last_mouse_down_pos;
                                gestureEnd = currentPos;
                                break;
                            case SelectionDetectType::DoubleClick:
                                gestureStart = currentPos;
                                gestureEnd = currentPos;
                                break;
                            case SelectionDetectType::ShiftClick:
                                gestureStart = prev_mouse_up_pos;
                                gestureEnd = currentPos;
                                break;
                            default:
                                gestureStart = currentPos;
                                gestureEnd = currentPos;
                                break;
                        }

                        bool emitted = false;

                        if (detectionType == SelectionDetectType::Drag &&
                            had_selection_during_drag.load())
                        {
                            had_selection_during_drag.store(false);
                            emitted = emitSelectionEvent(detectionType, gestureStart, gestureEnd);
                            if (emitted)
                            {
                                last_selection_event_time.store(0);
                                lastSelectionEvent = 0;
                            }
                        }

                        if (!emitted && lastSelectionEvent > 0 &&
                            (static_cast<uint64_t>(currentTime) - lastSelectionEvent) < CORRELATION_WINDOW_MS)
                        {
                            last_selection_event_time.store(0);
                            emitted = emitSelectionEvent(detectionType, gestureStart, gestureEnd);
                        }

                        if (!emitted)
                        {
                            std::lock_guard<std::mutex> lock(pending_mutex);
                            pending_gesture.active = true;
                            pending_gesture.type = detectionType;
                            pending_gesture.mousePosStart = gestureStart;
                            pending_gesture.mousePosEnd = gestureEnd;
                            pending_gesture.timestamp = currentTime;
                        }
                    }

                    is_last_valid_click = isCurrentValidClick;
                }

                prev_mouse_up_pos = prevUp;
                prev_mouse_up_time = prevUpTime;
            }
            break;
        }

        case BTN_RIGHT:
        {
            mouseTypeStr = (mouseValue == 1) ? "mouse-down" : "mouse-up";
            mouseButton = MouseButton::Right;
            break;
        }

        case BTN_MIDDLE:
            mouseTypeStr = (mouseValue == 1) ? "mouse-down" : "mouse-up";
            mouseButton = MouseButton::Middle;
            break;

        case REL_WHEEL:
            mouseTypeStr = "mouse-wheel";
            mouseButton = MouseButton::WheelVertical;
            mouseFlagValue = mouseValue > 0 ? 1 : -1;
            break;

        case REL_HWHEEL:
            mouseTypeStr = "mouse-wheel";
            mouseButton = MouseButton::WheelHorizontal;
            mouseFlagValue = mouseValue > 0 ? 1 : -1;
            break;

        default:
            if (mouseCode == REL_X || mouseCode == REL_Y)
            {
                mouseTypeStr = "mouse-move";
                mouseButton = MouseButton::None;
            }
            else
            {
                mouseTypeStr = "unknown";
                mouseButton = MouseButton::Unknown;
            }
            break;
    }

    if (!mouseTypeStr.empty())
    {
        if (mouseTypeStr == "mouse-move" && !is_enabled_mouse_move.load())
        {
            return;
        }

        if (mouseCallback)
        {
            int outX = currentPos.valid ? currentPos.x : INVALID_COORDINATE;
            int outY = currentPos.valid ? currentPos.y : INVALID_COORDINATE;

            cached_mouse_event.x = static_cast<int32_t>(outX);
            cached_mouse_event.y = static_cast<int32_t>(outY);
            cached_mouse_event.button = static_cast<int32_t>(mouseButton);
            cached_mouse_event.flag = static_cast<int32_t>(mouseFlagValue);

            if (mouseTypeStr == "mouse-down")
                cached_mouse_event.event_type = 0;
            else if (mouseTypeStr == "mouse-up")
                cached_mouse_event.event_type = 1;
            else if (mouseTypeStr == "mouse-move")
                cached_mouse_event.event_type = 2;
            else if (mouseTypeStr == "mouse-wheel")
                cached_mouse_event.event_type = 3;

            mouseCallback(&cached_mouse_event);
        }
    }
}

void SelectionHookCore::processSelectionEvent(SelectionChangeContext *event)
{
    SelectionDetectType type = SelectionDetectType::None;
    Point start, end;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        if (pending_gesture.active)
        {
            int64_t delta = (int64_t)event->timestamp_ms - (int64_t)pending_gesture.timestamp;
            if (std::abs(delta) < (int64_t)CORRELATION_WINDOW_MS)
            {
                type = pending_gesture.type;
                start = pending_gesture.mousePosStart;
                end = pending_gesture.mousePosEnd;
            }
            pending_gesture.active = false;
        }
    }

    if (type != SelectionDetectType::None)
    {
        last_selection_event_time.store(0);
        emitSelectionEvent(type, start, end);
    }
}

bool SelectionHookCore::emitSelectionEvent(SelectionDetectType type, Point start, Point end)
{
    if (is_selection_passive_mode.load())
        return false;

    uint64_t activeWindow = protocol->GetActiveWindow();
    if (!activeWindow)
        return false;

    TextSelectionInfo selectionInfo;
    if (!getSelectedText(activeWindow, selectionInfo) || IsTrimmedEmpty(selectionInfo.text))
        return false;

    switch (type)
    {
        case SelectionDetectType::Drag:
            selectionInfo.mousePosStart = start;
            selectionInfo.mousePosEnd = end;
            if (selectionInfo.posLevel == SelectionPositionLevel::None)
                selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
            break;
        case SelectionDetectType::DoubleClick:
            selectionInfo.mousePosStart = start;
            selectionInfo.mousePosEnd = end;
            if (selectionInfo.posLevel == SelectionPositionLevel::None)
                selectionInfo.posLevel = SelectionPositionLevel::MouseSingle;
            break;
        case SelectionDetectType::ShiftClick:
            selectionInfo.mousePosStart = start;
            selectionInfo.mousePosEnd = end;
            if (selectionInfo.posLevel == SelectionPositionLevel::None)
                selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
            break;
        default:
            break;
    }

    dispatchSelection(selectionInfo);
    return true;
}

void SelectionHookCore::dispatchSelection(const TextSelectionInfo &info)
{
    if (!callback || !running)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(cached_data_mutex);

    fillSHSelectionData(info, cached_sel_data);

    cached_text = info.text;
    cached_program = info.programName;
    cached_sel_data.text = cached_text.c_str();
    cached_sel_data.program_name = cached_program.c_str();

    callback(&cached_sel_data);
}

void SelectionHookCore::fillSHSelectionData(const TextSelectionInfo &info, SHSelectionData &out) const
{
    out.start_top.x = INVALID_COORDINATE;
    out.start_top.y = INVALID_COORDINATE;
    out.start_bottom.x = INVALID_COORDINATE;
    out.start_bottom.y = INVALID_COORDINATE;
    out.end_top.x = INVALID_COORDINATE;
    out.end_top.y = INVALID_COORDINATE;
    out.end_bottom.x = INVALID_COORDINATE;
    out.end_bottom.y = INVALID_COORDINATE;

    out.mouse_start.x = info.mousePosStart.valid ? info.mousePosStart.x : INVALID_COORDINATE;
    out.mouse_start.y = info.mousePosStart.valid ? info.mousePosStart.y : INVALID_COORDINATE;
    out.mouse_end.x = info.mousePosEnd.valid ? info.mousePosEnd.x : INVALID_COORDINATE;
    out.mouse_end.y = info.mousePosEnd.valid ? info.mousePosEnd.y : INVALID_COORDINATE;

    out.method = static_cast<int32_t>(info.method);
    out.pos_level = static_cast<int32_t>(info.posLevel);
    out.is_fullscreen = 0;
}
