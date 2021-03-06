/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1985-2009 AT&T Intellectual Property          *
*                      and is licensed under the                       *
*                  Common Public License, Version 1.0                  *
*                    by AT&T Intellectual Property                     *
*                                                                      *
*                A copy of the License is available at                 *
*            http://www.opensource.org/licenses/cpl1.0.txt             *
*         (with md5 checksum 059e8cd6165cb4c31e351f2b69388fd9)         *
*                                                                      *
*              Information and Software Systems Research               *
*                            AT&T Research                             *
*                           Florham Park NJ                            *
*                                                                      *
*                 Glenn Fowler <gsf@research.att.com>                  *
*                  David Korn <dgk@research.att.com>                   *
*                   Phong Vo <kpv@research.att.com>                    *
*                                                                      *
***********************************************************************/
#pragma prototyped
/*
 * Glenn Fowler
 * AT&T Bell Laboratories
 *
 * time conversion support
 */

#include <ast.h>
#include <tm.h>
#include <ctype.h>

/*
 * return minutes offset from absolute timezone expression
 *
 *	[[-+]hh[:mm[:ss]]]
 *	[-+]hhmm
 *
 * if e is non-null then it points to the first unrecognized char in s
 * d returned if no offset in s
 */

int
tmgoff(register const char* s, char** e, int d)
{
	register int	n = d;
	int		east;
	const char*	t = s;

	if ((east = *s == '+') || *s == '-')
	{
		s++;
		if (isdigit(*s) && isdigit(*(s + 1)))
		{
			n = ((*s - '0') * 10 + (*(s + 1) - '0')) * 60;
			s += 2;
			if (*s == ':')
				s++;
			if (isdigit(*s) && isdigit(*(s + 1)))
			{
				n += ((*s - '0') * 10 + (*(s + 1) - '0'));
				s += 2;
				if (*s == ':')
					s++;
				if (isdigit(*s) && isdigit(*(s + 1)))
					s += 2;
			}
			if (east)
				n = -n;
			t = s;
		}
	}
	if (e)
		*e = (char*)t;
	return n;
}
