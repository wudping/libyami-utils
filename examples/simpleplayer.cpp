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
#include "common/log.h"
#include "common/lock.h"
#include "vppinputoutput.h"
#include "vppoutputencode.h"
#include "vppinputdecode.h"
#include "vppinputdecodecapi.h"

using namespace YamiMediaCodec;

#define OUTPUT_DPWU 1

#if (OUTPUT_DPWU)

#ifndef N_ELEMENTS
#define N_ELEMENTS(array) (sizeof(array)/sizeof(array[0]))
#endif

struct ResolutionEntry {
    uint32_t fourcc;
    uint32_t planes;
    //multiple to half width
    //if it equals 1, you need divide width with 2
    //if it equals 4, you need multiple width with 2
    uint32_t widthMultiple[3];
    uint32_t heightMultiple[3];
};

const static ResolutionEntry resolutionEntrys[] = {
    { VA_FOURCC_I420, 3, { 2, 1, 1 }, { 2, 1, 1 } },
    { VA_FOURCC_YV12, 3, { 2, 1, 1 }, { 2, 1, 1 } },
    { VA_FOURCC_IMC3, 3, { 2, 1, 1 }, { 2, 1, 1 } },
    { VA_FOURCC_422H, 3, { 2, 1, 1 }, { 2, 2, 2 } },
    { VA_FOURCC_422V, 3, { 2, 2, 2 }, { 2, 1, 1 } },
    { VA_FOURCC_444P, 3, { 2, 2, 2 }, { 2, 2, 2 } },
    { VA_FOURCC_YUY2, 1, { 4 }, { 2 } },
    { VA_FOURCC_UYVY, 1, { 4 }, { 2 } },
    { VA_FOURCC_RGBX, 1, { 8 }, { 2 } },
    { VA_FOURCC_RGBA, 1, { 8 }, { 2 } },
    { VA_FOURCC_BGRX, 1, { 8 }, { 2 } },
    { VA_FOURCC_BGRA, 1, { 8 }, { 2 } },
};

#endif


#if (1)
void VADisplayDeleter_dpwu(VADisplay* display)
{
    vaTerminate(*display);
    delete display;
}

SharedPtr<VADisplay> createVADisplay_dpwu()
{
    SharedPtr<VADisplay> display;
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) {
        ERROR("can't open /dev/dri/renderD128, try to /dev/dri/card0");
        fd = open("/dev/dri/card0", O_RDWR);
    }
    if (fd < 0) {
        ERROR("can't open drm device");
        return display;
    }
    VADisplay vadisplay = vaGetDisplayDRM(fd);
    int majorVersion, minorVersion;
    VAStatus vaStatus = vaInitialize(vadisplay, &majorVersion, &minorVersion);
    if (vaStatus != VA_STATUS_SUCCESS) {
        ERROR("va init failed, status =  %d", vaStatus);
        close(fd);
        return display;
    }
    //display.reset(new VADisplay(vadisplay), VADisplayDeleter_dpwu(fd));
    display.reset(new VADisplay(vadisplay));
    return display;
}
#endif

class SimplePlayer
{
public:
    bool init(int argc, char** argv)
    {
        m_fp = NULL;
        m_getFirst = 0;
        if (argc != 2) {
            printf("usage: simpleplayer xxx.264\n");
            return false;
        }
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

        m_fp = fopen("dd_sim.yuv", "wb");
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
        while (1) {
            m_input->getNextDecodeUnit(inputBuffer);
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
                return true;
            }
        }
        m_decoder->stop();
        return true;
    }
    SimplePlayer():m_width(0), m_height(0) {}
    ~SimplePlayer()
    {
        if (m_nativeDisplay) {
            vaTerminate(m_vaDisplay);
        }
        if (m_fp)
            fclose(m_fp);
    }
private:
    #if (OUTPUT_DPWU)
    void renderOutputs()
    {
        SharedPtr<VideoFrame> frame = m_decoder->getOutput();
        if (!frame)
            return ;
        else{
            m_getFirst = 1;
            doIO(m_fp, frame);
            return ;
        }
    }
    #else
    void renderOutputs()
    {
        do {
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame){
                break;
            }
            else{
                m_getFirst = 1;
                break;
            }
        } while (1);
    }
    #endif
    
    bool initDisplay()
    {        
        m_vaDisplayPtr = createVADisplay_dpwu();
        m_vaDisplay = m_vaDisplayPtr.get();
        m_nativeDisplay.reset(new NativeDisplay);
        m_nativeDisplay->type = NATIVE_DISPLAY_VA;
        m_nativeDisplay->handle = (intptr_t)*m_vaDisplayPtr;
        return true;
    }

#if (OUTPUT_DPWU)
    /* l is length in pixel*/
    /* length[] are length in each plane*/
    static void getPlaneLength(uint32_t l, uint32_t plane, const uint32_t multiple[3], uint32_t length[3])
    {
        for (uint32_t i = 0; i < plane; i++) {
            length[i] = (l * multiple[i] + 1) >> 1;
        }
    }
    
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
        for (size_t i = 0; i < N_ELEMENTS(resolutionEntrys); i++) {
            const ResolutionEntry& e = resolutionEntrys[i];
            if (e.fourcc == fourcc) {
                planes = e.planes;
                getPlaneLength(pixelWidth, planes, e.widthMultiple, width);
                getPlaneLength(pixelHeight, planes, e.heightMultiple, height);
                return true;
            }
        }
        ERROR("do not support this format, fourcc %.4s", (char*)&fourcc);
        planes = 0;
        return false;
    }

    bool doIO(FILE* fp, const SharedPtr<VideoFrame>& frame)
    {
        if (!fp || !frame) {
            ERROR("invalid param");
            return false;
        }
        VASurfaceID surface = (VASurfaceID)frame->surface;
        VAImage image;

        VAStatus status = vaDeriveImage(*m_vaDisplayPtr,surface,&image);
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
        status = vaMapBuffer(*m_vaDisplayPtr, image.buf, (void**)&buf);
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
        vaUnmapBuffer(*m_vaDisplayPtr, image.buf);
        vaDestroyImage(*m_vaDisplayPtr, image.image_id);
        return ret;

    }


#endif

    SharedPtr<NativeDisplay> m_nativeDisplay;
    VADisplay m_vaDisplay;
    SharedPtr<VADisplay> m_vaDisplayPtr;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<DecodeInput> m_input;
    int m_width, m_height;
    int m_getFirst;
    //SharedPtr<DecodeOutput> m_output;
    FILE* m_fp;
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
    printf("play file done\n");
    return  0;

}

