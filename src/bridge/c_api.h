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
 * ## Scope
 * This header covers the full API surface including mouse events, keyboard
 * events, clipboard, filtering, passive mode, and platform-specific helpers.
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

/**
 * Mouse/wheel event data.
 *
 * event_type: 0=mouse-down, 1=mouse-up, 2=mouse-move, 3=mouse-wheel
 * button: None=-1, Left=0, Middle=1, Right=2, Back=3, Forward=4, Unknown=99.
 *   For mouse-wheel: 0=Vertical, 1=Horizontal.
 * flag: wheel direction (1=Up/Right, -1=Down/Left; 0 for non-wheel events).
 *
 * Sentinel INVALID_COORDINATE (-99999) applies to x/y on Linux Wayland.
 */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t button;
    int32_t event_type;
    int32_t flag;
} SHMouseEventData;

/**
 * Keyboard event data.
 *
 * uni_key: MDN KeyboardEvent.key value (UTF-8, NUL-terminated).
 *   Library-owned, valid only during callback.
 * vk_code: Platform-specific virtual key code.
 *   Windows=VK_*, macOS=kVK_*, Linux=KEY_* from linux/input-event-codes.h.
 * sys: Non-zero if Alt/Ctrl/Win/Cmd/Fn modifier pressed simultaneously.
 * flags: Additional platform-specific flags (Linux: modifier bitmask).
 */
typedef struct {
    const char* uni_key;
    int32_t vk_code;
    int32_t sys;
    int32_t flags;
} SHKeyboardEventData;

/** Mouse event callback. data valid only during callback. */
typedef void (*SHMouseCallback)(const SHMouseEventData* data);

/** Keyboard event callback. data valid only during callback. */
typedef void (*SHKeyboardCallback)(const SHKeyboardEventData* data);

/**
 * Configuration for text selection monitoring.
 * All fields are optional — pass 0 for defaults.
 */
typedef struct {
    int32_t debug;                    // 1=enable diagnostic logging
    int32_t enable_mouse_move;        // 1=enable high-CPU mouse-move events
    int32_t enable_clipboard;        // 1=enable clipboard fallback (default: 1)
    int32_t selection_passive_mode;  // 1=require manual trigger for selection
    int32_t clipboard_mode;          // FilterMode: 0=Default, 1=IncludeList, 2=ExcludeList
    int32_t global_filter_mode;      // FilterMode for global program filter
} SHSelectionConfig;

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
 * the pointer returned by sh_get_current_selection.
 * Caller owns the returned data — MUST call sh_free_selection_data.
 *
 * @param hook Valid SelectionHook instance.
 * @return Current selection data (caller-owned), or NULL.
 * @thread_safety Safe from any thread.
 */
SH_API SHSelectionData* sh_get_current_selection(SelectionHook* hook);

/**
 * Free a SHSelectionData previously returned by sh_get_current_selection.
 *
 * Frees the heap-allocated copy including its string fields.
 * Safe to call with NULL.
 *
 * @param hook Valid SelectionHook instance (unused, kept for API consistency).
 * @param data Pointer returned by sh_get_current_selection, or NULL (no-op).
 * @thread_safety Safe from any thread.
 */
SH_API void sh_free_selection_data(SelectionHook* hook, SHSelectionData* data);

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
 * Mouse Events
 * --------------------------------------------------------------------------- */

/**
 * Register a callback for mouse/wheel events.
 *
 * The callback is invoked from native system threads. Data pointers are
 * valid only for the duration of the callback — Dart MUST copy immediately.
 *
 * Must be called before sh_start. Calling after sh_start has undefined
 * behavior — stop first.
 *
 * @param hook Valid SelectionHook instance.
 * @param callback Function pointer, or NULL to unregister.
 * @return SH_OK (0) on success.
 * @thread_safety May be called from any thread before sh_start.
 */
SH_API int sh_set_mouse_callback(SelectionHook* hook, SHMouseCallback callback);

/**
 * Enable mouse-move events.
 *
 * Mouse-move events are high-CPU on some platforms. Only enable if your
 * application needs per-pixel mouse tracking.
 *
 * @param hook Valid SelectionHook instance (must be started).
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety Safe from any thread.
 */
SH_API int sh_enable_mouse_move(SelectionHook* hook);

/**
 * Disable mouse-move events.
 *
 * Stops receiving mouse-move callbacks. Mouse-down, mouse-up, and
 * mouse-wheel events are unaffected.
 *
 * @param hook Valid SelectionHook instance (must be started).
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety Safe from any thread.
 */
SH_API int sh_disable_mouse_move(SelectionHook* hook);

/* ---------------------------------------------------------------------------
 * Keyboard Events
 * --------------------------------------------------------------------------- */

/**
 * Register a callback for keyboard events.
 *
 * The callback is invoked from native system threads. Data pointers are
 * valid only for the duration of the callback — Dart MUST copy immediately.
 *
 * Must be called before sh_start. Calling after sh_start has undefined
 * behavior — stop first.
 *
 * @param hook Valid SelectionHook instance.
 * @param callback Function pointer, or NULL to unregister.
 * @return SH_OK (0) on success.
 * @thread_safety May be called from any thread before sh_start.
 */
SH_API int sh_set_keyboard_callback(SelectionHook* hook, SHKeyboardCallback callback);

/* ---------------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------------- */

/**
 * Apply a full configuration to the hook.
 *
 * All fields in SHSelectionConfig are optional — pass 0/0.0 for defaults.
 * Overwrites any previously set configuration. Must be called before sh_start.
 *
 * @param hook Valid SelectionHook instance.
 * @param config Pointer to configuration struct.
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety May be called from any thread before sh_start.
 */
SH_API int sh_set_config(SelectionHook* hook, const SHSelectionConfig* config);

/**
 * Enable or disable passive mode.
 *
 * In passive mode, text selection is only detected when the user explicitly
 * triggers it (e.g., via a system shortcut), rather than automatically on
 * every mouse-release.
 *
 * @param hook Valid SelectionHook instance (must be started).
 * @param passive 1=enable passive mode, 0=disable.
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety Safe from any thread.
 */
SH_API int sh_set_passive_mode(SelectionHook* hook, int passive);

/* ---------------------------------------------------------------------------
 * Clipboard (macOS / Windows only, returns error on Linux)
 * --------------------------------------------------------------------------- */

/**
 * Write text to the system clipboard.
 *
 * On Linux, this function returns SH_ERR_GENERIC — clipboard write-back
 * is not supported on Linux via this API.
 *
 * @param hook Valid SelectionHook instance.
 * @param text UTF-8 NUL-terminated string to place on the clipboard.
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety Safe from any thread.
 */
SH_API int sh_write_clipboard(SelectionHook* hook, const char* text);

/**
 * Read text from the system clipboard.
 *
 * Returns the current clipboard contents as UTF-8, or NULL if the
 * clipboard is empty or contains non-text data.
 *
 * On Linux, this function returns NULL — clipboard read-back is not
 * supported on Linux via this API.
 *
 * The returned string is owned by the library and is valid until the
 * next call to sh_read_clipboard from the same thread.
 *
 * @param hook Valid SelectionHook instance.
 * @return Clipboard text (library-owned), or NULL.
 * @thread_safety Safe from any thread.
 */
SH_API const char* sh_read_clipboard(SelectionHook* hook);

/* ---------------------------------------------------------------------------
 * Platform-Specific
 * --------------------------------------------------------------------------- */

/**
 * Check if process is trusted for accessibility (macOS only).
 *
 * On non-macOS platforms, always returns 1 (trusted).
 *
 * @param hook Valid SelectionHook instance.
 * @return 1 if trusted or non-macOS platform, 0 if not trusted.
 * @thread_safety Safe from any thread.
 */
SH_API int sh_mac_is_process_trusted(SelectionHook* hook);

/**
 * Try to request accessibility permissions (macOS only, may show dialog).
 *
 * On non-macOS platforms, always returns SH_OK.
 *
 * @param hook Valid SelectionHook instance.
 * @return SH_OK (0) on success, negative error code on failure.
 * @thread_safety Safe from any thread. May present a system dialog — be
 *                mindful of UI context when calling.
 */
SH_API int sh_mac_request_process_trust(SelectionHook* hook);

#ifdef __cplusplus
}
#endif

#endif /* SELECTION_HOOK_C_API_H */
