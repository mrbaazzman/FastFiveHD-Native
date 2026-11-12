#ifndef FAST5_PLATFORM_H
#define FAST5_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

int platform_init(int width, int height, const char *title);
int platform_width(void);
int platform_height(void);
/* The resolution the guest engine runs at (the window may be a different size). */
void platform_set_design_size(int width, int height);
void platform_poll(void);
void platform_swap(void);
void platform_shutdown(void);
int platform_should_quit(void);
int platform_take_key(void);
int platform_take_game_key(int *code, int *down);
int platform_held_key(int code);
void platform_inject_key(int code, int down);
int platform_take_pointer(int *x, int *y, int *down);
void *platform_gl_proc(const char *name);

#ifdef __cplusplus
}
#endif

#endif
