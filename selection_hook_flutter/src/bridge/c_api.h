/**
 * @file c_api.h
 * @brief C-ABI specification for selection-hook Flutter FFI plugin.
 *
 * ## Memory Ownership Rules
 *
 * ### Outgoing Strings/Structures
 * - Functions prefixed `sh_get_*` returning `const char*` or a struct via
 *   pointer: memory is owned by the library. Lifetime = until the next call to
 *   the same function OR until `sh_free_*` is called (documented per function).
 * - Dart MUST copy data before the next call if it needs to retain it.
 * - In SHSelectionCallback: all pointers are valid only for the duration of
 *   the callback. Copy immediately in the Dart listener.
 *
 * ### Incoming Strings
 * - `const char*` passed INTO the library: UTF-8, NUL-terminated. The library
 *   copies internally. Caller may immediately free after the call returns.
 *
 * ## Error Convention
 * - Functions returning `int`: 0 = OK, negative = error code.
 * - `sh_last_error(hook)` returns a human-readable description of the last
 *   error on the calling thread, valid until the next call from the same thread.
 * - Constructor functions: return NULL on error; use `sh_last_global_error()`
 *   to retrieve the error description.
 *
 * ## Thread Safety
 * - All `sh_*` functions are thread-safe. May be called from any Dart isolate.
 * - SHSelectionCallback IS invoked from native threads (CGEventTap RunLoop on
 *   macOS, hook thread on Windows, X11 event loop on Linux). This is expected.
 *   Dart must use `NativeCallable.listener` to receive callbacks.
 * - `sh_stop` is synchronous: blocks until no callback is in-flight.
 *
 * ## MVP Scope
 * This header covers the minimal viable subset. Extensions (mouse events,
 * keyboard events, clipboard, filtering, passive mode, Linux env info) will be
 * added in a later phase.
 */

#ifndef SELECTION_HOOK_C_API_H
#define SELECTION_HOOK_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Platform export macro
 * --------------------------------------------------------------------------- */

#if defined(_WIN32)
#  define SH_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define SH_API __attribute__((visibility("default")))
#else
#  define SH_API
#endif

/* ---------------------------------------------------------------------------
 * Opaque handle
 * --------------------------------------------------------------------------- */

/**
 * Opaque handle to the SelectionHook native instance.
 * Created by sh_create(), destroyed by sh_destroy().
 */
typedef struct SelectionHook SelectionHook;

/* ---------------------------------------------------------------------------
 * POD Structures
 * --------------------------------------------------------------------------- */

/** 2D coordinate in screen pixels. */
typedef struct {
    int32_t x;
    int32_t y;
} SHPoint;

/**
 * Text selection data returned via callback or sh_get_current_selection.
 *
 * All `const char*` fields are owned by the library. In callbacks, they are
 * valid only for the duration of the callback. In sh_get_current_selection, they
 * are valid until the next call or until sh_free_selection_data.
 *
 * Sentinel value for unavailable coordinates: -99999 (INVALID_COORDINATE).
 * On Linux, startTop/startBottom/endTop/endBottom are always INVALID_COORDINATE.
 * On Linux Wayland, mouseStart/mouseEnd may also be INVALID_COORDINATE.
 */
typedef struct {
    /** Selected text content (UTF-8, NUL-terminated). */
    const char* text;
    /** Program name that triggered the selection (UTF-8, NUL-terminated). */
    const char* program_name;
    /** First paragraph top-left (screen pixels). */
    SHPoint start_top;
    /** First paragraph bottom-left (screen pixels). */
    SHPoint start_bottom;
    /** Last paragraph top-right (screen pixels). */
    SHPoint end_top;
    /** Last paragraph bottom-right (screen pixels). */
    SHPoint end_bottom;
    /** Mouse position when selection started (screen pixels). */
    SHPoint mouse_start;
    /** Mouse position when selection ended (screen pixels). */
    SHPoint mouse_end;
    /** Selection method (enum SHSelectionMethod). */
    int32_t method;
    /** Position level (enum SHPositionLevel). */
    int32_t pos_level;
    /** Fullscreen state of the source window (macOS only, 0=unknown/no, 1=yes). */
    int32_t is_fullscreen;
} SHSelectionData;

/* ---------------------------------------------------------------------------
 * Enumerations
 * --------------------------------------------------------------------------- */

/** Selection method identifiers. */
typedef enum {
    SH_METHOD_NONE       = 0,
    SH_METHOD_UIA        = 1,
    /** Removed upstream, kept for alignment. */
    SH_METHOD_FOCUSCTL   = 2,
    SH_METHOD_ACCESSIBLE = 3,
    SH_METHOD_AXAPI      = 11,
    /** @reserved AT-SPI method reserved for future use. */
    SH_METHOD_ATSPI      = 21,
    SH_METHOD_PRIMARY    = 22,
    SH_METHOD_CLIPBOARD  = 99,
} SHSelectionMethod;

/** Position level identifiers. */
typedef enum {
    SH_POS_LEVEL_NONE          = 0,
    SH_POS_LEVEL_MOUSE_SINGLE  = 1,
    SH_POS_LEVEL_MOUSE_DUAL    = 2,
    SH_POS_LEVEL_SEL_FULL      = 3,
    SH_POS_LEVEL_SEL_DETAILED  = 4,
} SHPositionLevel;

/* ---------------------------------------------------------------------------
 * Callback Types
 * --------------------------------------------------------------------------- */

/**
 * Text selection callback.
 *
 * Called from a native thread (CGEventTap RunLoop, Windows hook thread,
 * X11 event loop, etc.). The `data` pointer is valid ONLY for the duration
 * of the callback. Dart MUST copy any needed fields before returning.
 *
 * Must be registered via sh_set_selection_callback before sh_start.
 *
 * @param data Selection data. Lifetime = callback duration. Copy immediately.
 */
typedef void (*SHSelectionCallback)(const SHSelectionData* data);

/* ---------------------------------------------------------------------------
 * Error Codes
 * --------------------------------------------------------------------------- */

#define SH_OK                   0
#define SH_ERR_GENERIC         (-1)
#define SH_ERR_INVALID_ARG     (-2)
#define SH_ERR_ALREADY_RUNNING (-3)
#define SH_ERR_NOT_RUNNING     (-4)
#define SH_ERR_NOT_TRUSTED     (-5)

/* ---------------------------------------------------------------------------
 * Lifecycle Functions
 * --------------------------------------------------------------------------- */

/**
 * Create a new SelectionHook instance.
 *
 * Allocates and initializes platform-specific resources.
 * Call sh_set_selection_callback BEFORE sh_start.
 *
 * @return Opaque handle, or NULL on failure. On failure, call
 *         sh_last_global_error() for details.
 * @thread_safety Safe from any thread.
 */
SH_API SelectionHook* sh_create(void);

/**
 * Destroy a SelectionHook instance.
 *
 * Caller MUST ensure sh_stop has returned before calling sh_destroy.
 * Releases all native resources.
 *
 * @param hook Instance to destroy. May be NULL (no-op).
 * @thread_safety Safe from any thread. Not safe if callback is in-flight
 *                (caller responsibility to serialize with sh_stop).
 */
SH_API void sh_destroy(SelectionHook* hook);

/**
 * Start monitoring text selections.
 *
 * Initializes platform-specific hooks and begins monitoring. A callback
 * must have been registered via sh_set_selection_callback before calling
 * this function.
 *
 * On macOS, this will call AXIsProcessTrustedWithOptions with prompt
 * before any AXAPI calls. The user must grant accessibility permissions.
 *
 * @param hook Valid SelectionHook instance.
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety Safe from any thread. The callback will be invoked from
 *                native threads.
 */
SH_API int sh_start(SelectionHook* hook);

/**
 * Stop monitoring text selections.
 *
 * Unregisters platform hooks and blocks until no callback is in-flight.
 * After sh_stop returns, it is safe to sh_destroy and/or close the
 * NativeCallable.
 *
 * @param hook Valid SelectionHook instance.
 * @return SH_OK (0) on success, SH_ERR_NOT_RUNNING if not started.
 * @thread_safety Safe from any thread. Blocks until in-flight callbacks
 *                complete.
 */
SH_API int sh_stop(SelectionHook* hook);

/**
 * Check if the hook is currently running.
 *
 * @param hook Valid SelectionHook instance.
 * @return 1 if running, 0 if not running.
 * @thread_safety Safe from any thread.
 */
SH_API int sh_is_running(SelectionHook* hook);

/* ---------------------------------------------------------------------------
 * Selection Functions
 * --------------------------------------------------------------------------- */

/**
 * Get the current text selection synchronously.
 *
 * Returns a snapshot of the current selection, or NULL if no text is
 * selected or the hook is not running.
 *
 * The returned SHSelectionData* is owned by the library. It is valid until
 * the next call to sh_get_current_selection OR until
 * sh_free_selection_data is called on the same hook, whichever comes first.
 * Dart MUST copy the data before the next call.
 *
 * @param hook Valid SelectionHook instance.
 * @return Current selection data (library-owned), or NULL.
 * @thread_safety Safe from any thread.
 */
SH_API const SHSelectionData* sh_get_current_selection(SelectionHook* hook);

/**
 * Free a SHSelectionData previously returned by sh_get_current_selection.
 *
 * This is an explicit release. It is also implicitly released by the next
 * sh_get_current_selection call. Safe to call with NULL.
 *
 * @param hook Valid SelectionHook instance.
 * @param data Pointer returned by sh_get_current_selection, or NULL (no-op).
 * @thread_safety Safe from any thread.
 */
SH_API void sh_free_selection_data(SelectionHook* hook, const SHSelectionData* data);

/* ---------------------------------------------------------------------------
 * Callback Registration
 * --------------------------------------------------------------------------- */

/**
 * Register the text selection callback.
 *
 * Stores a function pointer that will be called from native threads when
 * a text selection is detected. Must be called before sh_start.
 *
 * The callback will be invoked from native system threads. Use
 * `NativeCallable<Void Function(Pointer<SHSelectionData>)>.listener(...)`
 * on the Dart side to receive these callbacks.
 *
 * Calling this after sh_start has undefined behavior — stop first.
 *
 * @param hook Valid SelectionHook instance.
 * @param callback Function pointer, or NULL to unregister.
 * @return SH_OK (0) on success.
 * @thread_safety May be called from any thread before sh_start.
 */
SH_API int sh_set_selection_callback(SelectionHook* hook, SHSelectionCallback callback);

/* ---------------------------------------------------------------------------
 * Error Information
 * --------------------------------------------------------------------------- */

/**
 * Get the last error description for this hook instance.
 *
 * Returns a human-readable description of the last error that occurred on
 * the calling thread. The returned string is owned by the library and is
 * valid until the next call to sh_last_error from the same thread.
 *
 * @param hook Valid SelectionHook instance.
 * @return Error description (UTF-8, NUL-terminated), or NULL if no error.
 * @thread_safety Thread-local — each thread gets its own error state.
 */
SH_API const char* sh_last_error(SelectionHook* hook);

/**
 * Get the last global error description (for constructor failures).
 *
 * Used when sh_create returns NULL. Returns a description of why creation
 * failed. The returned string is owned by the library and is valid until
 * the next call to sh_last_global_error.
 *
 * @return Error description (UTF-8, NUL-terminated), or NULL if no error.
 * @thread_safety Thread-safe. Returns the most recent global error.
 */
SH_API const char* sh_last_global_error(void);

/* ---------------------------------------------------------------------------
 * Future Extensions (NOT IMPLEMENTED in MVP)
 *
 * The following functions will be added in a later phase:
 *
 *   // Mouse events
 *   SH_API int sh_set_mouse_callback(SelectionHook*, SHMouseCallback);
 *   SH_API int sh_enable_mouse_move(SelectionHook*);
 *   SH_API int sh_disable_mouse_move(SelectionHook*);
 *
 *   // Keyboard events
 *   SH_API int sh_set_keyboard_callback(SelectionHook*, SHKeyboardCallback);
 *
 *   // Clipboard
 *   SH_API int sh_enable_clipboard(SelectionHook*);
 *   SH_API int sh_disable_clipboard(SelectionHook*);
 *   SH_API int sh_set_clipboard_mode(SelectionHook*, int mode,
 *                                     const char** program_list, int count);
 *   SH_API int sh_write_clipboard(SelectionHook*, const char* text);
 *   SH_API const char* sh_read_clipboard(SelectionHook*);
 *
 *   // Configuration
 *   SH_API int sh_set_global_filter_mode(SelectionHook*, int mode,
 *                                         const char** program_list, int count);
 *   SH_API int sh_set_fine_tuned_list(SelectionHook*, int list_type,
 *                                      const char** program_list, int count);
 *   SH_API int sh_set_passive_mode(SelectionHook*, int passive);
 *
 *   // Platform-specific
 *   SH_API int sh_mac_is_process_trusted(SelectionHook*);
 *   SH_API int sh_mac_request_process_trust(SelectionHook*);
 *   SH_API int sh_linux_get_env_info(SelectionHook*, ...);
 * --------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* SELECTION_HOOK_C_API_H */
