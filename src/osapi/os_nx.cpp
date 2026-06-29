/*
 * os_nx.cpp - Nintendo Switch (libnx + SDL2) OS / application layer.
 * Replaces os_unix.cpp: the SDL2 event pump, the controller / touch-screen ->
 * keyboard / mouse / joystick bridge, HD rumble, gyro aiming and text entry.
 */

#ifdef SWITCH

#include <switch.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "pstypes.h"
#include "osapi.h"
#include "key.h"
#include "palman.h"
#include "mouse.h"
#include "outwnd.h"
#include "2d.h"
#include "cfile.h"
#include "sound.h"
#include "freespaceresource.h"
#include "managepilot.h"
#include "joy.h"
#include "joy_ff.h"
#include "gamesequence.h"
#include "freespace.h"
#include "osregistry.h"
#include "cmdline.h"
#include "timer.h"
#include "switch_input.h"

static int  fAppActive = 1;
static int  Os_inited = 0;
static CRITICAL_SECTION Os_lock;
int Os_debugger_running = 0;

extern int SDLtoFS2[];		// SDL scancode -> FreeSpace key table (key.cpp)

void os_deinit();

// config.txt settings
static int	Nx_cfg_rumble = 1;
static int	Nx_cfg_gyro = 0;
static int	Nx_cfg_gyro_sens = 50;

const char *detect_home(void)
{
	return ".";
}

// Read config.txt from the working directory, writing defaults on first boot.
static void nx_load_config(void)
{
	FILE *fp = fopen("config.txt", "r");

	if (fp == NULL) {
		fp = fopen("config.txt", "w");
		if (fp != NULL) {
			fprintf(fp,
				"rumble = 1\n"
				"gyro = 0\n"
				"gyro_sensitivity = 50\n");
			fclose(fp);
		}
		return;
	}

	char line[256];
	while (fgets(line, sizeof(line), fp) != NULL) {
		char *hash = strchr(line, '#');
		if (hash) *hash = '\0';

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';

		char *k = line;
		while (*k == ' ' || *k == '\t') k++;
		char *ke = eq - 1;
		while (ke >= k && (*ke == ' ' || *ke == '\t' || *ke == '\r' || *ke == '\n')) *ke-- = '\0';

		char *v = eq + 1;
		while (*v == ' ' || *v == '\t') v++;

		int n = atoi(v);
		if (!strcmp(k, "rumble"))                Nx_cfg_rumble = n ? 1 : 0;
		else if (!strcmp(k, "gyro"))             Nx_cfg_gyro = n ? 1 : 0;
		else if (!strcmp(k, "gyro_sensitivity")) Nx_cfg_gyro_sens = (n < 0) ? 0 : ((n > 100) ? 100 : n);
	}
	fclose(fp);
}

void os_init(const char *wclass, const char *title, const char *app_name, const char *version_string)
{
#ifndef NDEBUG
	outwnd_init(1);
#endif

	nx_load_config();

	// default to 1024x768 (used only if sparky_hi_fs2.vp is present, else 640x480)
	if (os_config_read_string(NULL, NOX("Videocard"), NULL) == NULL)
		os_config_write_string(NULL, NOX("Videocard"), NOX("OpenGL (1024x768)"));

	if (os_config_read_string(NULL, NOX("NetworkConnection"), NULL) == NULL)
		os_config_write_string(NULL, NOX("NetworkConnection"), NOX("lan"));

	if (os_config_read_string(NULL, NOX("ConnectionSpeed"), NULL) == NULL)
		os_config_write_string(NULL, NOX("ConnectionSpeed"), NOX("Slow"));

	// fly with the left stick, not the mouse
	extern int Use_mouse_to_fly;
	extern int Keep_mouse_centered;
	Use_mouse_to_fly = 0;
	Keep_mouse_centered = 0;

	Os_inited = 1;
	Os_lock = SDL_CreateMutex();

	nx_input_init();

	atexit(os_deinit);
}

void os_set_title(const char *title)
{
}

void os_cleanup()
{
#ifndef NDEBUG
	outwnd_close();
#endif
}

int os_foreground()
{
	return fAppActive;
}

uint os_get_window()
{
	return 0;
}

void os_sleep(int ms)
{
	SDL_Delay(ms);
}

void os_suspend()
{
	ENTER_CRITICAL_SECTION(&Os_lock);
}

void os_resume()
{
	LEAVE_CRITICAL_SECTION(&Os_lock);
}

void os_check_debugger()
{
}

void os_deinit()
{
	if (Os_lock != NULL) {
		SDL_DestroyMutex(Os_lock);
		Os_lock = NULL;
	}
	SDL_Quit();
}

// ---- controller / touch -> keyboard / mouse / gyro bridge ----

static PadState	Nx_pad;
static int		Nx_input_inited = 0;

// virtual mouse cursor (game-screen coordinates)
static int			Nx_mouse_x = 0;
static int			Nx_mouse_y = 0;
static unsigned int	Nx_mouse_buttons = 0;
static int			Nx_mouse_seeded = 0;
static int			Nx_kbd_request = 0;

static HidVibrationDeviceHandle	Nx_vibe_hh[2];
static HidVibrationDeviceHandle	Nx_vibe_no1[2];
static int		Nx_vibe_inited = 0;
static int		Nx_rumble_pulse_end = 0;
static float	Nx_rumble_pulse_amp = 0.0f;
static float	Nx_rumble_continuous = 0.0f;

static HidSixAxisSensorHandle	Nx_sixaxis_hh;
static HidSixAxisSensorHandle	Nx_sixaxis_no1;
static int		Nx_gyro_started = 0;
static float	Nx_gyro_yaw = 0.0f;
static float	Nx_gyro_pitch = 0.0f;

#define NX_GYRO_SCALE	(65536.0f * 8.0f)

#define NX_STICK_CURSOR_SPEED	14.0f
#define NX_STICK_DEADZONE		3500

// sentinel "key": act as the left mouse button (menu selection)
#define KEY_NX_MOUSE_LEFT	0x7000

typedef struct nx_btn_map {
	u64	mask;
	int	key_mission;
	int	key_menu;
} nx_btn_map;

// default bindings (flight controls are remappable in Options -> Control Config)
static const nx_btn_map Nx_buttons[] = {
	{ HidNpadButton_A,		KEY_LCTRL,		KEY_NX_MOUSE_LEFT },
	{ HidNpadButton_B,		KEY_SPACEBAR,	KEY_ESC          },
	{ HidNpadButton_X,		KEY_X,			KEY_ENTER        },
	{ HidNpadButton_Y,		KEY_Y,			KEY_TAB          },
	{ HidNpadButton_L,		KEY_H,			KEY_PAGEUP       },
	{ HidNpadButton_R,		KEY_TAB,		KEY_PAGEDOWN     },
	{ HidNpadButton_ZL,		KEY_SPACEBAR,	KEY_NX_MOUSE_LEFT },
	{ HidNpadButton_ZR,		KEY_LCTRL,		KEY_NX_MOUSE_LEFT },
	{ HidNpadButton_Plus,	KEY_ESC,		KEY_ESC          },
	{ HidNpadButton_Up,		KEY_UP,			KEY_UP           },
	{ HidNpadButton_Down,	KEY_DOWN,		KEY_DOWN         },
	{ HidNpadButton_Left,	KEY_LEFT,		KEY_LEFT         },
	{ HidNpadButton_Right,	KEY_RIGHT,		KEY_RIGHT        },
	{ HidNpadButton_StickL,	KEY_R,			-1               },
	{ HidNpadButton_StickR,	KEY_M,			-1               },
};
#define NX_NUM_BUTTONS	((int)(sizeof(Nx_buttons) / sizeof(Nx_buttons[0])))

static int nx_in_mission()
{
	return (gameseq_get_state() == GS_STATE_GAME_PLAY);
}

static void nx_set_mouse_button(unsigned int fs_button, int down)
{
	if (down)
		Nx_mouse_buttons |= fs_button;
	else
		Nx_mouse_buttons &= ~fs_button;

	mouse_mark_button(fs_button, down ? 1 : 0);
}

void nx_input_init(void)
{
	if (Nx_input_inited)
		return;

	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&Nx_pad);
	hidInitializeTouchScreen();

	if (Nx_cfg_rumble) {
		hidInitializeVibrationDevices(Nx_vibe_hh, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
		hidInitializeVibrationDevices(Nx_vibe_no1, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
		Nx_vibe_inited = 1;
	}

	if (Nx_cfg_gyro) {
		hidGetSixAxisSensorHandles(&Nx_sixaxis_hh, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
		hidGetSixAxisSensorHandles(&Nx_sixaxis_no1, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
		hidStartSixAxisSensor(Nx_sixaxis_hh);
		hidStartSixAxisSensor(Nx_sixaxis_no1);
		Nx_gyro_started = 1;
	}

	Nx_input_inited = 1;
}

static void nx_apply_button(int key, int down)
{
	if (key < 0)
		return;

	if (key == KEY_NX_MOUSE_LEFT) {
		nx_set_mouse_button(MOUSE_LEFT_BUTTON, down);
		return;
	}

	key_mark((uint)key, down ? 1 : 0, 0);
}

static void nx_apply_rumble(float amp)
{
	if (amp > 1.0f) amp = 1.0f;
	if (amp < 0.0f) amp = 0.0f;

	HidVibrationValue v;
	v.amp_low  = amp;  v.freq_low  = 160.0f;
	v.amp_high = amp;  v.freq_high = 320.0f;

	HidVibrationValue values[2] = { v, v };
	const HidVibrationDeviceHandle *h = padIsHandheld(&Nx_pad) ? Nx_vibe_hh : Nx_vibe_no1;
	hidSendVibrationValues(h, values, 2);
}

void nx_input_poll(void)
{
	if (!Nx_input_inited)
		nx_input_init();

	padUpdate(&Nx_pad);

	// center the cursor once gr_screen is valid (gr_init runs after nx_input_init)
	if (!Nx_mouse_seeded && gr_screen.max_w > 0) {
		Nx_mouse_x = gr_screen.max_w / 2;
		Nx_mouse_y = gr_screen.max_h / 2;
		Nx_mouse_seeded = 1;
	}

	const u64 kDown = padGetButtonsDown(&Nx_pad);
	const u64 kUp   = padGetButtonsUp(&Nx_pad);
	const int in_mission = nx_in_mission();

	for (int i = 0; i < NX_NUM_BUTTONS; i++) {
		const int key = in_mission ? Nx_buttons[i].key_mission : Nx_buttons[i].key_menu;

		if (kDown & Nx_buttons[i].mask)
			nx_apply_button(key, 1);
		if (kUp & Nx_buttons[i].mask)
			nx_apply_button(key, 0);
	}

	// Minus opens the on-screen keyboard for the focused text field. LATCH the
	// request rather than pulsing it: os_poll() runs more than once per frame (the
	// main loop AND UI_WINDOW::process), so a per-poll pulse is cleared by the second
	// poll - Minus is then held, not newly pressed - before the input box reads it.
	// nx_keyboard_requested() consumes the latch.
	if (kDown & HidNpadButton_Minus)
		Nx_kbd_request = 1;

	// right stick -> menu cursor (in a mission it is read as throttle/rudder)
	if (!in_mission) {
		const HidAnalogStickState rs = padGetStickPos(&Nx_pad, 1);

		if (rs.x > NX_STICK_DEADZONE || rs.x < -NX_STICK_DEADZONE)
			Nx_mouse_x += (int)((rs.x / 32767.0f) * NX_STICK_CURSOR_SPEED);
		if (rs.y > NX_STICK_DEADZONE || rs.y < -NX_STICK_DEADZONE)
			Nx_mouse_y -= (int)((rs.y / 32767.0f) * NX_STICK_CURSOR_SPEED);
	}

	// gyro aiming -> yaw/pitch contribution (added to the stick in controlsconfig)
	if (Nx_cfg_gyro && Nx_gyro_started && in_mission) {
		HidSixAxisSensorState st = {0};
		HidSixAxisSensorHandle h = padIsHandheld(&Nx_pad) ? Nx_sixaxis_hh : Nx_sixaxis_no1;
		if (hidGetSixAxisSensorStates(h, &st, 1) > 0) {
			float sens = (Nx_cfg_gyro_sens / 100.0f) * NX_GYRO_SCALE;
			Nx_gyro_yaw   = st.angular_velocity.y * sens;
			Nx_gyro_pitch = st.angular_velocity.x * sens;
		}
	} else {
		Nx_gyro_yaw   = 0.0f;
		Nx_gyro_pitch = 0.0f;
	}

	// touch screen -> absolute cursor + left button.
	// The Switch touch panel always reports in a fixed 1280x720 space, independent of
	// the GL drawable/output resolution. Map through a pillarbox computed in that same
	// panel space (NOT the drawable-space view rect used for rendering), or the scale
	// is wrong (e.g. the right edge lands near the middle).
	HidTouchScreenState touch = {0};
	if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0 && gr_screen.max_h > 0) {
		const int panel_w = 1280, panel_h = 720;
		float aspect = (float)gr_screen.max_w / (float)gr_screen.max_h;
		int vw = panel_w, vh = (int)(panel_w / aspect + 0.5f);
		if (vh > panel_h) { vh = panel_h; vw = (int)(panel_h * aspect + 0.5f); }
		int vx = (panel_w - vw) / 2, vy = (panel_h - vh) / 2;

		Nx_mouse_x = (int)((s64)((int)touch.touches[0].x - vx) * gr_screen.max_w / vw);
		Nx_mouse_y = (int)((s64)((int)touch.touches[0].y - vy) * gr_screen.max_h / vh);

		if (!(Nx_mouse_buttons & MOUSE_LEFT_BUTTON))
			nx_set_mouse_button(MOUSE_LEFT_BUTTON, 1);
	} else {
		if ((Nx_mouse_buttons & MOUSE_LEFT_BUTTON) &&
		    (in_mission || !(padGetButtons(&Nx_pad) & (HidNpadButton_A | HidNpadButton_ZL | HidNpadButton_ZR))))
			nx_set_mouse_button(MOUSE_LEFT_BUTTON, 0);
	}

	if (Nx_mouse_x < 0) Nx_mouse_x = 0;
	if (Nx_mouse_y < 0) Nx_mouse_y = 0;
	if (Nx_mouse_x >= gr_screen.max_w) Nx_mouse_x = gr_screen.max_w - 1;
	if (Nx_mouse_y >= gr_screen.max_h) Nx_mouse_y = gr_screen.max_h - 1;

	if (Nx_cfg_rumble && Nx_vibe_inited) {
		float amp = Nx_rumble_continuous;
		if (timer_get_milliseconds() < Nx_rumble_pulse_end && Nx_rumble_pulse_amp > amp)
			amp = Nx_rumble_pulse_amp;
		nx_apply_rumble(amp);
	}
}

void nx_get_mouse(int *x, int *y)
{
	if (x) *x = Nx_mouse_x;
	if (y) *y = Nx_mouse_y;
}

unsigned int nx_get_mouse_buttons(void)
{
	return Nx_mouse_buttons;
}

void nx_warp_mouse(int x, int y)
{
	Nx_mouse_x = x;
	Nx_mouse_y = y;
}

// Pop up the system keyboard and return the entered text.
int nx_get_text_input(const char *guide_text, const char *initial_text, char *out, int out_size)
{
	SwkbdConfig kbd;
	Result rc;

	if (out == NULL || out_size <= 0)
		return 0;
	out[0] = '\0';

	rc = swkbdCreate(&kbd, 0);
	if (R_FAILED(rc))
		return 0;

	swkbdConfigMakePresetDefault(&kbd);
	if (guide_text != NULL)
		swkbdConfigSetGuideText(&kbd, guide_text);
	if (initial_text != NULL && initial_text[0] != '\0')
		swkbdConfigSetInitialText(&kbd, initial_text);
	swkbdConfigSetStringLenMax(&kbd, out_size - 1);

	rc = swkbdShow(&kbd, out, out_size);
	swkbdClose(&kbd);

	if (R_FAILED(rc)) {
		out[0] = '\0';
		return 0;
	}

	return (out[0] != '\0') ? 1 : 0;
}

int nx_keyboard_requested(void)
{
	int r = Nx_kbd_request;
	Nx_kbd_request = 0;		// consume: the request is handled exactly once
	return r;
}

// CPU boost for faster level loading (hooked around the loading screen only).
void nx_cpu_boost(int on)
{
	appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

void nx_rumble_pulse(float amp, int duration_ms)
{
	if (!Nx_cfg_rumble)
		return;

	int now = timer_get_milliseconds();
	if (now >= Nx_rumble_pulse_end || amp >= Nx_rumble_pulse_amp)
		Nx_rumble_pulse_amp = amp;
	Nx_rumble_pulse_end = now + duration_ms;
}

void nx_rumble_set_continuous(float amp)
{
	Nx_rumble_continuous = amp;
}

void nx_get_gyro_aim(float *yaw, float *pitch)
{
	if (yaw)   *yaw   = Nx_gyro_yaw;
	if (pitch) *pitch = Nx_gyro_pitch;
}

void os_poll()
{
	// clean exit on Home-menu close
	if (!appletMainLoop()) {
		gameseq_post_event(GS_EVENT_QUIT_GAME);
		return;
	}

	SDL_Event e;

	while (SDL_PollEvent(&e)) {
		switch (e.type) {
			case SDL_QUIT:
				gameseq_post_event(GS_EVENT_QUIT_GAME);
				break;

			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				switch (e.button.button) {
					case SDL_BUTTON_RIGHT:
						nx_set_mouse_button(MOUSE_RIGHT_BUTTON, e.button.state == SDL_PRESSED);
						break;
					case SDL_BUTTON_MIDDLE:
						nx_set_mouse_button(MOUSE_MIDDLE_BUTTON, e.button.state == SDL_PRESSED);
						break;
					case SDL_BUTTON_LEFT:
						nx_set_mouse_button(MOUSE_LEFT_BUTTON, e.button.state == SDL_PRESSED);
						break;
				}
				break;

			case SDL_MOUSEMOTION:
				Nx_mouse_x = e.motion.x;
				Nx_mouse_y = e.motion.y;
				break;

			case SDL_KEYDOWN:
				if (SDLtoFS2[e.key.keysym.scancode])
					key_mark(SDLtoFS2[e.key.keysym.scancode], 1, 0);
				break;

			case SDL_KEYUP:
				if (SDLtoFS2[e.key.keysym.scancode])
					key_mark(SDLtoFS2[e.key.keysym.scancode], 0, 0);
				break;

			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					fAppActive = 0;
					gr_activate(0);
				} else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
					fAppActive = 1;
					gr_activate(1);
				}
				break;

			default:
				break;
		}
	}

	nx_input_poll();

	extern void joy_read();
	joy_read();
}

void debug_int3()
{
	STUB_FUNCTION;
}

#endif // SWITCH
