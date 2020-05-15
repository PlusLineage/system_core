/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "incremental.h"

#include <android-base/endian.h>
#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <openssl/base64.h>

#include "adb_client.h"
#include "adb_io.h"
#include "adb_utils.h"
#include "commandline.h"
#include "sysdeps.h"

using namespace std::literals;

namespace incremental {

namespace {

static constexpr auto IDSIG = ".idsig"sv;

using android::base::StringPrintf;

using Size = int64_t;

static inline int32_t read_int32(borrowed_fd fd) {
    int32_t result;
    return ReadFdExactly(fd, &result, sizeof(result)) ? result : -1;
}

static inline void append_int(borrowed_fd fd, std::vector<char>* bytes) {
    int32_t le_val = read_int32(fd);
    auto old_size = bytes->size();
    bytes->resize(old_size + sizeof(le_val));
    memcpy(bytes->data() + old_size, &le_val, sizeof(le_val));
}

static inline void append_bytes_with_size(borrowed_fd fd, std::vector<char>* bytes) {
    int32_t le_size = read_int32(fd);
    if (le_size < 0) {
        return;
    }
    int32_t size = int32_t(le32toh(le_size));
    auto old_size = bytes->size();
    bytes->resize(old_size + sizeof(le_size) + size);
    memcpy(bytes->data() + old_size, &le_size, sizeof(le_size));
    ReadFdExactly(fd, bytes->data() + old_size + sizeof(le_size), size);
}

static inline std::pair<std::vector<char>, int32_t> read_id_sig_headers(borrowed_fd fd) {
    std::vector<char> result;
    append_int(fd, &result);              // version
    append_bytes_with_size(fd, &result);  // hashingInfo
    append_bytes_with_size(fd, &result);  // signingInfo
    auto le_tree_size = read_int32(fd);
    auto tree_size = int32_t(le32toh(le_tree_size));  // size of the verity tree
    return {std::move(result), tree_size};
}

static inline Size verity_tree_size_for_file(Size fileSize) {
    constexpr int INCFS_DATA_FILE_BLOCK_SIZE = 4096;
    constexpr int SHA256_DIGEST_SIZE = 32;
    constexpr int digest_size = SHA256_DIGEST_SIZE;
    constexpr int hash_per_block = INCFS_DATA_FILE_BLOCK_SIZE / digest_size;

    Size total_tree_block_count = 0;

    auto block_count = 1 + (fileSize - 1) / INCFS_DATA_FILE_BLOCK_SIZE;
    auto hash_block_count = block_count;
    for (auto i = 0; hash_block_count > 1; i++) {
        hash_block_count = (hash_block_count + hash_per_block - 1) / hash_per_block;
        total_tree_block_count += hash_block_count;
    }
    return total_tree_block_count * INCFS_DATA_FILE_BLOCK_SIZE;
}

// Read, verify and return the signature bytes. Keeping fd at the position of start of verity tree.
static std::pair<unique_fd, std::vector<char>> read_signature(Size file_size,
                                                              std::string signature_file,
                                                              bool silent) {
    signature_file += IDSIG;

    struct stat st;
    if (stat(signature_file.c_str(), &st)) {
        if (!silent) {
            fprintf(stderr, "Failed to stat signature file %s. Abort.\n", signature_file.c_str());
        }
        return {};
    }

    unique_fd fd(adb_open(signature_file.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        if (!silent) {
            fprintf(stderr, "Failed to open signature file: %s. Abort.\n", signature_file.c_str());
        }
        return {};
    }

    auto [signature, tree_size] = read_id_sig_headers(fd);
    if (auto expected = verity_tree_size_for_file(file_size); tree_size != expected) {
        if (!silent) {
            fprintf(stderr,
                    "Verity tree size mismatch in signature file: %s [was %lld, expected %lld].\n",
                    signature_file.c_str(), (long long)tree_size, (long long)expected);
        }
        return {};
    }

    return {std::move(fd), std::move(signature)};
}

// Base64-encode signature bytes. Keeping fd at the position of start of verity tree.
static std::pair<unique_fd, std::string> read_and_encode_signature(Size file_size,
                                                                   std::string signature_file,
                                                                   bool silent) {
    auto [fd, signature] = read_signature(file_size, std::move(signature_file), silent);
    if (!fd.ok()) {
        return {};
    }

    size_t base64_len = 0;
    if (!EVP_EncodedLength(&base64_len, signature.size())) {
        if (!silent) {
            fprintf(stderr, "Fail to estimate base64 encoded length. Abort.\n");
        }
        return {};
    }
    std::string encoded_signature(base64_len, '\0');
    encoded_signature.resize(EVP_EncodeBlock((uint8_t*)encoded_signature.data(),
                                             (const uint8_t*)signature.data(), signature.size()));

    return {std::move(fd), std::move(encoded_signature)};
}

// Send install-incremental to the device along with properly configured file descriptors in
// streaming format. Once connection established, send all fs-verity tree bytes.
static unique_fd start_install(const Files& files, const Args& passthrough_args, bool silent) {
    std::vector<std::string> command_args{"package", "install-incremental"};
    command_args.insert(command_args.end(), passthrough_args.begin(), passthrough_args.end());

    for (int i = 0, size = files.size(); i < size; ++i) {
        const auto& file = files[i];

        struct stat st;
        if (stat(file.c_str(), &st)) {
            if (!silent) {
                fprintf(stderr, "Failed to stat input file %s. Abort.\n", file.c_str());
            }
            return {};
        }

        auto [signature_fd, signature] = read_and_encode_signature(st.st_size, file, silent);
        if (!signature_fd.ok()) {
            return {};
        }

        auto file_desc =
                StringPrintf("%s:%lld:%s:%s", android::base::Basename(file).c_str(),
                             (long long)st.st_size, std::to_string(i).c_str(), signature.c_str());
        command_args.push_back(std::move(file_desc));
    }

    std::string error;
    auto connection_fd = unique_fd(send_abb_exec_command(command_args, &error));
    if (connection_fd < 0) {
        if (!silent) {
            fprintf(stderr, "Failed to run: %s, error: %s\n",
                    android::base::Join(command_args, " ").c_str(), error.c_str());
        }
        return {};
    }

    // Pushing verity trees for all installation files.
    for (auto&& local_fd : signature_fds) {
        if (!copy_to_file(local_fd.get(), connection_fd.get())) {
            if (!silent) {
                fprintf(stderr, "Failed to stream tree bytes: %s. Abort.\n", strerror(errno));
            }
            return {};
        }
    }

    return connection_fd;
}

}  // namespace

bool can_install(const Files& files) {
    for (const auto& file : files) {
        struct stat st;
        if (stat(file.c_str(), &st)) {
            return false;
        }

        auto [fd, _] = read_signature(st.st_size, file, true);
        if (!fd.ok()) {
            return false;
        }
    }
    return true;
}

std::optional<Process> install(const Files& files, const Args& passthrough_args, bool silent) {
    auto connection_fd = start_install(files, passthrough_args, silent);
    if (connection_fd < 0) {
        if (!silent) {
            fprintf(stderr, "adb: failed to initiate installation on device.\n");
        }
        return {};
    }

    std::string adb_path = android::base::GetExecutablePath();

    auto osh = cast_handle_to_int(adb_get_os_handle(connection_fd.get()));
    auto fd_param = std::to_string(osh);

    // pipe for child process to write output
    int print_fds[2];
    if (adb_socketpair(print_fds) != 0) {
        if (!silent) {
            fprintf(stderr, "Failed to create socket pair for child to print to parent\n");
        }
        return {};
    }
    auto [pipe_read_fd, pipe_write_fd] = print_fds;
    auto pipe_write_fd_param = std::to_string(cast_handle_to_int(adb_get_os_handle(pipe_write_fd)));
    close_on_exec(pipe_read_fd);

    std::vector<std::string> args(std::move(files));
    args.insert(args.begin(), {"inc-server", fd_param, pipe_write_fd_param});
    auto child =
            adb_launch_process(adb_path, std::move(args), {connection_fd.get(), pipe_write_fd});
    if (!child) {
        if (!silent) {
            fprintf(stderr, "adb: failed to fork: %s\n", strerror(errno));
        }
        return {};
    }

    adb_close(pipe_write_fd);

    auto killOnExit = [](Process* p) { p->kill(); };
    std::unique_ptr<Process, decltype(killOnExit)> serverKiller(&child, killOnExit);

    Result result = wait_for_installation(pipe_read_fd);
    adb_close(pipe_read_fd);

    if (result == Result::Success) {
        // adb client exits now but inc-server can continue
        serverKiller.release();
    }
    return child;
}

Result wait_for_installation(int read_fd) {
    static constexpr int maxMessageSize = 256;
    std::vector<char> child_stdout(CHUNK_SIZE);
    int bytes_read;
    int buf_size = 0;
    // TODO(b/150865433): optimize child's output parsing
    while ((bytes_read = adb_read(read_fd, child_stdout.data() + buf_size,
                                  child_stdout.size() - buf_size)) > 0) {
        // print to parent's stdout
        fprintf(stdout, "%.*s", bytes_read, child_stdout.data() + buf_size);

        buf_size += bytes_read;
        const std::string_view stdout_str(child_stdout.data(), buf_size);
        // wait till installation either succeeds or fails
        if (stdout_str.find("Success") != std::string::npos) {
            return Result::Success;
        }
        // on failure, wait for full message
        static constexpr auto failure_msg_head = "Failure ["sv;
        if (const auto begin_itr = stdout_str.find(failure_msg_head);
            begin_itr != std::string::npos) {
            if (buf_size >= maxMessageSize) {
                return Result::Failure;
            }
            const auto end_itr = stdout_str.rfind("]");
            if (end_itr != std::string::npos && end_itr >= begin_itr + failure_msg_head.size()) {
                return Result::Failure;
            }
        }
        child_stdout.resize(buf_size + CHUNK_SIZE);
    }
    return Result::None;
}

}  // namespace incremental
