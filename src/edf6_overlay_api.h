#pragma once

#include <stdint.h>

#define EDF6_OVERLAY_API_VERSION 1
#define EDF6_OVERLAY_HOST_DLL "EDF6Compendium.dll"
#define EDF6_OVERLAY_REGISTER "Edf6Overlay_Register"

/* Colors are 0xRRGGBBAA. Sizes and positions are screen pixels. */
struct Edf6OverlayHost {
    int version;
    void (*log)(const char *message);
    float (*scale)(void);
    void (*screenSize)(float *width, float *height);
    void (*fillRect)(float x0, float y0, float x1, float y1, uint32_t rgba);
    void (*strokeRect)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);
    void (*text)(float x, float y, float size, uint32_t rgba, const char *utf8);
    void (*textSize)(float size, const char *utf8, float *width, float *height);
};

/* The module struct must outlive the process. onToggle runs on the host's input thread;
   wantsDraw and draw run on the render thread. host->log works from any thread at any time;
   every other host function is only valid inside draw. */
struct Edf6OverlayModule {
    int version;
    const char *name;
    int toggleKey;
    void (*onToggle)(void);
    int (*wantsDraw)(void);
    void (*draw)(const struct Edf6OverlayHost *host);
};

typedef int (*Edf6OverlayRegisterFn)(const struct Edf6OverlayModule *module, const struct Edf6OverlayHost **host);
