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

static u32 sEyeOffset = 0;  // 3D: where the eye being drawn starts in the display buffer

// 3D: draw into the right eye's lines (eye 1) or the left eye's (0) of display buffer `index`.
void setRenderTargetEye(u32 index,int eye)
{
	sEyeOffset = (stereo_on && eye) ? 750*color_pitch : 0;
	setRenderTarget(index);
	sEyeOffset = 0;
}

void setRenderTarget(u32 index)
{
	gcmSurface sf;

	sf.colorFormat		= GCM_SURFACE_X8R8G8B8;
	sf.colorTarget		= GCM_SURFACE_TARGET_0;
	sf.colorLocation[0]	= GCM_LOCATION_RSX;
	sf.colorOffset[0]	= color_offset[index] + sEyeOffset;
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

// Stereoscopic 3D (2.0): HDMI 720p frame packing (0x81), a 1280 x 1470 picture: the left eye's 720 lines,
// 30 blank ones, the right eye's 720. Everything draws the left eye's lines as a 1280 x 720 display (so
// display_width / display_height stay one eye's); the renderer draws the right eye's too (3D step 1:
// the same picture; later each eye draws its own).
int stereo_on = 0;
static u32 flat_mode, flat_width, flat_height, flat_pitch;
static u32 flat_buffer_bytes;

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
	flat_mode = state.displayMode.resolution;
	flat_width = display_width; flat_height = display_height; flat_pitch = color_pitch;
	flat_buffer_bytes = display_height*color_pitch;
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

// The renderer's flip (v88): queued without making the RSX wait there for the screen; the wait is owed
// and made (settle_flip_wait) just before the next drawing into a display buffer, so the RSX gets on with
// the next display list's own images meanwhile (at 60 fps it sat idle up to a refresh after every picture).
int flip_wait_owed = 0;

void settle_flip_wait()
{
	if(flip_wait_owed) { gcmSetWaitFlip(context); flip_wait_owed = 0; }
}

int stereo_set(int on)
{
	if(on==stereo_on) return 0;
	if(!first_fb) { while(gcmGetFlipStatus()!=0) usleep(200); }
	waitRSXIdle();
	videoConfiguration vc;
	memset(&vc,0,sizeof vc);
	vc.format = VIDEO_BUFFER_FORMAT_XRGB;
	if(on) {
		videoResolution r;
		if(videoGetResolution(VIDEO_RESOLUTION_720_3D_FRAME_PACKING,&r)!=0 || r.width!=1280 || r.height!=1470) {
			printf("[ps3] 3D: no 720p frame packing mode (%d)\n",(int)r.height);
			return -1;
		}
		const u32 pitch = 1280*4, bytes = pitch*1470;
		if(bytes>flat_buffer_bytes) {
			// The 2D display's buffers are too small (a 720p display): new ones (before the renderer takes the rest).
			for(int b=0;b<2;b++) {
				u32 *buf = (u32*)rsxMemalign(64,bytes);
				if(!buf) { printf("[ps3] 3D: no room for the display buffers\n"); return -2; }
				color_buffer[b] = buf;
				rsxAddressToOffset(buf,&color_offset[b]);
			}
			flat_buffer_bytes = bytes;
		}
		vc.resolution = VIDEO_RESOLUTION_720_3D_FRAME_PACKING;
		vc.pitch = pitch;
		const s32 rc = videoConfigure(0,&vc,NULL,0);
		if(rc!=0) { printf("[ps3] 3D: videoConfigure failed: 0x%08X\n",(u32)rc); return -3; }
		for(int b=0;b<2;b++) memset(color_buffer[b],0,bytes);  // the 30 lines between the eyes stay black
		color_pitch = pitch;
		display_width = 1280; display_height = 720;
		for(int b=0;b<2;b++) gcmSetDisplayBuffer(b,color_offset[b],pitch,1280,1470);
		stereo_on = 1;
	} else {
		vc.resolution = flat_mode;
		vc.pitch = flat_pitch;
		const s32 rc = videoConfigure(0,&vc,NULL,0);
		printf("[ps3] 3D off: videoConfigure 0x%02X: %d\n",flat_mode,(int)rc);
		for(int b=0;b<2;b++) memset(color_buffer[b],0,flat_buffer_bytes);
		color_pitch = flat_pitch;
		display_width = flat_width; display_height = flat_height;
		for(int b=0;b<2;b++) gcmSetDisplayBuffer(b,color_offset[b],flat_pitch,flat_width,flat_height);
		stereo_on = 0;
	}
	// Until the system says the new mode is on.
	for(int i=0;i<100;i++) {
		videoState st;
		videoGetState(0,0,&st);
		if(st.state==0 && st.displayMode.resolution==vc.resolution) { printf("[ps3] 3D %s: mode 0x%02X on after %d ms\n",on?"on":"off",vc.resolution,i*50); break; }
		usleep(50000);
	}
	setRenderTarget(curr_fb);
	return 0;
}

void stereo_leave_for_exit()
{
	// Just the video mode (the renderer may be using the RSX from its own thread): back to 2D before the
	// process ends.
	if(!stereo_on) return;
	videoConfiguration vc;
	memset(&vc,0,sizeof vc);
	vc.format = VIDEO_BUFFER_FORMAT_XRGB;
	vc.resolution = flat_mode;
	vc.pitch = flat_pitch;
	const s32 rc = videoConfigure(0,&vc,NULL,0);
	printf("[ps3] 3D off for exit: videoConfigure 0x%02X: %d\n",flat_mode,(int)rc);
	stereo_on = 0;
}

int flip_pending()
{
	return !first_fb && gcmGetFlipStatus()!=0;
}

void flip_queue()
{
	if(!first_fb) waitflip();
	else gcmResetFlipStatus();

	gcmSetFlip(context,curr_fb);
	rsxFlushBuffer(context);
	flip_wait_owed = 1;

	curr_fb ^= 1;
	setRenderTarget(curr_fb);

	first_fb = 0;
}

void flip()
{
	settle_flip_wait();
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
		u32 n = (color_pitch/4)*(stereo_on ? 1470 : display_height);
		for(u32 i=0;i<n;i++) p[i] = xrgb;
	}
}
