#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% SMI"
#

include	../Makefile.com

DATAFILES += depend

LICENSEFILES = ../../uts/common/gssapi/mechs/krb5/THIRDPARTYLICENSE

ACTION_SUNWgss=grep SUNWgss depend > /dev/null || \
	( chmod 666 depend; \
	echo "P SUNWgss	GSSAPI V2" >> depend; \
	chmod 444 depend );

ACTION_SUNWgssc=grep SUNWgssc depend > /dev/null || \
	( chmod 666 depend; \
	echo "P SUNWgssc	GSSAPI CONFIG V2" >> depend; \
	chmod 444 depend );

ACTION_SUNWkrbu=grep SUNWkrbu depend > /dev/null || \
	( chmod 666 depend; \
	echo "P SUNWkrbu	Kerberos version 5 support (Usr)" >> depend; \
	chmod 444 depend );

.KEEP_STATE:

all: $(FILES) action

install: all pkg

# action is a pseudotarget denoting completed work on the depend file
action: depend
	$(ACTION_SUNWgss)
	$(ACTION_SUNWgssc)
	$(ACTION_SUNWkrbu)
	touch $@

include ../Makefile.targ
