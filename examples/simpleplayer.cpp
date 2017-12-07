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

#include <va/va.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <va/va_x11.h>
#include <va/va_drm.h>
#include <iostream>
#include <sstream>

#include "common/log.h"
#include "common/lock.h"
#include "vppinputoutput.h"
#include "vppoutputencode.h"
#include "vppinputdecode.h"
#include "vppinputdecodecapi.h"

using namespace YamiMediaCodec;

//static int i_dpwu = 0;
#define OUTPUT_DPWU 1
#define CPPPRINT(...) std::cout << __VA_ARGS__ << std::endl

#define TIME_DURATION(end1, start1) ((end1.tv_sec * 1000 + end1.tv_usec / 1000) - (start1.tv_sec * 1000 + start1.tv_usec / 1000))

#define TIME_MS(time_dd) (time_dd.tv_sec * 1000 + time_dd.tv_usec / 1000)

static uint32_t output_file = 0;
static uint32_t output_all_file = 0;



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
        m_fp = NULL;
        m_getFirst = 0;
        if(2 == argc) {
            output_file = 1;
            output_all_file = 1;
        }else if (3 == argc) {
            output_file = atoi(argv[2]);
        }else{
            printf("usage: \n");
            printf("decode 1 frame, not dump: simpleplayer xxx.264 0\n");
            printf("decode and dump 1 frame: simpleplayer xxx.264 1\n");
            printf("decode and dump all frames: simpleplayer xxx.264\n");
            return false;
        }
        
        m_parameters.inputFile.assign(argv[1]);
        m_input.reset(DecodeInput::create(argv[1]));
        if (!m_input) {
            fprintf(stderr, "failed to open %s", argv[1]);
            return false;
        }

        //init decoder
        m_decoder.reset(createVideoDecoder(m_input->getMimeType()), releaseVideoDecoder);
        if (!m_decoder) {
            fprintf(stderr, "failed create decoder for %s", m_input->getMimeType());
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

        Decode_Status status = m_decoder->start(&configBuffer);
        assert(status == DECODE_SUCCESS);

        VideoDecodeBuffer inputBuffer;
        
        while (m_input->getNextDecodeUnit(inputBuffer)) {
            status = m_decoder->decode(&inputBuffer);
            if (DECODE_FORMAT_CHANGE == status) {
                //drain old buffers
                renderOutputs();
                //m_decoder->getFormatInfo();
                //resizeWindow(formatInfo->width, formatInfo->height);
                //resend the buffer
                status = m_decoder->decode(&inputBuffer);
            }
            if(status == DECODE_SUCCESS) {
                renderOutputs();
            } else {
                ERROR("decode error status = %d", status);
                break;
            }
            if(m_getFirst){
                break;
            }
        }

        if(! m_getFirst){
            inputBuffer.data = NULL;
            inputBuffer.size = 0;
            m_decoder->decode(&inputBuffer);
            renderOutputs();
        }

        m_decoder->stop();
 
        return true;
    }
    SimplePlayer():m_width(0), m_height(0) 
    {
        m_parameters.inputFile.clear();
        m_parameters.outputFile.clear();
        m_parameters.outputFrameNumber = 0;
        m_parameters.dumpToFile = true;
        m_drmFd = -1;
        m_vaDisplay = NULL;
        m_fpOutput = NULL;
    }
    ~SimplePlayer()
    {
        if (m_nativeDisplay) {
            vaTerminate(m_vaDisplay);
        }
        if (output_file)
            if (m_fpOutput)
                fclose(m_fpOutput);
    }
private:
#if (1)
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
#endif

#if (0)
    bool renderOutputs()
    {
        VAStatus status = VA_STATUS_SUCCESS;
        do {
            m_parameters.dumpToFile = true;
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame)
                break;
            printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
            if (m_parameters.dumpToFile){
                if (!m_fpOutput) {
                    if (m_parameters.outputFile.empty()) {
                        std::ostringstream stringStream;
                        stringStream << m_parameters.inputFile.c_str();
                        stringStream << "_NV12_" << m_width << "x" << m_height << ".yuv";
                        m_parameters.outputFile = stringStream.str();
                        printf("dpwu  %s %s %d, m_parameters.outputFile.c_str() = %s ====\n", __FILE__, __FUNCTION__, __LINE__, m_parameters.outputFile.c_str());
                    }
                    printf("dpwu  %s %s %d, m_parameters.outputFile.c_str() = %s ====\n", __FILE__, __FUNCTION__, __LINE__, m_parameters.outputFile.c_str());
                    m_fpOutput = fopen(m_parameters.outputFile.c_str(), "wb");
                    if (!m_fpOutput) {
                        ERROR("fail to open output file: %s", m_parameters.outputFile.c_str());
                        return false;
                    }
                    INFO("output file(%s) is opened.", m_parameters.inputFile.c_str());
                }
                
                printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);

                if (!writeNV12ToFile((VASurfaceID)frame->surface, m_width, m_height))
                    return false;
            } else {
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
#endif
#if (1)
    void renderOutputs()
    {
        do {
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame)
                return ;
            
            m_parameters.dumpToFile = true;
            if (m_parameters.dumpToFile){
                if(output_file){                    
                    if (!m_fpOutput) {
                        if (m_parameters.outputFile.empty()) {
                            std::ostringstream stringStream;
                            stringStream << m_parameters.inputFile.c_str();
                            stringStream << "_NV12_" << m_width << "x" << m_height << ".yuv";
                            m_parameters.outputFile = stringStream.str();
                            printf("dpwu  %s %s %d, m_parameters.outputFile.c_str() = %s ====\n", __FILE__, __FUNCTION__, __LINE__, m_parameters.outputFile.c_str());
                        }
                        printf("dpwu  %s %s %d, m_parameters.outputFile.c_str() = %s ====\n", __FILE__, __FUNCTION__, __LINE__, m_parameters.outputFile.c_str());
                        m_fpOutput = fopen(m_parameters.outputFile.c_str(), "wb");
                        if (!m_fpOutput) {
                            ERROR("fail to open output file: %s", m_parameters.outputFile.c_str());
                            break; //return false;
                        }
                        INFO("output file(%s) is opened.", m_parameters.inputFile.c_str());
                    }
                    
                    if (m_fpOutput){
                        //doIO(m_fpOutput, frame);
                        printf("dpwu  %s %s %d ====\n", __FILE__, __FUNCTION__, __LINE__);
                        writeNV12ToFile((VASurfaceID)frame->surface, frame->crop.width, frame->crop.height);
                        if(! output_all_file){
                            m_getFirst = 1;
                            break;
                        }
                    }
                }
                else{
                    m_getFirst = 1;
                    break;
                }
            }
        } while (1);
    }
#endif

#if (0)
    void renderOutputs()
    {
        do {
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame)
                return ;
            else{
                if(output_file){
                    if (output_file)
                        if(! m_fp){
                            char file_name[128];
                            sprintf (file_name, "dd_sim_nv12_%dx%d.yuv", frame->crop.width, frame->crop.height);
                            fprintf(stderr, "%s %s %d, file_name = %s ====\n", __FILE__, __FUNCTION__, __LINE__, file_name);
                            m_fp = fopen(file_name, "wb");
                        }
                    if (m_fp){
                        doIO(m_fp, frame);
                        if(! output_all_file){
                            m_getFirst = 1;
                            break;
                        }
                    }
                }
                else{
                    m_getFirst = 1;
                    break;
                }
            }
        } while (1);
    }
#endif

    bool createVadisplay()
    {
        m_parameters.dumpToFile = true;
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

    /* l is length in pixel*/
    /* length[] are length in each plane*/
    bool getPlaneResolution(uint32_t fourcc, uint32_t pixelWidth, uint32_t pixelHeight, uint32_t byteWidth[3], uint32_t byteHeight[3],  uint32_t& planes)
    {
        int w = pixelWidth;
        int h = pixelHeight;
        uint32_t* width = byteWidth;
        uint32_t* height = byteHeight;
        //NV12 is special since it  need add one for width[1] when w is odd
        if (fourcc == VA_FOURCC_NV12) {
            width[0] = w;
            height[0] = h;
            width[1] = w + (w & 1);
            height[1] = (h + 1) >> 1;
            planes = 2;
            return true;
        }
        return false;
    }
#if (1)
        bool doIO(FILE* fp, const SharedPtr<VideoFrame>& frame)
        {
            if (!fp || !frame) {
                ERROR("invalid param");
                return false;
            }
            VASurfaceID surface = (VASurfaceID)frame->surface;
            VAImage image;
    
            VAStatus status = vaDeriveImage(m_vaDisplay,surface,&image);
            if (status != VA_STATUS_SUCCESS) {
                ERROR("vaDeriveImage failed = %d", status);
                return false;
            }
            uint32_t byteWidth[3], byteHeight[3], planes;
            //image.width is not equal to frame->crop.width.
            //for supporting VPG Driver, use YV12 to replace I420
            if (!getPlaneResolution(frame->fourcc, frame->crop.width, frame->crop.height, byteWidth, byteHeight, planes)) {
                ERROR("get plane reoslution failed for %x, %dx%d", frame->fourcc, frame->crop.width, frame->crop.height);
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
                    ret = fwrite(ptr, 1, w, fp);
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
#endif

#if (0)
    bool doIO(FILE* fp, const SharedPtr<VideoFrame>& frame)
    {
        if (!fp || !frame) {
            ERROR("invalid param");
            return false;
        }
        VASurfaceID surface = (VASurfaceID)frame->surface;
        VAImage image;

        VAStatus status = vaDeriveImage(m_vaDisplay,surface,&image);
        if (status != VA_STATUS_SUCCESS) {
            ERROR("vaDeriveImage failed = %d", status);
            return false;
        }
        uint32_t byteWidth[3], byteHeight[3], planes;
        //image.width is not equal to frame->crop.width.
        //for supporting VPG Driver, use YV12 to replace I420
        if (!getPlaneResolution(frame->fourcc, frame->crop.width, frame->crop.height, byteWidth, byteHeight, planes)) {
            ERROR("get plane reoslution failed for %x, %dx%d", frame->fourcc, frame->crop.width, frame->crop.height);
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
                ret = fwrite(ptr, 1, w, fp);
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
#endif

    SharedPtr<Display> m_display;
    SharedPtr<NativeDisplay> m_nativeDisplay;
    VADisplay m_vaDisplay;
    Window   m_window;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<DecodeInput> m_input;
    int m_width, m_height;
    int m_getFirst;
    //SharedPtr<DecodeOutput> m_output;
    FILE* m_fp;
    SimplePlayerParameter m_parameters;
    int m_drmFd;
    FILE* m_fpOutput;
    uint32_t m_frameNum;
};

int main(int argc, char** argv)
{
    SimplePlayer player;
    //show_h264();
    
    if (!player.init(argc, argv)) {
        return -1;
    }
    if (!player.run()){
        ERROR("run simple player failed");
        return -1;
    }    
    return  0;

}

