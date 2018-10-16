/* Copyright 2014-2018 Gregor Uhlenheuer
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
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static volatile bool use_syslog = false;
static volatile bool quiet = false;
static volatile bool use_color = false;
static volatile bool initialized = false;

void
log_init(nyx_t *nyx)
{
    quiet = nyx->options.quiet;

    use_color = !nyx->options.no_color &&
        !nyx->options.syslog &&
        (nyx->options.no_daemon || !nyx->is_daemon);

    use_syslog = nyx->options.syslog &&
        nyx->is_daemon &&
        !nyx->options.no_daemon;

    if (use_syslog)
        openlog("nyx", LOG_NDELAY, LOG_USER);

    initialized = true;
}

void
log_shutdown(void)
{
    if (!initialized)
        return;

    if (use_syslog)
        closelog();

    initialized = false;
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

static int32_t
get_syslog_level(log_level_e level)
{
    int32_t lvl = LOG_INFO;

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
        /* case NYX_LOG_CRITICAL | NYX_LOG_PERROR: */
            return "[C] ";
        case NYX_LOG_INFO:
        default:
            return "[I] ";
    }
}

static void
log_msg(FILE *stream, log_level_e level, const char *msg, size_t length)
{
    /* safe errno */
    int32_t error = errno;

    time_t now = time(NULL);
    struct tm *ltime = localtime(&now);

    if (use_color)
    {
        size_t start_length;
        const char *start_color = get_log_color(level, &start_length);

        fwrite(start_color, start_length, 1, stream);
    }

    fwrite(get_log_prefix(level), 4, 1, stream);

    fprintf(stream, "%04d-%02d-%02dT%02d:%02d:%02d ",
                ltime->tm_year + 1900,
                ltime->tm_mon + 1,
                ltime->tm_mday,
                ltime->tm_hour,
                ltime->tm_min,
                ltime->tm_sec);

    fwrite(msg, length, 1, stream);

    /* errno specific handling */
    if (level & NYX_LOG_PERROR)
        fprintf(stream, ": error %d", error);

    if (use_color)
    {
        /* write end of coloring */
        fwrite("\033[0m", 4, 1, stream);
    }

    fputc('\n', stream);

    errno = error;
}

static void
log_format_msg(FILE *stream, log_level_e level, const char *format, va_list values)
{
    char *msg = NULL;

    int32_t length = vasprintf(&msg, format, values);

    if (length > 0)
    {
        log_msg(stream, level, msg, length);
        free(msg);
    }
}

void
log_message(nyx_t *nyx, log_level_e level, const char *format, ...)
{
    if (!quiet)
    {
        va_list vas;
        va_start(vas, format);

        if (use_syslog)
            vsyslog(get_syslog_level(level), format, vas);
        else
        {
            FILE *stream = stdout;

            /* write to log file in case we are running as a daemon */
            if (!nyx->options.no_daemon && !nyx->is_init)
            {
                const char *log_file = nyx->options.log_file
                    ? nyx->options.log_file
                    : NYX_DEFAULT_LOG_FILE;

                stream = fopen(log_file, "ae");

                /* fallback to stdout */
                if (stream == NULL)
                    stream = stdout;
            }

            log_format_msg(stream, level, format, vas);

            if (stream != NULL && stream != stdout)
                fclose(stream);
        }

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
                log_format_msg(stdout, level_, format, vas); \
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
