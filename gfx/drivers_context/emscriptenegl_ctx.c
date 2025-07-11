/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2012-2015 - Michael Lelli
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include "../../frontend/drivers/platform_emscripten.h"

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../retroarch.h"
#include "../../verbosity.h"

#ifdef HAVE_EGL
#include "../common/egl_common.h"
#endif

typedef struct
{
#ifdef HAVE_EGL
   egl_ctx_data_t egl;
#endif
   unsigned fb_width;
   unsigned fb_height;
} emscripten_ctx_data_t;

static void gfx_ctx_emscripten_swap_interval(void *data, int interval)
{
   platform_emscripten_set_main_loop_interval(interval);
}

static void gfx_ctx_emscripten_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height)
{
   int input_width;
   int input_height;
   emscripten_ctx_data_t *emscripten = (emscripten_ctx_data_t*)data;

   platform_emscripten_get_canvas_size(&input_width, &input_height);

   *resize = (emscripten->fb_width != input_width || emscripten->fb_height != input_height);
   *width  = emscripten->fb_width  = (unsigned)input_width;
   *height = emscripten->fb_height = (unsigned)input_height;
   *quit   = false;
}

static void gfx_ctx_emscripten_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   emscripten_ctx_data_t *emscripten = (emscripten_ctx_data_t*)data;

   if (!emscripten)
      return;

   *width  = emscripten->fb_width;
   *height = emscripten->fb_height;
}

static bool gfx_ctx_emscripten_get_metrics(void *data,
      enum display_metric_types type, float *value)
{
   switch (type)
   {
      // there is no way to get the actual DPI in emscripten, so return a standard value instead.
      // this is needed for menu touch/pointer swipe scrolling to work.
      case DISPLAY_METRIC_DPI:
         *value = 150.0f;
         break;

      default:
         *value = 0.0f;
         return false;
   }

   return true;
}

static void gfx_ctx_emscripten_destroy(void *data)
{
   emscripten_ctx_data_t *emscripten = (emscripten_ctx_data_t*)data;

   if (!emscripten)
      return;
#ifdef HAVE_EGL
   egl_destroy(&emscripten->egl);
#endif
   free(data);
}

static void *gfx_ctx_emscripten_init(void *video_driver)
{
#ifdef HAVE_EGL
   unsigned width, height;
   EGLint major, minor;
   EGLint n;
   static const EGLint attribute_list[] =
   {
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 16,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_NONE
   };
   static const EGLint context_attributes[] =
   {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
   };
#endif
   emscripten_ctx_data_t *emscripten = (emscripten_ctx_data_t*)
      calloc(1, sizeof(*emscripten));

   if (!emscripten)
      return NULL;

#ifdef HAVE_EGL
   if (g_egl_inited)
   {
      RARCH_LOG("[EMSCRIPTEN/EGL] Attempted to re-initialize driver.\n");
      return (void*)"emscripten";
   }

   if (!egl_init_context(&emscripten->egl, EGL_NONE,
      (void *)EGL_DEFAULT_DISPLAY, &major, &minor, &n, attribute_list, NULL))
   {
      egl_report_error();
      goto error;
   }

   if (!egl_create_context(&emscripten->egl, context_attributes))
   {
      egl_report_error();
      goto error;
   }

   if (!egl_create_surface(&emscripten->egl, 0))
      goto error;

   egl_get_video_size(&emscripten->egl, &width, &height);

   emscripten->fb_width  = width;
   emscripten->fb_height = height;
   RARCH_LOG("[EMSCRIPTEN/EGL] Dimensions: %ux%u.\n", width, height);
#endif

   return emscripten;
error:
   gfx_ctx_emscripten_destroy(video_driver);
   return NULL;
}

static bool gfx_ctx_emscripten_set_video_mode(void *data,
      unsigned width, unsigned height, bool fullscreen)
{
   platform_emscripten_set_fullscreen_state(fullscreen);
   if (!fullscreen)
      platform_emscripten_set_canvas_size(width, height);

   g_egl_inited = true;
   return true;
}

static enum gfx_ctx_api gfx_ctx_emscripten_get_api(void *data) { return GFX_CTX_OPENGL_ES_API; }

static bool gfx_ctx_emscripten_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
#ifdef HAVE_EGL
   if (api == GFX_CTX_OPENGL_ES_API)
      return egl_bind_api(EGL_OPENGL_ES_API);
#endif
   return false;
}

static void gfx_ctx_emscripten_input_driver(void *data,
      const char *name,
      input_driver_t **input, void **input_data)
{
   void *rwebinput = input_driver_init_wrap(&input_rwebinput, name);
   *input          = rwebinput ? &input_rwebinput : NULL;
   *input_data     = rwebinput;
}

static bool gfx_ctx_emscripten_has_focus(void *data) {
   return g_egl_inited && !platform_emscripten_is_window_hidden();
}

static bool gfx_ctx_emscripten_suppress_screensaver(void *data, bool enable)
{
   platform_emscripten_set_wake_lock(enable);
   return true;
}

static void gfx_ctx_emscripten_show_mouse(void *data, bool state)
{
   platform_emscripten_set_pointer_visibility(state);
}

static float gfx_ctx_emscripten_translate_aspect(void *data,
      unsigned width, unsigned height) { return (float)width / height; }

static bool gfx_ctx_emscripten_init_egl_image_buffer(void *data,
      const video_info_t *video) { return false; }

static bool gfx_ctx_emscripten_write_egl_image(void *data,
      const void *frame, unsigned width, unsigned height, unsigned pitch,
      bool rgb32, unsigned index, void **image_handle) { return false; }

static void gfx_ctx_emscripten_bind_hw_render(void *data, bool enable)
{
#ifdef HAVE_EGL
   emscripten_ctx_data_t *emscripten = (emscripten_ctx_data_t*)data;
   egl_bind_hw_render(&emscripten->egl, enable);
#endif
}

static uint32_t gfx_ctx_emscripten_get_flags(void *data)
{
   uint32_t flags = 0;
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_GLSL);
   return flags;
}

static void gfx_ctx_emscripten_set_flags(void *data, uint32_t flags) { }

const gfx_ctx_driver_t gfx_ctx_emscripten = {
   gfx_ctx_emscripten_init,
   gfx_ctx_emscripten_destroy,
   gfx_ctx_emscripten_get_api,
   gfx_ctx_emscripten_bind_api,
   gfx_ctx_emscripten_swap_interval,
   gfx_ctx_emscripten_set_video_mode,
   gfx_ctx_emscripten_get_video_size,
   NULL, /* get_refresh_rate */
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   gfx_ctx_emscripten_get_metrics,
   gfx_ctx_emscripten_translate_aspect,
   NULL, /* update_title */
   gfx_ctx_emscripten_check_window,
   NULL, /* set_resize: no-op */
   gfx_ctx_emscripten_has_focus,
   gfx_ctx_emscripten_suppress_screensaver,
   true, /* has_windowed */
   NULL, /* swap_buffers: no-op */
   gfx_ctx_emscripten_input_driver,
#ifdef HAVE_EGL
   egl_get_proc_address,
#else
   NULL,
#endif
   gfx_ctx_emscripten_init_egl_image_buffer,
   gfx_ctx_emscripten_write_egl_image,
   gfx_ctx_emscripten_show_mouse,
   "egl_emscripten",
   gfx_ctx_emscripten_get_flags,
   gfx_ctx_emscripten_set_flags,
   gfx_ctx_emscripten_bind_hw_render,
   NULL, /* get_context_data */
   NULL  /* make_current */
};
