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

#include "def.h"
#include "log.h"
#include "poll.h"
#include "process.h"
#include "state.h"
#include "utils.h"

#include <unistd.h>

static volatile bool need_exit = false;

static void
on_terminate(UNUSED int signum)
{
    log_debug("Caught termination signal - exiting polling manager loop");
    need_exit = true;
}

bool
poll_loop(nyx_t *nyx, poll_handler_t handler)
{
    uint32_t interval = MAX(nyx->options.polling_interval, 1);

    /* reset exit state in case this is a restart */
    need_exit = false;

    setup_signals(nyx, on_terminate);

    log_debug("Starting polling manager loop (interval: %u sec)", interval);

    while (!need_exit)
    {
        list_t *states = nyx->states;

        if (!states)
        {
            wait_interval_fd(nyx->event, interval);
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
                bool running = check_process_running(pid);

                log_debug("Poll: watch '%s' process with PID %d is %srunning",
                        state->watch->name, pid,
                        (running ? "" : "not "));

                handler(pid, running, nyx);
            }
            else
            {
                log_debug("Poll: watch '%s' has no PID (yet)",
                        state->watch->name);
            }

            node = node->next;
        }

        wait_interval_fd(nyx->event, interval);
    }

    return true;
}

/* vim: set et sw=4 sts=4 tw=80: */
