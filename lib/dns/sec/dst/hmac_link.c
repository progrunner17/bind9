/*
 * Portions Copyright (c) 1995-1998 by Network Associates, Inc.
 *
 * Permission to use, copy modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND NETWORK ASSOCIATES
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL
 * NETWORK ASSOCIATES BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THE SOFTWARE.
 */

/*
 * Principal Author: Brian Wellington
 * $Id: hmac_link.c,v 1.9 1999/10/08 13:08:56 bwelling Exp $
 */

#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>

#include <isc/assertions.h>
#include <isc/buffer.h>
#include <isc/int.h>
#include <isc/region.h>

#include <openssl/md5.h>

#include "dst_internal.h"
#include "dst_parse.h"

#define HMAC_LEN	64
#define HMAC_IPAD	0x36
#define HMAC_OPAD	0x5c
#define MD5_LEN		16

#define MD5Init MD5_Init
#define MD5Update MD5_Update
#define MD5Final MD5_Final

typedef struct hmackey {
	unsigned char ipad[64], opad[64];
} HMAC_Key;

static struct dst_func hmacmd5_functions;

static dst_result_t	dst_hmacmd5_sign(const unsigned int mode,
					 dst_key_t *key,
					 void **context, isc_region_t *data,
					 isc_buffer_t *sig, isc_mem_t *mctx);
static dst_result_t	dst_hmacmd5_verify(const unsigned int mode,
					   dst_key_t *key,
					   void **context, isc_region_t *data,
					   isc_region_t *sig, isc_mem_t *mctx);
static isc_boolean_t	dst_hmacmd5_compare(const dst_key_t *key1,
					    const dst_key_t *key2);
static dst_result_t	dst_hmacmd5_generate(dst_key_t *key, int exp,
					     isc_mem_t *mctx);
static isc_boolean_t	dst_hmacmd5_isprivate(const dst_key_t *key);
static void		dst_hmacmd5_destroy(void *key, isc_mem_t *mctx);
static dst_result_t	dst_hmacmd5_to_dns(const dst_key_t *in_key,
					   isc_buffer_t *data);
static dst_result_t	dst_hmacmd5_from_dns(dst_key_t *key, isc_buffer_t *data,
					     isc_mem_t *mctx);
static dst_result_t	dst_hmacmd5_to_file(const dst_key_t *key);
static dst_result_t	dst_hmacmd5_from_file(dst_key_t *key,
					      const isc_uint16_t id,
					      isc_mem_t *mctx);

/*
 * dst_s_hmacmd5_init()
 * Sets up function pointers for HMAC-MD5 related functions 
 */
void
dst_s_hmacmd5_init()
{
	REQUIRE(dst_t_func[DST_ALG_HMACMD5] == NULL);
	dst_t_func[DST_ALG_HMACMD5] = &hmacmd5_functions;
	memset(&hmacmd5_functions, 0, sizeof(struct dst_func));
	hmacmd5_functions.sign = dst_hmacmd5_sign;
	hmacmd5_functions.verify = dst_hmacmd5_verify;
	hmacmd5_functions.computesecret = NULL;
	hmacmd5_functions.compare = dst_hmacmd5_compare;
	hmacmd5_functions.paramcompare = NULL;
	hmacmd5_functions.generate = dst_hmacmd5_generate;
	hmacmd5_functions.isprivate = dst_hmacmd5_isprivate;
	hmacmd5_functions.destroy = dst_hmacmd5_destroy;
	hmacmd5_functions.to_dns = dst_hmacmd5_to_dns;
	hmacmd5_functions.from_dns = dst_hmacmd5_from_dns;
	hmacmd5_functions.to_file = dst_hmacmd5_to_file;
	hmacmd5_functions.from_file = dst_hmacmd5_from_file;
}

/*
 * dst_hmacmd5_sign
 *	Call HMAC-MD5 signing functions to sign a block of data.
 *	There are three steps to signing, INIT (initialize structures), 
 *	UPDATE (hash (more) data), FINAL (generate a signature).  This
 *	routine performs one or more of these steps.
 * Parameters
 *	mode		DST_SIGMODE_{INIT_UPDATE_FINAL|ALL}
 *	key		key to use for signing
 *	context		the context to use for this computation
 *	data		data to be signed
 *	signature	buffer to store signature
 *	mctx		memory context for temporary allocations
 * Returns 
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */
static dst_result_t
dst_hmacmd5_sign(const unsigned int mode, dst_key_t *key, void **context,
		 isc_region_t *data, isc_buffer_t *sig, isc_mem_t *mctx)
{
	isc_region_t r;
	MD5_CTX *ctx = NULL;
	
	if (mode & DST_SIGMODE_INIT) { 
		ctx = (MD5_CTX *) isc_mem_get(mctx, sizeof(MD5_CTX));
		if (ctx == NULL)
			return (DST_R_NOMEMORY);
	}
	else if (context != NULL) 
		ctx = (MD5_CTX *) *context;
	REQUIRE (ctx != NULL);

	if (mode & DST_SIGMODE_INIT) {
		HMAC_Key *hkey = key->opaque;

		MD5Init(ctx);
		MD5Update(ctx, hkey->ipad, HMAC_LEN);
	}

	if ((mode & DST_SIGMODE_UPDATE))
		MD5Update(ctx, data->base, data->length);

	if (mode & DST_SIGMODE_FINAL) {
		HMAC_Key *hkey = key->opaque;

		isc_buffer_available(sig, &r);
		if (r.length < MD5_LEN)
			return DST_R_NOSPACE;

		MD5Final(r.base, ctx);

		/* perform outer MD5 */
		MD5Init(ctx);
		MD5Update(ctx, hkey->opad, HMAC_LEN);
		MD5Update(ctx, r.base, MD5_LEN);
		MD5Final(r.base, ctx);
		isc_buffer_add(sig, MD5_LEN);

		isc_mem_put(mctx, ctx, sizeof(MD5_CTX));
	}
	else
		*context = ctx;

	return (DST_R_SUCCESS);
}


/*
 * dst_hmacmd5_verify 
 *	Calls HMAC-MD5 verification routines.  There are three steps to 
 *	verification, INIT (initialize structures), UPDATE (hash (more) data), 
 *	FINAL (generate a signature).  This routine performs one or more of 
 *	these steps.
 * Parameters
 *	mode		DST_SIGMODE_{INIT_UPDATE_FINAL|ALL}
 *	key		key to use for verifying
 *	context		the context to use for this computation
 *	data		signed data
 *	signature	signature
 *	mctx		memory context for temporary allocations
 * Returns 
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */
static dst_result_t
dst_hmacmd5_verify(const unsigned int mode, dst_key_t *key, void **context,
		   isc_region_t *data, isc_region_t *sig, isc_mem_t *mctx)
{
	MD5_CTX *ctx = NULL;
	
	if (mode & DST_SIGMODE_INIT) { 
		ctx = (MD5_CTX *) isc_mem_get(mctx, sizeof(MD5_CTX));
		if (ctx == NULL)
			return (DST_R_NOMEMORY);
	}
	else if (context != NULL) 
		ctx = (MD5_CTX *) *context;
	REQUIRE (ctx != NULL);

	if (mode & DST_SIGMODE_INIT) {
		HMAC_Key *hkey = key->opaque;

		MD5Init(ctx);
		MD5Update(ctx, hkey->ipad, HMAC_LEN);
	}

	if ((mode & DST_SIGMODE_UPDATE))
		MD5Update(ctx, data->base, data->length);

	if (mode & DST_SIGMODE_FINAL) {
		u_char digest[MD5_LEN];
		HMAC_Key *hkey = key->opaque;

		if (sig->length < MD5_LEN)
			return (DST_R_VERIFYFINALFAILURE);

		MD5Final(digest, ctx);

		/* perform outer MD5 */
		MD5Init(ctx);
		MD5Update(ctx, hkey->opad, HMAC_LEN);
		MD5Update(ctx, digest, MD5_LEN);
		MD5Final(digest, ctx);

		isc_mem_put(mctx, ctx, sizeof(MD5_CTX));

		if (memcmp(digest, sig->base, MD5_LEN) != 0)
			return (DST_R_VERIFYFINALFAILURE);
	}
	else
		*context = ctx;

	return (DST_R_SUCCESS);
}

/*
 * dst_hmacmd5_isprivate
 *	Is this a private key?  Yes
 * Parameters
 *	key		DST KEY structure
 * Returns
 *	ISC_TRUE
 */
static isc_boolean_t
dst_hmacmd5_isprivate(const dst_key_t *key) {
	key = key; /* suppress warning */

        return (ISC_TRUE);
}


/*
 * dst_hmacmd5_to_dns
 *	Converts key from HMAC to DNS rdata (raw bytes)
 * Parameters
 *	key		DST KEY structure
 *	data		output data
 * Returns
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */

static dst_result_t
dst_hmacmd5_to_dns(const dst_key_t *key, isc_buffer_t *data) {
	HMAC_Key *hkey;
	isc_region_t r;
	unsigned int bytes, i;

	REQUIRE(key->opaque != NULL);

	hkey = (HMAC_Key *) key->opaque;

	isc_buffer_available(data, &r);

	bytes = (key->key_size + 7) / 8;
	if (r.length < bytes)
		return (DST_R_NOSPACE);

	for (i = 0; i < bytes; i++)
		*r.base++ = hkey->ipad[i] ^ HMAC_IPAD;

	isc_buffer_add(data, bytes);

	return (DST_R_SUCCESS);
}


/*
 * dst_hmacmd5_from_dns
 *	Converts from a DNS KEY RR format to an HMAC-MD5 KEY. 
 * Parameters
 *	key		Partially filled key structure
 *	data		Buffer containing key in DNS format
 * Return
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */
static dst_result_t
dst_hmacmd5_from_dns(dst_key_t *key, isc_buffer_t *data, isc_mem_t *mctx) {
	HMAC_Key *hkey;
	isc_region_t r;
	int i, keylen;

	isc_buffer_remaining(data, &r);
	if (r.length == 0)
		return (DST_R_SUCCESS);

	hkey = (HMAC_Key *) isc_mem_get(mctx, sizeof(HMAC_Key));
	if (hkey == NULL)
		return (DST_R_NOMEMORY);

	memset(hkey->ipad, 0, sizeof(hkey->ipad));
	memset(hkey->opad, 0, sizeof(hkey->opad));

	if (r.length > HMAC_LEN) {
		MD5_CTX ctx;
		unsigned char digest[MD5_LEN];

		MD5Init(&ctx);
		MD5Update(&ctx, r.base, r.length);
		MD5Final(digest, &ctx);
		memcpy(hkey->ipad, digest, MD5_LEN);
		memcpy(hkey->opad, digest, MD5_LEN);
		keylen = MD5_LEN;
	}
	else {
		memcpy(hkey->ipad, r.base, r.length);
		memcpy(hkey->opad, r.base, r.length);
		keylen = r.length;
	}
	
	/* XOR key with ipad and opad values */
	for (i = 0; i < HMAC_LEN; i++) {
		hkey->ipad[i] ^= HMAC_IPAD;
		hkey->opad[i] ^= HMAC_OPAD;
	}
	key->key_size = keylen * 8;
	key->opaque = hkey;

	return (DST_R_SUCCESS);
}


/*
 * dst_hmacmd5_to_file
 *	Encodes an HMAC-MD5 Key into the portable file format.
 * Parameters 
 *	key		DST KEY structure 
 * Returns
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */
static dst_result_t
dst_hmacmd5_to_file(const dst_key_t *key) {
	int i, cnt = 0;
	HMAC_Key *hkey;
	dst_private_t priv;
	unsigned char keydata[HMAC_LEN];

	if (key->opaque == NULL)
		return (DST_R_NULLKEY);

	hkey = (HMAC_Key *) key->opaque;
	for (i = 0; i < HMAC_LEN; i++)
		keydata[i] = hkey->ipad[i] ^ HMAC_IPAD;

	priv.elements[cnt].tag = TAG_HMACMD5_KEY;
	priv.elements[cnt].length = HMAC_LEN;
	priv.elements[cnt++].data = keydata;

	priv.nelements = cnt;
	return (dst_s_write_private_key_file(key->key_name, key->key_alg,
					     key->key_id, &priv));
}


/*
 * dst_hmacmd5_from_file
 *	Converts contents of a private key file into a private HMAC-MD5 key. 
 * Parameters 
 *	key		Partially filled HMAC-MD5 KEY structure
 *	id		The key id
 *	path		The directory that the file will be read from
 * Return
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */

static dst_result_t 
dst_hmacmd5_from_file(dst_key_t *key, const isc_uint16_t id, isc_mem_t *mctx) {
	dst_private_t priv;
	dst_result_t ret;
	isc_buffer_t b;
	HMAC_Key *hkey = NULL;
#define DST_RET(a) {ret = a; goto err;}

	/* read private key file */
	ret = dst_s_parse_private_key_file(key->key_name, key->key_alg, 
					   id, &priv, mctx);
	if (ret != DST_R_SUCCESS)
		return (ret);

	hkey = (HMAC_Key *) isc_mem_get(mctx, sizeof(HMAC_Key *));
	if (hkey == NULL)
		DST_RET(DST_R_NOMEMORY);

	key->opaque = hkey;
	isc_buffer_init(&b, priv.elements[0].data, priv.elements[0].length,
			ISC_BUFFERTYPE_BINARY);
	ret = dst_hmacmd5_from_dns(key, &b, mctx);
	if (ret != DST_R_SUCCESS)
		DST_RET(ret);

	return (DST_R_SUCCESS);

 err:
	dst_hmacmd5_destroy(hkey, mctx);
	dst_s_free_private_structure_fields(&priv, mctx);
	memset(&priv, 0, sizeof(priv));
	return (ret);
}

/*
 * dst_hmacmd5_destroy
 *	Frees all dynamically allocated structures in key.
 */
static void
dst_hmacmd5_destroy(void *key, isc_mem_t *mctx) {
	HMAC_Key *hkey = (HMAC_Key *) key;
	memset(hkey, 0, sizeof(HMAC_Key));
	isc_mem_put(mctx, hkey, sizeof(HMAC_Key));
}


/*
 *  dst_hmacmd5_generate
 *	Creates an HMAC-MD5 key.  If the specified size is more than 512
 *	bits, a key of size 512 is generated.
 *  Parameters
 *	key		DST Key structure
 *	unused		algorithm specific data, unused for HMAC-MD5.
 *	mctx		memory context to allocate key
 *  Return 
 *	DST_R_SUCCESS	Success
 *	!DST_R_SUCCESS	Failure
 */

static dst_result_t
dst_hmacmd5_generate(dst_key_t *key, int unused, isc_mem_t *mctx) {
	isc_buffer_t b;
	dst_result_t ret;
	int bytes;
	unsigned char data[HMAC_LEN];

	unused = unused;	/* make the compiler happy */

	bytes = (key->key_size + 7) / 8;
	if (bytes > 64) {
		bytes = 64;
		key->key_size = 512;
	}

	memset(data, 0, HMAC_LEN);
	isc_buffer_init(&b, data, sizeof(data), ISC_BUFFERTYPE_BINARY);
	ret = dst_random(bytes, &b);
	if (ret != DST_R_SUCCESS)
		return (ret);

	ret = dst_hmacmd5_from_dns(key, &b, mctx);
	memset(data, 0, HMAC_LEN);

	return ret;
}


/************************************************************************** 
 *  dst_hmacmd5_compare
 *	Compare two keys for equality.
 *  Return
 *	ISC_TRUE	The keys are equal
 *	ISC_FALSE	The keys are not equal
 */
static isc_boolean_t
dst_hmacmd5_compare(const dst_key_t *key1, const dst_key_t *key2) {
	HMAC_Key *hkey1, *hkey2;

	hkey1 = (HMAC_Key *) key1->opaque;
	hkey2 = (HMAC_Key *) key2->opaque;

	if (hkey1 == NULL && hkey2 == NULL) 
		return(ISC_TRUE);
	else if (hkey1 == NULL || hkey2 == NULL)
		return (ISC_FALSE);

	if (memcmp(hkey1->ipad, hkey2->ipad, HMAC_LEN) == 0)
		return (ISC_TRUE);
	else
		return (ISC_FALSE);
}
