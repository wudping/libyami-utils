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
#include <stdint.h>

namespace YamiMediaCodec{

class NalReader
{
public:
    NalReader(const uint8_t* buf, int32_t size, uint32_t nalLengthSize = 0, bool asWhole = false);

    /*nal point to the nal unit without start code or length bytes*/
    bool read(const uint8_t*& nal, int32_t& nalSize);

private:
    const uint8_t* searchNalStart();
    const uint8_t* searchStartCode();
    const uint8_t* m_begin;
    const uint8_t* m_next;
    const uint8_t* m_end;
    bool  m_asWhole;
    uint32_t m_nalLengthSize;
    uint32_t m_size;
};

} //namespace YamiMediaCodec
