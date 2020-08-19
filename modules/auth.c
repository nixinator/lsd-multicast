/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2020 Brett Sheffield <bacs@librecast.net> */

#include "auth.h"
#include "../src/config.h"
#include "../src/log.h"
#include "../src/wire.h"
#include <assert.h>
#include <curl/curl.h>
#include <librecast.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* TODO: from config */
#define FROM "noreply@librecast.net"

lc_ctx_t *lctx;

void hash_field(unsigned char *hash, size_t hashlen,
		const char *key, size_t keylen,
		const char *fld, size_t fldlen)
{
	crypto_generichash_state state;
	crypto_generichash_init(&state, NULL, 0, hashlen);
	crypto_generichash_update(&state, (unsigned char *)key, keylen);
	crypto_generichash_update(&state, (unsigned char *)fld, fldlen);
	crypto_generichash_final(&state, hash, hashlen);
}

lc_ctx_t *auth_init()
{
	lctx = lc_ctx_new();
	handler_t *h = config.handlers;
	if (h && h->dbpath) {
		if (mkdir(h->dbpath, S_IRWXU) == -1 && errno != EEXIST) {
			ERROR("can't create database path '%s': %s", h->dbpath, strerror(errno));
		}
		lc_db_open(lctx, h->dbpath);
	}
	return lctx;
}

void auth_free()
{
	lc_ctx_free(lctx);
}

int auth_field_get(char *key, size_t keylen, char *field, void *data, size_t *datalen)
{
	int ret = 0;
	unsigned char hash[crypto_generichash_BYTES] = "";
	hash_field(hash, sizeof hash, key, keylen, field, strlen(field));
	if ((ret = lc_db_get(lctx, config.handlers->dbname, hash, sizeof hash, data, datalen))) {
		errno = ret;
		ret = -1;
	}
	return ret;
}

int auth_field_getv(char *key, size_t keylen, char *field, struct iovec *data)
{
	return auth_field_get(key, keylen, field, &data->iov_base, &data->iov_len);
}

int auth_field_set(char *key, size_t keylen, const char *field, void *data, size_t datalen)
{
	unsigned char hash[crypto_generichash_BYTES];
	hash_field(hash, sizeof hash, key, keylen, field, strlen(field));
	return lc_db_set(lctx, config.handlers->dbname, hash, sizeof hash, data, datalen);
}

int auth_user_pass_set(char *userid, struct iovec *pass)
{
	char pwhash[crypto_pwhash_STRBYTES];
	if (crypto_pwhash_str(pwhash, pass->iov_base, pass->iov_len,
			crypto_pwhash_OPSLIMIT_INTERACTIVE,
			crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
	{
		ERROR("crypto_pwhash() error");
		return -1;
	}
	return auth_field_set(userid, AUTH_HEXLEN, "pass", pwhash, sizeof pwhash);
}

int auth_user_create(char *userid, struct iovec *mail, struct iovec *pass)
{
	struct iovec nopass = {0};
	if (!auth_valid_email(mail->iov_base, mail->iov_len))
		return -1;

	/* we don't do any strength checking on passwords here
	 * save that for the UI where we can give proper feedback */
	if (pass && !pass->iov_len) return -1;
	if (!pass) pass = &nopass;

	unsigned char userid_bytes[crypto_box_PUBLICKEYBYTES];
	randombytes_buf(userid_bytes, sizeof userid_bytes);
	sodium_bin2hex(userid, AUTH_HEXLEN, userid_bytes, sizeof userid_bytes);
	DEBUG("userid created: %s", userid);
	if (auth_user_pass_set(userid, pass)) {
		ERROR("failed to set password");
		return -1;
	}
	auth_field_set(userid, AUTH_HEXLEN, "mail", mail->iov_base, mail->iov_len);
	auth_field_set(mail->iov_base, mail->iov_len, "user", userid, AUTH_HEXLEN);
	return 0;
}

int auth_user_bymail(struct iovec *mail, struct iovec *userid)
{
	DEBUG("searching for mail: %.*s", (int)mail->iov_len, (char *)mail->iov_base);
	return auth_field_get(mail->iov_base, mail->iov_len, "user",
			&userid->iov_base,
			&userid->iov_len);
}

/* minimal email verification - our smtp server will do the rest */
int auth_valid_email(char *mail, size_t len)
{
	char *at, *end;
	end = mail + len;
	if (len < 3) return 0;		/* too short at least 3 chars eg. a@b */
	mail++; len--;			/* must have at least one char for local part */
	at = memchr(mail, '@', len);
	if (!at) return 0;		/* check for '@' */
	if (at + 1 >= end) return 0;	/* no domain part */
	return 1;
}

static int auth_mail_token(char *subject, char *to, char *token)
{
	char filename[] = "/tmp/lsd-auth-mail-XXXXXX";
	FILE *f;
	time_t t;
	int ret = 0;
	int fd;

	if ((fd = mkstemp(filename)) == -1) {
		ERROR("error creating tempfile: %s", strerror(errno));
		return -1;
	}
	if ((f = fdopen(fd, "w")) == NULL) {
		close(fd);
		return -1;
	}

	t = time(NULL);
	char ts[40];
	char welcometext[] = "You (or someone on your behalf) has signed up to Librecast Live using this email address.  To verify your address, please click the following link\r\n";
	strftime(ts, sizeof ts, "%a, %d %b %Y %T %z", localtime(&t));
	fprintf(f, "Date: %s\r\n", ts);
	fprintf(f, "From: %s\r\n", FROM); /* TODO: from config */
	fprintf(f, "To: Librecast Live <%s>\r\n", to);
	fprintf(f, "Subject: %s\r\n", subject);
	fprintf(f, "\r\n"); /* blank line */
	fprintf(f, "%s", welcometext); /* TODO: from config */
	fprintf(f, "    https://live.librecast.net/verifyemail/%s\r\n", token);
	fprintf(f, "We look forward to you joining us soon!\r\n");
	fflush(f); rewind(f);

	CURL *curl = NULL;
	CURLcode res = CURLE_OK;
	struct curl_slist *recipients = NULL;
	if (curl_global_init(CURL_GLOBAL_ALL)) {
		goto exit_err;
		ret = -1;
	}
	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, "smtp://smtp.gladserv.com:25"); /* FIXME config */
		curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
		curl_easy_setopt(curl, CURLOPT_MAIL_FROM, FROM);
		recipients = curl_slist_append(recipients, to);
		DEBUG("to: %s", to);
		if (!recipients) {
			ERROR("unable to append recipients");
			ret = -1;
			goto cleanup;
		}
		curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
		curl_easy_setopt(curl, CURLOPT_READDATA, f);
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		if (config.debug) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		if (curl_easy_perform(curl) != CURLE_OK) {
			ERROR("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
			ret = -1;
		}
cleanup:
		curl_slist_free_all(recipients);
		curl_easy_cleanup(curl);
	}
	curl_global_cleanup();
exit_err:
	fclose(f);
	close(fd);
	unlink(filename);
	return ret;
}

int auth_decode_packet(lc_message_t *msg, auth_payload_t *payload)
{
	/* unpack outer packet [opcode][flags] + [public key][nonce][payload] */
	DEBUG("auth module unpacking outer packet of %zu bytes", msg->len);
	enum { /* outer fields */
		fld_key,
		fld_nonce,
		fld_payload,
		outerfields
	};
	struct iovec pkt = { .iov_base = msg->data, .iov_len = msg->len };
	uint8_t op, flags;
	struct iovec outer[outerfields] = {0};

	if (wire_unpack(&pkt, outer, outerfields, &op, &flags) == -1) {
		errno = EBADMSG;
		return -1;
	}
	/* outer fields are all required */
	if ((outer[fld_key].iov_len != crypto_box_PUBLICKEYBYTES)
	||  (outer[fld_nonce].iov_len != crypto_box_NONCEBYTES)
	||  (outer[fld_payload].iov_len < 1)) {
		errno = EBADMSG;
		return -1;
	}
	DEBUG("auth module decrypting contents");
	if (sodium_init() == -1) {
		ERROR("error initalizing libsodium");
		return -1;
	}

	unsigned char data[outer[fld_payload].iov_len - crypto_box_MACBYTES];
	unsigned char privatekey[crypto_box_SECRETKEYBYTES];
	payload->senderkey = outer[fld_key].iov_base;
	unsigned char *nonce = outer[fld_nonce].iov_base;
	sodium_hex2bin(privatekey,
			crypto_box_SECRETKEYBYTES,
			config.handlers->key_private,
			crypto_box_SECRETKEYBYTES * 2,
			NULL,
			0,
			NULL);
	memset(data, 0, sizeof data);
	if (crypto_box_open_easy(data,
				outer[fld_payload].iov_base,
				outer[fld_payload].iov_len,
				nonce, payload->senderkey, privatekey) != 0)
	{
		ERROR("packet decryption failed");
		return -1;
	}
	DEBUG("auth module decryption successful");

	/* unpack inner data fields */
	DEBUG("auth module unpacking fields");
	struct iovec clearpkt = {0};
	clearpkt.iov_base = data;
	clearpkt.iov_len = outer[fld_payload].iov_len - crypto_box_MACBYTES;
	if (wire_unpack(&clearpkt,
			payload->fields,
			payload->fieldcount,
			&payload->opcode,
			&payload->flags) == -1)
	{
		return -1;
	}
	DEBUG("wire_unpack() fieldcount: %i", payload->fieldcount);
	DEBUG("wire_unpack() done, dumping fields...");

	for (int i = 1; i < payload->fieldcount; i++) {
		DEBUG("[%i] %zu bytes", i, payload->fields[i].iov_len);
	}
	for (int i = 1; i < payload->fieldcount; i++) {
		DEBUG("[%i] %.*s", i, (int)payload->fields[i].iov_len, (char *)payload->fields[i].iov_base);
	}

	return 0;
}

int auth_user_pass_verify(struct iovec *user, struct iovec *pass)
{
	int ret = 0;
	struct iovec pwhash = {0};
	struct iovec *pw = &pwhash;
	struct iovec nopass = { .iov_base = "*", .iov_len = 1 };
	if (auth_field_getv(user->iov_base, AUTH_HEXLEN, "pass", &pwhash))
	{
		DEBUG("unable to find password for user");
		pw = &nopass; /* preserve constant time */
	}
	else if (pw->iov_len == 0) {
		DEBUG("zero length password");
		pw = &nopass; /* preserve constant time */
	}
	if (crypto_pwhash_str_verify(pw->iov_base, pass->iov_base, pass->iov_len) != 0) {
		DEBUG("password verification failed");
		errno = EACCES;
		ret = -1;
	}
	free(pwhash.iov_base);
	return ret;
}

int auth_serv_token_get(struct iovec *tok, struct iovec *user, struct iovec *pass, struct iovec *serv)
{
	/* TODO: create token with:
	 * - senderkey
	 * - resource / service
	 * - flags (read / write etc)
	 * - signed
	 */

	return 0;
}

int auth_user_token_new(auth_user_token_t *token, auth_payload_t *payload)
{
	if (config.testmode) {
		DEBUG("auth_user_token_new(): test mode enabled");
		unsigned char seed[randombytes_SEEDBYTES];
		memcpy(seed, payload->senderkey, randombytes_SEEDBYTES);
		randombytes_buf_deterministic(token->token, sizeof token->token, seed);
	}
	else randombytes_buf(token->token, sizeof token->token);
	sodium_bin2hex(token->hextoken, AUTH_HEXLEN, token->token, sizeof token->token);
	token->expires = htobe64((uint64_t)time(NULL) + 60 * 15); /* expires in 15 minutes */
	DEBUG("token created: %s", token->hextoken);
	return 0;
}

int auth_user_token_set(char *userid, auth_user_token_t *token)
{
	if (auth_field_set(token->hextoken, AUTH_HEXLEN, "user", userid, AUTH_HEXLEN)) {
		DEBUG ("error setting user token");
		return -1;
	}
	if (auth_field_set(token->hextoken, AUTH_HEXLEN, "expires",
			&token->expires, sizeof token->expires))
	{
		DEBUG ("error setting user token expiry");
		return -1;
	}
	return 0;
}

int auth_user_token_use(struct iovec *token, struct iovec *pass)
{
	int ret = 0;
	struct iovec user = {0};
	struct iovec expires = {0};
	auth_user_token_t tok = {0};
	DEBUG("search for token '%.*s'", (int)token->iov_len, (char *)token->iov_base);
	if (auth_field_getv(token->iov_base, AUTH_HEXLEN, "user", &user)) {
		DEBUG ("user token not found");
		return -1;
	}
	if (auth_field_getv(token->iov_base, AUTH_HEXLEN, "expires", &expires)) {
		DEBUG ("user token expiry not found");
		return -1;
	}
	tok.expires = *((uint64_t *)expires.iov_base);
	free(expires.iov_base);
	if (!auth_user_token_valid(&tok)) {
		return -1;
	}
	char *userid = strndup(user.iov_base, user.iov_len);
	if (auth_user_pass_set(userid, pass))
		ret = -1;
	free(userid);
	free(user.iov_base);

	/* TODO: delete token */

	return ret;
}

int auth_user_token_valid(auth_user_token_t *token)
{
	return (be64toh(token->expires) <= time(NULL) + 60 * 15);
}

static void auth_op_noop(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_user_add(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
	enum {
		repl,
		user,
		mail,
		pass,
		serv,
		fieldcount
	};
	struct iovec fields[fieldcount] = {0};
	auth_payload_t p = { .fields = fields, .fieldcount = fieldcount };
	lc_socket_t *sock = NULL;
	lc_channel_t *chan = NULL;
	lc_message_t response = {0};

	/* FIXME: must have keys to continue */

	if (auth_decode_packet(msg, &p) == -1) {
		perror("auth_decode_packet()");
		return;
	}
	if (!auth_valid_email(fields[mail].iov_base, fields[mail].iov_len)) {
		ERROR("invalid email address");
		return;
	}

	char userid[AUTH_HEXLEN];
	auth_user_create(userid, &fields[mail], &fields[pass]);
	auth_user_token_t token;
	auth_user_token_new(&token, &p);
	auth_user_token_set(userid, &token);
	DEBUG("user created");

	/* TODO: logfile entry */


	DEBUG("emailing token");
	if (!config.testmode) {
		char *to = strndup(fields[mail].iov_base, fields[mail].iov_len);
		char subject[] = "Librecast Live - Confirm Your Email Address";
		if (auth_mail_token(subject, to, token.hextoken) == -1) {
			ERROR("error in auth_mail_token()");
		}
		else {
			DEBUG("email sent");
		}
		free(to);
	}

	DEBUG("response to requestor");
	sock = lc_socket_new(lctx);
	chan = lc_channel_nnew(lctx, p.senderkey, crypto_box_PUBLICKEYBYTES);
	lc_channel_bind(sock, chan);
	lc_msg_init_size(&response, 2); /* just an opcode + flag really */
	((uint8_t *)response.data)[0] = AUTH_OP_NOOP;	/* TODO: response opcode */
	((uint8_t *)response.data)[1] = 7;		/* TODO: define response codes */
	int opt = 1; /* set loopback in case we're on the same host as the sender */
	lc_socket_setopt(sock, IPV6_MULTICAST_LOOP, &opt, sizeof(opt));
	lc_msg_send(chan, &response);
	lc_msg_free(&response);
};

static void auth_op_user_delete(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_user_lock(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_user_unlock(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_key_add(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_key_delete(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_key_replace(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
};

static void auth_op_auth_service(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);
	enum {
		repl,
		user,
		mail,
		pass,
		serv,
		fieldcount
	};
	struct iovec fields[fieldcount] = {0};
	auth_payload_t p = {0};
	p.fields = fields;
	p.fieldcount = fieldcount;
	int state;
	//lc_socket_t *sock = NULL;
	//lc_channel_t *chan = NULL;
	//lc_message_t response = {0};
	handler_t *h = config.handlers;

	/* FIXME: must have keys to continue */

	if (auth_decode_packet(msg, &p) == -1) {
		perror("auth_decode_packet()");
		return;
	}

	/* hash password to compare */
#if 0
	char pwhash[crypto_pwhash_STRBYTES] = "";
	if (crypto_pwhash_str(pwhash, fields[pass].iov_base, fields[pass].iov_len,
			crypto_pwhash_OPSLIMIT_INTERACTIVE,
			crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
	{
		ERROR("crypto_pwhash() error");
	}
#endif
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state);
	if (mkdir(h->dbpath, S_IRWXU) == -1 && errno != EEXIST) {
		ERROR("can't create database path '%s': %s", h->dbpath, strerror(errno));
	}


	//unsigned char hash[AUTH_HEXLEN] = "";
	void *vptr = NULL;
	size_t vlen;

	/* find userid for email */
	if (auth_field_get(fields[mail].iov_base, fields[mail].iov_len, "user", &vptr, &vlen)) {
		ERROR("invalid mail");
	}
	else {
		DEBUG("got userid '%.*s' for email '%.*s'", vptr, vlen,
			fields[mail].iov_base, fields[mail].iov_len);
		free(vptr);
	}

	/* TODO: fetch password from database */
	/* TODO: check password */
	/* TODO: create capability token */
	/* TODO: logfile entry */
	/* TODO: reply to reply address */

	pthread_setcancelstate(state, NULL);

};

void init(config_t *c)
{
	TRACE("auth.so %s()", __func__);
	if (c) config = *c;
	DEBUG("I am the very model of a modern auth module");
	auth_init();
}

void finit(void)
{
	TRACE("auth.so %s()", __func__);
	config_free();
	auth_free();
}

void handle_msg(lc_message_t *msg)
{
	TRACE("auth.so %s()", __func__);

	DEBUG("%zu bytes received", msg->len);

	/* TODO: read opcode and pass to handler */
	uint8_t opcode = ((uint8_t *)msg->data)[0];
	uint8_t flags = ((uint8_t *)msg->data)[1];
	DEBUG("opcode read: %u", opcode);
	DEBUG("flags read: %u", flags);

	switch (opcode) {
		AUTH_OPCODES(AUTH_OPCODE_FUN)
	default:
		ERROR("Invalid auth opcode received: %u", opcode);
	}

	DEBUG("handle_msg() - after the handler");

	//lc_ctx_t *lctx = lc_ctx_new();
	//lc_ctx_t *lctx = lc_channel_ctx(msg->chan);
	//lc_socket_t *sock = lc_channel_socket(msg->chan);
	//lc_socket_t *sock = lc_socket_new(lctx);
	//lc_channel_t *chan_repl = lc_channel_new(lctx, "repl");
	//DEBUG("auth.so binding socket");
	//lc_channel_bind(sock, chan_repl);
	//lc_msg_send(chan_repl, msg);

	/* TODO:
	 * - decrypt
	 * - call opcode handler
	 * - unpack
	 */

	//lc_channel_unbind(chan_repl);

	//DEBUG("message says '%.*s'", (int)msg->len, (char *)msg->data);
}

void handle_err(int err)
{
	TRACE("auth.so %s()", __func__);
	DEBUG("handle_err() err=%i", err);
}
