/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*! \file */

#include <ctype.h>
#include <stdbool.h>

#include <isc/buffer.h>
#include <isc/hex.h>
#include <isc/lex.h>
#include <isc/string.h>
#include <isc/util.h>

#define RETERR(x)                        \
	do {                             \
		isc_result_t _r = (x);   \
		if (_r != ISC_R_SUCCESS) \
			return (_r);     \
	} while (0)

/*
 * BEW: These static functions are copied from lib/dns/rdata.c.
 */
static isc_result_t
str_totext(const char *source, isc_buffer_t *target);

static isc_result_t
mem_tobuffer(isc_buffer_t *target, void *base, unsigned int length);

static const char hex[] = "0123456789ABCDEF";

isc_result_t
isc_hex_totext(isc_region_t *source, int wordlength, const char *wordbreak,
	       isc_buffer_t *target)
{
	char	     buf[3];
	unsigned int loops = 0;

	if (wordlength < 2)
		wordlength = 2;

	memset(buf, 0, sizeof(buf));
	while (source->length > 0) {
		buf[0] = hex[(source->base[0] >> 4) & 0xf];
		buf[1] = hex[(source->base[0]) & 0xf];
		RETERR(str_totext(buf, target));
		isc_region_consume(source, 1);

		loops++;
		if (source->length != 0 &&
		    (int)((loops + 1) * 2) >= wordlength) {
			loops = 0;
			RETERR(str_totext(wordbreak, target));
		}
	}
	return (ISC_R_SUCCESS);
}

/*%
 * State of a hex decoding process in progress.
 */
typedef struct {
	int	      length; /*%< Desired length of binary data or -1 */
	isc_buffer_t *target; /*%< Buffer for resulting binary data */
	int	      digits; /*%< Number of buffered hex digits */
	int	      val[2];
} hex_decode_ctx_t;

static inline void
hex_decode_init(hex_decode_ctx_t *ctx, int length, isc_buffer_t *target)
{
	ctx->digits = 0;
	ctx->length = length;
	ctx->target = target;
}

static inline isc_result_t
hex_decode_char(hex_decode_ctx_t *ctx, int c)
{
	const char *s;

	if ((s = strchr(hex, toupper(c))) == NULL)
		return (ISC_R_BADHEX);
	ctx->val[ctx->digits++] = (int)(s - hex);
	if (ctx->digits == 2) {
		unsigned char num;

		num = (ctx->val[0] << 4) + (ctx->val[1]);
		RETERR(mem_tobuffer(ctx->target, &num, 1));
		if (ctx->length >= 0) {
			if (ctx->length == 0)
				return (ISC_R_BADHEX);
			else
				ctx->length -= 1;
		}
		ctx->digits = 0;
	}
	return (ISC_R_SUCCESS);
}

static inline isc_result_t
hex_decode_finish(hex_decode_ctx_t *ctx)
{
	if (ctx->length > 0)
		return (ISC_R_UNEXPECTEDEND);
	if (ctx->digits != 0)
		return (ISC_R_BADHEX);
	return (ISC_R_SUCCESS);
}

isc_result_t
isc_hex_tobuffer(isc_lex_t *lexer, isc_buffer_t *target, int length)
{
	unsigned int	  before, after;
	hex_decode_ctx_t  ctx;
	isc_textregion_t *tr;
	isc_token_t	  token;
	bool		  eol;

	REQUIRE(length >= -2);

	hex_decode_init(&ctx, length, target);

	before = isc_buffer_usedlength(target);
	while (ctx.length != 0) {
		unsigned int i;

		if (length > 0) {
			eol = false;
		} else {
			eol = true;
		}
		RETERR(isc_lex_getmastertoken(lexer, &token,
					      isc_tokentype_string, eol));
		if (token.type != isc_tokentype_string) {
			break;
		}
		tr = &token.value.as_textregion;
		for (i = 0; i < tr->length; i++) {
			RETERR(hex_decode_char(&ctx, tr->base[i]));
		}
	}
	after = isc_buffer_usedlength(target);
	if (ctx.length < 0) {
		isc_lex_ungettoken(lexer, &token);
	}
	RETERR(hex_decode_finish(&ctx));
	if (length == -2 && before == after) {
		return (ISC_R_UNEXPECTEDEND);
	}
	return (ISC_R_SUCCESS);
}

isc_result_t
isc_hex_decodestring(const char *cstr, isc_buffer_t *target)
{
	hex_decode_ctx_t ctx;

	hex_decode_init(&ctx, -1, target);
	for (;;) {
		int c = *cstr++;
		if (c == '\0')
			break;
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		RETERR(hex_decode_char(&ctx, c));
	}
	RETERR(hex_decode_finish(&ctx));
	return (ISC_R_SUCCESS);
}

static isc_result_t
str_totext(const char *source, isc_buffer_t *target)
{
	unsigned int l;
	isc_region_t region;

	isc_buffer_availableregion(target, &region);
	l = strlen(source);

	if (l > region.length)
		return (ISC_R_NOSPACE);

	memmove(region.base, source, l);
	isc_buffer_add(target, l);
	return (ISC_R_SUCCESS);
}

static isc_result_t
mem_tobuffer(isc_buffer_t *target, void *base, unsigned int length)
{
	isc_region_t tr;

	isc_buffer_availableregion(target, &tr);
	if (length > tr.length)
		return (ISC_R_NOSPACE);
	memmove(tr.base, base, length);
	isc_buffer_add(target, length);
	return (ISC_R_SUCCESS);
}
