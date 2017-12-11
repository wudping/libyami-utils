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

#include "vaapi/vaapicontext.h"

#include "common/log.h"
#include "common/common_def.h"
#include "vaapi/vaapidisplay.h"
#include "vaapi/VaapiUtils.h"
#include <algorithm>
#include <vector>


#include <sys/time.h>

#define TIME_DURATION(end1, start1) ((end1.tv_sec * 1000 + end1.tv_usec / 1000) - (start1.tv_sec * 1000 + start1.tv_usec / 1000))
#define TIME_MS(time_x) (time_x.tv_sec * 1000 + time_x.tv_usec / 1000)

#if (1)
static struct timeval befor_context_t, context_t, before_config_t, config_t;
#endif

namespace YamiMediaCodec{
using std::vector;
static const VAProfile h264ProfileList[] = {VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High};

//Driver may declare support higher profile but don't support lower profile.
//In this case, higher profile should be created.
//Note: creating higher va profile won't affect the detail encoding/decoding process of libva driver.
static bool checkH264Profile(VAProfile& profile, vector<VAProfile>& profileList)
{
    vector<VAProfile>h264Profiles(h264ProfileList, h264ProfileList + N_ELEMENTS(h264ProfileList));
    vector<VAProfile>::iterator start = std::find(h264Profiles.begin(), h264Profiles.end(), profile);
    vector<VAProfile>::iterator result = std::find_first_of(profileList.begin(), profileList.end(), start, h264Profiles.end());

    if (result != profileList.end()){
        profile = *result;
    } else{
        // VAProfileH264Baseline is super profile for VAProfileH264ConstrainedBaseline
        // old i965 driver incorrectly claims supporting VAProfileH264Baseline, but not VAProfileH264ConstrainedBaseline
        if (profile == VAProfileH264ConstrainedBaseline && std::count(profileList.begin(), profileList.end(), VAProfileH264Baseline))
            profile = VAProfileH264Baseline;
        else
            return false;
    }

    return true;
}

static bool checkProfileCompatible(const DisplayPtr& display, VAProfile& profile)
{
    int maxNumProfiles, numProfiles;
    VAStatus vaStatus;

    maxNumProfiles = vaMaxNumProfiles(display->getID());

    vector<VAProfile> profileList(maxNumProfiles);

    vaStatus = vaQueryConfigProfiles(display->getID(), &profileList[0], &numProfiles);
    if (!checkVaapiStatus(vaStatus, "vaQueryConfigProfiles"))
        return false;
    assert((numProfiles > 0) && (numProfiles <= maxNumProfiles));

    profileList.resize(numProfiles);

    if (profile == VAProfileH264ConstrainedBaseline || profile == VAProfileH264Main){
        if (!checkH264Profile(profile, profileList))
            return false;
    } else if (!std::count(profileList.begin(), profileList.end(), profile))
        return false;

    return true;
}

ConfigPtr VaapiConfig::create(const DisplayPtr& display,
                                     VAProfile profile, VAEntrypoint entry,
                                     VAConfigAttrib *attribList, int numAttribs)
{
    ConfigPtr ret;
    if (!display)
        return ret;
    VAStatus vaStatus;
    VAConfigID config;

    if (!checkProfileCompatible(display, profile)){
        ERROR("Unsupport profile");
        return ret;
    }

    gettimeofday(&before_config_t, NULL);
    vaStatus = vaCreateConfig(display->getID(), profile, entry, attribList, numAttribs, &config);
    gettimeofday(&config_t, NULL);
    //printf("dpwu  %s %s %d, config_duration = %ld ====\n", __FILE__, __FUNCTION__, __LINE__, TIME_DURATION(before_config_t, config_t));
    

    if (!checkVaapiStatus(vaStatus, "vaCreateConfig "))
        return ret;
    ret.reset(new VaapiConfig(display, config));
    return ret;
}

VaapiConfig::VaapiConfig(const DisplayPtr& display, VAConfigID config)
:m_display(display), m_config(config)
{
}

VaapiConfig::~VaapiConfig()
{
    vaDestroyConfig(m_display->getID(), m_config);
}

ContextPtr VaapiContext::create(const ConfigPtr& config,
                                       int width,int height,int flag,
                                       VASurfaceID *render_targets,
                                       int num_render_targets)
{
    ContextPtr ret;
    if (!config) {
        ERROR("No display");
        return ret;
    }
    VAStatus vaStatus;
    VAContextID context;
    
    gettimeofday(&befor_context_t, NULL);
    vaStatus = vaCreateContext(config->m_display->getID(), config->m_config,
                               width, height, flag,
                               render_targets, num_render_targets, &context);
    gettimeofday(&context_t, NULL);
    //printf("dpwu  %s %s %d, context_duration = %ld ====\n", __FILE__, __FUNCTION__, __LINE__, TIME_DURATION(context_t, befor_context_t));
    
    if (!checkVaapiStatus(vaStatus, "vaCreateContext "))
        return ret;
    ret.reset(new VaapiContext(config, context));
    return ret;
}

VaapiContext::VaapiContext(const ConfigPtr& config, VAContextID context)
:m_config(config), m_context(context)
{
}

VaapiContext::~VaapiContext()
{
    vaDestroyContext(m_config->m_display->getID(), m_context);
}
}
