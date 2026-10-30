#pragma once
#include "qp.h"
#include "qp_surface.h"
// The simulator draws the whole UI into an RGB565 framebuffer instead.
extern uint16_t sim_fb[];
#define qp_nv3007_make_spi_device(...) qp_make_rgb565_surface(LCD_WIDTH, LCD_HEIGHT, sim_fb)
