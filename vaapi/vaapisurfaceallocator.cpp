/*
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
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

#include "common/log.h"
#include "vaapi/vaapisurfaceallocator.h"
#include "vaapi/VaapiUtils.h"
#include <vector>
#include <sys/time.h>

#define TIME_DURATION(end1, start1) ((end1.tv_sec * 1000 + end1.tv_usec / 1000) - (start1.tv_sec * 1000 + start1.tv_usec / 1000))
#define TIME_MS(time_x) (time_x.tv_sec * 1000 + time_x.tv_usec / 1000)

struct timeval before_create_surface, create_surface;

namespace YamiMediaCodec{

VaapiSurfaceAllocator::VaapiSurfaceAllocator(VADisplay display, uint32_t extraSize)
    :m_display(display),
    m_extraSize(extraSize)
{
}

YamiStatus VaapiSurfaceAllocator::doAlloc(SurfaceAllocParams* params)
{
    if (!params)
        return YAMI_INVALID_PARAM;
    uint32_t size = params->size;
    uint32_t width = params->width;
    uint32_t height = params->height;
    if (!width || !height || !size)
        return YAMI_INVALID_PARAM;
    uint32_t rtFormat = getRtFormat(params->fourcc);
    if (!rtFormat) {
        ERROR("unsupported format %x", params->fourcc);
        return YAMI_UNSUPPORTED;
    }

    size += m_extraSize;
    
    printf("dpwu  %s %s %d, size = %d ====\n", __FILE__, __FUNCTION__, __LINE__, size);

    std::vector<VASurfaceID> v(size);
    VASurfaceAttrib attrib;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = params->fourcc;
    
    gettimeofday(&before_create_surface, NULL);
    VAStatus status = vaCreateSurfaces(m_display,
        rtFormat, width, height,
        &v[0], size, &attrib, 1);
    if (!checkVaapiStatus(status, "vaCreateSurfaces"))
        return YAMI_OUT_MEMORY;
    params->surfaces = new intptr_t[size];
    for (uint32_t i = 0; i < size; i++) {
        params->surfaces[i] = (intptr_t)v[i];
    }
    gettimeofday(&create_surface, NULL);
    //fprintf(stderr, "%s %s %d, before = %ld, surface = %ld, time_cost = %ld, num = %d ====\n", __FILE__, __FUNCTION__, __LINE__, TIME_MS(before_create_surface), TIME_MS(create_surface), TIME_DURATION(create_surface, before_create_surface), size);

    params->size = size;
    return YAMI_SUCCESS;
}

YamiStatus VaapiSurfaceAllocator::doFree(SurfaceAllocParams* params)
{
    if (!params || !params->size || !params->surfaces)
        return YAMI_INVALID_PARAM;
    uint32_t size = params->size;
    std::vector<VASurfaceID> v(size);
    for (uint32_t i = 0; i < size; i++) {
        v[i] = (VASurfaceID)params->surfaces[i];
    }
    checkVaapiStatus(vaDestroySurfaces(m_display, &v[0], size), "vaDestroySurfaces");
    delete[] params->surfaces;
    return YAMI_SUCCESS;
}

void VaapiSurfaceAllocator::doUnref()
{
    delete this;
}

} //YamiMediaCodec
