/**
 * macOS-specific bridge between the C-ABI API (c_api.h) and the macOS core
 * (SelectionHookCore). Objective-C++ (needs .mm for Cocoa types included via
 * selection_hook_core.h).
 */

#include "c_api.h"
#include "../mac/selection_hook_core.h"

#include <mutex>
#include <string>

struct SelectionHook {
    SelectionHookCore* core = nullptr;
    SHSelectionCallback callback = nullptr;
    std::string last_error;
    std::mutex cache_mutex;
    SHSelectionData cached_sel_data = {};
    std::string cached_sel_text;
    std::string cached_sel_program;
    bool has_cached_selection = false;
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

SH_API const SHSelectionData* sh_get_current_selection(SelectionHook* hook) {
    if (!hook || !hook->core) return nullptr;

    TextSelectionInfo* info = hook->core->getCurrentSelection();
    if (!info || info->text.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(hook->cache_mutex);

    hook->cached_sel_text = info->text;
    hook->cached_sel_program = info->programName;

    hook->cached_sel_data.text = hook->cached_sel_text.c_str();
    hook->cached_sel_data.program_name = hook->cached_sel_program.c_str();
    hook->cached_sel_data.start_top = {
        static_cast<int32_t>(info->startTop.x),
        static_cast<int32_t>(info->startTop.y)
    };
    hook->cached_sel_data.start_bottom = {
        static_cast<int32_t>(info->startBottom.x),
        static_cast<int32_t>(info->startBottom.y)
    };
    hook->cached_sel_data.end_top = {
        static_cast<int32_t>(info->endTop.x),
        static_cast<int32_t>(info->endTop.y)
    };
    hook->cached_sel_data.end_bottom = {
        static_cast<int32_t>(info->endBottom.x),
        static_cast<int32_t>(info->endBottom.y)
    };
    hook->cached_sel_data.mouse_start = {
        static_cast<int32_t>(info->mousePosStart.x),
        static_cast<int32_t>(info->mousePosStart.y)
    };
    hook->cached_sel_data.mouse_end = {
        static_cast<int32_t>(info->mousePosEnd.x),
        static_cast<int32_t>(info->mousePosEnd.y)
    };
    hook->cached_sel_data.method = static_cast<int32_t>(info->method);
    hook->cached_sel_data.pos_level = static_cast<int32_t>(info->posLevel);
    hook->cached_sel_data.is_fullscreen = info->isFullscreen ? 1 : 0;
    hook->has_cached_selection = true;

    return &hook->cached_sel_data;
}

SH_API void sh_free_selection_data(SelectionHook* hook, const SHSelectionData* data) {
    if (!hook || !data) return;
    if (data == &hook->cached_sel_data) {
        std::lock_guard<std::mutex> lock(hook->cache_mutex);
        hook->has_cached_selection = false;
        hook->cached_sel_text.clear();
        hook->cached_sel_program.clear();
        hook->cached_sel_data.text = nullptr;
        hook->cached_sel_data.program_name = nullptr;
    }
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

} // extern "C"
