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

#include "def.h"
#include "log.h"
#include "poll.h"
#include "process.h"
#include "state.h"

#include <unistd.h>

static volatile int need_exit = 0;

static void
on_terminate(UNUSED int signum)
{
    log_debug("Caught termination signal - exiting polling manager loop");
    need_exit = 1;
}

static void
wait_interval(int timeout)
{
    /* in case we were interrupted by a signal
     * we don't want to be stuck in here sleeping */
    while (!need_exit && timeout-- > 0)
    {
        sleep(1);
    }
}

int
poll_loop(nyx_t *nyx, poll_handler_t handler)
{
    int interval = nyx->options.polling_interval;

    /* reset exit state in case this is a restart */
    need_exit = 0;

    setup_signals(nyx, on_terminate);

    log_debug("Starting polling manager loop (interval: %d sec)", interval);

    while (!need_exit)
    {
        list_t *states = nyx->states;

        if (!states)
        {
            wait_interval(interval);
            continue;
        }

        list_node_t *node = states->head;

        while (node)
        {
            state_t *state = node->data;
            pid_t pid = state->pid;

            if (pid < 1)
            {
                pid = determine_pid(state->watch->name, nyx);
                state->pid = pid;
            }

            if (pid > 0)
            {
                int running = check_process_running(pid);

                log_debug("Poll: watch '%s' process with PID %d is %srunning",
                        state->watch->name, pid,
                        (running ? "" : "not "));

                handler(pid, running, nyx);
            }
            else
            {
                /* TODO: try to determine pid */
                log_debug("Poll: watch '%s' has no PID (yet)",
                        state->watch->name);
            }

            node = node->next;
        }

        wait_interval(interval);
    }

    return 1;
}

/* vim: set et sw=4 sts=4 tw=80: */
