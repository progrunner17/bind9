/*
 * Copyright (C) 1999  Internet Software Consortium.
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

#ifndef DNS_ZONE_H
#define DNS_ZONE_H 1

/***
 ***	Imports
 ***/

#include <isc/lang.h>
#include <isc/mem.h>
#include <isc/lex.h>
#include <isc/mutex.h>
#include <isc/time.h>
#include <isc/stdtime.h>
#include <isc/socket.h>

#include <dns/types.h>
#include <dns/result.h>
#include <dns/name.h>
#include <dns/fixedname.h>
#include <dns/rdataset.h>
#include <dns/callbacks.h>
#include <dns/confctx.h> 

typedef enum {
	dns_zone_none,
	dns_zone_master,
	dns_zone_slave,
	dns_zone_hint,
	dns_zone_stub,
	dns_zone_cache,
	dns_zone_forward
} dns_zonetype_t;

#define DNS_ZONE_O_SERVERS	0x00000001U	/* perform server checks */
#define DNS_ZONE_O_PARENTS	0x00000002U	/* perform parent checks */
#define DNS_ZONE_O_CHILDREN	0x00000004U	/* perform child checks */
#define DNS_ZONE_O_DIALUP	0x00000008U	/* zone xfr over dialup link */
#define DNS_ZONE_O_NOTIFY	0x00000010U	/* perform NOTIFY */

ISC_LANG_BEGINDECLS

/***
 ***	Functions
 ***/

dns_result_t dns_zone_create(dns_zone_t **zonep, isc_mem_t *mctx);

/*
 *	Creates a new empty zone and attach to it.
 *
 * Requires:
 *	'zonep' to point to a NULL pointer.
 *	'mctx' to be a valid memory context.
 *
 * Ensures:
 *	'*zonep' refers to a valid zone.
 *
 * Returns:
 *	DNS_R_SUCCESS
 *	DNS_R_NOMEMORY
 *	DNS_R_UNEXPECTED
 */

void dns_zone_setclass(dns_zone_t *zone, dns_rdataclass_t rdclass);
/*
 *	Sets the class of a zone.  This operation can only be performed
 *	once on a zone.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	dns_zone_setclass() not to have been called since the zone was
 *	initalised.
 *	'rdclass' != dns_rdataclass_none.
 */	

void dns_zone_settype(dns_zone_t *zone, dns_zonetype_t type);
/*
 *	Sets the zone type. This operation can only be performed once on
 *	a zone.
 *
 * Requires:
 *	'zone' to be a valid initalised zone.
 *	dns_zone_settype() not to have been called since the zone was
 *	initalised.
 *	'type' != dns_zone_none
 */

dns_result_t dns_zone_setorigin(dns_zone_t *zone, char *origin);
/*
 *	Sets the zones origin to 'origin'.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'origin' to be non NULL.
 *
 * Returns:
 *	All possible values from dns_name_fromtext().
 */

dns_name_t * dns_zone_getorigin(dns_zone_t *zone);
/*
 *	Returns the value of the origin.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_setdatabase(dns_zone_t *zone, const char *database);
/*
 *	Sets the name of the database to be loaded. 
 *	For databases loaded from MASTER files this corresponds to the
 *	file name of the MASTER file.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'database' to be non NULL.
 *
 * Returns:
 *	DNS_R_NOMEMORY
 *	DNS_R_SUCCESS
 */

dns_result_t dns_zone_setixfrlog(dns_zone_t *zone, const char *ixfrlog);
/*
 *	Sets the name of the IXFR log file.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'ixfrlog' to be non NULL.
 *
 * Returns:
 *	DNS_R_NOMEMORY
 *	DNS_R_SUCCESS
 */

dns_result_t dns_zone_setupdatelog(dns_zone_t *zone, char *updatelog);
/*
 *	Sets the name of the UPDATE log file.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'updatelog' to be non NULL.
 *
 * Returns:
 *	DNS_R_NOMEMORY
 *	DNS_R_SUCCESS
 */

dns_result_t dns_zone_load(dns_zone_t *zone);
/*
 *	Cause the database to be loaded from its backing store.
 *	Confirm that the mimimum requirements for the zone type are
 *	met, otherwise DNS_R_BADZONE is return.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *
 * Returns:
 *	DNS_R_UNEXPECTED
 *	DNS_R_SUCCESS
 *	DNS_R_BADZONE
 *	Any result value from dns_db_load().
 */

void dns_zone_checkservers(dns_zone_t *zone);
/*
 *	Initiate a consistancy check of the zones servers.
 *	XXX MPA to be implemented.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_checkparents(dns_zone_t *zone);
/*
 *	Initiate a consistancy check of the zone and the parent zone servers.
 *	XXX MPA to be implemented.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_checkchildren(dns_zone_t *zone);
/*
 *	Initiate a consistancy check of the child delegations from this zone.
 *	XXX MPA to be implemented.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_checkglue(dns_zone_t *zone);
/*
 *	Initiate a consistancy check of the glue records in this zone.
 *	XXX MPA to be implemented.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_attach(dns_zone_t *source, dns_zone_t **target);
/*
 *	Attach 'zone' to 'target'.  Increment reference count.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'target' to be non NULL and '*target' to be NULL.
 */

void dns_zone_detach(dns_zone_t **zonep);
/*
 *	Detach the current zone.  If this is the last reference to the
 *	zone it will be destroyed.
 *
 * Require:
 *	'zonep' to point to a valid initalised zone.
 */

void dns_zone_setflag(dns_zone_t *zone, unsigned int flags,
		      isc_boolean_t value);
/*
 *	Sets ('value' == 'ISC_TRUE') / clears ('value' == 'IS_FALSE')
 *	zone flags.  Valid flag bits are DNS_ZONE_F_*.
 *
 * Requires
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_adddbarg(dns_zone_t *zone, char *arg);
/*
 *	Add 'arg' to the end of the list of database arguements.
 *	No attempt in made to validate the arguements.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'arg' to be non NULL.
 *
 * Returns:
 *	DNS_R_NOMEMORY
 *	DNS_R_SUCCESS
 */

void dns_zone_cleardbargs(dns_zone_t *zone);
/*
 *	Clear all database arguements.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_getdb(dns_zone_t *zone, dns_db_t **dbp);
/*
 * 	Attach the database to '*dbp' if it exists otherwise
 *	return DNS_R_NOTLOADED.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'dbp' to be != NULL && '*dbp' == NULL.
 *
 * Returns:
 *	DNS_R_SUCCESS
 *	DNS_R_NOTLOADED
 */

dns_result_t dns_zone_setdbtype(dns_zone_t *zone, char *db_type);
/*
 *	Sets the database type. Current database types are: "rbt", "rbt64".
 *	'db_type' is not checked to see if it is a valid database type. 
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'database' to be non NULL.
 *
 * Returns:
 *	DNS_R_NOMEMORY
 *	DNS_R_SUCCESS
 */

void dns_zone_validate(dns_zone_t *zone);

	/* XXX MPA */

void dns_zone_expire(dns_zone_t *zone);
/*
 *	Mark the zone as expired.  If the zone requires dumping cause it to
 *	be initiated.  Set the refresh and retry intervals to there default
 *	values and unload the zone.
 *
 * Require
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_refresh(dns_zone_t *zone);
/*
 *	Initiate zone up to date checks.  The zone must already be being
 *	managed.
 *
 * Require
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_dump(dns_zone_t *zone, FILE *fd);
/*
 *	Write the zone to 'fd' in MASTER file format.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'fd' to be an active file handle open for writing.
 */

void dns_zone_maintenance(dns_zone_t *zone);
/*
 *	Perform regular maintenace on the zone.  This is called as a
 *	result of a zone being managed.
 *
 * Require
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_clearmasters(dns_zone_t *zone);
/*
 *	Clear the set of master servers the zone transfers from.
 *
 * Require
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_addmaster(dns_zone_t *zone, isc_sockaddr_t *master);
/*
 *	Add a master server to the end of the set of master servers for
 *	the zone.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'master' to be non NULL.
 *
 * Returns:
 *	DNS_R_SUCCESS
 *	DNS_R_NOMEMORY
 */

void dns_zone_clearnotify(dns_zone_t *zone);
/*
 *	Clear the set of additional servers to be notified when the zone
 *	changes.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_addnotify(dns_zone_t *zone, isc_sockaddr_t *notify);
/*
 *	Add a server to the end of the list of additional servers to be
 *	notified when a zone changes.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'notify' to be non NULL.
 *
 * Returns:
 *	DNS_R_SUCCESS
 *	DNS_R_NOMEMORY
 */

void dns_zone_unmount(dns_zone_t *zone);
	/* XXX MPA */

void dns_zone_unload(dns_zone_t *zone);
/*
 *	detach the database from the zone structure.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

dns_result_t dns_zone_manage(dns_zone_t *zone, isc_taskmgr_t *tmgr);
/*
 *	Bring the zone under control of a task manger.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'tmgr' to be a valid initalised task manager.
 *
 * Returns:
 *	DNS_R_UNEXPECTED
 *	DNS_R_SUCCESS
 */

void dns_zone_setoption(dns_zone_t *zone, unsigned int option,
		        isc_boolean_t value);
/*
 *	Set given options on ('value' == ISC_TRUE) or off ('value' ==
 *	ISC_FALSE).
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_clearoption(dns_zone_t *zone, unsigned int option);
/*
 *	Clear the given options from the zone and allow system wide value
 *	to be used.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_getoptions(dns_zone_t *zone, unsigned int *options,
			 unsigned int *optionsmask);
/*
 *	Return which options a set ('options') and which are active
 *	('optionsmask').
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'options' to be non NULL.
 *	'optionsmask' to be non NULL.
 */

void dns_zone_setrefresh(dns_zone_t *zone, isc_uint32_t refresh,
			 isc_uint32_t retry);
/*
 *	Set the refresh and retry values.  Normally this are set as a
 *	result of loading the zone (dns_zone_load).
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

dns_result_t
dns_zone_setxfrsource(dns_zone_t *zone, isc_sockaddr_t *xfrsource);
/*
 * 	Set the source address to be used in zone transfers.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 *	'xfrsource' to contain the address.
 *
 * Returns:
 *	DNS_R_SUCCESS
 */

isc_sockaddr_t *
dns_zone_getxfrsource(dns_zone_t *zone);
/*
 *	Returns the source address set by a previous dns_zone_setxfrsource
 *	call.  If dns_zone_setxfrsource the in6addr_any port 0 is returned.
 *
 * Require:
 *	'zone' to be a valid initalised zone.
 */

void dns_zone_setqueryacl(dns_zone_t *zone, dns_c_ipmatchlist_t *acl);
/*
 *	Sets the query acl list for the zone.
 *
 * Require:
 *	'zone' to be initalised.
 *	'acl' to be initalised.
 */

void dns_zone_setupdateacl(dns_zone_t *zone, dns_c_ipmatchlist_t *acl);
/*
 *	Sets the update acl list for the zone.
 *
 * Require:
 *	'zone' to be initalised.
 *	'acl' to be initalised.
 */

void dns_zone_setxfracl(dns_zone_t *zone, dns_c_ipmatchlist_t *acl);
/*
 *	Sets the transfer acl list for the zone.
 *
 * Require:
 *	'zone' to be initalised.
 *	'acl' to be initalised.
 */

dns_c_ipmatchlist_t * dns_zone_getqueryacl(dns_zone_t *zone);
/*
 * 	Returns the current query acl or NULL.
 *
 * Require:
 *	'zone' to be initalised.
 *
 * Returns:
 *	acl a pointer to the acl.
 *	NULL
 */

dns_c_ipmatchlist_t * dns_zone_getupdateacl(dns_zone_t *zone);
/*
 * 	Returns the current update acl or NULL.
 *
 * Require:
 *	'zone' to be initalised.
 *
 * Returns:
 *	acl a pointer to the acl.
 *	NULL
 */

dns_c_ipmatchlist_t * dns_zone_getxfracl(dns_zone_t *zone);
/*
 * 	Returns the current transfer acl or NULL.
 *
 * Require:
 *	'zone' to be initalised.
 *
 * Returns:
 *	acl a pointer to the acl.
 *	NULL
 */

void dns_zone_clearupdateacl(dns_zone_t *zone);
/*
 *	Clear the current update acl.
 *
 * Require:
 *	'zone' to be initalised.
 */

void dns_zone_clearqueryacl(dns_zone_t *zone);
/*
 *	Clear the current query acl.
 *
 * Require:
 *	'zone' to be initalised.
 */

void dns_zone_clearxfracl(dns_zone_t *zone);
/*
 *	Clear the current transfer acl.
 *
 * Require:
 *	'zone' to be initalised.
 */

void dns_zone_setchecknames(dns_zone_t *zone, dns_c_severity_t severity);
/*
 * 	Set the severity of name checking when loading a zone.
 *
 * Require:
 *      'zone' to be initalised.
 */

dns_c_severity_t dns_zone_getchecknames(dns_zone_t *zone);
/*
 *	Return the current severity of name checking.
 *
 * Require:
 *	'zone' to be initalised.
 */

void dns_zone_setpubkey(dns_zone_t *zone, dns_c_pubkey_t *pubkey);
/*
 *	Set the current public key to validate this and child zones
 *	via DNSSEC.
 *
 * Require:
 *	'zone' to be initalised.
 *	'pubkey' to be valid or NULL
 */

dns_c_pubkey_t * dns_zone_getpubkey(dns_zone_t *zone);
/*
 *	Return the current public key.
 *
 * Require:
 *	'zone' to be initalised.
 */

void dns_zone_setixfrlogsize(dns_zone_t *zone, isc_int32_t size);
isc_int32_t dns_zone_getixfrlogsize(dns_zone_t *zone);

void dns_zone_setmasterport(dns_zone_t *zone,  isc_uint16_t port);
isc_uint16_t dns_zone_getmasterport(dns_zone_t *zone);

void dns_zone_setresolver(dns_zone_t *zone, dns_resolver_t *resolver);

dns_result_t dns_zone_copy(isc_log_t *lctx, dns_c_ctx_t *ctx,
			   dns_c_zone_t *czone, dns_zone_t *zone);
/*
 *	Copy the zone configuration information into the zone structure.
 *
 * Require:
 *	'lctx' to be initalised or NULL.
 *	'ctx' to be initalised or NULL.
 *	'czone' to be initalised.
 *	'zone' to be initalised.
 */

dns_result_t dns_zone_notifyreceive(dns_zone_t *zone, isc_sockaddr_t *from,
				dns_message_t *msg);

void
dns_zone_setxfrtime(dns_zone_t *zone, isc_uint32_t xfrtime);
/*
 * Set the maximum time (in seconds) that a zone transfer (AXFR/IXFR)
 * in of this zone will use before being aborted.
 *
 * Requires:
 * 	'zone' to be valid initialised zone.
 *	'xfrtime' to be non zero.
 */

isc_uint32_t dns_zone_getxfrtime(dns_zone_t *zone);
/*
 * Returns the maximum transfer time for this zone.  This will be
 * either the value set by the last call to dns_zone_setxfrtime() or
 * the default value of 1 hour.
 *
 * Requires:
 *	'zone' to be valid initialised zone.
 */

dns_result_t dns_zone_setjournal(dns_zone_t *zone, const char *journal);

/*
 * Sets the filename used for journaling updates / IXFR transfers.
 * The default journal name is set by dns_zone_setdatabase() to be
 * "database.jnl".
 *
 * Requires:
 *	'zone' to be initalised.
 *	'journal' to be non NULL.
 *
 * Returns:
 *	DNS_R_SUCCESS
 *	DNS_R_NOMEMORY 
 */

char * dns_zone_getjournal(dns_zone_t *zone);

/*
 * Returns the journal name associated with this zone.
 * If not journal has been set this will be NULL.
 *
 * Requires:
 *	'zone' to be valid initialised zone.
 */

dns_zonetype_t dns_zone_gettype(dns_zone_t *zone);
/*
 * Returns the type of the zone (master/slave/etc.)
 *
 * Requires:
 *	'zone' to be valid initialised zone.
 */

void
dns_zone_settask(dns_zone_t *zone, isc_task_t *task);
/*
 * Give a zone a task to work with.  Any current task will be detached.
 *
 * Requires:
 *	'zone' to be valid.
 *	'task' to be valid.
 */

void
dns_zone_gettask(dns_zone_t *zone, isc_task_t **target);
/*
 * Attach the zone's task to '*target'.
 *
 * Requires:
 *	'zone' to be valid initialised zone.
 *	'zone' to have a task.
 *	'target' to be != NULL && '*target' == NULL.
 */

const char *dns_zone_getdatabase(dns_zone_t *zone);
/*
 * Gets the name of the database.  For databases loaded from
 * master files, this corresponds to the file name of the master file.
 *
 * Requires:
 *	'zone' to be valid initialised zone.
 */

const char *dns_zone_getixfrlog(dns_zone_t *zone);
/*
 * Gets the name of the IXFR log file.
 *
 * Requires:
 *	'zone' to be valid initialised zone.
 */

void dns_zone_notify(dns_zone_t *zone);
/*
 * Generate notify events for this zone.
 *
 * Requires:
 *	'zone' to be a valid zone.
 */

dns_result_t
dns_zone_replacedb(dns_zone_t *zone, dns_db_t *db,
                   isc_boolean_t dump);
/*
 * Replace the database of "zone" with a new database "db".
 *
 * If "dump" is ISC_TRUE, then the new zone contents are dumped
 * into to the zone's master file for persistence.  When replacing
 * a zone database by one just loaded from a master file, set
 * "dump" to ISC_FALSE to avoid a redunant redump of the data just
 * loaded.  Otherwise, it should be set to ISC_TRUE.
 *
 * If the "diff-on-reload" option is enabled in the configuration file,
 * the differences between the old and the new database are added to the
 * journal file, and the master file dump is postponed.
 *
 * Requires:
 *	'zone' to be a valid zone.
 */

isc_boolean_t
dns_zone_equal(dns_zone_t *oldzone, dns_zone_t *newzone);

/*
 * Tests whether the configuration of two zones is equal.
 * Zone contents and state information is not tested.
 *
 * Requires:
 *	'oldzone' and 'newzone' to be valid.
 *
 * Returns:
 *	ISC_TRUE if the configurations are equal.
 *	ISC_FALSE if the configurations differ.
 */

void
dns_zone_print(dns_zone_t *zone);
/*
 * test use only
 */

isc_result_t
dns_zonemgr_create(isc_mem_t *mctx, isc_taskmgr_t *taskmgr,
		   isc_timermgr_t *timermgr, isc_socketmgr_t *socketmgr,
		   dns_zonemgr_t **zmgrp);
/*
 * Create a zone manager.
 *
 * Requires:
 *	'mctx' to be a valid memory context.
 *	'taskmgr' to be a valid task manager.
 *	'timermgr' to be a valid timer manager.
 *	'zmgrp'	to point to a NULL pointer.
 */

isc_result_t
dns_zonemgr_managezone(dns_zonemgr_t *zmgr, dns_zone_t *zone);
/*
 * Start managing the zone 'zone' with zone manager 'zmgr'.
 */

isc_result_t
dns_zonemgr_forcemaint(dns_zonemgr_t *zmgr);
/*
 * Force zone maintenance of all zones managed by 'zmgr' at its
 * earliest conveniene.
 */

void
dns_zonemgr_destroy(dns_zonemgr_t **zmgrp);
/*
 * Destroy a zone manager.
 *
 * Requires:
 *	'*zmgrp' is a valid, non-NULL zone manager pointer.
 * Ensures:
 *	'*zmgrp' is NULL.
 */

ISC_LANG_ENDDECLS

#endif	/* DNS_ZONE_H */
