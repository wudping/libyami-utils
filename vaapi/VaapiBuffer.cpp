/*
 * Copyright (C) 2014 Intel Corporation. All rights reserved.
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

#include "VaapiBuffer.h"

#include "VaapiUtils.h"
#include "vaapicontext.h"
#include "vaapidisplay.h"
#include <sys/time.h>
#define TIME_DURATION(end1, start1) ((end1.tv_sec * 1000 + end1.tv_usec / 1000) - (start1.tv_sec * 1000 + start1.tv_usec / 1000))

#define TIME_MS(time_dd) (time_dd.tv_sec * 1000 + time_dd.tv_usec / 1000)

#if (1)
static struct timeval before_buffer_t, buffer_t;
#endif

namespace YamiMediaCodec {

BufObjectPtr VaapiBuffer::create(const ContextPtr& context,
    VABufferType type,
    uint32_t size,
    const void* data,
    void** mapped)
{
    BufObjectPtr buf;
    if (!size || !context || !context->getDisplay()){
        ERROR("vaapibuffer: can't create buffer");
        return buf;
    }
    DisplayPtr display = context->getDisplay();
    VABufferID id;
    
    gettimeofday(&before_buffer_t, NULL);
    VAStatus status = vaCreateBuffer(display->getID(), context->getID(),
        type, size, 1, (void*)data, &id);
    if (!checkVaapiStatus(status, "vaCreateBuffer"))
        return buf;
    gettimeofday(&buffer_t, NULL);
    //printf("dpwu  %s %s %d, buffer type = %d, createbuffer = %ld ====\n", __FILE__, __FUNCTION__, __LINE__, type, TIME_DURATION(buffer_t, before_buffer_t));
            
    buf.reset(new VaapiBuffer(display, id, size));
    if (mapped) {
        *mapped = buf->map();
        if (!*mapped)
            buf.reset();
    }
    return buf;
}

void* VaapiBuffer::map()
{
    if (!m_data) {
        VAStatus status = vaMapBuffer(m_display->getID(), m_id, &m_data);
        if (!checkVaapiStatus(status, "vaMapBuffer")) {
            m_data = NULL;
        }
    }
    return m_data;
}

void VaapiBuffer::unmap()
{
    if (m_data) {
        checkVaapiStatus(vaUnmapBuffer(m_display->getID(), m_id), "vaUnmapBuffer");
        m_data = NULL;
    }
}

uint32_t VaapiBuffer::getSize()
{
    return m_size;
}

VABufferID VaapiBuffer::getID()
{
    return m_id;
}

VaapiBuffer::VaapiBuffer(const DisplayPtr& display, VABufferID id, uint32_t size)
    : m_display(display)
    , m_id(id)
    , m_data(NULL)
    , m_size(size)
{
}

VaapiBuffer::~VaapiBuffer()
{
    unmap();
    checkVaapiStatus(vaDestroyBuffer(m_display->getID(), m_id), "vaDestroyBuffer");
}
}
