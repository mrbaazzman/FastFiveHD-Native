/*
 * Presentation transform between the guest's 2D design space and the host framebuffer.
 *
 * The shipped game lays its 2D layer out in a fixed pixel space (800x480 on the reference
 * device) and anchors parts of it to the screen size. The port therefore keeps two spaces:
 *
 *   design space  --(sx, sy, ox, oy)-->  framebuffer
 *
 * `design` is the space the 2D content is authored in; `frame` is the window.
 *
 * Two engine modes are supported:
 *
 *  - native (the default): the engine runs at the window size, so its render targets, the
 *    camera and the scene are native. The screen-size globals it reads for 2D layout are
 *    converted back into the design space, so the 2D content stays in one space and the
 *    host can present it at any window size. The engine's 2D projection then maps the
 *    window onto the framebuffer one to one, and a design-space rect only needs scaling.
 *
 *  - design: the engine runs at the design resolution (the old behaviour). Its projection
 *    maps the design space onto the whole framebuffer, so a design-space rect has to be
 *    pre-scaled to land where the host wants it.
 *
 * The 2D layer has two ways of reaching the framebuffer:
 *
 *  - host-scaled (`ui_native == 0`): the guest lays its 2D out in the fixed 800x480 design
 *    space and the host scales every draw, the pointer and the 3D pick to the window.
 *  - engine-native (`ui_native == 1`): the guest is told the real screen size, so the engine
 *    lays the 2D layer out for it itself (it scales the logical space by the application
 *    scale factor) and everything already arrives in framebuffer pixels.  The host's
 *    transform is then the identity.
 *
 * `fill` mode scales each axis independently so the 2D layer covers the whole framebuffer
 * (the game is stretched by the aspect difference). `letterbox` mode uses one uniform scale
 * and centres the result, so nothing is distorted.
 */
#ifndef FAST5_RESOLUTION_H
#define FAST5_RESOLUTION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int design_w, design_h;   /* the space the 2D content is authored in */
    int frame_w, frame_h;     /* the framebuffer / window size */
    int fill;                 /* 1 = fill (per-axis), 0 = letterbox (uniform) */
    int engine_native;        /* 1 = the engine itself runs at the framebuffer size */
    int ui_native;            /* 1 = the engine lays the 2D layer out for the framebuffer */
    double sx, sy;            /* framebuffer pixels per design pixel */
    double ox, oy;            /* framebuffer offset of the design origin */
    double px, py;            /* how the engine's own projection maps the design space */
} Fast5Resolution;

extern Fast5Resolution g_resolution;

/*
 * Configure the transform.
 *   fill != 0        stretch each axis, letterbox == 0 scales uniformly and centres
 *   engine_native != 0  the engine runs at the framebuffer size
 */
void fast5_resolution_setup(int design_w, int design_h, int frame_w, int frame_h,
                             int fill, int engine_native, int ui_native);

/* design pixels -> framebuffer pixels */
void fast5_resolution_to_frame(double dx, double dy, double *fx, double *fy);
/* framebuffer pixels -> design pixels */
void fast5_resolution_to_design(double fx, double fy, double *dx, double *dy);

/*
 * A 2D draw asks for a rect in the design space; this converts it into the rect that
 * produces the wanted framebuffer position.
 */
void fast5_resolution_fit_rect(int *x, int *y, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* FAST5_RESOLUTION_H */
