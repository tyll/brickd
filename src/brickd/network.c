/*
 * brickd
 * Copyright (C) 2012-2014 Matthias Bolte <matthias@tinkerforge.com>
 * Copyright (C) 2014 Olaf Lüke <olaf@tinkerforge.com>
 *
 * network.c: Network specific functions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
	#include <netdb.h>
	#include <unistd.h>
#endif

#include <daemonlib/array.h>
#include <daemonlib/config.h>
#include <daemonlib/event.h>
#include <daemonlib/log.h>
#include <daemonlib/packet.h>
#include <daemonlib/socket.h>
#include <daemonlib/utils.h>

#include "network.h"

#include "hmac.h"
#include "websocket.h"

#define LOG_CATEGORY LOG_CATEGORY_NETWORK

static Array _clients;
static Socket _server_socket_plain;
static Socket _server_socket_websocket;
static bool _server_socket_plain_open = false;
static bool _server_socket_websocket_open = false;
static uint32_t _next_authentication_nonce = 0;

static void network_handle_accept(void *opaque) {
	Socket *server_socket = opaque;
	Socket *client_socket;
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	char hostname[NI_MAXHOST];
	char port[NI_MAXSERV];
	char buffer[NI_MAXHOST + NI_MAXSERV];
	char *name = "<unknown>";

	// accept new client socket
	client_socket = socket_accept(server_socket, (struct sockaddr *)&address, &length);

	if (client_socket == NULL) {
		if (!errno_interrupted()) {
			log_error("Could not accept new client socket: %s (%d)",
			          get_errno_name(errno), errno);
		}

		return;
	}

	if (socket_address_to_hostname((struct sockaddr *)&address, length,
	                               hostname, sizeof(hostname),
	                               port, sizeof(port)) < 0) {
		log_warn("Could not get hostname and port of client (socket: %d): %s (%d)",
		         client_socket->base.handle, get_errno_name(errno), errno);
	} else {
		if (address.ss_family == AF_INET6) {
			snprintf(buffer, sizeof(buffer), "[%s]:%s", hostname, port);
		} else {
			snprintf(buffer, sizeof(buffer), "%s:%s", hostname, port);
		}

		name = buffer;
	}

	// create new client
	if (network_create_client(name, &client_socket->base) == NULL) {
		socket_destroy(client_socket);
		free(client_socket);
	}
}

static const char *network_get_address_family_name(int family, bool report_dual_stack) {
	switch (family) {
	case AF_INET:
		return "IPv4";

	case AF_INET6:
		if (report_dual_stack && config_get_option("listen.dual_stack")->value.boolean) {
			return "IPv6 dual-stack";
		} else {
			return "IPv6";
		}

	default:
		return "<unknown>";
	}
}

static int network_open_server_socket(Socket *server_socket, uint16_t port,
                                      SocketCreateAllocatedFunction create_allocated) {
	int phase = 0;
	const char *address = config_get_option("listen.address")->value.string;
	struct addrinfo *resolved_address = NULL;
	bool dual_stack;

	log_debug("Opening server socket on port %u", port);

	// resolve listen address
	// FIXME: bind to all returned addresses, instead of just the first one.
	//        requires special handling if IPv4 and IPv6 addresses are returned
	//        and dual-stack mode is enabled
	resolved_address = socket_hostname_to_address(address, port);

	if (resolved_address == NULL) {
		log_error("Could not resolve listen address '%s' (port: %u): %s (%d)",
		          address, port, get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 1;

	// create socket
	if (socket_create(server_socket) < 0) {
		log_error("Could not create socket: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 2;

	if (socket_open(server_socket, resolved_address->ai_family,
	                resolved_address->ai_socktype, resolved_address->ai_protocol) < 0) {
		log_error("Could not open %s server socket: %s (%d)",
		          network_get_address_family_name(resolved_address->ai_family, false),
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	if (resolved_address->ai_family == AF_INET6) {
		dual_stack = config_get_option("listen.dual_stack")->value.boolean;

		if (socket_set_dual_stack(server_socket, dual_stack) < 0) {
			log_error("Could not %s dual-stack mode for IPv6 server socket: %s (%d)",
			          dual_stack ? "enable" : "disable",
			          get_errno_name(errno), errno);

			goto cleanup;
		}
	}

#ifndef _WIN32
	// on Unix the SO_REUSEADDR socket option allows to rebind sockets in
	// CLOSE-WAIT state. this is a desired effect. on Windows SO_REUSEADDR
	// allows to rebind sockets in any state. this is dangerous. therefore,
	// don't set SO_REUSEADDR on Windows. sockets can be rebound in CLOSE-WAIT
	// state on Windows by default.
	if (socket_set_address_reuse(server_socket, true) < 0) {
		log_error("Could not enable address-reuse mode for server socket: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}
#endif

	// bind socket and start to listen
	if (socket_bind(server_socket, resolved_address->ai_addr,
	                resolved_address->ai_addrlen) < 0) {
		log_error("Could not bind %s server socket to '%s' on port %u: %s (%d)",
		          network_get_address_family_name(resolved_address->ai_family, true),
		          address, port, get_errno_name(errno), errno);

		goto cleanup;
	}

	if (socket_listen(server_socket, 10, create_allocated) < 0) {
		log_error("Could not listen to %s server socket bound to '%s' on port %u: %s (%d)",
		          network_get_address_family_name(resolved_address->ai_family, true),
		          address, port, get_errno_name(errno), errno);

		goto cleanup;
	}

	log_debug("Started listening to '%s' (%s) on port %u",
	          address,
	          network_get_address_family_name(resolved_address->ai_family, true),
	          port);

	if (event_add_source(server_socket->base.handle, EVENT_SOURCE_TYPE_GENERIC,
	                     EVENT_READ, network_handle_accept, server_socket) < 0) {
		goto cleanup;
	}

	phase = 3;

	freeaddrinfo(resolved_address);

cleanup:
	switch (phase) { // no breaks, all cases fall through intentionally
	case 2:
		socket_destroy(server_socket);

	case 1:
		freeaddrinfo(resolved_address);

	default:
		break;
	}

	return phase == 3 ? 0 : -1;
}

int network_init(void) {
	uint16_t plain_port = (uint16_t)config_get_option("listen.plain_port")->value.integer;
	uint16_t websocket_port = (uint16_t)config_get_option("listen.websocket_port")->value.integer;

	log_debug("Initializing network subsystem");

	if (config_get_option("authentication.secret")->value.string != NULL) {
		log_info("Authentication is enabled");

		_next_authentication_nonce = get_random_uint32();
	}

	// create client array. the Client struct is not relocatable, because a
	// pointer to it is passed as opaque parameter to the event subsystem
	if (array_create(&_clients, 32, sizeof(Client), false) < 0) {
		log_error("Could not create client array: %s (%d)",
		          get_errno_name(errno), errno);

		return -1;
	}

	if (network_open_server_socket(&_server_socket_plain, plain_port,
	                               socket_create_allocated) >= 0) {
		_server_socket_plain_open = true;
	}

	if (websocket_port != 0) {
		if (config_get_option("authentication.secret")->value.string == NULL) {
			log_warn("WebSocket support is enabled without authentication");
		}

		if (network_open_server_socket(&_server_socket_websocket, websocket_port,
		                               websocket_create_allocated) >= 0) {
			_server_socket_websocket_open = true;
		}
	}

	if (!_server_socket_plain_open && !_server_socket_websocket_open) {
		log_error("Could not open any socket to listen to");

		array_destroy(&_clients, (ItemDestroyFunction)client_destroy);

		return -1;
	}

	return 0;
}

void network_exit(void) {
	log_debug("Shutting down network subsystem");

	array_destroy(&_clients, (ItemDestroyFunction)client_destroy);

	if (_server_socket_plain_open) {
		event_remove_source(_server_socket_plain.base.handle, EVENT_SOURCE_TYPE_GENERIC);
		socket_destroy(&_server_socket_plain);
	}

	if (_server_socket_websocket_open) {
		event_remove_source(_server_socket_websocket.base.handle, EVENT_SOURCE_TYPE_GENERIC);
		socket_destroy(&_server_socket_websocket);
	}
}

Client *network_create_client(const char *name, IO *io) {
	Client *client;

	// append to client array
	client = array_append(&_clients);

	if (client == NULL) {
		log_error("Could not append to client array: %s (%d)",
		          get_errno_name(errno), errno);

		return NULL;
	}

	// create new client that takes ownership of the I/O object
	if (client_create(client, name, io, _next_authentication_nonce++, NULL) < 0) {
		array_remove(&_clients, _clients.count - 1, NULL);

		return NULL;
	}

	log_info("Added new client ("CLIENT_INFO_FORMAT")", client_expand_info(client));

	return client;
}

// remove clients that got marked as disconnected
void network_cleanup_clients(void) {
	int i;
	Client *client;

	// iterate backwards for simpler index handling
	for (i = _clients.count - 1; i >= 0; --i) {
		client = array_get(&_clients, i);

		if (client->disconnected) {
			log_debug("Removing disconnected client ("CLIENT_INFO_FORMAT")",
			          client_expand_info(client));

			array_remove(&_clients, i, (ItemDestroyFunction)client_destroy);
		}
	}
}

void network_dispatch_response(Packet *response) {
	char packet_signature[PACKET_MAX_SIGNATURE_LENGTH];
	int i;
	Client *client;

	if (_clients.count == 0) {
		if (packet_header_get_sequence_number(&response->header) == 0) {
			log_debug("No clients connected, dropping %scallback (%s)",
			          packet_get_callback_type(response),
			          packet_get_callback_signature(packet_signature, response));
		} else {
			log_debug("No clients connected, dropping response (%s)",
			          packet_get_response_signature(packet_signature, response));
		}

		return;
	}

	if (packet_header_get_sequence_number(&response->header) == 0) {
		log_debug("Broadcasting %scallback (%s) to %d client(s)",
		          packet_get_callback_type(response),
		          packet_get_callback_signature(packet_signature, response),
		          _clients.count);

		for (i = 0; i < _clients.count; ++i) {
			client = array_get(&_clients, i);

			client_dispatch_response(client, response, true, false);
		}
	} else {
		log_debug("Dispatching response (%s) to %d client(s)",
		          packet_get_response_signature(packet_signature, response),
		          _clients.count);

		for (i = 0; i < _clients.count; ++i) {
			client = array_get(&_clients, i);

			if (client_dispatch_response(client, response, false, false) > 0) {
				// found client with matching pending request
				return;
			}
		}

		log_warn("Broadcasting response (%s) because no client has a matching pending request",
		         packet_get_response_signature(packet_signature, response));

		for (i = 0; i < _clients.count; ++i) {
			client = array_get(&_clients, i);

			client_dispatch_response(client, response, true, false);
		}
	}
}
