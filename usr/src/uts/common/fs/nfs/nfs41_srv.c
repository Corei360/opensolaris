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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 *	Copyright (c) 1983,1984,1985,1986,1987,1988,1989  AT&T.
 *	All Rights Reserved
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/cred.h>
#include <sys/buf.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/kmem.h>
#include <sys/dirent.h>
#include <sys/cmn_err.h>
#include <sys/disp.h>
#include <sys/debug.h>
#include <sys/systeminfo.h>
#include <sys/flock.h>
#include <sys/pathname.h>
#include <sys/nbmlock.h>
#include <sys/share.h>
#include <sys/atomic.h>
#include <sys/policy.h>
#include <sys/fem.h>
#include <sys/sdt.h>
#include <sys/ddi.h>
#include <sys/modctl.h>

#include <rpc/types.h>
#include <rpc/auth.h>
#include <rpc/rpcsec_gss.h>
#include <rpc/svc.h>

#include <nfs/nfs.h>
#include <nfs/export.h>
#include <nfs/lm.h>
#include <nfs/nfs4.h>

#include <sys/strsubr.h>
#include <sys/strsun.h>

#include <inet/common.h>
#include <inet/ip.h>
#include <inet/ip6.h>

#include <sys/tsol/label.h>
#include <sys/tsol/tndb.h>

#include <nfs/nfs4_attrmap.h>
#include <nfs/nfs4_srv_attr.h>
#include <nfs/mds_state.h>
#include <nfs/mds_odl.h>

#include <nfs/nfs41_filehandle.h>
#include <nfs/ctl_mds_clnt.h>

#define	RFS4_MAXLOCK_TRIES 4	/* Try to get the lock this many times */
static int rfs4_maxlock_tries = RFS4_MAXLOCK_TRIES;
#define	RFS4_LOCK_DELAY 10	/* Milliseconds */
static clock_t rfs4_lock_delay = RFS4_LOCK_DELAY;

int mds_strict_seqid = 0;

static void ping_cb_null_thr(mds_session_t *);

/* End of Tunables */

/*
 * Used to bump the stateid4.seqid value and show changes in the stateid
 */
#define	next_stateid(sp) (++(sp)->v41_bits.chgseq)

/*
 * RFS4_MINLEN_ENTRY4: XDR-encoded size of smallest possible dirent.
 *	This is used to return NFS4ERR_TOOSMALL when clients specify
 *	maxcount that isn't large enough to hold the smallest possible
 *	XDR encoded dirent.
 *
 *	    sizeof cookie (8 bytes) +
 *	    sizeof name_len (4 bytes) +
 *	    sizeof smallest (padded) name (4 bytes) +
 *	    sizeof bitmap4_len (12 bytes) +   NOTE: we always encode len=2 bm4
 *	    sizeof attrlist4_len (4 bytes) +
 *	    sizeof next boolean (4 bytes)
 *
 * RFS4_MINLEN_RDDIR4: XDR-encoded size of READDIR op reply containing
 * the smallest possible entry4 (assumes no attrs requested).
 *	sizeof nfsstat4 (4 bytes) +
 *	sizeof verifier4 (8 bytes) +
 *	sizeof entry4list bool (4 bytes) +
 *	sizeof entry4 	(36 bytes) +
 *	sizeof eof bool  (4 bytes)
 *
 * RFS4_MINLEN_RDDIR_BUF: minimum length of buffer server will provide to
 *	VOP_READDIR.  Its value is the size of the maximum possible dirent
 *	for solaris.  The DIRENT64_RECLEN macro returns	the size of dirent
 *	required for a given name length.  MAXNAMELEN is the maximum
 *	filename length allowed in Solaris.  The first two DIRENT64_RECLEN()
 *	macros are to allow for . and .. entries -- just a minor tweak to try
 *	and guarantee that buffer we give to VOP_READDIR will be large enough
 *	to hold ., .., and the largest possible solaris dirent64.
 */
#define	RFS4_MINLEN_ENTRY4 36
#define	RFS4_MINLEN_RDDIR4 (4 + NFS4_VERIFIER_SIZE + 4 + RFS4_MINLEN_ENTRY4 + 4)
#define	RFS4_MINLEN_RDDIR_BUF \
	(DIRENT64_RECLEN(1) + DIRENT64_RECLEN(2) + DIRENT64_RECLEN(MAXNAMELEN))

/*
 * It would be better to pad to 4 bytes since that's what XDR would do,
 * but the dirents UFS gives us are already padded to 8, so just take
 * what we're given.  Dircount is only a hint anyway.  Currently the
 * solaris kernel is ASCII only, so there's no point in calling the
 * UTF8 functions.
 *
 * dirent64: named padded to provide 8 byte struct alignment
 *	d_ino(8) + d_off(8) + d_reclen(2) + d_name(namelen + null(1) + pad)
 *
 * cookie: uint64_t   +  utf8namelen: uint_t  +   utf8name padded to 8 bytes
 *
 */
#define	DIRENT64_TO_DIRCOUNT(dp) \
	(3 * BYTES_PER_XDR_UNIT + DIRENT64_NAMELEN((dp)->d_reclen))

/*
 * types of label comparison
 */
#define	EQUALITY_CHECK	0
#define	DOMINANCE_CHECK	1

static sysid_t lockt_sysid;		/* dummy sysid for all LOCKT calls */

void		rfs4_init_compound_state(struct compound_state *);

static void	nullfree(nfs_resop4 *, compound_state_t *);
static void	mds_op_inval(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_notsup(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_access(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_close(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_commit(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_create(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_create_free(nfs_resop4 *resop);
static void	mds_op_delegreturn(nfs_argop4 *, nfs_resop4 *,
				struct svc_req *, compound_state_t *);
static void	mds_op_getattr(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_getattr_free(nfs_resop4 *, compound_state_t *);
static void	mds_op_getfh(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_getfh_free(nfs_resop4 *, compound_state_t *);
static void	mds_op_link(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_lock(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_lock_denied_free(nfs_resop4 *, compound_state_t *);
static void	mds_op_locku(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_lockt(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_lookup(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_lookupp(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_openattr(nfs_argop4 *argop, nfs_resop4 *resop,
				struct svc_req *req, compound_state_t *);
static void	mds_op_nverify(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_open(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_open_downgrade(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_putfh(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_putpubfh(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_putrootfh(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_read(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_read_free(nfs_resop4 *, compound_state_t *);
void		mds_op_readdir(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_readdir_free(nfs_resop4 *, compound_state_t *);
static void	mds_op_readlink(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_readlink_free(nfs_resop4 *, compound_state_t *);
static void	mds_op_release_lockowner(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_remove(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_rename(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_renew(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_restorefh(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_savefh(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_setattr(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_verify(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_write(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_exchange_id(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_secinfo(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
static void	mds_op_secinfonn(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);
nfsstat4	do_rfs4_op_secinfo(struct compound_state *, char *, int,
    SECINFO4res *);

static void	mds_op_secinfo_free(nfs_resop4 *, compound_state_t *);

static void	mds_op_backchannel_ctl(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_bind_conn_to_session(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_create_clientid(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_create_session(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_destroy_session(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);
static void	mds_op_sequence(nfs_argop4 *, nfs_resop4 *,
			struct svc_req *, compound_state_t *);

static void mds_op_get_devlist(nfs_argop4 *, nfs_resop4 *,
		struct svc_req *, compound_state_t *);

static void mds_op_get_devinfo(nfs_argop4 *, nfs_resop4 *,
		struct svc_req *, compound_state_t *);

static void mds_op_layout_get(nfs_argop4 *, nfs_resop4 *,
		struct svc_req *, compound_state_t *);

static void mds_op_layout_commit(nfs_argop4 *, nfs_resop4 *,
		struct svc_req *, compound_state_t *);

static void mds_op_layout_return(nfs_argop4 *, nfs_resop4 *,
		struct svc_req *, compound_state_t *);

static void mds_op_reclaim_complete(nfs_argop4 *, nfs_resop4 *,
    struct svc_req *, compound_state_t *);

nfsstat4 check_open_access(uint32_t,
			struct compound_state *, struct svc_req *);
nfsstat4 rfs4_client_sysid(rfs4_client_t *, sysid_t *);
int	vop_shrlock(vnode_t *, int, struct shrlock *, int);
int 	rfs4_shrlock(rfs4_state_t *, int);
int	rfs4_share(rfs4_state_t *);

static void	mds_free_reply(nfs_resop4 *, compound_state_t *);

vnode_t *do_rfs4_op_mknod(CREATE4args *, CREATE4res *, struct svc_req *,
			struct compound_state *, vattr_t *, char *);

nfsstat4 rfs4_do_lock(rfs4_lo_state_t *, nfs_lock_type4, seqid4,
		offset4, length4, cred_t *, nfs_resop4 *);

rfs4_lo_state_t *mds_findlo_state_by_owner(rfs4_lockowner_t *,
	    rfs4_state_t *, bool_t *);

bool_t in_flavor_list(int, int *, int);

boolean_t do_rfs_label_check(bslabel_t *, vnode_t *, int);

nfsstat4 attrmap4_to_vattrmask(attrmap4 *, struct nfs4_svgetit_arg *);

nfsstat4 bitmap4_get_sysattrs(struct nfs4_svgetit_arg *);

nfsstat4 do_rfs4_op_getattr(attrmap4 *, fattr4 *, struct nfs4_svgetit_arg *);

nfsstat4 do_rfs4_op_lookup(char *, uint_t, struct svc_req *,
		struct compound_state *);

rfs4_lockowner_t *mds_findlockowner_by_pid(nfs_server_instance_t *, pid_t);

mds_session_t *mds_findsession_by_id(nfs_server_instance_t *, sessionid4);

rfs4_openowner_t *mds_findopenowner(nfs_server_instance_t *, open_owner4 *,
    bool_t *);

static void	mds_op_nverify(nfs_argop4 *, nfs_resop4 *, struct svc_req *,
			compound_state_t *);

extern mds_mpd_t *mds_find_mpd(nfs_server_instance_t *, uint32_t);
extern void rfs41_lo_seqid(stateid_t *);

nfsstat4
create_vnode(vnode_t *, char *,  vattr_t *, createmode4, timespec32_t *,
    cred_t *, vnode_t **, bool_t *);


/* HACKERY */
nfsstat4 rfs4_get_all_state(struct compound_state *, stateid4 *,
    rfs4_state_t **, rfs4_deleg_state_t **, rfs4_lo_state_t **);

void rfs4_ss_clid(struct compound_state *, rfs4_client_t *, struct svc_req *);
void rfs4_ss_chkclid(struct compound_state *, rfs4_client_t *);

int layout_match(stateid_t, stateid4, nfsstat4 *);

extern stateid4 special0;
extern stateid4 special1;

#define	ISSPECIAL(id)  (stateid4_cmp(id, &special0) || \
			stateid4_cmp(id, &special1))

void rfs4_cn_release(compound_state_t *);

mds_layout_grant_t *rfs41_findlogrant(struct compound_state *,
    rfs4_file_t *, rfs4_client_t *, bool_t *);
void rfs41_lo_grant_rele(mds_layout_grant_t *);
mds_ever_grant_t *rfs41_findevergrant(rfs4_client_t *, vnode_t *, bool_t *);
void rfs41_ever_grant_rele(mds_ever_grant_t *);

static uint32_t compute_use_pnfs_flags(uint32_t);

/* ARGSUSED */
static void
mds_op_notsup(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	DTRACE_NFSV4_1(op__notsup__start,
	    strcut compound_state *, cs);

	*cs->statusp = *((nfsstat4 *)&(resop)->nfs_resop4_u) = NFS4ERR_NOTSUPP;

	DTRACE_NFSV4_1(op__notsup__done,
	    struct compound_state *, cs);
}
/* ARGSUSED */
static void
mds_op_illegal(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	DTRACE_NFSV4_1(op__illegal__start,
	    struct compound_state *, cs);

	*cs->statusp =
	    *((nfsstat4 *)&(resop)->nfs_resop4_u) = NFS4ERR_OP_ILLEGAL;

	DTRACE_NFSV4_1(op__illegal__done,
	    struct compound_state *, cs);
}

/* ARGSUSED */
static void
mds_op_inval(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	DTRACE_NFSV4_1(op__inval__start,
	    struct compound_state *, cs);

	*cs->statusp = *((nfsstat4 *)&(resop)->nfs_resop4_u) = NFS4ERR_INVAL;

	DTRACE_NFSV4_1(op__inval__done,
	    struct compound_state *, cs);
}

/*ARGSUSED*/
static void
nullfree(nfs_resop4 *resop, compound_state_t *cs)
{
}

static op_disp_tbl_t mds_disptab[] = {
	{mds_op_illegal, nullfree, DISP_OP_BAD, "BAD Op 0"},
	{mds_op_illegal, nullfree, DISP_OP_BAD, "BAD Op 1"},
	{mds_op_illegal, nullfree, DISP_OP_BAD, "BAD Op 2"},
	{mds_op_access, nullfree, DISP_OP_MDS, "ACCESS"},
	{mds_op_close, nullfree, DISP_OP_MDS, "CLOSE"},
	{mds_op_commit, nullfree, DISP_OP_BOTH, "COMMIT"},
	{mds_op_create, nullfree, DISP_OP_MDS, "CREATE"},
	{mds_op_inval, nullfree, DISP_OP_BAD, "BAD Op 7"},
	{mds_op_delegreturn, nullfree, DISP_OP_MDS, "DELEGRETURN"},
	{mds_op_getattr, mds_op_getattr_free, DISP_OP_MDS, "GETATTR"},
	{mds_op_getfh, mds_op_getfh_free, DISP_OP_MDS, "GETFH"},
	{mds_op_link, nullfree, DISP_OP_MDS, "LINK"},
	{mds_op_lock, mds_lock_denied_free, DISP_OP_MDS, "LOCK"},
	{mds_op_lockt, mds_lock_denied_free,  DISP_OP_MDS, "LOCKT"},
	{mds_op_locku, nullfree,  DISP_OP_MDS, "LOCKU"},
	{mds_op_lookup, nullfree,  DISP_OP_MDS, "LOOKUP"},
	{mds_op_lookupp, nullfree,  DISP_OP_MDS, "LOOKUPP"},
	{mds_op_nverify, nullfree,  DISP_OP_MDS, "NVERIFY"},
	{mds_op_open, mds_free_reply,  DISP_OP_MDS, "OPEN"},
	{mds_op_openattr, nullfree,  DISP_OP_MDS, "OPENATTR"},
	{mds_op_notsup, nullfree,  DISP_OP_BAD, "BAD Op 20"},
	{mds_op_open_downgrade, nullfree,  DISP_OP_MDS, "OPEN_DOWNGRADE"},
	{mds_op_putfh, nullfree, DISP_OP_BOTH, "PUTFH"},
	{mds_op_putpubfh, nullfree,  DISP_OP_MDS, "PUTPUBFH"},
	{mds_op_putrootfh, nullfree,  DISP_OP_MDS, "PUTROOTFH"},
	{mds_op_read, mds_op_read_free, DISP_OP_BOTH, "READ"},
	{mds_op_readdir, mds_op_readdir_free,  DISP_OP_MDS, "READDIR"},
	{mds_op_readlink, mds_op_readlink_free,  DISP_OP_MDS, "READLINK"},
	{mds_op_remove, nullfree,  DISP_OP_MDS, "REMOVE"},
	{mds_op_rename, nullfree,  DISP_OP_MDS, "RENAME"},
	{mds_op_notsup, nullfree,  DISP_OP_BAD, "BAD Op 30"},
	{mds_op_restorefh, nullfree,  DISP_OP_MDS, "RESTOREFH"},
	{mds_op_savefh, nullfree,  DISP_OP_MDS, "SAVEFH"},
	{mds_op_secinfo, mds_op_secinfo_free,  DISP_OP_MDS, "SECINFO"},
	{mds_op_setattr, nullfree,  DISP_OP_MDS, "SETATTR"},
	{mds_op_notsup, nullfree,  DISP_OP_BAD, "BAD Op 35"},
	{mds_op_notsup, nullfree,  DISP_OP_BAD, "BAD Op 36"},
	{mds_op_verify, nullfree,  DISP_OP_MDS, "VERIFY"},
	{mds_op_write, nullfree, DISP_OP_BOTH, "WRITE"},
	{mds_op_notsup, nullfree,  DISP_OP_BAD, "BAD Op 39"},
	{mds_op_backchannel_ctl, nullfree,  DISP_OP_BOTH, "BACKCHANNEL_CTL"},
	{mds_op_bind_conn_to_session, nullfree,
	    DISP_OP_BOTH, "BIND_CONN_TO_SESS"},
	{mds_op_exchange_id, nullfree,  DISP_OP_BOTH, "EXCHANGE_ID"},
	{mds_op_create_session, nullfree,  DISP_OP_BOTH, "CREATE_SESS"},
	{mds_op_destroy_session, nullfree,  DISP_OP_BOTH, "DESTROY_SESS"},
	{mds_op_illegal, nullfree,  DISP_OP_MDS, "FREE_STATEID"},
	{mds_op_illegal, nullfree,  DISP_OP_MDS, "GET_DIR_DELEG"},
	{mds_op_get_devinfo, nullfree,  DISP_OP_MDS, "GET_DEVINFO"},
	{mds_op_get_devlist, nullfree,  DISP_OP_MDS, "GET_DEVLIST"},
	{mds_op_layout_commit, nullfree,  DISP_OP_MDS, "LAYOUT_COMMIT"},
	{mds_op_layout_get, nullfree,  DISP_OP_MDS, "LAYOUT_GET"},
	{mds_op_layout_return, nullfree,  DISP_OP_MDS, "LAYOUT_RETURN"},
	{mds_op_secinfonn, nullfree,
	    DISP_OP_BOTH, "SECINFO_NONAME"},
	{mds_op_sequence, nullfree,  DISP_OP_BOTH, "SEQUENCE"},
	{mds_op_notsup, nullfree,  DISP_OP_BOTH, "SET_SSV"},
	{mds_op_notsup, nullfree,  DISP_OP_MDS, "TEST_STATEID"},
	{mds_op_notsup, nullfree,  DISP_OP_MDS, "WANT_DELEG"},
	{mds_op_notsup, nullfree,  DISP_OP_BOTH, "DESTROY_CLIENTID"},
	{mds_op_reclaim_complete, nullfree,  DISP_OP_MDS, "RECLAIM_COMPLETE"}
};

static uint_t mds_disp_cnt = sizeof (mds_disptab) / sizeof (mds_disptab[0]);

#define	OP_ILLEGAL_IDX (mds_disp_cnt)

extern size_t strlcpy(char *dst, const char *src, size_t dstsize);

#ifdef	nextdp
#undef nextdp
#endif
#define	nextdp(dp)	((struct dirent64 *)((char *)(dp) + (dp)->d_reclen))

/*ARGSUSED*/
static void
mds_op_readdir_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function used for NFSv4.0 and NFSv4.1 */
	rfs4_op_readdir_free(resop);
}

/*ARGSUSED*/
static void
mds_op_secinfo_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function used for NFSv4.0 and NFSv4.1 */
	rfs4_op_secinfo_free(resop);
}

/*
 */
void
mds_srvrfini(void)
{
	/* some shutdown stuff for the minor verson 1 server */
}

nfsstat4	rfs4_state_has_access(rfs4_state_t *, int, vnode_t *);
int		rfs4_verify_attr(struct nfs4_svgetit_arg *, attrmap4 *,
		    struct nfs4_ntov_table *);


/*
 * Given the I/O mode (FREAD or FWRITE), the vnode, the stateid and whether
 * the file is being truncated, return NFS4_OK if allowed or approriate
 * V4 error if not. Note NFS4ERR_DELAY will be returned and a recall on
 * the associated file will be done if the I/O is not consistent with any
 * delegation in effect on the file. Should be holding VOP_RWLOCK, either
 * as reader or writer as appropriate. rfs4_op_open will accquire the
 * VOP_RWLOCK as writer when setting up delegation. If the stateid is bad
 * this routine will return NFS4ERR_BAD_STATEID. In addition, through the
 * deleg parameter, we will return whether a write delegation is held by
 * the client associated with this stateid.
 * If the server instance associated with the relevant client is in its
 * grace period, return NFS4ERR_GRACE.
 */
nfsstat4
mds_validate_stateid(int mode, struct compound_state *cs, vnode_t *vp,
    stateid4 *stateid, bool_t trunc, bool_t *deleg, bool_t do_access)
{
	rfs4_file_t *fp;
	bool_t create = FALSE;
	rfs4_state_t *sp;
	rfs4_deleg_state_t *dsp;
	rfs4_lo_state_t *lsp;
	stateid_t *id = (stateid_t *)stateid;
	nfsstat4 stat = NFS4_OK;

	if (ISSPECIAL(stateid)) {
		fp = rfs4_findfile(cs->instp, vp, NULL, &create);
		if (fp == NULL)
			return (NFS4_OK);
		if (fp->dinfo->dtype == OPEN_DELEGATE_NONE) {
			rfs4_file_rele(fp);
			return (NFS4_OK);
		}
		if (mode == FWRITE ||
		    fp->dinfo->dtype == OPEN_DELEGATE_WRITE) {
			rfs4_recall_deleg(fp, trunc, NULL);
			rfs4_file_rele(fp);
			return (NFS4ERR_DELAY);
		}
		rfs4_file_rele(fp);
		return (NFS4_OK);
	}

	stat = rfs4_get_all_state(cs, stateid, &sp, &dsp, &lsp);
	if (stat != NFS4_OK)
		return (stat);

	/*
	 * Ordering of the following 'if' statements is specific
	 * since rfs4_get_all_state() may return a value for sp and
	 * lsp. First we check lsp, then 'fall' through to sp.
	 */
	if (lsp != NULL) {
		/* Is associated server instance in its grace period? */
		if (rfs4_clnt_in_grace(lsp->locker->client)) {
			rfs4_lo_state_rele(lsp, FALSE);
			if (sp != NULL)
				rfs4_dbe_rele(sp->dbe);
			return (NFS4ERR_GRACE);
		}

		if (lsp->lockid.v41_bits.chgseq != 0) {
			/* Seqid in the future? - that's bad */
			if (lsp->lockid.v41_bits.chgseq <
			    id->v41_bits.chgseq) {
				rfs4_lo_state_rele(lsp, FALSE);
				if (sp != NULL)
					rfs4_dbe_rele(sp->dbe);
				return (NFS4ERR_BAD_STATEID);
			}
			/* Seqid in the past? - that's old */
			if (lsp->lockid.v41_bits.chgseq >
			    id->v41_bits.chgseq) {
				rfs4_lo_state_rele(lsp, FALSE);
				if (sp != NULL)
					rfs4_dbe_rele(sp->dbe);
				return (NFS4ERR_OLD_STATEID);
			}
		}

		/* Ensure specified filehandle matches */
		if (lsp->state->finfo->vp != vp) {
			rfs4_lo_state_rele(lsp, FALSE);
			if (sp != NULL)
				rfs4_dbe_rele(sp->dbe);
			return (NFS4ERR_BAD_STATEID);
		}
		rfs4_lo_state_rele(lsp, FALSE);
	}

	/*
	 * Stateid provided was an "open" or via the lock stateid
	 */
	if (sp != NULL) {
		/*
		 * only check if the passed in stateid was an OPENID,
		 * ie. Skip if we got here via the LOCKID.
		 */
		if (id->v41_bits.type == OPENID) {
			/* Is associated server instance in its grace period? */
			if (rfs4_clnt_in_grace(sp->owner->client)) {
				rfs4_dbe_rele(sp->dbe);
				return (NFS4ERR_GRACE);
			}

			if (sp->stateid.v41_bits.chgseq != 0) {
				/* Seqid in the future? - that's bad */
				if (sp->stateid.v41_bits.chgseq <
				    id->v41_bits.chgseq) {
					rfs4_dbe_rele(sp->dbe);
					return (NFS4ERR_BAD_STATEID);
				}
				/* Seqid in the past - that's old */
				if (sp->stateid.v41_bits.chgseq >
				    id->v41_bits.chgseq) {
					rfs4_dbe_rele(sp->dbe);
					return (NFS4ERR_OLD_STATEID);
				}
			}

			/* Ensure specified filehandle matches */
			if (sp->finfo->vp != vp) {
				rfs4_dbe_rele(sp->dbe);
				return (NFS4ERR_BAD_STATEID);
			}
		}
		if (sp->owner->need_confirm) {
			rfs4_dbe_rele(sp->dbe);
			return (NFS4ERR_BAD_STATEID);
		}

		if (sp->closed == TRUE) {
			rfs4_dbe_rele(sp->dbe);
			return (NFS4ERR_OLD_STATEID);
		}

		if (do_access)
			stat = rfs4_state_has_access(sp, mode, vp);
		else
			stat = NFS4_OK;

		/*
		 * Return whether this state has write
		 * delegation if desired
		 */
		if (deleg &&
		    (sp->finfo->dinfo->dtype == OPEN_DELEGATE_WRITE))
			*deleg = TRUE;

		/*
		 * We got a valid stateid, so we update the
		 * lease on the client. Ideally we would like
		 * to do this after the calling op succeeds,
		 * but for now this will be good
		 * enough. Callers of this routine are
		 * currently insulated from the state stuff.
		 */
		rfs4_update_lease(sp->owner->client);

		/*
		 * If a delegation is present on this file and
		 * this is a WRITE, then update the lastwrite
		 * time to indicate that activity is present.
		 */
		if (sp->finfo->dinfo->dtype == OPEN_DELEGATE_WRITE &&
		    mode == FWRITE) {
			sp->finfo->dinfo->time_lastwrite =
			    gethrestime_sec();
		}

		rfs4_dbe_rele(sp->dbe);
		return (stat);
	}

	if (dsp != NULL) {
		/* Is associated server instance in its grace period? */
		if (rfs4_clnt_in_grace(dsp->client)) {
			rfs4_deleg_state_rele(dsp);
			return (NFS4ERR_GRACE);
		}

		if ((dsp->delegid.v41_bits.chgseq != 0) &&
		    (dsp->delegid.v41_bits.chgseq != id->v41_bits.chgseq)) {
			rfs4_deleg_state_rele(dsp);
			return (NFS4ERR_BAD_STATEID);
		}

		/* Ensure specified filehandle matches */
		if (dsp->finfo->vp != vp) {
			rfs4_deleg_state_rele(dsp);
			return (NFS4ERR_BAD_STATEID);
		}
		/*
		 * Return whether this state has write
		 * delegation if desired
		 */
		if (deleg &&
		    (dsp->finfo->dinfo->dtype == OPEN_DELEGATE_WRITE))
			*deleg = TRUE;

		rfs4_update_lease(dsp->client);

		/*
		 * If a delegation is present on this file and
		 * this is a WRITE, then update the lastwrite
		 * time to indicate that activity is present.
		 */
		if (dsp->finfo->dinfo->dtype == OPEN_DELEGATE_WRITE &&
		    mode == FWRITE) {
			dsp->finfo->dinfo->time_lastwrite =
			    gethrestime_sec();
		}

		/*
		 * XXX - what happens if this is a WRITE and the
		 * delegation type of for READ.
		 */
		rfs4_deleg_state_rele(dsp);

		return (stat);
	}
	/*
	 * If we got this far, something bad happened
	 */
	return (NFS4ERR_BAD_STATEID);
}

nfsstat4
mds_setattr(attrmap4 *resp, fattr4 *fattrp, struct compound_state *cs,
    stateid4 *stateid)
{
	int error = 0;
	struct nfs4_svgetit_arg sarg;
	bool_t trunc;

	nfsstat4 status = NFS4_OK;
	cred_t *cr = cs->cr;
	vnode_t *vp = cs->vp;
	struct nfs4_ntov_table ntov;
	struct statvfs64 sb;
	struct vattr bva;
	struct flock64 bf;
	int in_crit = 0;
	uint_t saved_mask = 0;
	caller_context_t ct;
	attrvers_t avers;
	struct nfs4_ntov_map *nvmap;

	avers = RFS4_ATTRVERS(cs);
	nvmap = NFS4_NTOV_MAP(avers);
	*resp = NFS4_EMPTY_ATTRMAP(avers);
	sarg.sbp = &sb;
	nfs4_ntov_table_init(&ntov, avers);
	status = do_rfs4_set_attrs(resp, fattrp, cs, &sarg, &ntov,
	    NFS4ATTR_SETIT);
	if (status != NFS4_OK) {
		/*
		 * failed set attrs
		 */
		goto done;
	}

	if (sarg.vap->va_mask == 0 && ! ATTR_ISSET(fattrp->attrmask, ACL) &&
	    ! ATTR_ISSET(fattrp->attrmask, LAYOUT_HINT)) {
		/*
		 * no further work to be done
		 */
		goto done;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	/*
	 * If we got a request to set the ACL and the MODE, only
	 * allow changing VSUID, VSGID, and VSVTX.  Attempting
	 * to change any other bits, along with setting an ACL,
	 * gives NFS4ERR_INVAL.
	 */
	if (ATTR_ISSET(fattrp->attrmask, ACL) &&
	    ATTR_ISSET(fattrp->attrmask, MODE)) {
		vattr_t va;

		va.va_mask = AT_MODE;
		error = VOP_GETATTR(vp, &va, 0, cs->cr, &ct);
		if (error) {
			status = puterrno4(error);
			goto done;
		}
		if ((sarg.vap->va_mode ^ va.va_mode) &
		    ~(VSUID | VSGID | VSVTX)) {
			status = NFS4ERR_INVAL;
			goto done;
		}
	}

	/* Check stateid only if size has been set */
	if (sarg.vap->va_mask & AT_SIZE) {
		trunc = (sarg.vap->va_size == 0);
		status = mds_validate_stateid(FWRITE,
		    cs, cs->vp, stateid, trunc,
		    &cs->deleg, sarg.vap->va_mask & AT_SIZE);
		if (status != NFS4_OK)
			goto done;
	}

	/* XXX start of possible race with delegations */

	/*
	 * We need to specially handle size changes because it is
	 * possible for the client to create a file with read-only
	 * modes, but with the file opened for writing. If the client
	 * then tries to set the file size, e.g. ftruncate(3C),
	 * fcntl(F_FREESP), the normal access checking done in
	 * VOP_SETATTR would prevent the client from doing it even though
	 * it should be allowed to do so.  To get around this, we do the
	 * access checking for ourselves and use VOP_SPACE which doesn't
	 * do the access checking.
	 * Also the client should not be allowed to change the file
	 * size if there is a conflicting non-blocking mandatory lock in
	 * the region of the change.
	 */
	if (vp->v_type == VREG && (sarg.vap->va_mask & AT_SIZE)) {
		u_offset_t offset;
		ssize_t length;

		/*
		 * ufs_setattr clears AT_SIZE from vap->va_mask, but
		 * before returning, sarg.vap->va_mask is used to
		 * generate the setattr reply bitmap.  We also clear
		 * AT_SIZE below before calling VOP_SPACE.  For both
		 * of these cases, the va_mask needs to be saved here
		 * and restored after calling VOP_SETATTR.
		 */
		saved_mask = sarg.vap->va_mask;

		/*
		 * Check any possible conflict due to NBMAND locks.
		 * Get into critical region before VOP_GETATTR, so the
		 * size attribute is valid when checking conflicts.
		 */
		if (nbl_need_check(vp)) {
			nbl_start_crit(vp, RW_READER);
			in_crit = 1;
		}

		bva.va_mask = AT_UID|AT_SIZE;
		if (error = VOP_GETATTR(vp, &bva, 0, cr, &ct)) {
			status = puterrno4(error);
			goto done;
		}

		if (in_crit) {
			if (sarg.vap->va_size < bva.va_size) {
				offset = sarg.vap->va_size;
				length = bva.va_size - sarg.vap->va_size;
			} else {
				offset = bva.va_size;
				length = sarg.vap->va_size - bva.va_size;
			}
			if (nbl_conflict(vp, NBL_WRITE, offset, length, 0,
			    &ct)) {
				status = NFS4ERR_LOCKED;
				goto done;
			}
		}

		if (crgetuid(cr) == bva.va_uid) {
			sarg.vap->va_mask &= ~AT_SIZE;
			bf.l_type = F_WRLCK;
			bf.l_whence = 0;
			bf.l_start = (off64_t)sarg.vap->va_size;
			bf.l_len = 0;
			bf.l_sysid = 0;
			bf.l_pid = 0;
			error = VOP_SPACE(vp, F_FREESP, &bf, FWRITE,
			    (offset_t)sarg.vap->va_size, cr, &ct);
		}
	}

	if (!error && sarg.vap->va_mask != 0)
		error = VOP_SETATTR(vp, sarg.vap, sarg.flag, cr, &ct);

	/* restore va_mask -- ufs_setattr clears AT_SIZE */
	if (saved_mask & AT_SIZE)
		sarg.vap->va_mask |= AT_SIZE;

	/*
	 * If an ACL was being set, it has been delayed until now,
	 * in order to set the mode (via the VOP_SETATTR() above) first.
	 */
	if (! error && ATTR_ISSET(fattrp->attrmask, ACL)) {
		int i;

		for (i = 0; i < ntov.attrcnt; i++)
			if (ntov.amap[i] == FATTR4_ACL)
				break;
		if (i < ntov.attrcnt) {
			error = (*nvmap[FATTR4_ACL].sv_getit)(NFS4ATTR_SETIT,
			    &sarg, &ntov.na[i]);
			if (error == 0) {
				ATTR_SET(*resp, ACL);
			} else if (error == ENOTSUP) {
				(void) rfs4_verify_attr(&sarg, resp, &ntov);
				status = NFS4ERR_ATTRNOTSUPP;
				goto done;
			}
		} else {
			error = EINVAL;
		}
	}

	if (! error && ATTR_ISSET(fattrp->attrmask, LAYOUT_HINT)) {
		/*
		 * Store layout hint.  Layout hint will be stored
		 * in file struct (which means it can only be set
		 * when the file is open).  If layout hint is allowed
		 * for files not open, then it must be stored
		 * persistently.
		 *
		 * status assignment placates lint.  it will
		 * be replaced with code to store the layout
		 * hint.
		 */
		status = NFS4_OK;
	}

	if (error) {
		/* check if a monitor detected a delegation conflict */
		if (error == EAGAIN && (ct.cc_flags & CC_WOULDBLOCK))
			status = NFS4ERR_DELAY;
		else
			status = puterrno4(error);

		/*
		 * Set the response bitmap when setattr failed.
		 * If VOP_SETATTR partially succeeded, test by doing a
		 * VOP_GETATTR on the object and comparing the data
		 * to the setattr arguments.
		 */
		(void) rfs4_verify_attr(&sarg, resp, &ntov);
	} else {
		/*
		 * Force modified metadata out to stable storage.
		 */
		(void) VOP_FSYNC(vp, FNODSYNC, cr, &ct);
		/*
		 * Set response bitmap
		 */
		nfs4_vmask_to_nmask_set(sarg.vap->va_mask, resp);
	}

	/* Return early and already have a NFSv4 error */
done:
	/*
	 * Except for nfs4_vmask_to_nmask_set(), vattr --> fattr
	 * conversion sets both readable and writeable NFS4 attrs
	 * for AT_MTIME and AT_ATIME.  The line below masks out
	 * unrequested attrs from the setattr result bitmap.  This
	 * is placed after the done: label to catch the ATTRNOTSUP
	 * case.
	 */
	ATTRMAP_MASK(*resp, fattrp->attrmask);

	if (in_crit)
		nbl_end_crit(vp);

	nfs4_ntov_table_free(&ntov, &sarg);

	return (status);
}

/* ARGSUSED */
void
mds_op_secinfonn(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
    compound_state_t *cs)
{
	SECINFO_NO_NAME4res *respnn;
	int dotdot;

	DTRACE_NFSV4_1(op__secinfo__no__name__start,
	    struct compound_state *, cs);

	respnn = &resop->nfs_resop4_u.opsecinfo_no_name;

	/*
	 * Current file handle (cfh) should have been set before
	 * getting into this function. If not, return error.
	 */
	if (cs->vp == NULL) {
		*cs->statusp = respnn->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	dotdot =
	    (argop->nfs_argop4_u.opsecinfo_no_name == SECINFO_STYLE4_PARENT);

	*cs->statusp = respnn->status = do_rfs4_op_secinfo(cs, NULL,
	    dotdot, (SECINFO4res *)respnn);

final:
	DTRACE_NFSV4_2(op__secinfo__no__name__done,
	    struct compound_state *, cs,
	    SECINFO_NO_NAME4res *, respnn);
}

/* ARGSUSED */
void
mds_op_secinfo(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
    compound_state_t *cs)
{
	SECINFO4res *resp;
	utf8string *utfnm;
	uint_t len, dotdot;
	char *nm;

	SECINFO4args *args = &argop->nfs_argop4_u.opsecinfo;

	DTRACE_NFSV4_2(op__secinfo__start, struct compound_state *, cs,
	    SECINFO4args *, args);

	resp = &resop->nfs_resop4_u.opsecinfo;

	/*
	 * Current file handle (cfh) should have been set before
	 * getting into this function. If not, return error.
	 */
	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}
	if (cs->vp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}

	/*
	 * Verify the component name. If failed, error out, but
	 * do not error out if the component name is a "..".
	 * SECINFO will return its parents secinfo data for SECINFO "..".
	 */
	utfnm = &argop->nfs_argop4_u.opsecinfo.name;
	if (!utf8_dir_verify(utfnm)) {
		if (utfnm->utf8string_len != 2 ||
		    utfnm->utf8string_val[0] != '.' ||
		    utfnm->utf8string_val[1] != '.') {
			*cs->statusp = resp->status = NFS4ERR_INVAL;
			goto final;
		}
		dotdot = 1;
	} else
		dotdot = 0;

	nm = utf8_to_str(utfnm, &len, NULL);
	if (nm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (len > MAXNAMELEN) {
		*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
		kmem_free(nm, len);
		goto final;
	}

	*cs->statusp = resp->status = do_rfs4_op_secinfo(cs, nm, dotdot, resp);

	kmem_free(nm, len);

final:
	DTRACE_NFSV4_2(op__secinfo__done, struct compound_state *, cs,
	    SECINFO4res *, resp);
}

/*
 * verify and nverify are exactly the same, except that nverify
 * succeeds when some argument changed, and verify succeeds when
 * when none changed.
 */

/* ARGSUSED */
void
mds_op_verify(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
    compound_state_t *cs)
{
	VERIFY4args  *args = &argop->nfs_argop4_u.opverify;
	VERIFY4res *resp = &resop->nfs_resop4_u.opverify;
	int error;
	struct nfs4_svgetit_arg sarg;
	struct statvfs64 sb;
	struct nfs4_ntov_table ntov;

	DTRACE_NFSV4_2(op__verify__start, struct compound_state *, cs,
	    VERIFY4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	sarg.sbp = &sb;
	nfs4_ntov_table_init(&ntov, RFS4_ATTRVERS(cs));
	resp->status = do_rfs4_set_attrs(NULL, &args->obj_attributes, cs,
	    &sarg, &ntov, NFS4ATTR_VERIT);
	if (resp->status != NFS4_OK) {
		/*
		 * do_rfs4_set_attrs will try to verify systemwide attrs,
		 * so could return -1 for "no match".
		 */
		if (resp->status == -1)
			resp->status = NFS4ERR_NOT_SAME;
		goto done;
	}
	error = rfs4_verify_attr(&sarg, NULL, &ntov);
	switch (error) {
	case 0:
		resp->status = NFS4_OK;
		break;
	case -1:
		resp->status = NFS4ERR_NOT_SAME;
		break;
	default:
		resp->status = puterrno4(error);
		break;
	}
done:
	*cs->statusp = resp->status;
	nfs4_ntov_table_free(&ntov, &sarg);

final:
	DTRACE_NFSV4_2(op__verify__done, struct compound_state *, cs,
	    VERIFY4res *, resp);
}

/* ARGSUSED */
void
mds_op_nverify(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
    compound_state_t *cs)
{
	NVERIFY4args  *args = &argop->nfs_argop4_u.opnverify;
	NVERIFY4res *resp = &resop->nfs_resop4_u.opnverify;
	int error;
	struct nfs4_svgetit_arg sarg;
	struct statvfs64 sb;
	struct nfs4_ntov_table ntov;

	DTRACE_NFSV4_2(op__nverify__start, struct compound_state *, cs,
	    NVERIFY4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}
	sarg.sbp = &sb;
	nfs4_ntov_table_init(&ntov, RFS4_ATTRVERS(cs));
	resp->status = do_rfs4_set_attrs(NULL, &args->obj_attributes, cs,
	    &sarg, &ntov, NFS4ATTR_VERIT);
	if (resp->status != NFS4_OK) {
		/*
		 * do_rfs4_set_attrs will try to verify systemwide attrs,
		 * so could return -1 for "no match".
		 */
		if (resp->status == -1)
			resp->status = NFS4_OK;
		goto done;
	}
	error = rfs4_verify_attr(&sarg, NULL, &ntov);
	switch (error) {
	case 0:
		resp->status = NFS4ERR_SAME;
		break;
	case -1:
		resp->status = NFS4_OK;
		break;
	default:
		resp->status = puterrno4(error);
		break;
	}
done:
	*cs->statusp = resp->status;
	nfs4_ntov_table_free(&ntov, &sarg);

final:
	DTRACE_NFSV4_2(op__nverify__done, struct compound_state *, cs,
	    NVERIFY4res *, resp);

}

/* ARGSUSED */
void
mds_op_access(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
    compound_state_t *cs)
{
	ACCESS4args *args = &argop->nfs_argop4_u.opaccess;
	ACCESS4res *resp = &resop->nfs_resop4_u.opaccess;
	int error;
	vnode_t *vp;
	struct vattr va;
	int checkwriteperm;
	cred_t *cr = cs->cr;
	bslabel_t *clabel, *slabel;
	ts_label_t *tslabel;
	boolean_t admin_low_client;

	DTRACE_NFSV4_2(op__access__start, struct compound_state *, cs,
	    ACCESS4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	ASSERT(cr != NULL);

	vp = cs->vp;

	/*
	 * If the file system is exported read only, it is not appropriate
	 * to check write permissions for regular files and directories.
	 * Special files are interpreted by the client, so the underlying
	 * permissions are sent back to the client for interpretation.
	 */
	if (rdonly4(cs->exi, cs->vp, req) &&
	    (vp->v_type == VREG || vp->v_type == VDIR))
		checkwriteperm = 0;
	else
		checkwriteperm = 1;

	/*
	 * XXX
	 * We need the mode so that we can correctly determine access
	 * permissions relative to a mandatory lock file.  Access to
	 * mandatory lock files is denied on the server, so it might
	 * as well be reflected to the server during the open.
	 */
	va.va_mask = AT_MODE;
	error = VOP_GETATTR(vp, &va, 0, cr, NULL);
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}
	resp->access = 0;
	resp->supported = 0;

	if (is_system_labeled()) {
		ASSERT(req->rq_label != NULL);
		clabel = req->rq_label;
		DTRACE_PROBE2(tx__rfs4__log__info__opaccess__clabel, char *,
		    "got client label from request(1)",
		    struct svc_req *, req);
		if (!blequal(&l_admin_low->tsl_label, clabel)) {
			if ((tslabel = nfs_getflabel(vp)) == NULL) {
				*cs->statusp = resp->status = puterrno4(EACCES);
				goto final;
			}
			slabel = label2bslabel(tslabel);
			DTRACE_PROBE3(tx__rfs4__log__info__opaccess__slabel,
			    char *, "got server label(1) for vp(2)",
			    bslabel_t *, slabel, vnode_t *, vp);

			admin_low_client = B_FALSE;
		} else
			admin_low_client = B_TRUE;
	}

	if (args->access & ACCESS4_READ) {
		error = VOP_ACCESS(vp, VREAD, 0, cr, NULL);
		if (!error && !MANDLOCK(vp, va.va_mode) &&
		    (!is_system_labeled() || admin_low_client ||
		    bldominates(clabel, slabel)))
			resp->access |= ACCESS4_READ;
		resp->supported |= ACCESS4_READ;
	}
	if ((args->access & ACCESS4_LOOKUP) && vp->v_type == VDIR) {
		error = VOP_ACCESS(vp, VEXEC, 0, cr, NULL);
		if (!error && (!is_system_labeled() || admin_low_client ||
		    bldominates(clabel, slabel)))
			resp->access |= ACCESS4_LOOKUP;
		resp->supported |= ACCESS4_LOOKUP;
	}
	if (checkwriteperm &&
	    (args->access & (ACCESS4_MODIFY|ACCESS4_EXTEND))) {
		error = VOP_ACCESS(vp, VWRITE, 0, cr, NULL);
		if (!error && !MANDLOCK(vp, va.va_mode) &&
		    (!is_system_labeled() || admin_low_client ||
		    blequal(clabel, slabel)))
			resp->access |=
			    (args->access & (ACCESS4_MODIFY|ACCESS4_EXTEND));
		resp->supported |= (ACCESS4_MODIFY|ACCESS4_EXTEND);
	}

	if (checkwriteperm &&
	    (args->access & ACCESS4_DELETE) && vp->v_type == VDIR) {
		error = VOP_ACCESS(vp, VWRITE, 0, cr, NULL);
		if (!error && (!is_system_labeled() || admin_low_client ||
		    blequal(clabel, slabel)))
			resp->access |= ACCESS4_DELETE;
		resp->supported |= ACCESS4_DELETE;
	}
	if (args->access & ACCESS4_EXECUTE && vp->v_type != VDIR) {
		error = VOP_ACCESS(vp, VEXEC, 0, cr, NULL);
		if (!error && !MANDLOCK(vp, va.va_mode) &&
		    (!is_system_labeled() || admin_low_client ||
		    bldominates(clabel, slabel)))
			resp->access |= ACCESS4_EXECUTE;
		resp->supported |= ACCESS4_EXECUTE;
	}

	if (is_system_labeled() && !admin_low_client)
		label_rele(tslabel);

	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__access__done, struct compound_state *, cs,
	    ACCESS4res *, resp);
}

/* ARGSUSED */
static void
mds_op_commit(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	COMMIT4args *args = &argop->nfs_argop4_u.opcommit;
	COMMIT4res *resp = &resop->nfs_resop4_u.opcommit;
	int error;
	vnode_t *vp = cs->vp;
	cred_t *cr = cs->cr;
	vattr_t va;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__commit__start, struct compound_state *, cs,
	    COMMIT4args *, args);

	if (vp == NULL) {
		/*
		 * XXX kludge: fake the commit if we are a data server
		 * This will be replaced once we have nnop_commit().
		 */
		if (cs->nn != NULL) {
			*cs->statusp = resp->status = NFS4_OK;
			resp->writeverf = cs->instp->Write4verf;
		} else {
			*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		}
		goto final;
	}
	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	if (args->offset + args->count < args->offset) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	va.va_mask = AT_UID;
	error = VOP_GETATTR(vp, &va, 0, cr, &ct);

	/*
	 * If we can't get the attributes, then we can't do the
	 * right access checking.  So, we'll fail the request.
	 */
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}
	if (rdonly4(cs->exi, cs->vp, req)) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		goto final;
	}

	if (vp->v_type != VREG) {
		if (vp->v_type == VDIR)
			resp->status = NFS4ERR_ISDIR;
		else
			resp->status = NFS4ERR_INVAL;
		*cs->statusp = resp->status;
		goto final;
	}

	if (crgetuid(cr) != va.va_uid &&
	    (error = VOP_ACCESS(vp, VWRITE, 0, cs->cr, &ct))) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	error = VOP_PUTPAGE(vp, args->offset, args->count, 0, cr, &ct);
	if (!error)
		error = VOP_FSYNC(vp, FNODSYNC, cr, &ct);

	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	*cs->statusp = resp->status = NFS4_OK;
	resp->writeverf = cs->instp->Write4verf;

final:
	DTRACE_NFSV4_2(op__commit__done, struct compound_state *, cs,
	    COMMIT4res *, resp);
}

/*
 * rfs4_op_mknod is called from rfs4_op_create after all initial verification
 * was completed. It does the nfsv4 create for special files.
 *
 * nfsv4 create is used to create non-regular files. For regular files,
 * use nfsv4 open.
 */
/* ARGSUSED */
static void
mds_op_create(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	CREATE4args *args = &argop->nfs_argop4_u.opcreate;
	CREATE4res *resp = &resop->nfs_resop4_u.opcreate;
	int error;
	struct vattr bva, iva, iva2, ava, *vap;
	cred_t *cr = cs->cr;
	vnode_t *dvp = cs->vp;
	vnode_t *vp = NULL;
	char *nm, *lnm;
	uint_t len, llen;
	int syncval = 0;
	struct nfs4_svgetit_arg sarg;
	struct nfs4_ntov_table ntov;
	struct statvfs64 sb;
	nfsstat4 status;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__create__start, struct compound_state *, cs,
	    CREATE4args *, args);

	resp->attrset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));

	if (dvp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/*
	 * If there is an unshared filesystem mounted on this vnode,
	 * do not allow to create an object in this directory.
	 */
	if (vn_ismntpt(dvp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	/* Verify that type is correct */
	switch (args->type) {
	case NF4LNK:
	case NF4BLK:
	case NF4CHR:
	case NF4SOCK:
	case NF4FIFO:
	case NF4DIR:
		break;
	default:
		*cs->statusp = resp->status = NFS4ERR_BADTYPE;
		goto final;
	};

	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}
	if (dvp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}
	if (!utf8_dir_verify(&args->objname)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (rdonly4(cs->exi, cs->vp, req)) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		goto final;
	}

	/*
	 * Name of newly created object
	 */
	nm = utf8_to_fn(&args->objname, &len, NULL);
	if (nm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (len > MAXNAMELEN) {
		*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
		kmem_free(nm, len);
		goto final;
	}

	sarg.sbp = &sb;
	nfs4_ntov_table_init(&ntov, RFS4_ATTRVERS(cs));

	status = do_rfs4_set_attrs(&resp->attrset,
	    &args->createattrs, cs, &sarg, &ntov, NFS4ATTR_SETIT);

	if (sarg.vap->va_mask == 0 && status == NFS4_OK)
		status = NFS4ERR_INVAL;

	if (status != NFS4_OK) {
		*cs->statusp = resp->status = status;
		kmem_free(nm, len);
		nfs4_ntov_table_free(&ntov, &sarg);

		resp->attrset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
		goto final;
	}

	/* Get "before" change value */
	bva.va_mask = AT_CTIME|AT_SEQ;
	error = VOP_GETATTR(dvp, &bva, 0, cr, &ct);
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		kmem_free(nm, len);
		nfs4_ntov_table_free(&ntov, &sarg);

		resp->attrset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
		goto final;
	}
	NFS4_SET_FATTR4_CHANGE(resp->cinfo.before, bva.va_ctime)

	vap = sarg.vap;

	/*
	 * Set default initial values for attributes when not specified
	 * in createattrs.
	 */
	if ((vap->va_mask & AT_UID) == 0) {
		vap->va_uid = crgetuid(cr);
		vap->va_mask |= AT_UID;
	}
	if ((vap->va_mask & AT_GID) == 0) {
		vap->va_gid = crgetgid(cr);
		vap->va_mask |= AT_GID;
	}

	vap->va_mask |= AT_TYPE;
	switch (args->type) {
	case NF4DIR:
		vap->va_type = VDIR;
		if ((vap->va_mask & AT_MODE) == 0) {
			vap->va_mode = 0700;	/* default: owner rwx only */
			vap->va_mask |= AT_MODE;
		}
		error = VOP_MKDIR(dvp, nm, vap, &vp, cr, &ct, 0, NULL);
		if (error)
			break;

		/*
		 * Get the initial "after" sequence number, if it fails,
		 * set to zero
		 */
		iva.va_mask = AT_SEQ;
		if (VOP_GETATTR(dvp, &iva, 0, cs->cr, &ct))
			iva.va_seq = 0;
		break;
	case NF4LNK:
		vap->va_type = VLNK;
		if ((vap->va_mask & AT_MODE) == 0) {
			vap->va_mode = 0700;	/* default: owner rwx only */
			vap->va_mask |= AT_MODE;
		}

		/*
		 * symlink names must be treated as data
		 */
		lnm = utf8_to_str(&args->ftype4_u.linkdata, &llen, NULL);

		if (lnm == NULL) {
			*cs->statusp = resp->status = NFS4ERR_INVAL;
			kmem_free(nm, len);
			nfs4_ntov_table_free(&ntov, &sarg);
			resp->attrset =
			    NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
			goto final;
		}

		if (llen > MAXPATHLEN) {
			*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
			kmem_free(nm, len);
			kmem_free(lnm, llen);
			nfs4_ntov_table_free(&ntov, &sarg);
			resp->attrset =
			    NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
			goto final;
		}

		error = VOP_SYMLINK(dvp, nm, vap, lnm, cr, &ct, 0);
		if (lnm != NULL)
			kmem_free(lnm, llen);
		if (error)
			break;

		/*
		 * Get the initial "after" sequence number, if it fails,
		 * set to zero
		 */
		iva.va_mask = AT_SEQ;
		if (VOP_GETATTR(dvp, &iva, 0, cs->cr, &ct))
			iva.va_seq = 0;

		error = VOP_LOOKUP(dvp, nm, &vp, NULL, 0, NULL, cr,
		    &ct, 0, NULL);
		if (error)
			break;

		/*
		 * va_seq is not safe over VOP calls, check it again
		 * if it has changed zero out iva to force atomic = FALSE.
		 */
		iva2.va_mask = AT_SEQ;
		if (VOP_GETATTR(dvp, &iva2, 0, cs->cr, &ct) ||
		    iva2.va_seq != iva.va_seq)
			iva.va_seq = 0;
		break;
	default:
		/*
		 * probably a special file.
		 */
		if ((vap->va_mask & AT_MODE) == 0) {
			vap->va_mode = 0600;	/* default: owner rw only */
			vap->va_mask |= AT_MODE;
		}
		syncval = FNODSYNC;
		/*
		 * We know this will only generate one VOP call
		 */
		vp = do_rfs4_op_mknod(args, resp, req, cs, vap, nm);

		if (vp == NULL) {
			kmem_free(nm, len);
			nfs4_ntov_table_free(&ntov, &sarg);
			resp->attrset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
			goto final;
		}

		/*
		 * Get the initial "after" sequence number, if it fails,
		 * set to zero
		 */
		iva.va_mask = AT_SEQ;
		if (VOP_GETATTR(dvp, &iva, 0, cs->cr, &ct))
			iva.va_seq = 0;

		break;
	}
	kmem_free(nm, len);

	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
	}

	/*
	 * Force modified data and metadata out to stable storage.
	 */
	(void) VOP_FSYNC(dvp, 0, cr, &ct);

	if (resp->status != NFS4_OK) {
		if (vp != NULL)
			VN_RELE(vp);
		nfs4_ntov_table_free(&ntov, &sarg);
		resp->attrset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
		goto final;
	}

	/*
	 * Finish setup of cinfo response, "before" value already set.
	 * Get "after" change value, if it fails, simply return the
	 * before value.
	 */
	ava.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(dvp, &ava, 0, cr, &ct)) {
		ava.va_ctime = bva.va_ctime;
		ava.va_seq = 0;
	}
	NFS4_SET_FATTR4_CHANGE(resp->cinfo.after, ava.va_ctime);

	/*
	 * True verification that object was created with correct
	 * attrs is impossible.  The attrs could have been changed
	 * immediately after object creation.  If attributes did
	 * not verify, the only recourse for the server is to
	 * destroy the object.  Maybe if some attrs (like gid)
	 * are set incorrectly, the object should be destroyed;
	 * however, seems bad as a default policy.  Do we really
	 * want to destroy an object over one of the times not
	 * verifying correctly?  For these reasons, the server
	 * currently sets bits in attrset for createattrs
	 * that were set; however, no verification is done.
	 *
	 * vmask_to_nmask accounts for vattr bits set on create
	 *	[do_rfs4_set_attrs() only sets resp bits for
	 *	 non-vattr/vfs bits.]
	 * Mask off any bits set by default so as not to return
	 * more attrset bits than were requested in createattrs
	 */
	nfs4_vmask_to_nmask(sarg.vap->va_mask, &resp->attrset,
	    RFS4_ATTRVERS(cs));
	ATTRMAP_MASK(resp->attrset, args->createattrs.attrmask);
	nfs4_ntov_table_free(&ntov, &sarg);

	error = mknfs41_fh(&cs->fh, vp, cs->exi);
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
	}

	/*
	 * The cinfo.atomic = TRUE only if we got no errors, we have
	 * non-zero va_seq's, and it has incremented by exactly one
	 * during the creation and it didn't change during the VOP_LOOKUP
	 * or VOP_FSYNC.
	 */
	if (!error && bva.va_seq && iva.va_seq && ava.va_seq &&
	    iva.va_seq == (bva.va_seq + 1) &&
	    iva.va_seq == ava.va_seq)
		resp->cinfo.atomic = TRUE;
	else
		resp->cinfo.atomic = FALSE;

	(void) VOP_FSYNC(vp, syncval, cr, &ct);

	if (resp->status != NFS4_OK) {
		VN_RELE(vp);
		goto final;
	}
	if (cs->vp)
		VN_RELE(cs->vp);

	cs->vp = vp;
	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__create__done, struct compound_state *, cs,
	    CREATE4res *, resp);
}


/*ARGSUSED*/
static void
mds_op_delegreturn(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	DELEGRETURN4args *args = &argop->nfs_argop4_u.opdelegreturn;
	DELEGRETURN4res *resp = &resop->nfs_resop4_u.opdelegreturn;
	rfs4_deleg_state_t *dsp;
	nfsstat4 status;

	DTRACE_NFSV4_2(op__delegreturn__start, struct compound_state *, cs,
	    DELEGRETURN4args *, args);

	status = rfs4_get_deleg_state(cs, &args->deleg_stateid, &dsp);
	resp->status = *cs->statusp = status;
	if (status != NFS4_OK)
		goto final;

	/* Ensure specified filehandle matches */
	if (cs->vp != dsp->finfo->vp) {
		resp->status = *cs->statusp = NFS4ERR_BAD_STATEID;
	} else
		rfs4_return_deleg(dsp, FALSE);

	rfs4_update_lease(dsp->client);

	rfs4_deleg_state_rele(dsp);

final:
	DTRACE_NFSV4_2(op__delegreturn__done, struct compound_state *, cs,
	    DELEGRETURN4res *, resp);
}



/* ARGSUSED */
static void
mds_op_getattr(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	GETATTR4args *args = &argop->nfs_argop4_u.opgetattr;
	GETATTR4res *resp = &resop->nfs_resop4_u.opgetattr;
	struct nfs4_svgetit_arg sarg;
	struct statvfs64 sb;
	nfsstat4 status;

	DTRACE_NFSV4_2(op__getattr__start, struct compound_state *, cs,
	    GETATTR4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	sarg.sbp = &sb;
	sarg.cs = cs;

	status = attrmap4_to_vattrmask(&args->attr_request, &sarg);
	if (status == NFS4_OK) {
		status = bitmap4_get_sysattrs(&sarg);
		if (status == NFS4_OK)
			status = do_rfs4_op_getattr(&args->attr_request,
			    &resp->obj_attributes, &sarg);
	}
	*cs->statusp = resp->status = status;

final:
	DTRACE_NFSV4_2(op__getattr__done, struct compound_state *, cs,
	    GETATTR4res *, resp);
}

/*ARGSUSED*/
void
mds_op_getattr_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function for NFSv4.0 and NFSv4.1 */
	rfs4_op_getattr_free(resop);
}

/* ARGSUSED */
static void
mds_op_getfh(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	GETFH4res *resp = &resop->nfs_resop4_u.opgetfh;

	DTRACE_NFSV4_1(op__getfh__start,
	    struct compound_state *, cs);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}
	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	resp->object.nfs_fh4_val =
	    kmem_alloc(cs->fh.nfs_fh4_len, KM_SLEEP);
	nfs_fh4_copy(&cs->fh, &resp->object);
	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__getfh__done, struct compound_state *, cs,
	    GETFH4res *, resp);
}

/*ARGSUSED*/
static void
mds_op_getfh_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function for NFSv4.0 and NFSv4.1 */
	rfs4_op_getfh_free(resop);
}

/*
 * link: args: SAVED_FH: file, CURRENT_FH: target directory
 *	 res: status. If success - CURRENT_FH unchanged, return change_info
 */
/* ARGSUSED */
static void
mds_op_link(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	LINK4args *args = &argop->nfs_argop4_u.oplink;
	LINK4res *resp = &resop->nfs_resop4_u.oplink;
	int error;
	vnode_t *vp;
	vnode_t *dvp;
	struct vattr bdva, idva, adva;
	char *nm;
	uint_t  len;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__link__start, struct compound_state *, cs,
	    LINK4args *, args);

	/* SAVED_FH: source object */
	vp = cs->saved_vp;
	if (vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/* CURRENT_FH: target directory */
	dvp = cs->vp;
	if (dvp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/*
	 * If there is a non-shared filesystem mounted on this vnode,
	 * do not allow to link any file in this directory.
	 */
	if (vn_ismntpt(dvp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	/* Check source object's type validity */
	if (vp->v_type == VDIR) {
		*cs->statusp = resp->status = NFS4ERR_ISDIR;
		goto final;
	}

	/* Check target directory's type */
	if (dvp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}

	if (cs->saved_exi != cs->exi) {
		*cs->statusp = resp->status = NFS4ERR_XDEV;
		goto final;
	}

	if (!utf8_dir_verify(&args->newname)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	nm = utf8_to_fn(&args->newname, &len, NULL);
	if (nm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (len > MAXNAMELEN) {
		*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
		kmem_free(nm, len);
		goto final;
	}

	if (rdonly4(cs->exi, cs->vp, req)) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		kmem_free(nm, len);
		goto final;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	/* Get "before" change value */
	bdva.va_mask = AT_CTIME|AT_SEQ;
	error = VOP_GETATTR(dvp, &bdva, 0, cs->cr, &ct);
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		kmem_free(nm, len);
		goto final;
	}

	NFS4_SET_FATTR4_CHANGE(resp->cinfo.before, bdva.va_ctime)

	error = VOP_LINK(dvp, vp, nm, cs->cr, &ct, 0);

	kmem_free(nm, len);

	/*
	 * Get the initial "after" sequence number, if it fails, set to zero
	 */
	idva.va_mask = AT_SEQ;
	if (VOP_GETATTR(dvp, &idva, 0, cs->cr, &ct))
		idva.va_seq = 0;

	/*
	 * Force modified data and metadata out to stable storage.
	 */
	(void) VOP_FSYNC(vp, FNODSYNC, cs->cr, &ct);
	(void) VOP_FSYNC(dvp, 0, cs->cr, &ct);

	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	/*
	 * Get "after" change value, if it fails, simply return the
	 * before value.
	 */
	adva.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(dvp, &adva, 0, cs->cr, &ct)) {
		adva.va_ctime = bdva.va_ctime;
		adva.va_seq = 0;
	}

	NFS4_SET_FATTR4_CHANGE(resp->cinfo.after, adva.va_ctime)

	/*
	 * The cinfo.atomic = TRUE only if we have
	 * non-zero va_seq's, and it has incremented by exactly one
	 * during the VOP_LINK and it didn't change during the VOP_FSYNC.
	 */
	if (bdva.va_seq && idva.va_seq && adva.va_seq &&
	    idva.va_seq == (bdva.va_seq + 1) &&
	    idva.va_seq == adva.va_seq)
		resp->cinfo.atomic = TRUE;
	else
		resp->cinfo.atomic = FALSE;

	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__link__done, struct compound_state *, cs,
	    LINK4res *, resp);
}

/*
 * Used by mds_op_lookup and mds_op_lookupp to do the actual work.
 */

/* ARGSUSED */
static nfsstat4
mds_do_lookup(char *nm, uint_t buflen, struct svc_req *req,
	struct compound_state *cs)
{
	int error;
	int different_export = 0;
	vnode_t *vp, *tvp, *pre_tvp = NULL, *oldvp = NULL;
	struct exportinfo *exi = NULL, *pre_exi = NULL;
	nfsstat4 stat;
	fid_t fid;
	int attrdir, dotdot, walk;
	bool_t is_newvp = FALSE;
	caller_context_t ct;
	nfs41_fh_fmt_t *fhp;

	fhp = (nfs41_fh_fmt_t *)cs->fh.nfs_fh4_val;

	attrdir = ((cs->vp->v_flag & V_XATTRDIR) == V_XATTRDIR)
	    ? FH41_ATTRDIR : 0;

	ASSERT(FH41_GET_FLAG(fhp, FH41_ATTRDIR) == attrdir);

	dotdot = (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0');

	/*
	 * If dotdotting, then need to check whether it's
	 * above the root of a filesystem, or above an
	 * export point.
	 */
	if (dotdot) {

		/*
		 * If dotdotting at the root of a filesystem, then
		 * need to traverse back to the mounted-on filesystem
		 * and do the dotdot lookup there.
		 */
		if (cs->vp->v_flag & VROOT) {

			/*
			 * If at the system root, then can
			 * go up no further.
			 */
			if (VN_CMP(cs->vp, rootdir))
				return (puterrno4(ENOENT));

			/*
			 * Traverse back to the mounted-on filesystem
			 */
			cs->vp = untraverse(cs->vp);

			/*
			 * Set the different_export flag so we remember
			 * to pick up a new exportinfo entry for
			 * this new filesystem.
			 */
			different_export = 1;
		} else {

			/*
			 * If dotdotting above an export point then set
			 * the different_export to get new export info.
			 */
			different_export = nfs_exported(cs->exi, cs->vp);
		}
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	error = VOP_LOOKUP(cs->vp, nm, &vp, NULL, 0, NULL, cs->cr,
	    &ct, 0, NULL);
	if (error)
		return (puterrno4(error));

	/*
	 * If the vnode is in a pseudo filesystem, check whether it is visible.
	 *
	 * XXX if the vnode is a symlink and it is not visible in
	 * a pseudo filesystem, return ENOENT (not following symlink).
	 * V4 client can not mount such symlink.
	 *
	 * In the same exported filesystem, if the security flavor used
	 * is not an explicitly shared flavor, limit the view to the visible
	 * list entries only. This is not a WRONGSEC case because it's already
	 * checked via PUTROOTFH/PUTPUBFH or PUTFH.
	 */
	if (!different_export &&
	    (PSEUDO(cs->exi) || ! is_exported_sec(cs->nfsflavor, cs->exi) ||
	    cs->access & CS_ACCESS_LIMITED)) {
		if (! nfs_visible(cs->exi, vp, &different_export)) {
			VN_RELE(vp);
			return (puterrno4(ENOENT));
		}
	}

	/*
	 * If it's a mountpoint, then traverse it.
	 */
	if (vn_ismntpt(vp)) {
		pre_exi = cs->exi;	/* save pre-traversed exportinfo */
		pre_tvp = vp;		/* save pre-traversed vnode	*/

		/*
		 * hold pre_tvp to counteract rele by traverse.  We will
		 * need pre_tvp below if checkexport4 fails
		 */
		VN_HOLD(pre_tvp);
		tvp = vp;
		if ((error = traverse(&tvp)) != 0) {
			VN_RELE(vp);
			VN_RELE(pre_tvp);
			return (puterrno4(error));
		}
		vp = tvp;
		different_export = 1;

	} else if (vp->v_vfsp != cs->vp->v_vfsp) {
		/*
		 * The vfsp comparison is to handle the case where
		 * a LOFS mount is shared.  lo_lookup traverses mount points,
		 * and NFS is unaware of local fs transistions because
		 * v_vfsmountedhere isn't set.  For this special LOFS case,
		 * the dir and the obj returned by lookup will have different
		 * vfs ptrs.
		 */
		different_export = 1;
	}

	if (different_export) {

		bzero(&fid, sizeof (fid));
		fid.fid_len = MAXFIDSZ;
		error = vop_fid_pseudo(vp, &fid);
		if (error) {
			VN_RELE(vp);
			if (pre_tvp)
				VN_RELE(pre_tvp);
			return (puterrno4(error));
		}

		if (dotdot)
			exi = nfs_vptoexi(NULL, vp, cs->cr, &walk, NULL, TRUE);
		else
			exi = checkexport4(&vp->v_vfsp->vfs_fsid, &fid, vp);

		if (exi == NULL) {
			if (pre_tvp) {
				/*
				 * If this vnode is a mounted-on vnode,
				 * but the mounted-on file system is not
				 * exported, send back the filehandle for
				 * the mounted-on vnode, not the root of
				 * the mounted-on file system.
				 */
				VN_RELE(vp);
				vp = pre_tvp;
				exi = pre_exi;
			} else {
				VN_RELE(vp);
				return (puterrno4(EACCES));
			}
		} else if (pre_tvp) {
			/* we're done with pre_tvp now. release extra hold */
			VN_RELE(pre_tvp);
		}

		cs->exi = exi;

		/*
		 * Now do a checkauth4.
		 *
		 * Checking here since the client/principle may not have
		 * access to the cs->exi exported file system.
		 *
		 * If the client has access we also need to validate
		 * the principle since it may have been re-mapped.
		 *
		 * We start with a new credential as a previous call to
		 * checkauth4(), via a PUT*FH operation, wrote over cs->cr.
		 */
		crfree(cs->cr);
		cs->cr = crdup(cs->basecr);

		if (cs->vp)
			oldvp = cs->vp;
		cs->vp = vp;
		is_newvp = TRUE;

		stat = call_checkauth4(cs, req);
		if (stat != NFS4_OK) {
			VN_RELE(cs->vp);
			cs->vp = oldvp;
			return (stat);
		}
	}

	/*
	 * After various NFS checks, do a label check on the path
	 * component. The label on this path should either be the
	 * global zone's label or a zone's label. We are only
	 * interested in the zone's label because exported files
	 * in global zone is accessible (though read-only) to
	 * clients. The exportability/visibility check is already
	 * done before reaching this code.
	 */
	if (is_system_labeled()) {
		bslabel_t *clabel;

		ASSERT(req->rq_label != NULL);
		clabel = req->rq_label;
		DTRACE_PROBE2(tx__rfs4__log__info__oplookup__clabel, char *,
		    "got client label from request(1)", struct svc_req *, req);

		if (!blequal(&l_admin_low->tsl_label, clabel)) {
			if (!do_rfs_label_check(clabel, vp, DOMINANCE_CHECK)) {
				error = EACCES;
				goto err_out;
			}
		} else {
			/*
			 * We grant access to admin_low label clients
			 * only if the client is trusted, i.e. also
			 * running Solaris Trusted Extension.
			 */
			struct sockaddr	*ca;
			int		addr_type;
			void		*ipaddr;
			tsol_tpc_t	*tp;

			ca = (struct sockaddr *)svc_getrpccaller(
			    req->rq_xprt)->buf;
			if (ca->sa_family == AF_INET) {
				addr_type = IPV4_VERSION;
				ipaddr = &((struct sockaddr_in *)ca)->sin_addr;
			} else if (ca->sa_family == AF_INET6) {
				addr_type = IPV6_VERSION;
				ipaddr = &((struct sockaddr_in6 *)
				    ca)->sin6_addr;
			}
			tp = find_tpc(ipaddr, addr_type, B_FALSE);
			if (tp == NULL || tp->tpc_tp.tp_doi !=
			    l_admin_low->tsl_doi || tp->tpc_tp.host_type !=
			    SUN_CIPSO) {
				error = EACCES;
				goto err_out;
			}
		}
	}

	error = mknfs41_fh(&cs->fh, vp, cs->exi);

err_out:
	if (error) {
		if (is_newvp) {
			VN_RELE(cs->vp);
			cs->vp = oldvp;
		} else
			VN_RELE(vp);
		return (puterrno4(error));
	}

	if (!is_newvp) {
		if (cs->vp)
			VN_RELE(cs->vp);
		cs->vp = vp;
	} else if (oldvp)
		VN_RELE(oldvp);

	/*
	 * if did lookup on attrdir and didn't lookup .., set named
	 * attr fh flag
	 */
	if (attrdir && ! dotdot)
		FH41_SET_FLAG(fhp, FH41_NAMEDATTR);

	/* Assume false for now, open proc will set this */
	cs->mandlock = FALSE;

	return (NFS4_OK);
}

/* ARGSUSED */
static void
mds_op_lookup(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	LOOKUP4args *args = &argop->nfs_argop4_u.oplookup;
	LOOKUP4res *resp = &resop->nfs_resop4_u.oplookup;
	char *nm;
	uint_t len;

	DTRACE_NFSV4_2(op__lookup__start, struct compound_state *, cs,
	    LOOKUP4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (cs->vp->v_type == VLNK) {
		*cs->statusp = resp->status = NFS4ERR_SYMLINK;
		goto final;
	}

	if (cs->vp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}

	if (!utf8_dir_verify(&args->objname)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	nm = utf8_to_str(&args->objname, &len, NULL);
	if (nm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (len > MAXNAMELEN) {
		*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
		kmem_free(nm, len);
		goto final;
	}

	*cs->statusp = resp->status = mds_do_lookup(nm, len, req, cs);

	kmem_free(nm, len);

final:
	DTRACE_NFSV4_2(op__lookup__done, struct compound_state *, cs,
	    LOOKUP4res *, resp);
}

/* ARGSUSED */
static void
mds_op_lookupp(nfs_argop4 *args, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	LOOKUPP4res *resp = &resop->nfs_resop4_u.oplookupp;

	DTRACE_NFSV4_1(op__lookupp__start, struct compound_state *, cs);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (cs->vp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}

	*cs->statusp = resp->status = mds_do_lookup("..", 3, req, cs);

	/*
	 * From NFSV4 Specification, LOOKUPP should not check for
	 * NFS4ERR_WRONGSEC. Retrun NFS4_OK instead.
	 */
	if (resp->status == NFS4ERR_WRONGSEC) {
		*cs->statusp = resp->status = NFS4_OK;
	}

final:
	DTRACE_NFSV4_2(op__lookupp__done, struct compound_state *, cs,
	    LOOKUPP4res *, resp);
}


/*ARGSUSED2*/
static void
mds_op_openattr(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	OPENATTR4args	*args = &argop->nfs_argop4_u.opopenattr;
	OPENATTR4res	*resp = &resop->nfs_resop4_u.opopenattr;
	vnode_t		*avp = NULL;
	int		lookup_flags = LOOKUP_XATTR, error;
	int		exp_ro = 0;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__openattr__start, struct compound_state *, cs,
	    OPENATTR4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/*
	 * Make a couple of checks made by copen()
	 *
	 * Check to make sure underlying fs supports xattrs.  This
	 * is required because solaris filesystem implementations
	 * (UFS/TMPFS) don't enforce the noxattr mount option
	 * in VOP_LOOKUP(LOOKUP_XATTR).  If fs doesn't support this
	 * pathconf cmd or if fs supports cmd but doesn't claim
	 * support for xattr, return NOTSUPP.  It would be better
	 * to use VOP_PATHCONF( _PC_XATTR_ENABLED) for this; however,
	 * that cmd is not available to VOP_PATHCONF interface
	 * (it's only implemented inside pathconf syscall)...
	 *
	 * Verify permission to put attributes on files (access
	 * checks from copen).
	 */

	if ((cs->vp->v_vfsp->vfs_flag & VFS_XATTR) == 0) {
		error = ENOTSUP;
		goto error_out;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	if ((VOP_ACCESS(cs->vp, VREAD, 0, cs->cr, &ct) != 0) &&
	    (VOP_ACCESS(cs->vp, VWRITE, 0, cs->cr, &ct) != 0) &&
	    (VOP_ACCESS(cs->vp, VEXEC, 0, cs->cr, &ct) != 0)) {
		error = EACCES;
		goto error_out;
	}

	/*
	 * The CREATE_XATTR_DIR VOP flag cannot be specified if
	 * the file system is exported read-only -- regardless of
	 * createdir flag.  Otherwise the attrdir would be created
	 * (assuming server fs isn't mounted readonly locally).  If
	 * VOP_LOOKUP returns ENOENT in this case, the error will
	 * be translated into EROFS.  ENOSYS is mapped to ENOTSUP
	 * because specfs has no VOP_LOOKUP op, so the macro would
	 * return ENOSYS.  EINVAL is returned by all (current)
	 * Solaris file system implementations when any of their
	 * restrictions are violated (xattr(dir) can't have xattrdir).
	 * Returning NOTSUPP is more appropriate in this case
	 * because the object will never be able to have an attrdir.
	 */
	if (args->createdir && ! (exp_ro = rdonly4(cs->exi, cs->vp, req)))
		lookup_flags |= CREATE_XATTR_DIR;

	error = VOP_LOOKUP(cs->vp, "", &avp, NULL, lookup_flags, NULL,
	    cs->cr, &ct, 0, NULL);

	if (error) {
		if (error == ENOENT && args->createdir && exp_ro)
			error = EROFS;
		else if (error == EINVAL || error == ENOSYS)
			error = ENOTSUP;
		goto error_out;
	}

	ASSERT(avp->v_flag & V_XATTRDIR);

	error = mknfs41_fh(&cs->fh, avp, cs->exi);

	if (error) {
		VN_RELE(avp);
		goto error_out;
	}

	VN_RELE(cs->vp);
	cs->vp = avp;

	/*
	 * There is no requirement for an attrdir fh flag
	 * because the attrdir has a vnode flag to distinguish
	 * it from regular (non-xattr) directories.  The
	 * FH41_ATTRDIR flag is set for future sanity checks.
	 */
	FH41_SET_FLAG((nfs41_fh_fmt_t *)cs->fh.nfs_fh4_val, FH41_ATTRDIR);
	*cs->statusp = resp->status = NFS4_OK;
	goto final;

error_out:

	*cs->statusp = resp->status = puterrno4(error);

final:
	DTRACE_NFSV4_2(op__openattr__done, struct compound_state *, cs,
	    OPENATTR4res *, resp);
}

static int
do_io(int direction, vnode_t *vp, struct uio *uio, int ioflag, cred_t *cred,
    caller_context_t *ct)
{
	int error;
	int i;
	clock_t delaytime;

	delaytime = MSEC_TO_TICK_ROUNDUP(rfs4_lock_delay);

	/*
	 * Don't block on mandatory locks. If this routine returns
	 * EAGAIN, the caller should return NFS4ERR_LOCKED.
	 */
	uio->uio_fmode = FNONBLOCK;

	for (i = 0; i < rfs4_maxlock_tries; i++) {
		if (direction == FREAD) {
			(void) VOP_RWLOCK(vp, V_WRITELOCK_FALSE, ct);
			error = VOP_READ(vp, uio, ioflag, cred, ct);
			VOP_RWUNLOCK(vp, V_WRITELOCK_FALSE, ct);
		} else {
			(void) VOP_RWLOCK(vp, V_WRITELOCK_TRUE, ct);
			error = VOP_WRITE(vp, uio, ioflag, cred, ct);
			VOP_RWUNLOCK(vp, V_WRITELOCK_TRUE, ct);
		}

		if (error != EAGAIN)
			break;

		if (i < rfs4_maxlock_tries - 1) {
			delay(delaytime);
			delaytime *= 2;
		}
	}

	return (error);
}

/* ARGSUSED */
static void
mds_op_read(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	READ4args *args = &argop->nfs_argop4_u.opread;
	READ4res *resp = &resop->nfs_resop4_u.opread;
	int error;
	nnode_t *nn = NULL;
	struct iovec iov;
	struct uio uio;
	bool_t *deleg = &cs->deleg;
	nfsstat4 stat;
	mblk_t *mp;
	int alloc_err = 0;
	caller_context_t ct;
	uint32_t nnioflags = 0;

	DTRACE_NFSV4_2(op__read__start, struct compound_state *, cs,
	    READ4args, args);

	nn = cs->nn;
	if (nn == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}
	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	if ((stat = nnop_check_stateid(nn, cs, FREAD, &args->stateid,
	    FALSE, deleg, TRUE, &ct, NULL)) != NFS4_OK) {
		*cs->statusp = resp->status = stat;
		goto final;
	}

	error = nnop_io_prep(nn, &nnioflags, cs->cr, &ct, args->offset,
	    args->count, NULL);
	if (error != 0) {
		*cs->statusp = resp->status = nnode_stat4(error, 1);
		goto out;
	}

	if (nnioflags & NNODE_IO_FLAG_PAST_EOF) {
		*cs->statusp = resp->status = NFS4_OK;
		resp->eof = TRUE;
		resp->data_len = 0;
		resp->data_val = NULL;
		resp->mblk = NULL;
		*cs->statusp = resp->status = NFS4_OK;
		goto out;
	}

	if (args->count == 0) {
		*cs->statusp = resp->status = NFS4_OK;
		resp->eof = FALSE;
		resp->data_len = 0;
		resp->data_val = NULL;
		resp->mblk = NULL;
		goto out;
	}

	/*
	 * Do not allocate memory more than maximum allowed
	 * transfer size
	 */
	if (args->count > rfs4_tsize(req))
		args->count = rfs4_tsize(req);

	if (args->wlist) {
		mp = NULL;
		(void) rdma_get_wchunk(req, &iov, args->wlist);
	} else {
		/*
		 * mp will contain the data to be sent out in the read reply.
		 * It will be freed after the reply has been sent.
		 * Let's roundup the data to a BYTES_PER_XDR_UNIT multiple,
		 * so that the call to xdrmblk_putmblk() never fails.
		 * If the first alloc of the requested size fails, then
		 * decrease the size to something more reasonable and wait
		 * for the allocation to occur.
		 */
		mp = allocb(RNDUP(args->count), BPRI_MED);
		if (mp == NULL) {
			if (args->count > MAXBSIZE)
				args->count = MAXBSIZE;
			mp = allocb_wait(RNDUP(args->count), BPRI_MED,
			    STR_NOSIG, &alloc_err);
		}
		ASSERT(mp != NULL);
		ASSERT(alloc_err == 0);

		iov.iov_base = (caddr_t)mp->b_datap->db_base;
		iov.iov_len = args->count;
	}

	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_extflg = UIO_COPY_CACHED;
	uio.uio_loffset = args->offset;
	uio.uio_resid = args->count;

	error = nnop_read(nn, &nnioflags, cs->cr, &ct, &uio, 0);
	if (error) {
		if (mp != NULL)
			freeb(mp);
		*cs->statusp = resp->status = nnode_stat4(error, 1);
		goto out;
	}

	*cs->statusp = resp->status = NFS4_OK;

	ASSERT(uio.uio_resid >= 0);
	resp->data_len = args->count - uio.uio_resid;
	resp->data_val = (char *)mp->b_datap->db_base;
	resp->mblk = mp;

	resp->eof = (nnioflags & NNODE_IO_FLAG_EOF) ? TRUE : FALSE;

out:
	nnop_io_release(nn, nnioflags, &ct);

final:
	DTRACE_NFSV4_2(op__read__done, struct compound_state *, cs,
	    READ4res *, resp);
}

/*ARGSUSED*/
static void
mds_op_read_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function for NFSv4.0 and NFSv4.1 */
	rfs4_op_read_free(resop);
}

/* ARGSUSED */
static void
mds_op_putpubfh(nfs_argop4 *args, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	PUTPUBFH4res *resp = &resop->nfs_resop4_u.opputpubfh;
	int error;
	vnode_t *vp;
	struct exportinfo *exi, *sav_exi;
	nfs41_fh_fmt_t *fhp;
	fid_t exp_fid;

	DTRACE_NFSV4_1(op__putpubfh__start, struct compound_state *, cs);

	if (cs->vp) {
		VN_RELE(cs->vp);
		cs->vp = NULL;
	}

	if (cs->cr)
		crfree(cs->cr);

	cs->cr = crdup(cs->basecr);

	vp = exi_public->exi_vp;
	if (vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_SERVERFAULT;
		goto final;
	}

	error = mknfs41_fh(&cs->fh, vp, exi_public);
	if (error != 0) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}
	sav_exi = cs->exi;
	if (exi_public == exi_root) {
		/*
		 * No filesystem is actually shared public, so we default
		 * to exi_root. In this case, we must check whether root
		 * is exported.
		 */
		fhp = (nfs41_fh_fmt_t *)cs->fh.nfs_fh4_val;

		exp_fid.fid_len = fhp->fh.v1.export_fid.len;

		bcopy(fhp->fh.v1.export_fid.val, exp_fid.fid_data,
		    exp_fid.fid_len);

		/*
		 * if root filesystem is exported, the exportinfo struct that we
		 * should use is what checkexport4 returns, because root_exi is
		 * actually a mostly empty struct.
		 */
		exi = checkexport4(&fhp->fh.v1.export_fsid, &exp_fid, NULL);
		cs->exi = ((exi != NULL) ? exi : exi_public);
	} else {
		/*
		 * it's a properly shared filesystem
		 */
		cs->exi = exi_public;
	}

	VN_HOLD(vp);
	cs->vp = vp;

	if ((resp->status = call_checkauth4(cs, req)) != NFS4_OK) {
		VN_RELE(cs->vp);
		cs->vp = NULL;
		cs->exi = sav_exi;
		goto final;
	}

	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__putpubfh__done, struct compound_state *, cs,
	    PUTPUBFH4res *, resp);
}

/*
 * XXX - issue with put*fh operations.
 *
 * let us assume that /export/home is shared via NFS and a NFS client
 * wishes to mount /export/home/joe.
 *
 * If /export, home, or joe have restrictive search permissions, then
 * the NFS Server should not return a filehandle to the client.
 *
 * This case is easy to enforce. However, the NFS Client does not know
 * which security flavor should be used until the pathname has been
 * fully resolved. In addition there is another complication for uid
 * mapping. If the credential being used is root, the default behaviour
 * will be to map it to the anonymous user. However the NFS Server can not
 * map it until the pathname has been fully resolved.
 *
 * XXX: JEFF:  Proposed solution.
 *
 * Luckily, SECINFO uses a full pathname.  So what we will
 * have to do in mds_op_lookup is check that flavor of
 * the target object matches that of the request, and if root was the
 * caller, check for the root= and anon= options, and if necessary,
 * repeat the lookup using the right cred_t.
 *
 * But that's not done yet.
 */
/* ARGSUSED */
static void
mds_op_putfh(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	PUTFH4args *args = &argop->nfs_argop4_u.opputfh;
	PUTFH4res *resp = &resop->nfs_resop4_u.opputfh;
	nfs41_fh_fmt_t *fhp = NULL;
	fid_t  exp_fid;
	int error;

	DTRACE_NFSV4_2(op__putfh__start, struct compound_state *, cs,
	    PUTFH4args *, args);

	/*
	 * release the old nnode, vnode and cred.
	 */
	if (cs->nn)
		nnode_rele(&cs->nn);
	if (cs->vp) {
		VN_RELE(cs->vp);
		cs->vp = NULL;
	}

	if (cs->cr) {
		crfree(cs->cr);
		cs->cr = NULL;
	}


	/*
	 * Check exportinfo only if it's a FH41_TYPE_NFS filehandle.
	 * If the filehandle is otherwise incorrect,
	 * nnode_from_fh_v41() will return an error.
	 */
	fhp = (nfs41_fh_fmt_t *)args->object.nfs_fh4_val;
	if (fhp->type == FH41_TYPE_NFS) {
		exp_fid.fid_len = fhp->fh.v1.export_fid.len;
		bcopy(fhp->fh.v1.export_fid.val, exp_fid.fid_data,
		    exp_fid.fid_len);
		cs->exi = checkexport4(&fhp->fh.v1.export_fsid, &exp_fid, NULL);
		if (cs->exi == NULL) {
			*cs->statusp = resp->status = NFS4ERR_STALE;
			DTRACE_PROBE(nfss41__e__chkexp);
			goto final;
		}
	}

	cs->cr = crdup(cs->basecr);

	ASSERT(cs->cr != NULL);

	error = nnode_from_fh_v41(&cs->nn, &args->object);
	if (error != 0) {
		resp->status = *cs->statusp = nnode_stat4(error, 1);
		goto final;
	}
	cs->vp = nnop_io_getvp(cs->nn);

	if (fhp->type == FH41_TYPE_NFS) {
		if ((resp->status = call_checkauth4(cs, req)) != NFS4_OK) {
			VN_RELE(cs->vp);
			*cs->statusp = resp->status;
			cs->vp = NULL;
			DTRACE_PROBE(nfss41__e__fail_auth);
			goto final;
		}
	}

	nfs_fh4_copy(&args->object, &cs->fh);
	*cs->statusp = resp->status = NFS4_OK;
	cs->deleg = FALSE;

final:
	DTRACE_NFSV4_2(op__putfh__done, struct compound_state *, cs,
	    PUTFH4res *, resp);
}

/* ARGSUSED */
static void
mds_op_putrootfh(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)

{
	PUTROOTFH4res *resp = &resop->nfs_resop4_u.opputrootfh;
	int error;
	fid_t fid;
	struct exportinfo *exi, *sav_exi;

	DTRACE_NFSV4_1(op__putrootfh__start, struct compound_state *, cs);

	if (cs->vp) {
		VN_RELE(cs->vp);
		cs->vp = NULL;
	}

	if (cs->cr)
		crfree(cs->cr);

	cs->cr = crdup(cs->basecr);

	/*
	 * Using rootdir, the system root vnode,
	 * get its fid.
	 */
	bzero(&fid, sizeof (fid));
	fid.fid_len = MAXFIDSZ;
	error = vop_fid_pseudo(rootdir, &fid);
	if (error != 0) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	/*
	 * Then use the root fsid & fid it to find out if it's exported
	 *
	 * If the server root isn't exported directly, then
	 * it should at least be a pseudo export based on
	 * one or more exports further down in the server's
	 * file tree.
	 */
	exi = checkexport4(&rootdir->v_vfsp->vfs_fsid, &fid, NULL);
	if (exi == NULL || exi->exi_export.ex_flags & EX_PUBLIC) {
		DTRACE_PROBE(nfss41__e__chkexp);
		*cs->statusp = resp->status = NFS4ERR_SERVERFAULT;
		goto final;
	}

	/*
	 * Now make a filehandle based on the root
	 * export and root vnode.
	 */
	error = mknfs41_fh(&cs->fh, rootdir, exi);
	if (error != 0) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	sav_exi = cs->exi;
	cs->exi = exi;

	VN_HOLD(rootdir);
	cs->vp = rootdir;

	if ((resp->status = call_checkauth4(cs, req)) != NFS4_OK) {
		VN_RELE(rootdir);
		cs->vp = NULL;
		cs->exi = sav_exi;
		goto final;
	}

	*cs->statusp = resp->status = NFS4_OK;
	cs->deleg = FALSE;

final:
	DTRACE_NFSV4_2(op__putrootfh__done, struct compound_state *, cs,
	    PUTROOTFH4res *, resp);
}

/*
 * A directory entry is a valid nfsv4 entry if
 * - it has a non-zero ino
 * - it is not a dot or dotdot name
 * - it is visible in a pseudo export or in a real export that can
 *   only have a limited view.
 */
static bool_t
valid_nfs4_entry(struct exportinfo *exi, struct dirent64 *dp,
		int *expseudo, int check_visible)
{
	if (dp->d_ino == 0 || NFS_IS_DOTNAME(dp->d_name)) {
		*expseudo = 0;
		return (FALSE);
	}

	if (! check_visible) {
		*expseudo = 0;
		return (TRUE);
	}

	return (nfs_visible_inode(exi, dp->d_ino, expseudo));
}


/*
 * readlink: args: CURRENT_FH.
 *	res: status. If success - CURRENT_FH unchanged, return linktext.
 */

/* ARGSUSED */
static void
mds_op_readlink(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	READLINK4res *resp = &resop->nfs_resop4_u.opreadlink;
	int error;
	vnode_t *vp;
	struct iovec iov;
	struct vattr va;
	struct uio uio;
	char *data;
	caller_context_t ct;

	DTRACE_NFSV4_1(op__readlink__start, struct compound_state *, cs);

	/* CURRENT_FH: directory */
	vp = cs->vp;
	if (vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	if (vp->v_type == VDIR) {
		*cs->statusp = resp->status = NFS4ERR_ISDIR;
		goto final;
	}

	if (vp->v_type != VLNK) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	va.va_mask = AT_MODE;
	error = VOP_GETATTR(vp, &va, 0, cs->cr, &ct);
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	if (MANDLOCK(vp, va.va_mode)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	data = kmem_alloc(MAXPATHLEN + 1, KM_SLEEP);

	iov.iov_base = data;
	iov.iov_len = MAXPATHLEN;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_extflg = UIO_COPY_CACHED;
	uio.uio_loffset = 0;
	uio.uio_resid = MAXPATHLEN;

	error = VOP_READLINK(vp, &uio, cs->cr, &ct);

	if (error) {
		kmem_free((caddr_t)data, (uint_t)MAXPATHLEN + 1);
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	*(data + MAXPATHLEN - uio.uio_resid) = '\0';

	/*
	 * treat link name as data
	 */
	(void) str_to_utf8(data, &resp->link);

	kmem_free((caddr_t)data, (uint_t)MAXPATHLEN + 1);
	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__readlink__done, struct compound_state *, cs,
	    READLINK4res *, resp);
}

/*ARGSUSED*/
static void
mds_op_readlink_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function used for NFSv4.0 and NFSv4.1 */
	rfs4_op_readlink_free(resop);
}

/* ARGSUSED */
static void
mds_op_reclaim_complete(nfs_argop4 *argop, nfs_resop4 *resop,
    struct svc_req *req, compound_state_t *cs)
{
	RECLAIM_COMPLETE4args *args = &argop->nfs_argop4_u.opreclaim_complete;
	RECLAIM_COMPLETE4res *resp = &resop->nfs_resop4_u.opreclaim_complete;
	rfs4_client_t *cp;

	cp = cs->cp;

	if (cp->reclaim_completed) {
		*cs->statusp = resp->rcr_status = NFS4ERR_COMPLETE_ALREADY;
		return;
	}

	if (args->rca_one_fs) {
		/* do what?  we don't track this */
		*cs->statusp = resp->rcr_status = NFS4_OK;
		return;
	}

	cp->reclaim_completed = 1;

	/* did we have reclaimable state stored for this client? */
	if (cp->can_reclaim)
		atomic_add_32(&(cs->instp->reclaim_cnt), -1);

	*cs->statusp = resp->rcr_status = NFS4_OK;
}

/*
 * short utility function to lookup a file and recall the delegation
 */
static rfs4_file_t *
mds_lookup_and_findfile(vnode_t *dvp, char *nm, vnode_t **vpp,
	int *lkup_error, struct compound_state *cs)
{
	vnode_t *vp;
	rfs4_file_t *fp = NULL;
	bool_t fcreate = FALSE;
	int error;

	if (vpp)
		*vpp = NULL;

	if ((error = VOP_LOOKUP(dvp, nm, &vp, NULL, 0, NULL, cs->cr,
	    NULL, 0, NULL)) == 0) {
		if (vp->v_type == VREG)
			fp = rfs4_findfile(cs->instp, vp, NULL, &fcreate);
		if (vpp)
			*vpp = vp;
		else
			VN_RELE(vp);
	}

	if (lkup_error)
		*lkup_error = error;

	return (fp);
}

static int
do_ctl_mds_remove(vnode_t *vp, rfs4_file_t *fp, compound_state_t *cs)
{
	fid_t fid;
	nfs41_fid_t nfs41_fid;
	int error = 0;

	/*
	 * Use the file layout to determine which data servers to
	 * send DS_REMOVEs to.  If the layout is not cached in the
	 * rfs4_file_t either this means that we do not have a layout
	 * or it needs to be read in from disk.  Right now, we do not
	 * attempt to read the layout in from disk, but future phases
	 * of REMOVE handling will take * this into consideration.
	 *
	 * Known Problems with this implementation of REMOVE:
	 * 1. Not attempting to read a layout * from disk could mean
	 * that if an on-disk layout did exist, storage on the data
	 * servers will not be freed.  Although, the implementation
	 * doesn't currently persistenly store layouts so we'll never
	 * run into this situation.
	 *
	 * 2. The server populates the layout stored in the rfs4_file_t
	 * when it receives a LAYOUTGET.  If the file has been written
	 * (perhaps in a past server instance), but no clients have
	 * issued new LAYOUTGETs, we will not have a cached layout and
	 * we will not free space on the data servers.
	 *
	 * 3. If any of the DS_REMOVE calls to the data servers fail
	 * the errors are ignored and will not be retried.  This may
	 * cause leaked space on the the data server.
	 */
	if (fp->flp != NULL) {
		error = vop_fid_pseudo(vp, &fid);
		if (error) {
			DTRACE_PROBE(nfss__e__vop_fid_pseudo_failed);
			return (error);
		} else {
			nfs41_fid.len = fid.fid_len;
			bcopy(fid.fid_data, nfs41_fid.val, nfs41_fid.len);
		}

		error = ctl_mds_clnt_remove_file(cs->instp, cs->exi->exi_fsid,
		    nfs41_fid, fp->flp);
	} else
		DTRACE_PROBE(nfss__i__layout_is_null_cannot_remove);

	return (error);
}

/*
 * remove: args: CURRENT_FH: directory; name.
 *	res: status. If success - CURRENT_FH unchanged, return change_info
 *		for directory.
 */
/* ARGSUSED */
static void
mds_op_remove(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	REMOVE4args *args = &argop->nfs_argop4_u.opremove;
	REMOVE4res *resp = &resop->nfs_resop4_u.opremove;
	int error;
	vnode_t *dvp, *vp;
	struct vattr bdva, idva, adva;
	char *nm;
	uint_t len;
	rfs4_file_t *fp;
	int in_crit = 0;
	bslabel_t *clabel;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__remove__start, struct compound_state *, cs,
	    REMOVE4args *, args);

	/* CURRENT_FH: directory */
	dvp = cs->vp;
	if (dvp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	/*
	 * If there is an unshared filesystem mounted on this vnode,
	 * Do not allow to remove anything in this directory.
	 */
	if (vn_ismntpt(dvp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	if (dvp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}

	if (!utf8_dir_verify(&args->target)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	/*
	 * Lookup the file so that we can check if it's a directory
	 */
	nm = utf8_to_fn(&args->target, &len, NULL);
	if (nm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (len > MAXNAMELEN) {
		*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
		kmem_free(nm, len);
		goto final;
	}

	if (rdonly4(cs->exi, cs->vp, req)) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		kmem_free(nm, len);
		goto final;
	}

	/*
	 * Lookup the file to determine type and while we are see if
	 * there is a file struct around and check for delegation.
	 * We don't need to acquire va_seq before this lookup, if
	 * it causes an update, cinfo.before will not match, which will
	 * trigger a cache flush even if atomic is TRUE.
	 */
	fp = mds_lookup_and_findfile(dvp, nm, &vp, &error, cs);
	if (vp != NULL) {
		if (rfs4_check_delegated(FWRITE, vp, TRUE, TRUE, TRUE, NULL)) {
			VN_RELE(vp);
			rfs4_file_rele(fp);
			*cs->statusp = resp->status = NFS4ERR_DELAY;
			kmem_free(nm, len);
			goto final;
		}
	} else {	/* Didn't find anything to remove */
		*cs->statusp = resp->status = error;
		kmem_free(nm, len);
		goto final;
	}

	if (nbl_need_check(vp)) {
		nbl_start_crit(vp, RW_READER);
		in_crit = 1;
		if (nbl_conflict(vp, NBL_REMOVE, 0, 0, 0, &ct)) {
			*cs->statusp = resp->status = NFS4ERR_FILE_OPEN;
			kmem_free(nm, len);
			nbl_end_crit(vp);
			VN_RELE(vp);
			if (fp) {
				rfs4_clear_dont_grant(cs->instp, fp);
				rfs4_file_rele(fp);
			}
			goto final;
		}
	}

	/* check label before allowing removal */
	if (is_system_labeled()) {
		ASSERT(req->rq_label != NULL);
		clabel = req->rq_label;
		DTRACE_PROBE2(tx__rfs4__log__info__opremove__clabel, char *,
		    "got client label from request(1)",
		    struct svc_req *, req);
		if (!blequal(&l_admin_low->tsl_label, clabel)) {
			if (!do_rfs_label_check(clabel, vp, EQUALITY_CHECK)) {
				*cs->statusp = resp->status = NFS4ERR_ACCESS;
				kmem_free(nm, len);
				if (in_crit)
					nbl_end_crit(vp);
				VN_RELE(vp);
				if (fp) {
					rfs4_clear_dont_grant(cs->instp, fp);
					rfs4_file_rele(fp);
				}
				goto final;
			}
		}
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	/* Get dir "before" change value */
	bdva.va_mask = AT_CTIME|AT_SEQ;
	error = VOP_GETATTR(dvp, &bdva, 0, cs->cr, &ct);
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		kmem_free(nm, len);
		goto final;
	}
	NFS4_SET_FATTR4_CHANGE(resp->cinfo.before, bdva.va_ctime)

	/* Actually do the REMOVE operation */
	if (vp->v_type == VDIR) {
		/*
		 * Can't remove a directory that has a mounted-on filesystem.
		 */
		if (vn_ismntpt(vp)) {
			error = EACCES;
		} else {
			/*
			 * System V defines rmdir to return EEXIST,
			 * not * ENOTEMPTY, if the directory is not
			 * empty.  A System V NFS server needs to map
			 * NFS4ERR_EXIST to NFS4ERR_NOTEMPTY to
			 * transmit over the wire.
			 */
			if ((error = VOP_RMDIR(dvp, nm, rootdir,
			    cs->cr, &ct, 0)) == EEXIST)
				error = ENOTEMPTY;
		}
	} else {
		if ((error = VOP_REMOVE(dvp, nm, cs->cr, &ct, 0)) == 0 &&
		    fp != NULL) {
			struct vattr va;
			vnode_t *tvp;

			rfs4_dbe_lock(fp->dbe);
			tvp = fp->vp;
			if (tvp)
				VN_HOLD(tvp);
			rfs4_dbe_unlock(fp->dbe);

			if (tvp) {
				/*
				 * This is va_seq safe because we are not
				 * manipulating dvp.
				 */
				va.va_mask = AT_NLINK;
				if (!VOP_GETATTR(tvp, &va, 0, cs->cr,
				    &ct) && va.va_nlink == 0) {
					if (in_crit) {
						nbl_end_crit(vp);
						in_crit = 0;
					}

					/*
					 * Remove objects on data servers.
					 * Ignore errors for now..
					 */
					(void) do_ctl_mds_remove(tvp, fp, cs);

					/* Remove state on file remove */
					rfs4_close_all_state(fp);
				}
				VN_RELE(tvp);
			}
		}
	}

	if (in_crit)
		nbl_end_crit(vp);
	VN_RELE(vp);

	if (fp) {
		rfs4_clear_dont_grant(cs->instp, fp);
		rfs4_file_rele(fp);
	}
	kmem_free(nm, len);

	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	/*
	 * Get the initial "after" sequence number, if it fails, set to zero
	 */
	idva.va_mask = AT_SEQ;
	if (VOP_GETATTR(dvp, &idva, 0, cs->cr, &ct))
		idva.va_seq = 0;

	/*
	 * Force modified data and metadata out to stable storage.
	 */
	(void) VOP_FSYNC(dvp, 0, cs->cr, &ct);

	/*
	 * Get "after" change value, if it fails, simply return the
	 * before value.
	 */
	adva.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(dvp, &adva, 0, cs->cr, &ct)) {
		adva.va_ctime = bdva.va_ctime;
		adva.va_seq = 0;
	}

	NFS4_SET_FATTR4_CHANGE(resp->cinfo.after, adva.va_ctime)

	/*
	 * The cinfo.atomic = TRUE only if we have
	 * non-zero va_seq's, and it has incremented by exactly one
	 * during the VOP_REMOVE/RMDIR and it didn't change during
	 * the VOP_FSYNC.
	 */
	if (bdva.va_seq && idva.va_seq && adva.va_seq &&
	    idva.va_seq == (bdva.va_seq + 1) &&
	    idva.va_seq == adva.va_seq)
		resp->cinfo.atomic = TRUE;
	else
		resp->cinfo.atomic = FALSE;

	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__remove__done, struct compound_state *, cs,
	    REMOVE4res *, resp);
}

/*
 * rename: args: SAVED_FH: from directory, CURRENT_FH: target directory,
 *		oldname and newname.
 *	res: status. If success - CURRENT_FH unchanged, return change_info
 *		for both from and target directories.
 */
/* ARGSUSED */
static void
mds_op_rename(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	RENAME4args *args = &argop->nfs_argop4_u.oprename;
	RENAME4res *resp = &resop->nfs_resop4_u.oprename;
	int error;
	vnode_t *odvp;
	vnode_t *ndvp;
	vnode_t *srcvp, *targvp;
	struct vattr obdva, oidva, oadva;
	struct vattr nbdva, nidva, nadva;
	char *onm, *nnm;
	uint_t olen, nlen;
	rfs4_file_t *fp, *sfp;
	int in_crit_src, in_crit_targ;
	int fp_rele_grant_hold, sfp_rele_grant_hold;
	bslabel_t *clabel;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__rename__start, struct compound_state *, cs,
	    RENAME4args *, args);

	fp = sfp = NULL;
	srcvp = targvp = NULL;
	in_crit_src = in_crit_targ = 0;
	fp_rele_grant_hold = sfp_rele_grant_hold = 0;

	/* CURRENT_FH: target directory */
	ndvp = cs->vp;
	if (ndvp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/* SAVED_FH: from directory */
	odvp = cs->saved_vp;
	if (odvp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	/*
	 * If there is an unshared filesystem mounted on this vnode,
	 * do not allow to rename objects in this directory.
	 */
	if (vn_ismntpt(odvp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	/*
	 * If there is an unshared filesystem mounted on this vnode,
	 * do not allow to rename to this directory.
	 */
	if (vn_ismntpt(ndvp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	if (odvp->v_type != VDIR || ndvp->v_type != VDIR) {
		*cs->statusp = resp->status = NFS4ERR_NOTDIR;
		goto final;
	}

	if (cs->saved_exi != cs->exi) {
		*cs->statusp = resp->status = NFS4ERR_XDEV;
		goto final;
	}

	if (!utf8_dir_verify(&args->oldname)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if (!utf8_dir_verify(&args->newname)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	onm = utf8_to_fn(&args->oldname, &olen, NULL);
	if (onm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	nnm = utf8_to_fn(&args->newname, &nlen, NULL);
	if (nnm == NULL) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		kmem_free(onm, olen);
		goto final;
	}

	if (olen > MAXNAMELEN || nlen > MAXNAMELEN) {
		*cs->statusp = resp->status = NFS4ERR_NAMETOOLONG;
		kmem_free(onm, olen);
		kmem_free(nnm, nlen);
		goto final;
	}


	if (rdonly4(cs->exi, cs->vp, req)) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		kmem_free(onm, olen);
		kmem_free(nnm, nlen);
		goto final;
	}

	/* check label of the target dir */
	if (is_system_labeled()) {
		ASSERT(req->rq_label != NULL);
		clabel = req->rq_label;
		DTRACE_PROBE2(tx__rfs4__log__info__oprename__clabel, char *,
		    "got client label from request(1)",
		    struct svc_req *, req);
		if (!blequal(&l_admin_low->tsl_label, clabel)) {
			if (!do_rfs_label_check(clabel, ndvp,
			    EQUALITY_CHECK)) {
				*cs->statusp = resp->status = NFS4ERR_ACCESS;
				goto final;
			}
		}
	}

	/*
	 * Is the source a file and have a delegation?
	 * We don't need to acquire va_seq before these lookups, if
	 * it causes an update, cinfo.before will not match, which will
	 * trigger a cache flush even if atomic is TRUE.
	 */
	sfp = mds_lookup_and_findfile(odvp, onm, &srcvp, &error, cs);
	if (srcvp != NULL) {
		if (rfs4_check_delegated(FWRITE, srcvp, TRUE, TRUE, TRUE,
		    NULL)) {
			*cs->statusp = resp->status = NFS4ERR_DELAY;
			goto err_out;
		}
	} else {
		*cs->statusp = resp->status = puterrno4(error);
		kmem_free(onm, olen);
		kmem_free(nnm, nlen);
		goto final;
	}

	sfp_rele_grant_hold = 1;

	/* Does the destination exist and a file and have a delegation? */
	fp = mds_lookup_and_findfile(ndvp, nnm, &targvp, NULL, cs);
	if (targvp != NULL) {
		if (rfs4_check_delegated(FWRITE, targvp, TRUE, TRUE, TRUE,
		    NULL)) {
			*cs->statusp = resp->status = NFS4ERR_DELAY;
			goto err_out;
		}
	}
	fp_rele_grant_hold = 1;


	/* Check for NBMAND lock on both source and target */
	if (nbl_need_check(srcvp)) {
		nbl_start_crit(srcvp, RW_READER);
		in_crit_src = 1;
		if (nbl_conflict(srcvp, NBL_RENAME, 0, 0, 0, &ct)) {
			*cs->statusp = resp->status = NFS4ERR_FILE_OPEN;
			goto err_out;
		}
	}

	if (targvp && nbl_need_check(targvp)) {
		nbl_start_crit(targvp, RW_READER);
		in_crit_targ = 1;
		if (nbl_conflict(targvp, NBL_REMOVE, 0, 0, 0, &ct)) {
			*cs->statusp = resp->status = NFS4ERR_FILE_OPEN;
			goto err_out;
		}
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	/* Get source "before" change value */
	obdva.va_mask = AT_CTIME|AT_SEQ;
	error = VOP_GETATTR(odvp, &obdva, 0, cs->cr, &ct);
	if (!error) {
		nbdva.va_mask = AT_CTIME|AT_SEQ;
		error = VOP_GETATTR(ndvp, &nbdva, 0, cs->cr, &ct);
	}
	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto err_out;
	}

	NFS4_SET_FATTR4_CHANGE(resp->source_cinfo.before, obdva.va_ctime)
	NFS4_SET_FATTR4_CHANGE(resp->target_cinfo.before, nbdva.va_ctime)

	if ((error = VOP_RENAME(odvp, onm, ndvp, nnm, cs->cr, &ct, 0)) ==
	    0 && fp != NULL) {
		struct vattr va;
		vnode_t *tvp;

		rfs4_dbe_lock(fp->dbe);
		tvp = fp->vp;
		if (tvp)
			VN_HOLD(tvp);
		rfs4_dbe_unlock(fp->dbe);

		if (tvp) {
			va.va_mask = AT_NLINK;
			if (!VOP_GETATTR(tvp, &va, 0, cs->cr, &ct) &&
			    va.va_nlink == 0) {
				/* The file is gone and so should the state */
				if (in_crit_targ) {
					nbl_end_crit(targvp);
					in_crit_targ = 0;
				}
				rfs4_close_all_state(fp);
			}
			VN_RELE(tvp);
		}
	}
	if (error == 0) {
		char *tmp;

		/* fix the path name for the renamed file */
		mutex_enter(&srcvp->v_lock);
		tmp = srcvp->v_path;
		srcvp->v_path = NULL;
		mutex_exit(&srcvp->v_lock);
		vn_setpath(rootdir, ndvp, srcvp, nnm, nlen - 1);
		if (tmp != NULL)
			kmem_free(tmp, strlen(tmp) + 1);
	}

	if (in_crit_src)
		nbl_end_crit(srcvp);
	if (srcvp)
		VN_RELE(srcvp);
	if (in_crit_targ)
		nbl_end_crit(targvp);
	if (targvp)
		VN_RELE(targvp);

	if (sfp) {
		rfs4_clear_dont_grant(cs->instp, sfp);
		rfs4_file_rele(sfp);
	}
	if (fp) {
		rfs4_clear_dont_grant(cs->instp, fp);
		rfs4_file_rele(fp);
	}

	kmem_free(onm, olen);
	kmem_free(nnm, nlen);

	/*
	 * Get the initial "after" sequence number, if it fails, set to zero
	 */
	oidva.va_mask = AT_SEQ;
	if (VOP_GETATTR(odvp, &oidva, 0, cs->cr, &ct))
		oidva.va_seq = 0;

	nidva.va_mask = AT_SEQ;
	if (VOP_GETATTR(ndvp, &nidva, 0, cs->cr, &ct))
		nidva.va_seq = 0;

	/*
	 * Force modified data and metadata out to stable storage.
	 */
	(void) VOP_FSYNC(odvp, 0, cs->cr, &ct);
	(void) VOP_FSYNC(ndvp, 0, cs->cr, &ct);

	if (error) {
		*cs->statusp = resp->status = puterrno4(error);
		goto final;
	}

	/*
	 * Get "after" change values, if it fails, simply return the
	 * before value.
	 */
	oadva.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(odvp, &oadva, 0, cs->cr, &ct)) {
		oadva.va_ctime = obdva.va_ctime;
		oadva.va_seq = 0;
	}

	nadva.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(odvp, &nadva, 0, cs->cr, &ct)) {
		nadva.va_ctime = nbdva.va_ctime;
		nadva.va_seq = 0;
	}

	NFS4_SET_FATTR4_CHANGE(resp->source_cinfo.after, oadva.va_ctime)
	NFS4_SET_FATTR4_CHANGE(resp->target_cinfo.after, nadva.va_ctime)

	/*
	 * The cinfo.atomic = TRUE only if we have
	 * non-zero va_seq's, and it has incremented by exactly one
	 * during the VOP_RENAME and it didn't change during the VOP_FSYNC.
	 */
	if (obdva.va_seq && oidva.va_seq && oadva.va_seq &&
	    oidva.va_seq == (obdva.va_seq + 1) &&
	    oidva.va_seq == oadva.va_seq)
		resp->source_cinfo.atomic = TRUE;
	else
		resp->source_cinfo.atomic = FALSE;

	if (nbdva.va_seq && nidva.va_seq && nadva.va_seq &&
	    nidva.va_seq == (nbdva.va_seq + 1) &&
	    nidva.va_seq == nadva.va_seq)
		resp->target_cinfo.atomic = TRUE;
	else
		resp->target_cinfo.atomic = FALSE;

	*cs->statusp = resp->status = NFS4_OK;
	goto final;

err_out:
	kmem_free(onm, olen);
	kmem_free(nnm, nlen);

	if (in_crit_src) nbl_end_crit(srcvp);
	if (in_crit_targ) nbl_end_crit(targvp);
	if (targvp) VN_RELE(targvp);
	if (srcvp) VN_RELE(srcvp);
	if (sfp) {
		if (sfp_rele_grant_hold) rfs4_clear_dont_grant(cs->instp, sfp);
		rfs4_file_rele(sfp);
	}
	if (fp) {
		if (fp_rele_grant_hold) rfs4_clear_dont_grant(cs->instp, fp);
		rfs4_file_rele(fp);
	}

final:
	DTRACE_NFSV4_2(op__rename__done, struct compound_state *, cs,
	    RENAME4res *, resp);
}


/* ARGSUSED */
static void
mds_op_restorefh(nfs_argop4 *args, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	RESTOREFH4res *resp = &resop->nfs_resop4_u.oprestorefh;

	DTRACE_NFSV4_1(op__restorefh__start, struct compound_state *, cs);

	/* No need to check cs->access - we are not accessing any object */
	if ((cs->saved_vp == NULL) || (cs->saved_fh.nfs_fh4_val == NULL)) {
		*cs->statusp = resp->status = NFS4ERR_RESTOREFH;
		goto final;
	}
	if (cs->vp != NULL) {
		VN_RELE(cs->vp);
	}
	cs->vp = cs->saved_vp;
	cs->saved_vp = NULL;
	cs->exi = cs->saved_exi;
	nfs_fh4_copy(&cs->saved_fh, &cs->fh);
	*cs->statusp = resp->status = NFS4_OK;
	cs->deleg = FALSE;

final:
	DTRACE_NFSV4_2(op__restorefh__done, struct compound_state *, cs,
	    RESTOREFH4res *, resp);
}

/* ARGSUSED */
static void
mds_op_savefh(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	SAVEFH4res *resp = &resop->nfs_resop4_u.opsavefh;

	DTRACE_NFSV4_1(op__savefh__start, struct compound_state *, cs);

	/* No need to check cs->access - we are not accessing any object */
	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}
	if (cs->saved_vp != NULL) {
		VN_RELE(cs->saved_vp);
	}
	cs->saved_vp = cs->vp;
	VN_HOLD(cs->saved_vp);
	cs->saved_exi = cs->exi;
	/*
	 * since SAVEFH is fairly rare, don't alloc space for its fh
	 * unless necessary.
	 */
	if (cs->saved_fh.nfs_fh4_val == NULL) {
		cs->saved_fh.nfs_fh4_val = kmem_alloc(NFS4_FHSIZE, KM_SLEEP);
	}
	nfs_fh4_copy(&cs->fh, &cs->saved_fh);
	*cs->statusp = resp->status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__savefh__done, struct compound_state *, cs,
	    SAVEFH4res *, resp);
}

/* ARGSUSED */
static void
mds_op_setattr(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	SETATTR4args *args = &argop->nfs_argop4_u.opsetattr;
	SETATTR4res *resp = &resop->nfs_resop4_u.opsetattr;
	bslabel_t *clabel;

	DTRACE_NFSV4_2(op__setattr__start, struct compound_state *, cs,
	    SETATTR4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/*
	 * If there is an unshared filesystem mounted on this vnode,
	 * do not allow to setattr on this vnode.
	 */
	if (vn_ismntpt(cs->vp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	resp->attrsset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));

	if (rdonly4(cs->exi, cs->vp, req)) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		goto final;
	}

	/* check label before setting attributes */
	if (is_system_labeled()) {
		ASSERT(req->rq_label != NULL);
		clabel = req->rq_label;
		DTRACE_PROBE2(tx__rfs4__log__info__opsetattr__clabel, char *,
		    "got client label from request(1)",
		    struct svc_req *, req);
		if (!blequal(&l_admin_low->tsl_label, clabel)) {
			if (!do_rfs_label_check(clabel, cs->vp,
			    EQUALITY_CHECK)) {
				*cs->statusp = resp->status = NFS4ERR_ACCESS;
				goto final;
			}
		}
	}

	*cs->statusp = resp->status =
	    mds_setattr(&resp->attrsset, &args->obj_attributes, cs,
	    &args->stateid);

final:
	DTRACE_NFSV4_2(op__setattr__done, struct compound_state *, cs,
	    SETATTR4res *, resp);
}


/* ARGSUSED */
static void
mds_op_write(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *req,
	compound_state_t *cs)
{
	WRITE4args  *args = &argop->nfs_argop4_u.opwrite;
	WRITE4res *resp = &resop->nfs_resop4_u.opwrite;
	nnode_io_flags_t nnioflags = NNODE_IO_FLAG_WRITE;
	int error;
	nnode_t *nn;
	u_offset_t rlimit;
	struct uio uio;
	struct iovec iov[NFS_MAX_IOVECS];
	struct iovec *iovp = iov;
	int iovcnt;
	int ioflag;
	cred_t *savecred, *cr;
	bool_t *deleg = &cs->deleg;
	nfsstat4 stat;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__write__start, struct compound_state *, cs,
	    WRITE4args *, args);

	nn = cs->nn;
	if (nn == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}
	/*
	 * cs->access is set in call_checkauth4 called in putfh code.  The
	 * current putfh code will not invoke these security functions on the
	 * DS codepath since it goes by the filehandle, not by nnodes per se.
	 */
	if (cs->access == CS_ACCESS_DENIED) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto final;
	}

	cr = cs->cr;
	if ((cs->vp != NULL) && (rdonly4(cs->exi, cs->vp, req))) {
		*cs->statusp = resp->status = NFS4ERR_ROFS;
		goto final;
	}

	if ((stat = nnop_check_stateid(nn, cs, FWRITE, &args->stateid, FALSE,
	    deleg, TRUE, &ct, NULL)) != NFS4_OK) {
		*cs->statusp = resp->status = stat;
		goto out;
	}

	error = nnop_io_prep(nn, &nnioflags, cr, &ct, args->offset,
	    args->data_len, NULL);
	if (error != 0)
		goto err;

	if (args->data_len == 0) {
		*cs->statusp = resp->status = NFS4_OK;
		resp->count = 0;
		resp->committed = args->stable;
		resp->writeverf = cs->instp->Write4verf;
		goto out;
	}

	if (args->mblk != NULL) {
		mblk_t *m;
		uint_t bytes, round_len;

		iovcnt = 0;
		bytes = 0;
		round_len = roundup(args->data_len, BYTES_PER_XDR_UNIT);
		for (m = args->mblk;
		    m != NULL && bytes < round_len;
		    m = m->b_cont) {
			iovcnt++;
			bytes += MBLKL(m);
		}
#ifdef DEBUG
		/* should have ended on an mblk boundary */
		if (bytes != round_len) {
			printf("bytes=0x%x, round_len=0x%x, req len=0x%x\n",
			    bytes, round_len, args->data_len);
			printf("args=%p, args->mblk=%p, m=%p", (void *)args,
			    (void *)args->mblk, (void *)m);
			ASSERT(bytes == round_len);
		}
#endif
		if (iovcnt <= NFS_MAX_IOVECS) {
			iovp = iov;
		} else {
			iovp = kmem_alloc(sizeof (*iovp) * iovcnt, KM_SLEEP);
		}
		mblk_to_iov(args->mblk, iovcnt, iovp);
	} else {
		iovcnt = 1;
		iovp = iov;
		iovp->iov_base = args->data_val;
		iovp->iov_len = args->data_len;
	}

	uio.uio_iov = iovp;
	uio.uio_iovcnt = iovcnt;

	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_extflg = UIO_COPY_DEFAULT;
	uio.uio_loffset = args->offset;
	uio.uio_resid = args->data_len;
	uio.uio_llimit = curproc->p_fsz_ctl;
	rlimit = uio.uio_llimit - args->offset;
	if (rlimit < (u_offset_t)uio.uio_resid)
		uio.uio_resid = (int)rlimit;

	if (args->stable == UNSTABLE4)
		ioflag = 0;
	else if (args->stable == FILE_SYNC4)
		ioflag = FSYNC;
	else if (args->stable == DATA_SYNC4)
		ioflag = FDSYNC;
	else {
		if (iovp != iov)
			kmem_free(iovp, sizeof (*iovp) * iovcnt);
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * We're changing creds because VM may fault and we need
	 * the cred of the current thread to be used if quota
	 * checking is enabled.
	 */
	savecred = curthread->t_cred;
	curthread->t_cred = cr;
	error = nnop_write(nn, &nnioflags, &uio, ioflag, cr, &ct, NULL);
	curthread->t_cred = savecred;

	if (iovp != iov)
		kmem_free(iovp, sizeof (*iovp) * iovcnt);

err:
	if (error) {
		*cs->statusp = resp->status = nnode_stat4(error, 1);
		goto out;
	}

	*cs->statusp = resp->status = NFS4_OK;
	resp->count = args->data_len - uio.uio_resid;

	if (ioflag == 0)
		resp->committed = UNSTABLE4;
	else
		resp->committed = FILE_SYNC4;

	resp->writeverf = cs->instp->Write4verf;

out:
	nnop_io_release(nn, nnioflags, &ct);

final:
	DTRACE_NFSV4_2(op__write__done, struct compound_state *, cs,
	    WRITE4res *, resp);
}

static void
rfs41_op_dispatch(compound_state_t *cs,
    COMPOUND4args *args, COMPOUND4res *resp, struct svc_req *req)
{
	nfs_argop4		*argop;
	nfs_resop4		*resop;
	uint_t			 op;

	argop = &args->array[cs->op_ndx];
	resop = &resp->array[cs->op_ndx];

	op = (uint_t)argop->argop;
	resop->resop = op;

	if (op >= OP_ILLEGAL_IDX) {
		/*
		 * This is effectively dead code since XDR code
		 * will have already returned BADXDR if op doesn't
		 * decode to legal value.  This only done for a
		 * day when XDR code doesn't verify v4 opcodes.
		 * or some bozo didn't update the operation dispatch
		 * table.
		 */
		rfsproccnt_v4_ptr[OP_ILLEGAL_IDX].value.ui64++;

		mds_op_illegal(argop, resop, req, cs);
		DTRACE_PROBE(nfss41__e__operation_tilt);
		goto bail;
	}

	/*
	 * First if this is a bad operation stop
	 * the compound processing right now!
	 */
	if (mds_disptab[op].op_flag == DISP_OP_BAD) {
		mds_op_illegal(argop, resop, req, cs);
		DTRACE_PROBE1(nfss41__e__disp_op_inval, int, op);
		goto bail;
	}

	(*mds_disptab[op].dis_op)(argop, resop, req, cs);

bail:
	if (*cs->statusp != NFS4_OK)
		cs->cont = FALSE;

	/*
	 * If not at last op, and if we are to stop, then
	 * compact the results array.
	 */
	if ((cs->op_ndx + 1) < cs->op_len && !cs->cont) {
		nfs_resop4 *new_res = kmem_alloc(
		    (cs->op_ndx+1) * sizeof (nfs_resop4), KM_SLEEP);
		bcopy(resp->array,
		    new_res, (cs->op_ndx+1) * sizeof (nfs_resop4));
		kmem_free(resp->array,
		    cs->op_len * sizeof (nfs_resop4));

		resp->array_len =  cs->op_ndx + 1;
		resp->array = new_res;
	}
}

void
rfs41_err_resp(COMPOUND4args *args, COMPOUND4res *resp, nfsstat4 err)
{
	size_t	sz;

	resp->array_len = 1;
	sz = resp->array_len * sizeof (nfs_resop4);
	resp->array = kmem_zalloc(sz, KM_SLEEP);

	resp->array[0].resop = args->array[0].argop;
	resp->array[0].nfs_resop4_u.opillegal.status = err;
}


/* ARGSUSED */
void
mds_compound(compound_state_t *cs,
    COMPOUND4args *args, COMPOUND4res *resp, struct exportinfo *exi,
    struct svc_req *req, int *rv)
{
	cred_t *cr;
	size_t	reslen;

	if (rv != NULL)
		*rv = 0;
	/*
	 * Form a reply tag by copying over the reqeuest tag.
	 */
	resp->tag.utf8string_val =
	    kmem_alloc(args->tag.utf8string_len, KM_SLEEP);

	resp->tag.utf8string_len = args->tag.utf8string_len;

	bcopy(args->tag.utf8string_val, resp->tag.utf8string_val,
	    resp->tag.utf8string_len);

	ASSERT(exi == NULL);

	cr = crget();
	ASSERT(cr != NULL);

	if (sec_svc_getcred(req, cr, &cs->principal, &cs->nfsflavor) == 0) {

		DTRACE_NFSV4_2(compound__start,
		    struct compound_state *,
		    &cs, COMPOUND4args *, args);

		crfree(cr);

		DTRACE_NFSV4_2(compound__done,
		    struct compound_state *,
		    &cs, COMPOUND4res *, resp);

		svcerr_badcred(req->rq_xprt);
		if (rv != NULL)
			*rv = 1;
		return;
	}
	cs->basecr = cr;
	cs->req = req;

	DTRACE_NFSV4_2(compound__start, struct compound_state *, &cs,
	    COMPOUND4args *, args);

	/*
	 * For now, NFS4 compound processing must be protected by
	 * exported_lock because it can access more than one exportinfo
	 * per compound and share/unshare can now change multiple
	 * exinfo structs.  The NFS2/3 code only refs 1 exportinfo
	 * per proc (excluding public exinfo), and exi_count design
	 * is sufficient to protect concurrent execution of NFS2/3
	 * ops along with unexport.
	 */
	rw_enter(&exported_lock, RW_READER);

	/*
	 * If this is the first compound we've seen, we need to start
	 * the instances' grace period.
	 */
	if (cs->instp->seen_first_compound == 0) {
		rfs4_grace_start_new(cs->instp);
		cs->instp->seen_first_compound = 1;
	}

	/*
	 * Any operations _other_ than the ones listed below, should _not_
	 * appear as the first operation in a compound. If so we will
	 * error out. We use the opilleagal.status without regard to
	 * the actual operation since we know that status always appears
	 * as the first element for all the operations.
	 */
	switch (args->array[0].argop) {
	case OP_SEQUENCE:
	case OP_EXCHANGE_ID:
	case OP_CREATE_SESSION:
	case OP_DESTROY_SESSION:
		break;

	case OP_BIND_CONN_TO_SESSION:
		/*
		 * Should be the _only_ op in compound
		 */
		if (args->array_len != 1) {
			*cs->statusp = NFS4ERR_NOT_ONLY_OP;
			rfs41_err_resp(args, resp, *cs->statusp);
			goto out;
		}
		break;

	default:
		*cs->statusp = NFS4ERR_OP_NOT_IN_SESSION;
		rfs41_err_resp(args, resp, *cs->statusp);
		goto out;
	}

	/*
	 * Everything kosher; allocate results array
	 */
	reslen = cs->op_len = resp->array_len = args->array_len;
	resp->array = kmem_zalloc(reslen * sizeof (nfs_resop4), KM_SLEEP);

	/*
	 * Iterate over the compound until we have exhausted the operations
	 * or the compound state indicates that we should terminate.
	 */
	for (cs->op_ndx = 0;
	    cs->op_ndx < cs->op_len && cs->cont == TRUE; cs->op_ndx++)
		rfs41_op_dispatch(cs, args, resp, req);

out:
	rw_exit(&exported_lock);

	/*
	 * done with this compound request, free the label
	 */

	if (req->rq_label != NULL) {
		kmem_free(req->rq_label, sizeof (bslabel_t));
		req->rq_label = NULL;
	}

	DTRACE_NFSV4_2(compound__done, struct compound_state *, &cs,
	    COMPOUND4res *, resp);
}

/*
 * XXX: ?? is this tracked by an issue number i wonder ??
 *
 * XXX because of what appears to be duplicate calls to rfs4_compound_free
 * XXX zero out the tag and array values. Need to investigate why the
 * XXX calls occur, but at least prevent the panic for now.
 */
void
rfs41_compound_free(COMPOUND4res *resp, compound_state_t *cs)
{
	uint_t i;

	if (resp->tag.utf8string_val) {
		UTF8STRING_FREE(resp->tag)
	}

	for (i = 0; i < resp->array_len; i++) {
		nfs_resop4 *resop;
		uint_t op;

		resop = &resp->array[i];
		op = (uint_t)resop->resop;
		if (op < OP_ILLEGAL_IDX) {
			(*mds_disptab[op].dis_resfree)(resop, cs);
		}
	}

	if (resp->array != NULL) {
		kmem_free(resp->array, resp->array_len * sizeof (nfs_resop4));
	}
}

delegreq_t
do_41_deleg_hack(int osa)
{
	int want_deleg;

	want_deleg = (osa & OPEN4_SHARE_ACCESS_WANT_DELEG_MASK);

	switch (want_deleg) {
	case OPEN4_SHARE_ACCESS_WANT_READ_DELEG:
		return (DELEG_READ);

	case OPEN4_SHARE_ACCESS_WANT_WRITE_DELEG:
		return (DELEG_WRITE);

	case OPEN4_SHARE_ACCESS_WANT_ANY_DELEG:
		return (DELEG_ANY);

	case OPEN4_SHARE_ACCESS_WANT_NO_DELEG:
		return (DELEG_NONE);
	}
	return (DELEG_ANY);
}

/*
 * this could be a handy function to call out to the SPE in.. :-D
 */
nfsstat4
mds_createfile(OPEN4args *args, struct svc_req *req, struct compound_state *cs,
    change_info4 *cinfo, attrmap4 *attrset)
{
	struct nfs4_svgetit_arg sarg;
	struct nfs4_ntov_table ntov;

	bool_t ntov_table_init = FALSE;
	struct statvfs64 sb;
	nfsstat4 status;
	vnode_t *vp;
	vattr_t bva, ava, iva, cva, *vap;
	vnode_t *dvp;
	timespec32_t *mtime;
	char *nm = NULL;
	uint_t buflen;
	bool_t created;
	bool_t setsize = FALSE;
	len_t reqsize;
	int error;
	bool_t trunc;
	caller_context_t ct;
	component4 *component;
	bslabel_t *clabel;
	attrvers_t avers;

	avers = RFS4_ATTRVERS(cs);
	sarg.sbp = &sb;
	dvp = cs->vp;

	/* Check if the file system is read only */
	if (rdonly4(cs->exi, dvp, req))
		return (NFS4ERR_ROFS);

	/* check the label of including directory */
	if (is_system_labeled()) {
		ASSERT(req->rq_label != NULL);
		clabel = req->rq_label;
		DTRACE_PROBE2(tx__rfs4__log__info__opremove__clabel, char *,
		    "got client label from request(1)",
		    struct svc_req *, req);
		if (!blequal(&l_admin_low->tsl_label, clabel)) {
			if (!do_rfs_label_check(clabel, dvp, EQUALITY_CHECK)) {
				return (NFS4ERR_ACCESS);
			}
		}
	}

	/*
	 * Get the last component of path name in nm. cs will reference
	 * the including directory on success.
	 */
	component = &args->open_claim4_u.file;
	if (!utf8_dir_verify(component))
		return (NFS4ERR_INVAL);

	nm = utf8_to_fn(component, &buflen, NULL);

	if (nm == NULL)
		return (NFS4ERR_RESOURCE);

	if (buflen > MAXNAMELEN) {
		kmem_free(nm, buflen);
		return (NFS4ERR_NAMETOOLONG);
	}

	bva.va_mask = AT_TYPE|AT_CTIME|AT_SEQ;
	error = VOP_GETATTR(dvp, &bva, 0, cs->cr, NULL);
	if (error) {
		kmem_free(nm, buflen);
		return (puterrno4(error));
	}

	if (bva.va_type != VDIR) {
		kmem_free(nm, buflen);
		return (NFS4ERR_NOTDIR);
	}

	NFS4_SET_FATTR4_CHANGE(cinfo->before, bva.va_ctime)

	switch (args->mode) {
	case GUARDED4:
		/*FALLTHROUGH*/
	case UNCHECKED4:
		nfs4_ntov_table_init(&ntov, avers);
		ntov_table_init = TRUE;

		*attrset = NFS4_EMPTY_ATTRMAP(avers);
		status = do_rfs4_set_attrs(attrset,
		    &args->createhow4_u.createattrs,
		    cs, &sarg, &ntov, NFS4ATTR_SETIT);

		if (status == NFS4_OK && (sarg.vap->va_mask & AT_TYPE) &&
		    sarg.vap->va_type != VREG) {
			if (sarg.vap->va_type == VDIR)
				status = NFS4ERR_ISDIR;
			else if (sarg.vap->va_type == VLNK)
				status = NFS4ERR_SYMLINK;
			else
				status = NFS4ERR_INVAL;
		}

		if (status != NFS4_OK) {
			kmem_free(nm, buflen);
			nfs4_ntov_table_free(&ntov, &sarg);
			*attrset = NFS4_EMPTY_ATTRMAP(avers);
			return (status);
		}

		vap = sarg.vap;
		vap->va_type = VREG;
		vap->va_mask |= AT_TYPE;

		if ((vap->va_mask & AT_MODE) == 0) {
			vap->va_mask |= AT_MODE;
			vap->va_mode = (mode_t)0600;
		}

		if (vap->va_mask & AT_SIZE) {

			/* Disallow create with a non-zero size */

			if ((reqsize = sarg.vap->va_size) != 0) {
				kmem_free(nm, buflen);
				nfs4_ntov_table_free(&ntov, &sarg);
				*attrset = NFS4_EMPTY_ATTRMAP(avers);
				return (NFS4ERR_INVAL);
			}
			setsize = TRUE;
		}
		break;

	case EXCLUSIVE4:
		/* prohibit EXCL create of named attributes */
		if (dvp->v_flag & V_XATTRDIR) {
			kmem_free(nm, buflen);
			*attrset = NFS4_EMPTY_ATTRMAP(avers);
			return (NFS4ERR_INVAL);
		}

		cva.va_mask = AT_TYPE | AT_MTIME | AT_MODE;
		cva.va_type = VREG;
		/*
		 * Ensure no time overflows. Assumes underlying
		 * filesystem supports at least 32 bits.
		 * Truncate nsec to usec resolution to allow valid
		 * compares even if the underlying filesystem truncates.
		 */
		mtime = (timespec32_t *)&args->createhow4_u.createverf;
		cva.va_mtime.tv_sec = mtime->tv_sec % TIME32_MAX;
		cva.va_mtime.tv_nsec = (mtime->tv_nsec / 1000) * 1000;
		cva.va_mode = (mode_t)0;
		vap = &cva;
		break;

	case EXCLUSIVE4_1:
		kmem_free(nm, buflen);
		*attrset = NFS4_EMPTY_ATTRMAP(avers);
		return (NFS4ERR_INVAL);
	}

	status = create_vnode(dvp, nm, vap, args->mode, mtime,
	    cs->cr, &vp, &created);
	kmem_free(nm, buflen);

	if (status != NFS4_OK) {
		if (ntov_table_init)
			nfs4_ntov_table_free(&ntov, &sarg);
		*attrset = NFS4_EMPTY_ATTRMAP(avers);
		return (status);
	}

	trunc = (setsize && !created);

	if (args->mode != EXCLUSIVE4) {
		attrmap4 createmask = args->createhow4_u.createattrs.attrmask;

		/*
		 * True verification that object was created with correct
		 * attrs is impossible.  The attrs could have been changed
		 * immediately after object creation.  If attributes did
		 * not verify, the only recourse for the server is to
		 * destroy the object.  Maybe if some attrs (like gid)
		 * are set incorrectly, the object should be destroyed;
		 * however, seems bad as a default policy.  Do we really
		 * want to destroy an object over one of the times not
		 * verifying correctly?  For these reasons, the server
		 * currently sets bits in attrset for createattrs
		 * that were set; however, no verification is done.
		 *
		 * vmask_to_nmask accounts for vattr bits set on create
		 *	[do_rfs4_set_attrs() only sets resp bits for
		 *	 non-vattr/vfs bits.]
		 * Mask off any bits we set by default so as not to return
		 * more attrset bits than were requested in createattrs
		 */
		if (created) {
			nfs4_vmask_to_nmask(sarg.vap->va_mask, attrset, avers);
			ATTRMAP_MASK(*attrset, createmask);
		} else {
			/*
			 * We did not create the vnode (we tried but it
			 * already existed).  In this case, the only createattr
			 * that the spec allows the server to set is size,
			 * and even then, it can only be set if it is 0.
			 */
			*attrset = NFS4_EMPTY_ATTRMAP(avers);
			if (trunc)
				ATTR_SET(*attrset, SIZE);
		}
	}
	if (ntov_table_init)
		nfs4_ntov_table_free(&ntov, &sarg);

	/*
	 * Get the initial "after" sequence number, if it fails,
	 * set to zero, time to before.
	 */
	iva.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(dvp, &iva, 0, cs->cr, NULL)) {
		iva.va_seq = 0;
		iva.va_ctime = bva.va_ctime;
	}

	/*
	 * create_vnode attempts to create the file exclusive,
	 * if it already exists the VOP_CREATE will fail and
	 * may not increase va_seq. It is atomic if
	 * we haven't changed the directory, but if it has changed
	 * we don't know what changed it.
	 */
	if (!created) {
		if (bva.va_seq && iva.va_seq &&
		    bva.va_seq == iva.va_seq)
			cinfo->atomic = TRUE;
		else
			cinfo->atomic = FALSE;
		NFS4_SET_FATTR4_CHANGE(cinfo->after, iva.va_ctime);
	} else {
		/*
		 * The entry was created, we need to sync the
		 * directory metadata.
		 */
		(void) VOP_FSYNC(dvp, 0, cs->cr, NULL);

		/*
		 * Get "after" change value, if it fails, simply return the
		 * before value.
		 */
		ava.va_mask = AT_CTIME|AT_SEQ;
		if (VOP_GETATTR(dvp, &ava, 0, cs->cr, NULL)) {
			ava.va_ctime = bva.va_ctime;
			ava.va_seq = 0;
		}

		NFS4_SET_FATTR4_CHANGE(cinfo->after, ava.va_ctime);

		/*
		 * The cinfo->atomic = TRUE only if we have
		 * non-zero va_seq's, and it has incremented by exactly one
		 * during the create_vnode and it didn't
		 * change during the VOP_FSYNC.
		 */
		if (bva.va_seq && iva.va_seq && ava.va_seq &&
		    iva.va_seq == (bva.va_seq + 1) && iva.va_seq == ava.va_seq)
			cinfo->atomic = TRUE;
		else
			cinfo->atomic = FALSE;
	}

	/* Check for mandatory locking and that the size gets set. */
	cva.va_mask = AT_MODE;
	if (setsize)
		cva.va_mask |= AT_SIZE;

	/* Assume the worst */
	cs->mandlock = TRUE;

	if (VOP_GETATTR(vp, &cva, 0, cs->cr, NULL) == 0) {
		cs->mandlock = MANDLOCK(cs->vp, cva.va_mode);

		/*
		 * Truncate the file if necessary; this would be
		 * the case for create over an existing file.
		 */

		if (trunc) {
			int in_crit = 0;
			rfs4_file_t *fp;
			bool_t create = FALSE;

			/*
			 * We are writing over an existing file.
			 * Check to see if we need to recall a delegation.
			 */
			rfs4_hold_deleg_policy(cs->instp);
			if ((fp = rfs4_findfile(cs->instp, vp, NULL, &create))
			    != NULL) {
				if (rfs4_check_delegated_byfp(cs->instp,
				    FWRITE, fp, (reqsize == 0), FALSE, FALSE,
				    &cs->cp->clientid)) {

					rfs4_file_rele(fp);
					rfs4_rele_deleg_policy(cs->instp);
					VN_RELE(vp);
					*attrset = NFS4_EMPTY_ATTRMAP(avers);
					return (NFS4ERR_DELAY);
				}
				rfs4_file_rele(fp);
			}
			rfs4_rele_deleg_policy(cs->instp);

			if (nbl_need_check(vp)) {
				in_crit = 1;

				ASSERT(reqsize == 0);

				nbl_start_crit(vp, RW_READER);
				if (nbl_conflict(vp, NBL_WRITE, 0,
				    cva.va_size, 0, NULL)) {
					in_crit = 0;
					nbl_end_crit(vp);
					VN_RELE(vp);
					*attrset = NFS4_EMPTY_ATTRMAP(avers);
					return (NFS4ERR_ACCESS);
				}
			}
			ct.cc_sysid = 0;
			ct.cc_pid = 0;
			ct.cc_caller_id = cs->instp->caller_id;

			cva.va_mask = AT_SIZE;
			cva.va_size = reqsize;
			(void) VOP_SETATTR(vp, &cva, 0, cs->cr, &ct);
			if (in_crit)
				nbl_end_crit(vp);
		}
	}

	error = mknfs41_fh(&cs->fh, vp, cs->exi);
	/*
	 * Force modified data and metadata out to stable storage.
	 */
	(void) VOP_FSYNC(vp, FNODSYNC, cs->cr, NULL);

	if (error) {
		VN_RELE(vp);
		*attrset = NFS4_EMPTY_ATTRMAP(avers);
		return (puterrno4(error));
	}

	/* if parent dir is attrdir, set namedattr fh flag */
	if (dvp->v_flag & V_XATTRDIR)
		FH41_SET_FLAG((nfs41_fh_fmt_t *)cs->fh.nfs_fh4_val,
		    FH41_NAMEDATTR);

	if (cs->vp)
		VN_RELE(cs->vp);

	cs->vp = vp;

	/*
	 * if we did not create the file, we will need to check
	 * the access bits on the file
	 */

	if (!created) {
		if (setsize)
			args->share_access |= OPEN4_SHARE_ACCESS_WRITE;
		status = check_open_access(args->share_access, cs, req);
		if (status != NFS4_OK)
			*attrset = NFS4_EMPTY_ATTRMAP(avers);
	}
	return (status);
}

/*
 * 1) CB RACE           <kill stored rs record>		[done]
 * 2) slot reuse	<kill stored rs record>		[done]
 * 3) CB_RECALL		<(typical/normal case) use new sessid in deleg_state
 *			 to find session originally granted the delegation to
 *			 issue recall over _that_ session's back channel>
 *			XXX - <<< check spec >>>
 */
void
rfs41_rs_record(struct compound_state *cs, stateid_type_t type, void *p)
{
	rfs4_deleg_state_t	*dsp;
	slot41_t		*slp;
#ifdef	DEBUG_VERBOSE
	/*
	 * XXX - Do not change this to a static D probe;
	 *	 this is not intended for production !!!
	 */
	ulong_t			 offset;
	char			*who;
	who = modgetsymname((uintptr_t)caller(), &offset);
#endif	/* DEBUG_VERBOSE */

	switch (type) {
	case DELEGID:			/* sessid/slot/seqid + rsid */
		ASSERT(cs != NULL && cs->sp != NULL);

		dsp = (rfs4_deleg_state_t *)p;
		ASSERT(dsp != NULL);
#ifdef	DEBUG_VERBOSE
		cmn_err(CE_NOTE, "rfs41_rs_record: (%s, dsp = 0x%p)", who, dsp);
#endif	/* DEBUG_VERBOSE */

		/* delegation state id stored in rfs4_deleg_state_t */
		bcopy(cs->sp->sn_sessid, dsp->rs.sessid, sizeof (sessionid4));
		dsp->rs.seqid = cs->seqid;
		dsp->rs.slotno = cs->slotno;
		rfs41_deleg_rs_hold(dsp);

		/* add it to slrc slot to track slot-reuse case */
		slp = &cs->sp->sn_slrc->sc_slot[cs->slotno];
		ASSERT(slp != NULL);
		ASSERT(slp->p == NULL);
		slp->p = (rfs4_deleg_state_t *)dsp;
		rfs4_dbe_hold(dsp->dbe);	/* added ref to deleg_state */
		break;

	case LAYOUTID:
		/*
		 * Layout stateid race detection will be done
		 * using the stateid's embedded seqid field.
		 */
		/* FALLTHROUGH */
	default:
		break;
	}
}

void
rfs41_rs_erase(void *p)
{
	rfs4_deleg_state_t	*dsp = (rfs4_deleg_state_t *)p;
#ifdef	DEBUG_VERBOSE
	/*
	 * XXX - Do not change this to a static D probe;
	 *	 this is not intended for production !!!
	 */
	ulong_t			 offset;
	char			*who;
	who = modgetsymname((uintptr_t)caller(), &offset);
	cmn_err(CE_NOTE, "rfs41_rs_erase: (%s, dsp = 0x%p)", who, dsp);
#endif	/* DEBUG_VERBOSE */

	ASSERT(dsp != NULL);
	rfs41_deleg_rs_rele(dsp);
	rfs4_deleg_state_rele(dsp);
}

#ifdef	DEBUG
/*
 * XXX - This is a handy way to force the server to "wait" before
 *	granting a delegation to the requesting client (thereby
 *	forcing the CB_RACE condition). rsec == # of secs to wait.
 */
int	rsec = 0;
#endif

/*ARGSUSED*/
static void
mds_do_open(struct compound_state *cs, struct svc_req *req,
		rfs4_openowner_t *oo, delegreq_t deleg,
		uint32_t access, uint32_t deny,
		OPEN4res *resp, int deleg_cur)
{
	rfs4_state_t *state;
	rfs4_file_t *file;
	bool_t screate = TRUE;
	bool_t fcreate = TRUE;
	uint32_t amodes;
	uint32_t dmodes;
	rfs4_deleg_state_t *dsp;
	struct shrlock shr;
	struct shr_locowner shr_loco;
	sysid_t sysid;
	nfsstat4 status;
	caller_context_t ct;
	int fflags = 0;
	int recall = 0;
	int err;

	/* get the file struct and hold a lock on it during initial open */
	file = rfs4_findfile_withlock(cs->instp, cs->vp, &cs->fh, &fcreate);
	if (file == NULL) {
		DTRACE_PROBE(nfss__e__no_file);
		resp->status = NFS4ERR_SERVERFAULT;
		return;
	}

	state = rfs4_findstate_by_owner_file(cs, oo, file, &screate);
	if (state == NULL) {
		DTRACE_PROBE(nfss__e__no_state);
		resp->status = NFS4ERR_RESOURCE;
		/* No need to keep any reference */
		rfs4_file_rele_withunlock(file);
		return;
	}

	/* try to get the sysid before continuing */
	if ((status = rfs4_client_sysid(oo->client, &sysid)) != NFS4_OK) {
		resp->status = status;
		rfs4_file_rele(file);
		/* Not a fully formed open; "close" it */
		if (screate == TRUE)
			rfs4_state_close(state, FALSE, FALSE, cs->cr);
		rfs4_state_rele(state);
		return;
	}

	/* Calculate the fflags for this OPEN */
	if (access & OPEN4_SHARE_ACCESS_READ)
		fflags |= FREAD;
	if (access & OPEN4_SHARE_ACCESS_WRITE)
		fflags |= FWRITE;

	/*
	 * Calculate the new deny and access mode that this open is adding to
	 * the file for this open owner;
	 */
	dmodes = (deny & ~state->share_deny);
	amodes = (access & ~state->share_access);

	/*
	 * Check to see the client has already sent an open for this
	 * open owner on this file with the same share/deny modes.
	 * If so, we don't need to check for a conflict and we don't
	 * need to add another shrlock.  If not, then we need to
	 * check for conflicts in deny and access before checking for
	 * conflicts in delegation.  We don't want to recall a
	 * delegation based on an open that will eventually fail based
	 * on shares modes.
	 */

	if (dmodes || amodes) {
		shr.s_access = (short)access;
		shr.s_deny = (short)deny;
		shr.s_pid = rfs4_dbe_getid(oo->dbe);
		shr.s_sysid = sysid;
		shr_loco.sl_pid = shr.s_pid;
		shr_loco.sl_id = shr.s_sysid;
		shr.s_owner = (caddr_t)&shr_loco;
		shr.s_own_len = sizeof (shr_loco);

		if ((err = vop_shrlock(cs->vp, F_SHARE, &shr, fflags)) != 0) {

			resp->status = err == EAGAIN ?
			    NFS4ERR_SHARE_DENIED : puterrno4(err);

			rfs4_file_rele(file);
			/* Not a fully formed open; "close" it */
			if (screate == TRUE)
				rfs4_state_close(state, FALSE, FALSE, cs->cr);
			rfs4_state_rele(state);
			return;
		}
	}

	rfs4_dbe_lock(state->dbe);
	rfs4_dbe_lock(file->dbe);

	/*
	 * Check to see if this file is delegated and if so, if a
	 * recall needs to be done.
	 */
	if (rfs4_check_recall(state, access)) {
		rfs4_dbe_unlock(file->dbe);
		rfs4_dbe_unlock(state->dbe);
		rfs4_recall_deleg(file, FALSE, state->owner->client);
		delay(NFS4_DELEGATION_CONFLICT_DELAY);
		rfs4_dbe_lock(state->dbe);
		rfs4_dbe_lock(file->dbe);
		/* Let's see if the delegation was returned */
		if (rfs4_check_recall(state, access)) {
			rfs4_dbe_unlock(file->dbe);
			rfs4_dbe_unlock(state->dbe);
			rfs4_file_rele(file);
			rfs4_update_lease(state->owner->client);
			(void) vop_shrlock(cs->vp, F_UNSHARE, &shr, fflags);
			/* Not a fully formed open; "close" it */
			if (screate == TRUE)
				rfs4_state_close(state, FALSE, FALSE, cs->cr);
			rfs4_state_rele(state);
			resp->status = NFS4ERR_DELAY;
			return;
		}
	}

	/*
	 * the share check passed and any delegation conflict has been
	 * taken care of, now call vop_open.
	 * if this is the first open then call vop_open with fflags.
	 * if not, call vn_open_upgrade with just the upgrade flags.
	 *
	 * if the file has been opened already, it will have the current
	 * access mode in the state struct.  if it has no share access, then
	 * this is a new open.
	 *
	 * However, if this is open with CLAIM_DELEGATE_CUR, then don't
	 * call VOP_OPEN(), just do the open upgrade.
	 */
	if (((state->share_access & OPEN4_SHARE_ACCESS_BOTH) == 0) &&
	    !deleg_cur) {
		ct.cc_sysid = sysid;
		ct.cc_pid = shr.s_pid;
		ct.cc_caller_id = cs->instp->caller_id;
		ct.cc_flags = CC_DONTBLOCK;
		err = VOP_OPEN(&cs->vp, fflags, cs->cr, &ct);
		if (err) {
			rfs4_dbe_unlock(file->dbe);
			rfs4_dbe_unlock(state->dbe);
			rfs4_file_rele(file);
			(void) vop_shrlock(cs->vp, F_UNSHARE, &shr, fflags);
			/* Not a fully formed open; "close" it */
			if (screate == TRUE)
				rfs4_state_close(state, FALSE, FALSE, cs->cr);
			rfs4_state_rele(state);
			if (err == EAGAIN && (ct.cc_flags & CC_WOULDBLOCK))
				resp->status = NFS4ERR_DELAY;
			else
				resp->status = NFS4ERR_SERVERFAULT;
			return;
		}
	} else { /* open upgrade */
		/*
		 * calculate the fflags for the new mode that is being added
		 * by this upgrade.
		 */
		fflags = 0;
		if (amodes & OPEN4_SHARE_ACCESS_READ)
				fflags |= FREAD;
		if (amodes & OPEN4_SHARE_ACCESS_WRITE)
				fflags |= FWRITE;
		vn_open_upgrade(cs->vp, fflags);
	}

	if (dmodes & OPEN4_SHARE_DENY_READ)
		file->deny_read++;
	if (dmodes & OPEN4_SHARE_DENY_WRITE)
		file->deny_write++;
	file->share_deny |= deny;
	state->share_deny |= deny;

	if (amodes & OPEN4_SHARE_ACCESS_READ)
		file->access_read++;
	if (amodes & OPEN4_SHARE_ACCESS_WRITE)
		file->access_write++;
	file->share_access |= access;
	state->share_access |= access;

	/*
	 * Check for delegation here. if the deleg argument is not
	 * DELEG_ANY, then this is a reclaim from a client and
	 * we must honor the delegation requested. If necessary we can
	 * set the recall flag.
	 */
	dsp = rfs4_grant_delegation(cs, deleg, state, &recall);

	cs->deleg = (file->dinfo->dtype == OPEN_DELEGATE_WRITE);

	next_stateid(&state->stateid);

	resp->stateid = state->stateid.stateid;

	rfs4_dbe_unlock(file->dbe);
	rfs4_dbe_unlock(state->dbe);

	if (dsp) {
		rfs4_set_deleg_response(dsp, &resp->delegation, NULL, recall);
		rfs41_rs_record(cs, DELEGID, dsp);
		rfs4_deleg_state_rele(dsp);
#ifdef	DEBUG
		if (rsec) {
			/* add delay here to force CB_RACE; rick */
			delay(SEC_TO_TICK(rsec));
		}
#endif
	}

	rfs4_file_rele(file);
	rfs4_state_rele(state);

	resp->status = NFS4_OK;
}

nfsstat4
mds_lookupfile(component4 *component, struct svc_req *req,
		struct compound_state *cs, uint32_t access,
		change_info4 *cinfo)
{
	nfsstat4 status;
	char *nm;
	uint32_t len;
	vnode_t *dvp = cs->vp;
	vattr_t bva, ava, fva;
	int error;

	if (dvp == NULL) {
		return (NFS4ERR_NOFILEHANDLE);
	}

	if (dvp->v_type != VDIR) {
		return (NFS4ERR_NOTDIR);
	}

	if (!utf8_dir_verify(component))
		return (NFS4ERR_INVAL);

	nm = utf8_to_fn(component, &len, NULL);
	if (nm == NULL) {
		return (NFS4ERR_INVAL);
	}

	if (len > MAXNAMELEN) {
		kmem_free(nm, len);
		return (NFS4ERR_NAMETOOLONG);
	}

	/* Get "before" change value */
	bva.va_mask = AT_CTIME|AT_SEQ;
	error = VOP_GETATTR(dvp, &bva, 0, cs->cr, NULL);
	if (error)
		return (puterrno4(error));

	/* mds_lookup may VN_RELE directory */
	VN_HOLD(dvp);

	status = mds_do_lookup(nm, len, req, cs);

	kmem_free(nm, len);

	if (status != NFS4_OK) {
		VN_RELE(dvp);
		return (status);
	}

	/*
	 * Get "after" change value, if it fails, simply return the
	 * before value.
	 */
	ava.va_mask = AT_CTIME|AT_SEQ;
	if (VOP_GETATTR(dvp, &ava, 0, cs->cr, NULL)) {
		ava.va_ctime = bva.va_ctime;
		ava.va_seq = 0;
	}
	VN_RELE(dvp);

	/*
	 * Validate the file is a file
	 */
	fva.va_mask = AT_TYPE|AT_MODE;
	error = VOP_GETATTR(cs->vp, &fva, 0, cs->cr, NULL);
	if (error)
		return (puterrno4(error));

	if (fva.va_type != VREG) {
		if (fva.va_type == VDIR)
			return (NFS4ERR_ISDIR);
		if (fva.va_type == VLNK)
			return (NFS4ERR_SYMLINK);
		return (NFS4ERR_INVAL);
	}

	NFS4_SET_FATTR4_CHANGE(cinfo->before, bva.va_ctime);
	NFS4_SET_FATTR4_CHANGE(cinfo->after, ava.va_ctime);

	/*
	 * It is undefined if VOP_LOOKUP will change va_seq, so
	 * cinfo.atomic = TRUE only if we have
	 * non-zero va_seq's, and they have not changed.
	 */
	if (bva.va_seq && ava.va_seq && ava.va_seq == bva.va_seq)
		cinfo->atomic = TRUE;
	else
		cinfo->atomic = FALSE;

	/* Check for mandatory locking */
	cs->mandlock = MANDLOCK(cs->vp, fva.va_mode);
	return (check_open_access(access, cs, req));
}

/*ARGSUSED*/
static void
mds_do_opennull(struct compound_state *cs,
		struct svc_req *req,
		OPEN4args *args,
		rfs4_openowner_t *oo,
		OPEN4res *resp)
{
	change_info4 *cinfo = &resp->cinfo;
	attrmap4 *attrset = &resp->attrset;

	if (args->opentype == OPEN4_NOCREATE)
		resp->status = mds_lookupfile(&args->open_claim4_u.file,
		    req, cs, (args->share_access & 0xff), cinfo);
	else {
		/* inhibit delegation grants during exclusive create */

		if (args->mode == EXCLUSIVE4)
			rfs4_disable_delegation(cs->instp);

		resp->status = mds_createfile(args, req, cs, cinfo, attrset);
	}

	if (resp->status == NFS4_OK) {

		/* cs->vp cs->fh now reference the desired file */

		mds_do_open(cs, req, oo, do_41_deleg_hack(args->share_access),
		    (args->share_access & 0xff), args->share_deny, resp, 0);

		/*
		 * If rfs4_createfile set attrset, we must
		 * clear this attrset before the response is copied.
		 */
		if (resp->status != NFS4_OK)
			resp->attrset =
			    NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
	}
	else
		*cs->statusp = resp->status;

	if (args->mode == EXCLUSIVE4)
		rfs4_enable_delegation(cs->instp);
}

/*ARGSUSED*/
static void
mds_do_openprev(struct compound_state *cs, struct svc_req *req,
		OPEN4args *args, rfs4_openowner_t *oo, OPEN4res *resp)
{
	change_info4 *cinfo = &resp->cinfo;
	vattr_t va;
	vtype_t v_type = cs->vp->v_type;
	int error = 0;
	caller_context_t ct;

	/* Verify that we have a regular file */
	if (v_type != VREG) {
		if (v_type == VDIR)
			resp->status = NFS4ERR_ISDIR;
		else if (v_type == VLNK)
			resp->status = NFS4ERR_SYMLINK;
		else
			resp->status = NFS4ERR_INVAL;
		return;
	}

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	va.va_mask = AT_MODE|AT_UID;
	error = VOP_GETATTR(cs->vp, &va, 0, cs->cr, &ct);
	if (error) {
		resp->status = puterrno4(error);
		return;
	}

	cs->mandlock = MANDLOCK(cs->vp, va.va_mode);

	/*
	 * Check if we have access to the file, Note the the file
	 * could have originally been open UNCHECKED or GUARDED
	 * with mode bits that will now fail, but there is nothing
	 * we can really do about that except in the case that the
	 * owner of the file is the one requesting the open.
	 */
	if (crgetuid(cs->cr) != va.va_uid) {
		resp->status = check_open_access((args->share_access & 0xff),
		    cs, req);
		if (resp->status != NFS4_OK) {
			return;
		}
	}

	/*
	 * cinfo on a CLAIM_PREVIOUS is undefined, initialize to zero
	 */
	cinfo->before = 0;
	cinfo->after = 0;
	cinfo->atomic = FALSE;

	mds_do_open(cs, req, oo,
	    NFS4_DELEG4TYPE2REQTYPE(args->open_claim4_u.delegate_type),
	    (args->share_access && 0xff), args->share_deny, resp, 0);
}

static void
mds_do_opendelcur(struct compound_state *cs, struct svc_req *req,
		OPEN4args *args, rfs4_openowner_t *oo, OPEN4res *resp)
{
	int error;
	nfsstat4 status;
	stateid4 stateid =
	    args->open_claim4_u.delegate_cur_info.delegate_stateid;
	rfs4_deleg_state_t *dsp;

	/*
	 * Find the state info from the stateid and confirm that the
	 * file is delegated.  If the state openowner is the same as
	 * the supplied openowner we're done. If not, get the file
	 * info from the found state info. Use that file info to
	 * create the state for this lock owner. Note solaris doen't
	 * really need the pathname to find the file. We may want to
	 * lookup the pathname and make sure that the vp exist and
	 * matches the vp in the file structure. However it is
	 * possible that the pathname nolonger exists (local process
	 * unlinks the file), so this may not be that useful.
	 */

	status = rfs4_get_deleg_state(cs, &stateid, &dsp);
	if (status != NFS4_OK) {
		resp->status = status;
		return;
	}

	ASSERT(dsp->finfo->dinfo->dtype != OPEN_DELEGATE_NONE);

	/*
	 * New lock owner, create state. Since this was probably called
	 * in response to a CB_RECALL we set deleg to DELEG_NONE
	 */

	ASSERT(cs->vp != NULL);
	VN_RELE(cs->vp);
	VN_HOLD(dsp->finfo->vp);
	cs->vp = dsp->finfo->vp;

	if (error = mknfs41_fh(&cs->fh, cs->vp, cs->exi)) {
		rfs4_deleg_state_rele(dsp);
		*cs->statusp = resp->status = puterrno4(error);
		return;
	}

	/* Mark progress for delegation returns */
	dsp->finfo->dinfo->time_lastwrite = gethrestime_sec();
	rfs4_deleg_state_rele(dsp);
	mds_do_open(cs, req, oo, DELEG_NONE,
	    (args->share_access & 0xff),
	    args->share_deny, resp, 1);
}

/*ARGSUSED*/
static void
mds_do_opendelprev(struct compound_state *cs, struct svc_req *req,
			OPEN4args *args, rfs4_openowner_t *oo, OPEN4res *resp)
{
	/*
	 * Lookup the pathname, it must already exist since this file
	 * was delegated.
	 *
	 * Find the file and state info for this vp and open owner pair.
	 *	check that they are in fact delegated.
	 *	check that the state access and deny modes are the same.
	 *
	 * Return the delgation possibly seting the recall flag.
	 */
	rfs4_file_t *file;
	rfs4_state_t *state;
	bool_t create = FALSE;
	bool_t dcreate = FALSE;
	rfs4_deleg_state_t *dsp;
	nfsace4 *ace;


	/* Note we ignore oflags */
	resp->status = mds_lookupfile(&args->open_claim4_u.file_delegate_prev,
	    req, cs, (args->share_access & 0xff), &resp->cinfo);

	if (resp->status != NFS4_OK) {
		return;
	}

	/* get the file struct and hold a lock on it during initial open */
	file = rfs4_findfile_withlock(cs->instp, cs->vp, NULL, &create);
	if (file == NULL) {
		DTRACE_PROBE(nfss__e__no_file);
		resp->status = NFS4ERR_SERVERFAULT;
		return;
	}

	state = rfs4_findstate_by_owner_file(cs, oo, file, &create);
	if (state == NULL) {
		DTRACE_PROBE(nfss__e__no_state);
		resp->status = NFS4ERR_SERVERFAULT;
		rfs4_file_rele_withunlock(file);
		return;
	}

	rfs4_dbe_lock(state->dbe);
	rfs4_dbe_lock(file->dbe);
	if ((args->share_access & 0xff) != state->share_access ||
	    args->share_deny != state->share_deny ||
	    state->finfo->dinfo->dtype == OPEN_DELEGATE_NONE) {
		DTRACE_PROBE2(nfss__e__state_mixup, rfs4_state_t *, state,
		    OPEN4args *, args);
		rfs4_dbe_unlock(file->dbe);
		rfs4_dbe_unlock(state->dbe);
		rfs4_file_rele(file);
		rfs4_state_rele(state);
		resp->status = NFS4ERR_SERVERFAULT;
		return;
	}
	rfs4_dbe_unlock(file->dbe);
	rfs4_dbe_unlock(state->dbe);

	dsp = rfs4_finddeleg(cs, state, &dcreate);
	if (dsp == NULL) {
		DTRACE_PROBE(nfss__e__no_deleg);
		rfs4_state_rele(state);
		rfs4_file_rele(file);
		resp->status = NFS4ERR_SERVERFAULT;
		return;
	}

	next_stateid(&state->stateid);

	resp->stateid = state->stateid.stateid;

	resp->delegation.delegation_type = dsp->dtype;

	if (dsp->dtype == OPEN_DELEGATE_READ) {
		open_read_delegation4 *rv =
		    &resp->delegation.open_delegation4_u.read;

		rv->stateid = dsp->delegid.stateid;
		rv->recall = FALSE; /* no policy in place to set to TRUE */
		ace = &rv->permissions;
	} else {
		open_write_delegation4 *rv =
		    &resp->delegation.open_delegation4_u.write;

		rv->stateid = dsp->delegid.stateid;
		rv->recall = FALSE;  /* no policy in place to set to TRUE */
		ace = &rv->permissions;
		rv->space_limit.limitby = NFS_LIMIT_SIZE;
		rv->space_limit.nfs_space_limit4_u.filesize = UINT64_MAX;
	}

	/* XXX For now */
	ace->type = ACE4_ACCESS_ALLOWED_ACE_TYPE;
	ace->flag = 0;
	ace->access_mask = 0;
	ace->who.utf8string_len = 0;
	ace->who.utf8string_val = 0;

	rfs4_deleg_state_rele(dsp);
	rfs4_state_rele(state);
	rfs4_file_rele(file);
}

static void
mds_op_open(nfs_argop4 *argop, nfs_resop4 *resop,
	    struct svc_req *req, compound_state_t *cs)
{
	OPEN4args		*args = &argop->nfs_argop4_u.opopen;
	OPEN4res		*resp = &resop->nfs_resop4_u.opopen;
	open_owner4		*owner = &args->owner;
	open_claim_type4	claim = args->claim;
	rfs4_client_t		*cp;
	rfs4_openowner_t	*oo;
	bool_t			create;
	int			can_reclaim;
	int			share_access;

	DTRACE_NFSV4_2(op__open__start, struct compound_state *, cs,
	    OPEN4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	cp = cs->cp;
	owner->clientid = cp->clientid;
	can_reclaim = cp->can_reclaim;

retry:
	create = TRUE;
	oo = mds_findopenowner(cs->instp, owner, &create);
	if (oo == NULL) {
		/* XXX: this seems a little fishy... */
		*cs->statusp = resp->status = NFS4ERR_STALE_CLIENTID;
		goto final;
	}

	/* Need to serialize access to the stateid space */
	rfs4_sw_enter(&oo->oo_sw);

	/* Grace only applies to regular-type OPENs */
	if (rfs4_clnt_in_grace(cp) &&
	    (claim == CLAIM_NULL || claim == CLAIM_DELEGATE_CUR)) {
		*cs->statusp = resp->status = NFS4ERR_GRACE;
		goto out;
	}

	/*
	 * If previous state at the server existed then can_reclaim
	 * will be set. If not reply NFS4ERR_NO_GRACE to the
	 * client.
	 */
	if (rfs4_clnt_in_grace(cp) && claim == CLAIM_PREVIOUS && !can_reclaim) {
		*cs->statusp = resp->status = NFS4ERR_NO_GRACE;
		goto out;
	}

	/*
	 * Reject the open if the client has missed the grace period
	 */
	if (!rfs4_clnt_in_grace(cp) && claim == CLAIM_PREVIOUS) {
		*cs->statusp = resp->status = NFS4ERR_NO_GRACE;
		goto out;
	}

	/*
	 * OPEN_CONFIRM is mandatory not to impl in 4.1.
	 */
	oo->need_confirm = FALSE;
	resp->rflags |= OPEN4_RESULT_LOCKTYPE_POSIX;

	/*
	 * If there is an unshared filesystem mounted on this vnode,
	 * do not allow to open/create in this directory.
	 */
	if (vn_ismntpt(cs->vp)) {
		*cs->statusp = resp->status = NFS4ERR_ACCESS;
		goto out;
	}

	share_access = (args->share_access && 0xff);

	/*
	 * access must READ, WRITE, or BOTH.  No access is invalid.
	 * deny can be READ, WRITE, BOTH, or NONE.
	 * bits not defined for access/deny are invalid.
	 */
	if (! (share_access & OPEN4_SHARE_ACCESS_BOTH) ||
	    (share_access & ~OPEN4_SHARE_ACCESS_BOTH) ||
	    (args->share_deny & ~OPEN4_SHARE_DENY_BOTH)) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * make sure attrset is zero before response is built.
	 */
	resp->attrset = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));

	switch (claim) {
	case CLAIM_NULL:
		mds_do_opennull(cs, req, args, oo, resp);
		break;
	case CLAIM_PREVIOUS:
		mds_do_openprev(cs, req, args, oo, resp);
		break;
	case CLAIM_DELEGATE_CUR:
		mds_do_opendelcur(cs, req, args, oo, resp);
		break;
	case CLAIM_DELEGATE_PREV:
		mds_do_opendelprev(cs, req, args, oo, resp);
		break;
	/*  OTHER CLAIM TYPES !!! */
	default:
		resp->status = NFS4ERR_INVAL;
		break;
	}

out:
	switch (resp->status) {
	case NFS4ERR_BADXDR:
	case NFS4ERR_BAD_SEQID:
	case NFS4ERR_BAD_STATEID:
	case NFS4ERR_NOFILEHANDLE:
	case NFS4ERR_RESOURCE:
	case NFS4ERR_STALE_CLIENTID:
	case NFS4ERR_STALE_STATEID:
		/*
		 * The protocol states that if any of these errors are
		 * being returned, the sequence id should not be
		 * incremented.  Any other return requires an
		 * increment.
		 */
		break;
	}
	*cs->statusp = resp->status;
	rfs4_sw_exit(&oo->oo_sw);
	rfs4_openowner_rele(oo);

final:
	DTRACE_NFSV4_2(op__open__done, struct compound_state *, cs,
	    OPEN4res *, resp);
}

/*ARGSUSED*/
static void
mds_free_reply(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function for NFSv4.0 and NFSv4.1 */
	rfs4_free_reply(resop);
}

/*ARGSUSED*/
void
mds_op_open_downgrade(nfs_argop4 *argop, nfs_resop4 *resop,
		    struct svc_req *req, compound_state_t *cs)
{
	OPEN_DOWNGRADE4args *args = &argop->nfs_argop4_u.opopen_downgrade;
	OPEN_DOWNGRADE4res *resp = &resop->nfs_resop4_u.opopen_downgrade;
	uint32_t access = (args->share_access & 0xff);
	uint32_t deny = args->share_deny;
	nfsstat4 status;
	rfs4_state_t *sp;
	rfs4_file_t *fp;
	int fflags = 0;
	int rc;

	DTRACE_NFSV4_2(op__open__downgrade__start, struct compound_state *, cs,
	    OPEN_DOWNGRADE4args *, args);


	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (mds_strict_seqid && args->seqid) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	status = rfs4_get_state(cs, &args->open_stateid, &sp, RFS4_DBS_VALID);
	if (status != NFS4_OK) {
		*cs->statusp = resp->status = status;
		goto final;
	}

	/* Ensure specified filehandle matches */
	if (cs->vp != sp->finfo->vp) {
		rfs4_state_rele(sp);
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto final;
	}

	/* hold off other access to open_owner while we tinker */
	rfs4_sw_enter(&sp->owner->oo_sw);

	rc = mds_check_stateid_seqid(sp, &args->open_stateid);
	switch (rc) {
	case NFS4_CHECK_STATEID_OKAY:
		break;
	case NFS4_CHECK_STATEID_OLD:
		*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_BAD:
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_EXPIRED:
		*cs->statusp = resp->status = NFS4ERR_EXPIRED;
		goto end;
	case NFS4_CHECK_STATEID_CLOSED:
		*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_UNCONFIRMED:
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_REPLAY:
		ASSERT(0);
		break;
	default:
		ASSERT(FALSE);
		break;
	}

	rfs4_dbe_lock(sp->dbe);
	/*
	 * Check that the new access modes and deny modes are valid.
	 * Check that no invalid bits are set.
	 */
	if ((access & ~(OPEN4_SHARE_ACCESS_READ | OPEN4_SHARE_ACCESS_WRITE)) ||
	    (deny & ~(OPEN4_SHARE_DENY_READ | OPEN4_SHARE_DENY_WRITE))) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		rfs4_dbe_unlock(sp->dbe);
		goto end;
	}

	/*
	 * The new modes must be a subset of the current modes and
	 * the access must specify at least one mode. To test that
	 * the new mode is a subset of the current modes we bitwise
	 * AND them together and check that the result equals the new
	 * mode. For example:
	 * New mode, access == R and current mode, sp->share_access  == RW
	 * access & sp->share_access == R == access, so the new access mode
	 * is valid. Consider access == RW, sp->share_access = R
	 * access & sp->share_access == R != access, so the new access mode
	 * is invalid.
	 */
	if ((access & sp->share_access) != access ||
	    (deny & sp->share_deny) != deny ||
	    (access &
	    (OPEN4_SHARE_ACCESS_READ | OPEN4_SHARE_ACCESS_WRITE)) == 0) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		rfs4_dbe_unlock(sp->dbe);
		goto end;
	}

	/*
	 * Release any share locks associated with this stateID.
	 * Strictly speaking, this violates the spec because the
	 * spec effectively requires that open downgrade be atomic.
	 * At present, fs_shrlock does not have this capability.
	 */
	rfs4_dbe_unlock(sp->dbe);
	rfs4_unshare(sp);
	rfs4_dbe_lock(sp->dbe);

	fp = sp->finfo;
	rfs4_dbe_lock(fp->dbe);

	/*
	 * If the current mode has deny read and the new mode
	 * does not, decrement the number of deny read mode bits
	 * and if it goes to zero turn off the deny read bit
	 * on the file.
	 */
	if ((sp->share_deny & OPEN4_SHARE_DENY_READ) &&
	    (deny & OPEN4_SHARE_DENY_READ) == 0) {
		fp->deny_read--;
		if (fp->deny_read == 0)
			fp->share_deny &= ~OPEN4_SHARE_DENY_READ;
	}

	/*
	 * If the current mode has deny write and the new mode
	 * does not, decrement the number of deny write mode bits
	 * and if it goes to zero turn off the deny write bit
	 * on the file.
	 */
	if ((sp->share_deny & OPEN4_SHARE_DENY_WRITE) &&
	    (deny & OPEN4_SHARE_DENY_WRITE) == 0) {
		fp->deny_write--;
		if (fp->deny_write == 0)
			fp->share_deny &= ~OPEN4_SHARE_DENY_WRITE;
	}

	/*
	 * If the current mode has access read and the new mode
	 * does not, decrement the number of access read mode bits
	 * and if it goes to zero turn off the access read bit
	 * on the file. set fflags to FREAD for the call to
	 * vn_open_downgrade().
	 */
	if ((sp->share_access & OPEN4_SHARE_ACCESS_READ) &&
	    (access & OPEN4_SHARE_ACCESS_READ) == 0) {
		fp->access_read--;
		if (fp->access_read == 0)
			fp->share_access &= ~OPEN4_SHARE_ACCESS_READ;
		fflags |= FREAD;
	}

	/*
	 * If the current mode has access write and the new mode
	 * does not, decrement the number of access write mode bits
	 * and if it goes to zero turn off the access write bit
	 * on the file. set fflags to FWRITE for the call to
	 * vn_open_downgrade().
	 */
	if ((sp->share_access & OPEN4_SHARE_ACCESS_WRITE) &&
	    (access & OPEN4_SHARE_ACCESS_WRITE) == 0) {
		fp->access_write--;
		if (fp->access_write == 0)
			fp->share_deny &= ~OPEN4_SHARE_ACCESS_WRITE;
		fflags |= FWRITE;
	}

	/* Set the new access and deny modes */
	sp->share_access = access;
	sp->share_deny = deny;
	/* Check that the file is still accessible */
	ASSERT(fp->share_access);

	rfs4_dbe_unlock(fp->dbe);

	rfs4_dbe_unlock(sp->dbe);
	if ((status = rfs4_share(sp)) != NFS4_OK) {
		*cs->statusp = resp->status = NFS4ERR_SERVERFAULT;
		goto end;
	}

	/*
	 * we successfully downgraded the share lock, now we need to downgrade
	 * the open.  it is possible that the downgrade was only for a deny
	 * mode and we have nothing else to do.
	 */
	if ((fflags & (FREAD|FWRITE)) != 0)
		vn_open_downgrade(cs->vp, fflags);

	rfs4_dbe_lock(sp->dbe);

	/* Update the stateid */
	next_stateid(&sp->stateid);
	resp->open_stateid = sp->stateid.stateid;

	rfs4_dbe_unlock(sp->dbe);

	*cs->statusp = resp->status = NFS4_OK;
	/* Update the lease */
	rfs4_update_lease(sp->owner->client);
end:
	rfs4_sw_exit(&sp->owner->oo_sw);
	rfs4_state_rele(sp);

final:
	DTRACE_NFSV4_2(op__open__downgrade__done, struct compound_state *, cs,
	    OPEN_DOWNGRADE4res *, resp);

}

/*ARGSUSED*/
void
mds_op_close(nfs_argop4 *argop, nfs_resop4 *resop,
	    struct svc_req *req, compound_state_t *cs)
{
	/* XXX Currently not using req arg */
	CLOSE4args *args = &argop->nfs_argop4_u.opclose;
	CLOSE4res *resp = &resop->nfs_resop4_u.opclose;
	rfs4_state_t *sp;
	nfsstat4 status;
	int rc;

	DTRACE_NFSV4_2(op__close__start, struct compound_state *, cs,
	    CLOSE4args *, args);


	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (mds_strict_seqid && args->seqid) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	status = rfs4_get_state(cs, &args->open_stateid, &sp, RFS4_DBS_INVALID);
	if (status != NFS4_OK) {
		*cs->statusp = resp->status = status;
		goto final;
	}

	/* Ensure specified filehandle matches */
	if (cs->vp != sp->finfo->vp) {
		rfs4_state_rele(sp);
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto final;
	}

	/* hold off other access to open_owner while we tinker */
	rfs4_sw_enter(&sp->owner->oo_sw);

	rc = mds_check_stateid_seqid(sp, &args->open_stateid);
	switch (rc) {
	case NFS4_CHECK_STATEID_OKAY:
		break;
	case NFS4_CHECK_STATEID_OLD:
		*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_BAD:
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_EXPIRED:
		*cs->statusp = resp->status = NFS4ERR_EXPIRED;
		goto end;
	case NFS4_CHECK_STATEID_CLOSED:
		*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_UNCONFIRMED:
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_REPLAY:
		ASSERT(0);
		break;
	default:
		ASSERT(FALSE);
		break;
	}

	rfs4_dbe_lock(sp->dbe);

	/* Update the stateid. */
	next_stateid(&sp->stateid);
	resp->open_stateid = sp->stateid.stateid;

	rfs4_dbe_unlock(sp->dbe);

	rfs4_update_lease(sp->owner->client);
	rfs4_state_close(sp, FALSE, FALSE, cs->cr);

	*cs->statusp = resp->status = status;

end:
	rfs4_sw_exit(&sp->owner->oo_sw);
	rfs4_state_rele(sp);

final:
	DTRACE_NFSV4_2(op__close__done, struct compound_state *, cs,
	    CLOSE4res *, resp);

}

/*
 * lock_denied: Fill in a LOCK4deneid structure given an flock64 structure.
 */
static nfsstat4
mds_lock_denied(nfs_server_instance_t *instp, LOCK4denied *dp,
    struct flock64 *flk)
{
	rfs4_lockowner_t *lo;
	rfs4_client_t *cp;
	uint32_t len;

	lo = findlockowner_by_pid(instp, flk->l_pid);
	if (lo != NULL) {
		cp = lo->client;
		if (rfs4_lease_expired(cp)) {
			rfs4_lockowner_rele(lo);
			rfs4_dbe_hold(cp->dbe);
			rfs4_client_close(cp);
			return (NFS4ERR_EXPIRED);
		}
		dp->owner.clientid = lo->owner.clientid;
		len = lo->owner.owner_len;
		dp->owner.owner_val = kmem_alloc(len, KM_SLEEP);
		bcopy(lo->owner.owner_val, dp->owner.owner_val, len);
		dp->owner.owner_len = len;
		rfs4_lockowner_rele(lo);
		goto finish;
	}

	/*
	 * Its not a NFS4 lock. We take advantage that the upper 32 bits
	 * of the client id contain the boot time for a NFS4 lock. So we
	 * fabricate and identity by setting clientid to the sysid, and
	 * the lock owner to the pid.
	 */
	dp->owner.clientid = flk->l_sysid;
	len = sizeof (pid_t);
	dp->owner.owner_len = len;
	dp->owner.owner_val = kmem_alloc(len, KM_SLEEP);
	bcopy(&flk->l_pid, dp->owner.owner_val, len);
finish:
	dp->offset = flk->l_start;
	dp->length = flk->l_len;

	if (flk->l_type == F_RDLCK)
		dp->locktype = READ_LT;
	else if (flk->l_type == F_WRLCK)
		dp->locktype = WRITE_LT;
	else
		return (NFS4ERR_INVAL);	/* no mapping from POSIX ltype to v4 */

	return (NFS4_OK);
}

/*ARGSUSED*/
void
mds_op_lock(nfs_argop4 *argop, nfs_resop4 *resop,
	    struct svc_req *req, compound_state_t *cs)
{
	/* XXX Currently not using req arg */
	LOCK4args *args = &argop->nfs_argop4_u.oplock;
	LOCK4res *resp = &resop->nfs_resop4_u.oplock;
	nfsstat4 status;
	stateid4 *stateid;
	rfs4_lockowner_t *lo;
	rfs4_client_t *cp;
	rfs4_state_t *sp = NULL;
	rfs4_lo_state_t *lsp = NULL;
	bool_t ls_sw_held = FALSE;
	bool_t create = TRUE;
	bool_t lcreate = TRUE;
	int rc;

	DTRACE_NFSV4_2(op__lock__start, struct compound_state *, cs,
	    LOCK4args *, args);


	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (args->locker.new_lock_owner) {
		/* Create a new lockowner for this instance */
		open_to_lock_owner4 *olo = &args->locker.locker4_u.open_owner;

		/*
		 * validate that open_seqid, lock_seqid and the
		 * clientid in lock_owner are all zero.
		 */
		if (mds_strict_seqid && (olo->open_seqid ||
		    olo->lock_seqid ||
		    olo->lock_owner.clientid)) {
			*cs->statusp = resp->status = NFS4ERR_INVAL;
			goto final;
		}

		/*
		 * get/validate the open stateid
		 */
		stateid = &olo->open_stateid;
		status = rfs4_get_state(cs, stateid, &sp, RFS4_DBS_VALID);
		if (status != NFS4_OK) {
			*cs->statusp = resp->status = status;
			goto final;
		}

		/* Ensure specified filehandle matches */
		if (cs->vp != sp->finfo->vp) {
			rfs4_state_rele(sp);
			*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
			goto final;
		}

		/* hold off other access to open_owner while we tinker */
		rfs4_sw_enter(&sp->owner->oo_sw);

		rc = mds_check_stateid_seqid(sp, stateid);
		switch (rc) {
		case NFS4_CHECK_STATEID_OLD:
			*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_BAD:
			*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_EXPIRED:
			*cs->statusp = resp->status = NFS4ERR_EXPIRED;
			goto end;
		case NFS4_CHECK_STATEID_UNCONFIRMED:
			*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_CLOSED:
			*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_OKAY:
		case NFS4_CHECK_STATEID_REPLAY:
			break;
		}

		/*
		 * Use the clientid4 from the session.
		 *
		 * XXX a quick hack is to plop the clientid4 from the
		 * XXX compound state into the lock_owner structure,
		 * XXX since the _hash and _compare functions use
		 * XXX that field.
		 */
		olo->lock_owner.clientid = cs->cp->clientid;

		lo = findlockowner(cs->instp, &olo->lock_owner, &lcreate);
		if (lo == NULL) {
			*cs->statusp = resp->status = NFS4ERR_RESOURCE;
			goto end;
		}

		lsp = mds_findlo_state_by_owner(lo, sp, &create);
		if (lsp == NULL) {
			rfs4_update_lease(sp->owner->client);
			*cs->statusp = resp->status = NFS4ERR_SERVERFAULT;
			rfs4_lockowner_rele(lo);
			goto end;
		}

		/*
		 * This is the new_lock_owner branch and the client is
		 * supposed to be associating a new lock_owner with
		 * the open file at this point.  If we find that a
		 * lock_owner/state association already exists and a
		 * successful LOCK request was returned to the client,
		 * an error is returned to the client since this is
		 * not appropriate.  The client should be using the
		 * existing lock_owner branch.
		 */
		if (create == FALSE) {
			if (lsp->lock_completed == TRUE) {
				*cs->statusp =
				    resp->status = NFS4ERR_BAD_SEQID;
				rfs4_lockowner_rele(lo);
				goto end;
			}
		}

		rfs4_update_lease(sp->owner->client);
		rfs4_dbe_lock(lsp->dbe);

		/* hold off other access to lsp while we tinker */
		rfs4_sw_enter(&lsp->ls_sw);
		ls_sw_held = TRUE;

		rfs4_dbe_unlock(lsp->dbe);

		rfs4_lockowner_rele(lo);
	} else {
		/*
		 * validate lock_seqid is zero.
		 */
		if (mds_strict_seqid &&
		    args->locker.locker4_u.lock_owner.lock_seqid) {
			*cs->statusp = resp->status = NFS4ERR_INVAL;
			goto final;
		}

		stateid = &args->locker.locker4_u.lock_owner.lock_stateid;
		/* get lsp and hold the lock on the underlying file struct */
		if ((status = rfs4_get_lo_state(cs, stateid, &lsp, TRUE))
		    != NFS4_OK) {
			*cs->statusp = resp->status = status;
			goto final;
		}
		create = FALSE;	/* We didn't create lsp */

		/* Ensure specified filehandle matches */
		if (cs->vp != lsp->state->finfo->vp) {
			rfs4_lo_state_rele(lsp, TRUE);
			*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
			goto final;
		}

		/* hold off other access to lsp while we tinker */
		rfs4_sw_enter(&lsp->ls_sw);
		ls_sw_held = TRUE;

		switch (rfs4_check_lo_stateid_seqid(lsp, stateid)) {
		/*
		 * The stateid looks like it was okay (expected to be
		 * the next one)
		 */
		case NFS4_CHECK_STATEID_OKAY:
			/*
			 * The sequence id is now checked.  Determine
			 * if this is a replay or if it is in the
			 * expected (next) sequence.  In the case of a
			 * replay, there are two replay conditions
			 * that may occur.  The first is the normal
			 * condition where a LOCK is done with a
			 * NFS4_OK response and the stateid is
			 * updated.  That case is handled below when
			 * the stateid is identified as a REPLAY.  The
			 * second is the case where an error is
			 * returned, like NFS4ERR_DENIED, and the
			 * sequence number is updated but the stateid
			 * is not updated.  This second case is dealt
			 * with here.  So it may seem odd that the
			 * stateid is okay but the sequence id is a
			 * replay but it is okay.
			 */
			/* XXX: rbg -- missing code ? :-)  */
			break;
		case NFS4_CHECK_STATEID_OLD:
			*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_BAD:
			*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_EXPIRED:
			*cs->statusp = resp->status = NFS4ERR_EXPIRED;
			goto end;
		case NFS4_CHECK_STATEID_CLOSED:
			*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
			goto end;
		case NFS4_CHECK_STATEID_REPLAY:
			ASSERT(0);
			break;
		default:
			ASSERT(FALSE);
			break;
		}

		rfs4_update_lease(lsp->locker->client);
	}

	/*
	 * NFS4 only allows locking on regular files, so
	 * verify type of object.
	 */
	if (cs->vp->v_type != VREG) {
		if (cs->vp->v_type == VDIR)
			status = NFS4ERR_ISDIR;
		else
			status = NFS4ERR_INVAL;
		goto out;
	}

	cp = lsp->state->owner->client;

	if (rfs4_clnt_in_grace(cp) && !args->reclaim) {
		status = NFS4ERR_GRACE;
		goto out;
	}

	if (rfs4_clnt_in_grace(cp) && args->reclaim && !cp->can_reclaim) {
		status = NFS4ERR_NO_GRACE;
		goto out;
	}

	if (!rfs4_clnt_in_grace(cp) && args->reclaim) {
		status = NFS4ERR_NO_GRACE;
		goto out;
	}

	if (lsp->state->finfo->dinfo->dtype == OPEN_DELEGATE_WRITE)
		cs->deleg = TRUE;

	status = rfs4_do_lock(lsp, args->locktype,
	    args->locker.locker4_u.lock_owner.lock_seqid, args->offset,
	    args->length, cs->cr, resop);

out:
	*cs->statusp = resp->status = status;

	if (status == NFS4_OK) {
		resp->LOCK4res_u.lock_stateid = lsp->lockid.stateid;
		lsp->lock_completed = TRUE;
	}

end:
	if (lsp) {
		if (ls_sw_held)
			rfs4_sw_exit(&lsp->ls_sw);
		/*
		 * If an sp obtained, then the lsp does not represent
		 * a lock on the file struct.
		 */
		if (sp != NULL)
			rfs4_lo_state_rele(lsp, FALSE);
		else
			rfs4_lo_state_rele(lsp, TRUE);
	}
	if (sp) {
		rfs4_sw_exit(&sp->owner->oo_sw);
		rfs4_state_rele(sp);
	}

final:
	DTRACE_NFSV4_2(op__lock__done, struct compound_state *, cs,
	    LOCK4res *, resp);

}

/* free function for LOCK/LOCKT */
/*ARGSUSED*/
static void
mds_lock_denied_free(nfs_resop4 *resop, compound_state_t *cs)
{
	/* Common function for NFSv4.0 and NFSv4.1 */
	lock_denied_free(resop);
}

/*ARGSUSED*/
void
mds_op_locku(nfs_argop4 *argop, nfs_resop4 *resop,
	    struct svc_req *req, compound_state_t *cs)
{
	/* XXX Currently not using req arg */
	LOCKU4args *args = &argop->nfs_argop4_u.oplocku;
	LOCKU4res *resp = &resop->nfs_resop4_u.oplocku;
	nfsstat4 status;
	stateid4 *stateid = &args->lock_stateid;
	rfs4_lo_state_t *lsp;

	DTRACE_NFSV4_2(op__locku__start, struct compound_state *, cs,
	    LOCKU4args *, args);


	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (mds_strict_seqid && args->seqid) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	}

	if ((status = rfs4_get_lo_state(cs, stateid, &lsp, TRUE)) != NFS4_OK) {
		*cs->statusp = resp->status = status;
		goto final;
	}

	/* Ensure specified filehandle matches */
	if (cs->vp != lsp->state->finfo->vp) {
		rfs4_lo_state_rele(lsp, TRUE);
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto final;
	}

	/* hold off other access to lsp while we tinker */
	rfs4_sw_enter(&lsp->ls_sw);

	switch (rfs4_check_lo_stateid_seqid(lsp, stateid)) {
	case NFS4_CHECK_STATEID_OKAY:
		break;
	case NFS4_CHECK_STATEID_OLD:
		*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_BAD:
		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_EXPIRED:
		*cs->statusp = resp->status = NFS4ERR_EXPIRED;
		goto end;
	case NFS4_CHECK_STATEID_CLOSED:
		*cs->statusp = resp->status = NFS4ERR_OLD_STATEID;
		goto end;
	case NFS4_CHECK_STATEID_REPLAY:
		ASSERT(0);
		break;
	default:
		ASSERT(FALSE);
		break;
	}

	rfs4_update_lock_sequence(lsp);
	rfs4_update_lease(lsp->locker->client);

	/*
	 * NFS4 only allows locking on regular files, so
	 * verify type of object.
	 */
	if (cs->vp->v_type != VREG) {
		if (cs->vp->v_type == VDIR)
			status = NFS4ERR_ISDIR;
		else
			status = NFS4ERR_INVAL;
		goto out;
	}

	if (rfs4_clnt_in_grace(lsp->state->owner->client)) {
		status = NFS4ERR_GRACE;
		goto out;
	}

	status = rfs4_do_lock(lsp, args->locktype,
	    args->seqid, args->offset, args->length, cs->cr, resop);

out:
	*cs->statusp = resp->status = status;

	if (status == NFS4_OK)
		resp->lock_stateid = lsp->lockid.stateid;

end:
	rfs4_sw_exit(&lsp->ls_sw);
	rfs4_lo_state_rele(lsp, TRUE);

final:
	DTRACE_NFSV4_2(op__locku__done, struct compound_state *, cs,
	    LOCKU4res *, resp);

}

/*
 * LOCKT is a best effort routine, the client can not be guaranteed that
 * the status return is still in effect by the time the reply is received.
 * They are numerous race conditions in this routine, but we are not required
 * and can not be accurate.
 */
/*ARGSUSED*/
void
mds_op_lockt(nfs_argop4 *argop, nfs_resop4 *resop,
	    struct svc_req *req, compound_state_t *cs)
{
	LOCKT4args *args = &argop->nfs_argop4_u.oplockt;
	LOCKT4res *resp = &resop->nfs_resop4_u.oplockt;
	rfs4_lockowner_t *lo;
	bool_t create = FALSE;
	struct flock64 flk;
	int error;
	int flag = FREAD | FWRITE;
	int ltype;
	length4 posix_length;
	sysid_t sysid;
	pid_t pid;
	caller_context_t ct;

	DTRACE_NFSV4_2(op__lockt__start, struct compound_state *, cs,
	    LOCKT4args *, args);

	if (cs->vp == NULL) {
		*cs->statusp = resp->status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	args->owner.clientid = cs->cp->clientid;

	ct.cc_sysid = 0;
	ct.cc_pid = 0;
	ct.cc_caller_id = cs->instp->caller_id;
	ct.cc_flags = CC_DONTBLOCK;

	/*
	 * NFS4 only allows locking on regular files, so
	 * verify type of object.
	 */
	if (cs->vp->v_type != VREG) {
		if (cs->vp->v_type == VDIR)
			*cs->statusp = resp->status = NFS4ERR_ISDIR;
		else
			*cs->statusp = resp->status =  NFS4ERR_INVAL;
		goto final;
	}

	resp->status = NFS4_OK;

	switch (args->locktype) {
	case READ_LT:
	case READW_LT:
		ltype = F_RDLCK;
		break;
	case WRITE_LT:
	case WRITEW_LT:
		ltype = F_WRLCK;
		break;
	}

	posix_length = args->length;
	/* Check for zero length. To lock to end of file use all ones for V4 */
	if (posix_length == 0) {
		*cs->statusp = resp->status = NFS4ERR_INVAL;
		goto final;
	} else if (posix_length == (length4)(~0)) {
		posix_length = 0;	/* Posix to end of file  */
	}

	/* Find or create a lockowner */
	lo = findlockowner(cs->instp, &args->owner, &create);

	if (lo) {
		pid = lo->pid;
		if ((resp->status =
		    rfs4_client_sysid(lo->client, &sysid)) != NFS4_OK)
		goto out;
	} else {
		pid = 0;
		sysid = cs->instp->lockt_sysid;
	}
retry:
	flk.l_type = ltype;
	flk.l_whence = 0;		/* SEEK_SET */
	flk.l_start = args->offset;
	flk.l_len = posix_length;
	flk.l_sysid = sysid;
	flk.l_pid = pid;
	flag |= F_REMOTELOCK;

	/* Note that length4 is uint64_t but l_len and l_start are off64_t */
	if (flk.l_len < 0 || flk.l_start < 0) {
		resp->status = NFS4ERR_INVAL;
		goto out;
	}
	error = VOP_FRLOCK(cs->vp, F_GETLK, &flk, flag, (u_offset_t)0,
	    NULL, cs->cr, &ct);

	/*
	 * N.B. We map error values to nfsv4 errors. This is differrent
	 * than puterrno4 routine.
	 */
	switch (error) {
	case 0:
		if (flk.l_type == F_UNLCK)
			resp->status = NFS4_OK;
		else {
			if (mds_lock_denied(cs->instp, &resp->denied, &flk)
			    == NFS4ERR_EXPIRED)
				goto retry;
			resp->status = NFS4ERR_DENIED;
		}
		break;
	case EOVERFLOW:
		resp->status = NFS4ERR_INVAL;
		break;
	case EINVAL:
		resp->status = NFS4ERR_NOTSUPP;
		break;
	default:
		cmn_err(CE_WARN, "rfs4_op_lockt: unexpected errno (%d)",
		    error);
		resp->status = NFS4ERR_SERVERFAULT;
		break;
	}

out:
	if (lo)
		rfs4_lockowner_rele(lo);
	*cs->statusp = resp->status;

final:
	DTRACE_NFSV4_2(op__lockt__done, struct compound_state *, cs,
	    LOCKT4res *, resp);
}

/*
 * NFSv4.1 Server Sessions
 */

/* Renew Lease */
void
mds_refresh(mds_session_t *sp)
{
	rfs4_client_t	*cp;

	ASSERT(sp != NULL && sp->sn_clnt != NULL);
	rfs4_dbe_lock(sp->dbe);
	cp = sp->sn_clnt;
	sp->sn_laccess = gethrestime_sec();
	rfs4_dbe_unlock(sp->dbe);

	rfs4_dbe_hold(cp->dbe);
	rfs4_update_lease(cp);
	rfs4_client_rele(cp);
}


nfsstat4
mds_lease_chk(mds_session_t *sp)
{
	rfs4_client_t	*cp;
	nfsstat4	 error = NFS4_OK;

	/*
	 * If the client lease expired, go ahead and invalidate
	 * all the sessions associated with this clientid.
	 */
	ASSERT(sp != NULL && sp->sn_clnt != NULL);
	cp = sp->sn_clnt;

	if (rfs4_lease_expired(cp)) {
		error = NFS4ERR_BADSESSION;
	}
	return (error);
}

void
mds_get_server_owner(server_owner4 *sop)
{
	bzero(sop, sizeof (server_owner4));	/* XXX - for now */
}

/*
 * Rudimentary server implementation (XXX - for now)
 */
void
mds_get_server_impl_id(EXCHANGE_ID4resok *resp)
{
	timestruc_t	 currtime;
	char		*sol_impl = "Solaris NFSv4.1 Server Implementation";
	char		*sol_idom = "nfsv41.ietf.org";
	void		*p;
	uint_t		 len = 0;
	nfs_impl_id4	*nip;

	resp->eir_server_impl_id.eir_server_impl_id_len = 1;
	nip = kmem_zalloc(sizeof (nfs_impl_id4), KM_SLEEP);
	resp->eir_server_impl_id.eir_server_impl_id_val = nip;

	/* Domain */
	nip->nii_domain.utf8string_len = len = strlen(sol_idom);
	p = kmem_zalloc(len * sizeof (char), KM_SLEEP);
	nip->nii_domain.utf8string_val = p;
	bcopy(sol_idom, p, len);

	/* Implementation */
	nip->nii_name.utf8string_len = len = strlen(sol_impl);
	p = kmem_zalloc(len * sizeof (char), KM_SLEEP);
	nip->nii_name.utf8string_val = p;
	bcopy(sol_impl, p, len);

	/* Time */
	gethrestime(&currtime);
	(void) nfs4_time_vton(&currtime, &nip->nii_date);
}

/*
 * Principal handling routines
 */
void
rfs4_free_cred_princ(rfs4_client_t *cp)
{
	cred_princ_t		*p;
	rpc_gss_principal_t	 ppl;

	ASSERT(cp != NULL);
	if ((p = cp->cr_set) == NULL)
		return;

	switch (p->cp_aflavor) {
	case AUTH_DES:
		kmem_free(p->cp_princ, strlen(p->cp_princ) + 1);
		break;

	case RPCSEC_GSS:
		ppl = (rpc_gss_principal_t)p->cp_princ;
		kmem_free(ppl, ppl->len + sizeof (int));
		break;
	}
	kmem_free(p, sizeof (cred_princ_t));
	cp->cr_set = NULL;
}

static rpc_gss_principal_t
rfs4_dup_princ(rpc_gss_principal_t ppl)
{
	rpc_gss_principal_t	pdup;
	int			len;

	if (ppl == NULL)
		return (NULL);

	len = sizeof (int) + ppl->len;
	pdup = (rpc_gss_principal_t)kmem_alloc(len, KM_SLEEP);
	bcopy(ppl, pdup, len);
	return (pdup);
}

void
rfs4_set_cred_princ(cred_princ_t **pp, struct compound_state *cs)
{
	cred_princ_t	*p;
	caddr_t		 t;

	ASSERT(pp != NULL);


	if (*pp == NULL)
		*pp = kmem_zalloc(sizeof (cred_princ_t), KM_SLEEP);

	p = *pp;

	p->cp_cr = crdup(cs->basecr);
	p->cp_aflavor = cs->req->rq_cred.oa_flavor;
	p->cp_secmod = cs->nfsflavor;	/* secmod != flavor for RPCSEC_GSS */

	/*
	 * Set principal as per security flavor
	 */
	switch (p->cp_aflavor) {
	case AUTH_DES:
		p->cp_princ = kstrdup(cs->principal);
		break;

	case RPCSEC_GSS:
		t = (caddr_t)rfs4_dup_princ((rpc_gss_principal_t)cs->principal);
		p->cp_princ = (caddr_t)t;
		break;

	case AUTH_SYS:
	case AUTH_NONE:
	default:
		break;
	}
}

/* returns 0 if no match; or 1 for a match */
int
rfs4_cmp_cred_princ(cred_princ_t *p, struct compound_state *cs)
{
	int			 rc = 0;
	rpc_gss_principal_t	 recp;		/* cached clnt princ */
	rpc_gss_principal_t	 ibrp;		/* inbound req princ */


	if (p == NULL)
		return (rc);	/* nothing to compare with */

	if (p->cp_aflavor != cs->req->rq_cred.oa_flavor)
		return (rc);

	if (p->cp_secmod != cs->nfsflavor)
		return (rc);

	if (crcmp(p->cp_cr, cs->basecr))
		return (rc);

	switch (p->cp_aflavor) {
	case AUTH_DES:
		rc = (strcmp(p->cp_princ, cs->principal) == 0);
		break;

	case RPCSEC_GSS:
		recp = (rpc_gss_principal_t)p->cp_princ;
		ibrp = (rpc_gss_principal_t)cs->principal;

		if (recp->len != ibrp->len)
			break;
		rc = (bcmp(recp->name, ibrp->name, ibrp->len) == 0);
		break;

	case AUTH_SYS:
	case AUTH_NONE:
	default:
		rc = 1;
		break;
	}
	return (rc);
}

/* { co_ownerid, co_verifier, principal, clientid, confirmed } */
rfs4_client_t *
client_record(nfs_client_id4 *cip, struct compound_state *cs)
{
	rfs4_client_t	*ncp;
	bool_t		 create = TRUE;

	/*
	 * 1. co_ownerid
	 * 2. co_verifier
	 */
	ncp = findclient(cs->instp, cip, &create, NULL);

	/* 3. principal */
	if (ncp != NULL)
		rfs4_set_cred_princ(&ncp->cr_set, cs);

	/*
	 * Both of the following items of the 5-tuple are built as
	 * part of creating the rfs4_client_t.
	 *
	 * 4. clientid;		created as part of findclient()
	 * 5. confirmed;	cp->need_confirmed is initialized to TRUE
	 */
	return (ncp);
}

rfs4_client_t *
client_lookup(nfs_client_id4 *cip, struct compound_state *cs)
{
	bool_t	create = FALSE;

	return (findclient(cs->instp, cip, &create, NULL));
}

bool_t
nfs_clid4_cmp(nfs_client_id4 *s1, nfs_client_id4 *s2)
{
	if (s1->verifier != s2->verifier)
		return (FALSE);
	if (bcmp(s1->id_val, s2->id_val, s2->id_len))
		return (FALSE);
	return (TRUE);
}

/*
 * Compute the "use bits", i.e. the flags specifying the permissible
 * regular, MDS, and data server ops for the returned clientid.
 *
 * The minorversion1 specification allows a server implementor two
 * alternatives: allow PNFS_MDS and PNFS_DS on the same clientid, or
 * force the client to create separate clientids to distinguish
 * MDS versus DS operations.
 *
 * Our design distinguishes operations based upon filehandle, and thus
 * there is no reason to force the client to create separate clientids.
 * Thus, we give the client as much as possible, while keeping the result
 * within the allowed combinations as specified in the specification.
 *
 * Our constraints are: use a subset of the client's request, unless
 * the client requested nothing, in which case we may return any
 * legal combination; and, the combination of NON_PNFS and PNFS_MDS
 * may not both be set in the results.  These constraints are reflected
 * in the ASSERT()s at the end.
 */

static uint32_t
compute_use_pnfs_flags(uint32_t request)
{
	uint32_t rc;

	/* Start with the client's initial request */
	rc = request & EXCHGID4_FLAG_MASK_PNFS;

	/* If the client requested nothing, return the most permissive. */
	if (rc == 0) {
		rc = (EXCHGID4_FLAG_USE_PNFS_MDS | EXCHGID4_FLAG_USE_PNFS_DS);
		goto done;
	}

	/* Don't permit the illegal combination of MDS and NON_PNFS */
	if ((rc &
	    (EXCHGID4_FLAG_USE_NON_PNFS | EXCHGID4_FLAG_USE_PNFS_MDS)) ==
	    (EXCHGID4_FLAG_USE_NON_PNFS | EXCHGID4_FLAG_USE_PNFS_MDS))
		rc &= ~EXCHGID4_FLAG_USE_NON_PNFS;

done:
	ASSERT(((request & EXCHGID4_FLAG_MASK_PNFS) == 0) ||
	    ((rc & ~(request & EXCHGID4_FLAG_MASK_PNFS)) == 0));
	ASSERT((rc & (EXCHGID4_FLAG_USE_NON_PNFS | EXCHGID4_FLAG_USE_PNFS_MDS))
	    != (EXCHGID4_FLAG_USE_NON_PNFS | EXCHGID4_FLAG_USE_PNFS_MDS));
	ASSERT(rc != 0);

	return (rc);
}

/* XXX - NOTE: EXCHANGE_ID conforms to draft-19 behavior */

/*ARGSUSED*/
void
mds_op_exchange_id(nfs_argop4 *argop, nfs_resop4 *resop,
    struct svc_req *req, compound_state_t *cs)
{
	EXCHANGE_ID4args	*args = &argop->nfs_argop4_u.opexchange_id;
	EXCHANGE_ID4res		*resp = &resop->nfs_resop4_u.opexchange_id;
	EXCHANGE_ID4resok	*rok = &resp->EXCHANGE_ID4res_u.eir_resok4;
	rfs4_client_t		*cp;
	rfs4_client_t		*ocp;
	bool_t			 update;
	client_owner4		*cop;
	nfs_client_id4		*cip;
	verifier4		 old_verifier_arg;

	DTRACE_NFSV4_2(op__exchange__id__start,
	    struct compound_state *, cs,
	    EXCHANGE_ID4args *, args);

	/*
	 * EXCHANGE_ID's may be preceded by SEQUENCE
	 * Check that eia_flags only has "valid" spec bits
	 * and that no 'eir_flag' ONLY bits are specified.
	 */
	if (args->eia_flags & ~EXID4_FLAG_MASK ||
	    args->eia_flags & EXID4_FLAG_INVALID_ARGS) {
		*cs->statusp = resp->eir_status = NFS4ERR_INVAL;
		goto final;
	}

	update = (args->eia_flags & EXCHGID4_FLAG_UPD_CONFIRMED_REC_A);
	cop = &args->eia_clientowner;
	cip = (nfs_client_id4 *)cop;

	/*
	 * Refer to Section 18.35.4 of draft 19
	 */
	cp = client_lookup(cip, cs);
	if (cp == NULL) {		/* no record exists */
		if (!update) {
case1:			/* case 1 - utok */
			cp = client_record(cip, cs);
			ASSERT(cp != NULL);
			*cs->statusp = resp->eir_status = NFS4_OK;
			rok->eir_clientid = cp->clientid;
			rok->eir_sequenceid = cp->contrived.xi_sid;
			goto out;
		}
		/* no record and trying to update */
		*cs->statusp = resp->eir_status = NFS4ERR_NOENT;
		goto final;
	}

	/* record exists */
	old_verifier_arg = cp->nfs_client.verifier;
	if (CLID_REC_CONFIRMED(cp)) {
		if (!update) {
			if (!rfs4_cmp_cred_princ(cp->cr_set, cs)) {
				/* case 3 */
				if (rfs4_lease_expired(cp)) {
					rfs4_client_close(cp);
					goto case1;
				}
				/*
				 * case 3: clid_in_use - utok
				 * old_client_ret has unexpired lease w/state.
				 */
				*cs->statusp = NFS4ERR_CLID_INUSE;
				resp->eir_status = NFS4ERR_CLID_INUSE;
				rfs4_client_rele(cp);
				goto final;

			} else if (nfs_clid4_cmp(&cp->nfs_client, cip)) {
				/* case 2 - utok */
				*cs->statusp = NFS4_OK;
				resp->eir_status = NFS4_OK;
				rok->eir_clientid = cp->clientid;
				rok->eir_sequenceid = cp->contrived.xi_sid;
				/* trickle down to "out" */

			} else if (old_verifier_arg != cip->verifier) {
				/* case 5 - utok */
				/*
				 * previous incarnation of clientid is first
				 * hidden such that any subsequent lookups
				 * will not find it in DB, then the current
				 * reference to it is dropped; this will
				 * force the reaper thread to clean it up.
				 */
				ocp = cp;
				rfs4_dbe_hide(ocp->dbe);
				rfs4_client_rele(ocp);

				cp = client_record(cip, cs);
				ASSERT(cp != NULL);
				*cs->statusp = resp->eir_status = NFS4_OK;
				rok->eir_clientid = cp->clientid;
				rok->eir_sequenceid = cp->contrived.xi_sid;
				/* trickle down to "out" */
			} else {
				/* something is really wacky in srv state */
				*cs->statusp = resp->eir_status =
				    NFS4ERR_SERVERFAULT;
				rfs4_client_rele(cp);
				goto final;
			}

		} else { /* UPDATE */
			if (rfs4_cmp_cred_princ(cp->cr_set, cs)) {
				if (old_verifier_arg == cip->verifier) {
					/* case 6 - utok */
					*cs->statusp = NFS4_OK;
					resp->eir_status = NFS4_OK;
					rok->eir_clientid = cp->clientid;
					rok->eir_sequenceid =
					    cp->contrived.xi_sid;
					/* trickle down to "out" */
				} else {
					/* case 8 - utok */
					*cs->statusp = NFS4ERR_NOT_SAME;
					resp->eir_status = NFS4ERR_NOT_SAME;
					rfs4_client_rele(cp);
					goto final;
				}
			} else {
				/* case 9 - utok */
				*cs->statusp = resp->eir_status = NFS4ERR_PERM;
				rfs4_client_rele(cp);
				goto final;
			}
		}

	} else { /* UNCONFIRMED */
		if (!update) {
			/* case 4 - utok */
			rfs4_client_close(cp);
			goto case1;

		} else {
			/* case 7 - utok */
			*cs->statusp = resp->eir_status = NFS4ERR_NOENT;
			rfs4_client_rele(cp);
			goto final;
		}
	}

out:
	rok->eir_flags = 0;
	if (resp->eir_status == NFS4_OK && CLID_REC_CONFIRMED(cp))
		rok->eir_flags |= EXCHGID4_FLAG_CONFIRMED_R;

	/*
	 * State Protection Mojo
	 */
	cp->state_prot.sp_type = args->eia_state_protect.spa_how;
	switch (cp->state_prot.sp_type) {
	case SP4_NONE:
		break;

	case SP4_MACH_CRED:
		/* XXX - Some of Karen's secret sauce here... */
		break;

	case SP4_SSV:
		/* XXX - ... and here */
		if (args->ssv_args.ssp_ops.spo_must_allow & OP_EXCHANGE_ID)
			/*
			 * if the client ID was created specifying SP4_SSV
			 * state protection and EXCHANGE_ID as the one of
			 * the operations in spo_must_allow, then server MUST
			 * authorize EXCHANGE_IDs with the SSV principal in
			 * addition to the principal that created the client
			 * ID.
			 */
			/* EMPTY */;
		break;
	}

	/*
	 * XXX - Still need to clarify if the NFSv4.1
	 *	 server will be supporting referrals.
	 */
	if (args->eia_flags & EXCHGID4_FLAG_SUPP_MOVED_REFER)
		/* EMPTY */;

	/*
	 * Migration/Replication not (yet) supported
	 */
	if (args->eia_flags & EXCHGID4_FLAG_SUPP_MOVED_MIGR)
		rok->eir_flags &= ~EXCHGID4_FLAG_SUPP_MOVED_MIGR;

	bzero(&rok->eir_server_scope, sizeof (uint_t) + sizeof (char *));

	/*
	 * Add the appropriate "use_pnfs" flags.
	 */
	rok->eir_flags |= compute_use_pnfs_flags(args->eia_flags);

	/* force no state protection for now */
	rok->eir_state_protect.spr_how = SP4_NONE;

	mds_get_server_owner(&rok->eir_server_owner);

	/* Implementation specific mojo */
	if (args->eia_client_impl_id.eia_client_impl_id_len != 0)
		/* EMPTY */;

	/* XXX - jw - best guess */
	rfs4_ss_clid(cs, cp, req);

	/* Server's implementation */
	mds_get_server_impl_id(rok);

	/*
	 * XXX - jw - best guess
	 * Check to see if client can perform reclaims
	 */
	rfs4_ss_chkclid(cs, cp);

	rfs4_client_rele(cp);

final:
	DTRACE_NFSV4_2(op__exchange__id__done,
	    struct compound_state *, cs,
	    EXCHANGE_ID4res *, resp);
}

/*ARGSUSED*/
void
mds_op_create_session(nfs_argop4 *argop, nfs_resop4 *resop,
			struct svc_req *req, compound_state_t *cs)
{
	CREATE_SESSION4args	*args = &argop->nfs_argop4_u.opcreate_session;
	CREATE_SESSION4res	*resp = &resop->nfs_resop4_u.opcreate_session;
	CREATE_SESSION4resok	*rok = &resp->CREATE_SESSION4res_u.csr_resok4;
	CREATE_SESSION4resok	*crp;
	rfs4_client_t		*cp;
	mds_session_t		*sp;
	session41_create_t	 sca;
	sequenceid4		 stseq;
	sequenceid4		 agseq;

	DTRACE_NFSV4_2(op__create__session__start,
	    struct compound_state *, cs,
	    CREATE_SESSION4args *, args);


	/*
	 * A CREATE_SESSION request can be prefixed by OP_SEQUENCE.
	 * In this case, the newly created session has no relation
	 * to the sessid used for the OP_SEQUENCE.
	 */

	/*
	 * Find the clientid
	 */
	cp = findclient_by_id(cs->instp, args->csa_clientid);
	if (cp == NULL) {
		*cs->statusp = resp->csr_status = NFS4ERR_STALE_CLIENTID;
		goto final;
	}

	/*
	 * Make sure the lease is still valid.
	 */
	if (rfs4_lease_expired(cp)) {
		rfs4_client_close(cp);
		*cs->statusp = resp->csr_status = NFS4ERR_STALE_CLIENTID;
		goto final;
	}

	/*
	 * Sequenceid processing (handling replay's, etc)
	 */
	agseq = args->csa_sequence;
	stseq = cp->contrived.cs_slot.seqid;
	if (stseq == agseq) {
		/*
		 * If the same sequenceid, then must be a replay of a
		 * previous CREATE_SESSION; return the cached result.
		 */
replay:
		crp = (CREATE_SESSION4resok *)&cp->contrived.cs_res;
		*cs->statusp = resp->csr_status = cp->contrived.cs_slot.status;
		rok->csr_sequence = cp->contrived.cs_slot.seqid;
		bcopy(crp->csr_sessionid, rok->csr_sessionid,
		    sizeof (sessionid4));
		rok->csr_flags = crp->csr_flags;
		rok->csr_fore_chan_attrs = crp->csr_fore_chan_attrs;
		rok->csr_back_chan_attrs = crp->csr_back_chan_attrs;

		rfs4_update_lease(cp);
		rfs4_client_rele(cp);
		goto final;
	}

	if (stseq + 1 == agseq) {
		/* Valid sequencing */
		cp->contrived.cs_slot.seqid = args->csa_sequence;
	} else {
		/*
		 * No way to differentiate MISORD_NEWREQ vs. MISORD_REPLAY,
		 * so anything else, we simply treat as SEQ_MISORDERED.
		 */
		*cs->statusp = resp->csr_status = NFS4ERR_SEQ_MISORDERED;
		rfs4_client_rele(cp);
		goto final;
	}

	/*
	 * Clientid confirmation
	 */
	if (cp->need_confirm && cp->clientid == args->csa_clientid) {
		if (rfs4_cmp_cred_princ(cp->cr_set, cs)) {
			cp->need_confirm = FALSE;
		} else {
			*cs->statusp = resp->csr_status = NFS4ERR_CLID_INUSE;
			rfs4_client_rele(cp);
			goto final;
		}
	}

	/*
	 * Session creation
	 */
	sca.cs_error = 0;
	sca.cs_xprt = req->rq_xprt;
	sca.cs_client = cp;
	sca.cs_aotw = *args;
	sp = mds_createsession(cs->instp, &sca);

	if (sca.cs_error) {
		*cs->statusp = resp->csr_status = sca.cs_error;
		rfs4_client_rele(cp);
		if (sp != NULL)
			rfs41_session_rele(sp);
		goto final;
	}

	if (sp == NULL) {
		*cs->statusp = resp->csr_status = NFS4ERR_SERVERFAULT;
		rfs4_client_rele(cp);
		goto final;
	}

	/*
	 * Need to store the result in the rfs4_client_t's contrived
	 * result slot and then respond from there. This way, when the
	 * csa_sequence == contrived.cc_sid, we can return the latest
	 * cached result. (see replay: above)
	 */
	crp = (CREATE_SESSION4resok *)&cp->contrived.cs_res;
	*cs->statusp = resp->csr_status =
	    cp->contrived.cs_slot.status = NFS4_OK;
	rok->csr_sequence = cp->contrived.xi_sid;
	bcopy(sp->sn_sessid, rok->csr_sessionid, sizeof (sessionid4));
	bcopy(sp->sn_sessid, crp->csr_sessionid, sizeof (sessionid4));
	rok->csr_flags = crp->csr_flags = sp->sn_csflags;

	/*
	 * XXX: struct assignment of channel4_attrs is broken because
	 * ca_rdma_ird is specified as a single element array.  A struct
	 * assignment will copy the ca_rdma_ird array ptr to multiple args/
	 * res structs, and the ptr will be free'd multiple times.
	 * Without RDMA, ca_rdma_ird is a zero element array so its ptr
	 * is NULL (which is why this doesn't cause problems right now).
	 *
	 * Struct assignment is convenient, and it would be best to enable
	 * it by creating an in-kernel channel4_attrs struct which didn't
	 * contain the single element array, but just contained the inbound
	 * receive queue depth.  Let the XDR encode/decode funcs convert
	 * from the in-kernel form to the OTW form.
	 */
	rok->csr_fore_chan_attrs =
	    crp->csr_fore_chan_attrs = sp->sn_fore->cn_attrs;
	rok->csr_back_chan_attrs = crp->csr_back_chan_attrs =
	    args->csa_back_chan_attrs;

	/* only send 1 callback at a time for now */
	rok->csr_back_chan_attrs.ca_maxrequests =
	    crp->csr_back_chan_attrs.ca_maxrequests = 1;
	rfs4_update_lease(cp);

	/*
	 * Don't hold the client struct or it will never be able to expire.
	 * If the client goes away w/o sending a destroy session (reboot
	 * or panic), the client struct must expire.
	 */
	rfs4_client_rele(cp);

	rfs41_session_rele(sp);

final:
	DTRACE_NFSV4_2(op__create__session__done,
	    struct compound_state *, cs,
	    CREATE_SESSION4res *, resp);
}

/*ARGSUSED*/
void
mds_op_destroy_session(nfs_argop4 *argop, nfs_resop4 *resop,
			struct svc_req *req, compound_state_t *cs)
{
	DESTROY_SESSION4args	*args = &argop->nfs_argop4_u.opdestroy_session;
	DESTROY_SESSION4res	*resp = &resop->nfs_resop4_u.opdestroy_session;
	mds_session_t		*sp;
	rfs4_client_t		*cp;

	DTRACE_NFSV4_2(op__destroy__session__start,
	    struct compound_state *, cs,
	    DESTROY_SESSION4args *, args);

	/*
	 *  XXXX: More work need on this function.
	 *
	 * (draft-17) - If this compound was started with
	 * SEQUENCE, make sure that the sessid referenced
	 * in the SEQUENCE op matches the sessid referenced
	 * in the DESTROY_SESSION op. If it DOES, then
	 * DESTROY_SESSION MUST be the last op in the COMPOUND.
	 *
	 * XXX - Still need to Code
	 * XXX - if the sessionid in SEQ differs
	 *	 from DESTROY_SESSION, we'll want to verify
	 *	 that both sessions are sharing the connection.
	 */
	if (cs->sp != NULL) {
		/* we must be in a compound with a sequence */
		if (bcmp(args->dsa_sessionid, cs->sp->sn_sessid,
		    sizeof (sessionid4)) == 0) {
			/*
			 * same sess as seq, must be last op in compound
			 */
			if ((cs->op_len - 1) != cs->op_ndx) {
				/*
				 * XXX - What's the right error to
				 * return if DESTROY_SESSION is NOT
				 * the last op in compound ??? Using
				 * UNSAFE_COMPOUND for now.
				 */
				*cs->statusp = resp->dsr_status =
				    NFS4ERR_UNSAFE_COMPOUND;
				goto final;
			}
			/* utok */
		}
	}

	/*
	 * Find session and check for clientid and lease expiration
	 */
	if ((sp =
	    mds_findsession_by_id(cs->instp, args->dsa_sessionid)) == NULL) {
		*cs->statusp = resp->dsr_status = NFS4ERR_BADSESSION;
		goto final;
	}

	/*
	 * Verify that "this" connection is associated
	 * w/the session being targeted for destruction.
	 */
	/* XXX - placeholder - XXX */

	/*
	 * Once we can trace back to the rfs4_client struct, verify the
	 * cred that was used to create the session matches and is in
	 * concordance w/the state protection type used.
	 */
	if ((cp = sp->sn_clnt) != NULL) {
		switch (cp->state_prot.sp_type) {
		case SP4_MACH_CRED:
			cmn_err(CE_NOTE, "op_destroy_session: SP4_MACH_CRED");
			if (!rfs4_cmp_cred_princ(cp->cr_set, cs)) {
				*cs->statusp = resp->dsr_status = NFS4ERR_PERM;
				goto final;
			}
			break;

		case SP4_SSV:
			/*
			 * XXX - Need some of Karen's secret ssv sauce
			 *	 here. For now, just allow the destroy.
			 */
			cmn_err(CE_NOTE, "op_destroy_session: SP4_SSV");
			break;

		case SP4_NONE:
			cmn_err(CE_NOTE, "op_destroy_session: SP4_NONE");

		default:
			break;
		}
	}

	/* session rele taken care of in mds_destroysession */
	*cs->statusp = resp->dsr_status = mds_destroysession(sp);

final:
	DTRACE_NFSV4_2(op__destroy__session__done,
	    struct compound_state *, cs,
	    DESTROY_SESSION4res *, resp);
}

/*ARGSUSED*/
void
mds_op_backchannel_ctl(nfs_argop4 *argop, nfs_resop4 *resop,
			struct svc_req *req, compound_state_t *cs)
{
}

/*
 * The thread will traverse the entire list pinging the connections
 * that need it and refreshing any stale/dead connections.
 */
static void
ping_cb_null_thr(mds_session_t *sp)
{
	CLIENT			*ch = NULL;
	struct timeval		 tv;
	enum clnt_stat		 cs;
	int 			conn_num, attempts = 5;

	tv.tv_sec = 30;
	tv.tv_usec = 0;


	if ((ch = rfs41_cb_getch(sp)) == NULL)
		goto out;

	/*
	 * Flag to let RPC know these are ping calls. RPC will only use
	 * untested connections.
	 */

	CLNT_CONTROL(ch, CLSET_CB_TEST, (void *)NULL);

	/*
	 * If another thread is working on the pings then
	 * just exit.
	 */

	rfs4_dbe_lock(sp->dbe);
	if (sp->sn_bc.pnginprog != 0) {
		rfs4_dbe_unlock(sp->dbe);
		goto out;
	}
	sp->sn_bc.pnginprog = 1;
	rfs4_dbe_unlock(sp->dbe);

	/*
	 * Get the number of untested conections
	 */

	if (!CLNT_CONTROL(ch, CLGET_CB_UNTESTED, (void *)&conn_num))
		goto out;

	/*
	 * If number of untested connections is zero, either
	 * - another thread's already tested it
	 * - a previously tested connection is being reused
	 * So no further testing is required
	 */

	if (conn_num == 0) {
		rfs4_dbe_lock(sp->dbe);
		sp->sn_bc.paths++;
		if (sp->sn_bc.pngcnt)
			sp->sn_bc.pngcnt--;
		rfs4_dbe_unlock(sp->dbe);
		goto out;
	}

call_again:
	while (conn_num-- > 0) {

		/*
		 * With CB_TEST flag set, RPC iterates over untested
		 * connections for each of these CLNT_CALL()
		 */

		cs = CLNT_CALL(ch, CB_NULL, xdr_void, NULL, xdr_void, NULL, tv);
		if (cs == RPC_SUCCESS) {
			rfs4_dbe_lock(sp->dbe);
			sp->sn_bc.paths++;
			sp->sn_bc.pngcnt--;
			rfs4_dbe_unlock(sp->dbe);
		}
	}

	rfs4_dbe_lock(sp->dbe);
	if (sp->sn_bc.paths == 0)
		sp->sn_bc.failed = 1;
	rfs4_dbe_unlock(sp->dbe);

	if (!CLNT_CONTROL(ch, CLGET_CB_UNTESTED, (void *)&conn_num))
		goto out;

	if (conn_num != 0) {
		/*
		 * Pause inbetween attempts and
		 * only try 5 times.
		 */
		attempts--;
		if (attempts > 0) {
			delay(2 * drv_usectohz(1000000));
			goto call_again;
		}
		DTRACE_PROBE(nfss41__i__cb_null_failed_attempts);
	}
out:
	rfs4_dbe_lock(sp->dbe);
	sp->sn_bc.pnginprog = 0;
	rfs4_dbe_unlock(sp->dbe);

	if (ch != NULL) {
		CLNT_CONTROL(ch, CLSET_CB_TEST_CLEAR, (void *)NULL);
		rfs41_cb_freech(sp, ch);
	}

	thread_exit();
}


/*
 * Process the SEQUENCE operation. The session pointer has already been
 * cached in the compound state, so we just dereference
 */
/*ARGSUSED*/
void
mds_op_sequence(nfs_argop4 *argop, nfs_resop4 *resop,
		struct svc_req *req, compound_state_t *cs)
{
	SEQUENCE4args		*args = &argop->nfs_argop4_u.opsequence;
	SEQUENCE4res		*resp = &resop->nfs_resop4_u.opsequence;
	SEQUENCE4resok		*rok  = &resp->SEQUENCE4res_u.sr_resok4;
	mds_session_t		*sp = cs ->sp;
	slot41_t		*slp;
	slotid4			 slot    = args->sa_slotid;
	nfsstat4		 status  = NFS4_OK;
	uint32_t		 cbstat = 0x0;
	rfs4_client_t		*cp;

	DTRACE_NFSV4_2(op__sequence__start,
	    struct compound_state *, cs,
	    SEQUENCE4args *, args);

	if (cs->sequenced > 1) {
		/*
		 * Spec Error ! If we detect a multi-SEQUENCE
		 * compound we halt processing here.
		 */
		*cs->statusp = resp->sr_status = NFS4ERR_SEQUENCE_POS;
		goto final;
	}

	cs->sequenced++;

	if ((status = mds_lease_chk(sp)) != NFS4_OK) {
		*cs->statusp = resp->sr_status = status;
		goto final;
	}

	/*
	 * If the back channel has been established...
	 *	. if the channel has _not_ been marked as failed _AND_
	 *	  there are connections that have pings outstanding,
	 *	  we go ahead and fire the thread to traverse all of
	 *	  the session's conns, issuing CB_NULL's to those that
	 *	  need a ping.
	 *	. if the channel is _not_ OK (ie. failed), then notify
	 *	  client that there is currently a problem with the CB
	 *	  path.
	 */
	rfs4_dbe_lock(sp->dbe);
	if (SN_CB_CHAN_EST(sp)) {
		if (SN_CB_CHAN_OK(sp)) {
			if (sp->sn_bc.pngcnt > 0 && !sp->sn_bc.pnginprog)
				(void) thread_create(NULL, 0, ping_cb_null_thr,
				    sp, 0, &p0, TS_RUN, minclsyspri);
		} else {
			cbstat |= SEQ4_STATUS_CB_PATH_DOWN;
		}
	}
	cs->cp = sp->sn_clnt;
	DTRACE_PROBE1(nfss41__i__compound_clid, clientid4, cs->cp->clientid);

	/*
	 * Valid range is [0, N-1]
	 */
	if (slot < 0 || slot >= sp->sn_slrc->sc_maxslot) {
		/* slot not in valid range */
		cmn_err(CE_WARN, "mds_op_sequence: Bad Slot");
		*cs->statusp = resp->sr_status = NFS4ERR_BADSLOT;
		goto sessrel;
	}

	/*
	 * valid slot !
	 *
	 * Duplicates/retransmissions have already been handled by
	 * rfs41_slrc_prologue(), so if we're here, it _must_ mean
	 * this is indeed a new request. We perform some sanity
	 * checks and return NFS4_OK if everything looks kosher;
	 * this reply will need to be cached by our caller.
	 */
	slp = &sp->sn_slrc->sc_slot[slot];

	/* XXX - still need to account for wraparound */
	if (args->sa_sequenceid != slp->seqid + 1) {
		cmn_err(CE_WARN, "mds_op_sequence: Misordered New Request");
		slp->status = NFS4ERR_SEQ_MISORDERED;
		*cs->statusp = resp->sr_status = NFS4ERR_SEQ_MISORDERED;
		goto sessrel;

	}

	if (args->sa_sequenceid == slp->seqid + 1) {
		/*
		 * New request.
		 */
		slp->status = NFS4_OK;	/* SLRC_NR_INPROG */
		slp->seqid = args->sa_sequenceid;
		if (slp->p != NULL) {
			/*
			 * slot previously used to return recallable state;
			 * since slot reused (NEW request) we are guaranteed
			 * the client saw the reply, so it's safe to nuke the
			 * race-detection accounting info.
			 */
			rfs41_rs_erase(slp->p);
			slp->p = NULL;
		}
	}

	/*
	 * Update access time and lease
	 */
	cs->slotno = slot;
	cs->seqid = slp->seqid;
	cp = sp->sn_clnt;
	sp->sn_laccess = gethrestime_sec();

	rfs4_dbe_hold(cp->dbe);
	rfs4_update_lease(cp);
	rfs4_client_rele(cp);

	/*
	 * Let's keep it simple for now
	 */
	bcopy(sp->sn_sessid, rok->sr_sessionid, sizeof (sessionid4));
	rok->sr_sequenceid = slp->seqid;
	rok->sr_slotid = sp->sn_slrc->sc_slotid = slot;
	rok->sr_highest_slotid = sp->sn_slrc->sc_maxslot;
	rok->sr_target_highest_slotid = sp->sn_slrc->sc_maxslot;
	rok->sr_status_flags |= cbstat;
	*cs->statusp = resp->sr_status = NFS4_OK;

sessrel:
	rfs4_dbe_unlock(sp->dbe);

final:
	DTRACE_NFSV4_2(op__sequence__done,
	    struct compound_state *, cs,
	    SEQUENCE4res *, resp);
}

void
rfs41_bc_setup(mds_session_t *sp)
{
	sess_channel_t	*bcp;
	sess_bcsd_t	*bsdp;
	bool_t		bcp_init = FALSE;

	ASSERT(sp != NULL);
	rfs4_dbe_lock(sp->dbe);

	/*
	 * If sn_back == NULL, setup and initialize the
	 * back channel.
	 */
	if (sp->sn_back == NULL) {
		bcp = rfs41_create_session_channel(CDFS4_BACK);
		bcp_init = TRUE;
	} else {
		bcp = sp->sn_back;
	}
	rfs4_dbe_unlock(sp->dbe);
	ASSERT(bcp != NULL);

	rw_enter(&bcp->cn_lock, RW_WRITER);
	bcp->cn_dir |= CDFS4_BACK;

	/* now set the conn's state so we know a ping is needed */
	atomic_add_32(&sp->sn_bc.pngcnt, 1);

	/*
	 * Initialize back channel specific data
	 */
	if (bcp_init) {
		if ((bsdp = CTOBSD(bcp)) == NULL) {
			cmn_err(CE_PANIC, "Back Chan Spec Data Not Set\t"
			    "<Internal Inconsistency>");
		}
		rw_enter(&bsdp->bsd_rwlock, RW_WRITER);
		/*
		 * XXX - 08/15/2008 (rick) if we're barely creating the
		 *	back channel, then the back channel attrs should
		 *	have been saved off by the originating CREATE_SESSION
		 *	call. If that's not the case, default to MAXSLOTS.
		 */
		bsdp->bsd_stok = sltab_create(MAXSLOTS);	/* XXX - 4now */
		rw_exit(&bsdp->bsd_rwlock);
	}
	rw_exit(&bcp->cn_lock);

	/*
	 * If no back channel yet, then we must've created one above.
	 * Make sure we set the session's back channel appropriately.
	 */
	rfs4_dbe_lock(sp->dbe);
	if (sp->sn_back == NULL)
		sp->sn_back = bcp;
	rfs4_dbe_unlock(sp->dbe);
}

/*ARGSUSED*/
void
mds_op_bind_conn_to_session(nfs_argop4 *argop, nfs_resop4 *resop,
			struct svc_req *req, compound_state_t *cs)
{
	BIND_CONN_TO_SESSION4args	*args = &argop->a_bc2s;
	BIND_CONN_TO_SESSION4res	*resp = &resop->r_bc2s;
	BIND_CONN_TO_SESSION4resok	*rok = &resp->rok_bc2s;
	mds_session_t			*sp;
	SVCMASTERXPRT			*mxprt;
	rpcprog_t			 prog;
	SVCCB_ARGS			cbargs;

	DTRACE_NFSV4_2(op__bind__conn__to__session__start,
	    struct compound_state *, cs,
	    BIND_CONN_TO_SESSION4args *, args);

	/*
	 * Find session and check for clientid and lease expiration
	 */
	if ((sp = mds_findsession_by_id(cs->instp, args->bctsa_sessid))
	    == NULL) {
		*cs->statusp = resp->bctsr_status = NFS4ERR_BADSESSION;
		goto final;
	}
	mds_refresh(sp);

	bzero(rok, sizeof (SEQUENCE4resok));

	rfs4_dbe_lock(sp->dbe);
	prog = sp->sn_bc.progno;
	rfs4_dbe_unlock(sp->dbe);

	rok->bctsr_use_conn_in_rdma_mode = FALSE;
	mxprt = (SVCMASTERXPRT *)req->rq_xprt->xp_master;
	switch (args->bctsa_dir) {
	case CDFC4_FORE:
	case CDFC4_FORE_OR_BOTH:
		/* always map to Fore */
		rok->bctsr_dir = CDFS4_FORE;
		SVC_CTL(
		    req->rq_xprt, SVCCTL_SET_TAG, (void *)sp->sn_sessid);
		break;

	case CDFC4_BACK:
	case CDFC4_BACK_OR_BOTH:
		/* always map to Back */

		rok->bctsr_dir = CDFS4_BACK;
		rfs41_bc_setup(sp);
		SVC_CTL(
		    req->rq_xprt, SVCCTL_SET_TAG, (void *)sp->sn_sessid);

		cbargs.xprt = mxprt;
		cbargs.prog = prog;
		cbargs.vers = NFS_CB;
		cbargs.family = AF_INET;
		cbargs.tag = (void *)sp->sn_sessid;

		SVC_CTL(req->rq_xprt, SVCCTL_SET_CBCONN, (void *)&cbargs);

		/* Recall: these bits denote # of active back chan conns */
		rfs41_seq4_hold(&sp->sn_seq4, SEQ4_STATUS_CB_PATH_DOWN_SESSION);
		rfs41_seq4_hold(&sp->sn_clnt->seq4, SEQ4_STATUS_CB_PATH_DOWN);
		break;

	default:
		break;
	}

	/*
	 * Handcraft the results !
	 */
	bcopy(sp->sn_sessid, rok->bctsr_sessid, sizeof (sessionid4));
	*cs->statusp = NFS4_OK;
	rfs41_session_rele(sp);

final:
	DTRACE_NFSV4_2(op__bind__conn__to__session__done,
	    struct compound_state *, cs,
	    BIND_CONN_TO_SESSION4res *, resp);
}

/* located in nfs4_state */
extern void mds_mpd_list(rfs4_entry_t, void *);


nfsstat4
mds_getdevicelist(nfs_server_instance_t *instp,
    deviceid4 **dlpp, int *len)
{
	mds_device_list_t mdl;
	int sz;

	/*
	 * no table updates till we're done with this...
	 */
	rw_enter(&instp->mds_mpd_lock, RW_READER);
	sz = instp->mds_mpd_tab->count * sizeof (deviceid4);

	mdl.dl = kmem_alloc(sz, KM_SLEEP);
	mdl.count = 0;

	rfs4_dbe_walk(instp->mds_mpd_tab, mds_mpd_list, &mdl);
	rw_exit(&instp->mds_mpd_lock);

	*len = mdl.count;
	*dlpp = mdl.dl;

	return (NFS4_OK);
}

/*ARGSUSED*/
static void
mds_op_get_devlist(nfs_argop4 *argop,
	nfs_resop4 *resop,
	struct svc_req *reqp,
	compound_state_t *cs)
{
	GETDEVICELIST4res *resp = &resop->nfs_resop4_u.opgetdevicelist;
	deviceid4 *devlist = NULL;
	nfsstat4 nfsstat = NFS4_OK;
	int len = 0;

	DTRACE_NFSV4_1(op__getdevicelist__start,
	    struct compound_state *, cs);

	if (cs->vp == NULL) {
		*cs->statusp = resp->gdlr_status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	/* mds_getdevicelist will allocate space for devlist. */
	nfsstat = mds_getdevicelist(cs->instp, &devlist, &len);
	*cs->statusp = resp->gdlr_status = nfsstat;

	if (nfsstat == NFS4_OK) {
		resp->GETDEVICELIST4res_u.gdlr_resok4.gdlr_cookie = 1234;
		resp->GETDEVICELIST4res_u.gdlr_resok4.gdlr_cookieverf = 1234;
		resp->GETDEVICELIST4res_u.gdlr_resok4.gdlr_deviceid_list.\
		    gdlr_deviceid_list_len = len;
		resp->GETDEVICELIST4res_u.gdlr_resok4.gdlr_deviceid_list.\
		    gdlr_deviceid_list_val = devlist;
		resp->GETDEVICELIST4res_u.gdlr_resok4.gdlr_eof = TRUE;
	}

final:
	DTRACE_NFSV4_2(op__getdevicelist__done,
	    struct compound_state *, cs,
	    GETDEVICELIST4res *, resp);
}

/*ARGSUSED*/
static void
mds_op_get_devlist_free(nfs_resop4 *resop)
{

}

/*ARGSUSED*/
static void
mds_op_get_devinfo(nfs_argop4 *argop, nfs_resop4 *resop, struct svc_req *reqp,
    compound_state_t *cs)
{
	ba_devid_t devid;
	mds_mpd_t *mp;

	GETDEVICEINFO4args *argp = &argop->nfs_argop4_u.opgetdeviceinfo;
	GETDEVICEINFO4res *resp = &resop->nfs_resop4_u.opgetdeviceinfo;

	DTRACE_NFSV4_2(op__getdeviceinfo__start,
	    struct compound_state *, cs,
	    GETDEVICEINFO4args *, argp);

	/* preset for failure */
	*cs->statusp = resp->gdir_status = NFS4ERR_INVAL;

	if (argp->gdia_layout_type != LAYOUT4_NFSV4_1_FILES)
		goto final;

	bcopy(&argp->gdia_device_id, &devid, sizeof (devid));

	mp = mds_find_mpd(cs->instp, devid.i.did);

	if (mp == NULL)
		goto final;

	/*
	 * Do the too_small check
	 */
	if (mp->mpd_encoded_len > argp->gdia_maxcount) {
		*cs->statusp = resp->gdir_status = NFS4ERR_TOOSMALL;
		goto final;
	}

	/*
	 * If the client requests notifications, then say we're
	 * willing to send them.
	 */
	resp->GETDEVICEINFO4res_u.gdir_resok4.gdir_notification = 0;
	if (argp->gdia_notify_types & NOTIFY_DEVICEID4_CHANGE_MASK)
		resp->GETDEVICEINFO4res_u.gdir_resok4.gdir_notification |=
		    NOTIFY_DEVICEID4_CHANGE_MASK;
	if (argp->gdia_notify_types & NOTIFY_DEVICEID4_DELETE_MASK)
		resp->GETDEVICEINFO4res_u.gdir_resok4.gdir_notification |=
		    NOTIFY_DEVICEID4_DELETE_MASK;

	resp->GETDEVICEINFO4res_u.gdir_resok4.gdir_device_addr.\
	    da_layout_type = LAYOUT4_NFSV4_1_FILES;
	resp->GETDEVICEINFO4res_u.gdir_resok4.gdir_device_addr.\
	    da_addr_body.da_addr_body_len = mp->mpd_encoded_len;
	resp->GETDEVICEINFO4res_u.gdir_resok4.gdir_device_addr.\
	    da_addr_body.da_addr_body_val = mp->mpd_encoded_val;

	*cs->statusp = resp->gdir_status = NFS4_OK;

final:
	DTRACE_NFSV4_2(op__getdeviceinfo__done,
	    struct compound_state *, cs,
	    GETDEVICEINFO4res *, resp);
}


/*
 * XXXX: Needs to pass in MDS_SID
 */
extern bool_t xdr_ds_fh_fmt(XDR *, mds_ds_fh *);
int
mds_alloc_ds_fh(fsid_t fsid, nfs41_fid_t fid, nfs_fh4 *fhp)
{
	mds_ds_fh dsfh;
	unsigned long hostid = 0;

	(void) ddi_strtoul(hw_serial, NULL, 10, &hostid);

	bzero(&dsfh, sizeof (mds_ds_fh));

	dsfh.vers = DS_FH_v1;
	dsfh.type = FH41_TYPE_DMU_DS;
	dsfh.fh.v1.mds_id = (uint64_t)hostid;

	/*
	 * XXX - Still need the MDS SID portion of the
	 * file handle to be filled in.
	 */
	/*
	 * Use the FSID for the MDS Dataset ID for now.  In the future
	 * the MDS Dataset ID will be made up of information from the
	 * ZFS dataset (i.e. dataset guid) on the MDS.  Regardless,
	 * the mds_dataset_id portion of the file handle is opaque so
	 * we can put what we want there (as long as it identifies the
	 * MDS file system).
	 */
	dsfh.fh.v1.mds_dataset_id.len = sizeof (fsid);
	bcopy(&fsid, dsfh.fh.v1.mds_dataset_id.val,
	    dsfh.fh.v1.mds_dataset_id.len);

	dsfh.fh.v1.mds_fid.len = fid.len;
	bcopy(fid.val, dsfh.fh.v1.mds_fid.val, fid.len);

	if (!xdr_encode_ds_fh(&dsfh, fhp))
		return (EINVAL);

	return (0);
}

int mds_layout_is_dense = 1;

void
fake_spe(nfs_server_instance_t *instp, mds_layout_t **flp)
{
	extern int mds_max_lo_devs;
	extern mds_layout_t *mds_gen_default_layout(nfs_server_instance_t *,
	    int);

	mds_layout_t *lp;
	bool_t create = FALSE;
	int key = 1;

	if (flp == NULL)
		return;

	*flp = NULL;

	rw_enter(&instp->mds_layout_lock, RW_READER);
	lp = (mds_layout_t *)rfs4_dbsearch(instp->mds_layout_idx,
	    (void *)(uintptr_t)key, &create, NULL, RFS4_DBS_VALID);
	rw_exit(&instp->mds_layout_lock);

	if (lp == NULL)
		lp = mds_gen_default_layout(instp, mds_max_lo_devs);

	if (lp != NULL)
		*flp = lp;
}

/*
 * get file layout XXX? should the rfs4_file_t be cached in
 * compound state
 */
nfsstat4
mds_get_flo(struct compound_state *cs, mds_layout_t **flopp)
{
	rfs4_file_t *fp;

	ASSERT(cs->vp);
	ASSERT(flopp);

	mutex_enter(&cs->vp->v_lock);
	fp = (rfs4_file_t *)vsd_get(cs->vp, cs->instp->vkey);
	mutex_exit(&cs->vp->v_lock);

	/* Odd.. no rfs4_file_t for the vnode.. */
	if (fp == NULL)
		return (NFS4ERR_LAYOUTUNAVAILABLE);

	/* do we have a odl already ? */
	if (fp->flp == NULL) {
		/* Nope, read from disk */
		if (mds_get_odl(cs->vp, &fp->flp) != NFS4_OK) {
			/*
			 * XXXXX:
			 * XXXXX: No ODL, so lets go query PE
			 * XXXXX:
			 */
			fake_spe(cs->instp, &fp->flp);

			if (fp->flp == NULL)
				return (NFS4ERR_LAYOUTUNAVAILABLE);
		}
	}

	/*
	 * pass back the mds_layout
	 */
	*flopp = (mds_layout_t *)fp->flp;
	return (NFS4_OK);
}

/*ARGSUSED*/
nfsstat4
mds_fetch_layout(struct compound_state *cs,
    LAYOUTGET4args *argp, LAYOUTGET4res *resp)
{
	mds_layout_t *lp;
	mds_mpd_t *dp;
	layout4 *logrp;
	nfsv4_1_file_layout4 otw_flo;
	nfs_fh4 *nfl_fh_list;
	rfs4_file_t *fp;
	mds_layout_grant_t *lgp;
	mds_ever_grant_t *egp;

	int i, err, nfl_size;
	bool_t create = TRUE;

	XDR  xdr;
	int  xdr_size = 0;
	char *xdr_buffer;


	if (mds_get_flo(cs, &lp) != NFS4_OK)
		return (NFS4ERR_LAYOUTUNAVAILABLE);

	/* validate the device id */
	dp = mds_find_mpd(cs->instp, lp->dev_id);

	bzero(&otw_flo, sizeof (otw_flo));

	if (dp == NULL) {
		DTRACE_PROBE1(nfss41__e__bad_devid, uint32_t, lp->dev_id);
		return (NFS4ERR_LAYOUTUNAVAILABLE);
	}

	mds_set_deviceid(lp->dev_id, &otw_flo.nfl_deviceid);

	/*
	 * 	NFL4_UFLG_COMMIT_THRU_MDS is FALSE
	 */
	otw_flo.nfl_util = (lp->stripe_unit & NFL4_UFLG_STRIPE_UNIT_SIZE_MASK);

	if (mds_layout_is_dense)
		otw_flo.nfl_util |= NFL4_UFLG_DENSE;

	/*
	 * Always start at the begining of the device array
	 */
	otw_flo.nfl_first_stripe_index = 0;

	/*
	 */
	nfl_size = lp->stripe_count * sizeof (nfs_fh4);

	nfl_fh_list = kmem_alloc(nfl_size, KM_NOSLEEP);

	if (nfl_fh_list == NULL)
		return (NFS4ERR_LAYOUTTRYLATER);

	/*
	 * this of course is totally bogus and this
	 * whole function will be re-whacked in the
	 * product.
	 */
	for (i = 0; i < lp->stripe_count; i++) {
		nfs41_fid_t fid =
		    ((nfs41_fh_fmt_t *)cs->fh.nfs_fh4_val)->fh.v1.obj_fid;

		/*
		 * Build DS Filehandles.
		 */
		err = mds_alloc_ds_fh(cs->exi->exi_fsid, fid,
		    &(nfl_fh_list[i]));
		if (err) {
			kmem_free(nfl_fh_list, nfl_size);
			return (NFS4ERR_LAYOUTUNAVAILABLE);
		}

	}

	otw_flo.nfl_fh_list.nfl_fh_list_len = lp->stripe_count;
	otw_flo.nfl_fh_list.nfl_fh_list_val = nfl_fh_list;

	xdr_size = xdr_sizeof(xdr_nfsv4_1_file_layout4, &otw_flo);
	ASSERT(xdr_size);

	/*
	 * Not big deal if we are resource constrained
	 * and the kmem_alloc fails. NFS Client will have
	 * to do IO through MDS.
	 */
	xdr_buffer = kmem_alloc(xdr_size, KM_NOSLEEP);
	if (xdr_buffer == NULL) {
		kmem_free(nfl_fh_list, nfl_size);
		return (NFS4ERR_LAYOUTTRYLATER);
	}

	/*
	 * Lets XDR Encode like we did last summer..
	 * (or twist again..)
	 */
	xdrmem_create(&xdr, xdr_buffer, xdr_size, XDR_ENCODE);

	if (xdr_nfsv4_1_file_layout4(&xdr, &otw_flo) == FALSE) {
		kmem_free(xdr_buffer, xdr_size);
		kmem_free(nfl_fh_list, nfl_size);
		return (NFS4ERR_LAYOUTTRYLATER);
	}

	/*
	 * create the layout grant
	 */
	mutex_enter(&cs->vp->v_lock);
	fp = (rfs4_file_t *)vsd_get(cs->vp, cs->instp->vkey);
	mutex_exit(&cs->vp->v_lock);

	lgp = rfs41_findlogrant(cs, fp, cs->cp, &create);
	if (lgp == NULL) {
		printf("rfs41_findlogrant() returned NULL; create=%d\n ",
		    create);
		kmem_free(xdr_buffer, xdr_size);
		kmem_free(nfl_fh_list, nfl_size);
		return (NFS4ERR_SERVERFAULT);
	}
	if (create == TRUE) {
		/* Insert the grant on the client's list */
		rfs4_dbe_lock(cs->cp->dbe);
		insque(&lgp->clientgrantlist, cs->cp->clientgrantlist.prev);
		rfs4_dbe_unlock(cs->cp->dbe);

		/* Insert the grant on the file's list */
		rfs4_dbe_lock(fp->dbe);
		insque(&lgp->lo_grant_list, fp->lo_grant_list.prev);
		rfs4_dbe_unlock(fp->dbe);
	}

	lgp->lop = lp;

	/*
	 * We may have just created the layout grant, or it could have
	 * already existed.  The client could be asking for "more" of the
	 * file.  However, it doesn't matter since we always hand out the
	 * entire file, we can remove any range fragments that might
	 * be on this grant and just have one range in the list that
	 * covers the whole file.
	 */
	(void) nfs_range_set(lgp->lo_range, 0, -1);

	/*
	 * create the ever grant structure
	 */
	create = TRUE;
	egp = rfs41_findevergrant(cs->cp, cs->vp, &create);
	if (create == FALSE)
		rfs41_ever_grant_rele(egp);

	/*
	 * Build layout get reply
	 */
	logrp = kmem_zalloc(sizeof (layout4), KM_SLEEP);

	resp->LAYOUTGET4res_u.logr_resok4.logr_layout.logr_layout_len = 1;
	resp->LAYOUTGET4res_u.logr_resok4.logr_layout.logr_layout_val = logrp;
	resp->LAYOUTGET4res_u.logr_resok4.logr_return_on_close = FALSE;
	resp->LAYOUTGET4res_u.logr_will_signal_layout_avail = FALSE;
	rfs41_lo_seqid(&lgp->lo_stateid);
	resp->LAYOUTGET4res_u.logr_resok4.logr_stateid =
	    lgp->lo_stateid.stateid;

	logrp->lo_offset = 0;
	logrp->lo_length = -1;
	logrp->lo_iomode = LAYOUTIOMODE4_RW;
	logrp->lo_content.loc_type = lp->layout_type;
	logrp->lo_content.loc_body.loc_body_len = xdr_size;
	logrp->lo_content.loc_body.loc_body_val = xdr_buffer;

	kmem_free(nfl_fh_list, nfl_size);
	return (NFS4_OK);
}

extern nfsstat4 mds_validate_logstateid(struct compound_state *, stateid_t *);

static mds_layout_grant_t *
mds_get_lo_grant_by_cp(struct compound_state *cs)
{
	rfs4_file_t *fp;
	rfs4_client_t *cp = cs->cp;
	mds_layout_grant_t *lgp;
	bool_t create = FALSE;

	mutex_enter(&cs->vp->v_lock);
	fp = (rfs4_file_t *)vsd_get(cs->vp, cs->instp->vkey);
	mutex_exit(&cs->vp->v_lock);
	if (fp == NULL)
		return (NULL);

	if (cp->clientgrantlist.next->lgp == NULL)
		return (NULL);

	lgp = rfs41_findlogrant(cs, fp, cp, &create);

	return (lgp);
}

/*
 * XXX - Eventually, we will support multi-range layouts. range_overlap
 *	 would be set to true if a new LAYOUTGET for a range (or part of
 *	 a range) that was already recalled, is obtained. For now, the
 *	 server does not handle multi-range layouts, but this is needed
 *	 for eventual CB_LAYOUT_RECALL race detection. Refer to section
 *	 12.5.5.2.1.3 of draft-25 for further details.
 */
uint32_t	range_overlap = 0;

/*ARGSUSED*/
static void
mds_op_layout_get(nfs_argop4 *argop, nfs_resop4 *resop,
    struct svc_req *reqp, compound_state_t *cs)
{
	LAYOUTGET4args		*argp = &argop->nfs_argop4_u.oplayoutget;
	LAYOUTGET4res		*resp = &resop->nfs_resop4_u.oplayoutget;
	nfsstat4		 nfsstat = NFS4_OK;
	stateid_t		*arg_stateid;
	mds_layout_grant_t	*lgp = NULL;
	sequenceid4		 log_seqid;

	DTRACE_NFSV4_2(op__layoutget__start,
	    struct compound_state *, cs,
	    LAYOUTGET4args *, argp);

	if (cs->vp == NULL) {
		*cs->statusp = resp->logr_status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (argp->loga_layout_type != LAYOUT4_NFSV4_1_FILES) {
		*cs->statusp = resp->logr_status = NFS4ERR_UNKNOWN_LAYOUTTYPE;
		goto final;
	}

	if (argp->loga_iomode == LAYOUTIOMODE4_ANY) {
		*cs->statusp = resp->logr_status = NFS4ERR_BADIOMODE;
		goto final;
	}

	if (argp->loga_length < argp->loga_minlength) {
		*cs->statusp = resp->logr_status = NFS4ERR_INVAL;
		goto final;
	}

	/*
	 * Validate the provided stateid
	 */
	arg_stateid = (stateid_t *)&(argp->loga_stateid);
	log_seqid = arg_stateid->v41_bits.chgseq;

	/*
	 * We may already have given this client a layout.  Better
	 * get it, if it exists.
	 */
	lgp = mds_get_lo_grant_by_cp(cs);

	/*
	 * first, only open, deleg, lock
	 * or layout stateids are permitted.
	 * currently open, deleg and lock stateids are handed
	 * out by v4 routines and are of v4 format.
	 * check if it is a layout stateid and treat differently.
	 */
	if (arg_stateid->v41_bits.type == LAYOUTID) {
		/*
		 * if they are using a layout stateid, then we must
		 * have already handed it out and should have found
		 * it above.
		 */
		if (lgp == NULL) {
			nfsstat = NFS4ERR_BAD_STATEID;
			goto final;
		}

		/*
		 * Layout race detection
		 */
		if (lgp->lo_status & LO_RECALL_INPROG) {
			ASSERT(lgp->lor_seqid != 0);
			if (log_seqid == (lgp->lor_seqid - 2)) {

				/* case 1 - pending recall in prog */
				nfsstat = NFS4ERR_RECALLCONFLICT;
				rfs41_lo_grant_rele(lgp);
				goto final;

			} else if (log_seqid >= lgp->lor_seqid &&
			    !lgp->lor_reply) {

				/*
				 * case 2 - server has NOT received
				 * the cb_lorecall reply yet.
				 */
				if (range_overlap) {
					nfsstat = NFS4ERR_RECALLCONFLICT;
					rfs41_lo_grant_rele(lgp);
					goto final;
				}

			} else if (log_seqid == lgp->lor_seqid &&
			    lgp->lor_reply) {

				/*
				 * case 3 - server HAS received
				 * reply to cb_lorecall
				 */
				nfsstat = NFS4ERR_RETURNCONFLICT;
				rfs41_lo_grant_rele(lgp);
				goto final;
			}
		}
		rfs41_lo_grant_rele(lgp);
	} else {
		/*
		 * not using a layout stateid, so we better not
		 * have handed one out already for this file or
		 * this client gets an error.
		 */
		if (lgp != NULL) {
			rfs41_lo_grant_rele(lgp);
			nfsstat = NFS4ERR_BAD_STATEID;
			goto final;
		}

		nfsstat = mds_validate_logstateid(cs, arg_stateid);
		if (nfsstat != NFS4_OK) {
			goto final;
		}
	}

	nfsstat = mds_fetch_layout(cs, argp, resp);

final:
	*cs->statusp = resp->logr_status = nfsstat;

	DTRACE_NFSV4_2(op__layoutget__done,
	    struct compound_state *, cs,
	    LAYOUTGET4res *, resp);
}

/*ARGSUSED*/
static void
mds_layoutget_free(nfs_resop4 *resop)
{

}

/*ARGSUSED*/
static void
mds_op_layout_commit(nfs_argop4 *argop, nfs_resop4 *resop,
    struct svc_req *reqp, compound_state_t *cs)
{
	LAYOUTCOMMIT4args *argp = &argop->nfs_argop4_u.oplayoutcommit;
	LAYOUTCOMMIT4res *resp = &resop->nfs_resop4_u.oplayoutcommit;
	nfsstat4 nfsstat = NFS4_OK;
	vnode_t *vp = cs->vp;
	cred_t *cr = cs->cr;
	rfs4_client_t *cp = cs->cp;
	mds_layout_grant_t *lgp;
	offset4 newsize;
	caller_context_t ct;
	vattr_t va;
	uio_t uio;
	iovec_t iov;
	char null_byte;
	int error;

	DTRACE_NFSV4_2(op__layoutcommit__start,
	    struct compound_state *, cs,
	    LAYOUTCOMMIT4args *, argp);

	if (cs->vp == NULL) {
		*cs->statusp = resp->locr_status = NFS4ERR_NOFILEHANDLE;
		goto final;
	}

	if (rfs4_clnt_in_grace(cp) && !argp->loca_reclaim) {
		*cs->statusp = resp->locr_status = NFS4ERR_GRACE;
		goto final;
	}

	if (argp->loca_reclaim) {
		if (!rfs4_clnt_in_grace(cp) || !cp->can_reclaim) {
			*cs->statusp = resp->locr_status = NFS4ERR_NO_GRACE;
			goto final;
		}
	} else {
		/*
		 * validate loca_stateid
		 */
		if ((lgp = mds_get_lo_grant_by_cp(cs)) == NULL) {
			*cs->statusp = resp->locr_status = NFS4ERR_BADLAYOUT;
			goto final;
		}

		if (layout_match(lgp->lo_stateid, argp->loca_stateid,
		    &nfsstat) == 0) {
			rfs41_lo_grant_rele(lgp);
			*cs->statusp = resp->locr_status = NFS4ERR_BADLAYOUT;
			goto final;
		}

		rfs41_lo_grant_rele(lgp);
	}

	resp->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize.\
	    ns_sizechanged = FALSE;

	if (argp->loca_last_write_offset.no_newoffset) {
		newsize = argp->loca_last_write_offset.newoffset4_u.no_offset
		    + 1;

		ct.cc_sysid = 0;
		ct.cc_pid = 0;
		ct.cc_caller_id = cs->instp->caller_id;
		ct.cc_flags = CC_DONTBLOCK;

		va.va_mask = AT_SIZE;
		error = VOP_GETATTR(vp, &va, 0, cr, &ct);
		if (error != 0) {
			*cs->statusp = resp->locr_status = puterrno4(error);
			goto final;
		}

		/*
		 * write a null byte at newsize-1 so that the size
		 * is correct.  VOP_SETATTR may fail if the mode of
		 * the file denies the implementation.
		 */
		if (newsize > va.va_size) {

			null_byte = '\0';

			iov.iov_base = &null_byte;
			iov.iov_len = 1;

			bzero(&uio, sizeof (uio));
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_offset = newsize - 1;
			uio.uio_segflg = UIO_SYSSPACE;
			uio.uio_fmode = FWRITE;
			uio.uio_extflg = 0;
			uio.uio_limit = newsize;
			uio.uio_resid = 1;

			(void) VOP_RWLOCK(vp, V_WRITELOCK_TRUE, &ct);
			error = VOP_WRITE(vp, &uio, FWRITE, cr, &ct);
			VOP_RWUNLOCK(vp, V_WRITELOCK_TRUE, &ct);
			if (error != 0) {
				*cs->statusp = resp->locr_status = \
				    puterrno4(error);
				goto final;
			}
			resp->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize.\
			    ns_sizechanged = TRUE;
			resp->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize.\
			    newsize4_u.ns_size = va.va_size;
		}
	}

	*cs->statusp = resp->locr_status = nfsstat;

final:
	DTRACE_NFSV4_2(op__layoutcommit__done,
	    struct compound_state *, cs,
	    LAYOUTCOMMIT4res *, resp);
}

int
layout_match(stateid_t lo_stateid, stateid4 lrf_id, nfsstat4 *status)
{
	stateid_t *id = (stateid_t *)&lrf_id;

	if (id->v41_bits.type != LAYOUTID) {
		*status = NFS4ERR_BAD_STATEID;
		return (0);
	}

	if (lo_stateid.v41_bits.boottime == id->v41_bits.boottime &&
	    lo_stateid.v41_bits.state_ident == id->v41_bits.state_ident) {
		*status = NFS4_OK;
		return (1);
	} else {
		*status = NFS4ERR_BAD_STATEID;
		return (0);
	}
}

nfsstat4
mds_return_layout_file(layoutreturn_file4 *lorf, struct compound_state *cs,
    LAYOUTRETURN4res *resp)
{
	rfs4_file_t *fp;
	mds_layout_grant_t *lgp;
	nfsstat4 status;
	bool_t create = FALSE;
	nfs_range_query_t remain;

	mutex_enter(&cs->vp->v_lock);
	fp = (rfs4_file_t *)vsd_get(cs->vp, cs->instp->vkey);
	mutex_exit(&cs->vp->v_lock);

	lgp = rfs41_findlogrant(cs, fp, cs->cp, &create);
	if (lgp == NULL) {
		cmn_err(CE_WARN, "lo_return(): findlogrant returned NULL");
		return (NFS4ERR_SERVERFAULT);
	}

	if (!layout_match(lgp->lo_stateid, lorf->lrf_stateid, &status)) {
		rfs41_lo_grant_rele(lgp);
		return (status);
	}

	/*
	 * Refer to Section 18.44.3 of draft-25 for the right
	 * lrs_present mojo and corresponding stateid setting.
	 */
	rfs41_lo_seqid(&lgp->lo_stateid);
	resp->lorr_stid_u.lrs_stateid = lgp->lo_stateid.stateid;

	remain = nfs_range_clear(lgp->lo_range, lorf->lrf_offset,
	    lorf->lrf_length);

#ifdef NOT_DONE
	/* XXX - could (should?) be async operation */
	/* need to add range to this */
	mds_invalidate_ds_state(lgp->lo_stateid, cs->cp, LAYOUT);
#endif

	/* if entire layout has been returned, clean up */
	if (remain == NFS_RANGE_NONE) {

		mutex_enter(&lgp->lo_lock);
		if (lgp->lo_status == LO_RECALL_INPROG) {
			lgp->lor_seqid = 0;  /* reset */
			lgp->lo_status = LO_RECALLED;
		} else {
			lgp->lo_status = LO_RETURNED;
		}
		mutex_exit(&lgp->lo_lock);

		/* remove the layout grant from both lists */
		rfs4_dbe_lock(lgp->cp->dbe);

		remque(&lgp->clientgrantlist);
		lgp->clientgrantlist.next = lgp->clientgrantlist.prev =
		    &lgp->clientgrantlist;

		rfs4_dbe_unlock(lgp->cp->dbe);

		lgp->cp = NULL;

		rfs4_dbe_lock(lgp->fp->dbe);

		remque(&lgp->lo_grant_list);
		lgp->lo_grant_list.next = lgp->lo_grant_list.prev =
		    &lgp->lo_grant_list;

		rfs4_dbe_unlock(lgp->fp->dbe);

		rfs4_file_rele(lgp->fp);
		lgp->fp = NULL;

		rfs4_dbe_invalidate(lgp->dbe);
		rfs41_lo_grant_rele(lgp);

#ifdef RECALL_ENGINE
		if (&fp->lo_grant_list == fp->lo_grant_list.next) {
			fp->layout->flag = LAYOUT_RETURNED;
			rfs4_dbe_cv_broadcast(fp->dbe);
		}
#endif

	} else {

		/*
		 * XXX - just in case our client does this
		 * remove this when the control protocol work is complete
		 * and we are telling the DS about partial returns.
		 */
		cmn_err(CE_WARN, "SURPRISE, partial layout return");

#ifdef RECALL_ENGINE
		/*
		 * Was a recall in progress?  Reset timers due to progress?
		 */
#endif
	}

	rfs41_lo_grant_rele(lgp);

	return (NFS4_OK);
}

/*ARGSUSED*/
static void
mds_op_layout_return(nfs_argop4 *argop, nfs_resop4 *resop,
    struct svc_req *reqp, compound_state_t *cs)
{
	LAYOUTRETURN4args	*args = &argop->nfs_argop4_u.oplayoutreturn;
	LAYOUTRETURN4res	*resp = &resop->nfs_resop4_u.oplayoutreturn;
	nfsstat4		 nfsstat = NFS4_OK;
	rfs4_client_t		*cp = cs->cp;
	layoutreturn_file4	*lorf;
	bool_t			 lora_reclaim = args->lora_reclaim;
	int			 ingrace;

	DTRACE_NFSV4_1(op__layoutreturn__start, struct compound_state *, cs);

	/*
	 * Layout type
	 */
	switch (args->lora_layout_type) {
	case LAYOUT4_NFSV4_1_FILES:
		break;

	case LAYOUT4_OSD2_OBJECTS:
	case LAYOUT4_BLOCK_VOLUME:
		nfsstat = NFS4ERR_NOTSUPP;
		goto final;

	default:
		nfsstat = NFS4ERR_UNKNOWN_LAYOUTTYPE;
		goto final;
	}

	/*
	 * XXX what if it is a bulk recall for FSID, but this client is
	 * doing a return of type FILE for a file on a different FSID?
	 */
	if (cp->bulk_recall &&
	    cp->bulk_recall != args->lora_layoutreturn.lr_returntype) {
		nfsstat = NFS4ERR_RECALLCONFLICT;
		goto final;
	}

	switch (args->lora_layoutreturn.lr_returntype) {
	case LAYOUTRETURN4_FILE:
		ingrace = rfs4_clnt_in_grace(cp);
		if (lora_reclaim && !ingrace) {
			nfsstat = NFS4ERR_NO_GRACE;
			goto final;
		} else if (ingrace && lora_reclaim) {
			nfsstat = NFS4ERR_GRACE;
			goto final;
		}
		lorf = &args->lora_layoutreturn.layoutreturn4_u.lr_layout;
		nfsstat = mds_return_layout_file(lorf, cs, resp);
		break;

	case LAYOUTRETURN4_FSID:
		if (lora_reclaim) {
			nfsstat = NFS4ERR_INVAL;
			goto final;
		}
#ifdef NOT_DONE
		mds_return_layout_fsid(cs);
#else
		cmn_err(CE_NOTE, "loreturn: LAYOUTRETURN4_FSID");
		nfsstat = NFS4ERR_NOTSUPP;
#endif
		cp->bulk_recall = 0;
		break;

	case LAYOUTRETURN4_ALL:
		if (lora_reclaim) {
			nfsstat = NFS4ERR_INVAL;
			goto final;
		}
#ifdef NOT_DONE
		mds_return_layout_all(cp);
#else
		cmn_err(CE_NOTE, "loreturn: LAYOUTRETURN4_ALL");
		nfsstat = NFS4ERR_NOTSUPP;
#endif
		cp->bulk_recall = 0;
		break;

	default:
		nfsstat = NFS4ERR_INVAL;
	}

final:
	*cs->statusp = resp->lorr_status = nfsstat;

	DTRACE_NFSV4_2(op__layoutreturn__done,
	    struct compound_state *, cs,
	    LAYOUTRETURN4res *, resp);
}

#ifdef JUST_HERE_TO_REMIND_ME
mds_return_layout_fsid(struct compound_state *cs)
{
	rfs4_client_t *cp = cs->cp;

	/* we need the fsid for this recall */
	fsid = get_fsid_from_fh(cs->fh);

	/*
	 * hg nits doesn't like any of this so i'm making it a comment block
	 * loop through the DS's
	 * 	mds_invalidate_ds_state(fsid, cp, LAYOUT_FSID)
	 * }
	 *
	 * clean up state
	 * 1) ever_grant
	 * 2) layout_grants
	 */
}

mds_return_layout_all(rfs4_client_t *cp)
{
	/*
	 * loop throug the DS's
	 * 	mds_invalidate_ds_state(NULL, cp, LAYOUT_ALL)
	 * }
	 *
	 * clean up state
	 * 1) ever_grant
	 * 2) layout_grants
	 */
}
#endif
