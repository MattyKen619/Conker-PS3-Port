// Based on the PSL1GHT msgdialog sample (MIT, Copyright (c) 2011 PSL1GHT Development Team).
#ifndef __RSXUTIL_H__
#define __RSXUTIL_H__

#include <ppu-types.h>

#include <rsx/rsx.h>

#define CB_SIZE		0x100000
#define HOST_SIZE	(18*1024*1024)	// IO-mapped: system command buffer, ours (2-6 MB), vertices (6-10 MB),
					// the SPU's two draw streams (10-14, 14-18 MB: the RSX reads their vertices in place)

extern gcmContextData *context;
extern u32 display_width;
extern u32 display_height;
extern u32 display_aspect;
extern u32 curr_fb;

void setRenderTarget(u32 index);
void init_screen(void *host_addr,u32 size);
// Flips that reached the screen: their times (the timebase), the last kFlipRing of them.
#define kFlipRing 1024
extern volatile u64 g_flip_times[kFlipRing];
extern volatile u32 g_flip_count;

void waitflip();
void flip();
void clear_screen(u32 xrgb);
extern u32 *color_buffer[2];
// The IO-mapped main memory given to init_screen (the system's command buffer uses its first 1 MB).
extern void *host_mem;
extern u32 host_mem_size;
extern u32 color_pitch;

#endif
