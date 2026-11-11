// Who draws on the screen: the main thread (status colors, dialogs) until the renderer starts on the
// runtime's graphics thread; from then on only the renderer touches the RSX.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>

#include "ultramodern/renderer_context.hpp"

extern std::mutex g_display_mutex;
extern std::atomic<bool> g_renderer_owns_display;

std::unique_ptr<ultramodern::renderer::RendererContext> ps3_create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
