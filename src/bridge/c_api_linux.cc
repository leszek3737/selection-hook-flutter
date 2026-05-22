#include "c_api.h"
#include "../linux/selection_hook_core.h"

#include <cstring>
#include <mutex>
#include <string>

struct SelectionHook
{
    SelectionHookCore *core = nullptr;
    SHSelectionCallback callback = nullptr;
    std::string last_error;
    SHMouseCallback mouse_callback = nullptr;
    SHKeyboardCallback keyboard_callback = nullptr;
};

static std::mutex g_global_error_mutex;
static std::string g_global_error;

extern "C"
{

    SH_API SelectionHook *sh_create(void)
    {
        auto *core = new (std::nothrow) SelectionHookCore();
        if (!core)
        {
            std::lock_guard<std::mutex> lock(g_global_error_mutex);
            g_global_error = "Failed to allocate SelectionHookCore";
            return nullptr;
        }

        auto *hook = new (std::nothrow) SelectionHook();
        if (!hook)
        {
            delete core;
            std::lock_guard<std::mutex> lock(g_global_error_mutex);
            g_global_error = "Failed to allocate SelectionHook";
            return nullptr;
        }

        hook->core = core;
        return hook;
    }

    SH_API void sh_destroy(SelectionHook *hook)
    {
        if (!hook)
            return;
        if (hook->core)
        {
            hook->core->stop();
            delete hook->core;
            hook->core = nullptr;
        }
        delete hook;
    }

    SH_API int sh_start(SelectionHook *hook)
    {
        if (!hook || !hook->core)
            return SH_ERR_INVALID_ARG;
        if (hook->core->isRunning())
            return SH_ERR_ALREADY_RUNNING;

        hook->core->setCallback(hook->callback);
        if (hook->mouse_callback)
            hook->core->setMouseCallback(hook->mouse_callback);
        if (hook->keyboard_callback)
            hook->core->setKeyboardCallback(hook->keyboard_callback);

        if (!hook->core->start())
        {
            hook->last_error = hook->core->lastError() ? hook->core->lastError() : "Failed to start selection hook";
            return SH_ERR_GENERIC;
        }

        return SH_OK;
    }

    SH_API int sh_stop(SelectionHook *hook)
    {
        if (!hook || !hook->core)
            return SH_ERR_INVALID_ARG;
        if (!hook->core->isRunning())
            return SH_ERR_NOT_RUNNING;

        hook->core->stop();
        return SH_OK;
    }

    SH_API int sh_is_running(SelectionHook *hook)
    {
        if (!hook || !hook->core)
            return 0;
        return hook->core->isRunning() ? 1 : 0;
    }

    SH_API SHSelectionData *sh_get_current_selection(SelectionHook *hook)
    {
        if (!hook || !hook->core)
            return nullptr;

        TextSelectionInfo *info = hook->core->getCurrentSelection();
        if (!info || info->text.empty())
        {
            delete info;
            return nullptr;
        }

        auto *result = new (std::nothrow) SHSelectionData();
        if (!result)
        {
            delete info;
            return nullptr;
        }

        result->text = strdup(info->text.c_str());
        result->program_name = strdup(info->programName.c_str());
        result->start_top = {INVALID_COORDINATE, INVALID_COORDINATE};
        result->start_bottom = {INVALID_COORDINATE, INVALID_COORDINATE};
        result->end_top = {INVALID_COORDINATE, INVALID_COORDINATE};
        result->end_bottom = {INVALID_COORDINATE, INVALID_COORDINATE};
        result->mouse_start = {
            info->mousePosStart.valid ? info->mousePosStart.x : INVALID_COORDINATE,
            info->mousePosStart.valid ? info->mousePosStart.y : INVALID_COORDINATE};
        result->mouse_end = {
            info->mousePosEnd.valid ? info->mousePosEnd.x : INVALID_COORDINATE,
            info->mousePosEnd.valid ? info->mousePosEnd.y : INVALID_COORDINATE};
        result->method = static_cast<int32_t>(info->method);
        result->pos_level = static_cast<int32_t>(info->posLevel);
        result->is_fullscreen = 0;
        delete info;

        return result;
    }

    SH_API void sh_free_selection_data(SelectionHook *hook, SHSelectionData *data)
    {
        (void)hook;
        if (!data)
            return;
        free(const_cast<char *>(data->text));
        free(const_cast<char *>(data->program_name));
        delete data;
    }

    SH_API int sh_set_selection_callback(SelectionHook *hook, SHSelectionCallback callback)
    {
        if (!hook)
            return SH_ERR_INVALID_ARG;
        hook->callback = callback;
        return SH_OK;
    }

    SH_API const char *sh_last_error(SelectionHook *hook)
    {
        if (!hook)
            return nullptr;
        if (!hook->last_error.empty())
            return hook->last_error.c_str();
        if (hook->core)
            return hook->core->lastError();
        return nullptr;
    }

    SH_API const char *sh_last_global_error(void)
    {
        std::lock_guard<std::mutex> lock(g_global_error_mutex);
        if (g_global_error.empty())
            return nullptr;
        return g_global_error.c_str();
    }

    SH_API int sh_set_mouse_callback(SelectionHook *hook, SHMouseCallback callback)
    {
        if (!hook)
            return SH_ERR_INVALID_ARG;
        hook->mouse_callback = callback;
        if (hook->core)
            hook->core->setMouseCallback(callback);
        return SH_OK;
    }

    SH_API int sh_set_keyboard_callback(SelectionHook *hook, SHKeyboardCallback callback)
    {
        if (!hook)
            return SH_ERR_INVALID_ARG;
        hook->keyboard_callback = callback;
        if (hook->core)
            hook->core->setKeyboardCallback(callback);
        return SH_OK;
    }

    SH_API int sh_enable_mouse_move(SelectionHook *hook)
    {
        if (!hook || !hook->core)
            return SH_ERR_INVALID_ARG;
        hook->core->enableMouseMove();
        return SH_OK;
    }

    SH_API int sh_disable_mouse_move(SelectionHook *hook)
    {
        if (!hook || !hook->core)
            return SH_ERR_INVALID_ARG;
        hook->core->disableMouseMove();
        return SH_OK;
    }

    SH_API int sh_set_config(SelectionHook *hook, const SHSelectionConfig *config)
    {
        if (!hook || !hook->core || !config)
            return SH_ERR_INVALID_ARG;
        hook->core->setDebugEnabled(config->debug != 0);
        hook->core->setClipboardEnabled(config->enable_clipboard != 0);
        hook->core->setPassiveMode(config->selection_passive_mode != 0);
        hook->core->setClipboardMode(static_cast<FilterMode>(config->clipboard_mode), {});
        hook->core->setGlobalFilterMode(static_cast<FilterMode>(config->global_filter_mode), {});
        if (config->enable_mouse_move)
            hook->core->enableMouseMove();
        else
            hook->core->disableMouseMove();
        return SH_OK;
    }

    SH_API int sh_set_passive_mode(SelectionHook *hook, int passive)
    {
        if (!hook || !hook->core)
            return SH_ERR_INVALID_ARG;
        hook->core->setPassiveMode(passive != 0);
        return SH_OK;
    }

    SH_API int sh_write_clipboard(SelectionHook *hook, const char *text)
    {
        (void)hook;
        (void)text;
        return SH_ERR_GENERIC;
    }

    SH_API const char *sh_read_clipboard(SelectionHook *hook)
    {
        (void)hook;
        return nullptr;
    }

    SH_API int sh_mac_is_process_trusted(SelectionHook *hook)
    {
        (void)hook;
        return 1;
    }

    SH_API int sh_mac_request_process_trust(SelectionHook *hook)
    {
        (void)hook;
        return SH_OK;
    }

} // extern "C"
