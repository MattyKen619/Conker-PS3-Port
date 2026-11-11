// The release build's XMB icon, made from the player's own ROM (as the Pikmin PS3 port makes its icon from
// the disc's banner). The package carries a stand-in icon marked as such (tools/release/mark_icon.py). On
// start, prepare() sees the mark; then, the first time the title screen's logo is on the screen, the
// renderer reads the picture, cuts the logo out, scales it to the XMB's 320 x 176 and saves it over the
// stand-in (ICON0.PNG in the program's folder). The repository and the package hold no game artwork.
// A package built with release/ICON0.local.png carries that icon instead, with no mark: kept as it is.
#pragma once
#include <cstdint>

#include "../gfx/n64gfx.h"
#include "../gfx/rsx_backend.h"

namespace xmb_icon {

// The mark in a stand-in icon (a PNG text chunk: keyword, NUL, text).
constexpr char kMark[] = "conker-ps3\0icon-from-rom";

// Main thread, at start: whether the icon is still the stand-in (then the renderer makes it).
void prepare();
// Whether the renderer should look for the title logo (cheap to call every frame).
bool wanted();
// The renderer, after each present: makes the icon once the logo has been on the screen a moment.
void after_present(n64gfx::RsxBackend& backend, n64gfx::Interpreter& interp, const n64gfx::RsxBackend::ViInfo& vi);

}  // namespace xmb_icon
