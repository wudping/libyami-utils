/*
 * Copyright (C) 2016 Intel Corporation. All rights reserved.
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
#ifndef oclpostprocess_mosaic_h
#define oclpostprocess_mosaic_h

#include "VideoCommonDefs.h"
#include "oclpostprocess_base.h"

namespace YamiMediaCodec {

/**
 * \class OclPostProcessMosaic
 * \brief OpenCL based mosaic filter
 */
class OclPostProcessMosaic : public OclPostProcessBase {
public:
    virtual YamiStatus process(const SharedPtr<VideoFrame>& src,
        const SharedPtr<VideoFrame>& dst);

    virtual YamiStatus setParameters(VppParamType type, void* vppParam);

    explicit OclPostProcessMosaic()
        : m_blockSize(32)
        , m_kernelMosaic(NULL)
    {
    }

private:
    virtual bool prepareKernels();

    static const bool s_registered; // VaapiPostProcessFactory registration result
    int m_blockSize;
    cl_kernel m_kernelMosaic;
};
}
#endif //oclpostprocess_mosaic_h
