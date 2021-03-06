/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2020 Brett Sheffield <bacs@librecast.net> */

#include "test.h"
#include "../modules/auth.h"
#include "../src/config.h"
#include "../src/server.h"
#include "../src/wire.h"
#include <assert.h>
#include <librecast.h>
#include <pthread.h>
#include <signal.h>
#include <sodium.h>
#include <unistd.h>

lc_ctx_t *lctx = NULL;

void *testthread(void *arg)
{
	test_log("test thread starting");
	lc_socket_t *sock, *sock_repl;
	lc_channel_t *chan, *chan_repl;
	lc_message_t msg, msg_repl;
	int opt = 1;
	struct iovec data;
	handler_t *h = config.handlers;
	test_assert(h != NULL, "handlers");

	/* generate keypair & use as reply address */
	unsigned char pk[crypto_box_PUBLICKEYBYTES];
	unsigned char sk[crypto_box_SECRETKEYBYTES];
	test_assert(sodium_init() != -1, "sodium_init()");
	test_assert(crypto_box_keypair(pk, sk) != -1, "crypto_box_keypair()");

	/* (1) build packet */
	unsigned char localpart[8];
	char localparthex[17];
	char emailaddr[31] = "XXXXXXXXXXXXXXXX@librecast.net";
	randombytes_buf(localpart, 8);
	sodium_bin2hex(localparthex, 17, localpart, 8);
	memcpy(emailaddr, localparthex, 15);
	struct iovec repl = { .iov_base = pk, .iov_len = crypto_box_PUBLICKEYBYTES };
	struct iovec user = { .iov_base = "username" };
	struct iovec mail = { .iov_base = emailaddr };
	struct iovec pass = { .iov_base = "password" };
	struct iovec serv = { .iov_base = "service" };
	struct iovec iovs[] = { repl, user, mail, pass, serv };
	const int iov_count = sizeof iovs / sizeof iovs[0];
	uint8_t op = AUTH_OP_USER_ADD;
	uint8_t flags = 9;
	ssize_t len;
	for (int i = 1; i < iov_count; i++) {
		iovs[i].iov_len = strlen(iovs[i].iov_base);
	}
	len = wire_pack_pre(&data, iovs, iov_count, NULL, 0);
	test_assert(len > 0, "wire_pack() returned %i", len);

	/* (2) encrypt packet */
	char *authpubhex = h->key_public;
	unsigned char authpubkey[crypto_box_PUBLICKEYBYTES];
	unsigned char nonce[crypto_box_NONCEBYTES];
	const size_t cipherlen = crypto_box_MACBYTES + data.iov_len;
	unsigned char ciphertext[cipherlen];

	sodium_hex2bin(authpubkey,
			crypto_box_PUBLICKEYBYTES,
			authpubhex,
			crypto_box_PUBLICKEYBYTES * 2,
			NULL,
			0,
			NULL);
	randombytes_buf(nonce, sizeof nonce);
	test_assert(!crypto_box_easy(ciphertext, (unsigned char *)data.iov_base, data.iov_len, nonce, authpubkey, sk), "crypto_box_easy()");

	/* (2b) now pack encrypted payload with public key and nonce prepended */
	struct iovec iovkey = { .iov_base = pk, .iov_len = crypto_box_PUBLICKEYBYTES };
	struct iovec iovnon = { .iov_base = nonce, .iov_len = crypto_box_NONCEBYTES };
	struct iovec crypted = { .iov_base = ciphertext, .iov_len = cipherlen };
	struct iovec *payload[] = { &iovkey, &iovnon, &crypted };
	struct iovec pkt;
	len = wire_pack(&pkt, *payload, 3, op, flags);

	/* (3) bind to send/receive channels, join recv channel */
	lctx = lc_ctx_new();
	sock = lc_socket_new(lctx);
	sock_repl = lc_socket_new(lctx);
	lc_socket_setopt(sock, IPV6_MULTICAST_LOOP, &opt, sizeof(opt));
	chan = lc_channel_new(lctx, authpubhex);
	chan_repl = lc_channel_nnew(lctx, pk, crypto_box_PUBLICKEYBYTES);
	lc_channel_bind(sock, chan);
	lc_channel_bind(sock_repl, chan_repl);
	lc_channel_join(chan_repl);

	/* (4) send packet */
	lc_msg_init_data(&msg, pkt.iov_base, pkt.iov_len, NULL, NULL);
	test_log("packed %zu bytes ready to send", data.iov_len);
	lc_msg_send(chan, &msg);
	free(pkt.iov_base);
	free(data.iov_base);

	/* (5) await reply */
	lc_msg_recv(sock_repl, &msg_repl);
	test_assert(msg_repl.len > 0, "message has nonzero length");
	test_assert(((uint8_t *)msg_repl.data)[0] == AUTH_OP_USER_ADD, "opcode");
	test_assert(((uint8_t *)msg_repl.data)[1] == 0, "flags");
	lc_msg_free(&msg_repl);

	/* TODO: (6) decrypt reply */
	/* TODO: (7) handle response/error */

	/*FIXME: temp - check token */
#if 0
	unsigned char token[crypto_box_PUBLICKEYBYTES];
	unsigned char seed[randombytes_SEEDBYTES];
	const size_t hexlen = crypto_box_PUBLICKEYBYTES * 2 + 1;
	char hextoken[hexlen];
	memcpy(seed, pk, randombytes_SEEDBYTES);
	randombytes_buf_deterministic(token, sizeof token, seed);
	sodium_bin2hex(hextoken, hexlen, token, sizeof token);
	test_log("test runner token: %s", hextoken);
#endif
	/* we finished quickly, wake the test runner */
	pthread_kill(*((pthread_t *)arg), SIGINT);
	test_log("test thread exiting");
	pthread_exit(arg);
}

void sighandler(int sig)
{
	test_log("signal caught: %i", sig);
}

void runtests(pid_t pid)
{
	struct timespec t = { 8, 0 }; /* crypto code is slow under valgrind */
	void *ret = NULL;
	struct sigaction sa = { .sa_handler = sighandler };
	sigaction(SIGINT, &sa, NULL);
	pthread_t thread;
	pthread_t self = pthread_self();
	pthread_attr_t attr = {0};
	pthread_attr_init(&attr);
	pthread_create(&thread, &attr, testthread, &self);
	nanosleep(&t, NULL); /* wait for tests to run */
	pthread_cancel(thread);
	pthread_join(thread, &ret);
	test_assert(ret != PTHREAD_CANCELED, "test thread timeout");
	kill(pid, SIGINT); /* stop server */
	lc_ctx_free(lctx);
}

int main()
{
	test_name("auth handler test");
	config_include("./0000-0009.conf");
	pid_t pid = fork();
	assert (pid != -1);
	if (pid)
		runtests(pid);
	else {
		close(1); /* prevent server messing up test output */
		server_start();
	}
	config_free();
	return fails;
}
