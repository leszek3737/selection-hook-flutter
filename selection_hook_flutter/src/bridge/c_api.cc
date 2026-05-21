#include "c_api.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

struct SelectionHook {
    SHSelectionCallback callback = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<int> counter{0};
    std::mutex error_mutex;
    std::string last_error;
};

static thread_local std::string tls_last_error;
static std::mutex g_last_global_error_mutex;
static std::string g_last_global_error;

extern "C" {

SH_API SelectionHook* sh_create(void) {
    auto* hook = new (std::nothrow) SelectionHook();
    if (!hook) {
        std::lock_guard<std::mutex> lock(g_last_global_error_mutex);
        g_last_global_error = "Failed to allocate SelectionHook";
    }
    return hook;
}

SH_API void sh_destroy(SelectionHook* hook) {
    delete hook;
}

SH_API int sh_start(SelectionHook* hook) {
    if (!hook) return SH_ERR_INVALID_ARG;
    if (hook->running.load()) return SH_ERR_ALREADY_RUNNING;

    hook->running.store(true);
    hook->counter.store(0);

    hook->worker = std::thread([hook]() {
        const char* fake_programs[] = {"SmokeBrowser", "SmokeTerminal", "SmokeEditor"};
        int prog_idx = 0;

        while (hook->running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!hook->running.load() || !hook->callback) break;

            int n = hook->counter.fetch_add(1) + 1;
            const char* prog = fake_programs[prog_idx % 3];
            prog_idx++;

            std::ostringstream oss;
            oss << "smoke test #" << n;

            std::string text = oss.str();
            std::string prog_name = prog;
            std::string process = prog;

            SHSelectionData data;
            data.text = text.c_str();
            data.program_name = prog_name.c_str();
            data.start_top = {10, 20};
            data.start_bottom = {10, 40};
            data.end_top = {200, 20};
            data.end_bottom = {200, 40};
            data.mouse_start = {10, 20};
            data.mouse_end = {200, 40};
            data.method = SH_METHOD_PRIMARY;
            data.pos_level = SH_POS_LEVEL_SEL_FULL;
            data.is_fullscreen = 0;

            hook->callback(&data);
        }
    });

    return SH_OK;
}

SH_API int sh_stop(SelectionHook* hook) {
    if (!hook) return SH_ERR_INVALID_ARG;
    if (!hook->running.load()) return SH_ERR_NOT_RUNNING;

    hook->running.store(false);

    if (hook->worker.joinable()) {
        hook->worker.join();
    }

    return SH_OK;
}

SH_API int sh_is_running(SelectionHook* hook) {
    if (!hook) return 0;
    return hook->running.load() ? 1 : 0;
}

SH_API const SHSelectionData* sh_get_current_selection(SelectionHook* hook) {
    if (!hook) return nullptr;
    return nullptr;
}

SH_API void sh_free_selection_data(SelectionHook* hook, const SHSelectionData* data) {
    (void)hook;
    (void)data;
}

SH_API int sh_set_selection_callback(SelectionHook* hook, SHSelectionCallback callback) {
    if (!hook) return SH_ERR_INVALID_ARG;
    hook->callback = callback;
    return SH_OK;
}

SH_API const char* sh_last_error(SelectionHook* hook) {
    if (!hook) return nullptr;
    if (tls_last_error.empty()) return nullptr;
    return tls_last_error.c_str();
}

SH_API const char* sh_last_global_error(void) {
    std::lock_guard<std::mutex> lock(g_last_global_error_mutex);
    if (g_last_global_error.empty()) return nullptr;
    return g_last_global_error.c_str();
}

} // extern "C"
