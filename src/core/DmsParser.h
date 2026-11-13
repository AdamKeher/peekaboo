/*
 * Copyright (c) 2026 Admin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace adm::core {

struct DmsTrackInfo
{
    uint16_t number;
    uint16_t packedLength;
    uint16_t unpackedLength;
    uint8_t cmode;
    uint16_t usum;
    uint16_t hcrc;
    uint16_t dcrc;
    uint8_t flags;
};

struct DmsInfo
{
    std::string filename;
    uint16_t dmsVersion;
    bool registered;
    time_t creationDate;
    uint16_t lowTrack;
    uint16_t highTrack;
    uint32_t packedSize;
    uint32_t unpackedSize;
    uint16_t diskType;
    uint16_t compressionMode;
    uint16_t generalInfo;
    uint16_t headerCrc;

    std::vector<DmsTrackInfo> tracks;
    std::string banner;
    std::string fileIdDiz;

    bool isValid = false;
};

class DmsParser
{
public:
    static DmsInfo Parse(const std::string& path);
};

} // namespace adm::core
