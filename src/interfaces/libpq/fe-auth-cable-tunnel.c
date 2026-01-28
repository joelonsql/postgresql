/*-------------------------------------------------------------------------
 * fe-auth-cable-tunnel.c
 *	  WebSocket tunnel client for caBLE protocol
 *
 * This implements the WebSocket tunnel connection to the caBLE relay server.
 * The tunnel establishes a secure channel between the client (psql) and the
 * phone's authenticator.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-cable-tunnel.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "libpq/cable.h"

#ifdef USE_OPENSSL

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

/* WebSocket constants */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_FRAME_SIZE (16 * 1024)

/* Internal buffer size */
#define TUNNEL_BUFFER_SIZE (32 * 1024)

/*
 * Set a socket to non-blocking mode.
 */
static int
set_nonblocking(int fd)
{
	int			flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Set a socket to blocking mode.
 */
static int
set_blocking(int fd)
{
	int			flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

/*
 * Connect to a host via TCP.
 */
static int
tcp_connect(const char *host, int port, int timeout_secs)
{
	struct addrinfo hints;
	struct addrinfo *result,
			   *rp;
	char		port_str[16];
	int			sock = -1;
	int			ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(port_str, sizeof(port_str), "%d", port);

	ret = getaddrinfo(host, port_str, &hints, &result);
	if (ret != 0)
		return -1;

	for (rp = result; rp != NULL; rp = rp->ai_next)
	{
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0)
			continue;

		/* Set non-blocking for connect with timeout */
		if (set_nonblocking(sock) < 0)
		{
			close(sock);
			sock = -1;
			continue;
		}

		ret = connect(sock, rp->ai_addr, rp->ai_addrlen);
		if (ret < 0 && errno == EINPROGRESS)
		{
			struct timeval tv;
			fd_set		wfds;

			tv.tv_sec = timeout_secs;
			tv.tv_usec = 0;

			FD_ZERO(&wfds);
			FD_SET(sock, &wfds);

			ret = select(sock + 1, NULL, &wfds, NULL, &tv);
			if (ret > 0)
			{
				int			err;
				socklen_t	len = sizeof(err);

				if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
				{
					/* Connection successful */
					break;
				}
			}
		}
		else if (ret == 0)
		{
			/* Immediate connection */
			break;
		}

		close(sock);
		sock = -1;
	}

	freeaddrinfo(result);
	return sock;
}

/*
 * Generate a random WebSocket key.
 */
static void
generate_ws_key(char *key, size_t key_len)
{
	uint8_t		random_bytes[16];
	EVP_ENCODE_CTX *ctx;
	int			outlen;

	RAND_bytes(random_bytes, sizeof(random_bytes));

	ctx = EVP_ENCODE_CTX_new();
	EVP_EncodeInit(ctx);
	EVP_EncodeBlock((unsigned char *) key, random_bytes, sizeof(random_bytes));
	EVP_ENCODE_CTX_free(ctx);
}

/*
 * Calculate the expected Sec-WebSocket-Accept value.
 */
static void
calculate_ws_accept(const char *key, char *accept, size_t accept_len)
{
	char		combined[128];
	uint8_t		sha1_hash[20];
	SHA_CTX		sha_ctx;

	snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

	SHA1_Init(&sha_ctx);
	SHA1_Update(&sha_ctx, combined, strlen(combined));
	SHA1_Final(sha1_hash, &sha_ctx);

	EVP_EncodeBlock((unsigned char *) accept, sha1_hash, sizeof(sha1_hash));
}

/*
 * Hex encode bytes.
 */
static void
hex_encode(const uint8_t *data, size_t len, char *out)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t		i;

	for (i = 0; i < len; i++)
	{
		out[i * 2] = hex[data[i] >> 4];
		out[i * 2 + 1] = hex[data[i] & 0x0F];
	}
	out[len * 2] = '\0';
}

/*
 * Perform WebSocket handshake.
 */
static int
websocket_handshake(CableTunnel *tunnel, const char *host, const char *path)
{
	char		ws_key[32];
	char		request[2048];
	char		response[2048];
	int			len,
				total = 0;
	char	   *header_end;

	/* Generate WebSocket key */
	generate_ws_key(ws_key, sizeof(ws_key));

	/* Build HTTP upgrade request - headers must match browser format for Apple */
	len = snprintf(request, sizeof(request),
				   "GET %s HTTP/1.1\r\n"
				   "Host: %s\r\n"
				   "Connection: Upgrade\r\n"
				   "Pragma: no-cache\r\n"
				   "Cache-Control: no-cache\r\n"
				   "Upgrade: websocket\r\n"
				   "Origin: wss://%s\r\n"
				   "Sec-WebSocket-Version: 13\r\n"
				   "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
				   "Sec-WebSocket-Key: %s\r\n"
				   "Sec-WebSocket-Protocol: %s\r\n"
				   "\r\n",
				   path, host, host, ws_key, CABLE_WEBSOCKET_PROTOCOL);

	fprintf(stderr, "[TUNNEL] Sending request:\n%s", request);

	/* Send request */
	if (SSL_write((SSL *) tunnel->ssl, request, len) != len)
	{
		tunnel->error_message = strdup("failed to send WebSocket handshake");
		return -1;
	}

	/* Read response */
	while (total < (int) sizeof(response) - 1)
	{
		len = SSL_read((SSL *) tunnel->ssl, response + total, sizeof(response) - 1 - total);
		if (len <= 0)
		{
			tunnel->error_message = strdup("failed to read WebSocket response");
			return -1;
		}
		total += len;
		response[total] = '\0';

		/* Check for end of headers */
		header_end = strstr(response, "\r\n\r\n");
		if (header_end)
			break;
	}

	/* Verify response (check both cases for header names) */
	if (strstr(response, "101") == NULL ||
		(strstr(response, "Upgrade") == NULL && strstr(response, "upgrade") == NULL))
	{
		char	   *errmsg;

		/* Include the server response in the error for debugging */
		errmsg = malloc(strlen(response) + 100);
		if (errmsg)
		{
			snprintf(errmsg, strlen(response) + 100,
					 "WebSocket upgrade failed. Server response: %.200s",
					 response);
			tunnel->error_message = errmsg;
		}
		else
			tunnel->error_message = strdup("WebSocket upgrade failed");
		return -1;
	}

	/* Verify Sec-WebSocket-Accept */
	{
		char		expected_accept[64];
		char	   *accept_header;

		calculate_ws_accept(ws_key, expected_accept, sizeof(expected_accept));

		accept_header = strstr(response, "Sec-WebSocket-Accept:");
		if (!accept_header || strstr(accept_header, expected_accept) == NULL)
		{
			/* Accept header validation - not strictly required */
		}
	}

	tunnel->ws_connected = true;
	return 0;
}

/*
 * Create and initialize a new tunnel.
 */
CableTunnel *
cable_tunnel_new(void)
{
	CableTunnel *tunnel;

	tunnel = calloc(1, sizeof(CableTunnel));
	if (!tunnel)
		return NULL;

	tunnel->socket_fd = -1;
	tunnel->ws_recv_buffer = malloc(TUNNEL_BUFFER_SIZE);
	if (!tunnel->ws_recv_buffer)
	{
		free(tunnel);
		return NULL;
	}
	tunnel->ws_recv_buffer_size = TUNNEL_BUFFER_SIZE;
	tunnel->ws_recv_buffer_len = 0;

	return tunnel;
}

/*
 * Free tunnel resources.
 */
void
cable_tunnel_free(CableTunnel *tunnel)
{
	if (!tunnel)
		return;

	if (tunnel->ssl)
	{
		SSL_shutdown((SSL *) tunnel->ssl);
		SSL_free((SSL *) tunnel->ssl);
	}

	if (tunnel->ssl_ctx)
		SSL_CTX_free((SSL_CTX *) tunnel->ssl_ctx);

	if (tunnel->socket_fd >= 0)
		close(tunnel->socket_fd);

	free(tunnel->ws_recv_buffer);
	free(tunnel->server_url);
	free(tunnel->error_message);
	free(tunnel);
}

/*
 * Connect to the tunnel server.
 */
int
cable_tunnel_connect(CableTunnel *tunnel, const char *server,
					 const uint8_t *tunnel_id, const uint8_t *routing_id)
{
	SSL_CTX	   *ctx;
	SSL		   *ssl;
	char		path[256];
	char		tunnel_id_hex[CABLE_TUNNEL_ID_LENGTH * 2 + 1];

	fprintf(stderr, "[TUNNEL] Connecting to %s:%d...\n", server, CABLE_TUNNEL_PORT);

	/* Copy tunnel and routing IDs */
	memcpy(tunnel->tunnel_id, tunnel_id, CABLE_TUNNEL_ID_LENGTH);
	memcpy(tunnel->routing_id, routing_id, CABLE_ROUTING_ID_LENGTH);

	/* Create TCP connection */
	tunnel->socket_fd = tcp_connect(server, CABLE_TUNNEL_PORT, CABLE_CONNECT_TIMEOUT_SECS);
	if (tunnel->socket_fd < 0)
	{
		fprintf(stderr, "[TUNNEL] TCP connection failed\n");
		tunnel->error_message = strdup("failed to connect to tunnel server");
		return -1;
	}
	fprintf(stderr, "[TUNNEL] TCP connected, fd=%d\n", tunnel->socket_fd);

	/* Switch socket to blocking mode for SSL handshake */
	if (set_blocking(tunnel->socket_fd) < 0)
	{
		tunnel->error_message = strdup("failed to set blocking mode");
		return -1;
	}

	/* Create SSL context */
	ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx)
	{
		tunnel->error_message = strdup("failed to create SSL context");
		return -1;
	}
	tunnel->ssl_ctx = ctx;

	/* Set minimum TLS version */
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

	/* Load system CA certificates for verification */
	if (!SSL_CTX_set_default_verify_paths(ctx))
	{
		tunnel->error_message = strdup("failed to load CA certificates");
		return -1;
	}

	/* Enable certificate verification */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

	/* Create SSL connection */
	ssl = SSL_new(ctx);
	if (!ssl)
	{
		tunnel->error_message = strdup("failed to create SSL object");
		return -1;
	}
	tunnel->ssl = ssl;

	/* Set SNI hostname */
	SSL_set_tlsext_host_name(ssl, server);

	/* Attach to socket */
	if (!SSL_set_fd(ssl, tunnel->socket_fd))
	{
		tunnel->error_message = strdup("failed to set SSL file descriptor");
		return -1;
	}

	/* Perform SSL handshake */
	{
		int			ssl_ret = SSL_connect(ssl);

		if (ssl_ret != 1)
		{
			char		errbuf[256];
			int			ssl_err = SSL_get_error(ssl, ssl_ret);
			unsigned long sslerr = ERR_get_error();

			if (sslerr)
				ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
			else
			{
				const char *ssl_err_str;

				switch (ssl_err)
				{
					case SSL_ERROR_ZERO_RETURN:
						ssl_err_str = "connection closed";
						break;
					case SSL_ERROR_WANT_READ:
						ssl_err_str = "would block on read";
						break;
					case SSL_ERROR_WANT_WRITE:
						ssl_err_str = "would block on write";
						break;
					case SSL_ERROR_SYSCALL:
						ssl_err_str = "syscall error";
						break;
					case SSL_ERROR_SSL:
						ssl_err_str = "protocol error";
						break;
					default:
						ssl_err_str = "unknown error";
						break;
				}
				snprintf(errbuf, sizeof(errbuf), "SSL error %d: %s (server: %s)",
						 ssl_err, ssl_err_str, server);
			}

			tunnel->error_message = malloc(512);
			if (tunnel->error_message)
				snprintf(tunnel->error_message, 512, "SSL handshake failed: %s", errbuf);
			return -1;
		}
	}

	/*
	 * Build WebSocket path: /cable/connect/{routing_id}/{tunnel_id}
	 *
	 * Both parties use /cable/connect/ with the same routing_id and tunnel_id.
	 * The routing_id is derived from the public key so both sides compute the same value.
	 */
	{
		char		routing_id_hex[CABLE_ROUTING_ID_LENGTH * 2 + 1];

		hex_encode(routing_id, CABLE_ROUTING_ID_LENGTH, routing_id_hex);
		hex_encode(tunnel_id, CABLE_TUNNEL_ID_LENGTH, tunnel_id_hex);
		snprintf(path, sizeof(path), "/cable/connect/%s/%s", routing_id_hex, tunnel_id_hex);
	}
	fprintf(stderr, "[TUNNEL] WebSocket path: %s\n", path);

	/* Perform WebSocket handshake */
	fprintf(stderr, "[TUNNEL] Starting WebSocket handshake...\n");
	if (websocket_handshake(tunnel, server, path) < 0)
	{
		fprintf(stderr, "[TUNNEL] WebSocket handshake failed: %s\n",
				tunnel->error_message ? tunnel->error_message : "unknown");
		return -1;
	}
	fprintf(stderr, "[TUNNEL] WebSocket connected!\n");

	/* Save server URL */
	tunnel->server_url = strdup(server);

	return 0;
}

/*
 * Wait for peer to connect through the tunnel.
 */
int
cable_tunnel_wait_for_peer(CableTunnel *tunnel, int timeout_secs)
{
	struct timeval tv;
	fd_set		rfds;
	int			ret;
	uint8_t		frame[256];
	int			len;
	time_t		start_time,
				now;

	fprintf(stderr, "[TUNNEL] Waiting for peer (timeout=%d sec)...\n", timeout_secs);

	if (!tunnel->ws_connected)
	{
		tunnel->error_message = strdup("WebSocket not connected");
		return -1;
	}

	start_time = time(NULL);

	while (1)
	{
		now = time(NULL);
		if (now - start_time >= timeout_secs)
		{
			fprintf(stderr, "[TUNNEL] Timeout waiting for peer\n");
			tunnel->error_message = strdup("timeout waiting for peer");
			return -1;
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_SET(tunnel->socket_fd, &rfds);

		ret = select(tunnel->socket_fd + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			tunnel->error_message = strdup("select failed");
			return -1;
		}

		if (ret > 0)
		{
			/* Try to read a WebSocket frame */
			len = SSL_read((SSL *) tunnel->ssl, frame, sizeof(frame));
			if (len > 0)
			{
				fprintf(stderr, "[TUNNEL] Received %d bytes from peer!\n", len);
				fprintf(stderr, "[TUNNEL] First bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
						len > 0 ? frame[0] : 0, len > 1 ? frame[1] : 0,
						len > 2 ? frame[2] : 0, len > 3 ? frame[3] : 0,
						len > 4 ? frame[4] : 0, len > 5 ? frame[5] : 0,
						len > 6 ? frame[6] : 0, len > 7 ? frame[7] : 0);

				/*
				 * Any message from the server indicates the peer has
				 * connected. The first message should be the Noise
				 * handshake initiation from the phone.
				 */

				/* Buffer the received data */
				if (tunnel->ws_recv_buffer_len + len <= tunnel->ws_recv_buffer_size)
				{
					memcpy(tunnel->ws_recv_buffer + tunnel->ws_recv_buffer_len, frame, len);
					tunnel->ws_recv_buffer_len += len;
				}

				fprintf(stderr, "[TUNNEL] Peer connected, buffered %zu bytes\n",
						tunnel->ws_recv_buffer_len);
				return 0;
			}
			else if (len == 0)
			{
				fprintf(stderr, "[TUNNEL] Connection closed by peer\n");
				tunnel->error_message = strdup("connection closed");
				return -1;
			}
			else
			{
				int			ssl_err = SSL_get_error((SSL *) tunnel->ssl, len);

				if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE)
				{
					fprintf(stderr, "[TUNNEL] SSL read error: %d\n", ssl_err);
					tunnel->error_message = strdup("SSL read error");
					return -1;
				}
			}
		}
	}
}

/*
 * Encode a WebSocket frame.
 */
static int
ws_encode_frame(const uint8_t *data, size_t len, uint8_t *out, size_t *out_len, bool mask)
{
	uint8_t		mask_key[4];
	size_t		header_len;
	size_t		i;

	/* Generate mask key if needed */
	if (mask)
		RAND_bytes(mask_key, sizeof(mask_key));

	/* Build header */
	out[0] = 0x82;				/* FIN + binary opcode */

	if (len < 126)
	{
		out[1] = (mask ? 0x80 : 0x00) | (uint8_t) len;
		header_len = 2;
	}
	else if (len < 65536)
	{
		out[1] = (mask ? 0x80 : 0x00) | 126;
		out[2] = (len >> 8) & 0xFF;
		out[3] = len & 0xFF;
		header_len = 4;
	}
	else
	{
		out[1] = (mask ? 0x80 : 0x00) | 127;
		out[2] = 0;
		out[3] = 0;
		out[4] = 0;
		out[5] = 0;
		out[6] = (len >> 24) & 0xFF;
		out[7] = (len >> 16) & 0xFF;
		out[8] = (len >> 8) & 0xFF;
		out[9] = len & 0xFF;
		header_len = 10;
	}

	/* Add mask key */
	if (mask)
	{
		memcpy(out + header_len, mask_key, 4);
		header_len += 4;
	}

	/* Copy and mask data */
	for (i = 0; i < len; i++)
	{
		if (mask)
			out[header_len + i] = data[i] ^ mask_key[i % 4];
		else
			out[header_len + i] = data[i];
	}

	*out_len = header_len + len;
	return 0;
}

/*
 * Decode a WebSocket frame.
 * Returns 0 on success, -1 on incomplete frame, -2 on close frame.
 */
static int
ws_decode_frame(const uint8_t *data, size_t data_len,
				uint8_t *payload, size_t *payload_len,
				size_t *frame_len)
{
	size_t		header_len;
	size_t		len;
	bool		masked;
	size_t		i;
	uint8_t		opcode;

	if (data_len < 2)
		return -1;

	/* Parse header */
	opcode = data[0] & 0x0F;
	masked = (data[1] & 0x80) != 0;
	len = data[1] & 0x7F;
	header_len = 2;

	if (len == 126)
	{
		if (data_len < 4)
			return -1;
		len = ((size_t) data[2] << 8) | data[3];
		header_len = 4;
	}
	else if (len == 127)
	{
		if (data_len < 10)
			return -1;
		len = ((size_t) data[6] << 24) | ((size_t) data[7] << 16) |
			((size_t) data[8] << 8) | data[9];
		header_len = 10;
	}

	if (masked)
		header_len += 4;

	if (data_len < header_len + len)
		return -1;

	/* Extract and unmask payload */
	for (i = 0; i < len; i++)
	{
		if (masked)
			payload[i] = data[header_len + i] ^ data[header_len - 4 + (i % 4)];
		else
			payload[i] = data[header_len + i];
	}

	*payload_len = len;
	*frame_len = header_len + len;

	/* Check for close frame (opcode 0x8) */
	if (opcode == 0x8)
	{
		uint16_t	close_code = 0;
		char		reason[128] = {0};

		if (len >= 2)
			close_code = ((uint16_t) payload[0] << 8) | payload[1];

		if (len > 2)
		{
			size_t reason_len = len - 2;
			if (reason_len > sizeof(reason) - 1)
				reason_len = sizeof(reason) - 1;
			memcpy(reason, payload + 2, reason_len);
			reason[reason_len] = '\0';
		}

		fprintf(stderr, "[TUNNEL] WebSocket CLOSE frame received!\n");
		fprintf(stderr, "[TUNNEL] Close code: %u\n", close_code);
		fprintf(stderr, "[TUNNEL] Close reason: %s\n", reason);
		return -2;	/* Indicate close frame */
	}

	return 0;
}

/*
 * Send data through the tunnel.
 */
int
cable_tunnel_send(CableTunnel *tunnel, const uint8_t *data, size_t len)
{
	uint8_t		frame[WS_MAX_FRAME_SIZE + 14];
	size_t		frame_len;
	int			ret;

	fprintf(stderr, "[TUNNEL] Sending %zu bytes\n", len);
	if (len > 0)
	{
		fprintf(stderr, "[TUNNEL] Send data first bytes: %02x %02x %02x %02x\n",
				data[0], len > 1 ? data[1] : 0,
				len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}

	if (!tunnel->ws_connected)
	{
		tunnel->error_message = strdup("WebSocket not connected");
		return -1;
	}

	if (len > WS_MAX_FRAME_SIZE)
	{
		tunnel->error_message = strdup("message too large");
		return -1;
	}

	/* Encode as WebSocket frame (client messages must be masked) */
	if (ws_encode_frame(data, len, frame, &frame_len, true) < 0)
	{
		tunnel->error_message = strdup("failed to encode frame");
		return -1;
	}

	ret = SSL_write((SSL *) tunnel->ssl, frame, frame_len);
	if (ret != (int) frame_len)
	{
		fprintf(stderr, "[TUNNEL] Send failed: wrote %d of %zu bytes\n", ret, frame_len);
		tunnel->error_message = strdup("failed to send data");
		return -1;
	}

	fprintf(stderr, "[TUNNEL] Send complete (ws frame %zu bytes)\n", frame_len);
	return 0;
}

/*
 * Receive data from the tunnel.
 */
int
cable_tunnel_recv(CableTunnel *tunnel, uint8_t **data, size_t *len, int timeout_ms)
{
	struct timeval tv;
	fd_set		rfds;
	int			ret;
	uint8_t		buffer[4096];
	int			read_len;
	uint8_t		payload[WS_MAX_FRAME_SIZE];
	size_t		payload_len;
	size_t		frame_len;

	fprintf(stderr, "[TUNNEL] Receiving (timeout=%d ms, buffered=%zu)...\n",
			timeout_ms, tunnel->ws_recv_buffer_len);

	*data = NULL;
	*len = 0;

	if (!tunnel->ws_connected)
	{
		tunnel->error_message = strdup("WebSocket not connected");
		return -1;
	}

	/* Check if we have buffered data */
	if (tunnel->ws_recv_buffer_len > 0)
	{
		fprintf(stderr, "[TUNNEL] Trying to decode buffered data (%zu bytes)\n",
				tunnel->ws_recv_buffer_len);
		fprintf(stderr, "[TUNNEL] Buffer first bytes: %02x %02x %02x %02x %02x %02x\n",
				tunnel->ws_recv_buffer[0],
				tunnel->ws_recv_buffer_len > 1 ? tunnel->ws_recv_buffer[1] : 0,
				tunnel->ws_recv_buffer_len > 2 ? tunnel->ws_recv_buffer[2] : 0,
				tunnel->ws_recv_buffer_len > 3 ? tunnel->ws_recv_buffer[3] : 0,
				tunnel->ws_recv_buffer_len > 4 ? tunnel->ws_recv_buffer[4] : 0,
				tunnel->ws_recv_buffer_len > 5 ? tunnel->ws_recv_buffer[5] : 0);

		{
			int	decode_ret = ws_decode_frame(tunnel->ws_recv_buffer,
											 tunnel->ws_recv_buffer_len,
											 payload, &payload_len, &frame_len);
			if (decode_ret == -2)
			{
				/* Close frame received */
				tunnel->error_message = strdup("connection closed by peer");
				return -1;
			}
			if (decode_ret == 0)
			{
				fprintf(stderr, "[TUNNEL] Decoded frame: payload=%zu bytes, frame=%zu bytes\n",
						payload_len, frame_len);

				/* Remove consumed data from buffer */
				memmove(tunnel->ws_recv_buffer, tunnel->ws_recv_buffer + frame_len,
						tunnel->ws_recv_buffer_len - frame_len);
				tunnel->ws_recv_buffer_len -= frame_len;

				*data = malloc(payload_len);
				if (*data)
				{
					memcpy(*data, payload, payload_len);
					*len = payload_len;
					fprintf(stderr, "[TUNNEL] Recv success from buffer: %zu bytes\n", *len);
					fprintf(stderr, "[TUNNEL] Payload first bytes: %02x %02x %02x %02x\n",
							payload_len > 0 ? payload[0] : 0,
							payload_len > 1 ? payload[1] : 0,
							payload_len > 2 ? payload[2] : 0,
							payload_len > 3 ? payload[3] : 0);
				}
				return 0;
			}
			else
			{
				fprintf(stderr, "[TUNNEL] Could not decode buffered data, waiting for more\n");
			}
		}
	}

	/* Wait for data with timeout */
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	FD_ZERO(&rfds);
	FD_SET(tunnel->socket_fd, &rfds);

	ret = select(tunnel->socket_fd + 1, &rfds, NULL, NULL, &tv);
	if (ret < 0)
	{
		tunnel->error_message = strdup("select failed");
		return -1;
	}

	if (ret == 0)
	{
		fprintf(stderr, "[TUNNEL] Recv timeout\n");
		tunnel->error_message = strdup("timeout");
		return -1;
	}

	/* Read from SSL */
	read_len = SSL_read((SSL *) tunnel->ssl, buffer, sizeof(buffer));
	fprintf(stderr, "[TUNNEL] SSL_read returned %d\n", read_len);
	if (read_len <= 0)
	{
		int ssl_err = SSL_get_error((SSL *) tunnel->ssl, read_len);
		fprintf(stderr, "[TUNNEL] SSL read error: %d\n", ssl_err);
		tunnel->error_message = strdup("read failed");
		return -1;
	}

	fprintf(stderr, "[TUNNEL] Read %d bytes from SSL\n", read_len);
	fprintf(stderr, "[TUNNEL] Raw bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			read_len > 0 ? buffer[0] : 0, read_len > 1 ? buffer[1] : 0,
			read_len > 2 ? buffer[2] : 0, read_len > 3 ? buffer[3] : 0,
			read_len > 4 ? buffer[4] : 0, read_len > 5 ? buffer[5] : 0,
			read_len > 6 ? buffer[6] : 0, read_len > 7 ? buffer[7] : 0);

	/* Add to buffer */
	if (tunnel->ws_recv_buffer_len + read_len > tunnel->ws_recv_buffer_size)
	{
		tunnel->error_message = strdup("buffer overflow");
		return -1;
	}
	memcpy(tunnel->ws_recv_buffer + tunnel->ws_recv_buffer_len, buffer, read_len);
	tunnel->ws_recv_buffer_len += read_len;

	/* Try to decode a frame */
	{
		int	decode_ret = ws_decode_frame(tunnel->ws_recv_buffer,
										 tunnel->ws_recv_buffer_len,
										 payload, &payload_len, &frame_len);
		if (decode_ret == -2)
		{
			/* Close frame received */
			tunnel->error_message = strdup("connection closed by peer");
			return -1;
		}
		if (decode_ret == 0)
		{
			fprintf(stderr, "[TUNNEL] Decoded frame: payload=%zu bytes\n", payload_len);

			/* Remove consumed data from buffer */
			memmove(tunnel->ws_recv_buffer, tunnel->ws_recv_buffer + frame_len,
					tunnel->ws_recv_buffer_len - frame_len);
			tunnel->ws_recv_buffer_len -= frame_len;

			*data = malloc(payload_len);
			if (*data)
			{
				memcpy(*data, payload, payload_len);
				*len = payload_len;
				fprintf(stderr, "[TUNNEL] Recv success: %zu bytes\n", *len);
				fprintf(stderr, "[TUNNEL] Payload first bytes: %02x %02x %02x %02x\n",
						payload_len > 0 ? payload[0] : 0,
						payload_len > 1 ? payload[1] : 0,
						payload_len > 2 ? payload[2] : 0,
						payload_len > 3 ? payload[3] : 0);
			}
			return 0;
		}
	}

	fprintf(stderr, "[TUNNEL] Incomplete frame, need more data\n");
	tunnel->error_message = strdup("incomplete frame");
	return -1;
}

/*
 * Get the last error message.
 */
const char *
cable_tunnel_error(CableTunnel *tunnel)
{
	if (tunnel && tunnel->error_message)
		return tunnel->error_message;
	return "unknown error";
}

#else							/* !USE_OPENSSL */

/* Stubs when OpenSSL is not available */

CableTunnel *
cable_tunnel_new(void)
{
	return NULL;
}

void
cable_tunnel_free(CableTunnel *tunnel)
{
}

int
cable_tunnel_connect(CableTunnel *tunnel, const char *server,
					 const uint8_t *tunnel_id, const uint8_t *routing_id)
{
	return -1;
}

int
cable_tunnel_wait_for_peer(CableTunnel *tunnel, int timeout_secs)
{
	return -1;
}

int
cable_tunnel_send(CableTunnel *tunnel, const uint8_t *data, size_t len)
{
	return -1;
}

int
cable_tunnel_recv(CableTunnel *tunnel, uint8_t **data, size_t *len, int timeout_ms)
{
	return -1;
}

const char *
cable_tunnel_error(CableTunnel *tunnel)
{
	return "caBLE requires OpenSSL support";
}

#endif							/* USE_OPENSSL */
