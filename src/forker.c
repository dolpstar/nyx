/* Copyright 2014-2017 Gregor Uhlenheuer
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

#define _GNU_SOURCE

#include "config.h"
#include "def.h"
#include "forker.h"
#include "fs.h"
#include "log.h"
#include "process.h"
#include "watch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static watch_t *
find_watch(nyx_t *nyx, int32_t id)
{
    if (nyx->watches == NULL)
        return NULL;

    const char *key = NULL;
    void *data = NULL;

    hash_iter_t *iter = hash_iter_start(nyx->watches);

    while (hash_iter(iter, &key, &data))
    {
        watch_t *watch = data;

        if (watch && watch->id == id)
        {
            free(iter);
            return watch;
        }
    }

    free(iter);
    return NULL;
}

static void
set_environment(const watch_t *watch)
{
    const char *key = NULL;
    void *data = NULL;

    if (watch->env == NULL || hash_count(watch->env) < 1)
        return;

    hash_iter_t *iter = hash_iter_start(watch->env);

    while (hash_iter(iter, &key, &data))
    {
        char *value = data;

        setenv(key, value, 1);
    }

    free(iter);
}

static void
set_magic_pid(pid_t pid)
{
    char str[32] = {0};
    snprintf(str, LEN(str)-1, "%d", pid);

    setenv("NYX_PID", str, 1);
}

static void
close_fds(pid_t pid)
{
    char path[256] = {0};

    /* first we try to search in /proc/{pid}/fd */
    snprintf(path, LEN(path)-1, "/proc/%d/fd", pid);

    DIR *dir = opendir(path);
    if (dir)
    {
        int32_t dir_fd = dirfd(dir);

        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL)
        {
            int32_t fd = atoi(entry->d_name);

            if (fd >= 3 && fd != dir_fd)
                close(fd);
        }

        closedir(dir);
        return;
    }

    /* otherwise we will close all file descriptors up
     * to the maximum descriptor index */
    int32_t max;
    if ((max = getdtablesize()) == -1)
        max = 256;

    for (int32_t fd = 3 /* stderr + 1 */; fd < max; fd++)
        close(fd);
}

static bool
write_pipe(int32_t fd, int32_t value)
{
    FILE *stream = fdopen(fd, "w");

    if (stream != NULL)
    {
        fprintf(stream, "%d\n", value);

        fclose(stream);
        return true;
    }

    return false;
}

static int32_t
read_pipe(int32_t fd)
{
    int32_t value = 0;
    FILE *stream = fdopen(fd, "r");

    if (stream != NULL)
    {
        if (fscanf(stream, "%d", &value) != 1)
            value = 0;

        fclose(stream);
    }

    return value;
}

static const char *
get_exec_directory(watch_t *watch, nyx_t *nyx)
{
    /* no watch specific directory given */
    if (watch->dir == NULL || *watch->dir == '\0')
    {
        /* either root dir ('/') or the current directory (in local-mode) */
        if (nyx->options.local_mode)
            return nyx->nyx_dir;
        return "/";
    }

    return watch->dir;
}

static void
spawn_exec(watch_t *watch, const char *dir, bool start, bool proxy_output, pid_t stop_pid)
{
    uid_t uid = 0;
    gid_t gid = 0;

    const char **args = start ? watch->start : watch->stop;
    const char *executable = *args;

    /* determine user and group */
    if (watch->uid)
        get_user(watch->uid, &uid, &gid);

    if (watch->gid)
        get_group(watch->gid, &gid);

    /* TODO: configurable mask */
    umask(0);

    /* create session */
    setsid();

    /* set user/group */
    if (gid)
    {
        gid_t groups[] = { gid };

        setgroups(1, groups);

        if (setgid(gid) == -1)
            log_perror("nyx: setgid");
    }

    if (uid && gid)
        initgroups(watch->uid, gid);

    if (uid)
    {
        /* in case the uid was modified we adjust the $USER and $HOME
         * environment variables appropriately */
        if (setuid(uid) != -1)
        {
            if (!watch->env)
                watch->env = hash_new(free);

            if (!hash_get(watch->env, "USER"))
            {
                hash_add(watch->env, "USER", strdup(watch->uid));
            }

            if (!hash_get(watch->env, "HOME"))
            {
                struct passwd *pw = getpwuid(uid);

                if (pw && pw->pw_dir)
                    hash_add(watch->env, "HOME", strdup(pw->pw_dir));
            }
        }
    }

    if (chdir(dir) == -1)
        log_critical_perror("nyx: chdir");

    /* stdin */
    close(STDIN_FILENO);

    if (open("/dev/null", O_RDONLY) == -1)
    {
        fprintf(stderr, "Failed to open /dev/null");
        exit(EXIT_FAILURE);
    };

    /* STDOUT */

    if (start && watch->log_file)
    {
        close(STDOUT_FILENO);

        if (open(watch->log_file,
                    O_RDWR | O_APPEND | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == -1)
        {
            fprintf(stderr, "Failed to open log file '%s'",
                    watch->log_file);
            exit(EXIT_FAILURE);
        }
    }
    else if (start && proxy_output)
    {
        /* in this case we want to keep stdout open as it is */
    }
    else
    {
        close(STDOUT_FILENO);

        if (open("/dev/null", O_WRONLY) == -1)
        {
            fprintf(stderr, "Failed to open /dev/null");
            exit(EXIT_FAILURE);
        }
    }

    /* STDERR */

    if (start && watch->error_file)
    {
        close(STDERR_FILENO);

        if (open(watch->error_file,
                    O_RDWR | O_APPEND | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == -1)
        {
            fprintf(stdout, "Failed to open error file '%s'",
                    watch->error_file);
            exit(EXIT_FAILURE);
        }
    }
    else if (start && proxy_output)
    {
        /* in this case we want to keep stderr open as it is */
    }
    else
    {
        close(STDERR_FILENO);

        if (open("/dev/null", O_RDWR) == -1)
        {
            fprintf(stdout, "Failed to open /dev/null");
            exit(EXIT_FAILURE);
        }
    }

    /* set user defined environment variables */
    set_environment(watch);

    /* set the 'magic' environment NYX_PID for custom stop-commands */
    if (stop_pid)
    {
        set_magic_pid(stop_pid);
    }

    close_fds(getpid());

    /* on success this call won't return */
    execvp(executable, (char * const *)args);

    if (errno == ENOENT)
        exit(EXIT_SUCCESS);

    log_critical_perror("nyx: execvp %s", executable);
}

static pid_t
spawn_stop(nyx_t *nyx, watch_t *watch, pid_t stop_pid)
{
    pid_t pid = fork();

    if (pid == -1)
        log_critical_perror("nyx: fork");

    if (pid == 0)
    {
        const char *dir = get_exec_directory(watch, nyx);
        spawn_exec(watch, dir, false, false, stop_pid);
    }

    /* the return value will be written into the process' pid file
     * that's why the actual 'stop-process-pid' is not of interest here */
    return 0;
}

static pid_t
spawn_start(nyx_t *nyx, watch_t *watch)
{
    int32_t pipes[2] = {0};
    bool double_fork = !nyx->is_init;

    /* In 'init-mode' and quiet output we will probably proxy
     * the service's stdout/stderr instead.
     * This will be the desired effect if using nyx as the
     * docker entrypoint for example */
    bool proxy_output = nyx->is_init && nyx->options.quiet;

    /* in case of a 'double-fork' we need some way to retrieve the
     * resulting process' pid */
    if (double_fork)
    {
        if (pipe(pipes) == -1)
            log_critical_perror("nyx: pipe");
    }

    pid_t pid = fork();
    pid_t outer_pid = pid;

    /* fork failed */
    if (pid == -1)
        log_critical_perror("nyx: fork");

    /* child process */
    if (pid == 0)
    {
        const char *dir = get_exec_directory(watch, nyx);

        /* in 'init mode' we have to fork only once */
        if (!double_fork)
        {
            /* this call won't return */
            spawn_exec(watch, dir, true, proxy_output, 0);
        }
        /* otherwise we want to 'double fork' */
        else
        {
            pid_t inner_pid = fork();

            if (inner_pid == -1)
                log_critical_perror("nyx: fork");

            if (inner_pid == 0)
            {
                /* this call won't return */
                spawn_exec(watch, dir, true, proxy_output, 0);
            }

            /* close the read end before */
            close(pipes[0]);

            /* now we write the child pid into the pipe */
            write_pipe(pipes[1], inner_pid);

            exit(EXIT_SUCCESS);
        }
    }

    /* in case of a 'double-fork' we have to read the actual
     * process' pid from the read end of the pipe */
    if (double_fork)
    {
        /* close the write end before */
        close(pipes[1]);

        pid = read_pipe(pipes[0]);

        /* wait for the intermediate forked process
         * to terminate */
        waitpid(outer_pid, NULL, 0);
    }

    return pid;
}

/**
 * @brief Callback to receive child termination signals
 * @param signum signal number
 */
static void
handle_child_stop(UNUSED int32_t signum)
{
    int32_t last_errno = errno;

    log_debug("Received child stop signal - waiting for termination");

    /* wait for all child processes */
    while (waitpid(-1, NULL, WNOHANG) > 0)
    {
        /* do nothing */
    }

    errno = last_errno;
}

static void
forker(nyx_t *nyx, int32_t pipe_fd)
{
    fork_info_t info = {0, 0, 0};

    /* register SIGCHLD handler */
    if (nyx->is_init)
    {
        log_debug("Running in init-mode - listening for child termination");

        struct sigaction action =
        {
            .sa_flags = SA_NOCLDSTOP | SA_RESTART,
            .sa_handler = handle_child_stop
        };

        sigfillset(&action.sa_mask);
        sigaction(SIGCHLD, &action, NULL);
    }

    while (read(pipe_fd, &info, sizeof(fork_info_t)) != 0)
    {
        if (info.id == NYX_FORKER_RELOAD)
        {
            log_debug("forker: received reload command");

            clear_watches(nyx);
            destroy_options(nyx);
            nyx->watches = hash_new(_watch_destroy);

            if (parse_config(nyx, true))
            {
                log_debug("forker: successfully reloaded config");
            }
            else
            {
                log_warn("forker: failed to reload config");
            }

            continue;
        }

        log_debug("forker: received watch id %d", info.id);

        watch_t *watch = find_watch(nyx, info.id);

        if (watch == NULL)
        {
            log_warn("forker: no watch with id %d found!", info.id);
            continue;
        }

        pid_t pid = (info.start)
            ? spawn_start(nyx, watch)
            : spawn_stop(nyx, watch, info.pid);

        write_pid(pid, watch->name, nyx);
    }

    close(pipe_fd);

    nyx_destroy(nyx);

    log_debug("forker: terminated");
}

static fork_info_t *
forker_new(int32_t id, bool start, pid_t pid)
{
    fork_info_t *info = xcalloc1(sizeof(fork_info_t));

    info->id = id;
    info->start = start;
    info->pid = pid;

    return info;
}

fork_info_t *
forker_stop(int32_t idx, pid_t pid)
{
    return forker_new(idx, false, pid);
}

fork_info_t *
forker_start(int32_t idx)
{
    return forker_new(idx, true, 0);
}

fork_info_t *
forker_reload(void)
{
    return forker_new(NYX_FORKER_RELOAD, true, 0);
}

int32_t
forker_init(nyx_t *nyx)
{
    int32_t pipes[2] = {0};

    /* open pipes -> bail out if failed */
    if (pipe(pipes) == -1)
        return 0;

    /* here we are still in the main nyx thread
     * we will fork now so both threads have access to both the read
     * and write side of the pipes */

    pid_t pid = fork();

    /* fork failed */
    if (pid == -1)
        return 0;

    /* here we are in the child/forker thread */
    if (pid == 0)
    {
        /* close the write end of the pipes first */
        close(pipes[1]);

        /* ignore SIGINT - we are terminated by the main thread */
        signal(SIGINT, SIG_IGN);

        /* enter the real fork processing logic now */
        forker(nyx, pipes[0]);
        exit(EXIT_SUCCESS);
    }

    /* parent/main thread here:
     * close the read end of the pipes */
    close(pipes[0]);

    /* set/refresh forker's pid */
    nyx->forker_pid = pid;

    /* return the write pipe descriptor */
    return pipes[1];
}

/* vim: set et sw=4 sts=4 tw=80: */
