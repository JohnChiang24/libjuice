/**
 * Copyright (c) 2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "conn_third.h"
#include "agent.h"
#include "log.h"
#include "socket.h"
#include "stun.h"
#include "thread.h"
#include "udp.h"

#include <assert.h>
#include <string.h>

#ifdef _WIN32
    #define delay(ms) _sleep(ms)
#else
    #include <unistd.h>
    #define delay(ms) usleep(ms * 1000)
#endif

typedef struct conn_impl {
	thread_t thread;
	timestamp_t next_timestamp;
	mutex_t mutex;
    bool stopped;
} conn_impl_t;

int conn_third_run(juice_agent_t *agent);

static thread_return_t THREAD_CALL conn_thread_entry(void *arg) {
	thread_set_name_self("juice agent");
	juice_agent_t *agent = (juice_agent_t *)arg;
	conn_third_run(agent);
	return (thread_return_t)0;
}

int conn_third_init(juice_agent_t *agent, conn_registry_t *registry, udp_socket_config_t *config) {
	conn_impl_t *conn_impl = calloc(1, sizeof(conn_impl_t));
	if (!conn_impl) {
		JLOG_FATAL("Memory allocation failed for connection impl");
		return -1;
	}

	mutex_init(&conn_impl->mutex, MUTEX_RECURSIVE); // Recursive to allow calls from user callbacks

	agent->conn_impl = conn_impl;

	JLOG_DEBUG("Starting connection third");
	int ret = thread_init(&conn_impl->thread, conn_thread_entry, agent);
	if (ret) {
		JLOG_FATAL("Thread creation failed, error=%d", ret);
		free(conn_impl);
		agent->conn_impl = NULL;
		return -1;
	}
    
	return 0;
}

void conn_third_cleanup(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;

	mutex_lock(&conn_impl->mutex);
	conn_impl->stopped = true;
	mutex_unlock(&conn_impl->mutex);

	conn_third_interrupt(agent);

	JLOG_VERBOSE("Waiting for connection third");
	thread_join(conn_impl->thread, NULL);

	mutex_destroy(&conn_impl->mutex);

	free(agent->conn_impl);
	agent->conn_impl = NULL;
}

void conn_third_lock(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_lock(&conn_impl->mutex);
}

void conn_third_unlock(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	mutex_unlock(&conn_impl->mutex);
}

int conn_third_interrupt(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;

	mutex_lock(&conn_impl->mutex);
	conn_impl->next_timestamp = current_timestamp();
	mutex_unlock(&conn_impl->mutex);

	return 0;
}

int conn_third_run(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;

	conn_impl->next_timestamp = current_timestamp();
	while (true) {
		mutex_lock(&conn_impl->mutex);
		if (conn_impl->stopped) {
			mutex_unlock(&conn_impl->mutex);
			break;
		}
		mutex_unlock(&conn_impl->mutex);

		delay(20);

		if (conn_impl->next_timestamp <= current_timestamp()) {
			if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
				JLOG_WARN("Agent update failed");
				break;
			}
		}
		//end
	}

	JLOG_DEBUG("Leaving connection third");
	return 0;
}

int conn_third_send(juice_agent_t *agent, const addr_record_t *dst, const char *data, size_t size,
                  int ds) {
    if (agent->config.cb_third_send) {
		char addr[128] = {0}, strPort[56] = {0};
        unsigned short port = 0;
        addr_to_string(dst,addr,sizeof(addr));
        port = addr_get_port(dst);
		snprintf(strPort, sizeof(strPort), ":%d", port);
		memset(addr + strlen(addr) - strlen(strPort), 0, 1);

        return agent->config.cb_third_send(data,size,addr,port,agent->config.user_ptr);
    }
	return -1;
}

int conn_third_get_addrs(juice_agent_t *agent, addr_record_t *records, size_t size) {
	if (agent->config.cb_third_get_addrs) {
		char *addrs[ICE_MAX_CANDIDATES_COUNT - 1] = {0};
		unsigned short port = 0;
		int addr_len = 128;
		for (int n = 0; n < ICE_MAX_CANDIDATES_COUNT - 1; n++) {
			addrs[n] = (char *)malloc(addr_len);
			memset(addrs[n], 0, addr_len);
		}
		agent->config.cb_third_get_addrs(&addrs, ICE_MAX_CANDIDATES_COUNT - 1, addr_len, &port,
		                                 agent->config.user_ptr);

		char strPort[56] = {0};
		snprintf(strPort, sizeof(strPort), "%d", port);

		int records_count = 0;
		for (int n = 0; n < ICE_MAX_CANDIDATES_COUNT - 1; n++) {
			if (strlen(addrs[n])) {
				if (addr_resolve(addrs[n], strPort, SOCK_DGRAM, records + records_count,
				                 size - records_count) > 0)
					records_count++;
			}

			free(addrs[n]);
		}
		return records_count;
	}
	return 0;


	//TEST CODE
    if (size < 1) return 0;

    const char * addr = "192.168.73.122";
	const char * port = "3434";

	JLOG_DEBUG("conn_third_get_addrs start : %d", size);

    int records_count = addr_resolve(addr, port, SOCK_DGRAM, records, size);

	JLOG_DEBUG("conn_third_get_addrs end : %d", records_count);
    return records_count;
}
