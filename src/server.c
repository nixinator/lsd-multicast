/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2020 Brett Sheffield <bacs@librecast.net> */

#include <librecast.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "log.h"
#include "server.h"

lc_ctx_t *lctx;
lc_socket_t *sock;
lc_channel_t *chan;

void *server_message_recv(lc_message_t *msg)
{
	DEBUG("server received message");
}

void server_stop()
{
	DEBUG("Stopping server");
	lc_socket_listen_cancel(sock);
	lc_channel_free(chan);
	lc_socket_close(sock);
	lc_ctx_free(lctx);
}

int server_start()
{
	DEBUG("Starting server");
	lctx = lc_ctx_new();
	sock = lc_socket_new(lctx);
	chan = lc_channel_new(lctx, "radio freedom");
	lc_channel_bind(sock, chan);
	lc_channel_join(chan);
	lc_socket_listen(sock, server_message_recv, NULL);
	return 0;
}
