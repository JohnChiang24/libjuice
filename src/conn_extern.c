/**
 * Copyright (c) 2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "conn_extern.h"
#include "agent.h"
#include "log.h"
#include "socket.h"
#include "thread.h"
#include "udp.h"

#include <assert.h>
#include <string.h>

typedef struct conn_impl {
	thread_t thread;
	mutex_t mutex;
	mutex_t send_mutex;
	timestamp_t next_timestamp;
	bool stopped;
	bool received;
	bool interrupted;
} conn_impl_t;

int conn_extern_run(juice_agent_t *agent);
int conn_extern_prepare(juice_agent_t *agent, timestamp_t *next_timestamp);
//int conn_extern_process(juice_agent_t *agent);

static thread_return_t THREAD_CALL conn_extern_entry(void *arg) {
	thread_set_name_self("juice agent");
	juice_agent_t *agent = (juice_agent_t *)arg;
	conn_extern_run(agent);
	return (thread_return_t)0;
}

int conn_extern_prepare(juice_agent_t *agent, timestamp_t *next_timestamp) {
	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_lock(&conn_impl->mutex);
	if (conn_impl->stopped) {
		mutex_unlock(&conn_impl->mutex);
		return 0;
	}

	*next_timestamp = conn_impl->next_timestamp;

	mutex_unlock(&conn_impl->mutex);
	return 1;
}

/*
int conn_extern_process(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_lock(&conn_impl->mutex);
	if (conn_impl->stopped) {
		mutex_unlock(&conn_impl->mutex);
		return -1;
	}

	if (conn_impl->received) {
		conn_impl->received = false;
	} else if (conn_impl->next_timestamp <= current_timestamp()) {
		if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
			JLOG_WARN("Agent update failed");
			mutex_unlock(&conn_impl->mutex);
			return -1;
		}
	}

	mutex_unlock(&conn_impl->mutex);
	return 0;
}
*/

int conn_extern_incoming2(juice_agent_t *agent, const addr_record_t *src, const char *data,
	size_t size) {

	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_lock(&conn_impl->mutex);
	if (conn_impl->stopped) {
		mutex_unlock(&conn_impl->mutex);
		return -1;
	}

	if (agent_conn_recv(agent, data, size, src) != 0) {
		JLOG_WARN("Agent receive failed");
		mutex_unlock(&conn_impl->mutex);
		return -1;
	}

	if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
		JLOG_WARN("Agent update failed");
		mutex_unlock(&conn_impl->mutex);
		return -1;
	}
	conn_impl->received = true;
	mutex_unlock(&conn_impl->mutex);

	return 0;
}

int conn_extern_run(juice_agent_t *agent) {
	timestamp_t next_timestamp;
	while (conn_extern_prepare(agent, &next_timestamp) > 0) {
		timediff_t timediff = next_timestamp - current_timestamp();
		if (timediff < 0)
			timediff = 0;

		JLOG_VERBOSE("Entering extern for %d ms", (int)timediff);
		conn_impl_t *conn_impl = agent->conn_impl;
		for (;;) {
			mutex_lock(&conn_impl->mutex);
			if (next_timestamp <= current_timestamp() || conn_impl->received ||
			    conn_impl->interrupted) {
				mutex_unlock(&conn_impl->mutex);
				break;
			}
			mutex_unlock(&conn_impl->mutex);
			delay(2);
		}

		JLOG_VERBOSE("Leaving extern");

		mutex_lock(&conn_impl->mutex);
		conn_impl->interrupted = false;

		if (conn_impl->received) {
			conn_impl->received = false;
		}
		else if (conn_impl->next_timestamp <= current_timestamp()) {
			if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
				JLOG_WARN("Agent update failed");
				mutex_unlock(&conn_impl->mutex);
				break;
			}
		}
		mutex_unlock(&conn_impl->mutex);

	}

	JLOG_DEBUG("Leaving connection thread");
	return 0;
}

int conn_extern_init(juice_agent_t *agent, conn_registry_t *registry, udp_socket_config_t *config) {
	(void)registry;

	conn_impl_t *conn_impl = calloc(1, sizeof(conn_impl_t));
	if (!conn_impl) {
		JLOG_FATAL("Memory allocation failed for connection impl");
		return -1;
	}

	mutex_init(&conn_impl->mutex, MUTEX_RECURSIVE); // Recursive to allow calls from user callbacks
	mutex_init(&conn_impl->send_mutex, 0);

	agent->conn_impl = conn_impl;

	JLOG_DEBUG("Starting connection thread");
	int ret = thread_init(&conn_impl->thread, conn_extern_entry, agent);
	if (ret) {
		JLOG_FATAL("Thread creation failed, error=%d", ret);
		free(conn_impl);
		agent->conn_impl = NULL;
		return -1;
	}

	return 0;
}

void conn_extern_cleanup(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;

	mutex_lock(&conn_impl->mutex);
	conn_impl->stopped = true;
	mutex_unlock(&conn_impl->mutex);

	conn_extern_interrupt(agent);

	JLOG_VERBOSE("Waiting for connection thread");
	thread_join(conn_impl->thread, NULL);

	mutex_destroy(&conn_impl->mutex);
	mutex_destroy(&conn_impl->send_mutex);
	free(agent->conn_impl);
	agent->conn_impl = NULL;
}

void conn_extern_lock(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_lock(&conn_impl->mutex);
}

void conn_extern_unlock(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_unlock(&conn_impl->mutex);
}

int conn_extern_interrupt(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;

	mutex_lock(&conn_impl->mutex);
	conn_impl->next_timestamp = current_timestamp();
	conn_impl->interrupted = true;
	mutex_unlock(&conn_impl->mutex);

	JLOG_VERBOSE("Interrupting connection thread");

	mutex_lock(&conn_impl->send_mutex);
	char dummy = 0; // Some C libraries might error out on NULL pointers
	mutex_unlock(&conn_impl->send_mutex);
	return 0;
}

int conn_extern_send(juice_agent_t *agent, const addr_record_t *dst, const char *data, size_t size,
                     int ds) {
	conn_impl_t *conn_impl = agent->conn_impl;

	mutex_lock(&conn_impl->send_mutex);

	/*
	if (conn_impl->send_ds >= 0 && conn_impl->send_ds != ds) {
		JLOG_VERBOSE("Setting Differentiated Services field to 0x%X", ds);
		if (udp_set_diffserv(conn_impl->sock, ds) == 0)
			conn_impl->send_ds = ds;
		else
			conn_impl->send_ds = -1; // disable for next time
	}
	*/

	JLOG_VERBOSE("Sending datagram, size=%d", size);

	char dst_addr[64] = {0}, port[24] = {0};
	unsigned short dst_port = 0;
	addr_to_string(dst, dst_addr, sizeof(dst_addr));
	dst_port = addr_get_port(dst);
	snprintf(port, sizeof(port), ":%d", dst_port);
	memset(dst_addr + strlen(dst_addr) - strlen(port), 0, 1);

	int ret = agent->config.extern_cb_outgoing(dst_addr, dst_port, data, size, agent->config.user_ptr); // ret >= 0: success
	if (ret < 0) {
		JLOG_WARN("Send failed, errno=%d", sockerrno);
	}

	mutex_unlock(&conn_impl->send_mutex);
	return ret;
}

int conn_extern_get_addrs(juice_agent_t *agent, addr_record_t *records, size_t size) {
	conn_impl_t *conn_impl = agent->conn_impl;

	return udp_get_addrs(agent->config.extern_sock, records, size);
}
