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
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <va/va_x11.h>
#include <va/va_drm.h>
#include <iostream>

using namespace YamiMediaCodec;

#define CPPPRINT(...) std::cout << __VA_ARGS__ << std::endl

typedef struct SimplePlayerParameter {
    string inputFile;
    string outputFile;
    uint32_t outputFrameNumber;
    bool dumpToFile;
} SimplePlayerParameter;

void printHelp(const char* app)
{
    CPPPRINT(app << " -i input.264 -m 0");
    CPPPRINT("   -i media file to decode");
    CPPPRINT("   -o dumped output file");
    CPPPRINT("   -n specify how many frames to be decoded");
    CPPPRINT("   -m render mode, default 0");
    CPPPRINT("      0: dump video frame to file in NV12 format [*]");
    CPPPRINT("      1: render to X window [*]");
}

bool processCmdLine(int argc, char** argv, SimplePlayerParameter* parameters)
{
    char opt;
    while ((opt = getopt(argc, argv, "h:i:o:n:m:?")) != -1) {
        switch (opt) {
        case 'h':
        case '?':
            printHelp(argv[0]);
            return false;
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
    return true;
}

class SimplePlayer
{
public:
    bool init(int argc, char** argv)
    {
        if (!processCmdLine(argc, argv, &m_parameters))
            return false;

        m_input.reset(DecodeInput::create(m_parameters.inputFile.c_str()));
        if (!m_input) {
            ERROR("failed to open file: %s.", m_parameters.inputFile.c_str());
            return false;
        }
        INFO("input initialization finished with file: %s", m_parameters.inputFile.c_str());

        //init decoder
        m_decoder.reset(createVideoDecoder(m_input->getMimeType()), releaseVideoDecoder);
        if (!m_decoder) {
            ERROR("failed create decoder for %s", m_input->getMimeType());
            return false;
        }
        INFO("decoder is created successfully");

        if (!initDisplay()) {
            return false;
        }

        //set native display
        m_decoder->setNativeDisplay(m_nativeDisplay.get());
        INFO("init finished.");
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

        Decode_Status status = m_decoder->start(&configBuffer);
        assert(status == DECODE_SUCCESS);

        VideoDecodeBuffer inputBuffer;
        while (m_input->getNextDecodeUnit(inputBuffer)) {
            status = m_decoder->decode(&inputBuffer);
            if (DECODE_FORMAT_CHANGE == status) {
                //drain old buffers
                renderOutputs();
                const VideoFormatInfo *formatInfo = m_decoder->getFormatInfo();
                resizeWindow(formatInfo->width, formatInfo->height);
                //resend the buffer
                status = m_decoder->decode(&inputBuffer);
            }
            if(status == DECODE_SUCCESS) {
                renderOutputs();
            } else {
                ERROR("decode error status = %d", status);
                break;
            }
        }
        inputBuffer.data = NULL;
        inputBuffer.size = 0;
        m_decoder->decode(&inputBuffer);
        renderOutputs();

        m_decoder->stop();
        return true;
    }
    uint32_t getFrameNum()
    {
        return m_frameNum;
    }
    SimplePlayer()
        : m_window(0)
        , m_width(0)
        , m_height(0)
        , m_frameNum(0)
    {
        m_parameters.inputFile.clear();
        m_parameters.outputFile.clear();
        m_parameters.outputFrameNumber = 0;
        m_parameters.dumpToFile = true;
        m_drmFd = -1;
    }
    ~SimplePlayer()
    {
        if (m_nativeDisplay) {
            vaTerminate(m_vaDisplay);
        }
        if (m_window) {
            XDestroyWindow(m_display.get(), m_window);
        }
    }
private:
    void renderOutputs()
    {
        VAStatus status = VA_STATUS_SUCCESS;
        do {
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame)
                break;
            status = vaPutSurface(m_vaDisplay, (VASurfaceID)frame->surface,
                m_window, 0, 0, m_width, m_height, 0, 0, m_width, m_height,
                NULL, 0, 0);
            if (status != VA_STATUS_SUCCESS) {
                ERROR("vaPutSurface return %d", status);
                break;
            }
            m_frameNum++;
        } while (1);
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
        else {
            Display* display = XOpenDisplay(NULL);
            if (!display) {
                ERROR("Failed to XOpenDisplay.\n");
                return false;
            }
            m_display.reset(display, XCloseDisplay);
            m_vaDisplay = vaGetDisplay(m_display.get());
        }
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

    void resizeWindow(int width, int height)
    {
        Display* display = m_display.get();
        if (m_window) {
        //todo, resize window;
        } else {
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
        m_width = width;
        m_height = height;
    }
    SharedPtr<Display> m_display;
    SharedPtr<NativeDisplay> m_nativeDisplay;
    VADisplay m_vaDisplay;
    Window   m_window;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<DecodeInput> m_input;
    int m_width, m_height;
    SimplePlayerParameter m_parameters;
    uint32_t m_frameNum;
    int m_drmFd;
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
    std::cout << "decoded frame number:" << player.getFrameNum() << std::endl;
    return  0;

}

