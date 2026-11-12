#include <math.h>
#include <stdio.h>

#include "resolution.h"

Fast5Resolution g_resolution;

void fast5_resolution_setup(int design_w, int design_h, int frame_w, int frame_h,
                             int fill, int engine_native, int ui_native, int crop) {
    Fast5Resolution *r = &g_resolution;
    r->design_w = design_w > 0 ? design_w : 800;
    r->design_h = design_h > 0 ? design_h : 480;
    r->frame_w = frame_w > 0 ? frame_w : r->design_w;
    r->frame_h = frame_h > 0 ? frame_h : r->design_h;
    r->fill = fill;
    r->engine_native = engine_native;
    r->ui_native = ui_native;
    if (ui_native) {
        /*
         * The engine lays the 2D layer out for the framebuffer itself: the guest is told the
         * real screen size and the engine scales its logical space by the application scale
         * factor, so every 2D draw, the clip, the pointer and the 3D pick already arrive in
         * framebuffer pixels.  The host transform is the identity.
         *
         * This is the engine's own resolution-independence mechanism, but the game's 2D
         * *screens* (the Universal logo, the loading widgets) are authored for a fixed
         * 800x480 canvas and do not re-centre, so this mode is kept for comparison only.
         */
        r->design_w = r->frame_w;
        r->design_h = r->frame_h;
        r->sx = 1.0;
        r->sy = 1.0;
        r->ox = 0.0;
        r->oy = 0.0;
        r->px = 1.0;
        r->py = 1.0;
        r->fill = 1;
        fprintf(stderr,
                "resolution: engine-native 2D, frame %dx%d, no host transform\n",
                r->frame_w, r->frame_h);
        return;
    }
    double px = (double)r->frame_w / (double)r->design_w;
    double py = (double)r->frame_h / (double)r->design_h;
    if (fill) {
        /*
         * Stretch each axis so the 2D layer covers the whole framebuffer.  The game's 2D is
         * authored for a 5:3 canvas, so on a 16:9 frame this distorts it (6.7 % at 1080p).
         */
        r->sx = px;
        r->sy = py;
        r->ox = 0.0;
        r->oy = 0.0;
    } else if (crop) {
        /*
         * One uniform scale, big enough to cover the framebuffer, centred: the canvas
         * overflows on the axis that does not fit and the overflow is cut off.  The 2D layer
         * covers the whole window and nothing is distorted, but the canvas loses a strip on
         * one axis (3 % of the height at 1080p).
         */
        double s = px > py ? px : py;
        r->sx = s;
        r->sy = s;
        r->ox = ((double)r->frame_w - (double)r->design_w * s) * 0.5;
        r->oy = ((double)r->frame_h - (double)r->design_h * s) * 0.5;
    } else {
        /*
         * One uniform scale, so the 2D layer keeps the reference proportions exactly.  The
         * framebuffer is wider (or taller) than the scaled canvas, and the canvas is
         * centred; the 3D scene fills the rest, so nothing is letterboxed away.
         */
        double s = px < py ? px : py;
        r->sx = s;
        r->sy = s;
        r->ox = ((double)r->frame_w - (double)r->design_w * s) * 0.5;
        r->oy = ((double)r->frame_h - (double)r->design_h * s) * 0.5;
    }
    /* How the engine's own 2D projection maps the design space onto the framebuffer. */
    if (engine_native) {
        r->px = 1.0;
        r->py = 1.0;
    } else {
        r->px = px;
        r->py = py;
    }
    fprintf(stderr,
            "resolution: design %dx%d -> frame %dx%d (%s, engine %s) scale %.4f/%.4f offset %.1f/%.1f\n",
            r->design_w, r->design_h, r->frame_w, r->frame_h,
            fill ? "fill" : (crop ? "crop" : "letterbox"),
            engine_native ? "native" : "design",
            r->sx, r->sy, r->ox, r->oy);
}

void fast5_resolution_to_frame(double dx, double dy, double *fx, double *fy) {
    const Fast5Resolution *r = &g_resolution;
    if (fx) *fx = dx * r->sx + r->ox;
    if (fy) *fy = dy * r->sy + r->oy;
}

void fast5_resolution_to_design(double fx, double fy, double *dx, double *dy) {
    const Fast5Resolution *r = &g_resolution;
    if (dx) *dx = (fx - r->ox) / r->sx;
    if (dy) *dy = (fy - r->oy) / r->sy;
}

void fast5_resolution_fit_rect(int *x, int *y, int *w, int *h) {
    const Fast5Resolution *r = &g_resolution;
    if (!x || !y || !w || !h) return;
    if (r->sx == 1.0 && r->sy == 1.0 && r->ox == 0.0 && r->oy == 0.0 &&
        r->px == 1.0 && r->py == 1.0) {
        return;
    }
    double nx = (*x * r->sx + r->ox) / r->px;
    double ny = (*y * r->sy + r->oy) / r->py;
    double nw = (double)*w * r->sx / r->px;
    double nh = (double)*h * r->sy / r->py;
    *x = (int)floor(nx + 0.5);
    *y = (int)floor(ny + 0.5);
    *w = (int)floor(nw + 0.5);
    *h = (int)floor(nh + 0.5);
}
