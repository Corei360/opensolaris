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
#pragma ident	"%Z%%M%	%I%	%E% SMI"
#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# uts/intel/ia32/sys/Makefile
#
# include global definitions
include ../../../../Makefile.master

HDRS=  \
	asm_linkage.h	\
	kdi_regs.h	\
	machtypes.h	\
	privmregs.h	\
	privregs.h	\
	psw.h		\
	pte.h		\
	reg.h		\
	stack.h		\
	trap.h		\
	traptrace.h

ROOTINCISA= $(ROOT)/usr/include/ia32
ROOTDIR= $(ROOTINCISA)/sys
ROOTDIRS= $(ROOTDIR)

ROOTHDRS= $(HDRS:%=$(ROOTDIR)/%)

CHECKHDRS= $(HDRS:%.h=%.check)

# install rules
$(ROOTDIR)/%: %
	$(INS.file)

.KEEP_STATE:

.PARALLEL: $(CHECKHDRS) $(ROOTHDRS)

install_h: $(ROOTINCISA) .WAIT $(ROOTDIRS) .WAIT $(ROOTHDRS)

$(ROOTINCISA):
	$(INS.dir)

$(ROOTDIRS):	$(ROOTINCISA)
	$(INS.dir)

check:	$(CHECKHDRS)
