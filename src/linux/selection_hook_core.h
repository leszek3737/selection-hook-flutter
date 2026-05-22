#ifndef SELECTION_HOOK_CORE_LINUX_H
#define SELECTION_HOOK_CORE_LINUX_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../bridge/c_api.h"
#include "common.h"

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
    bool getSelectedText(uint64_t window, TextSelectionInfo &selectionInfo);
    bool getTextViaPrimary(TextSelectionInfo &selectionInfo);
    bool isInFilterList(const std::string &programName, const std::vector<std::string> &filterList);
    bool emitSelectionEvent(SelectionDetectType type, Point start, Point end);

    void processMouseEvent(MouseEventContext *mouseEvent);
    void processSelectionEvent(SelectionChangeContext *event);

    void dispatchSelection(const TextSelectionInfo &info);
    void fillSHSelectionData(const TextSelectionInfo &info, SHSelectionData &out) const;

    static void onMouseEventCallback(void *context, MouseEventContext *mouseEvent);
    static void onKeyboardEventCallback(void *context, KeyboardEventContext *keyboardEvent);
    static void onSelectionEventCallback(void *context, SelectionChangeContext *selectionEvent);

    std::atomic<bool> running{false};
    SHSelectionCallback callback{nullptr};

    mutable std::string last_error;

    std::string cached_text;
    std::string cached_program;
    SHSelectionData cached_sel_data{};
    std::mutex cached_data_mutex;

    SHMouseEventData cached_mouse_event{};
    SHKeyboardEventData cached_keyboard_event{};
    std::string cached_uni_key;

    std::unique_ptr<ProtocolBase> protocol;

    Point current_mouse_pos;

    Point last_mouse_down_pos;
    uint64_t last_mouse_down_time = 0;
    Point last_mouse_up_pos;
    uint64_t last_mouse_up_time = 0;
    Point prev_mouse_up_pos;
    uint64_t prev_mouse_up_time = 0;
    uint64_t last_window_handler = 0;
    WindowRect last_window_rect;
    bool is_last_valid_click = false;
    int last_mouse_up_modifier_flags = 0;

    std::atomic<uint64_t> last_selection_event_time{0};
    std::atomic<bool> is_gesture_button_down{false};
    std::atomic<bool> had_selection_during_drag{false};

    struct
    {
        bool active = false;
        SelectionDetectType type = SelectionDetectType::None;
        Point mousePosStart;
        Point mousePosEnd;
        uint64_t timestamp = 0;
    } pending_gesture;
    std::mutex pending_mutex;

    std::atomic<bool> is_processing{false};
    bool is_triggered_by_user = false;

    std::atomic<bool> is_selection_passive_mode{false};
    std::atomic<bool> is_enabled_mouse_move{false};
    std::atomic<bool> debug_enabled{false};

    bool is_enabled_clipboard = false;
    FilterMode clipboard_filter_mode{FilterMode::Default};
    std::vector<std::string> clipboard_filter_list;

    FilterMode global_filter_mode{FilterMode::Default};
    std::vector<std::string> global_filter_list;

    SHMouseCallback mouseCallback{nullptr};
    SHKeyboardCallback keyboardCallback{nullptr};

    static std::atomic<SelectionHookCore *> currentInstance;
};

#endif // SELECTION_HOOK_CORE_LINUX_H
