// enhancement_hooks.cpp - the PC enhancement hooks not ported yet, as no-ops (the widescreen ones are
// in widescreen_hooks.cpp).
//
// CBFD-Recompiled inserts these calls into the recompiled game (conker.toml, [[patches.hook]]).
// Each runs *before* an original instruction and only adds behavior (widescreen, mouse camera,
// aiming, frame interpolation, RT64 render fixes, achievements), so doing nothing gives the
// original N64 behavior. They can be ported one by one later, where they make sense on PS3.

#include "recomp.h"

#define NOOP_HOOK(name) extern "C" void name(uint8_t*, recomp_context*) {}

NOOP_HOOK(conker_aim_lock_on)
NOOP_HOOK(conker_aim_stick)
NOOP_HOOK(conker_bee_begin)
NOOP_HOOK(conker_bee_depth)
NOOP_HOOK(conker_bee_end)
NOOP_HOOK(conker_camera_turn_invert)
NOOP_HOOK(conker_camera_turn_speed_held)
NOOP_HOOK(conker_camera_turn_speed_released)
NOOP_HOOK(conker_depth_copy_camera)
NOOP_HOOK(conker_depth_copy_probes)
NOOP_HOOK(conker_field_of_view)
NOOP_HOOK(conker_frame_clear)

NOOP_HOOK(conker_leap_view_lift)
NOOP_HOOK(conker_leap_view_restore)
NOOP_HOOK(conker_light_glow_depth)
NOOP_HOOK(conker_look_currents)
NOOP_HOOK(conker_look_stick_pitch)
NOOP_HOOK(conker_look_stick_yaw_a)
NOOP_HOOK(conker_look_stick_yaw_b)
NOOP_HOOK(conker_look_targets)
NOOP_HOOK(conker_look_view)
NOOP_HOOK(conker_look_yaw_from_facing)
NOOP_HOOK(conker_look_yaw_from_facing_scaled)
NOOP_HOOK(conker_model_detail)
NOOP_HOOK(conker_mouse_camera)
NOOP_HOOK(conker_mouse_camera_aim)
NOOP_HOOK(conker_mouse_camera_collide)
NOOP_HOOK(conker_mouse_camera_follow)
NOOP_HOOK(conker_mouse_camera_look_mode)
NOOP_HOOK(conker_shotgun_laser)
NOOP_HOOK(conker_shotgun_laser_walk)
NOOP_HOOK(conker_sprite_piece_size)
NOOP_HOOK(conker_stat_change)
