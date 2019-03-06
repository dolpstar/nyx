/* Copyright 2014-2019 Gregor Uhlenheuer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "hash.h"
#include "socket.h"

typedef struct watch_t
{
    int32_t id;
    const char *name;
    const char *uid;
    const char *gid;
    const char **start;
    const char **stop;
    const char *dir;
    const char *pid_file;
    const char *log_file;
    const char *error_file;
    const char *http_check;
    uint32_t http_check_port;
    http_method_e http_check_method;
    uint32_t port_check;
    uint32_t stop_timeout;
    uint32_t max_cpu;
    uint64_t max_memory;
    uint32_t startup_delay;
    hash_t *env;
} watch_t;

bool
is_all(const char* name);

watch_t *
watch_new(const char *name);

void
watch_dump(watch_t *watch);

void
watch_destroy(watch_t *watch);

void
_watch_destroy(void *watch);

bool
watch_validate(watch_t *watch);

/* vim: set et sw=4 sts=4 tw=80: */
