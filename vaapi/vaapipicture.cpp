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

#include "vaapipicture.h"

#include "common/log.h"
#include "VaapiBuffer.h"
#include "vaapidisplay.h"
#include "vaapicontext.h"
#include "VaapiSurface.h"
#include "VaapiUtils.h"
#include <sys/time.h>

#define TIME_DURATION(end1, start1) ((end1.tv_sec * 1000 + end1.tv_usec / 1000) - (start1.tv_sec * 1000 + start1.tv_usec / 1000))
#define TIME_MS(time_dd) (time_dd.tv_sec * 1000 + time_dd.tv_usec / 1000)

#if (1)
static struct timeval time_1, time_2, time_3, time_4;
#endif

namespace YamiMediaCodec{
VaapiPicture::VaapiPicture(const ContextPtr& context,
    const SurfacePtr& surface, int64_t timeStamp)
    : m_display(context->getDisplay())
    , m_context(context)
    , m_surface(surface)
    , m_timeStamp(timeStamp)
{

}

VaapiPicture::VaapiPicture()
    : m_timeStamp(0)
{
}

bool VaapiPicture::render()
{
    if (m_surface->getID() == VA_INVALID_SURFACE) {
        ERROR("bug: no surface to encode");
        return false;
    }

    VAStatus status;
    gettimeofday(&time_1, NULL);
    status = vaBeginPicture(m_display->getID(), m_context->getID(), m_surface->getID());
    if (!checkVaapiStatus(status, "vaBeginPicture()"))
        return false;

    gettimeofday(&time_2, NULL);

    bool ret = doRender();

    gettimeofday(&time_3, NULL);

    status = vaEndPicture(m_display->getID(), m_context->getID());
    if (!checkVaapiStatus(status, "vaEndPicture()"))
        return false;
    
    gettimeofday(&time_4, NULL);
    /*
    printf("dpwu  %s %s %d, duration1 = %ld, duration2 = %ld, duration3 vaEndPicture = %ld ====\n", __FILE__, __FUNCTION__, __LINE__
        , TIME_DURATION(time_2, time_1), TIME_DURATION(time_3, time_2), TIME_DURATION(time_4, time_3));
*/
    
    return ret;
}

bool VaapiPicture::render(BufObjectPtr& buffer)
{
    VAStatus status = VA_STATUS_SUCCESS;
    VABufferID bufferID = VA_INVALID_ID;

    if (!buffer)
        return true;

    buffer->unmap();

    bufferID = buffer->getID();
    if (bufferID == VA_INVALID_ID)
        return false;

    status = vaRenderPicture(m_display->getID(), m_context->getID(), &bufferID, 1);
    if (!checkVaapiStatus(status, "vaRenderPicture failed"))
        return false;

    buffer.reset();             // silently work  arouond for psb
    return true;
}

bool VaapiPicture::render(std::pair <BufObjectPtr,BufObjectPtr> &paramAndData)
{
    return render(paramAndData.first) && render(paramAndData.second);
}

bool VaapiPicture::addObject(std::vector<std::pair<BufObjectPtr,BufObjectPtr> >& objects,
                             const BufObjectPtr & param,
                             const BufObjectPtr & data)
{
    if (!param || !data)
        return false;
    objects.push_back(std::make_pair(param, data));
    return true;
}

bool VaapiPicture::addObject(std::vector < BufObjectPtr >& objects,
                             const BufObjectPtr& object)
{
    if (!object)
        return false;
    objects.push_back(object);
    return true;
}

bool VaapiPicture::sync()
{
    return vaSyncSurface(m_display->getID(), getSurfaceID()) == VA_STATUS_SUCCESS;
}
}
