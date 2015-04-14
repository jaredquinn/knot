/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
%{

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "utils/knot1to2/scheme.h"
#include "utils/knot1to2/extra.h"
#include "utils/knot1to2/cf-parse.tab.h"
#include "libknot/internal/strlcat.h"
#include "libknot/internal/strlcpy.h"

#define DEFAULT_PORT		53
#define DEFAULT_CTL_PORT	5533

#define EXTRA	(cf_get_extra(scanner))

static char *_addr = NULL;
static int _port = -1;
static int _mask = -1;
static char *_str = NULL;
static int _acl_run = -1;
static bool _first = true;

#define ERROR_BUFFER_SIZE       512
extern int cf_lex (YYSTYPE *lvalp, void *scanner);
extern int cf_get_lineno(void *scanner);
extern char *cf_get_text(void *scanner);
extern conf_extra_t *cf_get_extra(void *scanner);
volatile int parser_ret = 0;

static void cf_print_error(void *scanner, const char *prefix, const char *msg)
{
	int lineno = -1;
	char *filename = "";
	conf_include_t *inc = NULL;

	if (scanner) {
		conf_extra_t *extra = cf_get_extra(scanner);
		lineno = cf_get_lineno(scanner);
		inc = conf_includes_top(extra->includes);
	}

	if (inc && inc->filename) {
		filename = inc->filename;
	}

	printf("%s: %s (file '%s', line %d)\n", prefix, msg, filename, lineno);

	fflush(stdout);
}


void cf_error(void *scanner, const char *format, ...)
{
	char buffer[ERROR_BUFFER_SIZE];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);

	cf_print_error(scanner, "Error", buffer);
	parser_ret = -1;

	conf_extra_t *extra = cf_get_extra(scanner);
	extra->error = true;
}

void cf_warning(void *scanner, const char *format, ...)
{
	char buffer[ERROR_BUFFER_SIZE];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);

	cf_print_error(scanner, "Warning", buffer);
}

static bool f_section(void *scanner, int run, section_t id)
{
	conf_extra_t *extra = cf_get_extra(scanner);
	if (extra->run != run) return false;
	if (extra->share->have_sections[id]) return false;

	fprintf(extra->share->out, "\n%s:\n", section_name(id) + 1);
	extra->share->have_sections[id] = true;
	return true;
}

static void f_name(void *scanner, int run, const char *name, bool is_id)
{
	conf_extra_t *extra = cf_get_extra(scanner);
	if (extra->run != run) return;

	fprintf(extra->share->out, "%s%s: ", is_id ? "  - " : "    ", name + 1);
}

static void f_val(void *scanner, int run, bool quote, const char *format, ...)
{
	conf_extra_t *extra = cf_get_extra(scanner);
	if (extra->run != run) return;

	if (quote) {
		fprintf(extra->share->out, "\"");
	}

	va_list ap;
	va_start(ap, format);
	vfprintf(extra->share->out, format, ap);
	va_end(ap);

	if (quote) {
		fprintf(extra->share->out, "\"");
	}
}

static void f_quote(void *scanner, int run, const char *name, const char *val)
{
	f_name(scanner, run, name, false);
	f_val(scanner, run, true, "%s", val);
	f_val(scanner, run, false, "\n");
}

static void f_str(void *scanner, int run, const char *name, const char *val)
{
	f_name(scanner, run, name, false);
	f_val(scanner, run, false, "%s\n", val);
}

static void f_auto_str(void *scanner, int run, const char *name, long val)
{
	if (val != 0) {
		f_name(scanner, run, name, false);
		f_val(scanner, run, true, "");
		f_val(scanner, run, false, "\n");
	}
}

static void f_bool(void *scanner, int run, const char *name, long val)
{
	f_name(scanner, run, name, false);
	f_val(scanner, run, false, "%s\n", val != 0 ? "on" : "off");
}

static void f_int(void *scanner, int run, const char *name, long val)
{
	f_name(scanner, run, name, false);
	f_val(scanner, run, false, "%ld\n", val);
}

static void f_id(void *scanner, int run, const char *name, const char *val)
{
	f_name(scanner, run, name, true);
	f_val(scanner, run, false, "%s\n", val);
}

typedef enum {
	ACL_RMT,
	ACL_XFR,
	ACL_NTF,
	ACL_UPD,
	ACL_CTL
} acl_type_t;

static void acl_start(void *scanner, acl_type_t type)
{
	conf_extra_t *extra = cf_get_extra(scanner);

	if (extra->run == S_FIRST) {
		switch (type) {
		case ACL_RMT: extra->current_trie = extra->share->remotes; break;
		case ACL_XFR: extra->current_trie = extra->share->acl_xfer; break;
		case ACL_NTF: extra->current_trie = extra->share->acl_notify; break;
		case ACL_UPD: extra->current_trie = extra->share->acl_update; break;
		case ACL_CTL: extra->current_trie = extra->share->acl_control; break;
		}
	}

	if (extra->run != _acl_run) return;

	fprintf(extra->share->out, "[");
	_first = true;
}

static void acl_next(void *scanner, const char *value)
{
	conf_extra_t *extra = cf_get_extra(scanner);

	if (extra->run == S_FIRST) {
		*hattrie_get(extra->current_trie, value, strlen(value)) = NULL;
	}

	if (extra->run != _acl_run) return;

	if (_first) {
		_first = false;
	} else {
		fprintf(extra->share->out, ", ");
	}

	f_val(scanner, extra->run, false, "%s", _str);
	f_val(scanner, extra->run, false, "%s", value);
}

static void acl_end(void *scanner)
{
	conf_extra_t *extra = cf_get_extra(scanner);
	if (extra->run != _acl_run) return;

	fprintf(extra->share->out, "]\n");
}

static bool is_remote(void *scanner, const char *str) {
	conf_extra_t *extra = cf_get_extra(scanner);

	return hattrie_tryget(extra->share->remotes, str, strlen(str)) != NULL;
}

static bool is_acl(void *scanner, const char *str) {
	conf_extra_t *extra = cf_get_extra(scanner);

	return hattrie_tryget(extra->share->acl_xfer, str, strlen(str))    != NULL ||
	       hattrie_tryget(extra->share->acl_notify, str, strlen(str))  != NULL ||
	       hattrie_tryget(extra->share->acl_update, str, strlen(str))  != NULL ||
	       hattrie_tryget(extra->share->acl_control, str, strlen(str)) != NULL;
}

static bool have_remote(void *scanner) {
	conf_extra_t *extra = cf_get_extra(scanner);

	return hattrie_weight(extra->share->remotes) > 0;
}


static bool have_acl(void *scanner) {
	conf_extra_t *extra = cf_get_extra(scanner);

	return (hattrie_weight(extra->share->acl_xfer) +
	        hattrie_weight(extra->share->acl_notify) +
	        hattrie_weight(extra->share->acl_update) +
	        hattrie_weight(extra->share->acl_control)) > 0;
}

static char *acl_actions(void *scanner, const char *str) {
	conf_extra_t *extra = cf_get_extra(scanner);

	static char actions[64] = { 0 };
	_first = true;

	strlcpy(actions, "[", sizeof(actions));

	if (hattrie_tryget(extra->share->acl_xfer, str, strlen(str)) != NULL) {
		strlcat(actions, _first ? "" : ", ", sizeof(actions)); _first = false;
		strlcat(actions, "xfer", sizeof(actions));
	}
	if (hattrie_tryget(extra->share->acl_notify, str, strlen(str)) != NULL) {
		strlcat(actions, _first ? "" : ", ", sizeof(actions)); _first = false;
		strlcat(actions, "notify", sizeof(actions));
	}
	if (hattrie_tryget(extra->share->acl_update, str, strlen(str)) != NULL) {
		strlcat(actions, _first ? "" : ", ", sizeof(actions)); _first = false;
		strlcat(actions, "update", sizeof(actions));
	}
	if (hattrie_tryget(extra->share->acl_control, str, strlen(str)) != NULL) {
		strlcat(actions, _first ? "" : ", ", sizeof(actions)); _first = false;
		strlcat(actions, "control", sizeof(actions));
	}

	strlcat(actions, "]", sizeof(actions));

	return actions;
}

%}

%pure-parser
%parse-param{void *scanner}
%lex-param{void *scanner}
%name-prefix "cf_"

%union {
	struct {
		char *t;
		long i;
		size_t l;
	} tok;
}

%token END INVALID_TOKEN
%token <tok> TEXT
%token <tok> NUM
%token <tok> INTERVAL
%token <tok> SIZE
%token <tok> BOOL

%token <tok> SYSTEM IDENTITY HOSTNAME SVERSION NSID KEY KEYS
%token <tok> MAX_UDP_PAYLOAD
%token <tok> TSIG_ALGO_NAME
%token <tok> WORKERS
%token <tok> BACKGROUND_WORKERS
%token <tok> ASYNC_START
%token <tok> USER
%token <tok> RUNDIR
%token <tok> PIDFILE

%token <tok> REMOTES
%token <tok> GROUPS

%token <tok> ZONES FILENAME
%token <tok> DISABLE_ANY
%token <tok> SEMANTIC_CHECKS
%token <tok> NOTIFY_RETRIES
%token <tok> NOTIFY_TIMEOUT
%token <tok> DBSYNC_TIMEOUT
%token <tok> IXFR_FSLIMIT
%token <tok> XFR_IN
%token <tok> XFR_OUT
%token <tok> UPDATE_IN
%token <tok> NOTIFY_IN
%token <tok> NOTIFY_OUT
%token <tok> BUILD_DIFFS
%token <tok> MAX_CONN_IDLE
%token <tok> MAX_CONN_HS
%token <tok> MAX_CONN_REPLY
%token <tok> MAX_TCP_CLIENTS
%token <tok> RATE_LIMIT
%token <tok> RATE_LIMIT_SIZE
%token <tok> RATE_LIMIT_SLIP
%token <tok> TRANSFERS
%token <TOK> STORAGE
%token <tok> DNSSEC_ENABLE
%token <tok> DNSSEC_KEYDIR
%token <tok> SIGNATURE_LIFETIME
%token <tok> SERIAL_POLICY
%token <tok> SERIAL_POLICY_VAL
%token <tok> QUERY_MODULE

%token <tok> INTERFACES ADDRESS PORT
%token <tok> IPA
%token <tok> IPA6
%token <tok> VIA

%token <tok> CONTROL ALLOW LISTEN_ON

%token <tok> LOG
%token <tok> LOG_DEST
%token <tok> LOG_SRC
%token <tok> LOG_LEVEL

%%

config: conf_entries END { return 0; } ;

conf_entries:
 /* EMPTY */
 | conf_entries conf
 ;

interface_start:
 | TEXT		{ free($1.t); }
 | REMOTES	{ free($1.t); }
 | LOG_SRC	{ free($1.t); }
 | LOG		{ free($1.t); }
 | LOG_LEVEL	{ free($1.t); }
 | CONTROL	{ free($1.t); }
 ;

interface:
 | interface PORT NUM ';'		{ _port = $3.i; }
 | interface ADDRESS IPA ';'		{ _addr = $3.t; }
 | interface ADDRESS IPA '@' NUM ';'	{ _addr = $3.t; _port = $5.i;  }
 | interface ADDRESS IPA6 ';'		{ _addr = $3.t; }
 | interface ADDRESS IPA6 '@' NUM ';'	{ _addr = $3.t; _port = $5.i; }
 ;

interfaces:
   INTERFACES '{'			{ f_section(scanner, R_IF, S_SRV); }
 | interfaces interface_start '{'	{ f_name(scanner, R_IF, C_LISTEN, false); _addr = NULL, _port = -1; }
   interface '}' {
 	if (_addr == NULL) {
        	cf_error(scanner, "interface.listen address not defined");
	} else if (_port == -1) {
        	f_val(scanner, R_IF, false, "%s\n", _addr);
	} else {
        	f_val(scanner, R_IF, false, "%s@%i\n", _addr, _port);
	}
	free(_addr);
   }
 ;

system:
   SYSTEM '{'				{ f_section(scanner, R_SYS, S_SRV); }
 | system SVERSION TEXT ';'		{ f_quote(scanner, R_SYS, C_VERSION, $3.t); free($3.t); }
 | system SVERSION BOOL ';'		{ f_auto_str(scanner, R_SYS, C_VERSION, $3.i); }
 | system IDENTITY TEXT ';'		{ f_quote(scanner, R_SYS, C_IDENT, $3.t); free($3.t); }
 | system IDENTITY BOOL ';'		{ f_auto_str(scanner, R_SYS, C_IDENT, $3.i); }
 | system NSID TEXT ';'			{ f_quote(scanner, R_SYS, C_NSID,  $3.t); free($3.t); }
 | system NSID BOOL ';'			{ f_auto_str(scanner, R_SYS, C_NSID, $3.i); }
 | system MAX_UDP_PAYLOAD NUM ';'	{ f_int(scanner, R_SYS, C_MAX_UDP_PAYLOAD, $3.i); }
 | system RUNDIR TEXT ';'		{ f_quote(scanner, R_SYS, C_RUNDIR, $3.t); free($3.t); }
 | system PIDFILE TEXT ';'		{ f_quote(scanner, R_SYS, C_PIDFILE, $3.t); free($3.t); }
 | system WORKERS NUM ';'		{ f_int(scanner, R_SYS, C_WORKERS, $3.i); }
 | system BACKGROUND_WORKERS NUM ';'	{ f_int(scanner, R_SYS, C_BG_WORKERS, $3.i); }
 | system ASYNC_START BOOL ';'		{ f_bool(scanner, R_SYS, C_ASYNC_START, $3.i); }
 | system MAX_CONN_IDLE INTERVAL ';'	{ f_int(scanner, R_SYS, C_MAX_CONN_IDLE, $3.i); }
 | system MAX_CONN_HS INTERVAL ';'	{ f_int(scanner, R_SYS, C_MAX_CONN_HANDSHAKE, $3.i); }
 | system MAX_CONN_REPLY INTERVAL ';'	{ f_int(scanner, R_SYS, C_MAX_CONN_REPLY, $3.i); }
 | system MAX_TCP_CLIENTS NUM ';'	{ f_int(scanner, R_SYS, C_MAX_TCP_CLIENTS, $3.i); }
 | system RATE_LIMIT NUM ';'		{ f_int(scanner, R_SYS, C_RATE_LIMIT, $3.i); }
 | system RATE_LIMIT_SIZE SIZE ';'	{ f_int(scanner, R_SYS, C_RATE_LIMIT_SIZE, $3.l); }
 | system RATE_LIMIT_SIZE NUM ';'	{ f_int(scanner, R_SYS, C_RATE_LIMIT_SIZE, $3.i); }
 | system RATE_LIMIT_SLIP NUM ';'	{ f_int(scanner, R_SYS, C_RATE_LIMIT_SLIP, $3.i); }
 | system TRANSFERS NUM ';'		{ f_int(scanner, R_SYS, C_TRANSFERS, $3.i); }
 | system USER TEXT ';' {
 	char *sep = strchr($3.t, '.');
 	if (sep != NULL) {
 		*sep = ':';
 	}
 	f_str(scanner, R_SYS, C_USER, $3.t);
 	free($3.t);
   }
 | system HOSTNAME TEXT ';' {
     cf_warning(scanner, "option 'system.hostname' is deprecated, "
                         "use 'system.identity' instead");
     free($3.t);
   }
 | system STORAGE TEXT ';' {
     cf_warning(scanner, "option 'system.storage' was relocated, "
                         "use 'zones.storage' instead");
     free($3.t);
   }
 | system KEY TSIG_ALGO_NAME TEXT ';' {
     free($3.t);
     cf_warning(scanner, "option 'system.key' is deprecated and "
                         "it has no effect");
     free($3.t);
   }
 ;

keys:
   KEYS '{' {
   	f_section(scanner, R_KEY, S_KEY);
   }
 | keys TEXT TSIG_ALGO_NAME TEXT ';' {
	f_id(scanner, R_KEY, C_ID, $2.t); free($2.t);
	f_str(scanner, R_KEY, C_ALG, $3.t); free($3.t);
	f_quote(scanner, R_KEY, C_SECRET, $4.t); free($4.t);
   }
 ;

remote_start:
 | TEXT		{ _str = $1.t; }
 | LOG_SRC	{ _str = $1.t; }
 | LOG		{ _str = $1.t; }
 | LOG_LEVEL	{ _str = $1.t; }
 | CONTROL	{ _str = $1.t; }
 ;

remote:
 | remote PORT NUM ';'			{ _port = $3.i; }
 | remote ADDRESS IPA ';'		{ _addr = $3.t; }
 | remote ADDRESS IPA '@' NUM ';'	{ _addr = $3.t; _port = $5.i;  }
 | remote ADDRESS IPA6 ';'		{ _addr = $3.t; }
 | remote ADDRESS IPA6 '@' NUM ';'	{ _addr = $3.t; _port = $5.i; }
 | remote ADDRESS IPA '/' NUM ';'	{ _addr = $3.t; _mask = $5.i; }
 | remote ADDRESS IPA6 '/' NUM ';'	{ _addr = $3.t; _mask = $5.i; }
 | remote KEY TEXT ';' {
 	f_str(scanner, R_RMT, C_KEY, $3.t);
 	f_str(scanner, R_RMT_ACL, C_KEY, $3.t);
 	free($3.t);
   }
 | remote VIA IPA ';'	{ f_str(scanner, R_RMT, C_VIA, $3.t); }
 | remote VIA IPA6 ';'	{ f_str(scanner, R_RMT, C_VIA, $3.t); }
 | remote VIA TEXT ';' {
     cf_warning(scanner, "interface name in 'via' option is not valid in the new "
                "format, use address specification instead (see documentation)");
   }
 ;

remotes:
   REMOTES '{' {
   	_str = NULL;
   	if (have_remote(scanner)) {
		f_section(scanner, R_RMT, S_RMT);
	}
	if (have_acl(scanner)) {
		f_section(scanner, R_RMT_ACL, S_ACL);
	}
   }
 | remotes remote_start '{' {
	_addr = NULL, _port = -1; _mask = -1;
 	if (is_remote(scanner, _str)) {
		f_id(scanner, R_RMT, C_ID, _str);
	}
 	if (is_acl(scanner, _str)) {
		f_name(scanner, R_RMT_ACL, C_ID, true);
		f_val(scanner, R_RMT_ACL, false, "acl_%s\n", _str);
	}
   }
   remote '}' {
 	if (is_remote(scanner, _str)) {
		if (_addr == NULL) {
			cf_error(scanner, "remote.address not defined");
		} else if (_port == -1) {
			f_name(scanner, R_RMT, C_ADDR, false);
			f_val(scanner, R_RMT, false, "%s\n", _addr);
		} else {
			f_name(scanner, R_RMT, C_ADDR, false);
			f_val(scanner, R_RMT, false, "%s@%i\n", _addr, _port);
		}
	}
	if (is_acl(scanner, _str) && _addr != NULL) {
		if (_mask == -1) {
			f_name(scanner, R_RMT_ACL, C_ADDR, false);
			f_val(scanner, R_RMT_ACL, false, "%s\n", _addr);
		} else {
			f_name(scanner, R_RMT_ACL, C_ADDR, false);
			f_val(scanner, R_RMT_ACL, false, "%s/%i\n", _addr, _mask);
		}

		f_name(scanner, R_RMT_ACL, C_ACTION, false);
		f_val(scanner, R_RMT_ACL, false, "%s\n", acl_actions(scanner, _str));
	}
	free(_addr);
	free(_str);
   }
 ;

group_member:
 TEXT
 ;

group:
 /* empty */
 | group_member
 | group ',' group_member
 ;

group_start:
 TEXT
 ;

groups:
   GROUPS '{' {
     cf_warning(scanner, "group section is not valid in the new format, "
                "use a zone template instead (see the documentation)");
   }
 | groups group_start '{' group '}'
 ;

zone_acl_start:
   XFR_IN	{ f_name(scanner, R_ZONE, C_MASTER, false); acl_start(scanner, ACL_RMT); _str = ""; }
 | XFR_OUT	{ f_name(scanner, R_ZONE, C_ACL, false);    acl_start(scanner, ACL_XFR); _str = "acl_"; }
 | NOTIFY_IN	{ f_name(scanner, R_ZONE, C_ACL, false);    acl_start(scanner, ACL_NTF); _str = "acl_"; }
 | NOTIFY_OUT	{ f_name(scanner, R_ZONE, C_NOTIFY, false); acl_start(scanner, ACL_RMT); _str = ""; }
 | UPDATE_IN	{ f_name(scanner, R_ZONE, C_ACL, false);    acl_start(scanner, ACL_UPD); _str = "acl_"; }
 ;

zone_acl_item:
 | TEXT		{ acl_next(scanner, $1.t); free($1.t); }
 | LOG_SRC	{ acl_next(scanner, $1.t); free($1.t); }
 | LOG		{ acl_next(scanner, $1.t); free($1.t); }
 | LOG_LEVEL	{ acl_next(scanner, $1.t); free($1.t); }
 | CONTROL	{ acl_next(scanner, $1.t); free($1.t); }
 ;

zone_acl_list:
 | zone_acl_list zone_acl_item ','
 | zone_acl_list zone_acl_item ';' { acl_end(scanner); }
 ;

query_module:
 TEXT TEXT
 ;

query_module_list:
 | query_module ';' query_module_list
 ;

zone_start:
 | USER		{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 | REMOTES	{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 | LOG_SRC	{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 | LOG		{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 | LOG_LEVEL	{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 | CONTROL	{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 | NUM '/' TEXT	{
      f_name(scanner, R_ZONE, C_DOMAIN, true);
      f_val(scanner, R_ZONE, false, "%i/%s", $1.i, $3.t);
      f_val(scanner, R_ZONE, false, "\n");
      free($3.t);
   }
 | TEXT		{ f_id(scanner, R_ZONE, C_DOMAIN, $1.t); free($1.t); }
 ;

zone:
   zone_start '{'
 | zone zone_acl_start zone_acl_list
 | zone FILENAME TEXT ';'			{ f_quote(scanner, R_ZONE, C_FILE,           $3.t); free($3.t); }
 | zone DISABLE_ANY BOOL ';'			{ f_bool(scanner,  R_ZONE, C_DISABLE_ANY,    $3.i); }
 | zone BUILD_DIFFS BOOL ';'			{ f_bool(scanner,  R_ZONE, C_IXFR_DIFF,      $3.i); }
 | zone SEMANTIC_CHECKS BOOL ';'		{ f_bool(scanner,  R_ZONE, C_SEM_CHECKS,     $3.i); }
 | zone IXFR_FSLIMIT SIZE ';'			{ f_int(scanner,   R_ZONE, C_IXFR_FSLIMIT,   $3.l); }
 | zone IXFR_FSLIMIT NUM ';'			{ f_int(scanner,   R_ZONE, C_IXFR_FSLIMIT,   $3.i); }
 | zone NOTIFY_RETRIES NUM ';'			{ f_int(scanner,   R_ZONE, C_NOTIFY_RETRIES, $3.i); }
 | zone NOTIFY_TIMEOUT NUM ';'			{ f_int(scanner,   R_ZONE, C_NOTIFY_TIMEOUT, $3.i); }
 | zone DBSYNC_TIMEOUT NUM ';'			{ f_int(scanner,   R_ZONE, C_ZONEFILE_SYNC,  $3.i); }
 | zone DBSYNC_TIMEOUT INTERVAL ';'		{ f_int(scanner,   R_ZONE, C_ZONEFILE_SYNC,  $3.i); }
 | zone STORAGE TEXT ';'			{ f_quote(scanner, R_ZONE, C_STORAGE,        $3.t); free($3.t); }
 | zone DNSSEC_ENABLE BOOL ';'			{ f_bool(scanner,  R_ZONE, C_DNSSEC_ENABLE,  $3.i); }
 | zone DNSSEC_KEYDIR TEXT ';'			{ f_quote(scanner, R_ZONE, C_DNSSEC_KEYDIR,  $3.t); free($3.t); }
 | zone SIGNATURE_LIFETIME NUM ';'		{ f_int(scanner,   R_ZONE, C_SIG_LIFETIME,   $3.i); }
 | zone SIGNATURE_LIFETIME INTERVAL ';'		{ f_int(scanner,   R_ZONE, C_SIG_LIFETIME,   $3.i); }
 | zone SERIAL_POLICY SERIAL_POLICY_VAL ';'	{ f_str(scanner,   R_ZONE, C_SERIAL_POLICY,  $3.t); }
 | zone QUERY_MODULE '{' {
     cf_warning(scanner, "query module is not yet implemented");
   }
   query_module_list '}'
 ;

query_genmodule:
 TEXT TEXT
 ;
query_genmodule_list:
 | query_genmodule ';' query_genmodule_list
 ;

zones:
   ZONES '{' {
	f_section(scanner, R_ZONE, S_ZONE); _acl_run = R_ZONE;

	if (f_section(scanner, R_ZONE_TPL, S_TPL)) {
		f_id(scanner, R_ZONE_TPL, C_ID, "default");
	}
   }
 | zones zone '}'
 | zones DISABLE_ANY BOOL ';'			{ f_bool(scanner,  R_ZONE_TPL, C_DISABLE_ANY,    $3.i); }
 | zones BUILD_DIFFS BOOL ';'			{ f_bool(scanner,  R_ZONE_TPL, C_IXFR_DIFF,      $3.i); }
 | zones SEMANTIC_CHECKS BOOL ';'		{ f_bool(scanner,  R_ZONE_TPL, C_SEM_CHECKS,     $3.i); }
 | zones IXFR_FSLIMIT SIZE ';'			{ f_int(scanner,   R_ZONE_TPL, C_IXFR_FSLIMIT,   $3.l); }
 | zones IXFR_FSLIMIT NUM ';'			{ f_int(scanner,   R_ZONE_TPL, C_IXFR_FSLIMIT,   $3.i); }
 | zones NOTIFY_RETRIES NUM ';'			{ f_int(scanner,   R_ZONE_TPL, C_NOTIFY_RETRIES, $3.i); }
 | zones NOTIFY_TIMEOUT NUM ';'			{ f_int(scanner,   R_ZONE_TPL, C_NOTIFY_TIMEOUT, $3.i); }
 | zones DBSYNC_TIMEOUT NUM ';'			{ f_int(scanner,   R_ZONE_TPL, C_ZONEFILE_SYNC,  $3.i); }
 | zones DBSYNC_TIMEOUT INTERVAL ';'		{ f_int(scanner,   R_ZONE_TPL, C_ZONEFILE_SYNC,  $3.i); }
 | zones STORAGE TEXT ';'			{ f_quote(scanner, R_ZONE_TPL, C_STORAGE,        $3.t); free($3.t); }
 | zones DNSSEC_ENABLE BOOL ';'			{ f_bool(scanner,  R_ZONE_TPL, C_DNSSEC_ENABLE,  $3.i); }
 | zones DNSSEC_KEYDIR TEXT ';'			{ f_quote(scanner, R_ZONE_TPL, C_DNSSEC_KEYDIR,  $3.t); free($3.t); }
 | zones SIGNATURE_LIFETIME NUM ';'		{ f_int(scanner,   R_ZONE_TPL, C_SIG_LIFETIME,   $3.i); }
 | zones SIGNATURE_LIFETIME INTERVAL ';'	{ f_int(scanner,   R_ZONE_TPL, C_SIG_LIFETIME,   $3.i); }
 | zones SERIAL_POLICY SERIAL_POLICY_VAL ';'	{ f_str(scanner,   R_ZONE_TPL, C_SERIAL_POLICY,  $3.t); }
 | zones QUERY_MODULE '{' {
     cf_warning(scanner, "query module is not yet implemented");
   }
   query_genmodule_list '}'
 ;

log_prios:
 | log_prios LOG_LEVEL ',' { if (_str == NULL) _str = $2.t; }
 | log_prios LOG_LEVEL ';' { if (_str == NULL) _str = $2.t; }
 ;

log_src:
 | log_src LOG_SRC {
     f_name(scanner, R_LOG, $2.t, false);
     _str = NULL;
   }
   log_prios {
     f_val(scanner, R_LOG, false, "%s\n", _str);
   }
 ;

log_dest:
   LOG_DEST { f_id(scanner, R_LOG, C_TO, $1.t); }
;

log_file:
   FILENAME TEXT {
      f_name(scanner, R_LOG, C_TO, true);
      f_val(scanner, R_LOG, true, "%s", $2.t);
      f_val(scanner, R_LOG, false, "\n");
      free($2.t);
   }
;

log_start:
 | log_start log_dest '{' log_src '}'
 | log_start log_file '{' log_src '}'
 ;

log:
   LOG '{' { f_section(scanner, R_LOG, S_LOG); }
   log_start
 ;

ctl_listen_start:
  LISTEN_ON
  ;

ctl_allow_start:
  ALLOW	{ f_name(scanner, R_CTL, C_ACL, false); acl_start(scanner, ACL_CTL); _str = "acl_"; }
  ;

control:
   CONTROL '{' { f_section(scanner, R_CTL, S_CTL); _acl_run = R_CTL; }
 | control ctl_listen_start '{' { f_name(scanner, R_CTL, C_LISTEN, false); _addr = NULL, _port = -1; }
   interface '}' {
 	if (_addr == NULL) {
        	cf_error(scanner, "control.listen address not defined");
	} else if (_port == -1) {
        	f_val(scanner, R_CTL, false, "%s\n", _addr);
	} else {
        	f_val(scanner, R_CTL, false, "%s@%i\n", _addr, _port);
	}
	free(_addr);
   }
 | control ctl_listen_start TEXT ';' { f_quote(scanner, R_CTL, C_LISTEN, $3.t); free($3.t); }
 | control ctl_allow_start zone_acl_list
 ;

conf: ';' | system '}' | interfaces '}' | keys '}' | remotes '}' | groups '}' | zones '}' | log '}' | control '}';

%%