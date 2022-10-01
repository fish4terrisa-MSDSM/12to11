/* Wayland compositor running on top of an X server.

Copyright (C) 2022 to various contributors.

This file is part of 12to11.

12to11 is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

12to11 is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with 12to11.  If not, see <https://www.gnu.org/licenses/>.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <drm_fourcc.h>

#include "compositor.h"
#include "shaders.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "linux-dmabuf-unstable-v1.h"

/* These are flags for the DrmFormats.  */

enum
  {
    NeedExternalTarget = 1,
  };

typedef enum _EglBufferType EglBufferType;

typedef struct _EglTarget EglTarget;
typedef struct _EglBuffer EglBuffer;

typedef struct _EglDmaBufBuffer EglDmaBufBuffer;
typedef struct _EglShmBuffer EglShmBuffer;
typedef struct _FormatInfo FormatInfo;

typedef struct _CompositeProgram CompositeProgram;

enum _EglBufferType
  {
    DmaBufBuffer,
    ShmBuffer,
  };

struct _EglDmaBufBuffer
{
  /* The type of this buffer.  Always DmaBufBuffer.  */
  EglBufferType type;

  /* The EGL image associated with this buffer.  */
  EGLImageKHR *image;

  /* DRM format used to create this buffer.  */
  DrmFormat *format;
};

struct _EglShmBuffer
{
  /* The type of this buffer.  Always ShmBuffer.  */
  EglBufferType type;

  /* The pointer to pool data.  */
  void **data;

  /* The offset and stride of the buffer.  */
  int32_t offset, stride;

  /* The format info of this buffer.  */
  FormatInfo *format;
};

struct _FormatInfo
{
  /* The corresponding Wayland format.  */
  uint32_t wl_format;

  /* The corresponding DRM format.  */
  uint32_t drm_format;

  /* The GL internalFormat.  If 0, then it is actually gl_format.  */
  GLint gl_internalformat;

  /* The GL format and type.  */
  GLint gl_format, gl_type;

  /* Bits per pixel.  */
  short bpp;

  /* Whether or not an alpha channel is present.  */
  Bool has_alpha : 1;
};

enum
  {
    IsTextureGenerated = 1,
    HasAlpha	       = (1 << 2),
    CanRelease	       = (1 << 3),
    InvertY	       = (1 << 4),
  };

struct _EglBuffer
{
  /* Some flags.  */
  int flags;

  /* The texture name of any generated texture.  */
  GLuint texture;

  /* 3x3 matrix that is used to transform texcoord into actual texture
     coordinates.  Note that GLfloat[9] should be the same type as
     Matrix.  */
  GLfloat matrix[9];

  /* The width and height of the buffer.  */
  int width, height;

  /* Various different buffers.  */
  union {
    /* The type of the buffer.  */
    EglBufferType type;

    /* A dma-buf buffer.  */
    EglDmaBufBuffer dmabuf;

    /* A shared memory buffer.  */
    EglShmBuffer shm;
  } u;
};

/* This macro computes the size of an EglBuffer for the given type.
   It is used to only allocate as much memory as really required.  */

#define EglBufferSize(type)			\
  (offsetof (EglBuffer, u) + sizeof (type))

enum
  {
    SwapPreservesContents = 1,
    IsPixmap 		  = 2,
  };

struct _EglTarget
{
  /* The drawable backing this surface.  */
  Drawable source;

  /* The EGL surface.  */
  EGLSurface surface;

  /* The width and height of the backing drawable.  */
  unsigned short width, height;

  /* Various flags.  */
  int flags;
};

struct _CompositeProgram
{
  /* The name of the program.  */
  GLint program;

  /* The index of the texcoord attribute.  */
  GLuint texcoord;

  /* The index of the position attribute.  */
  GLuint position;

  /* The index of the texture uniform.  */
  GLuint texture;

  /* The index of the source uniform.  */
  GLuint source;

  /* The index of the invert_y uniform.  */
  GLuint invert_y;
};

/* This macro makes column major order easier to reason about for C
   folks.  */
#define Index(matrix, row, column) ((matrix)[(column) * 3 + (row)])

/* All known SHM formats.  */
static FormatInfo known_shm_formats[] =
  {
    {
      .wl_format = WL_SHM_FORMAT_ARGB8888,
      .drm_format = WL_SHM_FORMAT_ARGB8888,
      .gl_format = GL_BGRA_EXT,
      .gl_type = GL_UNSIGNED_BYTE,
      .has_alpha = True,
      .bpp = 32,
    },
    {
      .wl_format = WL_SHM_FORMAT_XRGB8888,
      .drm_format = WL_SHM_FORMAT_XRGB8888,
      .gl_format = GL_BGRA_EXT,
      .gl_type = GL_UNSIGNED_BYTE,
      .has_alpha = False,
      .bpp = 32,
    },
    {
      .wl_format = WL_SHM_FORMAT_XBGR8888,
      .drm_format = DRM_FORMAT_XBGR8888,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_BYTE,
      .has_alpha = False,
      .bpp = 32,
    },
    {
      .wl_format = WL_SHM_FORMAT_ABGR8888,
      .drm_format = DRM_FORMAT_ABGR8888,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_BYTE,
      .has_alpha = True,
      .bpp = 32,
    },
    {
      .wl_format = WL_SHM_FORMAT_BGR888,
      .drm_format = DRM_FORMAT_BGR888,
      .gl_format = GL_RGB,
      .gl_type = GL_UNSIGNED_BYTE,
      .has_alpha = False,
      .bpp = 24,
    },
    {
      .wl_format = WL_SHM_FORMAT_RGBX4444,
      .drm_format = DRM_FORMAT_RGBX4444,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_SHORT_4_4_4_4,
      .has_alpha = False,
      .bpp = 16,
    },
    {
      .wl_format = WL_SHM_FORMAT_RGBA4444,
      .drm_format = DRM_FORMAT_RGBA4444,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_SHORT_4_4_4_4,
      .has_alpha = True,
      .bpp = 16,
    },
    {
      .wl_format = WL_SHM_FORMAT_RGBX5551,
      .drm_format = DRM_FORMAT_RGBX5551,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_SHORT_5_5_5_1,
      .has_alpha = False,
      .bpp = 16,
    },
    {
      .wl_format = WL_SHM_FORMAT_RGBA5551,
      .drm_format = DRM_FORMAT_RGBA5551,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_SHORT_5_5_5_1,
      .has_alpha = True,
      .bpp = 16,
    },
    {
      .wl_format = WL_SHM_FORMAT_RGB565,
      .drm_format = DRM_FORMAT_RGB565,
      .gl_format = GL_RGB,
      .gl_type = GL_UNSIGNED_SHORT_5_6_5,
      .has_alpha = False,
      .bpp = 16,
    },
    {
      .wl_format = WL_SHM_FORMAT_XBGR2101010,
      .drm_format = DRM_FORMAT_XBGR2101010,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_INT_2_10_10_10_REV_EXT,
      .has_alpha = False,
      .bpp = 32,
    },
    {
      .wl_format = WL_SHM_FORMAT_ABGR2101010,
      .drm_format = DRM_FORMAT_ABGR2101010,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_INT_2_10_10_10_REV_EXT,
      .has_alpha = True,
      .bpp = 32,
    },
    {
      .wl_format = WL_SHM_FORMAT_XBGR16161616,
      .drm_format = DRM_FORMAT_XBGR16161616,
      .gl_internalformat = GL_RGBA16_EXT,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_SHORT,
      .has_alpha = False,
      .bpp = 64,
    },
    {
      .wl_format = WL_SHM_FORMAT_ABGR16161616,
      .drm_format = DRM_FORMAT_ABGR16161616,
      .gl_internalformat = GL_RGBA16_EXT,
      .gl_format = GL_RGBA,
      .gl_type = GL_UNSIGNED_SHORT,
      .has_alpha = True,
      .bpp = 64,
    },
  };

/* GL procedures needed.  */
static PFNEGLGETPLATFORMDISPLAYPROC IGetPlatformDisplay;
static PFNEGLCREATEPLATFORMWINDOWSURFACEPROC ICreatePlatformWindowSurface;
static PFNEGLCREATEPLATFORMPIXMAPSURFACEPROC ICreatePlatformPixmapSurface;
static PFNEGLCREATEIMAGEKHRPROC ICreateImage;
static PFNEGLDESTROYIMAGEKHRPROC IDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC IEGLImageTargetTexture2D;

/* GL procedures that are optional.  */
static PFNEGLQUERYDISPLAYATTRIBEXTPROC IQueryDisplayAttrib;
static PFNEGLQUERYDEVICESTRINGEXTPROC IQueryDeviceString;
static PFNEGLQUERYDMABUFFORMATSEXTPROC IQueryDmaBufFormats;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC IQueryDmaBufModifiers;
static PFNEGLCREATESYNCKHRPROC ICreateSync;
static PFNEGLDESTROYSYNCKHRPROC IDestroySync;
static PFNEGLCLIENTWAITSYNCKHRPROC IClientWaitSync;
static PFNEGLGETSYNCATTRIBKHRPROC IGetSyncAttrib;
static PFNEGLWAITSYNCKHRPROC IWaitSync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC IDupNativeFenceFD;
static PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC ISwapBuffersWithDamage;

/* The EGL display handle.  */
static EGLDisplay egl_display;

/* The EGL context handle.  */
static EGLContext egl_context;

/* The chosen framebuffer configuration.  */
static EGLConfig egl_config;

/* The current render target.  */
static EglTarget *current_target;

/* The major and minor versions of EGL.  */
static EGLint egl_major, egl_minor;

/* The DRM device node.  */
static dev_t drm_device;

/* Whether or not the device node is available.  */
static Bool drm_device_available;

/* List of DRM formats provided.  */
static DrmFormat *drm_formats;

/* Number of DRM formats provided.  */
static int n_drm_formats;

/* List of SHM formats provided.  */
static ShmFormat *shm_formats;

/* Number of SHM formats provided.  */
static int n_shm_formats;

/* Global shader programs.  */
static GLint clear_rect_program;

/* Index of position attrib.  */
static GLuint clear_rect_program_pos_attrib;

/* The picture format used for cursors.  */
static XRenderPictFormat *cursor_format;

/* Composition program for ARGB textures.  */
static CompositeProgram argb_program;

/* Composition program for XRGB textures.  */
static CompositeProgram xrgb_program;

/* Composition program for external textures.  */
static CompositeProgram external_program;

/* Whether or not buffer age is supported.  */
static Bool have_egl_ext_buffer_age;

/* EGL and GLES 2-based renderer.  */

#define CheckExtension(name)				\
  if (!name)						\
    {							\
      if (display)					\
	eglTerminate (display);				\
      fprintf (stderr, "Missing: egl%s\n", #name + 1);	\
      return False;					\
    }

#define CheckExtensionGl(name)				\
  if (!name)						\
    {							\
      /* If the context remains current, then nothing	\
	 will get released upon eglTerminate.  */	\
      eglMakeCurrent (display, EGL_NO_SURFACE,		\
		      EGL_NO_SURFACE, EGL_NO_CONTEXT);	\
      eglTerminate (display);				\
      fprintf (stderr, "Missing: gl%s\n", #name + 1);	\
      return False;					\
    }

#define LoadProc(name, ext, extname)			\
  if (HaveEglExtension (extname))			\
    I##name						\
      = (void *) eglGetProcAddress ("egl" #name ext)

#define LoadProcGl(name, ext, extname)			\
  if (HaveGlExtension (extname))			\
    I##name						\
      = (void *) eglGetProcAddress ("gl" #name ext)

#define CheckGlExtension(name)			\
  if (!HaveGlExtension (name))			\
    {						\
      fprintf (stderr, "Missing %s\n", name);	\
						\
      eglMakeCurrent (display, EGL_NO_SURFACE,	\
		      EGL_NO_SURFACE,		\
		      EGL_NO_CONTEXT);		\
      eglTerminate (display);			\
      return False;				\
    }

static Bool
HaveEglExtension1 (const char *extensions, const char *extension)
{
  const char *end;
  size_t extlen, n;

  extlen = strlen (extension);
  end = extensions + strlen (extensions);

  while (extensions < end)
    {
      n = 0;

      /* Skip spaces, if any.  */
      if (*extensions == ' ')
	{
	  extensions++;
	  continue;
	}

      n = strcspn (extensions, " ");

      /* Compare strings.  */
      if (n == extlen && !strncmp (extension, extensions, n))
	return True;

      extensions += n;
    }

  /* Not found.  */
  return False;
}

static Bool
HaveEglExtension (const char *extension)
{
  const char *extensions;

  if (egl_display)
    extensions = eglQueryString (egl_display, EGL_EXTENSIONS);
  else
    extensions = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);

  if (!extensions)
    return False;

  return HaveEglExtension1 (extensions, extension);
}

static Bool
HaveGlExtension (const char *extension)
{
  const GLubyte *extensions;

  extensions = glGetString (GL_EXTENSIONS);

  if (!extensions)
    return False;

  return HaveEglExtension1 ((const char *) extensions, extension);
}

static void
EglInitFuncsEarly (void)
{
  LoadProc (GetPlatformDisplay, "", "EGL_EXT_platform_base");
  LoadProc (CreatePlatformWindowSurface, "", "EGL_EXT_platform_base");
  LoadProc (CreatePlatformPixmapSurface, "", "EGL_EXT_platform_base");

  /* These extensions are not really required.  */
  LoadProc (QueryDisplayAttrib, "EXT", "EGL_EXT_device_query");
  LoadProc (QueryDeviceString, "EXT", "EGL_EXT_device_query");
}

static void
EglInitFuncs (void)
{
  /* Initialize extensions.  */
  LoadProc (CreateImage, "KHR", "EGL_KHR_image_base");
  LoadProc (DestroyImage, "KHR", "EGL_KHR_image_base");

  /* Initialize extensions that are not really required.  */
  LoadProc (QueryDmaBufFormats, "EXT",
	    "EGL_EXT_image_dma_buf_import_modifiers");
  LoadProc (QueryDmaBufModifiers, "EXT",
	    "EGL_EXT_image_dma_buf_import_modifiers");

  LoadProc (CreateSync, "KHR", "EGL_KHR_fence_sync");
  LoadProc (DestroySync, "KHR", "EGL_KHR_fence_sync");
  LoadProc (ClientWaitSync, "KHR", "EGL_KHR_fence_sync");
  LoadProc (GetSyncAttrib, "KHR", "EGL_KHR_fence_sync");
  LoadProc (WaitSync, "KHR", "EGL_KHR_wait_sync");
  LoadProc (DupNativeFenceFD, "ANDROID", "EGL_ANDROID_native_fence_sync");
  LoadProc (SwapBuffersWithDamage, "EXT",
	    "EGL_EXT_swap_buffers_with_damage");
}

static void
EglInitGlFuncs (void)
{
  LoadProcGl (EGLImageTargetTexture2D, "OES", "GL_OES_EGL_image");

  /* We treat eglWaitSyncKHR specially, since it only works if the
     server client API also supports GL_OES_EGL_sync.  */
  if (!HaveGlExtension ("GL_OES_EGL_sync"))
    IWaitSync = NULL;
}

static Visual *
PickBetterVisual (Visual *visual, int *depth)
{
  XRenderPictFormat target_format, *format, *found;
  int i, num_x_formats, bpp, n_visuals, j;
  EGLint alpha_size;
  XPixmapFormatValues *formats;
  XVisualInfo empty_template, *visuals;

  /* First, see if there is already an alpha channel.  */
  format = XRenderFindVisualFormat (compositor.display, visual);

  if (!format)
    return visual;

  if (format->type != PictTypeDirect)
    /* Can this actually happen? */
    return visual;

  if (format->direct.alphaMask)
    return visual;

  /* Next, build the target format from the visual format.  */
  target_format.type = PictTypeDirect;
  target_format.direct = format->direct;

  /* Obtain the size of the alpha mask in the EGL config.  */
  if (!eglGetConfigAttrib (egl_display, egl_config,
			   EGL_ALPHA_SIZE, &alpha_size))
    return visual;

  if (alpha_size > 16)
    /* If the alpha mask is too big, then use the chosen visual.  */
    return visual;

  /* Add the alpha mask.  */
  for (i = 0; i < alpha_size; ++i)
    target_format.direct.alphaMask |= 1 << i;

  /* Look for matching picture formats with the same bpp and a larger
     depth.  */
  formats = XListPixmapFormats (compositor.display, &num_x_formats);

  if (!formats)
    return visual;

  /* Obtain the number of bits per pixel for the given depth.  */
  bpp = 0;

  for (i = 0; i < num_x_formats; ++i)
    {
      if (formats[i].depth == format->depth)
	bpp = formats[i].bits_per_pixel;
    }

  if (!bpp)
    {
      XFree (formats);
      return visual;
    }

  /* Get a list of all visuals.  */
  empty_template.screen = DefaultScreen (compositor.display);
  visuals = XGetVisualInfo (compositor.display, VisualScreenMask,
			    &empty_template, &n_visuals);

  if (!visuals)
    {
      XFree (formats);
      return visual;
    }

  /* Now, loop through each depth.  */

  for (i = 0; i < num_x_formats; ++i)
    {
      if (formats[i].depth > format->depth
	  && formats[i].bits_per_pixel == bpp)
	{
	  /* Try to find a matching picture format.  */
	  target_format.depth = formats[i].depth;

	  found = XRenderFindFormat (compositor.display,
				     PictFormatType
				     | PictFormatDepth
				     | PictFormatRed
				     | PictFormatGreen
				     | PictFormatBlue
				     | PictFormatRedMask
				     | PictFormatBlueMask
				     | PictFormatGreenMask
				     | PictFormatAlphaMask,
				     &target_format, 0);

	  if (found)
	    {
	      /* Now try to find the corresponding visual.  */

	      for (j = 0; j < n_visuals; ++j)
		{
		  if (visuals[j].depth != formats[i].depth)
		    continue;

		  if (XRenderFindVisualFormat (compositor.display,
					       visuals[j].visual) == found)
		    {
		      /* We got a usable visual with an alpha channel
			 otherwise matching the characteristics of the
			 visual specified by EGL.  Return.  */
		      *depth = formats[i].depth;
		      visual = visuals[j].visual;

		      XFree (visuals);
		      XFree (formats);
		      return visual;
		    }
		}
	    }
	}
    }

  /* Otherwise, nothing was found.  Return the original visual
     untouched, but free visuals.  */
  XFree (visuals);
  XFree (formats);
  return visual;
}

static Visual *
FindVisual (VisualID visual, int *depth)
{
  XVisualInfo vinfo, *visuals;
  Visual *value;
  int nvisuals;
  const char *override;

  /* Normally, we do not want to manually specify this.  However, EGL
     happens to be buggy, and cannot find visuals with an alpha
     mask.  */
  override = getenv ("RENDER_VISUAL");

  if (!override)
    vinfo.visualid = visual;
  else
    vinfo.visualid = atoi (override);

  vinfo.screen = DefaultScreen (compositor.display);

  visuals = XGetVisualInfo (compositor.display,
			    VisualScreenMask | VisualIDMask,
			    &vinfo, &nvisuals);

  if (!visuals)
    return NULL;

  if (!nvisuals)
    {
      XLFree (visuals);
      return NULL;
    }

  /* Now, get the visual and depth, free the visual info, and return
     them.  */

  value = visuals->visual;

  /* EGL does not know how to find visuals with an alpha channel, even
     if we specify one in the framebuffer configuration.  Detect when
     that is the case, and pick a better visual.  */
  value = PickBetterVisual (value, &visuals->depth);

  *depth = visuals->depth;

  XLFree (visuals);

  return value;
}

static Bool
EglPickConfig (void)
{
  EGLint egl_config_attribs[20];
  EGLint n_configs;
  EGLint visual_id;

  /* We want the best framebuffer configuration that supports at least
     8 bits of alpha, red, green, and blue.  */
  egl_config_attribs[0] = EGL_BUFFER_SIZE;
  egl_config_attribs[1] = 32;
  egl_config_attribs[2] = EGL_RED_SIZE;
  egl_config_attribs[3] = 8;
  egl_config_attribs[4] = EGL_GREEN_SIZE;
  egl_config_attribs[5] = 8;
  egl_config_attribs[6] = EGL_BLUE_SIZE;
  egl_config_attribs[7] = 8;
  egl_config_attribs[8] = EGL_ALPHA_SIZE;
  egl_config_attribs[9] = 8;

  /* We want OpenGL ES 2 or later.  */
  egl_config_attribs[10] = EGL_RENDERABLE_TYPE;
  egl_config_attribs[11] = EGL_OPENGL_ES2_BIT;

  /* We don't care about the depth or stencil.  */
  egl_config_attribs[12] = EGL_DEPTH_SIZE;
  egl_config_attribs[13] = EGL_DONT_CARE;
  egl_config_attribs[14] = EGL_STENCIL_SIZE;
  egl_config_attribs[15] = EGL_DONT_CARE;

  /* We need support for both windows and pixmap-backed surfaces.  */
  egl_config_attribs[16] = EGL_SURFACE_TYPE;
  egl_config_attribs[17] = EGL_WINDOW_BIT | EGL_PIXMAP_BIT;

  /* Terminate the config list.  */
  egl_config_attribs[18] = EGL_NONE;

  /* Now, search for the best matching configuration.  */
  if (!eglChooseConfig (egl_display, egl_config_attribs,
			&egl_config, 1, &n_configs))
    /* No config could be found.  */
    return False;

  if (!n_configs)
    return False;

  /* See if the config has an attached visual ID.  */
  if (!eglGetConfigAttrib (egl_display, egl_config,
			   EGL_NATIVE_VISUAL_ID,
			   &visual_id))
    return False;

  /* Now, find the visual corresponding to the visual ID.  */
  compositor.visual = FindVisual (visual_id, &compositor.n_planes);

  if (!compositor.visual)
    /* The visual couldn't be found.  */
    return False;

  /* Try to find the cursor picture format.  */
  cursor_format = XRenderFindVisualFormat (compositor.display,
					   compositor.visual);

  /* If no cursor format was found, return False.  */
  if (!cursor_format)
    return False;

  /* Otherwise, all of this was set up successfully.  */
  return True;
}

static Bool
EglCreateContext (void)
{
  EGLint attrs[3];

  /* Require GLES 2.0.  eglBindAPI is not called, so the API used
     should be OpenGL ES.  */

  attrs[0] = EGL_CONTEXT_MAJOR_VERSION;
  attrs[1] = 2;
  attrs[2] = EGL_NONE;

  /* Create the context; if no context is returned, fail.  */
  egl_context = eglCreateContext (egl_display, egl_config,
				  EGL_NO_CONTEXT, attrs);

  return egl_context != EGL_NO_CONTEXT;
}

static void
CheckShaderCompilation (GLuint shader, const char *name)
{
  char msg[1024];
  GLint success;

  glGetShaderiv (shader, GL_COMPILE_STATUS, &success);

  if (success)
    return;

  glGetShaderInfoLog (shader, sizeof msg, NULL, msg);

  /* Report shader compilation error.  */
  fprintf (stderr, "Failed to compile shader %s: %s\n", name, msg);
  abort ();
}

static void
CheckProgramLink (GLuint program, const char *name)
{
  char msg[1024];
  GLint success;

  glGetProgramiv (program, GL_LINK_STATUS, &success);

  if (success)
    return;

  glGetProgramInfoLog (program, sizeof msg, NULL, msg);

  /* Report shader compilation error.  */
  fprintf (stderr, "Failed to link program %s: %s\n", name, msg);
  abort ();
}

static void
EglCompileCompositeProgram (CompositeProgram *program,
			    const char *fragment_shader)
{
  GLuint vertex, fragment;

  /* There are different composite programs for different
     kinds of textures, differing in their fragment shaders.  */

  vertex = glCreateShader (GL_VERTEX_SHADER);
  fragment = glCreateShader (GL_FRAGMENT_SHADER);

  glShaderSource (vertex, 1, &composite_rectangle_vertex_shader,
		  NULL);
  glCompileShader (vertex);
  CheckShaderCompilation (vertex, "compositor vertex shader");

  glShaderSource (fragment, 1, &fragment_shader, NULL);
  glCompileShader (fragment);
  CheckShaderCompilation (fragment, "compositor fragment shader");

  program->program = glCreateProgram ();
  glAttachShader (program->program, vertex);
  glAttachShader (program->program, fragment);
  glLinkProgram (program->program);
  CheckProgramLink (program->program, "compositor program");

  /* Obtain the indices of the texcoord and pos attributes.  */
  program->texcoord = glGetAttribLocation (program->program,
					   "texcoord");
  program->position = glGetAttribLocation (program->program,
					   "pos");
  program->texture = glGetUniformLocation (program->program,
					  "texture");
  program->source = glGetUniformLocation (program->program,
					  "source");
  program->invert_y = glGetUniformLocation (program->program,
					    "invert_y");

  /* Now delete the shaders.  */
  glDeleteShader (vertex);
  glDeleteShader (fragment);
}

static void
EglCompileShaders (void)
{
  GLuint vertex, fragment;

  vertex = glCreateShader (GL_VERTEX_SHADER);
  fragment = glCreateShader (GL_FRAGMENT_SHADER);

  glShaderSource (vertex, 1, &clear_rectangle_vertex_shader, NULL);
  glCompileShader (vertex);
  CheckShaderCompilation (vertex, "clear_rectangle_vertex_shader");

  glShaderSource (fragment, 1, &clear_rectangle_fragment_shader, NULL);
  glCompileShader (fragment);
  CheckShaderCompilation (fragment, "clear_rectangle_fragment_shader");

  clear_rect_program = glCreateProgram ();
  glAttachShader (clear_rect_program, vertex);
  glAttachShader (clear_rect_program, fragment);
  glLinkProgram (clear_rect_program);
  CheckProgramLink (clear_rect_program, "clear_rect_program");

  /* Obtain the location of an attribute.  */
  clear_rect_program_pos_attrib
    = glGetAttribLocation (clear_rect_program, "pos");

  /* Now delete the shaders.  */
  glDeleteShader (vertex);
  glDeleteShader (fragment);

  /* Compile some other programs used for compositing textures.  */
  EglCompileCompositeProgram (&argb_program,
			      composite_rectangle_fragment_shader_rgba);
  EglCompileCompositeProgram (&xrgb_program,
			      composite_rectangle_fragment_shader_rgbx);
  EglCompileCompositeProgram (&external_program,
			      composite_rectangle_fragment_shader_external);
}

/* Forward declaration.  */
static void AddRenderFlag (int);

static Bool
EglInitDisplay (void)
{
  EGLDisplay *display;
  EGLint major, minor;

  /* Initialize eglGetPlatformDisplay.  */

  display = NULL;
  EglInitFuncsEarly ();

  CheckExtension (IGetPlatformDisplay);
  CheckExtension (ICreatePlatformWindowSurface);
  CheckExtension (ICreatePlatformPixmapSurface);

  /* Then, get the display.  */
  display = IGetPlatformDisplay (EGL_PLATFORM_X11_KHR, compositor.display,
				 NULL);

  /* Return if the display could not be created.  */
  if (!display)
    return False;

  /* Next, try to initialize EGL.  */
  if (!eglInitialize (display, &major, &minor))
    return False;

  /* If "EGL_EXT_image_dma_buf_import" is not supported, fail display
     initialization.  */

  egl_display = display;

  if (!HaveEglExtension ("EGL_EXT_image_dma_buf_import"))
    {
      eglTerminate (display);
      return False;
    }

  /* Check if EGL_EXT_buffer_age is supported.  */
  have_egl_ext_buffer_age = HaveEglExtension ("EGL_EXT_buffer_age");

  /* Initialize functions.  */

  EglInitFuncs ();

  CheckExtension (ICreateImage);
  CheckExtension (IDestroyImage);

  /* If both EGL fences and EGL_ANDROID_native_fence_sync are
     supported, enable explicit sync.  */
  if (ICreateSync && IDupNativeFenceFD)
    AddRenderFlag (SupportsExplicitSync);

  /* Otherwise, the display has been initialized.  */
  egl_major = major;
  egl_minor = minor;

  /* Now, try to pick a framebuffer configuration.  */
  if (!EglPickConfig ())
    {
      /* Initializing the framebuffer configuration failed.  */
      eglTerminate (display);
      return False;
    }

  if (!EglCreateContext ())
    {
      /* A GL context could not be created.  */
      eglTerminate (display);
      return False;
    }

  /* Make the display current and initialize GL functions.  */
  eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		  egl_context);

  /* This is required for i.e. YUV dma buffer formats.  */
  CheckGlExtension ("GL_OES_EGL_image_external");

  /* This for little endian RGB.  */
  CheckGlExtension ("GL_EXT_read_format_bgra");

  /* This for unpacking subimages.  */
  CheckGlExtension ("GL_EXT_unpack_subimage");

  EglInitGlFuncs ();
  CheckExtensionGl (IEGLImageTargetTexture2D);

  /* Now, try to compile the shaders.  */
  EglCompileShaders ();

  return True;
}

static Bool
InitRenderFuncs (void)
{
  return EglInitDisplay ();
}

static Bool
TryPreserveOnSwap (EGLSurface *surface)
{
  EGLint value;

  /* Enable preserving the color buffer post eglSwapBuffers.  */
  eglSurfaceAttrib (egl_display, surface,
		    EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
  value = 0;

  /* Check that enabling the attribute was successful.  */
  if (!eglQuerySurface (egl_display, surface,
			EGL_SWAP_BEHAVIOR, &value)
      || value != EGL_BUFFER_PRESERVED)
    return False;

  return True;
}

static RenderTarget
TargetFromWindow (Window window)
{
  EglTarget *target;

  target = XLMalloc (sizeof *target);
  target->source = window;
  target->flags = 0;
  target->surface = ICreatePlatformWindowSurface (egl_display, egl_config,
						  &window, NULL);

  /* Try enabling EGL_BUFFER_PRESERVED to preserve the color buffer
     post swap.  */
  if (TryPreserveOnSwap (target->surface))
    target->flags |= SwapPreservesContents;

  if (target->surface == EGL_NO_SURFACE)
    abort ();

  return (RenderTarget) (void *) target;
}

static RenderTarget
TargetFromPixmap (Pixmap pixmap)
{
  EglTarget *target;

  target = XLMalloc (sizeof *target);
  target->source = pixmap;
  target->flags = 0;
  target->surface = ICreatePlatformPixmapSurface (egl_display, egl_config,
						  &pixmap, NULL);

  /* Mark the target as being a pixmap surface.  EGL pixmap surfaces
     are always single-buffered, so we have to call glFinish
     manually.  */
  target->flags |= IsPixmap;

  /* Try enabling EGL_BUFFER_PRESERVED to preserve the color buffer
     post swap.  */
  if (TryPreserveOnSwap (target->surface))
    target->flags |= SwapPreservesContents;

  if (target->surface == EGL_NO_SURFACE)
    abort ();

  return (RenderTarget) (void *) target;
}

static void
NoteTargetSize (RenderTarget target, int width, int height)
{
  EglTarget *egl_target;

  egl_target = target.pointer;

  /* This really ought to fit in unsigned short... */
  egl_target->width = width;
  egl_target->height = height;
}

static Picture
PictureFromTarget (RenderTarget target)
{
  EglTarget *egl_target;
  XRenderPictureAttributes picture_attrs;

  /* This is just to pacify GCC; picture_attrs is not used as mask is
     0.  */
  memset (&picture_attrs, 0, sizeof picture_attrs);
  egl_target = target.pointer;

  return XRenderCreatePicture (compositor.display,
			       egl_target->source,
			       cursor_format, 0,
			       &picture_attrs);
}

static void
FreePictureFromTarget (Picture picture)
{
  XRenderFreePicture (compositor.display, picture);
}

static void
DestroyRenderTarget (RenderTarget target)
{
  EglTarget *egl_target;

  egl_target = target.pointer;

  /* Destroy the EGL surface.  */
  eglDestroySurface (egl_display, egl_target->surface);

  /* If the target is current, clear the current target.  */
  if (egl_target == current_target)
    {
      current_target = NULL;

      /* Make the context current with no surface.  */
      eglMakeCurrent (egl_display, EGL_NO_SURFACE,
		      EGL_NO_SURFACE, egl_context);
    }

  /* Free the target object.  */
  XLFree (egl_target);
}

static void
MakeRenderTargetCurrent (RenderTarget target)
{
  EglTarget *egl_target;

  if (target.pointer == current_target)
    /* The target is already current.  */
    return;

  egl_target = target.pointer;

  /* Otherwise, make it current for the context.  */
  if (!eglMakeCurrent (egl_display, egl_target->surface,
		       egl_target->surface, egl_context))
    abort ();

  current_target = egl_target;

  /* Set the swap interval to 0 - we use _NET_WM_SYNC_REQUEST for
     synchronization.  */
  eglSwapInterval (egl_display, 0);

  /* Specify clear color for the color buffer.  We want the color
     buffer to be completely transparent when clear.  */
  glClearColor (0, 0, 0, 0);
}

static void
StartRender (RenderTarget target)
{
  EglTarget *egl_target;

  MakeRenderTargetCurrent (target);

  egl_target = target.pointer;

  /* Set the viewport.  */
  glViewport (0, 0, egl_target->width,
	      egl_target->height);

  /* Also set the blend function to one that makes sense.  */
  glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void
FillBoxesWithTransparency (RenderTarget target, pixman_box32_t *boxes,
			   int nboxes, int min_x, int min_y)
{
  GLfloat *verts;
  EglTarget *egl_target;
  int i;
  GLfloat x1, x2, y1, y2;

  egl_target = target.pointer;

  glDisable (GL_BLEND);
  glUseProgram (clear_rect_program);

  /* Allocate enough to hold each triangle.  */
  verts = alloca (sizeof *verts * nboxes * 8);

  for (i = 0; i < nboxes; ++i)
    {
      /* Translate the coordinates by the min_x and min_y.  */
      x1 = boxes[i].x1 - min_x;
      x2 = boxes[i].x2 - min_x;
      y1 = boxes[i].y1 - min_y;
      y2 = boxes[i].y2 - min_y;

      /* Bottom left.  */
      verts[i * 8 + 0] = -1.0f + x1 / egl_target->width * 2;
      verts[i * 8 + 1] = -1.0f + (egl_target->height - y2) / egl_target->height * 2;

      /* Top left.  */
      verts[i * 8 + 2] = -1.0f + x1 / egl_target->width * 2;
      verts[i * 8 + 3] = -1.0f + (egl_target->height - y1) / egl_target->height * 2;

      /* Bottom right.  */
      verts[i * 8 + 4] = -1.0f + x2 / egl_target->width * 2;
      verts[i * 8 + 5] = -1.0f + (egl_target->height - y2) / egl_target->height * 2;

      /* Top right.  */
      verts[i * 8 + 6] = -1.0f + x2 / egl_target->width * 2;
      verts[i * 8 + 7] = -1.0f + (egl_target->height - y1) / egl_target->height * 2;
    }

  /* Upload the verts.  */
  glVertexAttribPointer (clear_rect_program_pos_attrib,
			 2, GL_FLOAT, GL_FALSE, 0, verts);
  glEnableVertexAttribArray (clear_rect_program_pos_attrib);

  for (i = 0; i < nboxes; ++i)
    /* Draw each rectangle.  */
    glDrawArrays (GL_TRIANGLE_STRIP, i * 4, 4);

  glDisableVertexAttribArray (clear_rect_program_pos_attrib);
}

static void
ClearRectangle (RenderTarget target, int x, int y, int width, int height)
{
  pixman_box32_t box;

  box.x1 = x;
  box.x2 = x + width;
  box.y1 = y;
  box.y2 = y + height;

  FillBoxesWithTransparency (target, &box, 1, 0, 0);
}

static CompositeProgram *
FindProgram (EglBuffer *buffer)
{
  switch (buffer->u.type)
    {
    case DmaBufBuffer:
      if (buffer->u.dmabuf.format->flags & NeedExternalTarget)
	/* Use the external format compositor program.  */
	return &external_program;

      Fallthrough;
    case ShmBuffer:
    default:
      /* Otherwise, return the ARGB or XRGB program depending on
	 whether or not an alpha channel is present.  */
      return (buffer->flags & HasAlpha
	      ? &argb_program : &xrgb_program);
    }
}

static GLenum
GetTextureTarget (EglBuffer *buffer)
{
  switch (buffer->u.type)
    {
    case DmaBufBuffer:
      return (buffer->u.dmabuf.format->flags & NeedExternalTarget
	      ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D);

    case ShmBuffer:
      return GL_TEXTURE_2D;
    }

  /* This is not supposed to happen.  */
  abort ();
}

static void
ComputeTransformMatrix (EglBuffer *buffer, DrawParams *params)
{
  /* Update the transformation matrix of BUFFER.  This is a 3x3
     transformation matrix that maps from texcoords to actual
     coordinates in the buffer.  */

  /* Copy over the identity transform.  */
  MatrixIdentity (&buffer->matrix);

  /* Set the X and Y scales.  */
  if (params->flags & ScaleSet)
    {
      Index (buffer->matrix, 0, 0)
	= (float) (1.0 / params->scale);
      Index (buffer->matrix, 1, 1)
	= (float) (1.0 / params->scale);
    }

  /* Set the offsets.  */
  if (params->flags & OffsetSet)
    MatrixTranslate (&buffer->matrix,
		     (float) (params->off_x / buffer->width),
		     (float) (params->off_y / buffer->height));

  /* Set the stretch.  */
  if (params->flags & StretchSet)
    /* Scale the buffer down by this much.  */
    MatrixScale (&buffer->matrix,
		 (float) (params->crop_width / params->stretch_width),
		 (float) (params->crop_height / params->stretch_height));
}

static void
Composite (RenderBuffer buffer, RenderTarget target,
	   Operation op, int src_x, int src_y, int x, int y,
	   int width, int height, DrawParams *params)
{
  GLfloat verts[8], texcoord[8];
  GLfloat x1, x2, y1, y2;
  EglTarget *egl_target;
  EglBuffer *egl_buffer;
  CompositeProgram *program;
  GLenum tex_target;

  egl_target = target.pointer;
  egl_buffer = buffer.pointer;

  /* Assert that a texture was generated, since UpdateBuffer should be
     called before the buffer is ever used.  */
  XLAssert (egl_buffer->flags & IsTextureGenerated);

  /* Find the program to use for compositing.  */
  program = FindProgram (egl_buffer);

  /* Get the texturing target.  */
  tex_target = GetTextureTarget (egl_buffer);

  /* Compute the transformation matrix to use to draw the given
     buffer.  */
  ComputeTransformMatrix (egl_buffer, params);

  /* dest rectangle on target.  */
  x1 = x;
  y1 = y;
  x2 = x + width;
  y2 = y + height;

  /* Bottom left.  */
  verts[0] = -1.0f + x1 / egl_target->width * 2;
  verts[1] = -1.0f + (egl_target->height - y2) / egl_target->height * 2;

  /* Top left.  */
  verts[2] = -1.0f + x1 / egl_target->width * 2;
  verts[3] = -1.0f + (egl_target->height - y1) / egl_target->height * 2;

  /* Bottom right.  */
  verts[4] = -1.0f + x2 / egl_target->width * 2;
  verts[5] = -1.0f + (egl_target->height - y2) / egl_target->height * 2;

  /* Top right.  */
  verts[6] = -1.0f + x2 / egl_target->width * 2;
  verts[7] = -1.0f + (egl_target->height - y1) / egl_target->height * 2;

  /* source rectangle on buffer.  */
  x1 = src_x;
  y1 = src_y;
  x2 = src_x + width;
  y2 = src_y + height;

  texcoord[0] = x1 / egl_buffer->width;
  texcoord[1] = y2 / egl_buffer->height;
  texcoord[2] = x1 / egl_buffer->width;
  texcoord[3] = y1 / egl_buffer->height;
  texcoord[4] = x2 / egl_buffer->width;
  texcoord[5] = y2 / egl_buffer->height;
  texcoord[6] = x2 / egl_buffer->width;
  texcoord[7] = y1 / egl_buffer->height;

  /* Disable blending based on whether or not an alpha channel is
     present.  */
  if (op == OperationOver
      && egl_buffer->flags & HasAlpha)
    glEnable (GL_BLEND);
  else
    glDisable (GL_BLEND);

  glActiveTexture (GL_TEXTURE0);
  glBindTexture (tex_target, egl_buffer->texture);
  glTexParameteri (tex_target, GL_TEXTURE_MIN_FILTER,
		   GL_NEAREST);
  glTexParameteri (tex_target, GL_TEXTURE_MAG_FILTER,
		   GL_NEAREST);
  glUseProgram (program->program);

  glUniform1i (program->texture, 0);
  glUniformMatrix3fv (program->source, 1, GL_FALSE,
		      egl_buffer->matrix);
  glUniform1i (program->invert_y, egl_buffer->flags & InvertY);
  glVertexAttribPointer (program->position, 2, GL_FLOAT,
			 GL_FALSE, 0, verts);
  glVertexAttribPointer (program->texcoord, 2, GL_FLOAT,
			 GL_FALSE, 0, texcoord);

  glEnableVertexAttribArray (program->position);
  glEnableVertexAttribArray (program->texcoord);

  glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

  glDisableVertexAttribArray (program->position);
  glDisableVertexAttribArray (program->texcoord);

  glBindTexture (tex_target, 0);
}

static void
FinishRender (RenderTarget target, pixman_region32_t *damage)
{
  EglTarget *egl_target;
  EGLint *rects;
  int nboxes, i;
  pixman_box32_t *boxes;

  egl_target = target.pointer;

  if (egl_target->flags & IsPixmap)
    glFinish ();
  else if (!ISwapBuffersWithDamage || !damage)
    /* This should also do glFinish.  */
    eglSwapBuffers (egl_display, egl_target->surface);
  else
    {
      /* Do a swap taking the buffer damage into account.  First,
	 convert the damage into cartesian coordinates.  */
      boxes = pixman_region32_rectangles (damage, &nboxes);
      rects = alloca (nboxes * 4 * sizeof *damage);

      for (i = 0; i < nboxes; ++i)
	{
	  rects[i * 4 + 0] = boxes[i].x1;
	  rects[i * 4 + 1] = egl_target->height - boxes[i].y2;
	  rects[i * 4 + 2] = boxes[i].x2 - boxes[i].x1;
	  rects[i * 4 + 3] = boxes[i].y2 - boxes[i].y1;
	}

      /* Next, swap buffers with the damage.  */
      ISwapBuffersWithDamage (egl_display, egl_target->surface,
			      rects, nboxes);
    }
}

static int
TargetAge (RenderTarget target)
{
  EglTarget *egl_target;
  EGLint age;

  egl_target = target.pointer;

  /* If egl_target->flags & SwapPreservesContents, return 0.  */
  if (egl_target->flags & SwapPreservesContents)
    return 0;

  /* Otherwise, return <age of buffer> - 1.  */
  if (have_egl_ext_buffer_age
      && eglQuerySurface (egl_display, egl_target->surface,
			  EGL_BUFFER_AGE_EXT, &age))
    return age - 1;

  /* Fall back to -1 if obtaining the buffer age failed.  */
  return -1;
}

static RenderFence
ImportFdFence (int fd, Bool *error)
{
  EGLSyncKHR *fence;
  EGLint attribs[3];

  attribs[0] = EGL_SYNC_NATIVE_FENCE_FD_ANDROID;
  attribs[1] = fd;
  attribs[2] = EGL_NONE;

  /* This fence is supposed to assume ownership over the given file
     descriptor.  */
  fence = ICreateSync (egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID,
		       attribs);

  if (fence == EGL_NO_SYNC_KHR)
    {
      *error = True;
      return (RenderFence) NULL;
    }

  return (RenderFence) (void *) fence;
}

static void
WaitFence (RenderFence fence)
{
  /* N.B. that here egl_context must be current, which should always
     be true.  */

  if (IWaitSync)
    /* This is more asynchronous, as it doesn't wait for the fence
       on the CPU.  */
    IWaitSync (egl_display, fence.pointer, 0);
  else
    /* But eglWaitSyncKHR isn't available everywhere.  */
    IClientWaitSync (egl_display, fence.pointer, 0,
		     EGL_FOREVER_KHR);

  /* If either of these requests fail, simply proceed to read from the
     protected data.  */
}

static void
DeleteFence (RenderFence fence)
{
  if (!IDestroySync (egl_display, fence.pointer))
    /* There is no way to continue without leaking memory, and this
       shouldn't happen.  */
    abort ();
}

static void
HandleFenceReadable (int fd, void *data, ReadFd *readfd)
{
  XLRemoveReadFd (readfd);

  /* Now destroy the native fence.  */
  if (!IDestroySync (egl_display, data))
    abort ();

  /* And close the file descriptor.  */
  close (fd);
}

static int
GetFinishFence (Bool *error)
{
  EGLint attribs;
  EGLSyncKHR *fence;
  EGLint fd;

  attribs = EGL_NONE;

  /* Create the fence.  EGL_SYNC_CONDITION_KHR should default to
     EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR, meaning it will signal once
     all prior drawing commands complete.  */
  fence = ICreateSync (egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID,
		       &attribs);

  if (fence == EGL_NO_SYNC_KHR)
    {
      *error = True;
      return -1;
    }

  /* Obtain the file descriptor.  */
  fd = IDupNativeFenceFD (egl_display, fence);

  if (fd == -1)
    *error = True;
  else
    /* Delete the fence after it is signalled.  Duplicate the fd, as
       it will be closed by the caller.  */
    XLAddReadFd (dup (fd), fence, HandleFenceReadable);

  return fd;
}

static RenderFuncs egl_render_funcs =
  {
    .init_render_funcs = InitRenderFuncs,
    .target_from_window = TargetFromWindow,
    .target_from_pixmap = TargetFromPixmap,
    .note_target_size = NoteTargetSize,
    .picture_from_target = PictureFromTarget,
    .free_picture_from_target = FreePictureFromTarget,
    .destroy_render_target = DestroyRenderTarget,
    .start_render = StartRender,
    .fill_boxes_with_transparency = FillBoxesWithTransparency,
    .clear_rectangle = ClearRectangle,
    .composite = Composite,
    .finish_render = FinishRender,
    .target_age = TargetAge,
    .import_fd_fence = ImportFdFence,
    .wait_fence = WaitFence,
    .delete_fence = DeleteFence,
    .get_finish_fence = GetFinishFence,
    .flags = ImmediateRelease,
  };

static void
AddRenderFlag (int flags)
{
  egl_render_funcs.flags |= flags;
}

static DrmFormat *
GetDrmFormats (int *num_formats)
{
  *num_formats = n_drm_formats;
  return drm_formats;
}

static dev_t
GetRenderDevice (Bool *error)
{
  *error = !drm_device_available;
  return drm_device;
}

static ShmFormat *
GetShmFormats (int *num_formats)
{
  *num_formats = n_shm_formats;
  return shm_formats;
}

static DrmFormat *
FindDrmFormat (uint32_t format, uint64_t modifier)
{
  int i;

  /* Find the DRM format associated with FORMAT and MODIFIER.  This is
     mainly used to extract flags.  */

  for (i = 0; i < n_drm_formats; ++i)
    {
      if (drm_formats[i].drm_format == format
	  && drm_formats[i].drm_modifier == modifier)
	return &drm_formats[i];
    }

  return NULL;
}

static void
CloseFileDescriptors (DmaBufAttributes *attributes)
{
  int i;

  for (i = 0; i < attributes->n_planes; ++i)
    close (attributes->fds[i]);
}

static FormatInfo *
FindFormatInfoDrm (uint32_t drm_format)
{
  int i;

  for (i = 0; i < ArrayElements (known_shm_formats); ++i)
    {
      if (known_shm_formats[i].drm_format == drm_format)
	return &known_shm_formats[i];
    }

  return NULL;
}

static RenderBuffer
BufferFromDmaBuf (DmaBufAttributes *attributes, Bool *error)
{
  EglBuffer *buffer;
  DrmFormat *format;
  EGLint attribs[50], i;
  FormatInfo *info;

  buffer = XLMalloc (EglBufferSize (EglDmaBufBuffer));
  buffer->flags = 0;
  buffer->texture = EGL_NO_TEXTURE;
  buffer->width = attributes->width;
  buffer->height = attributes->height;
  buffer->u.type = DmaBufBuffer;

  /* Copy over the identity transform.  */
  MatrixIdentity (&buffer->matrix);

  i = 0;

  /* Find the DRM format in question so we can determine the right
     target to use.  */
  format = FindDrmFormat (attributes->drm_format,
			  attributes->modifier);
  XLAssert (format != NULL);

  /* Find the corresponding FormatInfo record, to determine whether or
     not an alpha channel is present.  If it cannot be found, no
     problems there - just assume there is an alpha channel.  */
  info = FindFormatInfoDrm (attributes->drm_format);

  if (!info || info->has_alpha)
    buffer->flags |= HasAlpha;

  /* If modifiers were specified and are not supported, fail.  */

  if (!IQueryDmaBufModifiers
      && attributes->modifier != DRM_FORMAT_MOD_INVALID)
    goto error;

  /* Set invert_y based on flags.  */
  if (attributes->flags
      & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT)
    buffer->flags |= InvertY;

  /* Otherwise, import the buffer now.  */
  attribs[i++] = EGL_WIDTH;
  attribs[i++] = attributes->width;
  attribs[i++] = EGL_HEIGHT;
  attribs[i++] = attributes->height;
  attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[i++] = attributes->drm_format;

  /* Note that the file descriptors have to be closed in any case.  */

#define LoadPlane(no)							\
  {									\
    attribs[i++] = EGL_DMA_BUF_PLANE ## no ## _FD_EXT;			\
    attribs[i++] = attributes->fds[no];					\
    attribs[i++] = EGL_DMA_BUF_PLANE ## no ## _OFFSET_EXT;		\
    attribs[i++] = attributes->offsets[no];				\
    attribs[i++] = EGL_DMA_BUF_PLANE ## no ## _PITCH_EXT;	       	\
    attribs[i++] = attributes->strides[no];				\
									\
    /* Set modifiers if the invalid modifier is not being used.  */	\
									\
    if (IQueryDmaBufModifiers						\
	&& attributes->modifier != DRM_FORMAT_MOD_INVALID)		\
      {									\
	attribs[i++] = EGL_DMA_BUF_PLANE ## no ## _MODIFIER_LO_EXT;	\
	attribs[i++] = attributes->modifier & 0xffffffff;		\
	attribs[i++] = EGL_DMA_BUF_PLANE ## no ## _MODIFIER_HI_EXT;	\
	attribs[i++] = attributes->modifier >> 32;			\
      }									\
  }

  /* Load plane 0.  There is always at least one plane.  */
  LoadPlane (0);

  /* Next, load each plane specified.  */

  if (attributes->n_planes > 1)
    LoadPlane (1);

  if (attributes->n_planes > 2)
    LoadPlane (2);

  if (attributes->n_planes > 3)
    LoadPlane (3);

#undef LoadPlane

  /* Make sure the pixel data is preserved.  */
  attribs[i++] = EGL_IMAGE_PRESERVED_KHR;
  attribs[i++] = EGL_TRUE;

  /* Terminate the attribute list.  */
  attribs[i++] = EGL_NONE;

  /* Create the image.  */
  buffer->u.dmabuf.image = ICreateImage (egl_display, EGL_NO_CONTEXT,
					 EGL_LINUX_DMA_BUF_EXT, NULL,
					 attribs);

  /* Check that no error occured.  */
  if (buffer->u.dmabuf.image == EGL_NO_IMAGE)
    goto error;

  /* Set the DRM format used to create this image.  */
  buffer->u.dmabuf.format = format;

  /* Close file descriptors and return the image.  */
  CloseFileDescriptors (attributes);

  return (RenderBuffer) (void *) buffer;

 error:
  CloseFileDescriptors (attributes);
  XLFree (buffer);
  *error = True;

  return (RenderBuffer) NULL;
}

static void
BufferFromDmaBufAsync (DmaBufAttributes *attributes,
		       DmaBufSuccessFunc success_callback,
		       DmaBufFailureFunc failure_callback,
		       void *callback_data)
{
  Bool error;
  RenderBuffer buffer;

  error = False;
  buffer = BufferFromDmaBuf (attributes, &error);

  if (error)
    failure_callback (callback_data);
  else
    success_callback (buffer, callback_data);
}

static FormatInfo *
FindFormatInfo (uint32_t wl_format)
{
  int i;

  for (i = 0; i < ArrayElements (known_shm_formats); ++i)
    {
      if (known_shm_formats[i].wl_format == wl_format)
	return &known_shm_formats[i];
    }

  return NULL;
}

static RenderBuffer
BufferFromShm (SharedMemoryAttributes *attributes, Bool *error)
{
  EglBuffer *buffer;

  buffer = XLMalloc (EglBufferSize (EglShmBuffer));
  buffer->flags = 0;
  buffer->texture = EGL_NO_TEXTURE;
  buffer->width = attributes->width;
  buffer->height = attributes->height;
  buffer->u.type = ShmBuffer;

  /* Copy over the identity transform.  */
  MatrixIdentity (&buffer->matrix);

  /* Record the buffer data.  */

  buffer->u.shm.format = FindFormatInfo (attributes->format);
  XLAssert (buffer->u.shm.format != NULL);

  buffer->u.shm.offset = attributes->offset;
  buffer->u.shm.stride = attributes->stride;
  buffer->u.shm.data = attributes->data;

  /* Record whether or not the format supports an alpha channel.  */

  if (buffer->u.shm.format->has_alpha)
    buffer->flags |= HasAlpha;

  /* Return the buffer.  */

  return (RenderBuffer) (void *) buffer;
}

static void
FreeShmBuffer (RenderBuffer buffer)
{
  EglBuffer *egl_buffer;

  egl_buffer = buffer.pointer;

  /* If a texture is attached, delete it.  */
  if (egl_buffer->flags & IsTextureGenerated)
    glDeleteTextures (1, &egl_buffer->texture);

  XLFree (buffer.pointer);
}

static void
FreeDmabufBuffer (RenderBuffer buffer)
{
  EglBuffer *egl_buffer;

  egl_buffer = buffer.pointer;

  /* If a texture is attached, delete it.  */
  if (egl_buffer->flags & IsTextureGenerated)
    glDeleteTextures (1, &egl_buffer->texture);

  /* Free the EGL image.  */
  IDestroyImage (egl_display, egl_buffer->u.dmabuf.image);

  XLFree (buffer.pointer);
}

/* Initialization functions.  */

static void
AddDrmFormat (uint32_t format, uint64_t modifier,
	      int flags)
{
  /* First, make drm_formats big enough.  */
  drm_formats
    = XLRealloc (drm_formats,
		 ++n_drm_formats * sizeof *drm_formats);

  /* Then, write the format to the end.  */
  drm_formats[n_drm_formats - 1].drm_format = format;
  drm_formats[n_drm_formats - 1].drm_modifier = modifier;
  drm_formats[n_drm_formats - 1].flags = flags;
}

static void
InitModifiersFor (uint32_t format)
{
  EGLuint64KHR *modifiers;
  EGLint i, n_total_modifiers;
  EGLBoolean *external_only;
  int flags;

  /* First, look up how many modifiers are supported for the given
     format.  */
  IQueryDmaBufModifiers (egl_display, format, 0, NULL,
			 NULL, &n_total_modifiers);

  /* Next, allocate a buffer that can hold that many modifiers.  */
  modifiers = alloca (n_total_modifiers * sizeof *modifiers);
  external_only = alloca (n_total_modifiers * sizeof *modifiers);

  /* And query the modifiers for real.  */
  IQueryDmaBufModifiers (egl_display, format, n_total_modifiers,
			 modifiers, external_only, &n_total_modifiers);

  /* Add each modifier.  */
  for (i = 0; i < n_total_modifiers; ++i)
    {
      /* If the modifier requires GL_TEXTURE_EXTERNAL_OES, specify
	 that.  */
      flags = external_only[i] ? NeedExternalTarget : 0;

      /* And add the DRM format.  */
      AddDrmFormat (format, modifiers[i], flags);
    }
}

static void
InitDmaBufFormats (void)
{
  static DrmFormat fallback_formats[2];
  EGLint i, n_total_formats, *formats;

  if (!IQueryDmaBufModifiers)
    {
      /* If the import_modifiers extension isn't supported, there's no
	 way to query for supported formats.  Return a few formats
	 that should probably be supported everywhere.  */

      fallback_formats[0].drm_format = DRM_FORMAT_ARGB8888;
      fallback_formats[0].drm_modifier = DRM_FORMAT_MOD_INVALID;
      fallback_formats[1].drm_format = DRM_FORMAT_XRGB8888;
      fallback_formats[1].drm_modifier = DRM_FORMAT_MOD_INVALID;

      drm_formats = fallback_formats;
      n_drm_formats = 2;
      return;
    }

  /* Otherwise, look up what is supported.  First, check how many
     formats are supported.  */
  IQueryDmaBufFormats (egl_display, 0, NULL, &n_total_formats);

  /* Next, allocate a buffer that can hold that many formats.  */
  formats = alloca (n_total_formats * sizeof *formats);

  /* And query the formats for real.  */
  IQueryDmaBufFormats (egl_display, n_total_formats, formats,
		       &n_total_formats);

  for (i = 0; i < n_total_formats; ++i)
    {
      /* Now, add the implicit modifier.  */
      AddDrmFormat (formats[i], DRM_FORMAT_MOD_INVALID, 0);

      /* Next, query for and add each supported modifier.  */
      InitModifiersFor (formats[i]);
    }
}

static void
AddShmFormat (uint32_t format)
{
  shm_formats
    = XLRealloc (shm_formats,
		 sizeof *shm_formats * ++n_shm_formats);

  shm_formats[n_shm_formats - 1].format = format;
}

static void
InitShmFormats (void)
{
  int i;

  /* Add formats always supported by GL.  */
  for (i = 0; i < ArrayElements (known_shm_formats); ++i)
    AddShmFormat (known_shm_formats[i].wl_format);
}

static void
InitBufferFuncs (void)
{
  EGLAttrib attrib;
  EGLDeviceEXT *device;
  const char *extensions, *name;
  struct stat dev_stat;

  /* Try to obtain the device name of a DRM node used to create the
     EGL display.  First, we try to look for render nodes, but settle
     for master nodes if render nodes are not available.  */

  if (IQueryDisplayAttrib && IQueryDeviceString)
    {
      if (!IQueryDisplayAttrib (egl_display, EGL_DEVICE_EXT, &attrib))
	goto failed;

      /* Get the EGLDevice object.  */
      device = (EGLDeviceEXT) attrib;

      /* Get extensions supported by the device.  */
      extensions = IQueryDeviceString (device, EGL_EXTENSIONS);

      if (!extensions)
	goto failed;

      /* Now, get the path to the render device.  */
      name = NULL;

      if (HaveEglExtension1 (extensions, "EGL_EXT_device_drm_render_node"))
	name = IQueryDeviceString (device, EGL_DRM_RENDER_NODE_FILE_EXT);

      if (!name && HaveEglExtension1 (extensions, "EGL_EXT_device_drm"))
	name = IQueryDeviceString (device, EGL_DRM_DEVICE_FILE_EXT);

      if (!name)
	goto failed;

      if (stat (name, &dev_stat) != 0)
	goto failed;

      if (!dev_stat.st_rdev)
	goto failed;

      drm_device = dev_stat.st_rdev;
      drm_device_available = True;
    }
  else
  failed:
    fprintf (stderr, "Warning: failed to obtain device node of"
	     " EGL display.  Hardware acceleration will probably not"
	     " be available.\n");

  /* Now, initialize the dmabuf formats that are supported.  */
  InitDmaBufFormats ();

  /* And initialize the SHM formats that are supported.  */
  InitShmFormats ();

  /* TODO: remove this message.  */
  fprintf (stderr, "EGL initialization complete.\n");
}

static Bool
ValidateShmParams (uint32_t format, uint32_t width, uint32_t height,
		   int32_t offset, int32_t stride, size_t pool_size)
{
  size_t total, after, min_stride;
  FormatInfo *info;

  if (stride < 0 || offset < 0)
    /* Return False if any signed values are less than 0.  */
    return False;

  /* Calculate the total size of the buffer, and make sure it is
     smaller than the pool size.  */
  if (IntMultiplyWrapv ((size_t) height, stride, &total))
    /* If obtaining the total size would overflow size_t, return.  */
    return False;

  /* If the total size + offset is larger than the pool size,
     return.  */
  if (IntAddWrapv (offset, total, &after)
      || after > pool_size)
    return False;

  /* Get the format info.  */
  info = FindFormatInfo (format);

  if (info == NULL)
    /* This isn't supposed to happen but pacifies
       -Wanalyzer-null-dereference.  */
    return False;

  /* If the stride is not enough to hold width, return.  */
  if (IntMultiplyWrapv ((size_t) width, info->bpp / 8, &min_stride)
      || stride < min_stride
      /* If stride is not a multiple of the pixel size, return.  */
      || stride % (info->bpp / 8))
    return False;

  /* The dimensions are valid.  */
  return True;
}

static void
GetShmParams (EglBuffer *buffer, void **data_ptr, size_t *expected_size)
{
  char *pool_data;

  /* The end of a buffer is as follows: pool data + offset + stride *
     height.  The following assumptions must also hold true: stride >=
     format->bpp / 8 * buffer->width, offset + size < pool size.  */

  pool_data = *buffer->u.shm.data;

  *data_ptr = pool_data + buffer->u.shm.offset;
  *expected_size = (size_t) buffer->u.shm.stride * buffer->height;
}

static void
UpdateTexture (EglBuffer *buffer)
{
  GLenum target;
  void *data_ptr;
  size_t expected_data_size;

  /* Get the appropriate target for the texture.  */
  target = GetTextureTarget (buffer);

  /* Bind the target to the texture.  */
  glBindTexture (target, buffer->texture);

  /* Set the wrapping mode to CLAMP_TO_EDGE.  */
  glTexParameteri (target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  switch (buffer->u.type)
    {
    case DmaBufBuffer:
      /* This is easy.  Simply bind the EGL image to the target.  */
      IEGLImageTargetTexture2D (target, buffer->u.dmabuf.image);
      break;

    case ShmBuffer:
      /* This is much more complicated... First, set the row length to
	 the stride.  */
      glPixelStorei (GL_UNPACK_ROW_LENGTH_EXT,
		     buffer->u.shm.stride / (buffer->u.shm.format->bpp / 8));

      /* Compute the expected data size and data pointer of the
	 buffer.  This is only valid until the next time ResizePool is
	 called.  */
      GetShmParams (buffer, &data_ptr, &expected_data_size);

      /* Next, specify the 2D image.  */
      glTexImage2D (target, 0, (buffer->u.shm.format->gl_internalformat
				? buffer->u.shm.format->gl_internalformat
				: buffer->u.shm.format->gl_format),
		    buffer->width, buffer->height, 0,
		    buffer->u.shm.format->gl_format,
		    buffer->u.shm.format->gl_type, data_ptr);

      /* Unset the row length.  */
      glPixelStorei (GL_UNPACK_ROW_LENGTH_EXT, 0);

      /* The buffer's been copied to the texture.  It can now be
	 released.  */
      buffer->flags |= CanRelease;
    }

  /* Bind the target to nothing.  */
  glBindTexture (target, 0);
}

static void
ReverseTransformToBox (DrawParams *params, pixman_box32_t *box)
{
  double x_factor, y_factor;

  if (!params)
    return;

  /* Apply the inverse of PARAMS to BOX, for use in damage
     tracking.  */

  if (params->flags & ScaleSet)
    {
      box->x1 = floor (box->x1 / params->scale);
      box->y1 = floor (box->y1 / params->scale);
      box->x2 = ceil (box->x2 / params->scale);
      box->y2 = ceil (box->y2 / params->scale);
    }

  if (params->flags & OffsetSet)
    {
      /* Since the offset can be a fractional value, also try to
	 include as much as possible in the box.  */
      box->x1 = floor (box->x1 + params->off_x);
      box->y1 = floor (box->y1 + params->off_y);
      box->x2 = ceil (box->x2 + params->off_x);
      box->y2 = ceil (box->y2 + params->off_y);
    }

  if (params->flags & StretchSet)
    {
      x_factor = params->crop_width / params->stretch_width;
      y_factor = params->crop_height / params->stretch_height;

      box->x1 = floor (box->x1 * x_factor);
      box->y1 = floor (box->y1 * y_factor);
      box->x2 = ceil (box->x2 * x_factor);
      box->y2 = ceil (box->y2 * y_factor);
    }
}

static void
UpdateShmBufferIncrementally (EglBuffer *buffer, pixman_region32_t *damage,
			      DrawParams *params)
{
  GLenum target;
  pixman_box32_t *boxes, box;
  int nboxes, i, width, height;
  void *data_ptr;
  size_t expected_data_size;

  /* Obtain the rectangles that are part of the damage.  */
  boxes = pixman_region32_rectangles (damage, &nboxes);

  if (!nboxes)
    return;

  /* Compute the expected data size and data pointer of the buffer.
     This is only valid until the next time ResizePool is called.  */
  GetShmParams (buffer, &data_ptr, &expected_data_size);

  /* Get the texturing target.  */
  target = GetTextureTarget (buffer);

  /* Bind the target to the texture.  */
  glBindTexture (target, buffer->texture);

  /* And copy from the shm data to the texture according to
     the damage.  */
  for (i = 0; i < nboxes; ++i)
    {
      /* Get a copy of the box.  */
      box = boxes[i];

      /* Transform the box according to any transforms.  */
      ReverseTransformToBox (params, &box);

      /* Clip the box X and Y to 0, 0.  */
      box.x1 = MIN (box.y1, 0);
      box.y1 = MIN (box.y1, 0);

      /* These computations are correct, since box->x2/box->y2 are
	 actually 1 pixel outside the last pixel in the box.  */
      width = MIN (box.x2, buffer->width) - box.x1;
      height = MIN (box.y2, buffer->height) - box.y1;

      if (width <= 0 || height <= 0)
	/* The box is effectively empty because it straddles one of
	   the corners of the buffer, or is outside of the buffer.  */
	continue;

      /* First, set the length of a single row.  */
      glPixelStorei (GL_UNPACK_ROW_LENGTH_EXT,
		     buffer->u.shm.stride / (buffer->u.shm.format->bpp / 8));

      /* Next, skip box.x1 pixels of each row.  */
      glPixelStorei (GL_UNPACK_SKIP_PIXELS_EXT, box.x1);

      /* And box.y1 rows.  */
      glPixelStorei (GL_UNPACK_SKIP_ROWS_EXT, box.y1);

      /* Copy the image into the sub-texture.  */
      glTexSubImage2D (target, 0, box.x1, box.y1, width, height,
		       buffer->u.shm.format->gl_format,
		       buffer->u.shm.format->gl_type, data_ptr);
    }

  /* Unspecify pixel sotrage modes.  */
  glPixelStorei (GL_UNPACK_ROW_LENGTH_EXT, 0);
  glPixelStorei (GL_UNPACK_SKIP_PIXELS_EXT, 0);
  glPixelStorei (GL_UNPACK_SKIP_ROWS_EXT, 0);

  /* Unbind from the texturing target.  */
  glBindTexture (target, 0);

  /* The buffer's been copied to the texture.  It can now be
     released.  */
  buffer->flags |= CanRelease;
}

static void
EnsureTexture (EglBuffer *buffer)
{
  /* If a texture has already been created, return.  */
  if (buffer->flags & IsTextureGenerated)
    return;

  /* Generate the name for the texture.  */
  glGenTextures (1, &buffer->texture);

  /* Update all texture data.  */
  UpdateTexture (buffer);

  /* Mark the texture as generated.  */
  buffer->flags |= IsTextureGenerated;
}

static void
UpdateBuffer (RenderBuffer buffer, pixman_region32_t *damage,
	      DrawParams *params)
{
  EglBuffer *egl_buffer;

  egl_buffer = buffer.pointer;

  if (!(egl_buffer->flags & IsTextureGenerated))
    /* No texture has been generated, so just create one and maybe
       upload the contents.  */
    EnsureTexture (egl_buffer);
  else if (!damage)
    /* Upload all the contents to the buffer's texture if the buffer
       type requires manual updates.  Buffers backed by EGLImages do
       not appear to need updates, since updates to the EGLImage are
       automatically reflected in the texture.

       However, someone on #dri says calling
       glEGLImageTargetTexture2DOES is still required and not doing so
       may cause certain drivers to stop working in the future.  So
       still do it for buffers backed by EGLImages.  */
    UpdateTexture (egl_buffer);
  else if (pixman_region32_not_empty (damage))
    {
      switch (egl_buffer->u.type)
	{
	case ShmBuffer:
	  /* Update the shared memory buffer incrementally, taking
	     into account the damaged area and transform.  */
	  UpdateShmBufferIncrementally (egl_buffer, damage,
					params);
	  break;

	case DmaBufBuffer:
	  /* See comment in !damage branch.  */
	  UpdateTexture (egl_buffer);
	  break;
	}
    }
}

static void
UpdateBufferForDamage (RenderBuffer buffer, pixman_region32_t *damage,
		       DrawParams *params)
{
  UpdateBuffer (buffer, damage, params);
}

static Bool
CanReleaseNow (RenderBuffer buffer)
{
  EglBuffer *egl_buffer;
  Bool rc;

  egl_buffer = buffer.pointer;

  /* Return if texture contents were copied.  */
  rc = (egl_buffer->flags & CanRelease) != 0;

  /* Clear that flag now.  */
  egl_buffer->flags &= ~CanRelease;

  return rc;
}

static BufferFuncs egl_buffer_funcs =
  {
    .get_drm_formats = GetDrmFormats,
    .get_render_device = GetRenderDevice,
    .get_shm_formats = GetShmFormats,
    .buffer_from_dma_buf = BufferFromDmaBuf,
    .buffer_from_dma_buf_async = BufferFromDmaBufAsync,
    .buffer_from_shm = BufferFromShm,
    .validate_shm_params = ValidateShmParams,
    .free_shm_buffer = FreeShmBuffer,
    .free_dmabuf_buffer = FreeDmabufBuffer,
    .update_buffer_for_damage = UpdateBufferForDamage,
    .can_release_now = CanReleaseNow,
    .init_buffer_funcs = InitBufferFuncs,
  };

void
InitEgl (void)
{
  RegisterStaticRenderer ("egl", &egl_render_funcs,
			  &egl_buffer_funcs);
}
