/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef _ADB_UTILS_H_
#define _ADB_UTILS_H_

#include <string>

bool getcwd(std::string* cwd);
bool directory_exists(const std::string& path);

int mkdirs(const char *path);

std::string escape_arg(const std::string& s);

void dump_hex(const void* ptr, size_t byte_count);

// Parses 'address' into 'host' and 'port'.
// If no port is given, takes the default from *port.
// 'canonical_address' then becomes "host:port" or "[host]:port" as appropriate.
// Note that no real checking is done that 'host' or 'port' is valid; that's
// left to getaddrinfo(3).
// Returns false on failure and sets *error to an appropriate message.
bool parse_host_and_port(const std::string& address,
                         std::string* canonical_address,
                         std::string* host, int* port,
                         std::string* error);

int network_connect(const std::string& host, int port, int type, int timeout, std::string* error);

#endif
