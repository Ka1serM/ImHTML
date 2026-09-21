#pragma once

// Host-platform services ImHTML needs but does not own.
//
// The framework is windowing-system agnostic: it draws through an ImGui draw
// list and never creates a window. Only two things genuinely need the platform
// — opening a URL in the user's browser, and telling the compositor that a text
// field wants keyboard composition events.
//
// Both default to no-ops, and ImHTML links no windowing library of its own. A
// host installs implementations through SetPlatformHooks before the first
// frame; with SDL3 that is SDL_OpenURL and SDL_StartTextInput/SDL_StopTextInput,
// with GLFW/Win32/Qt it is the equivalent two calls. Leaving them unset costs
// only link-out-to-browser and IME activation.

#include <functional>
#include <string>

namespace ImHTML {

struct PlatformHooks {
    // Opens an absolute URL in the user's default browser. Returns false when
    // the platform declined or has no browser integration.
    std::function<bool(const std::string& url)> open_url;

    // Starts or stops keyboard text composition (IME) for the focused window.
    std::function<void(bool active)> set_text_input_active;
};

inline PlatformHooks& GetPlatformHooks() {
    static PlatformHooks hooks;
    return hooks;
}

// Replaces the platform integration. Unset members keep the current behaviour.
inline void SetPlatformHooks(PlatformHooks hooks) {
    PlatformHooks& current = GetPlatformHooks();
    if (hooks.open_url) current.open_url = std::move(hooks.open_url);
    if (hooks.set_text_input_active) current.set_text_input_active = std::move(hooks.set_text_input_active);
}

namespace Platform {

inline bool OpenURL(const std::string& url) {
    const auto& hook = GetPlatformHooks().open_url;
    return hook ? hook(url) : false;
}

inline void SetTextInputActive(const bool active) {
    if (const auto& hook = GetPlatformHooks().set_text_input_active) hook(active);
}

}  // namespace Platform
}  // namespace ImHTML
