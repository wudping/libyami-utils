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

using namespace YamiMediaCodec;

#define CPPPRINT(...) std::cout << __VA_ARGS__ << std::endl

typedef struct SimplePlayerParameter {
    string inputFile;
    string outputFile;
    uint32_t outputFrameNumber;
    uint16_t surfaceNumber;
    uint32_t readSize;
    bool dumpToFile;
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
            parameters->dumpToFile = !atoi(optarg);
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
    if (!parameters->dumpToFile) {
        ERROR("x11 is disabled, so not support readering to X window!");
        return false;
    }
#endif
    return true;
}

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

        if (!initDisplay()) {
            return false;
        }
        //set native display
        m_decoder->setNativeDisplay(m_nativeDisplay.get());
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
                if (renderOutputs(frame))
                    continue;
                else
                    return false;
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
                            if (renderOutputs(frame))
                                continue;
                            else
                                return false;
                        }
                        else {
                            break;
                        }
                    }

                    const VideoFormatInfo* formatInfo = m_decoder->getFormatInfo();
#ifdef __ENABLE_X11__
                    if (!m_parameters.dumpToFile)
                        resizeWindow(formatInfo->width, formatInfo->height);
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
        m_parameters.dumpToFile = true;
        m_parameters.getFirstFrame = false;
        m_parameters.enableLowLatency = false;

        m_eos = false;
        m_drmFd = -1;
        m_frameNum = 0;
#ifdef __ENABLE_X11__
        m_window = 0;
#endif
    }
    ~SimplePlayer()
    {
        m_decoder.reset();
        if (m_nativeDisplay) {
            vaTerminate(m_vaDisplay);
        }
#ifdef __ENABLE_X11__
        if (m_window) {
            XDestroyWindow(m_display.get(), m_window);
        }
#endif
        if (m_drmFd >= 0)
            close(m_drmFd);
        if (m_ofs.is_open())
            m_ofs.close();
    }
private:
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
        if (m_parameters.dumpToFile) {
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
        else {
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
        m_frameNum++;

        return true;
    }

    bool createVadisplay()
    {
        if (m_parameters.dumpToFile) {
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
        else {
            Display* display = XOpenDisplay(NULL);
            if (!display) {
                ERROR("Failed to XOpenDisplay.\n");
                return false;
            }
            m_display.reset(display, XCloseDisplay);
            m_vaDisplay = vaGetDisplay(m_display.get());
        }
#endif
        return true;
    }

    bool initDisplay()
    {
        if (!createVadisplay())
            return false;

        int major, minor;
        VAStatus status;
        status = vaInitialize(m_vaDisplay, &major, &minor);
        if (status != VA_STATUS_SUCCESS) {
            ERROR("init va failed status = %d", status);
            return false;
        }
        else
            INFO("major = %d, minor = %d\n", major, minor);

        m_nativeDisplay.reset(new NativeDisplay);
        m_nativeDisplay->type = NATIVE_DISPLAY_VA;
        m_nativeDisplay->handle = (intptr_t)m_vaDisplay;
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
};

int main(int argc, char** argv)
{

    SimplePlayer player;
    if (!player.init(argc, argv)) {
        ERROR("init player failed with %s", argv[1]);
        return -1;
    }
    if (!player.run()){
        ERROR("run simple player failed");
        return -1;
    }
    CPPPRINT("get frame number: " << player.getFrameNum());
    return  0;
}

