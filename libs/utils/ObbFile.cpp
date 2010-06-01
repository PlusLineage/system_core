/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "ObbFile"
#include <utils/Log.h>
#include <utils/ObbFile.h>

//#define DEBUG 1

#define kFooterTagSize 8  /* last two 32-bit integers */

#define kFooterMinSize 21 /* 32-bit signature version
                           * 32-bit package version
                           * 32-bit package name size
                           * 1-character package name
                           * 32-bit footer size
                           * 32-bit footer marker
                           */

#define kMaxBufSize    32768 /* Maximum file read buffer */

#define kSignature     0x01059983U /* ObbFile signature */

#define kSigVersion    1 /* We only know about signature version 1 */

/* offsets in version 1 of the header */
#define kPackageVersionOffset 4
#define kPackageNameLenOffset 8
#define kPackageNameOffset    12

/*
 * TEMP_FAILURE_RETRY is defined by some, but not all, versions of
 * <unistd.h>. (Alas, it is not as standard as we'd hoped!) So, if it's
 * not already defined, then define it here.
 */
#ifndef TEMP_FAILURE_RETRY
/* Used to retry syscalls that can return EINTR. */
#define TEMP_FAILURE_RETRY(exp) ({         \
    typeof (exp) _rc;                      \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
#endif

/*
 * Work around situations where off_t is 64-bit and use off64_t in
 * situations where it's 32-bit.
 */
#ifdef OFF_T_IS_64_BIT
#define my_lseek64 lseek
typedef off_t my_off64_t;
#else
#define my_lseek64 lseek64
typedef off64_t my_off64_t;
#endif

namespace android {

ObbFile::ObbFile() :
        mVersion(-1) {
}

ObbFile::~ObbFile() {
}

bool ObbFile::readFrom(const char* filename)
{
    int fd;
    bool success = false;

    fd = ::open(filename, O_RDONLY);
    if (fd < 0) {
        goto out;
    }
    success = readFrom(fd);
    close(fd);

out:
    if (!success) {
        LOGW("failed to read from %s\n", filename);
    }
    return success;
}

bool ObbFile::readFrom(int fd)
{
    if (fd < 0) {
        LOGW("failed to read file\n");
        return false;
    }

    return parseObbFile(fd);
}

bool ObbFile::parseObbFile(int fd)
{
    my_off64_t fileLength = my_lseek64(fd, 0, SEEK_END);

    if (fileLength < kFooterMinSize) {
        if (fileLength < 0) {
            LOGW("error seeking in ObbFile: %s\n", strerror(errno));
        } else {
            LOGW("file is only %lld (less than %d minimum)\n", fileLength, kFooterMinSize);
        }
        return false;
    }

    ssize_t actual;
    size_t footerSize;

    {
        my_lseek64(fd, fileLength - kFooterTagSize, SEEK_SET);

        char *footer = new char[kFooterTagSize];
        actual = TEMP_FAILURE_RETRY(read(fd, footer, kFooterTagSize));
        if (actual != kFooterTagSize) {
            LOGW("couldn't read footer signature: %s\n", strerror(errno));
            return false;
        }

        unsigned int fileSig = get4LE((unsigned char*)footer + sizeof(int32_t));
        if (fileSig != kSignature) {
            LOGW("footer didn't match magic string (expected 0x%08x; got 0x%08x)\n",
                    kSignature, fileSig);
            return false;
        }

        footerSize = get4LE((unsigned char*)footer);
        if (footerSize > (size_t)fileLength - kFooterTagSize
                || footerSize > kMaxBufSize) {
            LOGW("claimed footer size is too large (0x%08lx; file size is 0x%08llx)\n",
                    footerSize, fileLength);
            return false;
        }
    }

    my_off64_t fileOffset = fileLength - footerSize - kFooterTagSize;
    if (my_lseek64(fd, fileOffset, SEEK_SET) != fileOffset) {
        LOGW("seek %lld failed: %s\n", fileOffset, strerror(errno));
        return false;
    }

    size_t readAmount = kMaxBufSize;
    if (readAmount > footerSize)
        readAmount = footerSize;

    char* scanBuf = (char*)malloc(readAmount);
    if (scanBuf == NULL) {
        LOGW("couldn't allocate scanBuf: %s\n", strerror(errno));
        return false;
    }

    actual = TEMP_FAILURE_RETRY(read(fd, scanBuf, readAmount));
    // readAmount is guaranteed to be less than kMaxBufSize
    if (actual != (ssize_t)readAmount) {
        LOGI("couldn't read ObbFile footer: %s\n", strerror(errno));
        free(scanBuf);
        return false;
    }

#ifdef DEBUG
    for (int i = 0; i < readAmount; ++i) {
        LOGI("char: 0x%02x", scanBuf[i]);
    }
#endif

    uint32_t sigVersion = get4LE((unsigned char*)scanBuf);
    if (sigVersion != kSigVersion) {
        LOGW("Unsupported ObbFile version %d\n", sigVersion);
        free(scanBuf);
        return false;
    }

    mVersion = (int32_t) get4LE((unsigned char*)scanBuf + kPackageVersionOffset);

    uint32_t packageNameLen = get4LE((unsigned char*)scanBuf + kPackageNameLenOffset);
    if (packageNameLen <= 0
            || packageNameLen > (footerSize - kPackageNameOffset)) {
        LOGW("bad ObbFile package name length (0x%08x)\n", packageNameLen);
        free(scanBuf);
        return false;
    }

    char* packageName = reinterpret_cast<char*>(scanBuf + kPackageNameOffset);
    mPackageName = String8(const_cast<char*>(packageName), packageNameLen);

    free(scanBuf);
    return true;
}

bool ObbFile::writeTo(const char* filename)
{
    int fd;
    bool success = false;

    fd = ::open(filename, O_WRONLY);
    if (fd < 0) {
        goto out;
    }
    success = writeTo(fd);
    close(fd);

out:
    if (!success) {
        LOGW("failed to write to %s: %s\n", filename, strerror(errno));
    }
    return success;
}

bool ObbFile::writeTo(int fd)
{
    if (fd < 0) {
        return false;
    }

    if (mPackageName.size() == 0 || mVersion == -1) {
        LOGW("tried to write uninitialized ObbFile data");
        return false;
    }

    unsigned char intBuf[sizeof(uint32_t)+1];
    memset(&intBuf, 0, sizeof(intBuf));

    put4LE(intBuf, kSigVersion);
    if (write(fd, &intBuf, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) {
        LOGW("couldn't write signature version: %s", strerror(errno));
        return false;
    }

    put4LE(intBuf, mVersion);
    if (write(fd, &intBuf, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) {
        LOGW("couldn't write package version");
        return false;
    }

    size_t packageNameLen = mPackageName.size();
    put4LE(intBuf, packageNameLen);
    if (write(fd, &intBuf, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) {
        LOGW("couldn't write package name length: %s", strerror(errno));
        return false;
    }

    if (write(fd, mPackageName.string(), packageNameLen) != (ssize_t)packageNameLen) {
        LOGW("couldn't write package name: %s", strerror(errno));
        return false;
    }

    put4LE(intBuf, 3*sizeof(uint32_t) + packageNameLen);
    if (write(fd, &intBuf, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) {
        LOGW("couldn't write footer size: %s", strerror(errno));
        return false;
    }

    put4LE(intBuf, kSignature);
    if (write(fd, &intBuf, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) {
        LOGW("couldn't write footer magic signature: %s", strerror(errno));
        return false;
    }

    return true;
}

}
