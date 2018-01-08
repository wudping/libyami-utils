/*
 * Copyright (C) 2013-2014 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "tests/decodeinput.h"
#include "common/log.h"
#include <Yami.h>
#include <stdlib.h>
#include <fcntl.h>
#include <va/va_drm.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <unistd.h>
#ifdef __ENABLE_X11__
#include <X11/Xlib.h>
#include <va/va_x11.h>
#endif
#ifdef __ENABLE_WAYLAND__
#include <va/va_wayland.h>
#endif

using namespace YamiMediaCodec;

#define CPPPRINT(...) std::cout << __VA_ARGS__ << std::endl
#define FILE_OUTPUT          0
#define X11_RENDERING        1
#define WAYLAND_RENDERING    2

typedef struct SimplePlayerParameter {
    string inputFile;
    string outputFile;
    uint32_t outputFrameNumber;
    uint16_t surfaceNumber;
    uint32_t readSize;
    uint16_t outputMode;
    bool getFirstFrame;
    bool enableLowLatency;
} SimplePlayerParameter;

void printHelp(const char* app)
{
    CPPPRINT(app << " -i input.264 -m 0");
    CPPPRINT("   -i media file to decode");
    CPPPRINT("   -o specify the name of dumped output file");
    CPPPRINT("   -r read size, only for 264, default 120539");
    CPPPRINT("   -s surface number, only for 264, default 8");
    CPPPRINT("   -l low latency");
    CPPPRINT("   -g just to get surface of the first decoded frame");
    CPPPRINT("   -n specify how many frames to be decoded");
    CPPPRINT("   -m render mode, default 0");
    CPPPRINT("      0: dump video frame to file in NV12 format [*]");
    CPPPRINT("      1: render to X window [*]");
}

bool processCmdLine(int argc, char** argv, SimplePlayerParameter* parameters)
{
    char opt;
    while ((opt = getopt(argc, argv, "h?r:s:lgi:o:n:m:")) != -1) {
        switch (opt) {
        case 'h':
        case '?':
            printHelp(argv[0]);
            return false;
        case 'r':
            parameters->readSize = atoi(optarg);
            break;
        case 's':
            parameters->surfaceNumber = atoi(optarg);
            break;
        case 'l':
            parameters->enableLowLatency = true;
            break;
        case 'g':
            parameters->getFirstFrame = true;
            break;
        case 'i':
            parameters->inputFile.assign(optarg);
            break;
        case 'o':
            parameters->outputFile.assign(optarg);
            break;
        case 'n':
            parameters->outputFrameNumber = atoi(optarg);
            break;
        case 'm':
            parameters->outputMode = atoi(optarg);
            break;
        default:
            printHelp(argv[0]);
            return false;
        }
    }

    if (optind < argc) {
        int indexOpt = optind;
        CPPPRINT("unrecognized option: ");
        while (indexOpt < argc)
            CPPPRINT(argv[indexOpt++]);
        CPPPRINT("");
        return false;
    }

    if (parameters->inputFile.empty()) {
        printHelp(argv[0]);
        ERROR("no input file.");
        return false;
    }
#ifndef __ENABLE_X11__
    printf("dpwu  %s %s %d, parameters->outputMode = %d ====\n", __FILE__, __FUNCTION__, __LINE__, parameters->outputMode);
    if (X11_RENDERING == parameters->outputMode) {
        ERROR("x11 is disabled, so not support readering to X window!");
        return false;
    }
#endif
#ifndef __ENABLE_WAYLAND__
    printf("dpwu  %s %s %d, parameters->outputMode = %d ====\n", __FILE__, __FUNCTION__, __LINE__, parameters->outputMode);
    if (WAYLAND_RENDERING == parameters->outputMode) {
        ERROR("WAYLAND is disabled, so not support readering to WAYLAND window!");
        return false;
    }
#endif
    return true;
}

#ifdef __ENABLE_WAYLAND__

struct VADisplayTerminator {
    VADisplayTerminator() {}
    void operator()(VADisplay* display)
    {
        vaTerminate(*display);
        delete display;
    }
};

#define checkVaapiStatus(status, prompt)                     \
    (                                                        \
        {                                                    \
            bool ret;                                        \
            ret = (status == VA_STATUS_SUCCESS);             \
            if (!ret)                                        \
                ERROR("%s: %s", prompt, vaErrorStr(status)); \
            ret;                                             \
        })

struct display {
    SharedPtr<wl_display>        display;
    SharedPtr<wl_compositor>     compositor;
    SharedPtr<wl_shell>          shell;
    SharedPtr<wl_shell_surface>  shell_surface;
    SharedPtr<wl_surface>        surface;
};

struct WaylanDisplay {
    SharedPtr<wl_display>        display;
    SharedPtr<wl_compositor>     compositor;
    SharedPtr<wl_shell>          shell;
    SharedPtr<wl_shell_surface>  shell_surface;
    SharedPtr<wl_surface>        surface;
};

class DecodeOutputWayland
{
public:
    DecodeOutputWayland();
    virtual ~DecodeOutputWayland();
    bool output(const SharedPtr<VideoFrame>& frame);
    bool init();
    bool DecodeOutput_init()
    {
        m_nativeDisplay.reset(new NativeDisplay);
        m_nativeDisplay->type = NATIVE_DISPLAY_VA;
        m_nativeDisplay->handle = (intptr_t)*m_vaDisplay;
        if (!m_vaDisplay || !m_nativeDisplay) {
            ERROR("init display error");
            return false;
        }
        return true;
    }
public:
    virtual bool setVideoSize(uint32_t width, uint32_t height);
    bool createWaylandDisplay();
    static void registryHandle(void *data, struct wl_registry *registry,
                               uint32_t id, const char *interface, uint32_t version);
    static void frameRedrawCallback(void *data, struct wl_callback *callback, uint32_t time);
    bool ensureWindow(unsigned int width, unsigned int height);
    bool vaPutSurfaceWayland(VASurfaceID surface,
                             const VARectangle *srcRect, const VARectangle *dstRect);
    bool m_redrawPending;
    struct display m_waylandDisplay;
    uint32_t m_width;
    uint32_t m_height;
    SharedPtr<VADisplay> m_vaDisplay;
    SharedPtr<NativeDisplay> m_nativeDisplay;
};

void DecodeOutputWayland::registryHandle(
    void                    *data,
    struct wl_registry      *registry,
    uint32_t                id,
    const char              *interface,
    uint32_t                version
)
{
    struct display * d = (struct display * )data;

    if (strcmp(interface, "wl_compositor") == 0)
        d->compositor.reset((struct wl_compositor *)wl_registry_bind(registry, id,
                                                   &wl_compositor_interface, 1), wl_compositor_destroy);
    else if (strcmp(interface, "wl_shell") == 0)
        d->shell.reset((struct wl_shell *)wl_registry_bind(registry, id,
	                                               &wl_shell_interface, 1), wl_shell_destroy);
}

void DecodeOutputWayland::frameRedrawCallback(void *data,
	                                       struct wl_callback *callback, uint32_t time)
{
    *(bool *)data = false;
    wl_callback_destroy(callback);
}

bool DecodeOutputWayland::ensureWindow(unsigned int width, unsigned int height)
{
    struct display * const d = &m_waylandDisplay;

    if (!d->surface) {
        d->surface.reset(wl_compositor_create_surface(d->compositor.get()), wl_surface_destroy);
        if (!d->surface)
            return false;
    }

    if (!d->shell_surface) {
        d->shell_surface.reset(wl_shell_get_shell_surface(d->shell.get(), d->surface.get()),
                                                                       wl_shell_surface_destroy);
        if (!d->shell_surface)
            return false;
        wl_shell_surface_set_toplevel(d->shell_surface.get());
    }
    return true;
}

bool DecodeOutputWayland::vaPutSurfaceWayland(VASurfaceID surface,
                                              const VARectangle *srcRect,
                                              const VARectangle *dstRect)
{
    VAStatus vaStatus;
    struct wl_buffer *buffer;
    struct wl_callback *callback;
    struct display * const d = &m_waylandDisplay;
    struct wl_callback_listener frame_callback_listener = {frameRedrawCallback};

    if (m_redrawPending) {
        wl_display_flush(d->display.get());
        while (m_redrawPending) {
            wl_display_dispatch(d->display.get());
        }
    }

    if (!ensureWindow(dstRect->width, dstRect->height))
        return false;

    vaStatus = vaGetSurfaceBufferWl(*m_vaDisplay, surface, VA_FRAME_PICTURE, &buffer);
    if (vaStatus != VA_STATUS_SUCCESS)
        return false;

    wl_surface_attach(d->surface.get(), buffer, 0, 0);
    wl_surface_damage(d->surface.get(), dstRect->x,
        dstRect->y, dstRect->width, dstRect->height);
    wl_display_flush(d->display.get());
    m_redrawPending = true;
    callback = wl_surface_frame(d->surface.get());
    wl_callback_add_listener(callback, &frame_callback_listener, &m_redrawPending);
    wl_surface_commit(d->surface.get());
    return true;
}

bool DecodeOutputWayland::output(const SharedPtr<VideoFrame>& frame)
{
    VARectangle srcRect, dstRect;
    if (!setVideoSize(frame->crop.width, frame->crop.height))
        return false;

    srcRect.x = 0;
    srcRect.y = 0;
    srcRect.width  = frame->crop.width;
    srcRect.height = frame->crop.height;

    dstRect.x = frame->crop.x;
    dstRect.y = frame->crop.y;
    dstRect.width  = frame->crop.width;
    dstRect.height = frame->crop.height;
    return vaPutSurfaceWayland((VASurfaceID)frame->surface, &srcRect, &dstRect);
}

bool DecodeOutputWayland::setVideoSize(uint32_t width, uint32_t height)
{
    return ensureWindow(width, height);
}

bool DecodeOutputWayland::createWaylandDisplay()
{
    int major, minor;
    SharedPtr<VADisplay> display;
    struct display *d = &m_waylandDisplay;
    struct wl_registry_listener registry_listener = {
        DecodeOutputWayland::registryHandle,
        NULL,
    };

    d->display.reset(wl_display_connect(NULL), wl_display_disconnect);
    if (!d->display) {
        return false;
    }
    wl_display_set_user_data(d->display.get(), d);
    struct wl_registry *registry = wl_display_get_registry(d->display.get());
    wl_registry_add_listener(registry, &registry_listener, d);
    wl_display_dispatch(d->display.get());
    VADisplay vaDisplayHandle = vaGetDisplayWl(d->display.get());
    
    VAStatus status = vaInitialize(vaDisplayHandle, &major, &minor);
    if (!checkVaapiStatus(status, "vaInitialize"))
        return false;
    m_vaDisplay.reset(new VADisplay(vaDisplayHandle), VADisplayTerminator());
    return true;
}

bool DecodeOutputWayland::init()
{
    return createWaylandDisplay() && DecodeOutput_init();
}

DecodeOutputWayland::DecodeOutputWayland()
    : m_redrawPending(false)
{
}

DecodeOutputWayland::~DecodeOutputWayland()
{
    m_vaDisplay.reset();
}
#endif

class SimplePlayer
{
public:
    uint64_t getFrameNum() { return m_frameNum; }
    bool init(int argc, char** argv)
    {
        if (!processCmdLine(argc, argv, &m_parameters))
            return false;

        if (m_parameters.readSize)
            m_input.reset(DecodeInput::create(m_parameters.inputFile.c_str(), m_parameters.readSize));
        else
            m_input.reset(DecodeInput::create(m_parameters.inputFile.c_str()));
        if (!m_input) {
            ERROR("failed to open %s", m_parameters.inputFile.c_str());
            return false;
        }
        INFO("input initialization finished with file: %s", m_parameters.inputFile.c_str());

        //init decoder
        m_decoder.reset(createVideoDecoder(m_input->getMimeType()), releaseVideoDecoder);
        if (!m_decoder) {
            ERROR("failed create decoder for %s", m_input->getMimeType());
            return false;
        }
#if (1)
        if (!initDisplay()) {
            return false;
        }
#endif
        
        printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
#if (0)
        m_decodeOutputWayland.init();
        m_nativeDisplay = m_decodeOutputWayland.m_nativeDisplay;
        //set native display
#endif
        m_decoder->setNativeDisplay(m_nativeDisplay.get());
        printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
        return true;
    }
    bool run()
    {
        VideoConfigBuffer configBuffer;

        memset(&configBuffer, 0, sizeof(configBuffer));

        configBuffer.profile = VAProfileNone;
        const string codecData = m_input->getCodecData();
        if (codecData.size()) {
            configBuffer.data = (uint8_t*)codecData.data();
            configBuffer.size = codecData.size();
        }

        configBuffer.enableLowLatency = m_parameters.enableLowLatency;
        if (m_parameters.surfaceNumber) {
            configBuffer.noNeedExtraSurface = true;
            configBuffer.flag |= HAS_SURFACE_NUMBER;
            configBuffer.surfaceNumber = m_parameters.surfaceNumber;
        }

        Decode_Status status = m_decoder->start(&configBuffer);
        assert(status == DECODE_SUCCESS);

        VideoDecodeBuffer inputBuffer;
        SharedPtr<VideoFrame> frame;

        while ((!m_parameters.outputFrameNumber) || (m_parameters.outputFrameNumber > 0 && m_frameNum < m_parameters.outputFrameNumber)) {
            frame = m_decoder->getOutput();
            if (frame) {
                if (m_parameters.getFirstFrame) {
                    m_frameNum++;
                    break;
                }
                #if (1)
                if (renderOutputs(frame))
                    continue;
                else
                    return false;
                #endif

                #if (0)
                m_decodeOutputWayland.output(frame);
                #endif
            }
            else if (m_eos) {
                break;
            }

            if (m_input->getNextDecodeUnit(inputBuffer)) {
                status = m_decoder->decode(&inputBuffer);
                if (DECODE_FORMAT_CHANGE == status) {
                    //drain old buffers
                    while ((!m_parameters.outputFrameNumber) || (m_parameters.outputFrameNumber > 0 && m_frameNum < m_parameters.outputFrameNumber)) {
                        frame = m_decoder->getOutput();
                        if (frame) {
                            #if (1)
                            if (renderOutputs(frame))
                                continue;
                            else
                                return false;
                            #endif
                            #if (0)
                            m_decodeOutputWayland.output(frame);
                            #endif
                        }
                        else {
                            break;
                        }
                    }

                    const VideoFormatInfo* formatInfo = m_decoder->getFormatInfo();
#ifdef __ENABLE_X11__
                    if (X11_RENDERING == m_parameters.outputMode)
                        resizeWindow(formatInfo->width, formatInfo->height);
#endif
#ifdef __ENABLE_WAYLAND__
                    if (WAYLAND_RENDERING == m_parameters.outputMode)
                        ensureWindow(formatInfo->width, formatInfo->height);
#endif
                    m_width = formatInfo->width;
                    m_height = formatInfo->height;

                    status = m_decoder->decode(&inputBuffer);
                }
                if (status != DECODE_SUCCESS) {
                    ERROR("decode error status = %d", status);
                    break;
                }
            }
            else {
                inputBuffer.data = NULL;
                inputBuffer.size = 0;
                m_decoder->decode(&inputBuffer);
                m_eos = true;
            }
        }

        m_decoder->stop();

        return true;
    }
    SimplePlayer()
        : m_width(0)
        , m_height(0)
    {
        m_parameters.inputFile.clear();
        m_parameters.outputFile.clear();
        m_parameters.outputFrameNumber = 0;
        m_parameters.surfaceNumber = 0;
        m_parameters.readSize = 0;
        m_parameters.outputMode = 0;
        m_parameters.getFirstFrame = false;
        m_parameters.enableLowLatency = false;

        m_eos = false;
        m_drmFd = -1;
        m_frameNum = 0;
#ifdef __ENABLE_X11__
        m_window = 0;
#endif
#ifdef __ENABLE_WAYLAND__
        m_redrawPending = false;
#endif
    }
    ~SimplePlayer()
    {
        m_decoder.reset();
        if (m_nativeDisplay) {
            vaTerminate(m_vaDisplay);
        }
#ifdef __ENABLE_X11__
        if (m_window && X11_RENDERING == m_parameters.outputMode) {
            XDestroyWindow(m_display.get(), m_window);
        }
#endif
        if (m_drmFd >= 0)
            close(m_drmFd);
        if (m_ofs.is_open())
            m_ofs.close();
    }
private:

#ifdef __ENABLE_WAYLAND__
    static void frameRedrawCallback(void *data,
                                struct wl_callback *callback, uint32_t time)
    {
        *(bool *)data = false;
        wl_callback_destroy(callback);
    }

    bool vaPutSurfaceWayland(VASurfaceID surface,
                                                  const VARectangle *srcRect,
                                                  const VARectangle *dstRect)
    {
        VAStatus vaStatus;
        struct wl_buffer *buffer;
        struct wl_callback *callback;
        struct WaylanDisplay * const d = &m_waylandDisplay;
        struct wl_callback_listener frame_callback_listener = {frameRedrawCallback};
    
        if (m_redrawPending) {
            wl_display_flush(d->display.get());
            while (m_redrawPending) {
                wl_display_dispatch(d->display.get());
            }
        }
    
        if (!ensureWindow(dstRect->width, dstRect->height))
            return false;
    
        vaStatus = vaGetSurfaceBufferWl(m_vaDisplay, surface, VA_FRAME_PICTURE, &buffer);
        if (vaStatus != VA_STATUS_SUCCESS)
            return false;
    
        wl_surface_attach(d->surface.get(), buffer, 0, 0);
        wl_surface_damage(d->surface.get(), dstRect->x,
            dstRect->y, dstRect->width, dstRect->height);
        wl_display_flush(d->display.get());
        m_redrawPending = true;
        callback = wl_surface_frame(d->surface.get());
        wl_callback_add_listener(callback, &frame_callback_listener, &m_redrawPending);
        wl_surface_commit(d->surface.get());
        return true;
    }

    bool waylandOutput(const SharedPtr<VideoFrame>& frame)
    {
        VARectangle srcRect, dstRect;
        if (!ensureWindow(frame->crop.width, frame->crop.height))
            return false;
    
        srcRect.x = 0;
        srcRect.y = 0;
        srcRect.width  = frame->crop.width;
        srcRect.height = frame->crop.height;
    
        dstRect.x = frame->crop.x;
        dstRect.y = frame->crop.y;
        dstRect.width  = frame->crop.width;
        dstRect.height = frame->crop.height;
        return vaPutSurfaceWayland((VASurfaceID)frame->surface, &srcRect, &dstRect);
    }

    bool ensureWindow(unsigned int width, unsigned int height)
    {
        struct WaylanDisplay * const d = &m_waylandDisplay;
    
        if (!d->surface) {
            d->surface.reset(wl_compositor_create_surface(d->compositor.get()), wl_surface_destroy);
            if (!d->surface)
                return false;
        }
    
        if (!d->shell_surface) {
            d->shell_surface.reset(wl_shell_get_shell_surface(d->shell.get(), d->surface.get()),
                                                                           wl_shell_surface_destroy);
            if (!d->shell_surface)
                return false;
            wl_shell_surface_set_toplevel(d->shell_surface.get());
        }
        return true;
    }
#endif

    bool getPlaneResolution_NV12(uint32_t pixelWidth, uint32_t pixelHeight, uint32_t byteWidth[3], uint32_t byteHeight[3])
    {
        int w = pixelWidth;
        int h = pixelHeight;
        uint32_t* width = byteWidth;
        uint32_t* height = byteHeight;
        //NV12 is special since it  need add one for width[1] when w is odd
        {
            width[0] = w;
            height[0] = h;
            width[1] = w + (w & 1);
            height[1] = (h + 1) >> 1;
            return true;
        }
    }

    bool writeNV12ToFile(VASurfaceID surface, int w, int h)
    {
        if (!m_ofs.is_open()) {
            ERROR("No output file for NV12.\n");
            return false;
        }

        VAImage image;
        VAStatus status = vaDeriveImage(m_vaDisplay, surface, &image);
        if (status != VA_STATUS_SUCCESS) {
            ERROR("vaDeriveImage failed = %d\n", status);
            return false;
        }
        uint32_t byteWidth[3], byteHeight[3], planes;
        //image.width is not equal to frame->crop.width.
        //for supporting VPG Driver, use YV12 to replace I420
        planes = 2;
        if (!getPlaneResolution_NV12(w, h, byteWidth, byteHeight)) {
            return false;
        }
        char* buf;
        status = vaMapBuffer(m_vaDisplay, image.buf, (void**)&buf);
        if (status != VA_STATUS_SUCCESS) {
            vaDestroyImage(m_vaDisplay, image.image_id);
            ERROR("vaMapBuffer failed = %d", status);
            return false;
        }
        bool ret = true;
        for (uint32_t i = 0; i < planes; i++) {
            char* ptr = buf + image.offsets[i];
            int w = byteWidth[i];
            for (uint32_t j = 0; j < byteHeight[i]; j++) {
                if (!m_ofs.write(reinterpret_cast<const char*>(ptr), w).good()) {
                    goto out;
                }
                ptr += image.pitches[i];
            }
        }
    out:
        vaUnmapBuffer(m_vaDisplay, image.buf);
        vaDestroyImage(m_vaDisplay, image.image_id);
        return ret;
    }

    bool renderOutputs(const SharedPtr<VideoFrame>& frame)
    {
        if (FILE_OUTPUT == m_parameters.outputMode) {
            if (!m_ofs.is_open()) {
                if (m_parameters.outputFile.empty()) {
                    std::ostringstream stringStream;
                    stringStream << m_parameters.inputFile.c_str();
                    stringStream << "_NV12_" << m_width << "x" << m_height << ".yuv";
                    m_parameters.outputFile = stringStream.str();
                }
                m_ofs.open(m_parameters.outputFile.c_str(), std::ofstream::out | std::ofstream::trunc);
                if (!m_ofs) {
                    ERROR("fail to open output file: %s", m_parameters.outputFile.c_str());
                    return false;
                }
                CPPPRINT("output file: " << m_parameters.outputFile.c_str());
            }

            if (!writeNV12ToFile((VASurfaceID)frame->surface, m_width, m_height))
                return false;
        }
#ifdef __ENABLE_X11__
        else if (X11_RENDERING == m_parameters.outputMode) {
            VAStatus status = VA_STATUS_SUCCESS;
            status = vaPutSurface(m_vaDisplay, (VASurfaceID)frame->surface,
                m_window, 0, 0, m_width, m_height, 0, 0, m_width, m_height,
                NULL, 0, 0);
            if (status != VA_STATUS_SUCCESS) {
                ERROR("vaPutSurface return %d", status);
                return false;
            }
        }
#endif

#ifdef __ENABLE_WAYLAND__
        else if (WAYLAND_RENDERING == m_parameters.outputMode) {
            return waylandOutput(frame);
        }
#endif
        m_frameNum++;

        return true;
    }

    bool createVadisplay()
    {
    
        printf("dpwu  %s %s %d, m_parameters.outputMode = %d ====\n", __FILE__, __FUNCTION__, __LINE__, m_parameters.outputMode);
        if (FILE_OUTPUT == m_parameters.outputMode) {
            m_drmFd = open("/dev/dri/renderD128", O_RDWR);
            if (m_drmFd < 0) {
                CPPPRINT("can't open /dev/dri/renderD128, try to /dev/dri/card0");
                m_drmFd = open("/dev/dri/card0", O_RDWR);
            }
            if (m_drmFd < 0) {
                ERROR("can't open drm device");
                return false;
            }

            m_vaDisplay = vaGetDisplayDRM(m_drmFd);
        }
#ifdef __ENABLE_X11__
        else if (X11_RENDERING == m_parameters.outputMode) {
            Display* display = XOpenDisplay(NULL);
            if (!display) {
                ERROR("Failed to XOpenDisplay.\n");
                return false;
            }
            m_display.reset(display, XCloseDisplay);
            m_vaDisplay = vaGetDisplay(m_display.get());
        }
#endif
#ifdef __ENABLE_WAYLAND__
        else if (WAYLAND_RENDERING == m_parameters.outputMode) {
            printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
            struct WaylanDisplay *d = &m_waylandDisplay;
            struct wl_registry_listener registry_listener = {
                DecodeOutputWayland::registryHandle,
                NULL,
            };
        
            d->display.reset(wl_display_connect(NULL), wl_display_disconnect);
            if (!d->display) {
                return false;
            }
            wl_display_set_user_data(d->display.get(), d);
            struct wl_registry *registry = wl_display_get_registry(d->display.get());
            wl_registry_add_listener(registry, &registry_listener, d);
            wl_display_dispatch(d->display.get());
            m_vaDisplay = vaGetDisplayWl(d->display.get());
            printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
        }
#endif
        return true;
    }

    

    bool initDisplay()
    {
        printf("dpwu  %s %s %d, m_parameters.outputMode = %d ====\n", __FILE__, __FUNCTION__, __LINE__, m_parameters.outputMode);
        if (!createVadisplay())
            return false;

        int major, minor;
        VAStatus status;
        printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
        status = vaInitialize(m_vaDisplay, &major, &minor);
        if (status != VA_STATUS_SUCCESS) {
            ERROR("init va failed status = %d", status);
            return false;
        }
        else
            INFO("major = %d, minor = %d\n", major, minor);
        
        printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);

        m_nativeDisplay.reset(new NativeDisplay);
        m_nativeDisplay->type = NATIVE_DISPLAY_VA;
        m_nativeDisplay->handle = (intptr_t)m_vaDisplay;

        printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
        return true;
    }

#ifdef __ENABLE_X11__
    void resizeWindow(int width, int height)
    {
        Display* display = m_display.get();
        if (m_window) {
            //todo, resize window;
        }
        else {
            DefaultScreen(display);

            XSetWindowAttributes x11WindowAttrib;
            x11WindowAttrib.event_mask = KeyPressMask;
            m_window = XCreateWindow(display, DefaultRootWindow(display),
                0, 0, width, height, 0, CopyFromParent, InputOutput,
                CopyFromParent, CWEventMask, &x11WindowAttrib);
            XMapWindow(display, m_window);
        }
        XSync(display, false);
        {
            DEBUG("m_window=%lu", m_window);
            XWindowAttributes wattr;
            XGetWindowAttributes(display, m_window, &wattr);
        }
    }
#endif

    DecodeOutputWayland m_decodeOutputWayland;
    SharedPtr<NativeDisplay> m_nativeDisplay;
    VADisplay m_vaDisplay;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<DecodeInput> m_input;
    int m_width, m_height;
    bool m_eos;
    int m_drmFd;
    std::ofstream m_ofs;
    uint64_t m_frameNum;
    SimplePlayerParameter m_parameters;
#ifdef __ENABLE_X11__
    SharedPtr<Display> m_display;
    Window m_window;
#endif

#ifdef __ENABLE_WAYLAND__
    struct WaylanDisplay m_waylandDisplay;
    bool m_redrawPending;
#endif
};

int main(int argc, char** argv)
{

    SimplePlayer player;
    if (!player.init(argc, argv)) {
        ERROR("init player failed with %s", argv[1]);
        return -1;
    }
    printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
    if (!player.run()){
        ERROR("run simple player failed");
        return -1;
    }
    printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
    CPPPRINT("get frame number: " << player.getFrameNum());
    return  0;
}

