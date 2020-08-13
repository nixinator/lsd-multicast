/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2020 Brett Sheffield <bacs@librecast.net> */

#ifndef _LSDM_CONFIG_H
#define _LSDM_CONFIG_H 1

#include "y.tab.h"
#include <librecast/types.h>

#define CONFIG_LOGLEVEL_MAX 127

typedef struct handler_s handler_t;
struct handler_s {
	handler_t *	next;
	char *		channel;
	char *		channelhash;
	char *		dbname;
	char *		dbpath;
	char *		key_private;
	char *		key_public;
	char *		module;
	char *		scope;
	unsigned short  port;
};

typedef struct module_s module_t;
struct module_s {
	char *          name;
	void *          handle;
	int (*		init)(void);
	void (*		finit)(void);
	void (*		handle_msg)(lc_message_t *msg);
	void (*		handle_err)(int);
};

typedef struct config_s config_t;
struct config_s {
	int	daemon;
	int	debug;
	int	loglevel;
	int	modules;
	int	testmode;
	char *	configfile;
	char *	key;
	char *	cert;
	char *	modpath;
	module_t *mods;
	handler_t *handlers;
};
extern config_t config;

void	config_free(void);
int	config_include(char *configfile);
int	config_modules_load(void);
void	config_modules_unload(void);
int	config_parse(void);

#endif /* _LSDM_CONFIG_H */
