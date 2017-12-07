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
#include <getopt.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <iostream>
#include <sstream>
#include <string>

using namespace YamiMediaCodec;
using std::string;

typedef struct SimplePlayerParameter {
    string inputFile;
    string outputFile;
    uint32_t outputFrameNumber;
    bool dumpToFile;
} SimplePlayerParameter;

class SimplePlayer
{
public:
    bool init(int argc, char** argv)
    {
        if (!processCmdLine(argc, argv))
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
                if (!renderOutputs()) {
                    return false;
                }
                const VideoFormatInfo *formatInfo = m_decoder->getFormatInfo();
                if (!m_parameters.dumpToFile)
                    resizeWindow(formatInfo->width, formatInfo->height);

                m_width = formatInfo->width;
                m_height = formatInfo->height;
                //resend the buffer
                status = m_decoder->decode(&inputBuffer);
            }
            if(status == DECODE_SUCCESS) {
                if (!renderOutputs())
                    return false;
            } else {
                ERROR("decode error status = %d", status);
                return false;
            }
            if (m_parameters.outputFrameNumber && m_frameNum >= m_parameters.outputFrameNumber)
                break;
        }
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
        , m_fpOutput(NULL)
        , m_frameNum(0)
    {
        m_parameters.inputFile.clear();
        m_parameters.outputFile.clear();
        m_parameters.outputFrameNumber = 0;
        m_parameters.dumpToFile = true;
        m_vaDisplay = NULL;
    }
    ~SimplePlayer()
    {
        if (m_nativeDisplay) {
            vaTerminate(m_vaDisplay);
        }
        if (m_window) {
            XDestroyWindow(m_display.get(), m_window);
        }
        if (m_fpOutput) {
            fclose(m_fpOutput);
            m_fpOutput = NULL;
        }
    }

private:
    bool renderOutputs()
    {
        VAStatus status = VA_STATUS_SUCCESS;
        do {
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame) {
                return true;
            }
            if (m_parameters.dumpToFile) {
                if (!m_fpOutput) {
                    if (m_parameters.outputFile.empty()) {
                        std::ostringstream stringStream;
                        stringStream << m_parameters.inputFile.c_str();
                        stringStream << "_NV12_" << m_width << "x" << m_height << ".yuv";
                        m_parameters.outputFile = stringStream.str();
                    }
                    m_fpOutput = fopen(m_parameters.outputFile.c_str(), "wb");
                    if (!m_fpOutput) {
                        ERROR("fail to open output file: %s", m_parameters.outputFile.c_str());
                        return false;
                    }
                    INFO("output file(%s) is opened.", m_parameters.inputFile.c_str());
                }

                if (!writeNV12ToFile((VASurfaceID)frame->surface, m_width, m_height))
                    return false;
            }
            else {
                status = vaPutSurface(m_vaDisplay, (VASurfaceID)frame->surface,
                    m_window, 0, 0, m_width, m_height, 0, 0, m_width, m_height,
                    NULL, 0, 0);
                if (status != VA_STATUS_SUCCESS) {
                    ERROR("vaPutSurface return %d", status);
                    break;
                }
            }
            m_frameNum++;
        } while (m_parameters.outputFrameNumber && m_frameNum < m_parameters.outputFrameNumber);

        return true;
    }

    bool writeNV12ToFile(VASurfaceID surface, int w, int h)
    {
        if (!m_fpOutput) {
            ERROR("No file domain for NV12 file writing.\n");
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
                //ret = m_io(ptr, w, fp);
                ret = fwrite(ptr, 1, w, m_fpOutput);
                if (!ret)
                    goto out;
                ptr += image.pitches[i];
            }
        }
    out:
        vaUnmapBuffer(m_vaDisplay, image.buf);
        vaDestroyImage(m_vaDisplay, image.image_id);
        return ret;
    }

    bool createVadisplay()
    {
        if (m_parameters.dumpToFile) {
            m_drmFd = open("/dev/dri/renderD128", O_RDWR);
            if (m_drmFd < 0) {
                printf("can't open /dev/dri/renderD128, try to /dev/dri/card0");
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
    }

    void printHelp(const char* app)
    {
        printf("%s -i input.264 -m 0 \n", app);
        printf("   -i media file to decode\n");
        printf("   -o dumped output file\n");
        printf("   -n specify how many frames to be decoded\n");
        printf("   -m <render mode>\n");
        printf("      0: dump video frame to file [*]\n");
        printf("      1: render to X window [*]\n");
    }

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

    bool processCmdLine(int argc, char** argv)
    {
        char opt;
        while ((opt = getopt(argc, argv, "h:i:o:n:m:?")) != -1) {
            switch (opt) {
            case 'h':
            case '?':
                printHelp(argv[0]);
                return false;
            case 'i':
                m_parameters.inputFile.assign(optarg);
                break;
            case 'o':
                m_parameters.outputFile.assign(optarg);
                break;
            case 'n':
                m_parameters.outputFrameNumber = atoi(optarg);
                break;
            case 'm':
                m_parameters.dumpToFile = !atoi(optarg);
                break;
            default:
                printHelp(argv[0]);
                return false;
            }
        }

        if (optind < argc) {
            int indexOpt = optind;
            printf("unrecognized option: ");
            while (indexOpt < argc)
                printf("%s ", argv[indexOpt++]);
            printf("\n");
            return false;
        }

        if (m_parameters.inputFile.empty()) {
            printHelp(argv[0]);
            ERROR("no input file.");
            return false;
        }
        return true;
    }

    SharedPtr<Display> m_display;
    SharedPtr<NativeDisplay> m_nativeDisplay;
    VADisplay m_vaDisplay;
    Window   m_window;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<DecodeInput> m_input;
    int m_width, m_height;
    SimplePlayerParameter m_parameters;
    int m_drmFd;
    FILE* m_fpOutput;
    uint32_t m_frameNum;
};

int main(int argc, char** argv)
{
    SimplePlayer player;
    if (!player.init(argc, argv)) {
        ERROR("init player failed.");
        return -1;
    }
    if (!player.run()){
        ERROR("run simple player failed.");
        return -1;
    }
    printf("play file done, output frame number: %d\n", player.getFrameNum());
    return  0;
}

