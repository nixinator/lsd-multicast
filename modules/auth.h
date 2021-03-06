/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2020 Brett Sheffield <bacs@librecast.net> */

#ifndef _LSDM_AUTH_H
#define _LSDM_AUTH_H 1

#include <errno.h>
#include <librecast/types.h>
#include <limits.h>
#include <sodium.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#define AUTH_TESTMODE 1
#define AUTH_HEXLEN crypto_box_PUBLICKEYBYTES * 2 + 1

#define AUTH_OPCODES(X) \
	X(0x0, AUTH_OP_NOOP,		"NOOP",		auth_op_noop) \
	X(0x1, AUTH_OP_USER_ADD,	"USER_ADD",	auth_op_user_add) \
	X(0x2, AUTH_OP_USER_DEL,	"USER_DEL",	auth_op_noop) \
	X(0x3, AUTH_OP_USER_LOCK,	"USER_LOCK",	auth_op_noop) \
	X(0x4, AUTH_OP_USER_UNLOCK,	"USER_UNLOCK",	auth_op_user_unlock) \
	X(0x5, AUTH_OP_KEY_ADD,		"KEY_ADD",	auth_op_noop) \
	X(0x6, AUTH_OP_KEY_DEL,		"KEY_DEL",	auth_op_noop) \
	X(0x7, AUTH_OP_KEY_REP,		"KEY_REP",	auth_op_noop) \
	X(0x8, AUTH_OP_AUTH_SERV,	"AUTH_SERV",	auth_op_auth_service)
#undef X

#define AUTH_OPCODE_ENUM(code, name, text, f) name = code,
#define AUTH_OPCODE_TEXT(code, name, text, f) case code: return text;
#define AUTH_OPCODE_FUN(code, name, text, f) case code: f(msg); break;
typedef enum {
	AUTH_OPCODES(AUTH_OPCODE_ENUM)
} auth_opcode_t;

#define AUTH_FLD_REPL		0x1
#define AUTH_FLD_USER		0x2
#define AUTH_FLD_MAIL		0x4
#define AUTH_FLD_PASS		0x8
#define AUTH_FLD_SERV		0x16
#define AUTH_FLD_KEY		0x32

/* map fields to opcodes with bitmask */
//#define AUTH_OP_USER_ADD	AUTH_FLD_REPL | AUTH_FLD_USER | AUTH_FLD_MAIL | AUTH_FLD_PASS | AUTH_FLD_SERV

enum {
	AUTH_REPL,
	AUTH_USER,
	AUTH_MAIL,
	AUTH_PASS,
	AUTH_SERV
};

typedef struct auth_payload_s auth_payload_t;
struct auth_payload_s {
	uint8_t		opcode;
	uint8_t		flags;
	struct iovec	senderkey;
	int		pre_count;
	int		fieldcount;
	void		*data;
	struct iovec	*pre;
	struct iovec	*fields;
};

typedef struct auth_user_token_s auth_user_token_t;
struct auth_user_token_s {
	uint64_t	expires;
	char		hextoken[AUTH_HEXLEN];
	unsigned char	token[crypto_box_PUBLICKEYBYTES];
};

extern lc_ctx_t *lctx;

void hash_field(unsigned char *hash, size_t hashlen,
		const char *key, size_t keylen,
		const char *fld, size_t fldlen);
lc_ctx_t *auth_init();
void auth_free();
int auth_field_get(char *key, size_t keylen, char *field, void *data, size_t *datalen);
int auth_field_getv(char *key, size_t keylen, char *field, struct iovec *data);
int auth_field_set(char *key, size_t keylen, const char *field, void *data, size_t datalen);
char *auth_key_sign_pk_hex(char *combokey);
char *auth_key_sign_sk_hex(char *combokey);
unsigned char *auth_key_crypt_pk_bin(unsigned char *binkey, char *combokey);
unsigned char *auth_key_crypt_sk_bin(unsigned char *binkey, char *combokey);
unsigned char *auth_key_sign_pk_bin(unsigned char *binkey, char *combokey);
unsigned char *auth_key_sign_sk_bin(unsigned char *binkey, char *combokey);
int auth_user_create(char *userid, struct iovec *mail, struct iovec *pass);
int auth_user_bymail(struct iovec *mail, struct iovec *userid);
int auth_valid_email(char *mail, size_t len);
int auth_reply(struct iovec *repl, struct iovec *clientkey, struct iovec *data, uint8_t op, uint8_t flags);
int auth_serv_token_new(struct iovec *tok, struct iovec *iov, size_t iovlen);
int auth_serv_token_get(struct iovec *tok, struct iovec *user, struct iovec *pass, struct iovec *serv);
int auth_user_pass_verify(struct iovec *user, struct iovec *pass);
int auth_user_token_new(auth_user_token_t *token, auth_payload_t *payload);
int auth_user_token_set(char *userid, auth_user_token_t *token);
uint8_t auth_user_token_use(struct iovec *token, struct iovec *pass);
int auth_user_token_valid(auth_user_token_t *token);
int auth_decode_packet(lc_message_t *msg, auth_payload_t *payload);
int auth_decode_packet_key(lc_message_t *msg, auth_payload_t *payload, unsigned char *sk);

#endif /* _LSDM_AUTH_H */
