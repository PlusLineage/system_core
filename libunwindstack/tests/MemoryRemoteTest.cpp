/*
 * Copyright (C) 2016 The Android Open Source Project
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
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>

#include <android-base/test_utils.h>
#include <android-base/file.h>
#include <gtest/gtest.h>

#include <unwindstack/Memory.h>

#include "MemoryFake.h"
#include "TestUtils.h"

namespace unwindstack {

class MemoryRemoteTest : public ::testing::Test {
 protected:
  static bool Attach(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) == -1) {
      return false;
    }

    return TestQuiescePid(pid);
  }

  static bool Detach(pid_t pid) {
    return ptrace(PTRACE_DETACH, pid, 0, 0) == 0;
  }

  static constexpr size_t NS_PER_SEC = 1000000000ULL;
};

TEST_F(MemoryRemoteTest, read) {
  std::vector<uint8_t> src(1024);
  memset(src.data(), 0x4c, 1024);

  pid_t pid;
  if ((pid = fork()) == 0) {
    while (true);
    exit(1);
  }
  ASSERT_LT(0, pid);
  TestScopedPidReaper reap(pid);

  ASSERT_TRUE(Attach(pid));

  MemoryRemote remote(pid);

  std::vector<uint8_t> dst(1024);
  ASSERT_TRUE(remote.ReadFully(reinterpret_cast<uint64_t>(src.data()), dst.data(), 1024));
  for (size_t i = 0; i < 1024; i++) {
    ASSERT_EQ(0x4cU, dst[i]) << "Failed at byte " << i;
  }

  ASSERT_TRUE(Detach(pid));
}

TEST_F(MemoryRemoteTest, Read) {
  char* mapping = static_cast<char*>(
      mmap(nullptr, 2 * getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

  ASSERT_NE(MAP_FAILED, mapping);

  mprotect(mapping + getpagesize(), getpagesize(), PROT_NONE);
  memset(mapping + getpagesize() - 1024, 0x4c, 1024);

  pid_t pid;
  if ((pid = fork()) == 0) {
    while (true)
      ;
    exit(1);
  }
  ASSERT_LT(0, pid);
  TestScopedPidReaper reap(pid);

  ASSERT_TRUE(Attach(pid));

  MemoryRemote remote(pid);

  std::vector<uint8_t> dst(4096);
  ASSERT_EQ(1024U, remote.Read(reinterpret_cast<uint64_t>(mapping + getpagesize() - 1024),
                               dst.data(), 4096));
  for (size_t i = 0; i < 1024; i++) {
    ASSERT_EQ(0x4cU, dst[i]) << "Failed at byte " << i;
  }

  ASSERT_TRUE(Detach(pid));
  ASSERT_EQ(0, munmap(mapping, 2 * getpagesize()));
}

TEST_F(MemoryRemoteTest, read_fail) {
  int pagesize = getpagesize();
  void* src = mmap(nullptr, pagesize * 2, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,-1, 0);
  memset(src, 0x4c, pagesize * 2);
  ASSERT_NE(MAP_FAILED, src);
  // Put a hole right after the first page.
  ASSERT_EQ(0, munmap(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(src) + pagesize),
                      pagesize));

  pid_t pid;
  if ((pid = fork()) == 0) {
    while (true);
    exit(1);
  }
  ASSERT_LT(0, pid);
  TestScopedPidReaper reap(pid);

  ASSERT_TRUE(Attach(pid));

  MemoryRemote remote(pid);

  std::vector<uint8_t> dst(pagesize);
  ASSERT_TRUE(remote.ReadFully(reinterpret_cast<uint64_t>(src), dst.data(), pagesize));
  for (size_t i = 0; i < 1024; i++) {
    ASSERT_EQ(0x4cU, dst[i]) << "Failed at byte " << i;
  }

  ASSERT_FALSE(remote.ReadFully(reinterpret_cast<uint64_t>(src) + pagesize, dst.data(), 1));
  ASSERT_TRUE(remote.ReadFully(reinterpret_cast<uint64_t>(src) + pagesize - 1, dst.data(), 1));
  ASSERT_FALSE(remote.ReadFully(reinterpret_cast<uint64_t>(src) + pagesize - 4, dst.data(), 8));

  // Check overflow condition is caught properly.
  ASSERT_FALSE(remote.ReadFully(UINT64_MAX - 100, dst.data(), 200));

  ASSERT_EQ(0, munmap(src, pagesize));

  ASSERT_TRUE(Detach(pid));
}

TEST_F(MemoryRemoteTest, read_overflow) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    while (true)
      ;
    exit(1);
  }
  ASSERT_LT(0, pid);
  TestScopedPidReaper reap(pid);

  ASSERT_TRUE(Attach(pid));

  MemoryRemote remote(pid);

  // Check overflow condition is caught properly.
  std::vector<uint8_t> dst(200);
  ASSERT_FALSE(remote.ReadFully(UINT64_MAX - 100, dst.data(), 200));

  ASSERT_TRUE(Detach(pid));
}

TEST_F(MemoryRemoteTest, read_illegal) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    while (true);
    exit(1);
  }
  ASSERT_LT(0, pid);
  TestScopedPidReaper reap(pid);

  ASSERT_TRUE(Attach(pid));

  MemoryRemote remote(pid);

  std::vector<uint8_t> dst(100);
  ASSERT_FALSE(remote.ReadFully(0, dst.data(), 1));
  ASSERT_FALSE(remote.ReadFully(0, dst.data(), 100));

  ASSERT_TRUE(Detach(pid));
}

TEST_F(MemoryRemoteTest, read_hole) {
  void* mapping =
      mmap(nullptr, 3 * 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(MAP_FAILED, mapping);
  memset(mapping, 0xFF, 3 * 4096);
  mprotect(static_cast<char*>(mapping) + 4096, 4096, PROT_NONE);

  pid_t pid;
  if ((pid = fork()) == 0) {
    while (true);
    exit(1);
  }
  ASSERT_LT(0, pid);
  TestScopedPidReaper reap(pid);

  ASSERT_TRUE(Attach(pid));

  MemoryRemote remote(pid);
  std::vector<uint8_t> dst(4096 * 3, 0xCC);
  ASSERT_EQ(4096U, remote.Read(reinterpret_cast<uintptr_t>(mapping), dst.data(), 4096 * 3));
  for (size_t i = 0; i < 4096; ++i) {
    ASSERT_EQ(0xFF, dst[i]);
  }
  for (size_t i = 4096; i < 4096 * 3; ++i) {
    ASSERT_EQ(0xCC, dst[i]);
  }
}

}  // namespace unwindstack
