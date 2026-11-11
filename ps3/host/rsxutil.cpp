// Based on the PSL1GHT msgdialog sample (MIT, Copyright (c) 2011 PSL1GHT Development Team).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "rsxutil.h"

// When each flip reached the screen (the timebase), for the log's report of how long each picture
// stayed up (rsxutil.h flip_times).
volatile u64 g_flip_times[kFlipRing];
volatile u32 g_flip_count = 0;

static void on_flip_done(const u32 head)
{
	(void)head;
	g_flip_times[g_flip_count % kFlipRing] = __builtin_ppc_get_timebase();
	__asm__ volatile("lwsync" ::: "memory");
	g_flip_count = g_flip_count + 1;
}

#define GCM_LABEL_INDEX		255

videoResolution res;
gcmContextData *context = NULL;

u32 curr_fb = 0;
u32 first_fb = 1;

u32 display_width;
u32 display_height;
u32 display_aspect;  // VIDEO_ASPECT_4_3 / _16_9 (as the PS3's display settings say)

u32 depth_pitch;
u32 depth_offset;
u32 *depth_buffer;

u32 color_pitch;
u32 color_offset[2];
u32 *color_buffer[2];

static u32 sLabelVal = 1;

static void waitFinish()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);

	rsxFlushBuffer(context);

	while(*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX)!=sLabelVal)
		usleep(30);

	++sLabelVal;
}

static void waitRSXIdle()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);
	rsxSetWaitLabel(context,GCM_LABEL_INDEX,sLabelVal);

	++sLabelVal;

	waitFinish();
}

void setRenderTarget(u32 index)
{
	gcmSurface sf;

	sf.colorFormat		= GCM_SURFACE_X8R8G8B8;
	sf.colorTarget		= GCM_SURFACE_TARGET_0;
	sf.colorLocation[0]	= GCM_LOCATION_RSX;
	sf.colorOffset[0]	= color_offset[index];
	sf.colorPitch[0]	= color_pitch;

	sf.colorLocation[1]	= GCM_LOCATION_RSX;
	sf.colorLocation[2]	= GCM_LOCATION_RSX;
	sf.colorLocation[3]	= GCM_LOCATION_RSX;
	sf.colorOffset[1]	= 0;
	sf.colorOffset[2]	= 0;
	sf.colorOffset[3]	= 0;
	sf.colorPitch[1]	= 64;
	sf.colorPitch[2]	= 64;
	sf.colorPitch[3]	= 64;

	sf.depthFormat		= GCM_SURFACE_ZETA_Z16;
	sf.depthLocation	= GCM_LOCATION_RSX;
	sf.depthOffset		= depth_offset;
	sf.depthPitch		= depth_pitch;

	sf.type				= GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias		= GCM_SURFACE_CENTER_1;

	sf.width			= display_width;
	sf.height			= display_height;
	sf.x				= 0;
	sf.y				= 0;

	rsxSetSurface(context,&sf);
}

void *host_mem = NULL;
u32 host_mem_size = 0;

void init_screen(void *host_addr,u32 size)
{
	host_mem = host_addr;
	host_mem_size = size;
	rsxInit(&context,CB_SIZE,size,host_addr);

	videoState state;
	videoGetState(0,0,&state);

	videoGetResolution(state.displayMode.resolution,&res);

	videoConfiguration vconfig;
	memset(&vconfig,0,sizeof(videoConfiguration));

	vconfig.resolution = state.displayMode.resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width*sizeof(u32);

	waitRSXIdle();

	videoConfigure(0,&vconfig,NULL,0);
	videoGetState(0,0,&state);

	gcmSetFlipMode(GCM_FLIP_VSYNC);
	gcmSetFlipHandler(on_flip_done);

	display_width = res.width;
	display_height = res.height;
	display_aspect = state.displayMode.aspect;

	color_pitch = display_width*sizeof(u32);
	color_buffer[0] = (u32*)rsxMemalign(64,(display_height*color_pitch));
	color_buffer[1] = (u32*)rsxMemalign(64,(display_height*color_pitch));

	rsxAddressToOffset(color_buffer[0],&color_offset[0]);
	rsxAddressToOffset(color_buffer[1],&color_offset[1]);

	gcmSetDisplayBuffer(0,color_offset[0],color_pitch,display_width,display_height);
	gcmSetDisplayBuffer(1,color_offset[1],color_pitch,display_width,display_height);

	depth_pitch = display_width*sizeof(u32);
	depth_buffer = (u32*)rsxMemalign(64,(display_height*depth_pitch)*2);
	rsxAddressToOffset(depth_buffer,&depth_offset);
}

extern "C" void ps3prof_block(int blocked) __attribute__((weak));

void waitflip()
{
	// Waiting for the display is idle time, not work (ps3/host/perf.c).
	if (ps3prof_block) ps3prof_block(1);
	while(gcmGetFlipStatus()!=0)
		usleep(200);
	if (ps3prof_block) ps3prof_block(0);
	gcmResetFlipStatus();
}

void flip()
{
	if(!first_fb) waitflip();
	else gcmResetFlipStatus();

	gcmSetFlip(context,curr_fb);
	rsxFlushBuffer(context);

	gcmSetWaitFlip(context);

	curr_fb ^= 1;
	setRenderTarget(curr_fb);

	first_fb = 0;
}

// Fill both display buffers with one color using the CPU (no shaders needed).
void clear_screen(u32 xrgb)
{
	for(int b=0;b<2;b++) {
		u32 *p = color_buffer[b];
		u32 n = (color_pitch/4)*display_height;
		for(u32 i=0;i<n;i++) p[i] = xrgb;
	}
}
