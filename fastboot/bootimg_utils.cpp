/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "bootimg_utils.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bootimg_set_cmdline(boot_img_hdr_v2* h, const std::string& cmdline) {
    if (cmdline.size() >= sizeof(h->cmdline)) die("command line too large: %zu", cmdline.size());
    strcpy(reinterpret_cast<char*>(h->cmdline), cmdline.c_str());
}

boot_img_hdr_v2* mkbootimg(const std::vector<char>& kernel, const std::vector<char>& ramdisk,
                           const std::vector<char>& second, const std::vector<char>& dtb,
                           size_t base, const boot_img_hdr_v2& src, std::vector<char>* out) {
    const size_t page_mask = src.page_size - 1;

    int64_t header_actual = (sizeof(boot_img_hdr_v1) + page_mask) & (~page_mask);
    int64_t kernel_actual = (kernel.size() + page_mask) & (~page_mask);
    int64_t ramdisk_actual = (ramdisk.size() + page_mask) & (~page_mask);
    int64_t second_actual = (second.size() + page_mask) & (~page_mask);
    int64_t dtb_actual = (dtb.size() + page_mask) & (~page_mask);

    int64_t bootimg_size =
            header_actual + kernel_actual + ramdisk_actual + second_actual + dtb_actual;
    out->resize(bootimg_size);

    boot_img_hdr_v2* hdr = reinterpret_cast<boot_img_hdr_v2*>(out->data());

    *hdr = src;
    memcpy(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE);

    hdr->kernel_size = kernel.size();
    hdr->ramdisk_size = ramdisk.size();
    hdr->second_size = second.size();

    hdr->kernel_addr += base;
    hdr->ramdisk_addr += base;
    hdr->second_addr += base;
    hdr->tags_addr += base;

    if (hdr->header_version == 1) {
        hdr->header_size = sizeof(boot_img_hdr_v1);
    } else if (hdr->header_version == 2) {
        hdr->header_size = sizeof(boot_img_hdr_v2);
        hdr->dtb_size = dtb.size();
        hdr->dtb_addr += base;
    }

    memcpy(hdr->magic + hdr->page_size, kernel.data(), kernel.size());
    memcpy(hdr->magic + hdr->page_size + kernel_actual, ramdisk.data(), ramdisk.size());
    memcpy(hdr->magic + hdr->page_size + kernel_actual + ramdisk_actual, second.data(),
           second.size());
    memcpy(hdr->magic + hdr->page_size + kernel_actual + ramdisk_actual + second_actual, dtb.data(),
           dtb.size());
    return hdr;
}
