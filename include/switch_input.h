/*
 * switch_input.h - Nintendo Switch input bridge (controller / touch / motion ->
 * keyboard / mouse / joystick), HD rumble and text entry. See src/osapi/os_nx.cpp.
 */

#ifndef _SWITCH_INPUT_H
#define _SWITCH_INPUT_H

#ifdef SWITCH

#ifdef __cplusplus
extern "C" {
#endif

void nx_input_init(void);
void nx_input_poll(void);				// called once per frame from os_poll()

// virtual mouse cursor (used by src/io/mouse.cpp)
void nx_get_mouse(int *x, int *y);
unsigned int nx_get_mouse_buttons(void);
void nx_warp_mouse(int x, int y);

// system software keyboard; returns 1 if text was accepted
int nx_get_text_input(const char *guide_text, const char *initial_text, char *out, int out_size);
int nx_keyboard_requested(void);		// non-zero the frame Minus is pressed

// HD rumble (driven by the joy_ff_* hooks in joy-sdl.cpp)
void nx_rumble_pulse(float amp, int duration_ms);
void nx_rumble_set_continuous(float amp);

// gyro aim contribution for the heading/pitch axes (added in controlsconfig.cpp)
void nx_get_gyro_aim(float *yaw, float *pitch);

// CPU boost, hooked around the loading screen
void nx_cpu_boost(int on);

#ifdef __cplusplus
}
#endif

#endif // SWITCH

#endif // _SWITCH_INPUT_H
