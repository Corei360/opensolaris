/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * From "misc.c	5.15	00/05/31 SMI; TSOL 2.x"
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 *	Miscellaneous user interfaces to trusted label functions.
 */


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>
#include <libintl.h>
#include <libtsnet.h>
#include <tsol/label.h>

#include <net/route.h>

#define	MAX_STRING_SIZE 256
#define	MAX_ATTR_LEN	1024

/*
 * Parse off an entry from a line.  Entry is stored in 'outbuf'.  Returned
 * value is a pointer to the first unprocessed input character from 'instr'.
 */
const char *
parse_entry(char *outbuf, size_t outlen, const char *instr,
    const char *delimit)
{
	boolean_t escape_state = B_FALSE;
	boolean_t any_white;
	char chr;

	any_white = strchr(delimit, '\n') != NULL;

	/*
	 * User may specify outlen as 0 to skip over a field without storing
	 * it anywhere.  Otherwise, we need at least one byte for the
	 * terminating NUL plus one byte to store another byte from instr.
	 */
	while (outlen != 1 && (chr = *instr++) != '\0') {
		if (!escape_state) {
			if (chr == '\\') {
				escape_state = B_TRUE;
				continue;
			}
			if (strchr(delimit, chr) != NULL)
				break;
			if (any_white && isspace(chr))
				break;
		}
		escape_state = B_FALSE;
		if (outlen > 0) {
			*outbuf++ = chr;
			outlen--;
		}
	}
	if (outlen != 1)
		instr--;
	if (escape_state)
		instr--;
	if (outlen > 0)
		*outbuf = '\0';
	return (instr);
}

const char *
sl_to_str(const bslabel_t *sl)
{
	const char *sl_str;
	static const char unknown_str[] = "UNKNOWN";

	if (sl == NULL)
		return (unknown_str);

	if ((sl_str = sbsltos(sl, MAX_STRING_SIZE)) == NULL &&
	    (sl_str = bsltoh(sl)) == NULL)
		sl_str = unknown_str;
	return (sl_str);
}

static const char *rtsa_keywords[] = {
#define	SAK_MINSL	0
	"min_sl",
#define	SAK_MAXSL	1
	"max_sl",
#define	SAK_DOI		2
	"doi",
#define	SAK_CIPSO	3
	"cipso",
#define	SAK_SL		4
	"sl",
#define	SAK_INVAL	5
	NULL
};

const char *
rtsa_to_str(const struct rtsa_s *rtsa, char *line, size_t len)
{
	size_t slen;
	uint32_t mask, i;

	slen = 0;
	*line = '\0';
	mask = rtsa->rtsa_mask;

	for (i = 1; mask != 0 && i != 0 && slen < len - 1; i <<= 1) {
		if (!(i & (RTSA_MINSL|RTSA_MAXSL|RTSA_DOI|RTSA_CIPSO)))
			continue;
		if (!(i & mask))
			continue;
		if (slen != 0)
			line[slen++] = ',';
		switch (i & mask) {
		case RTSA_MINSL:
			if ((mask & RTSA_MAXSL) &&
			    blequal(&rtsa->rtsa_slrange.lower_bound,
			    &rtsa->rtsa_slrange.upper_bound)) {
				slen += snprintf(line + slen, len - slen,
				    "sl=%s",
				    sl_to_str(&rtsa->rtsa_slrange.lower_bound));
				mask ^= RTSA_MAXSL;
				break;
			}
			slen += snprintf(line + slen, len - slen, "min_sl=%s",
			    sl_to_str(&rtsa->rtsa_slrange.lower_bound));
			break;
		case RTSA_MAXSL:
			slen += snprintf(line + slen, len - slen, "max_sl=%s",
			    sl_to_str(&rtsa->rtsa_slrange.upper_bound));
			break;
		case RTSA_DOI:
			slen += snprintf(line + slen, len - slen, "doi=%d",
			    rtsa->rtsa_doi);
			break;
		case RTSA_CIPSO:
			slen += snprintf(line + slen, len - slen, "cipso");
			break;
		}
	}

	return (line);
}

boolean_t
rtsa_keyword(const char *options, struct rtsa_s *sp, int *errp, char **errstrp)
{
	const char *valptr, *nxtopt;
	uint32_t mask = 0, doi;
	int key;
	bslabel_t min_sl, max_sl;
	char attrbuf[MAX_ATTR_LEN];
	const char **keyword;
	int err;
	char *errstr, *cp;

	if (errp == NULL)
		errp = &err;
	if (errstrp == NULL)
		errstrp = &errstr;

	*errstrp = (char *)options;

	while (*options != '\0') {
		valptr = parse_entry(attrbuf, sizeof (attrbuf), options, ",=");

		if (attrbuf[0] == '\0') {
			*errstrp = (char *)options;
			*errp = LTSNET_ILL_ENTRY;
			return (B_FALSE);
		}
		for (keyword = rtsa_keywords; *keyword != NULL; keyword++)
			if (strcmp(*keyword, attrbuf) == 0)
				break;
		if ((key = keyword - rtsa_keywords) == SAK_INVAL) {
			*errstrp = (char *)options;
			*errp = LTSNET_ILL_KEY;
			return (B_FALSE);
		}
		if ((key == SAK_CIPSO && *valptr == '=') ||
		    (key != SAK_CIPSO && *valptr != '=')) {
			*errstrp = (char *)valptr;
			*errp = LTSNET_ILL_VALDELIM;
			return (B_FALSE);
		}

		nxtopt = valptr;
		if (*valptr == '=') {
			valptr++;
			nxtopt = parse_entry(attrbuf, sizeof (attrbuf),
			    valptr, ",=");
			if (*nxtopt == '=') {
				*errstrp = (char *)nxtopt;
				*errp = LTSNET_ILL_KEYDELIM;
				return (B_FALSE);
			}
		}
		if (*nxtopt == ',')
			nxtopt++;

		switch (key) {
		case SAK_MINSL:
			if (mask & RTSA_MINSL) {
				*errstrp = (char *)options;
				*errp = LTSNET_DUP_KEY;
				return (B_FALSE);
			}
			if (stobsl(attrbuf, &min_sl, NO_CORRECTION,
			    &err) != 1) {
				*errstrp = (char *)valptr;
				*errp = LTSNET_ILL_LOWERBOUND;
				return (B_FALSE);
			}
			mask |= RTSA_MINSL;
			break;

		case SAK_MAXSL:
			if (mask & RTSA_MAXSL) {
				*errstrp = (char *)options;
				*errp = LTSNET_DUP_KEY;
				return (B_FALSE);
			}
			if (stobsl(attrbuf, &max_sl, NO_CORRECTION,
			    &err) != 1) {
				*errstrp = (char *)valptr;
				*errp = LTSNET_ILL_UPPERBOUND;
				return (B_FALSE);
			}
			mask |= RTSA_MAXSL;
			break;

		case SAK_SL:
			if (mask & (RTSA_MAXSL|RTSA_MINSL)) {
				*errstrp = (char *)options;
				*errp = LTSNET_DUP_KEY;
				return (B_FALSE);
			}
			if (stobsl(attrbuf, &min_sl, NO_CORRECTION,
			    &err) != 1) {
				*errstrp = (char *)valptr;
				*errp = LTSNET_ILL_LABEL;
				return (B_FALSE);
			}
			bcopy(&min_sl, &max_sl, sizeof (bslabel_t));
			mask |= (RTSA_MINSL | RTSA_MAXSL);
			break;

		case SAK_DOI:
			if (mask & RTSA_DOI) {
				*errstrp = (char *)options;
				*errp = LTSNET_DUP_KEY;
				return (B_FALSE);
			}
			errno = 0;
			doi = strtoul(attrbuf, &cp, 0);
			if (doi == 0 || errno != 0 || *cp != '\0') {
				*errstrp = (char *)valptr;
				*errp = LTSNET_ILL_DOI;
				return (B_FALSE);
			}
			mask |= RTSA_DOI;
			break;

		case SAK_CIPSO:
			if (mask & RTSA_CIPSO) {
				*errstrp = (char *)options;
				*errp = LTSNET_DUP_KEY;
				return (B_FALSE);
			}
			mask |= RTSA_CIPSO;
			break;
		}

		options = nxtopt;
	}

	/* Defaults to CIPSO if not specified */
	mask |= RTSA_CIPSO;

	/* If RTSA_CIPSO is specified, RTSA_DOI must be specified */
	if (!(mask & RTSA_DOI)) {
		*errp = LTSNET_NO_DOI;
		return (B_FALSE);
	}

	/* SL range must be specified */
	if (!(mask & (RTSA_MINSL|RTSA_MAXSL))) {
		*errp = LTSNET_NO_RANGE;
		return (B_FALSE);
	}
	if (!(mask & RTSA_MINSL)) {
		*errp = LTSNET_NO_LOWERBOUND;
		return (B_FALSE);
	}
	if (!(mask & RTSA_MAXSL)) {
		*errp = LTSNET_NO_UPPERBOUND;
		return (B_FALSE);
	}

	/* SL range must have upper bound dominating lower bound */
	if (!bldominates(&max_sl, &min_sl)) {
		*errp = LTSNET_ILL_RANGE;
		return (B_FALSE);
	}

	if (mask & RTSA_MINSL)
		sp->rtsa_slrange.lower_bound = min_sl;
	if (mask & RTSA_MAXSL)
		sp->rtsa_slrange.upper_bound = max_sl;
	if (mask & RTSA_DOI)
		sp->rtsa_doi = doi;
	sp->rtsa_mask = mask;

	return (B_TRUE);
}

/* Keep in sync with libtsnet.h */
static const char *tsol_errlist[] = {
	"No error",
	"System error",
	"Empty string or end of list",
	"Entry is malformed",
	"Missing name",
	"Missing attributes",
	"Illegal name",
	"Illegal keyword delimiter",
	"Unknown keyword",
	"Duplicate keyword",
	"Illegal value delimiter",
	"Missing host type",
	"Illegal host type",
	"Missing label",
	"Illegal label",
	"Missing label range",
	"Illegal label range",
	"No lower bound in range",
	"Illegal lower bound in range",
	"No upper bound in range",
	"Illegal upper bound in range",
	"Missing DOI",
	"Illegal DOI",
	"Too many entries in set",
	"Missing address/network",
	"Illegal address/network",
	"Illegal flag",
	"Illegal MLP specification",
	"Unacceptable keyword for type"
};
static const int tsol_nerr = sizeof (tsol_errlist) / sizeof (*tsol_errlist);

const char *
tsol_strerror(int libtserr, int errnoval)
{
	if (libtserr == LTSNET_SYSERR)
		return (strerror(errnoval));
	if (libtserr >= 0 && libtserr < tsol_nerr)
		return (gettext(tsol_errlist[libtserr]));
	return (gettext("Unknown error"));
}
