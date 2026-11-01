/****************************************************************************
 * apps/examples/deskmate/src/agent/dm_agent.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Agent bridge: the seam between the DeskMate UI and the ai_agent runtime.
 *
 * Backends:
 *
 *   mock : an offline responder that answers from the same device state the
 *          UI shows (clock, interfaces, camera frames, notes).  This is what
 *          makes the conversation page and the proactive engine testable on a
 *          host with no network and no API key, and it is the fallback the
 *          product uses when the LLM is unreachable.
 *
 *   ws   : a WebSocket client to the ai_agent built-in server (default
 *          127.0.0.1:28789).  The wire contract is the one already used by
 *          tools/test/ws_demo.py:
 *              -> {"type":"message","content":"...","chat_id":"deskmate-ui"}
 *              <- {"type":"response","content":"...","chat_id":"..."}
 *          which also lets the agent push proactive messages to the UI by
 *          calling its own ws_server_send("deskmate-ui", text).
 *
 *   off  : the bridge is disabled; sends are answered locally.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_AGENT_DM_AGENT_H
#define __APPS_EXAMPLES_DESKMATE_AGENT_DM_AGENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* mode is "mock", "ws" or "off" (NULL keeps the configured default). */

int dm_agent_init(const char *mode);

void dm_agent_set_endpoint(const char *host, int port);

/* Human readable state: "off", "mock", "connecting", "online", "offline". */

const char *dm_agent_state(void);

/* Queue a user utterance.  Returns 0 when it was accepted. */

int dm_agent_send(const char *text);

/* Poll for a reply.  Returns 1 and fills buf when a message is ready, 0 when
 * there is nothing new, and a negated errno on error.
 */

int dm_agent_poll(char *buf, size_t len);

/* Message counters, for the self-test and the console evidence. */

unsigned int dm_agent_sent_count(void);
unsigned int dm_agent_recv_count(void);

/* Feed the proactive engine: called with a text the device wants to say on
 * its own initiative (motion detected, daily briefing, ...).  It is rendered
 * like an agent message.
 */

void dm_agent_push_proactive(const char *text);

void dm_agent_destroy(void);

#endif /* __APPS_EXAMPLES_DESKMATE_AGENT_DM_AGENT_H */
