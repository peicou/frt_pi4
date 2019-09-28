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
    int _device;
    drmModeRes * _resources;
    drmModeConnector * _connector;
    uint32_t _connector_id;
    drmModeEncoder * _encoder;
    drmModeModeInfo  _mode_info;
    drmModeCrtc * _crtc;
    struct gbm_device * _gbm_device;
    struct gbm_surface * _gbm_surface;
    struct gbm_bo * _previous_bo = NULL;
    uint32_t _previous_fb;    
    struct gbm_bo * _bo;  
    uint32_t _handle;
    uint32_t _pitch;
    uint32_t _fb;
    uint64_t _modifier;
    EGLConfig _configs[32];
    int _config_index;

    drmModeConnector * _find_connector (drmModeRes *resources) 
    {
        for (int i=0; i<resources->count_connectors; i++) 
        {
            drmModeConnector *connector = drmModeGetConnector (_device, resources->connectors[i]);
            if (connector->connection == DRM_MODE_CONNECTED) {return connector;}
            drmModeFreeConnector (connector);
        }

        return NULL; // if no connector found
    }

    drmModeEncoder * _find_encoder (drmModeRes *resources, drmModeConnector *connector) 
    {
        if (connector->encoder_id) {return drmModeGetEncoder (_device, connector->encoder_id);}
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
            _device = open ("/dev/dri/card1", O_RDWR);
        printf("card1 oppened\n");
        _resources = drmModeGetResources (_device);
        printf("resources gotten\n");
        _connector = _find_connector (_resources);
        printf("connector found \n");
        if (_connector == NULL)
        {
            fatal("connector is null. no fb found");
        }
        _connector_id = _connector->connector_id;
        _mode_info = _connector->modes[0];
        _encoder = _find_encoder (_resources, _connector);
        printf("encoder oppened\n");
        _crtc = drmModeGetCrtc (_device, _encoder->crtc_id);
        drmModeFreeEncoder (_encoder);
        drmModeFreeConnector (_connector);
        drmModeFreeResources (_resources);
        _gbm_device = gbm_create_device (_device);
        display = eglGetDisplay (_gbm_device);
    
        result = eglInitialize (display, NULL ,NULL);
        if (result == EGL_FALSE)
            fatal("eglInitialize failed.");
        result = eglBindAPI (EGL_OPENGL_API);
        if (result == EGL_FALSE)
            fatal("eglBindAPI failed.");
        
        eglGetConfigs(display, NULL, 0, &count);
        result = eglChooseConfig (display, attributes, &_configs[0], count, &num_config);
        if (result == EGL_FALSE)
            fatal("eglChooseConfig failed.");

        _config_index = _match_config_to_visual(display,GBM_FORMAT_XRGB8888,&_configs[0],num_config);
        context = eglCreateContext (display, &_configs[_config_index], EGL_NO_CONTEXT, context_attribs);
        if (context == EGL_NO_CONTEXT)
            fatal("eglCreateContext failed.");
    }

    void create_surface() {
        surface = eglCreateWindowSurface (display, &_configs[_config_index], _gbm_surface, NULL);
        if (surface == EGL_NO_SURFACE)
            fatal("video_bcm: eglCreateWindowSurface failed.");
    }

    void swap_buffers()
    {
        eglSwapBuffers (display, surface);
        _bo = gbm_surface_lock_front_buffer (_gbm_surface);
        _handle = gbm_bo_get_handle (_bo).u32;
        _pitch = gbm_bo_get_stride (_bo);
        drmModeAddFB (_device, _mode_info.hdisplay, _mode_info.vdisplay, 24, 32, _pitch, _handle, &_fb);
        drmModeSetCrtc (_device, _crtc->crtc_id, _fb, 0, 0, &_connector_id, 1, &_mode_info);
        if (_previous_bo) 
        {
            drmModeRmFB (_device, _previous_fb);
            gbm_surface_release_buffer (_gbm_surface, _previous_bo);
        }
        _previous_bo = _bo;
        _previous_fb = _fb;
    }
    //! getters
    int getDevice() { return _device; }
    drmModeCrtc * getCrtc() { return _crtc; }
    uint32_t * getConnector_id() { return &_connector_id; }
    struct gbm_bo * getPrevious_bo() {return _previous_bo; } 
    uint32_t getPrevious_fb() { return _previous_fb; }
    struct gbm_surface * getGbm_surface() { return _gbm_surface; }
    struct gbm_device * getGbm_device() { return _gbm_device; }
};

class VideoBCMFull : public Video, public ContextGL {
private:
    EGLBCMFullContext egl;
    bool initialized;
    Vec2 screen_size;
    bool vsync;

    void init_egl(Vec2 size) {
        egl.init();
        egl.create_surface();
        egl.make_current();
        initialized = true;
    }
    void cleanup_egl() 
    {
        drmModeCrtc * crtc = egl.getCrtc();
        if (!initialized)
            return;

        drmModeSetCrtc (egl.getDevice(), crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y, egl.getConnector_id(), 1, &crtc->mode);
        drmModeFreeCrtc (crtc);
        if (egl.getPrevious_bo()) 
        {
            drmModeRmFB (egl.getDevice(), egl.getPrevious_fb());
            gbm_surface_release_buffer (egl.getGbm_surface(), egl.getPrevious_bo());
        }
        egl.destroy_surface();
        gbm_surface_destroy (egl.getGbm_surface());
        egl.cleanup();
        gbm_device_destroy (egl.getGbm_device());
        close (egl.getDevice());
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
        screen_size = size;
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
