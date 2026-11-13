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

#include "DmsParser.h"
#include "Logger.h"

#include <string.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

// xdms headers
extern "C" {
#include "cdata.h"
#include "crc_csum.h"
#include "pfile.h"
#include "u_deep.h"
#include "u_heavy.h"
#include "u_init.h"
#include "u_medium.h"
#include "u_quick.h"
#include "u_rle.h"

// Declaration for internal xdms functions we might need if not in headers
// u_init.h declares Init_Decrunchers
}

#define HEADLEN          56
#define THLEN            20
#define TRACK_BUFFER_LEN 32000
#define TEMP_BUFFER_LEN  32000

// Helper to access static/internal functions from xdms if needed,
// but we will reimplement the loop logic to avoid needing `Process_Track` which prints stuff.

namespace adm::core {

// Reimplementation of Unpack_Track logic or similar to avoid linking issues with static functions
// in xdms Actually, Unpack_Track is static in pfile.c, so we can't call it directly. We have to
// copy the Unpack_Track logic here or make it non-static in xdms (which we can't easily do without
// patching). Alternatively, since we only need to unpack Banner and DIZ, we can implement a
// simplified version or copy `Unpack_Track` from `pfile.c`.

uint16_t UnpackTrack(uint8_t* b1, uint8_t* b2, uint16_t pklen2, uint16_t unpklen, uint8_t cmode,
                     uint8_t flags)
{
    if (unpklen > TEMP_BUFFER_LEN)
        return ERR_NOMEMORY; // Safety check

    switch (cmode) {
        case 0:
            /*   No Compression   */
            memcpy(b2, b1, (size_t)unpklen);
            break;
        case 1:
            /*   Simple Compression   */
            if (Unpack_RLE(b1, b2, unpklen))
                return ERR_BADDECR;
            break;
        case 2:
            /*   Quick Compression   */
            if (Unpack_QUICK(b1, b2, pklen2))
                return ERR_BADDECR;
            if (Unpack_RLE(b2, b1, unpklen))
                return ERR_BADDECR;
            memcpy(b2, b1, (size_t)unpklen);
            break;
        case 3:
            /*   Medium Compression   */
            if (Unpack_MEDIUM(b1, b2, pklen2))
                return ERR_BADDECR;
            if (Unpack_RLE(b2, b1, unpklen))
                return ERR_BADDECR;
            memcpy(b2, b1, (size_t)unpklen);
            break;
        case 4:
            /*   Deep Compression   */
            if (Unpack_DEEP(b1, b2, pklen2))
                return ERR_BADDECR;
            if (Unpack_RLE(b2, b1, unpklen))
                return ERR_BADDECR;
            memcpy(b2, b1, (size_t)unpklen);
            break;
        case 5:
        case 6:
            /*   Heavy Compression   */
            if (cmode == 5) {
                /*   Heavy 1   */
                if (Unpack_HEAVY(b1, b2, flags & 7, pklen2))
                    return ERR_BADDECR;
            } else {
                /*   Heavy 2   */
                if (Unpack_HEAVY(b1, b2, flags | 8, pklen2))
                    return ERR_BADDECR;
            }
            if (flags & 4) {
                /*  Unpack with RLE only if this flag is set  */
                if (Unpack_RLE(b2, b1, unpklen))
                    return ERR_BADDECR;
                memcpy(b2, b1, (size_t)unpklen);
            }
            break;
        default:
            return ERR_UNKNMODE;
    }

    if (!(flags & 1))
        Init_Decrunchers();

    return NO_PROBLEM;
}

DmsInfo DmsParser::Parse(const std::string& path)
{
    DmsInfo info;
    info.filename = std::filesystem::path(path).filename().string();

    FILE* fi = fopen(path.c_str(), "rb");
    if (!fi) {
        Logger::Error("Could not open DMS file: {}", path);
        return info;
    }

    // Allocate buffers
    // We need 'text' global buffer for xdms unpackers (defined in cdata.h/cdata.c)
    // It is already allocated if we linked xdms properly?
    // Wait, xdms uses global variables. We must be careful if we are multithreaded (we are not in
    // this context mostly). Application.cpp / DmsConverter.cpp might be using it too. DmsConverter
    // uses Process_File which allocates text. We must allocate it here manually because we are
    // bypassing Process_File.

    // We need to check if 'text' is defined in a library we link to, or if we need to define it.
    // cdata.h says: extern UCHAR *text;
    // We need to allocate it.

    // Safety: Save old text pointer just in case (though it should be null if not in use)
    uint8_t* old_text = text;
    text = (uint8_t*)calloc(TEMP_BUFFER_LEN, 1);

    uint8_t* b1 = (uint8_t*)calloc(TRACK_BUFFER_LEN, 1);
    uint8_t* b2 = (uint8_t*)calloc(TRACK_BUFFER_LEN, 1);

    if (!text || !b1 || !b2) {
        if (text)
            free(text);
        if (b1)
            free(b1);
        if (b2)
            free(b2);
        if (fi)
            fclose(fi);
        text = old_text; // Restore
        return info;
    }

    if (fread(b1, 1, HEADLEN, fi) != HEADLEN) {
        fclose(fi);
        free(text);
        free(b1);
        free(b2);
        text = old_text;
        return info;
    }

    if (b1[0] != 'D' || b1[1] != 'M' || b1[2] != 'S' || b1[3] != '!') {
        fclose(fi);
        free(text);
        free(b1);
        free(b2);
        text = old_text;
        return info;
    }

    info.isValid = true;

    // Parse Header
    uint16_t hcrc = (uint16_t)((b1[HEADLEN - 2] << 8) | b1[HEADLEN - 1]);
    info.headerCrc = hcrc;

    info.generalInfo = (uint16_t)((b1[10] << 8) | b1[11]);
    info.creationDate = (time_t)((((uint32_t)b1[12]) << 24) | (((uint32_t)b1[13]) << 16) |
                                 (((uint32_t)b1[14]) << 8) | (uint32_t)b1[15]);
    info.lowTrack = (uint16_t)((b1[16] << 8) | b1[17]);
    info.highTrack = (uint16_t)((b1[18] << 8) | b1[19]);
    info.packedSize =
        (uint32_t)((((uint32_t)b1[21]) << 16) | (((uint32_t)b1[22]) << 8) | (uint32_t)b1[23]);
    info.unpackedSize =
        (uint32_t)((((uint32_t)b1[25]) << 16) | (((uint32_t)b1[26]) << 8) | (uint32_t)b1[27]);
    info.dmsVersion = (uint16_t)((b1[46] << 8) | b1[47]);
    info.diskType = (uint16_t)((b1[50] << 8) | b1[51]);
    info.compressionMode = (uint16_t)((b1[52] << 8) | b1[53]);
    info.registered = (info.generalInfo & 0x80);

    Init_Decrunchers();

    // Loop Tracks
    while (true) {
        size_t l = fread(b1, 1, THLEN, fi);
        if (l != THLEN)
            break;

        if (b1[0] != 'T' || b1[1] != 'R')
            break;

        // Track Header
        DmsTrackInfo tInfo;
        tInfo.number = (uint16_t)((b1[2] << 8) | b1[3]);
        tInfo.packedLength = (uint16_t)((b1[6] << 8) | b1[7]);
        uint16_t pklen2 = (uint16_t)((b1[8] << 8) | b1[9]);
        tInfo.unpackedLength = (uint16_t)((b1[10] << 8) | b1[11]);
        tInfo.flags = b1[12];
        tInfo.cmode = b1[13];
        tInfo.usum = (uint16_t)((b1[14] << 8) | b1[15]);
        tInfo.dcrc = (uint16_t)((b1[16] << 8) | b1[17]);
        tInfo.hcrc = (uint16_t)((b1[THLEN - 2] << 8) | b1[THLEN - 1]);

        // Validate Track Header CRC? Maybe later.

        // Check for big track
        if (tInfo.packedLength > TRACK_BUFFER_LEN || pklen2 > TRACK_BUFFER_LEN ||
            tInfo.unpackedLength > TRACK_BUFFER_LEN) {
            // Skip or error?
            break;
        }

        if (fread(b1, 1, tInfo.packedLength, fi) != tInfo.packedLength)
            break;

        // Decrypt if needed (Banner/DIZ/FakeBB usually not encrypted unless specified)
        // xdms says: if (pwd && (number != 80)) dms_decrypt...
        // We aren't handling passwords yet. Assume no password or handle later.

        if (tInfo.number == 80) { // FILEID.DIZ
            memset(b2, 0, tInfo.unpackedLength);
            if (UnpackTrack(b1, b2, pklen2, tInfo.unpackedLength, tInfo.cmode, tInfo.flags) ==
                NO_PROBLEM) {
                // Convert buffer to string properly (handling newlines maybe?)
                // xdms uses a print function that handles 0x0A specially?
                // printbandiz replaces 10 (LF) with 0 then prints... so it expects null terminated
                // strings? We will just copy raw bytes to string, filtering nulls if needed.
                std::string s((char*)b2, tInfo.unpackedLength);
                // Cleanup string: xdms might have nulls or special chars
                info.fileIdDiz = s;
            }
        } else if (tInfo.number == 0xFFFF) { // Banner
            memset(b2, 0, tInfo.unpackedLength);
            if (UnpackTrack(b1, b2, pklen2, tInfo.unpackedLength, tInfo.cmode, tInfo.flags) ==
                NO_PROBLEM) {
                std::string s((char*)b2, tInfo.unpackedLength);
                info.banner = s;
            }
        } else {
            info.tracks.push_back(tInfo);
        }
    }

    fclose(fi);
    free(text);
    free(b1);
    free(b2);
    text = old_text;

    return info;
}

} // namespace adm::core
