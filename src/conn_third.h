/**
 * Copyright (c) 2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef JUICE_CONN_THIRD_H
#define JUICE_CONN_THIRD_H

#include "addr.h"
#include "conn.h"
#include "thread.h"
#include "timestamp.h"

#include <stdbool.h>
#include <stdint.h>

int conn_third_init(juice_agent_t *agent, conn_registry_t *registry, udp_socket_config_t *config);
void conn_third_cleanup(juice_agent_t *agent);
void conn_third_lock(juice_agent_t *agent);
void conn_third_unlock(juice_agent_t *agent);
int conn_third_interrupt(juice_agent_t *agent);
int conn_third_send(juice_agent_t *agent, const addr_record_t *dst, const char *data, size_t size,
                        int ds);
int conn_third_get_addrs(juice_agent_t *agent, addr_record_t *records, size_t size);

#endif
