/**
 * macOS-specific bridge between the C-ABI API (c_api.h) and the macOS core
 * (SelectionHookCore). Objective-C++ (needs .mm for Cocoa types included via
 * selection_hook_core.h).
 */

#include "c_api.h"
#include "../mac/selection_hook_core.h"

#include <mutex>
#include <string>
#include "../mac/lib/clipboard.h"

struct SelectionHook {
    SelectionHookCore* core = nullptr;
    SHSelectionCallback callback = nullptr;
    std::string last_error;
    SHMouseCallback mouse_callback = nullptr;
    SHKeyboardCallback keyboard_callback = nullptr;
    std::mutex read_clipboard_mutex;
    std::string read_clipboard_buf;
};

static std::mutex g_global_error_mutex;
static std::string g_global_error;

extern "C" {

SH_API SelectionHook* sh_create(void) {
    auto* core = new (std::nothrow) SelectionHookCore();
    if (!core) {
        std::lock_guard<std::mutex> lock(g_global_error_mutex);
        g_global_error = "Failed to allocate SelectionHookCore";
        return nullptr;
    }

    auto* hook = new (std::nothrow) SelectionHook();
    if (!hook) {
        delete core;
        std::lock_guard<std::mutex> lock(g_global_error_mutex);
        g_global_error = "Failed to allocate SelectionHook";
        return nullptr;
    }

    hook->core = core;
    return hook;
}

SH_API void sh_destroy(SelectionHook* hook) {
    if (!hook) return;
    if (hook->core) {
        hook->core->stop();
        delete hook->core;
        hook->core = nullptr;
    }
    delete hook;
}

SH_API int sh_start(SelectionHook* hook) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    if (hook->core->isRunning()) return SH_ERR_ALREADY_RUNNING;

    hook->core->setCallback(hook->callback);
    if (hook->mouse_callback) hook->core->setMouseCallback(hook->mouse_callback);
    if (hook->keyboard_callback) hook->core->setKeyboardCallback(hook->keyboard_callback);

    if (!hook->core->start()) {
        hook->last_error = hook->core->lastError()
            ? hook->core->lastError()
            : "Failed to start selection hook";
        return SH_ERR_GENERIC;
    }

    return SH_OK;
}

SH_API int sh_stop(SelectionHook* hook) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    if (!hook->core->isRunning()) return SH_ERR_NOT_RUNNING;

    hook->core->stop();
    return SH_OK;
}

SH_API int sh_is_running(SelectionHook* hook) {
    if (!hook || !hook->core) return 0;
    return hook->core->isRunning() ? 1 : 0;
}

SH_API SHSelectionData* sh_get_current_selection(SelectionHook* hook) {
    if (!hook || !hook->core) return nullptr;

    TextSelectionInfo* info = hook->core->getCurrentSelection();
    if (!info || info->text.empty()) { delete info; return nullptr; }

    // Heap-allocate so caller owns the data — no race with dispatchSelection.
    auto* result = new (std::nothrow) SHSelectionData();
    if (!result) { delete info; return nullptr; }

    result->text = strdup(info->text.c_str());
    result->program_name = strdup(info->programName.c_str());
    result->start_top = { static_cast<int32_t>(info->startTop.x), static_cast<int32_t>(info->startTop.y) };
    result->start_bottom = { static_cast<int32_t>(info->startBottom.x), static_cast<int32_t>(info->startBottom.y) };
    result->end_top = { static_cast<int32_t>(info->endTop.x), static_cast<int32_t>(info->endTop.y) };
    result->end_bottom = { static_cast<int32_t>(info->endBottom.x), static_cast<int32_t>(info->endBottom.y) };
    result->mouse_start = { static_cast<int32_t>(info->mousePosStart.x), static_cast<int32_t>(info->mousePosStart.y) };
    result->mouse_end = { static_cast<int32_t>(info->mousePosEnd.x), static_cast<int32_t>(info->mousePosEnd.y) };
    result->method = static_cast<int32_t>(info->method);
    result->pos_level = static_cast<int32_t>(info->posLevel);
    result->is_fullscreen = info->isFullscreen ? 1 : 0;
    delete info;

    return result;
}

SH_API void sh_free_selection_data(SelectionHook* hook, SHSelectionData* data) {
    if (!data) return;
    // Free heap-allocated strings from strdup in sh_get_current_selection.
    free(const_cast<char*>(data->text));
    free(const_cast<char*>(data->program_name));
    delete data;
}

SH_API int sh_set_selection_callback(SelectionHook* hook, SHSelectionCallback callback) {
    if (!hook) return SH_ERR_INVALID_ARG;
    hook->callback = callback;
    return SH_OK;
}

SH_API const char* sh_last_error(SelectionHook* hook) {
    if (!hook) return nullptr;
    if (!hook->last_error.empty()) return hook->last_error.c_str();
    if (hook->core) return hook->core->lastError();
    return nullptr;
}

SH_API const char* sh_last_global_error(void) {
    std::lock_guard<std::mutex> lock(g_global_error_mutex);
    if (g_global_error.empty()) return nullptr;
    return g_global_error.c_str();
}

SH_API int sh_set_mouse_callback(SelectionHook* hook, SHMouseCallback callback) {
    if (!hook) return SH_ERR_INVALID_ARG;
    hook->mouse_callback = callback;
    if (hook->core) hook->core->setMouseCallback(callback);
    return SH_OK;
}

SH_API int sh_set_keyboard_callback(SelectionHook* hook, SHKeyboardCallback callback) {
    if (!hook) return SH_ERR_INVALID_ARG;
    hook->keyboard_callback = callback;
    if (hook->core) hook->core->setKeyboardCallback(callback);
    return SH_OK;
}

SH_API int sh_enable_mouse_move(SelectionHook* hook) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    return SH_OK;
}

SH_API int sh_disable_mouse_move(SelectionHook* hook) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    return SH_OK;
}

SH_API int sh_set_config(SelectionHook* hook, const SHSelectionConfig* config) {
    if (!hook || !hook->core || !config) return SH_ERR_INVALID_ARG;
    hook->core->setDebugEnabled(config->debug != 0);
    hook->core->setClipboardEnabled(config->enable_clipboard != 0);
    hook->core->setPassiveMode(config->selection_passive_mode != 0);
    hook->core->setClipboardMode(static_cast<FilterMode>(config->clipboard_mode), {});
    hook->core->setGlobalFilterMode(static_cast<FilterMode>(config->global_filter_mode), {});
    return SH_OK;
}

SH_API int sh_set_passive_mode(SelectionHook* hook, int passive) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    hook->core->setPassiveMode(passive != 0);
    return SH_OK;
}

SH_API int sh_write_clipboard(SelectionHook* hook, const char* text) {
    if (!hook || !hook->core || !text) return SH_ERR_INVALID_ARG;
    if (WriteClipboard(std::string(text)))
        return SH_OK;
    hook->last_error = "Failed to write clipboard";
    return SH_ERR_GENERIC;
}

SH_API const char* sh_read_clipboard(SelectionHook* hook) {
    if (!hook || !hook->core) return nullptr;
    std::lock_guard<std::mutex> lock(hook->read_clipboard_mutex);
    if (!ReadClipboard(hook->read_clipboard_buf))
        return nullptr;
    if (hook->read_clipboard_buf.empty()) return nullptr;
    return hook->read_clipboard_buf.c_str();
}

SH_API int sh_mac_is_process_trusted(SelectionHook* hook) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    return SelectionHookCore::isProcessTrusted() ? 1 : 0;
}

SH_API int sh_mac_request_process_trust(SelectionHook* hook) {
    if (!hook || !hook->core) return SH_ERR_INVALID_ARG;
    return SelectionHookCore::requestProcessTrust() ? 1 : 0;
}

} // extern "C"
