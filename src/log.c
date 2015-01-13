/* Copyright 2014-2015 Gregor Uhlenheuer
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

#include "log.h"
#include "nyx.h"

#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static volatile int use_syslog = 0;
static volatile int quiet = 0;
static volatile int color = 0;

void
log_init(nyx_t *nyx)
{
#ifndef NDEBUG
    quiet = 0;
#else
    quiet = nyx->options.quiet;
#endif

    color = !nyx->options.no_color &&
        !nyx->options.syslog &&
        (nyx->options.no_daemon || !nyx->is_daemon);

    use_syslog = nyx->options.syslog &&
        nyx->is_daemon &&
        !nyx->options.no_daemon;

    if (use_syslog)
        openlog("nyx", LOG_NDELAY, LOG_USER);
}

void
log_shutdown(void)
{
    if (use_syslog)
        closelog();
}

static const char *
get_log_color(log_level_e level, size_t *length)
{
    const char *color;

    if (level & NYX_LOG_INFO)
        color = "\033[36m";
    else if (level & NYX_LOG_WARN)
        color = "\033[33m";
    else if (level & NYX_LOG_CRITICAL)
        color = "\033[31;1m";
    else if (level & NYX_LOG_DEBUG)
        color = "\033[37m";
    else if (level & NYX_LOG_PERROR)
        color = "\033[35m";
    else if (level & NYX_LOG_ERROR)
        color = "\033[31;1m";
    else
        color = "\033[32m";

    *length = strlen(color);

    return color;
}

static int
get_syslog_level(log_level_e level)
{
    int lvl = LOG_INFO;

    if (level & NYX_LOG_INFO)
        lvl = LOG_INFO;
    else if (level & NYX_LOG_WARN)
        lvl = LOG_WARNING;
    else if (level & NYX_LOG_CRITICAL)
        lvl = LOG_CRIT;
    else if (level & NYX_LOG_DEBUG)
        lvl = LOG_DEBUG;
    else if (level & NYX_LOG_PERROR)
        lvl = LOG_ERR;
    else if (level & NYX_LOG_ERROR)
        lvl = LOG_ERR;

    return lvl;
}

static inline const char *
get_log_prefix(log_level_e level)
{
    switch (level)
    {
        case NYX_LOG_DEBUG:
            return "[D] ";
        case NYX_LOG_WARN:
            return "[W] ";
        case NYX_LOG_PERROR:
        case NYX_LOG_ERROR:
            return "[E] ";
        case NYX_LOG_CRITICAL:
            return "[C] ";
        case NYX_LOG_INFO:
        default:
            return "[I] ";
    }
}

static void
log_msg(log_level_e level, const char *msg, size_t length)
{
    static const size_t end_length = 4;
    static const char *end_color= "\033[0m";

    /* safe errno */
    int error = errno;

    if (color)
    {
        size_t start_length;
        const char *start_color = get_log_color(level, &start_length);

        fwrite(start_color, start_length, 1, stdout);
    }

    fwrite(get_log_prefix(level), 4, 1, stdout);
    fwrite(msg, length, 1, stdout);

    /* errno specific handling */
    if (level & NYX_LOG_PERROR)
    {
        char buffer[512];
        char *error_msg = strerror_r(error, buffer, 511);

        fputc(':', stdout);
        fputc(' ', stdout);
        fwrite(error_msg, strlen(error_msg), 1, stdout);
    }

    if (color)
        fwrite(end_color, end_length, 1, stdout);

    fputc('\n', stdout);

    errno = error;
}

static void
log_format_msg(log_level_e level, const char *format, va_list values)
{
    char *msg;

    int length = vasprintf(&msg, format, values);

    if (length > 0)
    {
        log_msg(level, msg, length);
        free(msg);
    }
}

void
log_message(log_level_e level, const char *format, ...)
{
    if (!quiet)
    {
        va_list vas;
        va_start(vas, format);

        if (use_syslog)
            vsyslog(get_syslog_level(level), format, vas);
        else
            log_format_msg(level, format, vas);

        va_end(vas);
    }

    if (level & NYX_LOG_CRITICAL)
        abort();
}

#define DECLARE_LOG_FUNC(fn_, level_) \
    void \
    log_##fn_(const char *format, ...) \
    { \
        if (!quiet) \
        { \
            va_list vas; \
            va_start(vas, format); \
            if (use_syslog) \
                vsyslog(get_syslog_level(level_), format, vas); \
            else \
                log_format_msg(level_, format, vas); \
            va_end(vas); \
        } \
        if ((level_) & NYX_LOG_CRITICAL) abort(); \
    }

#ifndef NDEBUG
DECLARE_LOG_FUNC (debug,           NYX_LOG_DEBUG)
#endif

DECLARE_LOG_FUNC (info,            NYX_LOG_INFO)
DECLARE_LOG_FUNC (warn,            NYX_LOG_WARN)
DECLARE_LOG_FUNC (error,           NYX_LOG_ERROR)
DECLARE_LOG_FUNC (perror,          NYX_LOG_PERROR)
DECLARE_LOG_FUNC (critical,        NYX_LOG_CRITICAL)
DECLARE_LOG_FUNC (critical_perror, NYX_LOG_CRITICAL | NYX_LOG_PERROR)

#undef DECLARE_LOG_FUNC

/* vim: set et sw=4 sts=4 tw=80: */
