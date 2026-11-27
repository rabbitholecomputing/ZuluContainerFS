/**
 * ZuluContainerFS - Copyright (c) 2024 Rabbit Hole Computing™
 *
 * Designed for ZuluIDE and ZuluSCSI
 * 
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <cstddef>
#include <cstring>
#include <algorithm>
#include "ZCUtil.h"
#include "ZCFsFile.h"

using namespace ZuluContainerFs;
// The Microsoft VHD standard's footer had two different length depending
// on which version was being used
inline constexpr uint16_t VHD_FOOTER_LEN_POST_2004 {512};
inline constexpr uint16_t VHD_FOOTER_LEN_PRE_2004 {511};
inline constexpr char VHD_COOKIE[] {"conectix"};


ZCFsFile::ZCFsFile()
    : FsFile()
    , m_container_format(Container::None)
    , m_chs({})
    , m_image_size_bytes(0)
{
}

bool ZCFsFile::open(const char *path, oflag_t oflag)
{
    FsFile::open(path, oflag);
    return openCheck();
}
bool ZCFsFile::open(FsBaseFile *dir, const char *path, oflag_t oflag)
{
    FsFile::open(dir, path, oflag);
    return openCheck();

}
bool ZCFsFile::open(FsBaseFile *dir, uint32_t index, oflag_t oflag)
{
    FsFile::open(dir, index, oflag);
    return openCheck();

}
bool ZCFsFile::open(FsVolume *vol, const char *path, oflag_t oflag)
{
    FsFile::open(vol, path, oflag);
    return openCheck();
}

bool ZCFsFile::close()
{
    reset();
    return FsFile::close();
}

bool ZCFsFile::contiguousRange(uint32_t *bgnSector, uint32_t *endSector)
{
    if (!isOpen())
        return false;

    uint32_t first_cluster = FsBaseFile::firstCluster();
    uint64_t data_length = FsBaseFile::dataLength();
    if (!m_contig_checked
        || m_contig_first_cluster != first_cluster
        || m_contig_data_length != data_length)
    {
        m_contig_bgn = 0;
        m_contig_end = 0;
        m_contig_result = FsBaseFile::contiguousRange(&m_contig_bgn, &m_contig_end);
        m_contig_first_cluster = first_cluster;
        m_contig_data_length = data_length;
        m_contig_checked = true;
    }

    if (!m_contig_result)
        return false;

    if (bgnSector)
        *bgnSector = m_contig_bgn;
    if (endSector)
        *endSector = m_contig_end;
    return true;
}

void ZCFsFile::setVhdEnabled(bool enable)
{
    m_vhd_enabled = enable;
}

bool ZCFsFile::isVhdEnabled() const
{
    return m_vhd_enabled;
}

uint64_t ZCFsFile::size() const
{
    switch (m_container_format)
    {
    case Container::Vhd:
        return m_image_size_bytes;
    default:
        return FsBaseFile::size();
    }
}

ZCFsFile &ZCFsFile::operator=(FsFile && from)
{
    move(&from);
    verifyAndInit();
    return *this;
}

Container ZCFsFile::getContainerFormat() const
{
    return m_container_format;
}


const char* ZCFsFile::getContainerNameCstr() const
{
    switch (m_container_format)
    {
    case Container::None:
        return "none";
    case Container::Vhd:
        return "vhd";
    }
    return "unknown";
}

bool ZCFsFile::isUnsupportedContainerType() const
{
    return !isOpen() && (m_container_format != Container::None);
}

bool ZCFsFile::setCHS(uint16_t &cylinders, uint8_t &heads, uint8_t &sectors)
{
    if (m_container_format != Container::Vhd)
        return false;
    if(m_chs.cylinders == 0 && m_chs.heads == 0 && m_chs.sectors == 0)
        return false;

    cylinders = m_chs.cylinders;
    heads = m_chs.heads;
    sectors = m_chs.sectors;
    return true;

}

bool ZCFsFile::openCheck()
{
    verifyAndInit();
    return isOpen();
}

bool ZCFsFile::verifyAndInit()
{
    reset();
    if (isOpen() && isFile())
    {
        // The caller enables the container formats to check, so plain
        // images are not searched for container metadata
        if (m_vhd_enabled && verifyAndInitVhd())
            return true;
    }
    // Container was found, but container format type is unsupported
    // Reset everything but the format for isUnsupportedType checking
    if (m_container_format != Container::None)
    {
        m_image_size_bytes = 0;
        m_chs = {};
        FsFile::close();
    }
    return false;
}

bool ZCFsFile::verifyAndInitVhd()
{
    if (FsBaseFile::size() < VHD_FOOTER_LEN_POST_2004)
        return false;

    // Walk the cluster chain once up front. On a contiguous FAT file this
    // lets SdFat seek straight to the footer instead of following the
    // chain, and the result is cached for the caller's own contiguity check.
    contiguousRange(nullptr, nullptr);

    // The last 512 bytes hold either footer: a post 2004 footer starts at
    // offset 0 and a pre 2004 footer starts at offset 1.
    // They are read in small pieces because this runs deep in the firmware's
    // image open call chain, where a 512 byte stack buffer overflowed the stack.
    if (!seekEnd(-static_cast<int64_t>(VHD_FOOTER_LEN_POST_2004)))
        return false;

    // head holds the VhdFooter fields of both footer versions
    constexpr size_t PRE_2004_OFFSET {VHD_FOOTER_LEN_POST_2004 - VHD_FOOTER_LEN_PRE_2004};
    uint8_t head[sizeof(VhdFooter) + PRE_2004_OFFSET];
    bool read_ok = read(head, sizeof(head)) == static_cast<int>(sizeof(head));

    // The rest is the reserved area of both footer versions, it only adds to the checksums
    uint32_t tail_sum {0};
    uint8_t chunk[64];
    size_t remaining = VHD_FOOTER_LEN_POST_2004 - sizeof(head);
    while (read_ok && remaining > 0)
    {
        size_t n = std::min(remaining, sizeof(chunk));
        read_ok = read(chunk, n) == static_cast<int>(n);
        for (size_t i = 0; i < n; i++)
            tail_sum += chunk[i];
        remaining -= n;
    }
    seekSet(0);
    if (!read_ok)
        return false;

    // Per the VHD spec the checksum covers the whole footer, excluding the checksum field
    uint32_t post_2004_sum = vhdFooterByteSum(head, sizeof(VhdFooter)) + head[sizeof(VhdFooter)] + tail_sum;
    if (initVhdFooter(head, VHD_FOOTER_LEN_POST_2004, ~post_2004_sum))
        return true;
    // A valid footer of an unsupported type was found
    if (m_container_format != Container::None)
        return false;

    uint32_t pre_2004_sum = vhdFooterByteSum(head + PRE_2004_OFFSET, sizeof(VhdFooter)) + tail_sum;
    return initVhdFooter(head + PRE_2004_OFFSET, VHD_FOOTER_LEN_PRE_2004, ~pre_2004_sum);
}

bool ZCFsFile::initVhdFooter(const uint8_t *footer_data, size_t footer_len, uint32_t calced_checksum)
{
    if (memcmp(footer_data, VHD_COOKIE, sizeof(VhdFooter::cookie)) != 0)
        return false;

    // VhdFooter is packed, so its fields can be read from an unaligned buffer
    auto *footer = reinterpret_cast<const VhdFooter*>(footer_data);

    if (calced_checksum != swapIntEndian(footer->checksum))
    {
        return false;
    }

    m_container_format = Container::Vhd;

    if (swapIntEndian(footer->disk_type) != 2)
    {
        return false;
    }

    m_chs.cylinders = swapIntEndian(footer->cylinders);
    m_chs.heads = swapIntEndian(footer->heads);
    m_chs.sectors = swapIntEndian(footer->sectors_per_track);

    // use image size is smallest, data payload or the current size in the footer
    // they should be equal
    auto file_size = FsBaseFile::size();
    auto adjusted_file_size = file_size > footer_len ? (file_size - footer_len) : 0;
    m_image_size_bytes = std::min(swapIntEndian(footer->current_size), adjusted_file_size);
    return true;
}


uint32_t ZCFsFile::vhdFooterByteSum(const uint8_t *footer_data, size_t len)
{
    uint32_t sum {0};
    for (size_t i = 0; i < len; i++)
    {
        // skip checksum data
        if (i >= offsetof(VhdFooter, checksum) && i < offsetof(VhdFooter, checksum) + sizeof(uint32_t))
            continue;

        sum += footer_data[i];
    }
    return sum;
}

void ZCFsFile::reset()
{
    m_chs = {};
    m_container_format = Container::None;
    m_image_size_bytes = 0;
    m_contig_checked = false;
    m_contig_result = false;
    m_contig_bgn = 0;
    m_contig_end = 0;
    m_contig_first_cluster = 0;
    m_contig_data_length = 0;
}