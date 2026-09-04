# 12to11
This is a tool for running Wayland applications on an X server,
preferably with a compositing manager running.

---

## Current Status
It is not yet complete.  What is not yet implemented includes support
for touchscreens, and device switching in dmabuf feedback.

It is not portable to systems other than recent versions of GNU/Linux
running the X.Org server 1.20 or later.
It historically was tested on GNOME Shell, but currently testing is mostly done
in MATE Desktop Environment, with HiDPI enabled. Some other contributors also
tested it in their own environment.
Report if you encountered any weird behaviors in your own environment.

It will not work very well unless the compositing manager supports the
EWMH frame synchronization protocol.
(Which is probably already implemented by the vast majority of modern X11 compositing managers)

In HiDPI environments, the result may be a bit pxielated due to scaling, proper
client side HiDPI isnt supported yet.

The following Wayland protocols are implemented to a more-or-less
complete degree:

```
  'wl_output',                                  version:  4
  'wl_compositor',                              version:  5
  'wl_shm',                                     version:  1
  'xdg_wm_base',                                version:  5
  'wl_subcompositor',                           version:  1
  'wl_seat',                                    version:  8
  'wl_data_device_manager',                     version:  3
  'zwp_linux_dmabuf_v1',                        version:  4
  'zwp_primary_selection_device_manager_v1',    version:  1
  'wp_viewporter',                              version:  1
  'zxdg_decoration_manager_v1',                 version:  1
  'zwp_text_input_manager_v3',                  version:  1
  'wp_single_pixel_buffer_manager_v1',          version:  1
  'zwp_pointer_constraints_v1',                 version:  1
  'zwp_relative_pointer_manager_v1',            version:  1
  'zwp_idle_inhibit_manager_v1',                version:  1
  'xdg_activation_v1',                          version:  1
  'wp_tearing_control_manager_v1',		version:  1
  'zwlr_layer_shell_v1',                        version:  4
  'ext_image_copy_capture_manager_v1',          version:  1
  'ext_output_image_capture_source_manager_v1', version:  1
```

When built with EGL, the following Wayland protocol is also supported:

```
  'zwp_linux_explicit_synchronization_v1',      version:  2
```

When the X server supports version 1.6 or later of the X Resize,
Rotate and Reflect Extension, the following Wayland protocol is also
supported:
```
  'wp_drm_lease_device_v1',                     version: 1
```
When the X server supports version 2.4 or later of the X Input
Extension, the following Wayland protocol is also supported:
```
  'zwp_pointer_gestures_v1',                    version: 3
```
## Requirements
`wayland-scanner` and `gawk` are required to build 12to11.
Building and running this tool also requires the following X protocol
extensions:
```
  Nonrectangular Window Shape Extension, version 1.1 or later
  MIT Shared Memory Extension, version 1.2 or later
  X Resize, Rotate and Reflect Extension, version 1.4 or later
  X Synchronization Extension, version 1.0 or later
  X Rendering Extension, version 1.2 or later
  X Input Extension, version 2.3 or later
  Direct Rendering Interface 3, version 1.2 or later
  X Fixes Extension, version 1.5 or later
  X Presentation Extension, version 1.0 or later
```
They should already exist if you are using modern Xorg or Xlibre.
In addition, it requires Xlib to be built with the XCB transport, and
the XCB bindings for MIT-SHM and DRI3 to be available.

EGL support requires the EGL and GLESv2 development files, and for
the following EGL and GLES extensions to be present at runtime:
```
  EGL_EXT_platform_base
  EGL_EXT_device_query
  EGL_KHR_image_base
  EGL_EXT_image_dma_buf_import_modifiers
  EGL_EXT_image_dma_buf_import
  EGL_EXT_buffer_age

  GL_OES_EGL_image
  GL_OES_EGL_image_external
  GL_EXT_read_format_bgra
  GL_EXT_unpack_subimage
```
Most modern EGL/GLESv2 implementions already have these covered.

Be sure to configure your system so that idle inhibition is reported
correctly.  For more details, see the description of the
idleInhibitCommand resource in the manual page.

## Building
Building the source code is simple, provided that you have the
necessary libwayland-server library, pixman, XCB, DRM, xshmfence, and
X extension libraries installed:
```bash
  make # to build the binary
```
The binary will be generated as `build/12to11`.

EGL support is enabled by default, and can be disabled by 
building with 
```bash
make EGL=0
```
## Installation
To install 12to11, you can use
```bash
make install
```
By default this will install 12to11 into `/usr/local`. However, if you want
to install it into other prefix(e.g. `/usr`), you can use `PREFIX=`:
```bash
make PREFIX=/usr install
```
instead.
For packaging you can use `DESTDIR=`:
```bash
make PREFIX=/usr DESTDIR="$pkgdir" install
```

## Run 12to11
Running 12to11 should be simple:
```bash
  12to11
```
Wayland programs will then run as regular X windows.

To test this, you can intentionally empty the environment varible `DISPLAY`
for the target programs that support both X11 and wayland to force them to
use 12to11.

Because many graphic libraries would prefer wayland when both X11 and wayland
environments are available, only run 12to11 when you need to run any wayland
specific programs.

## Source Code Structure

The source code directory is organized as follows:

```
  Makefile      - the top level Makefile
  src/          - C source code
  protocols/    - Wayland protocol definition source
  data/         - scripts and text data used to generate some headers,
                  i.e. those containing MIME types or shaders
  tests/        - integration tests
  man/          - the manual page
```

## License
[GPLv3](./COPYING)
