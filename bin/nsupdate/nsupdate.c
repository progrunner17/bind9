/*
 * Copyright (C) 2000  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/* $Id: nsupdate.c,v 1.16 2000/06/30 06:35:50 bwelling Exp $ */

#include <config.h>

#include <isc/app.h>
#include <isc/base64.h>
#include <isc/buffer.h>
#include <isc/condition.h>
#include <isc/commandline.h>
#include <isc/entropy.h>
#include <isc/lex.h>
#include <isc/mem.h>
#include <isc/mutex.h>
#include <isc/region.h>
#include <isc/sockaddr.h>
#include <isc/socket.h>
#include <isc/string.h>
#include <isc/task.h>
#include <isc/timer.h>
#include <isc/types.h>
#include <isc/util.h>

#include <dns/callbacks.h>
#include <dns/dispatch.h>
#include <dns/events.h>
#include <dns/message.h>
#include <dns/name.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/rdatatype.h>
#include <dns/request.h>
#include <dns/result.h>
#include <dns/tsig.h>
#include <dst/dst.h>

#include <ctype.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>

#define MXNAME 256
#define MAXCMD 1024
#define NAMEBUF 512
#define WORDLEN 512
#define PACKETSIZE 2048
#define MSGTEXT 4096
#define FIND_TIMEOUT 5

#define RESOLV_CONF "/etc/resolv.conf"

static isc_boolean_t busy = ISC_FALSE;
static isc_boolean_t debugging = ISC_FALSE, ddebugging = ISC_FALSE;
static isc_boolean_t have_ipv6 = ISC_FALSE;
static isc_boolean_t is_dst_up = ISC_FALSE;
static isc_mutex_t lock;
static isc_condition_t cond;

static isc_taskmgr_t *taskmgr = NULL;
static isc_task_t *global_task = NULL;
static isc_mem_t *mctx = NULL;
static dns_dispatchmgr_t *dispatchmgr = NULL;
static dns_requestmgr_t *requestmgr = NULL;
static isc_socketmgr_t *socketmgr = NULL;
static isc_timermgr_t *timermgr = NULL;
static dns_dispatch_t *dispatchv4 = NULL;
static dns_message_t *updatemsg = NULL;
static dns_name_t *zonename = NULL;
static dns_fixedname_t resolvdomain; /* from resolv.conf's domain line */
static dns_name_t *current_zone; /* Points to one of above, or dns_rootname */
static dns_name_t *master = NULL; /* Master nameserver, from SOA query */
static dns_tsigkey_t *key = NULL;
static dns_tsig_keyring_t *keyring = NULL;

static char nameservername[3][MXNAME];
static int nameservers;
static int ns_inuse = 0;
static unsigned int ndots = 1;
static char *domain = NULL;
static char *keystr = NULL;
static isc_entropy_t *entp = NULL;

static void sendrequest(int whichns, dns_message_t *msg,
			dns_request_t **request);

#define STATUS_MORE 0
#define STATUS_SEND 1
#define STATUS_QUIT 2
#define STATUS_SYNTAX 3

static void
fatal(const char *format, ...) {
	va_list args;

	va_start(args, format);	
	vfprintf(stderr, format, args);
	va_end(args);
	fprintf(stderr, "\n");
	exit(1);
}

static void
debug(const char *format, ...) {
	va_list args;

	if (debugging) {
		va_start(args, format);	
		vfprintf(stderr, format, args);
		va_end(args);
		fprintf(stderr, "\n");
	}
}

static void
ddebug(const char *format, ...) {
	va_list args;

	if (ddebugging) {
		va_start(args, format);	
		vfprintf(stderr, format, args);
		va_end(args);
		fprintf(stderr, "\n");
	}
}

static void
check_result(isc_result_t result, const char *msg) {
	if (result != ISC_R_SUCCESS)
		fatal("%s: %s", msg, isc_result_totext(result));
}

/*
 * Eat characters from FP until EOL or EOF. Returns EOF or '\n'
 */
static int
eatline(FILE *fp) {
	int ch;

	ch = fgetc(fp);
	while (ch != '\n' && ch != EOF)
		ch = fgetc(fp);
	return (ch);
}

/*
 * Eats white space up to next newline or non-whitespace character (of
 * EOF). Returns the last character read. Comments are considered white
 * space.
 */
static int
eatwhite(FILE *fp) {
	int ch;

	ch = fgetc(fp);
	while (ch != '\n' && ch != EOF && isspace((unsigned char)ch))
		ch = fgetc(fp);

	if (ch == ';' || ch == '#')
		ch = eatline(fp);

	return (ch);
}

/*
 * Skip over any leading whitespace and then read in the next sequence of
 * non-whitespace characters. In this context newline is not considered
 * whitespace. Returns EOF on end-of-file, or the character
 * that caused the reading to stop.
 */
static int
getword(FILE *fp, char *buffer, size_t size) {
	int ch;
	char *p = buffer;

	REQUIRE(buffer != NULL);
	REQUIRE(size > 0);

	*p = '\0';

	ch = eatwhite(fp);

	if (ch == EOF)
		return (EOF);

	do {
		*p = '\0';

		if (ch == EOF || isspace((unsigned char)ch))
			break;
		else if ((size_t) (p - buffer) == size - 1)
			return (EOF);   /* Not enough space. */

		*p++ = (char)ch;
		ch = fgetc(fp);
	} while (1);

	return (ch);
}

static char *
nsu_strsep(char **stringp, const char *delim) {
	char *string = *stringp;
	char *s;
	const char *d;
	char sc, dc;

	if (string == NULL)
		return (NULL);

	for (; *string != '\0'; string++) {
		sc = *string;
		for (d = delim; (dc = *d) != '\0'; d++) {
			if (sc == dc)
				break;
		}
		if (dc == 0)
			break;
	}

	for (s = string; *s != '\0'; s++) {
		sc = *s;
		for (d = delim; (dc = *d) != '\0'; d++) {
			if (sc == dc) {
				*s++ = '\0';
				*stringp = s;
				return (string);
			}
		}
	}
	*stringp = NULL;
	return (string);
}

static unsigned int
count_dots(char *s, isc_boolean_t *last_was_dot) {
	int i = 0;
	*last_was_dot = ISC_FALSE;
	while (*s != 0) {
		if (*s++ == '.') {
			i++;
			*last_was_dot = ISC_TRUE;
		} else
			*last_was_dot = ISC_FALSE;
	}
	return (i);
}

static void
load_resolv_conf(void) {
	FILE *fp;
	char word[256];
	int stopchar, delim;

	ddebug ("load_resolv_conf()");
	fp = fopen (RESOLV_CONF, "r");
	if (fp == NULL)
		return;
	do {
		stopchar = getword(fp, word, sizeof(word));
		if (stopchar == EOF)
			break;
		if (strcmp(word, "nameserver") == 0) {
			ddebug ("Got a nameserver line");
			if (nameservers >= 3) {
				stopchar = eatline(fp);
				if (stopchar == EOF)
					break;
				continue;
			}
			delim = getword(fp, word, sizeof(word));
			if (strlen(word) == 0) {
				stopchar = eatline(fp);
				if (stopchar == EOF)
					break;
				continue;
			}
			strncpy(nameservername[nameservers], word, MXNAME);
			nameservers++;
		} else if (strcasecmp(word, "options") == 0) {
			delim = getword(fp, word, sizeof(word));
			if (strlen(word) == 0)
				continue;
			while (strlen(word) > 0) {
				char *endp;
				if (strncmp("ndots:", word, 6) == 0) {
					ndots = strtol(word + 6, &endp, 10);
					if (*endp != 0)
						fatal("ndots is not numeric\n");
					ddebug ("ndots is %d.", ndots);
				}
			}
			if (delim == EOF || delim == '\n')
				break;
			else
				delim = getword(fp, word, sizeof(word));
		/* XXXMWS Searchlist not supported! */
		} else if (strcasecmp(word, "domain") == 0 && domain == NULL) {
			delim = getword(fp, word, sizeof(word));
			if (strlen(word) == 0) {
				stopchar = eatline(fp);
				if (stopchar == EOF)
					break;
				continue;
			}
			domain = isc_mem_strdup(mctx, word);
			if (domain == NULL)
				fatal("out of memory");
			strncpy(nameservername[nameservers], word, MXNAME);
			nameservers++;
		}
		stopchar = eatline(fp);
		if (stopchar == EOF)
			break;
	} while (ISC_TRUE);
	fclose (fp);
}	

static void
reset_system(void) {
	isc_result_t result;

	ddebug ("reset_system()");
	/* If the update message is still around, destroy it */
	if (updatemsg != NULL)
		dns_message_reset(updatemsg, DNS_MESSAGE_INTENTRENDER);
	else {
		result = dns_message_create(mctx, DNS_MESSAGE_INTENTRENDER,
					    &updatemsg);
		check_result (result, "dns_message_create");
	}
	updatemsg->opcode = dns_opcode_update;

	if (zonename != NULL) {
		dns_name_free(zonename, mctx);
		isc_mem_put(mctx, zonename, sizeof(dns_name_t));
		zonename = NULL;
	}
	if (master != NULL) {
		dns_name_free(master, mctx);
		isc_mem_put(mctx, master, sizeof(dns_name_t));
		master = NULL;
	}
	if (domain != NULL) 
		current_zone = dns_fixedname_name(&resolvdomain);
	else
		current_zone = dns_rootname;
}

static void
setup_system(void) {
	isc_result_t result;
	isc_sockaddr_t bind_any;
	isc_buffer_t buf;

	ddebug("setup_system()");

	/*
	 * Warning: This is not particularly good randomness.  We'll
	 * just use random() now for getting id values, but doing so
	 * does NOT insure that id's can't be guessed.
	 *
	 * XXX Shouldn't random() be called somewhere if this is here?
	 */
	srandom (getpid() + (int)&setup_system);

	result = isc_app_start();
	check_result(result, "isc_app_start");

	result = isc_net_probeipv4();
	check_result(result, "isc_net_probeipv4");

	/* XXXMWS There isn't any actual V6 support in the code yet */
	result = isc_net_probeipv6();
	if (result == ISC_R_SUCCESS)
		have_ipv6=ISC_TRUE;

	result = isc_mem_create(0, 0, &mctx);
	check_result(result, "isc_mem_create");

	load_resolv_conf();

	result = dns_dispatchmgr_create(mctx, NULL, &dispatchmgr);
	check_result(result, "dns_dispatchmgr_create");

	result = isc_socketmgr_create(mctx, &socketmgr);
	check_result(result, "dns_socketmgr_create");

	result = isc_timermgr_create(mctx, &timermgr);
	check_result(result, "dns_timermgr_create");

	result = isc_taskmgr_create (mctx, 1, 0, &taskmgr);
	check_result(result, "isc_taskmgr_create");

	result = isc_task_create (taskmgr, 0, &global_task);
	check_result(result, "isc_task_create");

	result = isc_entropy_create (mctx, &entp);
	check_result(result, "isc_entropy_create");

	result = dst_lib_init (mctx, entp, 0);
	check_result(result, "dst_lib_init");
	is_dst_up = ISC_TRUE;

	isc_sockaddr_any(&bind_any);

	result = dns_dispatch_getudp(dispatchmgr, socketmgr, taskmgr,
				     &bind_any, PACKETSIZE, 4, 2, 3, 5,
				     DNS_DISPATCHATTR_UDP |
				     DNS_DISPATCHATTR_IPV4 |
				     DNS_DISPATCHATTR_MAKEQUERY, 0,
				     &dispatchv4);
	check_result(result, "dns_dispatch_getudp");

	result = dns_requestmgr_create(mctx, timermgr,
				       socketmgr, taskmgr, dispatchmgr,
				       dispatchv4, NULL, &requestmgr);
	check_result(result, "dns_requestmgr_create");

	if (domain != NULL) {
		dns_fixedname_init(&resolvdomain);
		isc_buffer_init(&buf, domain, strlen(domain));
		isc_buffer_add(&buf, strlen(domain));
		result = dns_name_fromtext(dns_fixedname_name(&resolvdomain),
					   &buf, dns_rootname, ISC_FALSE, NULL);
		check_result(result, "dns_name_fromtext");
		current_zone = dns_fixedname_name(&resolvdomain);
	}
	else
		current_zone = dns_rootname;

	if (keystr != NULL) {
		dns_fixedname_t fkeyname;
		dns_name_t *keyname;
		isc_buffer_t keynamesrc;
		char *secretstr;
		unsigned char *secret;
		int secretlen;
		isc_buffer_t secretsrc, secretbuf;
		isc_lex_t *lex = NULL;
		char *s;

		debug("Creating key...");

		s = strchr(keystr, ':');
		if (s == NULL || s == keystr || *s++ == 0)
			fatal("key option must specify keyname:secret\n");
		secretstr = s + 1;

		result = dns_tsigkeyring_create(mctx, &keyring);
		check_result(result, "dns_tsigkeyringcreate");

		dns_fixedname_init(&fkeyname);
		keyname = dns_fixedname_name(&fkeyname);

		isc_buffer_init(&keynamesrc, keystr, s - keystr);

		debug("namefromtext");
		result = dns_name_fromtext(keyname, &keynamesrc, dns_rootname,
					   ISC_FALSE, NULL);
		check_result(result, "dns_name_fromtext");

		secretlen = strlen(secretstr) * 3 / 4;
		secret = isc_mem_allocate(mctx, secretlen);
		if (secret == NULL)
			fatal("out of memory");

		isc_buffer_init(&secretsrc, secretstr, strlen(secretstr));
		isc_buffer_add(&secretsrc, strlen(secretstr));

		isc_buffer_init(&secretbuf, secret, secretlen);

		result = isc_lex_create(mctx, strlen(secretstr), &lex);
		check_result(result, "isc_lex_create");
		result = isc_lex_openbuffer(lex, &secretsrc);
		check_result(result, "isc_lex_openbuffer");
		result = isc_base64_tobuffer(lex, &secretbuf, -1);
		if (result != ISC_R_SUCCESS) {
			printf (";; Couldn't create key from %s: %s\n",
				keystr, isc_result_totext(result));
			isc_lex_close(lex);
			isc_lex_destroy(&lex);
			goto SYSSETUP_FAIL;
		}
		secretlen = isc_buffer_usedlength(&secretbuf);
		debug("close");
		isc_lex_close(lex);
		isc_lex_destroy(&lex);
		
		debug("keycreate");
		result = dns_tsigkey_create(keyname, dns_tsig_hmacmd5_name,
					    secret, secretlen,
					    ISC_TRUE, NULL, 0, 0, mctx,
					    keyring, &key);
		if (result != ISC_R_SUCCESS) {
			printf (";; Couldn't create key from %s: %s\n",
				keystr, dns_result_totext(result));
		}
		isc_mem_free(mctx, secret);
		return;
	SYSSETUP_FAIL:
		isc_mem_free(mctx, secret);
		dns_tsigkeyring_destroy(&keyring);
	}
}

static void
parse_args(int argc, char **argv) {
	int ch;

	debug("parse_args");
	while ((ch = isc_commandline_parse(argc, argv, "dDMy:vk:")) != -1) {
		switch (ch) {
		case 'd':
			debugging = ISC_TRUE;
			break;
		case 'D': /* was -dd */
			debugging = ISC_TRUE;
			ddebugging = ISC_TRUE;
			break;
		case 'M': /* was -dm */
			debugging = ISC_TRUE;
			ddebugging = ISC_TRUE;
			isc_mem_debugging = ISC_TRUE;
			break;
		case 'y':
			keystr = isc_commandline_argument;
			break;
		case 'v':
			fprintf(stderr, "Virtual Circuit mode not "
				"currently implemented.\n");
			break;
		case 'k':
			fprintf(stderr, "TSIG not currently implemented.");
			break;
		default:
			fprintf(stderr, "%s: invalid argument -%c\n",
				argv[0], ch);
			exit(1);
		}
	}
}

static isc_uint16_t
make_prereq(char *cmdline, isc_boolean_t ispositive, isc_boolean_t isrrset) {
	isc_result_t result;
	char *word;
	dns_name_t *name = NULL;
	isc_buffer_t *namebuf = NULL;
	isc_buffer_t source;
	isc_textregion_t region;
	dns_rdataset_t *rdataset = NULL;
	dns_rdatalist_t *rdatalist = NULL;
	dns_rdataclass_t rdataclass;
	dns_rdatatype_t rdatatype;
	dns_rdata_t *rdata = NULL;
	dns_name_t *rn = current_zone;
	unsigned int dots;
	isc_boolean_t last;

	ddebug ("make_prereq()");

	/*
	 * Read the owner name
	 */
	word = nsu_strsep(&cmdline, " \t\r\n");
	if (*word == 0) {
		puts("failed to read owner name");
		return (STATUS_SYNTAX);
	}

	result = dns_message_gettempname(updatemsg, &name);
	check_result(result, "dns_message_gettempname");
	result = isc_buffer_allocate(mctx, &namebuf, NAMEBUF);
	check_result(result, "isc_buffer_allocate");
	dns_name_init(name, NULL);
	dns_name_setbuffer(name, namebuf);
	dns_message_takebuffer(updatemsg, &namebuf);
	isc_buffer_init(&source, word, strlen(word));
	isc_buffer_add(&source, strlen(word));
	dots = count_dots(word, &last);
	if (dots > ndots || last)
		rn = dns_rootname;
	result = dns_name_fromtext(name, &source, rn,
				   ISC_FALSE, NULL);
	check_result(result, "dns_name_fromtext");
		
	/*
	 * If this is an rrset prereq, read the class or type.
	 */
	if (isrrset) {
		word = nsu_strsep(&cmdline, " \t\r\n");
		if (*word == 0) {
			puts("failed to read class or type");
			dns_message_puttempname(updatemsg, &name);
			return (STATUS_SYNTAX);
		}
		region.base = word;
		region.length = strlen(word);
		result = dns_rdataclass_fromtext(&rdataclass, &region);
		if (result == ISC_R_SUCCESS) {
			/*
			 * Now read the type.
			 */
			word = nsu_strsep(&cmdline, " \t\r\n");
			if (*word == 0) {
				puts("failed to read type");
				dns_message_puttempname(updatemsg, &name);
				return (STATUS_SYNTAX);
			}
			region.base = word;
			region.length = strlen(word);
			result = dns_rdatatype_fromtext(&rdatatype, &region);
			check_result(result, "dns_rdatatype_fromtext");
		} else {
			rdataclass = dns_rdataclass_in;
			result = dns_rdatatype_fromtext(&rdatatype, &region);
			check_result(result, "dns_rdatatype_fromtext");
		}
	} else
		rdatatype = dns_rdatatype_any;


	result = dns_message_gettemprdatalist(updatemsg, &rdatalist);
	check_result(result, "dns_message_gettemprdatalist");
	result = dns_message_gettemprdataset(updatemsg, &rdataset);
	check_result(result, "dns_message_gettemprdataset");
	dns_rdatalist_init(rdatalist);
	rdatalist->type = rdatatype;
	if (ispositive)
		rdatalist->rdclass = dns_rdataclass_any;
	else
		rdatalist->rdclass = dns_rdataclass_none;
	rdatalist->covers = 0;
	rdatalist->ttl = 0;
	result = dns_message_gettemprdata(updatemsg, &rdata);
	check_result(result, "dns_message_gettemprdata");
	rdata->data = NULL;
	rdata->length = 0;
	rdata->rdclass = rdatalist->rdclass;
	rdata->type = rdatatype;
	ISC_LIST_INIT(rdatalist->rdata);
	ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	dns_rdataset_init(rdataset);
	dns_rdatalist_tordataset(rdatalist, rdataset);		
	ISC_LIST_INIT(name->list);
	ISC_LIST_APPEND(name->list, rdataset, link);
	dns_message_addname(updatemsg, name, DNS_SECTION_PREREQUISITE);
	return (STATUS_MORE);
}

static isc_uint16_t
evaluate_prereq(char *cmdline) {
	char *word;
	isc_boolean_t ispositive, isrrset;

	ddebug ("evaluate_prereq()");
	word = nsu_strsep(&cmdline, " \t\r\n");
	if (*word == 0) {
		puts ("failed to read operation code");
		return (STATUS_SYNTAX);
	}
	if (strcasecmp(word, "nxdomain") == 0) {
		ispositive = ISC_FALSE;
		isrrset = ISC_FALSE;
	} else if (strcasecmp(word, "yxdomain") == 0) {
		ispositive = ISC_TRUE;
		isrrset = ISC_FALSE;
	} else if (strcasecmp(word, "nxrrset") == 0) {
		ispositive = ISC_FALSE;
		isrrset = ISC_TRUE;
	} else if (strcasecmp(word, "yxrrset") == 0) {
		ispositive = ISC_TRUE;
		isrrset = ISC_TRUE;
	} else {
		printf("incorrect operation code: %s\n", word);
		return (STATUS_SYNTAX);
	}
	return (make_prereq(cmdline, ispositive, isrrset));
}

static isc_uint16_t
evaluate_server(char *cmdline) {
	UNUSED(cmdline);
	printf("The server statement is not currently implemented.\n");
	return (STATUS_MORE);
}

static isc_uint16_t
evaluate_zone(char *cmdline) {
	UNUSED(cmdline);
	printf("The zone statement is not currently implemented.\n");
	return (STATUS_MORE);
#if 0
	char *word;
	dns_name_t *name;
	isc_buffer_t src;
	isc_result_t result;
	dns_fixedname_t fname;

	ddebug ("evaluate_zone()");
	word = nsu_strsep(&cmdline, " \t\r\n");
	if (*word == 0) {
		printf("failed to read zone name");
		return (STATUS_SYNTAX);
	} else if (zonename != NULL)
		dns_name_free(zonename, mctx);

	dns_fixedname_init(&fname);
	name = dns_fixedname_name(&fname);

	isc_buffer_init(&src, word, strlen(word));
	isc_buffer_add(&src, strlen(word));

	result = dns_name_fromtext(name, &src, dns_rootname, ISC_FALSE, NULL);
	check_result(result, "dns_name_fromtext");

	if (zonename == NULL) {
		zonename = isc_mem_get(mctx, sizeof(dns_name_t));
		if (zonename == NULL)
			fatal("out of memory");
	}
	dns_name_init(zonename, NULL);
	dns_name_dup(name, mctx, zonename);
	current_zone = zonename;

	return (STATUS_MORE);
#endif
}

static isc_uint16_t
update_addordelete(char *cmdline, isc_boolean_t isdelete) {
	isc_result_t result;
	isc_lex_t *lex = NULL;
	isc_buffer_t *buf = NULL;
	isc_buffer_t *namebuf = NULL;
	isc_buffer_t source;
	dns_name_t *name = NULL;
	isc_uint16_t ttl;
	char *word;
	dns_rdataclass_t rdataclass;
	dns_rdatatype_t rdatatype;
	dns_rdatacallbacks_t callbacks;
	dns_rdata_t *rdata = NULL;
	dns_rdatalist_t *rdatalist = NULL;
	dns_rdataset_t *rdataset = NULL;
	isc_textregion_t region;
	dns_name_t *rn = current_zone;
	char *endp;
	unsigned int dots;
	isc_boolean_t last;

	ddebug ("update_addordelete()");

	/*
	 * Read the owner name
	 */
	word = nsu_strsep(&cmdline, " \t\r\n");
	if (*word == 0) {
		puts ("failed to read owner name");
		return (STATUS_SYNTAX);
	}

	result = dns_message_gettempname(updatemsg, &name);
	check_result(result, "dns_message_gettempname");
	result = isc_buffer_allocate(mctx, &namebuf, NAMEBUF);
	check_result(result, "isc_buffer_allocate");
	dns_name_init(name, NULL);
	dns_name_setbuffer(name, namebuf);
	dns_message_takebuffer(updatemsg, &namebuf);
	isc_buffer_init(&source, word, strlen(word));
	isc_buffer_add(&source, strlen(word));
	dots = count_dots(word, &last);
	if (dots > ndots || last)
		rn = dns_rootname;
	result = dns_name_fromtext(name, &source, rn,
				   ISC_FALSE, NULL);
	check_result(result, "dns_name_fromtext");

	result = dns_message_gettemprdata(updatemsg, &rdata);
	check_result(result, "dns_message_gettemprdata");

	rdata->rdclass = 0;
	rdata->type = 0;
	rdata->data = NULL;
	rdata->length = 0;

	/*
	 * If this is an add, read the TTL and verify that it's numeric.
	 */
	if (!isdelete) {
		word = nsu_strsep(&cmdline, " \t\r\n");
		if (*word == 0) {
			puts ("failed to read owner ttl");
			dns_message_puttempname(updatemsg, &name);
			return (STATUS_SYNTAX);
		}
		ttl = strtol(word, &endp, 0);
		if (*endp != 0) {
			printf("ttl '%s' is not numeric\n", word);
			dns_message_puttempname(updatemsg, &name);
			return (STATUS_SYNTAX);
		}
	} else
		ttl = 0;

	/*
	 * Read the class or type.
	 */
	word = nsu_strsep(&cmdline, " \t\r\n");
	if (*word == 0) {
		if (isdelete) {
			rdataclass = dns_rdataclass_any;
			rdatatype = dns_rdatatype_any;
			goto doneparsing;
		} else {
			puts ("failed to read class or type");
			dns_message_puttempname(updatemsg, &name);
			return (STATUS_SYNTAX);
		}
	}
	region.base = word;
	region.length = strlen(word);
	result = dns_rdataclass_fromtext(&rdataclass, &region);
	if (result == ISC_R_SUCCESS) {
		/*
		 * Now read the type.
		 */
		word = nsu_strsep(&cmdline, " \t\r\n");
		if (*word == 0) {
			if (isdelete) {
				rdataclass = dns_rdataclass_any;
				rdatatype = dns_rdatatype_any;
				goto doneparsing;
			} else {
				puts ("failed to read type");
				dns_message_puttempname(updatemsg, &name);
				return (STATUS_SYNTAX);
			}
		}
		region.base = word;
		region.length = strlen(word);
		result = dns_rdatatype_fromtext(&rdatatype, &region);
		check_result(result, "dns_rdatatype_fromtext");
	} else {
		rdataclass = dns_rdataclass_in;
		result = dns_rdatatype_fromtext(&rdatatype, &region);
		check_result(result, "dns_rdatatype_fromtext");
	}

	while (*cmdline != 0 && isspace(*cmdline))
		cmdline++;

	if (*cmdline == 0) {
		if (isdelete) {
			rdataclass = dns_rdataclass_any;
			goto doneparsing;
		} else {
			puts ("failed to read owner data");
			dns_message_puttempname(updatemsg, &name);
			return (STATUS_SYNTAX);
		}
	}

	result = isc_lex_create(mctx, WORDLEN, &lex);
	check_result(result, "isc_lex_create");	
	isc_buffer_invalidate(&source);

	isc_buffer_init(&source, cmdline, strlen(cmdline));
	isc_buffer_add(&source, strlen(cmdline));
	result = isc_lex_openbuffer(lex, &source);
	check_result(result, "isc_lex_openbuffer");

	result = isc_buffer_allocate(mctx, &buf, MXNAME);
	check_result(result, "isc_buffer_allocate");
	dns_rdatacallbacks_init_stdio(&callbacks);
	result = dns_rdata_fromtext(rdata, rdataclass, rdatatype,
				    lex, current_zone, ISC_FALSE, buf,
				    &callbacks);
	dns_message_takebuffer(updatemsg, &buf);
	isc_lex_destroy(&lex);
	if (result != ISC_R_SUCCESS) {
		dns_message_puttempname(updatemsg, &name);
		dns_message_puttemprdata(updatemsg, &rdata);
		return (STATUS_MORE);
	}

	if (isdelete)
		rdataclass = dns_rdataclass_none;

 doneparsing:

	result = dns_message_gettemprdatalist(updatemsg, &rdatalist);
	check_result(result, "dns_message_gettemprdatalist");
	result = dns_message_gettemprdataset(updatemsg, &rdataset);
	check_result(result, "dns_message_gettemprdataset");
	dns_rdatalist_init(rdatalist);
	rdatalist->type = rdatatype;
	rdatalist->rdclass = rdataclass;
	rdatalist->covers = rdatatype;
	rdatalist->ttl = ttl;
	ISC_LIST_INIT(rdatalist->rdata);
	ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	dns_rdataset_init(rdataset);
	dns_rdatalist_tordataset(rdatalist, rdataset);
	ISC_LIST_INIT(name->list);
	ISC_LIST_APPEND(name->list, rdataset, link);
	dns_message_addname(updatemsg, name, DNS_SECTION_UPDATE);
	return (STATUS_MORE);
}

static isc_uint16_t
evaluate_update(char *cmdline) {
	char *word;
	isc_boolean_t isdelete;

	ddebug ("evaluate_update()");
	word = nsu_strsep(&cmdline, " \t\r\n");
	if (*word == 0) {
		puts ("failed to read operation code");
		return (STATUS_SYNTAX);
	}
	if (strcasecmp(word, "delete") == 0)
		isdelete = ISC_TRUE;
	else if (strcasecmp(word, "add") == 0)
		isdelete = ISC_FALSE;
	else {
		printf ("incorrect operation code: %s\n", word);
		return (STATUS_SYNTAX);
	}
	return (update_addordelete(cmdline, isdelete));
}

static void
show_message(void) {
	isc_result_t result;
	char store[MSGTEXT];
	isc_buffer_t buf;

	ddebug ("show_message()");
	isc_buffer_init(&buf, store, MSGTEXT);
	result = dns_message_totext(updatemsg, 0, &buf);
	if (result != ISC_R_SUCCESS) {
		printf("Failed to concert message to text format.\n");
		return;
	}
	printf("Outgoing update query:\n%.*s",
	       (int)isc_buffer_usedlength(&buf),
	       (char*)isc_buffer_base(&buf));
}
	

static isc_uint16_t
get_next_command(void) {
	char cmdlinebuf[MAXCMD];
	char *cmdline;
	char *word;

	ddebug ("get_next_command()");
	fputs ("> ", stderr);
	fgets (cmdlinebuf, MAXCMD, stdin);
	cmdline = cmdlinebuf;
	word = nsu_strsep(&cmdline, " \t\r\n");

	if (feof(stdin))
		return (STATUS_QUIT);
	if (*word == 0)
		return (STATUS_SEND);
	if (strcasecmp(word, "quit") == 0)
		return (STATUS_QUIT);
	if (strcasecmp(word, "prereq") == 0)
		return (evaluate_prereq(cmdline));
	if (strcasecmp(word, "update") == 0)
		return (evaluate_update(cmdline));
	if (strcasecmp(word, "server") == 0)
		return (evaluate_server(cmdline));
	if (strcasecmp(word, "zone") == 0)
		return (evaluate_zone(cmdline));
	if (strcasecmp(word, "send") == 0)
		return (STATUS_SEND);
	if (strcasecmp(word, "show") == 0) {
		show_message();
		return (STATUS_MORE);
	}
	printf ("incorrect section name: %s\n", word);
	return (STATUS_SYNTAX);
}

static isc_boolean_t
user_interaction(void) {
	isc_uint16_t result = STATUS_MORE;

	ddebug ("user_interaction()");
	while ((result == STATUS_MORE) || (result == STATUS_SYNTAX))
		result = get_next_command();
	if (result == STATUS_SEND)
		return (ISC_TRUE);
	return (ISC_FALSE);

}

static void
get_address(char *host, in_port_t port, isc_sockaddr_t *sockaddr) {
        struct in_addr in4;
        struct in6_addr in6;
        struct hostent *he;

        ddebug("get_address()");
        if (have_ipv6 && inet_pton(AF_INET6, host, &in6) == 1)
                isc_sockaddr_fromin6(sockaddr, &in6, port);
        else if (inet_pton(AF_INET, host, &in4) == 1)
                isc_sockaddr_fromin(sockaddr, &in4, port);
        else {
                he = gethostbyname(host);
                if (he == NULL)
                     fatal("Couldn't look up your server host %s.  errno=%d",
                              host, h_errno);
                INSIST(he->h_addrtype == AF_INET);
                isc_sockaddr_fromin(sockaddr,
                                    (struct in_addr *)(he->h_addr_list[0]),
                                    port);
        }
}

static void
update_completed(isc_task_t *task, isc_event_t *event) {
	dns_requestevent_t *reqev = NULL;
	isc_result_t result;
	isc_buffer_t buf;
	dns_message_t *rcvmsg = NULL;
	char bufstore[MSGTEXT];
	
	UNUSED (task);

	ddebug ("updated_completed()");
	REQUIRE(event->ev_type == DNS_EVENT_REQUESTDONE);
	reqev = (dns_requestevent_t *)event;
	if (reqev->result != ISC_R_SUCCESS) {
		printf ("; Communication with server failed: %s\n",
			isc_result_totext(reqev->result));
		goto done;
	}

	result = dns_message_create(mctx, DNS_MESSAGE_INTENTPARSE, &rcvmsg);
	check_result(result, "dns_message_create");
	result = dns_request_getresponse(reqev->request, rcvmsg, ISC_TRUE);
	check_result(result, "dns_request_getresponse");
	if (debugging) {
		isc_buffer_init(&buf, bufstore, MSGTEXT);
		result = dns_message_totext(rcvmsg, 0, &buf);
		check_result(result, "dns_message_totext");
		printf ("\nReply from update query:\n%.*s\n",
			(int)isc_buffer_usedlength(&buf),
			(char*)isc_buffer_base(&buf));
	}
	dns_message_destroy(&rcvmsg);
 done:
	dns_request_destroy(&reqev->request);
	isc_event_free(&event);
	LOCK(&lock);
	busy = ISC_FALSE;
	SIGNAL(&cond);
	UNLOCK(&lock);
}

static void
send_update(void) {
	isc_result_t result;
	isc_sockaddr_t sockaddr;
	dns_request_t *request = NULL;
	char servername[MXNAME];
	isc_buffer_t buf;
	dns_name_t *name = NULL;
	dns_rdataset_t *rdataset = NULL;

	ddebug ("send_update()");

	result = dns_message_gettempname(updatemsg, &name);
	check_result(result, "dns_message_gettempname");
	if (zonename == NULL)
		fatal("don't have a valid zone yet.");
	dns_name_init(name, NULL);
	dns_name_clone(zonename, name);
	result = dns_message_gettemprdataset(updatemsg, &rdataset);
	check_result(result, "dns_message_gettemprdataset");
	dns_rdataset_makequestion(rdataset, dns_rdataclass_in,
				  dns_rdatatype_soa);
	ISC_LIST_INIT(name->list);
	ISC_LIST_APPEND(name->list, rdataset, link);
	dns_message_addname(updatemsg, name, DNS_SECTION_ZONE);

	isc_buffer_init(&buf, servername, MXNAME);
	result = dns_name_totext(master, ISC_TRUE, &buf);
	check_result(result, "dns_name_totext");

	servername[isc_buffer_usedlength(&buf)] = 0;	
	get_address(servername, 53, &sockaddr);
	result = dns_request_create(requestmgr, updatemsg, &sockaddr,
				    0, key,
				    FIND_TIMEOUT, global_task,
				    update_completed, NULL, &request);
	check_result(result, "dns_request_create");
}	
	

static void
find_completed(isc_task_t *task, isc_event_t *event) {
	dns_requestevent_t *reqev = NULL;
	dns_request_t *request = NULL;
	isc_result_t result, eresult;
	dns_message_t *rcvmsg = NULL;
	dns_message_t *soaquery = NULL;
	dns_section_t section;
	dns_name_t *name = NULL;
	dns_rdataset_t *soaset = NULL;
	dns_rdata_soa_t soa;
	dns_rdata_t soarr;
	int pass = 0;

	UNUSED(task);

	ddebug ("find_completed()");
	REQUIRE(event->ev_type == DNS_EVENT_REQUESTDONE);
	reqev = (dns_requestevent_t *)event;
	request = reqev->request;
	eresult = reqev->result;
	soaquery = reqev->ev_arg;

	isc_event_free(&event);
	reqev = NULL;

	if (eresult != ISC_R_SUCCESS) {
		printf ("; Communication with %s failed: %s\n",
			nameservername[ns_inuse], isc_result_totext(eresult));
		ns_inuse++;
		if (ns_inuse >= nameservers)
			fatal ("Couldn't talk to any default nameserver.");
		ddebug("Destroying request [%lx]", request);
		dns_request_destroy(&request);
		sendrequest(ns_inuse, soaquery, &request);
		return;
	}
	dns_message_destroy(&soaquery);

	ddebug ("About to create rcvmsg");
	result = dns_message_create(mctx, DNS_MESSAGE_INTENTPARSE, &rcvmsg);
	check_result(result, "dns_message_create");
	result = dns_request_getresponse(request, rcvmsg, ISC_TRUE);
	check_result(result, "dns_request_getresponse");
	section = DNS_SECTION_ANSWER;
	if (debugging) {
		isc_buffer_t buf;
		char bufstore[MSGTEXT];

		isc_buffer_init(&buf, bufstore, MSGTEXT);
		result = dns_message_totext(rcvmsg, 0, &buf);
		check_result(result, "dns_message_totext");
		printf ("Reply from SOA query:\n%.*s\n",
			(int)isc_buffer_usedlength(&buf),
			(char*)isc_buffer_base(&buf));
	}

	if (rcvmsg->rcode != dns_rcode_noerror &&
	    rcvmsg->rcode != dns_rcode_nxdomain)
		fatal("response to SOA query was unsuccessful");

 lookforsoa:
	if (pass == 0)
		section = DNS_SECTION_ANSWER;
	else if (pass == 1)
		section = DNS_SECTION_AUTHORITY;
	else
		fatal("response to SOA query didn't contain an SOA");


	result = dns_message_firstname(rcvmsg, section);
	if (result != ISC_R_SUCCESS) {
		pass++;
		goto lookforsoa;
	}
	while (result == ISC_R_SUCCESS) {
		name = NULL;
		dns_message_currentname(rcvmsg, section, &name);
		soaset = NULL;
		result = dns_message_findtype(name, dns_rdatatype_soa, 0,
					      &soaset);
		if (result == ISC_R_SUCCESS)
			break;
		result = dns_message_nextname(rcvmsg, section);
	}

	if (soaset == NULL) {
		pass++;
		goto lookforsoa;
	}

	if (debugging) {
		char namestr[1025];
		dns_name_format(name, namestr, sizeof(namestr));
		printf ("Found zone name: %s\n", namestr);
	}

	result = dns_rdataset_first(soaset);
	check_result(result, "dns_rdataset_first");

	dns_rdata_init(&soarr);
	dns_rdataset_current(soaset, &soarr);
	result = dns_rdata_tostruct(&soarr, &soa, NULL);
	check_result(result, "dns_rdata_tostruct");

	ddebug("Duping master");
	INSIST(master == NULL);
	master = isc_mem_get(mctx, sizeof(dns_name_t));
	dns_name_init(master, NULL);
	result = dns_name_dup(&soa.origin, mctx, master);
	check_result(result, "dns_name_dup");
	
	ddebug("Duping zonename");
	INSIST(zonename == NULL);
	zonename = isc_mem_get(mctx, sizeof(dns_name_t));
	dns_name_init(zonename, NULL);
	result = dns_name_dup(name, mctx, zonename);
	check_result(result, "dns_name_dup");

	dns_rdata_freestruct(&soa);
	
	if (debugging) {
		char namestr[1025];
		dns_name_format(master, namestr, sizeof(namestr));
		printf ("The master is: %s\n", namestr);
	}

	dns_message_destroy(&rcvmsg);
	dns_request_destroy(&request);
	ddebug ("Out of find_completed");
	send_update();
}

static void
sendrequest(int whichns, dns_message_t *msg, dns_request_t **request) {
	isc_sockaddr_t sockaddr;
	isc_result_t result;

	get_address(nameservername[whichns], 53, &sockaddr);
	result = dns_request_create(requestmgr, msg, &sockaddr,
				    0, NULL, FIND_TIMEOUT, global_task,
				    find_completed, msg, request);
	check_result(result, "dns_request_create");
}

static void
start_update(void) {
	isc_result_t result;
	dns_rdataset_t *rdataset = NULL;
	dns_name_t *name = NULL;
	dns_request_t *request = NULL;
	dns_message_t *soaquery = NULL;
	dns_name_t *firstname;

	ddebug ("start_update()");

	result = dns_message_firstname(updatemsg, DNS_SECTION_UPDATE);
	if (result != ISC_R_SUCCESS) {
		busy = ISC_FALSE;
		return;
	}

	result = dns_message_create(mctx, DNS_MESSAGE_INTENTRENDER,
				    &soaquery);
	check_result(result, "dns_message_create");

	soaquery->flags |= DNS_MESSAGEFLAG_RD;

	result = dns_message_gettempname(soaquery, &name);
	check_result(result, "dns_message_gettempname");

	result = dns_message_gettemprdataset(soaquery, &rdataset);
	check_result(result, "dns_message_gettemprdataset");

	dns_rdataset_makequestion(rdataset, dns_rdataclass_in,
				  dns_rdatatype_soa);

	firstname = NULL;
	dns_message_currentname(updatemsg, DNS_SECTION_UPDATE, &firstname);
	dns_name_init(name, NULL);
	dns_name_clone(firstname, name);

	ISC_LIST_INIT(name->list);
	ISC_LIST_APPEND(name->list, rdataset, link);
	dns_message_addname(soaquery, name, DNS_SECTION_QUESTION);

	ns_inuse = 0;
	sendrequest(ns_inuse, soaquery, &request);
}


static void
cleanup(void) {
	ddebug ("cleanup()");

	if (key != NULL) {
		ddebug("Freeing key");
		dns_tsigkey_setdeleted(key);
		dns_tsigkey_detach(&key);
	}

	if (keyring != NULL) {
		debug ("Freeing keyring %lx", keyring);
		dns_tsigkeyring_destroy(&keyring);
	}

	if (updatemsg != NULL)
		dns_message_destroy(&updatemsg);

	if (zonename != NULL) {
		dns_name_free(zonename, mctx);
		isc_mem_put(mctx, zonename, sizeof(dns_name_t));
		zonename = NULL;
	}
	if (master != NULL) {
		dns_name_free(master, mctx);
		isc_mem_put(mctx, master, sizeof(dns_name_t));
		master = NULL;
	}
	if (domain != NULL) {
		isc_mem_free(mctx, domain);
		domain = NULL;
	}
	if (is_dst_up) {
		debug ("Destroy DST lib");
		dst_lib_destroy();
		is_dst_up = ISC_FALSE;
	}
	if (entp != NULL) {
		debug ("Detach from entropy");
		isc_entropy_detach(&entp);
	}
		
	ddebug("Shutting down request manager");
	dns_requestmgr_shutdown(requestmgr);
	dns_requestmgr_detach(&requestmgr);

	ddebug("Freeing the dispatcher");
	dns_dispatch_detach(&dispatchv4);

	ddebug("Shutting down dispatch manager");
	dns_dispatchmgr_destroy(&dispatchmgr);

	ddebug("Ending task");
	isc_task_detach(&global_task);

	ddebug("Shutting down task manager");
	isc_taskmgr_destroy(&taskmgr);

	ddebug("Shutting down socket manager");
	isc_socketmgr_destroy(&socketmgr);

	ddebug("Shutting down timer manager");
	isc_timermgr_destroy(&timermgr);

	ddebug("Destroying memory context");
	if (isc_mem_debugging)
		isc_mem_stats(mctx, stderr);
	isc_mem_destroy(&mctx);
}

int
main(int argc, char **argv) {
        isc_result_t result;

        parse_args(argc, argv);

        setup_system();
        result = isc_mutex_init(&lock);
        check_result(result, "isc_mutex_init");
        result = isc_condition_init(&cond);
        check_result(result, "isc_condition_init");
	LOCK(&lock);

        while (ISC_TRUE) {
		reset_system();
                if (!user_interaction())
			break;
		busy = ISC_TRUE;
		start_update();
		while (busy)
			WAIT(&cond, &lock);
        }

        puts ("");
        ddebug ("Fell through app_run");
        isc_mutex_destroy(&lock);
        isc_condition_destroy(&cond);
        cleanup();

        return (0);
}
