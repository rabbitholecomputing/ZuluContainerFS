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


// All calls like open and close must be made on ZCFsFile class's object
// as there is no way to override FsFile calls

#pragma once

#include <FsLib/FsFile.h>

namespace ZuluContainerFs 
{
    enum class Container 
    {
        None,
        Vhd
    };

    struct __attribute__((packed)) VhdFooter {
        char cookie[8];
        uint32_t features;
        uint32_t file_format_version;
        uint64_t data_offset;
        uint32_t time_stamp;
        uint32_t creator_app;
        uint32_t creator_version;
        uint32_t creator_host_os;
        uint64_t original_size;
        uint64_t current_size;
        uint16_t cylinders;
        uint8_t  heads;
        uint8_t  sectors_per_track;
        uint32_t disk_type;
        uint32_t checksum;
        char unique_id[16];
        uint8_t save_state;
    };

    struct CHS
    {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    };

    class ZCFsFile : public FsFile 
    {
    public:
        ZCFsFile();

        // Replacing FsFile/ FsFileBase calls
        bool open(const char *path, oflag_t oflag=0X00);
        bool open(FsBaseFile *dir, const char *path, oflag_t oflag=0X00);
        bool open(FsBaseFile *dir, uint32_t index, oflag_t oflag=0X00);
        bool open(FsVolume *vol, const char *path, oflag_t oflag=0X00);
        bool close();
        uint64_t size() const;
        ZCFsFile &operator=(FsFile &&);

        /**
         * Same as FsBaseFile::contiguousRange(), but the result is cached while
         * the file stays open. On FAT16/32 SdFat finds the range by walking the
         * file's whole cluster chain through a one sector FAT cache, which for a
         * multi-GB image is thousands of SD card reads. The first call walks the
         * chain once (and on a contiguous FAT file that also lets SdFat seek
         * without walking the chain), later calls return the cached result as
         * long as the file's first cluster and allocated length are unchanged.
         * \param bgnSector set to the first sector of the contiguous range, may be nullptr
         * \param endSector set to the last sector of the contiguous range, may be nullptr
         * \returns true if the file is contiguous
         */
        bool contiguousRange(uint32_t *bgnSector, uint32_t *endSector);

        /**
         * Enable or disable checking for a Microsoft VHD container when a file is opened.
         * Disabled by default, the caller decides which files may be VHD containers,
         * for example by the file extension, and enables it before opening those.
         * The setting stays until changed, it is not reset by open() or close().
         * \param enable true to check for a VHD footer on open
         */
        void setVhdEnabled(bool enable);

        /**
         * \returns true if opening a file checks for a VHD container
         */
        bool isVhdEnabled() const;

        // Calls unique to this class
        /**
         * Get the current container format
         * \returns the format of the container as a class enum Container value
         */

        virtual Container getContainerFormat() const;
        /**
         * Get the C string name for the container type
         * \returns a C string name of the container type 
         */
        virtual const char* getContainerNameCstr() const;

        /**
         * Determine if the Container iamge type is not supported. Should generally be checked when an open fails
         * Currently only full image allocation is supported
         * \return true if the image is a container, but the type of the container format is unsupported
         *         false if the image isn't a container or the container is supported
         */
        virtual bool isUnsupportedContainerType() const;

        /**
         * Set the parameters with CHS from the metadata from the container
         * \param cylinders set the cylinders
         * \param heads set the heads
         * \param sectors set the sectors per track
         * \returns true if CHS was set, false if the metadata does not contain CHS values
         */
        virtual bool setCHS(uint16_t &cylinders, uint8_t &heads, uint8_t &sectors);

    protected:
        /**
         * Check if the file is open
         * \returns true if the file is open whether it is a container or not, false if the file is closed
         */
        virtual bool openCheck();
        /**
         * Check the enabled container formats, verify that metadata is good, and init the image
         * \returns true if a format is found, false if initialized as a plain iamge
         *  */
        virtual bool verifyAndInit();
        /**
         * Verify Microsoft's vhd file format and initialize the image.
         * The post 2004 (512 byte) and pre 2004 (511 byte) footers are both
         * checked from a single seek and read of the last 512 bytes of the file
         * \returns true if found a vhd format was initialized, false if format was invalid
         */
        virtual bool verifyAndInitVhd();
        /**
         * Validate a VHD footer and if it is valid initialize the image from it
         * \param footer_data pointer to the first byte of the footer, at least sizeof(VhdFooter) bytes
         * \param footer_len length of the footer from the specific version of vhd
         * \param calced_checksum the checksum calculated over the whole footer
         * \returns true if a valid, supported vhd footer was found and initialized
         */
        virtual bool initVhdFooter(const uint8_t *footer_data, size_t footer_len, uint32_t calced_checksum);
        /**
         * Sum the bytes of the start of a VHD footer, skipping the checksum field.
         * The VHD checksum is the one's complement of the sum over the whole footer.
         * \param footer_data pointer to the first byte of the footer
         * \param len number of bytes from the start of the footer to sum
         * \return the sum of the bytes
         */
        virtual uint32_t vhdFooterByteSum(const uint8_t *footer_data, size_t len);
        /**
         * Reset private values of the ZCFsFile class
         */
        virtual void reset();

    private:
        Container m_container_format {Container::None};
        CHS m_chs {};
        uint64_t m_image_size_bytes {};

        // Set by the caller with setVhdEnabled(), not cleared by reset()
        bool m_vhd_enabled {false};

        // Cached contiguousRange() result, used while m_contig_checked is set and
        // the file's first cluster and data length still match the cached values
        bool m_contig_checked {false};
        bool m_contig_result {false};
        uint32_t m_contig_bgn {};
        uint32_t m_contig_end {};
        uint32_t m_contig_first_cluster {};
        uint64_t m_contig_data_length {};
    };
}