// video_bcm.cc
//scons platform=frt target=release frt_arch=pi4 tools=no module_webm_enabled=no CCFLAGS="-mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mtune=cortex-a72 -mfloat-abi=hard -mlittle-endian -munaligned-access"
/*
 * FRT - A Godot platform targeting single board computers
 * Copyright (c) 2017-2019  Emanuele Fornara
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "frt.h"

#include <stdio.h>

#include <sys/time.h>

#include "dl/gles2.gen.h"
#include "bits/egl_base_context.h"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

namespace frt {

class EGLBCMFullContext : public EGLBaseContext {
private:
    int device;
    drmModeRes *resources;
    drmModeConnector *connector;
    uint32_t connector_id;
    drmModeEncoder *encoder;
    drmModeModeInfo mode_info;
    drmModeCrtc *crtc;
    struct gbm_device *gbm_device;
    struct gbm_bo *bo;  
    uint32_t handle;
    uint32_t pitch;
    int32_t fb;
    uint64_t modifier;
    EGLConfig **configs;
    int config_index;

    drmModeConnector * _find_connector (drmModeRes *resources) 
    {
        for (int i=0; i<resources->count_connectors; i++) 
        {
            drmModeConnector *connector = drmModeGetConnector (device, resources->connectors[i]);
            if (connector->connection == DRM_MODE_CONNECTED) {return connector;}
            drmModeFreeConnector (connector);
        }

        return NULL; // if no connector found
    }

    drmModeEncoder * _find_encoder (drmModeRes *resources, drmModeConnector *connector) 
    {
        if (connector->encoder_id) {return drmModeGetEncoder (device, connector->encoder_id);}
        return NULL; // if no encoder found
    }

    int _match_config_to_visual(EGLDisplay egl_display, EGLint visual_id, EGLConfig *configs, int count) 
    {
        EGLint id;
        for (int i = 0; i < count; ++i) 
        {
            if (!eglGetConfigAttrib(egl_display, configs[i], EGL_NATIVE_VISUAL_ID,&id)) continue;
            if (id == visual_id) return i;
        }
        return -1;
    }

public:
    void init()
    {
        EGLBoolean result;
        EGLint num_config;
        EGLint count=0;

        static EGLint attributes[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 0,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE
                };

        static const EGLint context_attribs[] = {
                EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL_NONE
                };
            device = open ("/dev/dri/card1", O_RDWR);
        printf("card1 oppened\n");
        resources = drmModeGetResources (device);
        printf("resources gotten\n");
        connector = _find_connector (resources);
        printf("connector found \n");
        if (connector == NULL)
        {
            fatal("connector is null. no fb found");
        }
        connector_id = connector->connector_id;
        mode_info = connector->modes[0];
        encoder = _find_encoder (resources, connector);
        printf("encoder oppened\n");
        crtc = drmModeGetCrtc (device, encoder->crtc_id);
        drmModeFreeEncoder (encoder);
        drmModeFreeConnector (connector);
        drmModeFreeResources (resources);
        gbm_device = gbm_create_device (device);
        display = eglGetDisplay (gbm_device);
    
        result = eglInitialize (display, NULL ,NULL);
        if (result == EGL_FALSE)
            fatal("eglInitialize failed.");
        result = eglBindAPI (EGL_OPENGL_API);
        if (result == EGL_FALSE)
            fatal("eglBindAPI failed.");
        
        eglGetConfigs(display, NULL, 0, &count);
        configs = malloc(count * sizeof(*configs));
        result = eglChooseConfig (display, attributes, configs, count, &num_config);
        if (result == EGL_FALSE)
            fatal("eglChooseConfig failed.");

        config_index = _match_config_to_visual(display,GBM_FORMAT_XRGB8888,configs,num_config);
        context = eglCreateContext (display, configs[config_index], EGL_NO_CONTEXT, context_attribs);
        if (context == EGL_NO_CONTEXT)
            fatal("eglCreateContext failed.");
    }

    void create_surface() {
        surface = eglCreateWindowSurface (display, configs[config_index], gbm_surface, NULL);
        if (surface == EGL_NO_SURFACE)
            fatal("video_bcm: eglCreateWindowSurface failed.");
        free(configs);
    }

    void swap_buffers()
    {
        eglSwapBuffers (display, surface);
        bo = gbm_surface_lock_front_buffer (gbm_surface);
        handle = gbm_bo_get_handle (bo).u32;
        pitch = gbm_bo_get_stride (bo);
        drmModeAddFB (device, mode_info.hdisplay, mode_info.vdisplay, 24, 32, pitch, handle, &fb);
        drmModeSetCrtc (device, crtc->crtc_id, fb, 0, 0, &connector_id, 1, &mode_info);
        if (previous_bo) 
        {
            drmModeRmFB (device, previous_fb);
            gbm_surface_release_buffer (gbm_surface, previous_bo);
        }
        previous_bo = bo;
        previous_fb = fb;
    }
};

class VideoBCMFull : public Video, public ContextGL {
private:
    EGLBCMFullContext egl;
    bool initialized;
    Vec2 screen_size;
    bool vsync;

    void init_egl(Vec2 size) {
        egl.init();
        egl.create_surface(view);
        egl.make_current();
        initialized = true;
    }
    void cleanup_egl() 
    {
        if (!initialized)
            return;

        drmModeSetCrtc (device, crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y, &connector_id, 1, &crtc->mode);
        drmModeFreeCrtc (crtc);
        if (previous_bo) 
        {
            drmModeRmFB (device, previous_fb);
            gbm_surface_release_buffer (gbm_surface, previous_bo);
        }
        egl.destroy_surface();
        gbm_surface_destroy (gbm_surface);
        egl.cleanup();
        gbm_device_destroy (gbm_device);
        close (device);
        initialized = false;
    }

public:
    // Module
    VideoBCMFull()
        : initialized(false), vsync(true) {}
    const char *get_id() const { return "video_bcm_full"; }
    bool probe() {
        //if (!frt_load_bcm("libbcm_host.so"))
        //    return false;
        if (!frt_load_gles2("libbrcmGLESv2.so"))
            return false;
        if (!frt_load_egl("libbrcmEGL.so"))
            return false;
        screen_size.x = 720;
        screen_size.y = 480;
        return true;
    }
    void cleanup() {
        cleanup_egl();
    }
    // Video
    Vec2 get_screen_size() const { return screen_size; }

    ContextGL *create_the_gl_context(int version, Vec2 size) {
        if (version != 2)
            return 0;
        view_size = size;
        return this;
    }
    bool provides_quit() { return false; }
    // ContextGL
    void release_current() {
        egl.release_current();
    }
    void make_current() {
        egl.make_current();
    }
    void swap_buffers() {
        egl.swap_buffers();
    }
    bool initialize() {
        init_egl(screen_size);
        return true;
    }
    void set_use_vsync(bool use) {
        egl.swap_interval(use ? 1 : 0);
        vsync = use;
    }
    bool is_using_vsync() const { return vsync; }
};

FRT_REGISTER(VideoBCMFull)

} // namespace frt
