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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* Copyright (c) 1983, 1984, 1985, 1986, 1987, 1988, 1989 AT&T */
/* All Rights Reserved */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/cred.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/pathname.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/kstat.h>
#include <sys/mkdev.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/cmn_err.h>
#include <sys/utsname.h>
#include <sys/bootconf.h>
#include <sys/modctl.h>
#include <sys/acl.h>
#include <sys/flock.h>
#include <sys/kstr.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/atomic.h>
#include <sys/disp.h>
#include <sys/policy.h>
#include <sys/list.h>
#include <sys/zone.h>
#include <sys/sdt.h>

#include <rpc/types.h>
#include <rpc/auth.h>
#include <rpc/rpcsec_gss.h>
#include <rpc/clnt.h>
#include <rpc/xdr.h>

#include <nfs/nfs.h>
#include <nfs/nfs_clnt.h>
#include <nfs/mount.h>
#include <nfs/nfs_acl.h>

#include <fs/fs_subr.h>

#include <nfs/nfs4.h>
#include <nfs/rnode4.h>
#include <nfs/nfs4_clnt.h>
#include <nfs/nfssys.h>
#include <nfs/nfs4_pnfs.h>

#ifdef	DEBUG
/*
 * These are "special" state IDs and file handles that
 * match any delegation state ID or file handled.  This
 * is for testing purposes only.
 */


stateid4 nfs4_deleg_any = { 0x7FFFFFF0 };
char nfs4_deleg_fh[] = "\0377\0376\0375\0374";
nfs_fh4 nfs4_deleg_anyfh = { sizeof (nfs4_deleg_fh)-1, nfs4_deleg_fh };
nfsstat4 cb4_getattr_fail = NFS4_OK;
nfsstat4 cb4_recall_fail = NFS4_OK;

int nfs4_callback_debug;
int nfs4_recall_debug;
int nfs4_drat_debug;

#endif

int	nfs41_birpc = 1;	/* Use bidirectional rpc */

#define	CB_NOTE(x)	NFS4_DEBUG(nfs4_callback_debug, (CE_NOTE, x))
#define	CB_WARN(x)	NFS4_DEBUG(nfs4_callback_debug, (CE_WARN, x))
#define	CB_WARN1(x, y)	NFS4_DEBUG(nfs4_callback_debug, (CE_WARN, x, y))

enum nfs4_delegreturn_policy nfs4_delegreturn_policy = INACTIVE;

static zone_key_t nfs4_callback_zone_key;

/*
 * NFS4_MAPSIZE is the number of bytes we are willing to consume
 * for the block allocation map when the server grants a NFS_LIMIT_BLOCK
 * style delegation.
 */

#define	NFS4_MAPSIZE	8192
#define	NFS4_MAPWORDS	NFS4_MAPSIZE/sizeof (uint_t)
#define	NbPW		(NBBY*sizeof (uint_t))

static int nfs4_num_prognums = 1024;
static SVC_CALLOUT_TABLE nfs4_cb_sct;

struct nfs4_dnode {
	list_node_t	linkage;
	rnode4_t	*rnodep;
	int		flags;		/* Flags for nfs4delegreturn_impl() */
};

static const struct nfs4_callback_stats nfs4_callback_stats_tmpl = {
	{ "delegations",	KSTAT_DATA_UINT64 },
	{ "cb_getattr",		KSTAT_DATA_UINT64 },
	{ "cb_recall",		KSTAT_DATA_UINT64 },
	{ "cb_null",		KSTAT_DATA_UINT64 },
	{ "cb_dispatch",	KSTAT_DATA_UINT64 },
	{ "delegaccept_r",	KSTAT_DATA_UINT64 },
	{ "delegaccept_rw",	KSTAT_DATA_UINT64 },
	{ "delegreturn",	KSTAT_DATA_UINT64 },
	{ "callbacks",		KSTAT_DATA_UINT64 },
	{ "claim_cur",		KSTAT_DATA_UINT64 },
	{ "claim_cur_ok",	KSTAT_DATA_UINT64 },
	{ "recall_trunc",	KSTAT_DATA_UINT64 },
	{ "recall_failed",	KSTAT_DATA_UINT64 },
	{ "return_limit_write",	KSTAT_DATA_UINT64 },
	{ "return_limit_addmap", KSTAT_DATA_UINT64 },
	{ "deleg_recover",	KSTAT_DATA_UINT64 },
	{ "cb_illegal",		KSTAT_DATA_UINT64 },
	{ "cb_sequence",	KSTAT_DATA_UINT64 }
};

struct nfs4_cb_port {
	list_node_t		linkage; /* linkage into per-zone port list */
	char			netid[KNC_STRSIZE];
	char			uaddr[KNC_STRSIZE];
	char			protofmly[KNC_STRSIZE];
	char			proto[KNC_STRSIZE];
};

static int cb_getattr_bytes;

struct cb_recall_pass {
	rnode4_t	*rp;
	int		flags;		/* Flags for nfs4delegreturn_impl() */
	bool_t		truncate;
};

static nfs4_open_stream_t *get_next_deleg_stream(rnode4_t *, int);
static void nfs4delegreturn_thread(struct cb_recall_pass *);
static int deleg_reopen(vnode_t *, bool_t *, struct nfs4_callback_globals *,
    int);
static void nfs4_dlistadd(rnode4_t *, struct nfs4_callback_globals *, int);
static void nfs4_dlistclean_impl(struct nfs4_callback_globals *, int);
static int nfs4delegreturn_impl(rnode4_t *, int,
    struct nfs4_callback_globals *);
static void nfs4delegreturn_cleanup_impl(rnode4_t *, nfs4_server_t *,
    struct nfs4_callback_globals *);


/*
 * Only used for non-bidirectional RPC --Performs a BC2S and
 * starts the cbconn_thread.
 * (expects np->s_lock to be held)
 */

void
nfs41set_callback(nfs4_server_t *np, servinfo4_t *svp, mntinfo4_t *mi,
    cred_t *cr)
{
	struct nfs41_cb_info	*cbi;
	CLIENT			*client;
	struct nfs4_clnt	*nfscl;
	int			error;

	ASSERT(MUTEX_HELD(&np->s_lock));

	if (nfs4bind_conn_to_session(np, svp, mi, cr, CDFC4_BACK)) {
		zcmn_err(getzoneid(), CE_WARN,
		    "Callback Channel Binding Failed");
		return;
	}

	/*
	 * The following below is to create a client handle
	 * used only by the cbconn_thread to send out NFSPROC4_NULL
	 * and should not be used for anything else.
	 */
	cbi = np->zone_globals->nfs4prog2cbinfo[np->s_program-NFS4_CALLBACK];
	ASSERT(cbi != NULL);
	client = cbi->cb_client;

	/*
	 * If client from a previous session, destroy it first
	 */
	if (client) {
		AUTH_DESTROY(client->cl_auth);
		CLNT_DESTROY(client);
	}

	nfscl = zone_getspecific(nfs4clnt_zone_key, nfs_zone());
	ASSERT(nfscl != NULL);

	/* Get a CLIENT handle */
	error = clnt_tli_kcreate(svp->sv_knconf, &svp->sv_addr,
	    NFS4_PROGRAM, NFS_V4, 0, 0, np->s_cred, &client);

	if (error != 0) {
		zcmn_err(getzoneid(), CE_WARN,
		    "Failed to get handle for callback");
		cbi->cb_client = NULL;
		return;
	}

	/* Define this handle as a back channel handle */
	if (!(CLNT_CONTROL(client, CLSET_BACKCHANNEL, NULL))) {
		zcmn_err(getzoneid(), CE_WARN,
		    "Failed to set client handle as callback");
		CLNT_DESTROY(client);
		cbi->cb_client = NULL;
		return;
	}

	/* Associate it with the session */
	if (!CLNT_CONTROL(client, CLSET_TAG, (char *)(np->ssx.sessionid))) {
		zcmn_err(getzoneid(), CE_WARN,
		    "Failed to set tag on client handle");
		CLNT_DESTROY(client);
		cbi->cb_client = NULL;
		return;
	}

	cbi->cb_nfscl = nfscl;
	cbi->cb_client = client;

	/*
	 * Now start the cbconn_thread
	 */

	np->s_refcnt++;
	mutex_enter(&cbi->cb_reflock);
	cbi->cb_refcnt++;
	mutex_exit(&cbi->cb_reflock);
	(void) zthread_create(NULL, 0, nfs4_cbconn_thread, np, 0,
	    minclsyspri);
}

/*
 * nfs4_cbconn_thread is used to send a null op to the server over the
 * backchannel connection, to keep the back channel connection up.
 * This is not needed for bidirectional rpc as the op_sequence
 * heartbeat thread is doing the same thing.
 */
void
nfs4_cbconn_thread(nfs4_server_t *np)
{
	clock_t 		tick_delay;
	callb_cpr_t 		cpr_info;
	kmutex_t 		cpr_lock;
	struct nfs41_cb_info	*cbi;
	uint32_t		zilch = 0;
	int			timeo;
	struct timeval		wait;
	enum clnt_stat		rpc_stat;

	cbi = np->zone_globals->nfs4prog2cbinfo[np->s_program-NFS4_CALLBACK];
	mutex_init(&cpr_lock, NULL, MUTEX_DEFAULT, NULL);
	CALLB_CPR_INIT(&cpr_info, &cpr_lock, callb_generic_cpr, "nfsv4cbconn");

	timeo = (NFS_TIMEO * hz) / 10;
	timeo = (MIN(NFS_TIMEO, (NFS_COTS_TIMEO / 10)) * hz) / 10;
	TICK_TO_TIMEVAL(timeo, &wait);
	tick_delay = MSEC_TO_TICK((4 * (60 * 1000L)));

	while (!(cbi->cb_cbconn_exit)) {
		if (!(CLNT_CONTROL(cbi->cb_client, CLSET_XID,
		    (char *)&zilch))) {
			zcmn_err(getzoneid(), CE_WARN,
			    "Failed to zero xid, cbconn thread exiting");
			break;
		}
		/* Execute remote NULL procedure to establish the connection */
		rpc_stat = CLNT_CALL(cbi->cb_client, NFSPROC4_NULL,
		    xdr_void, NULL, xdr_void, NULL, wait);
		if (rpc_stat != RPC_SUCCESS) {
			zcmn_err(getzoneid(), CE_WARN,
			    "OP_NULL failed to transmit "
			    " on callback connection "
			    "status: 0x%x, cbconn thread exiting", rpc_stat);
			break;
		}
		mutex_enter(&cpr_lock);
		CALLB_CPR_SAFE_BEGIN(&cpr_info);
		mutex_exit(&cpr_lock);

		mutex_enter(&cbi->cb_cbconn_lock);
		(void) cv_timedwait(&cbi->cb_cbconn_wait,
		    &cbi->cb_cbconn_lock, tick_delay + lbolt);
		mutex_exit(&cbi->cb_cbconn_lock);

		mutex_enter(&cpr_lock);
		CALLB_CPR_SAFE_END(&cpr_info, &cpr_lock);
		mutex_exit(&cpr_lock);
	}

	nfs4_server_rele(np);
	nfs41_cbinfo_rele(cbi);
	mutex_enter(&cpr_lock);
	CALLB_CPR_EXIT(&cpr_info);
	cv_signal(&cbi->cb_destroy_wait);
	zthread_exit();
}

static void
cb_sequence(nfs_cb_argop4 *argop, nfs_cb_resop4 *resop, struct svc_req *req,
	struct compound_state *cs, struct nfs4_callback_globals *ncg)
{
	nfs4_server_t	*np;
	nfs41_cb_slot_t	*cslot;

	CB_SEQUENCE4args *args = &argop->nfs_cb_argop4_u.opcbsequence;
	CB_SEQUENCE4res *resp = &resop->nfs_cb_resop4_u.opcbsequence;

	ncg->nfs4_callback_stats.cb_getattr.value.ui64++;

	mutex_enter(&ncg->nfs4_cb_lock);
	np = ncg->nfs4prog2server[req->rq_prog - NFS4_CALLBACK];
	mutex_exit(&ncg->nfs4_cb_lock);
	if (nfs4_server_vlock(np, 0) == FALSE) {
		CB_WARN("cb_sequence: cannot find server\n");
		*cs->statusp = resp->csr_status = NFS4ERR_BADHANDLE;
		return;
	}

	bcopy(&args->csa_sessionid,
	    &resp->CB_SEQUENCE4res_u.csr_resok4.csr_sessionid,
	    sizeof (args->csa_sessionid));
	resp->CB_SEQUENCE4res_u.csr_resok4.csr_slotid = args->csa_slotid;
	resp->CB_SEQUENCE4res_u.csr_resok4.csr_sequenceid =
	    args->csa_sequenceid;
	resp->CB_SEQUENCE4res_u.csr_resok4.csr_highest_slotid =
	    args->csa_highest_slotid;
	resp->CB_SEQUENCE4res_u.csr_resok4.csr_target_highest_slotid =
	    args->csa_highest_slotid;

	if (bcmp(&args->csa_sessionid, &np->ssx.sessionid,
	    sizeof (np->ssx.sessionid)) != 0) {
		CB_WARN("cb_sequence: Bad Sequence Id\n");
		*cs->statusp = resp->csr_status = NFS4ERR_BADSESSION;
		mutex_exit(&np->s_lock);
		nfs4_server_rele(np);
		return;
	}

	if (args->csa_slotid >= np->ssx.cb_slot_table_size) {
		CB_WARN("cb_sequence: Bad Slotid\n");
		*cs->statusp = resp->csr_status = NFS4ERR_BADSLOT;
		mutex_exit(&np->s_lock);
		nfs4_server_rele(np);
		return;
	}

	cslot = np->ssx.cb_slot_table[args->csa_slotid];

	if (args->csa_sequenceid != cslot->cb_seq + 1 || (cslot->cb_inuse)) {
		CB_WARN("cb_sequence: Bad Sequence\n");
		*cs->statusp = resp->csr_status = NFS4ERR_SEQ_MISORDERED;
		mutex_exit(&np->s_lock);
		nfs4_server_rele(np);
		return;
	}

	cslot->cb_seq = args->csa_sequenceid;
	/*
	 * todo: need to set inuse and deal with server having
	 * multiple callbacks in-flight.
	 */

	*cs->statusp = resp->csr_status = NFS4_OK;
	mutex_exit(&np->s_lock);
	nfs4_server_rele(np);
}

static void
cb_getattr(nfs_cb_argop4 *argop, nfs_cb_resop4 *resop, struct svc_req *req,
	struct compound_state *cs, struct nfs4_callback_globals *ncg)
{
	CB_GETATTR4args *args = &argop->nfs_cb_argop4_u.opcbgetattr;
	CB_GETATTR4res *resp = &resop->nfs_cb_resop4_u.opcbgetattr;
	rnode4_t *rp;
	vnode_t *vp;
	bool_t found = FALSE;
	struct nfs4_server *sp;
	struct fattr4 *fap;
	rpc_inline_t *fdata;
	long mapcnt;
	fattr4_change change;
	fattr4_size size;
	uint_t rflag;

	ncg->nfs4_callback_stats.cb_getattr.value.ui64++;

#ifdef DEBUG
	/*
	 * error injection hook: set cb_getattr_fail global to
	 * NFS4 pcol error to be returned
	 */
	if (cb4_getattr_fail != NFS4_OK) {
		*cs->statusp = resp->status = cb4_getattr_fail;
		return;
	}
#endif

	resp->obj_attributes.attrmask =
	    NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));

	mutex_enter(&ncg->nfs4_cb_lock);
	sp = ncg->nfs4prog2server[req->rq_prog - NFS4_CALLBACK];
	mutex_exit(&ncg->nfs4_cb_lock);

	if (nfs4_server_vlock(sp, 0) == FALSE) {

		CB_WARN("cb_getattr: cannot find server\n");

		*cs->statusp = resp->status = NFS4ERR_BADHANDLE;
		return;
	}

	/*
	 * In cb_compound, callback_ident was validated against rq_prog,
	 * but we couldn't verify that it was set to the value we provided
	 * at setclientid time (because we didn't have server struct yet).
	 * Now we have the server struct, but don't have callback_ident
	 * handy.  So, validate server struct program number against req
	 * RPC's prog number.  At this point, we know the RPC prog num
	 * is valid (else we wouldn't be here); however, we don't know
	 * that it was the prog number we supplied to this server at
	 * setclientid time.  If the prog numbers aren't equivalent, then
	 * log the problem and fail the request because either cbserv
	 * and/or cbclient are confused.  This will probably never happen.
	 */
	if (sp->s_program != req->rq_prog) {
#ifdef DEBUG
		zcmn_err(getzoneid(), CE_WARN,
		    "cb_getattr: wrong server program number srv=%d req=%d\n",
		    sp->s_program, req->rq_prog);
#else
		zcmn_err(getzoneid(), CE_WARN,
		    "cb_getattr: wrong server program number\n");
#endif
		mutex_exit(&sp->s_lock);
		nfs4_server_rele(sp);
		*cs->statusp = resp->status = NFS4ERR_BADHANDLE;
		return;
	}

	/*
	 * Search the delegation list for a matching file handle;
	 * mutex on sp prevents the list from changing.
	 */

	rp = list_head(&sp->s_deleg_list);
	for (; rp != NULL; rp = list_next(&sp->s_deleg_list, rp)) {
		nfs4_fhandle_t fhandle;

		sfh4_copyval(rp->r_fh, &fhandle);

		if ((fhandle.fh_len == args->fh.nfs_fh4_len &&
		    bcmp(fhandle.fh_buf, args->fh.nfs_fh4_val,
		    fhandle.fh_len) == 0)) {

			found = TRUE;
			break;
		}
#ifdef	DEBUG
		if (nfs4_deleg_anyfh.nfs_fh4_len == args->fh.nfs_fh4_len &&
		    bcmp(nfs4_deleg_anyfh.nfs_fh4_val, args->fh.nfs_fh4_val,
		    args->fh.nfs_fh4_len) == 0) {

			found = TRUE;
			break;
		}
#endif
	}

	/*
	 * VN_HOLD the vnode before releasing s_lock to guarantee
	 * we have a valid vnode reference.
	 */
	if (found == TRUE) {
		vp = RTOV4(rp);
		VN_HOLD(vp);
	}

	mutex_exit(&sp->s_lock);
	nfs4_server_rele(sp);

	if (found == FALSE) {

		CB_WARN("cb_getattr: bad fhandle\n");

		*cs->statusp = resp->status = NFS4ERR_BADHANDLE;
		return;
	}

	/*
	 * Figure out which attributes the server wants.  We only
	 * offer FATTR4_CHANGE & FATTR4_SIZE; ignore the rest.
	 */
	fdata = kmem_alloc(cb_getattr_bytes, KM_SLEEP);

	/*
	 * Don't actually need to create XDR to encode these
	 * simple data structures.
	 * xdrmem_create(&xdr, fdata, cb_getattr_bytes, XDR_ENCODE);
	 */
	fap = &resp->obj_attributes;

	fap->attrmask = NFS4_EMPTY_ATTRMAP(RFS4_ATTRVERS(cs));
	/* attrlist4_len starts at 0 and increases as attrs are processed */
	fap->attrlist4 = (char *)fdata;
	fap->attrlist4_len = 0;

	if (ATTR_ISSET(args->attr_request, CHANGE)) {
		/*
		 * If the file is mmapped, then increment the change
		 * attribute and return it.  This will guarantee that
		 * the server will perceive that the file has changed
		 * if there is any chance that the client application
		 * has changed it.  Otherwise, just return the change
		 * attribute as it has been updated by nfs4write_deleg.
		 */

		mutex_enter(&rp->r_statelock);
		mapcnt = rp->r_mapcnt;
		rflag = rp->r_flags;
		mutex_exit(&rp->r_statelock);

		mutex_enter(&rp->r_statev4_lock);
		/*
		 * If object mapped, then always return new change.
		 * Otherwise, return change if object has dirty
		 * pages.  If object doesn't have any dirty pages,
		 * then all changes have been pushed to server, so
		 * reset change to grant change.
		 */
		if (mapcnt)
			rp->r_deleg_change++;
		else if (! (rflag & R4DIRTY))
		rp->r_deleg_change = rp->r_deleg_change_grant;
		change = rp->r_deleg_change;
		mutex_exit(&rp->r_statev4_lock);

		/*
		 * Use inline XDR code directly, we know that we
		 * going to a memory buffer and it has enough
		 * space so it cannot fail.
		 */
		IXDR_PUT_U_HYPER(fdata, change);
		fap->attrlist4_len += 2 * BYTES_PER_XDR_UNIT;
		ATTR_SET(fap->attrmask, CHANGE);
	}

	if (ATTR_ISSET(args->attr_request, SIZE)) {
		/*
		 * Use an atomic add of 0 to fetch a consistent view
		 * of r_size; this avoids having to take rw_lock
		 * which could cause a deadlock.
		 */
		size = atomic_add_64_nv((uint64_t *)&rp->r_size, 0);

		/*
		 * Use inline XDR code directly, we know that we
		 * going to a memory buffer and it has enough
		 * space so it cannot fail.
		 */
		IXDR_PUT_U_HYPER(fdata, size);
		fap->attrlist4_len += 2 * BYTES_PER_XDR_UNIT;
		ATTR_SET(fap->attrmask, SIZE);
	}

	VN_RELE(vp);

	*cs->statusp = resp->status = NFS4_OK;
}

static void
cb_getattr_free(nfs_cb_resop4 *resop)
{
	if (resop->nfs_cb_resop4_u.opcbgetattr.obj_attributes.attrlist4)
		kmem_free(resop->nfs_cb_resop4_u.opcbgetattr.
		    obj_attributes.attrlist4, cb_getattr_bytes);
}

static nfsstat4
layoutrecall_all(nfs4_server_t *np)
{
	vnode_t *vp;
	rnode4_t *rp;
	nfs4_fsidlt_t *ltp;
	nfsstat4 nstatus = NFS4ERR_NOMATCHING_LAYOUT;

	/*
	 * Walk thru all of the fsid layout trees, and all of their
	 * rnode layout trees and asynchronously return and free each layout.
	 */
	mutex_enter(&np->s_lt_lock);
	for (ltp = avl_first(&np->s_fsidlt); ltp;
	    ltp = AVL_NEXT(&np->s_fsidlt, ltp)) {
		mutex_enter(&ltp->lt_rlt_lock);
		for (rp = avl_first(&ltp->lt_rlayout_tree); rp;
		    rp = AVL_NEXT(&ltp->lt_rlayout_tree, rp)) {
			nstatus = NFS4_OK;
			vp = RTOV4(rp);
			VN_HOLD(vp);
			mutex_enter(&rp->r_statelock);
			pnfs_layout_return(vp, kcred, LR_ASYNC);
			mutex_exit(&rp->r_statelock);
			VN_RELE(vp);
		}
		mutex_exit(&ltp->lt_rlt_lock);
	}
	mutex_exit(&np->s_lt_lock);
	return (nstatus);
}


static nfsstat4
layoutrecall_fsid(fsid4 *recallfsid, nfs4_server_t *np)
{
	vnode_t *vp;
	rnode4_t *rp;
	nfs4_fsidlt_t *ltp, lt;
	nfsstat4 nstatus = NFS4ERR_NOMATCHING_LAYOUT;

	lt.lt_fsid.major = recallfsid->major;
	lt.lt_fsid.minor = recallfsid->minor;

	mutex_enter(&np->s_lt_lock);
	ltp = avl_find(&np->s_fsidlt, &lt, NULL);

	/*
	 * If no matching fsid layout tree is found, then no layouts exist
	 * for this fsid.
	 */
	if (ltp == NULL) {
		mutex_exit(&np->s_lt_lock);
		return (nstatus);
	}

	/*
	 * Found a matching fsid tree, return and free all
	 * layouts on this tree.
	 */
	mutex_enter(&ltp->lt_rlt_lock);
	mutex_exit(&np->s_lt_lock);

	for (rp = avl_first(&ltp->lt_rlayout_tree); rp;
	    rp = AVL_NEXT(&ltp->lt_rlayout_tree, rp)) {
		/*
		 * For each rnode on this fsid's layout tree,
		 * asynchronously return and free the layout.
		 */
		vp = RTOV4(rp);
		VN_HOLD(vp);
		mutex_enter(&rp->r_statelock);
		pnfs_layout_return(vp, kcred, LR_ASYNC);
		mutex_exit(&rp->r_statelock);
		VN_RELE(vp);
		nstatus = NFS4_OK;
	}
	mutex_exit(&ltp->lt_rlt_lock);
	return (nstatus);
}

static nfsstat4
layoutrecall_file(layoutrecall_file4 *lrf, nfs4_server_t *np)
{
	nfs_fh4		*rawfh = &lrf->lor_fh;
	nfs4_sharedfh_t sfh;
	vnode_t		*vp;
	rnode4_t	lrp, *rp;
	nfs4_fsidlt_t	*ltp;
	nfsstat4 nstatus = NFS4ERR_NOMATCHING_LAYOUT;

	bcopy(rawfh, &sfh, sizeof (*rawfh));
	lrp.r_fh = &sfh;

	mutex_enter(&np->s_lt_lock);
	/*
	 * Look thru the fsid layout trees until we find a matching
	 * rnode on an fsid layout tree's rnode layout tree.
	 */
	for (ltp = avl_first(&np->s_fsidlt); ltp;
	    ltp = AVL_NEXT(&np->s_fsidlt, ltp)) {
		/*
		 * Look at this fsid layout tree's rnode layout tree
		 * and see if it has the rnode we want based on the
		 * file handle.
		 */
		mutex_enter(&ltp->lt_rlt_lock);
		rp = avl_find(&ltp->lt_rlayout_tree, &lrp, NULL);
		if (rp != NULL) {
			vp = RTOV4(rp);
			VN_HOLD(vp);
			mutex_enter(&rp->r_statelock);
			pnfs_layout_return(vp, kcred, LR_ASYNC);
			mutex_exit(&rp->r_statelock);
			mutex_exit(&ltp->lt_rlt_lock);
			VN_RELE(vp);
			nstatus = NFS4_OK;
			break;
		}
		mutex_exit(&ltp->lt_rlt_lock);
	}
	mutex_exit(&np->s_lt_lock);
	return (nstatus);
}

static void
cb_layoutrecall(nfs_cb_argop4 *argop, nfs_cb_resop4 *resop, struct svc_req *req,
	struct compound_state *cs, struct nfs4_callback_globals *ncg)
{
	CB_LAYOUTRECALL4args *args = &argop->nfs_cb_argop4_u.opcblayoutrecall;
	CB_LAYOUTRECALL4res *resp = &resop->nfs_cb_resop4_u.opcblayoutrecall;
	struct nfs4_server *sp;

	if (args->clora_type != LAYOUT4_NFSV4_1_FILES) {
		DTRACE_PROBE1(nfsc__i__badlayoutype, int32_t,
		    args->clora_type);
		*cs->statusp = resp->clorr_status = NFS4ERR_INVAL;
		return;
	}

	mutex_enter(&ncg->nfs4_cb_lock);
	sp = ncg->nfs4prog2server[req->rq_prog - NFS4_CALLBACK];
	mutex_exit(&ncg->nfs4_cb_lock);

	if (nfs4_server_vlock(sp, 0) == FALSE) {
		DTRACE_PROBE1(nfsc__i__bad_prog, int, req->rq_prog);
		*cs->statusp = resp->clorr_status = NFS4ERR_NOMATCHING_LAYOUT;
		return;
	}
	mutex_exit(&sp->s_lock);

	switch (args->clora_recall.lor_recalltype) {
	case LAYOUTRECALL4_FILE:
		*cs->statusp = resp->clorr_status =
		    layoutrecall_file(&args->clora_recall.
		    layoutrecall4_u.lor_layout, sp);
		break;
	case LAYOUTRECALL4_FSID:
		*cs->statusp = resp->clorr_status =
		    layoutrecall_fsid(&args->clora_recall.
		    layoutrecall4_u.lor_fsid, sp);
		break;
	case LAYOUTRECALL4_ALL:
		*cs->statusp = resp->clorr_status = layoutrecall_all(sp);
		break;
	default:
		*cs->statusp = resp->clorr_status = NFS4ERR_INVAL;
	}
	nfs4_server_rele(sp);

	if (resp->clorr_status != NFS4_OK)
		DTRACE_PROBE2(nfsc__i__cblayouterr,
		    nfs4_server_t *, sp, nfsstat, resp->clorr_status);
}

static void
cb_recall(nfs_cb_argop4 *argop, nfs_cb_resop4 *resop, struct svc_req *req,
	struct compound_state *cs, struct nfs4_callback_globals *ncg)
{
	CB_RECALL4args * args = &argop->nfs_cb_argop4_u.opcbrecall;
	CB_RECALL4res *resp = &resop->nfs_cb_resop4_u.opcbrecall;
	rnode4_t *rp;
	vnode_t *vp;
	struct nfs4_server *sp;
	bool_t found = FALSE;

	ncg->nfs4_callback_stats.cb_recall.value.ui64++;

	ASSERT(req->rq_prog >= NFS4_CALLBACK);
	ASSERT(req->rq_prog < NFS4_CALLBACK+nfs4_num_prognums);

#ifdef DEBUG
	/*
	 * error injection hook: set cb_recall_fail global to
	 * NFS4 pcol error to be returned
	 */
	if (cb4_recall_fail != NFS4_OK) {
		*cs->statusp = resp->status = cb4_recall_fail;
		return;
	}
#endif

	mutex_enter(&ncg->nfs4_cb_lock);
	sp = ncg->nfs4prog2server[req->rq_prog - NFS4_CALLBACK];
	mutex_exit(&ncg->nfs4_cb_lock);

	if (nfs4_server_vlock(sp, 0) == FALSE) {

		CB_WARN("cb_recall: cannot find server\n");

		*cs->statusp = resp->status = NFS4ERR_BADHANDLE;
		return;
	}

	/*
	 * Search the delegation list for a matching file handle
	 * AND stateid; mutex on sp prevents the list from changing.
	 */

	rp = list_head(&sp->s_deleg_list);
	for (; rp != NULL; rp = list_next(&sp->s_deleg_list, rp)) {
		mutex_enter(&rp->r_statev4_lock);

		/* check both state id and file handle! */

		if ((bcmp(&rp->r_deleg_stateid, &args->stateid,
		    sizeof (stateid4)) == 0)) {
			nfs4_fhandle_t fhandle;

			sfh4_copyval(rp->r_fh, &fhandle);
			if ((fhandle.fh_len == args->fh.nfs_fh4_len &&
			    bcmp(fhandle.fh_buf, args->fh.nfs_fh4_val,
			    fhandle.fh_len) == 0)) {

				found = TRUE;
				break;
			} else {
#ifdef	DEBUG
				CB_WARN("cb_recall: stateid OK, bad fh");
#endif
			}
		}
#ifdef	DEBUG
		if (bcmp(&args->stateid, &nfs4_deleg_any,
		    sizeof (stateid4)) == 0) {

			found = TRUE;
			break;
		}
#endif
		mutex_exit(&rp->r_statev4_lock);
	}

	/*
	 * VN_HOLD the vnode before releasing s_lock to guarantee
	 * we have a valid vnode reference.  The async thread will
	 * release the hold when it's done.
	 */
	if (found == TRUE) {
		mutex_exit(&rp->r_statev4_lock);
		vp = RTOV4(rp);
		VN_HOLD(vp);
	}
	mutex_exit(&sp->s_lock);
	nfs4_server_rele(sp);

	if (found == FALSE) {

		CB_WARN("cb_recall: bad stateid\n");

		*cs->statusp = resp->status = NFS4ERR_BAD_STATEID;
		return;
	}

	/* Fire up a thread to do the delegreturn */
	nfs4delegreturn_async(rp, NFS4_DR_RECALL|NFS4_DR_REOPEN,
	    args->truncate);

	*cs->statusp = resp->status = 0;
}

/* ARGSUSED */
static void
cb_recall_free(nfs_cb_resop4 *resop)
{
	/* nothing to do here, cb_recall doesn't kmem_alloc */
}

/*
 * This function handles the CB_NULL proc call from an NFSv4 Server.
 *
 * We take note that the server has sent a CB_NULL for later processing
 * in the recovery logic. It is noted so we may pause slightly after the
 * setclientid and before reopening files. The pause is to allow the
 * NFSv4 Server time to receive the CB_NULL reply and adjust any of
 * its internal structures such that it has the opportunity to grant
 * delegations to reopened files.
 *
 */

/* ARGSUSED */
static void
cb_null(CB_COMPOUND4args *args, CB_COMPOUND4res *resp, struct svc_req *req,
    struct nfs4_callback_globals *ncg)
{
	struct nfs4_server *sp;

	ncg->nfs4_callback_stats.cb_null.value.ui64++;

	ASSERT(req->rq_prog >= NFS4_CALLBACK);
	ASSERT(req->rq_prog < NFS4_CALLBACK+nfs4_num_prognums);

	mutex_enter(&ncg->nfs4_cb_lock);
	sp = ncg->nfs4prog2server[req->rq_prog - NFS4_CALLBACK];
	mutex_exit(&ncg->nfs4_cb_lock);

	if (nfs4_server_vlock(sp, 0) != FALSE) {
		sp->s_flags |= N4S_CB_PINGED;
		cv_broadcast(&sp->wait_cb_null);
		mutex_exit(&sp->s_lock);
		nfs4_server_rele(sp);
	}
}

/*
 * cb_illegal	args: void
 *		res : status (NFS4ERR_OP_CB_ILLEGAL)
 */
/* ARGSUSED */
static void
cb_illegal(nfs_cb_argop4 *argop, nfs_cb_resop4 *resop, struct svc_req *req,
	struct compound_state *cs, struct nfs4_callback_globals *ncg)
{
	CB_ILLEGAL4res *resp = &resop->nfs_cb_resop4_u.opcbillegal;

	ncg->nfs4_callback_stats.cb_illegal.value.ui64++;
	resop->resop = OP_CB_ILLEGAL;
	*cs->statusp = resp->status = NFS4ERR_OP_ILLEGAL;
}

static void
cb_compound(CB_COMPOUND4args *args, CB_COMPOUND4res *resp, struct svc_req *req,
	struct nfs4_callback_globals *ncg)
{
	uint_t i;
	struct compound_state cs;
	nfs_cb_argop4 *argop;
	nfs_cb_resop4 *resop, *new_res;
	uint_t op, mvers_0;
	boolean_t	sequenced = FALSE;

	bzero(&cs, sizeof (cs));
	cs.statusp = &resp->status;
	cs.cont = TRUE;

	/*
	 * Form a reply tag by copying over the reqeuest tag.
	 */
	resp->tag.utf8string_len = args->tag.utf8string_len;
	resp->tag.utf8string_val = kmem_alloc(resp->tag.utf8string_len,
	    KM_SLEEP);
	bcopy(args->tag.utf8string_val, resp->tag.utf8string_val,
	    args->tag.utf8string_len);

	/*
	 * minorversion should be zero or one
	 */
	if (args->minorversion != CB4_MINOR_v0 &&
	    args->minorversion != CB4_MINOR_v1) {
		resp->array_len = 0;
		resp->array = NULL;
		resp->status = NFS4ERR_MINOR_VERS_MISMATCH;
		return;
	}

	/*
	 * The XDR code for CB_COMPOUND decodes all cb ops regardless
	 * of the minorversion of the compound containing the ops.
	 *
	 * mvers_0 is used to validate ops according to minor version:
	 * - only mvers 0 cb ops are allowed in mv 0 cb compounds
	 * - "is sequenced" checks only apply to mv 1 cb compunds
	 */
	mvers_0 = (args->minorversion == CB4_MINOR_v0);

#ifdef DEBUG
	/*
	 * Verify callback_ident.  It doesn't really matter if it's wrong
	 * because we don't really use callback_ident -- we use prog number
	 * of the RPC request instead.  In this case, just print a DEBUG
	 * console message to reveal brokenness of cbclient (at bkoff/cthon).
	 */
	if (args->callback_ident != req->rq_prog)
		zcmn_err(getzoneid(), CE_WARN,
		    "cb_compound: cb_client using wrong "
		    "callback_ident(%d), should be %d",
		    args->callback_ident, req->rq_prog);
#endif

	resp->array_len = args->array_len;
	resp->array = kmem_zalloc(args->array_len * sizeof (nfs_cb_resop4),
	    KM_SLEEP);

	for (i = 0; i < args->array_len && cs.cont; i++) {

		argop = &args->array[i];
		resop = &resp->array[i];
		resop->resop = argop->argop;
		op = (uint_t)resop->resop;

		switch (op) {

		case OP_CB_SEQUENCE:

			if (mvers_0) {
				op = OP_CB_ILLEGAL;
				cb_illegal(argop, resop, req, &cs, ncg);
				break;
			}
			cb_sequence(argop, resop, req, &cs, ncg);
			if (*cs.statusp == NFS4_OK)
				sequenced = TRUE;
			break;

		case OP_CB_GETATTR:

			if (!sequenced && !mvers_0) {
				*cs.statusp = resp->status =
				    NFS4ERR_SEQUENCE_POS;
				break;
			}
			cb_getattr(argop, resop, req, &cs, ncg);
			break;

		case OP_CB_RECALL:
			if (!sequenced && !mvers_0) {
				*cs.statusp = resp->status =
				    NFS4ERR_SEQUENCE_POS;
				break;
			}
			cb_recall(argop, resop, req, &cs, ncg);
			break;

		case OP_CB_LAYOUTRECALL:
			if (mvers_0) {
				op = OP_CB_ILLEGAL;
				cb_illegal(argop, resop, req, &cs, ncg);
				break;
			}
			if (!sequenced) {
				*cs.statusp = resp->status =
				    NFS4ERR_SEQUENCE_POS;
				break;
			}
			cb_layoutrecall(argop, resop, req, &cs, ncg);
			break;

		case OP_CB_ILLEGAL:
			if (!sequenced && !mvers_0) {
				*cs.statusp = resp->status =
				    NFS4ERR_SEQUENCE_POS;
				break;
			}
			/* fall through */

		default:
			/*
			 * Handle OP_CB_ILLEGAL and any undefined opcode.
			 * Currently, the XDR code will return BADXDR
			 * if cb op doesn't decode to legal value, so
			 * it really only handles OP_CB_ILLEGAL.
			 */
			op = OP_CB_ILLEGAL;
			cb_illegal(argop, resop, req, &cs, ncg);
		}

		if (*cs.statusp != NFS4_OK)
			cs.cont = FALSE;

		/*
		 * If not at last op, and if we are to stop, then
		 * compact the results array.
		 */
		if ((i + 1) < args->array_len && !cs.cont) {

			new_res = kmem_alloc(
			    (i+1) * sizeof (nfs_cb_resop4), KM_SLEEP);
			bcopy(resp->array,
			    new_res, (i+1) * sizeof (nfs_cb_resop4));
			kmem_free(resp->array,
			    args->array_len * sizeof (nfs_cb_resop4));

			resp->array_len =  i + 1;
			resp->array = new_res;
		}
	}

}

static void
cb_compound_free(CB_COMPOUND4res *resp)
{
	uint_t i, op;
	nfs_cb_resop4 *resop;

	if (resp->tag.utf8string_val) {
		UTF8STRING_FREE(resp->tag)
	}

	for (i = 0; i < resp->array_len; i++) {

		resop = &resp->array[i];
		op = (uint_t)resop->resop;

		switch (op) {

		case OP_CB_GETATTR:

			cb_getattr_free(resop);
			break;

		case OP_CB_RECALL:

			cb_recall_free(resop);
			break;

		default:
			break;
		}
	}

	if (resp->array != NULL) {
		kmem_free(resp->array,
		    resp->array_len * sizeof (nfs_cb_resop4));
	}
}

static void
cb_dispatch(struct svc_req *req, SVCXPRT *xprt)
{
	CB_COMPOUND4args args;
	CB_COMPOUND4res res;
	struct nfs4_callback_globals *ncg;

	bool_t (*xdr_args)(), (*xdr_res)();
	void (*proc)(CB_COMPOUND4args *, CB_COMPOUND4res *, struct svc_req *,
	    struct nfs4_callback_globals *);
	void (*freeproc)(CB_COMPOUND4res *);

	ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
	ASSERT(ncg != NULL);

	ncg->nfs4_callback_stats.cb_dispatch.value.ui64++;

	switch (req->rq_proc) {
	case CB_NULL:
		xdr_args = xdr_void;
		xdr_res = xdr_void;
		proc = cb_null;
		freeproc = NULL;
		break;

	case CB_COMPOUND:
		xdr_args = xdr_CB_COMPOUND4args_clnt;
		xdr_res = xdr_CB_COMPOUND4res;
		proc = cb_compound;
		freeproc = cb_compound_free;
		break;

	default:
		CB_WARN("cb_dispatch: no proc\n");
		svcerr_noproc(xprt);
		return;
	}

	args.tag.utf8string_val = NULL;
	args.array = NULL;

	if (!SVC_GETARGS(xprt, xdr_args, (caddr_t)&args)) {

		CB_WARN("cb_dispatch: cannot getargs\n");
		svcerr_decode(xprt);
		return;
	}

	(*proc)(&args, &res, req, ncg);

	if (svc_sendreply(xprt, xdr_res, (caddr_t)&res) == FALSE) {

		CB_WARN("cb_dispatch: bad sendreply\n");
		svcerr_systemerr(xprt);
	}

	if (freeproc)
		(*freeproc)(&res);

	if (!SVC_FREEARGS(xprt, xdr_args, (caddr_t)&args)) {

		CB_WARN("cb_dispatch: bad freeargs\n");
	}
}

static rpcprog_t
nfs4_getnextprogram(struct nfs4_callback_globals *ncg)
{
	int i, j;

	j = ncg->nfs4_program_hint;
	for (i = 0; i < nfs4_num_prognums; i++, j++) {

		if (j >= nfs4_num_prognums)
			j = 0;

		if (ncg->nfs4prog2server[j] == NULL) {
			ncg->nfs4_program_hint = j+1;
			return (j+NFS4_CALLBACK);
		}
	}

	return (0);
}

void
nfs4callback_destroy(nfs4_server_t *np)
{
	struct nfs4_callback_globals *ncg;
	struct nfs41_cb_info *cbi;
	int i;

	if (np->s_program == 0)
		return;

	ncg = np->zone_globals;
	cbi = ncg->nfs4prog2cbinfo[np->s_program - NFS4_CALLBACK];

	i = np->s_program - NFS4_CALLBACK;

	mutex_enter(&ncg->nfs4_cb_lock);

	ASSERT(ncg->nfs4prog2server[i] == np);

	ncg->nfs4prog2server[i] = NULL;
	ncg->nfs4prog2cbinfo[i] = NULL;

	if (i < ncg->nfs4_program_hint)
		ncg->nfs4_program_hint = i;

	mutex_exit(&ncg->nfs4_cb_lock);
	np->s_program = 0;
	if (cbi != NULL)
		nfs41_cbinfo_rele(cbi);
}

void
nfs41_cbinfo_rele(struct nfs41_cb_info *cbi)
{
	mutex_enter(&cbi->cb_reflock);
	cbi->cb_refcnt--;
	if (cbi->cb_refcnt > 0) {
		mutex_exit(&cbi->cb_reflock);
		return;
	}
	ASSERT(cbi->cb_flags & NFS41_CB_THREAD_EXIT);
	ASSERT(cbi->cb_cbconn_exit);
	mutex_exit(&cbi->cb_reflock);

	cbi->cb_rpc->r_flags |= SVCCB_DEAD;
	cv_signal(&cbi->cb_rpc->r_cbwait);	/* XXX - See mir_set_cbinfo */

	if (cbi->cb_client) {
		if (!(CLNT_CONTROL(cbi->cb_client,
		    CLSET_BACKCHANNEL_CLEAR, NULL))) {
			zcmn_err(getzoneid(), CE_WARN,
			    "Failed To Clear Client Handle Callback %p",
			    (void *)cbi->cb_client);
		}
		CLNT_DESTROY(cbi->cb_client);
	}
	mutex_destroy(&cbi->cb_cbconn_lock);
	cv_destroy(&cbi->cb_destroy_wait);
	cv_destroy(&cbi->cb_cbconn_wait);
	mutex_destroy(&cbi->cb_reflock);
	kmem_free(cbi, sizeof (*cbi));
}

/*
 * nfs4_setport - This function saves a netid and univeral address for
 * the callback program.  These values will be used during setclientid.
 */
static void
nfs4_setport(char *netid, char *uaddr, char *protofmly, char *proto,
	struct nfs4_callback_globals *ncg)
{
	struct nfs4_cb_port *p;
	bool_t found = FALSE;

	ASSERT(MUTEX_HELD(&ncg->nfs4_cb_lock));

	p = list_head(&ncg->nfs4_cb_ports);
	for (; p != NULL; p = list_next(&ncg->nfs4_cb_ports, p)) {
		if (strcmp(p->netid, netid) == 0) {
			found = TRUE;
			break;
		}
	}
	if (found == TRUE)
		(void) strcpy(p->uaddr, uaddr);
	else {
		p = kmem_alloc(sizeof (*p), KM_SLEEP);

		(void) strcpy(p->uaddr, uaddr);
		(void) strcpy(p->netid, netid);
		(void) strcpy(p->protofmly, protofmly);
		(void) strcpy(p->proto, proto);
		list_insert_head(&ncg->nfs4_cb_ports, p);
	}
}

static void
nfs41_callback_thread(struct nfs41_cb_info *cbi)
{
	callb_cpr_t	cprinfo;
	kmutex_t	cpr_lock;
	SVCXPRT		*clone_xprt;
	mblk_t		*mp;
	struct rpc_msg	msg;
	struct svc_req	r;
	char		*cred_area;
	int		rqcred_size = 400; 	/* RQCRED_SIZE */
	SVCCB		*cb = cbi->cb_rpc;

	mutex_init(&cpr_lock, NULL, MUTEX_DEFAULT, NULL);
	CALLB_CPR_INIT(&cprinfo, &cpr_lock, callb_generic_cpr,
	    "nfs41_cb");

	mutex_enter(&cbi->cb_rpc->r_lock);
	while (!(cbi->cb_flags & NFS41_CB_THREAD_EXIT)) {
		mutex_enter(&cpr_lock);
		CALLB_CPR_SAFE_BEGIN(&cprinfo);
		mutex_exit(&cpr_lock);

		cv_wait(&cbi->cb_rpc->r_cbwait, &cbi->cb_rpc->r_lock);

		mutex_enter(&cpr_lock);
		CALLB_CPR_SAFE_END(&cprinfo, &cpr_lock);
		mutex_exit(&cpr_lock);

		if (cbi->cb_flags & NFS41_CB_THREAD_EXIT)
			break;

		mutex_exit(&cbi->cb_rpc->r_lock);

		mutex_enter(&cb->r_mlock);
		mp = cb->r_mp;
		cb->r_mp = NULL;
		mutex_exit(&cb->r_mlock);

		clone_xprt = svc_clone_init();

		svc_init_clone_xprt(clone_xprt, cb->r_q);
		clone_xprt->xp_master = NULL;
		clone_xprt->xp_msg_size = 2048; /* COTS_MAX_ALLOCSIZE */
		cred_area = kmem_zalloc(2 * MAX_AUTH_BYTES + rqcred_size,
		    KM_SLEEP);
		msg.rm_call.cb_cred.oa_base = cred_area;
		msg.rm_call.cb_verf.oa_base = &(cred_area[MAX_AUTH_BYTES]);
		r.rq_clntcred = &(cred_area[2 * MAX_AUTH_BYTES]);

		/*
		 * underlying transport recv routine may modify mblk data
		 * and make it difficult to extract label afterwards. So
		 * get the label from the raw mblk data now.
		 */
		if (is_system_labeled()) {
			mblk_t *lmp;

			r.rq_label = kmem_alloc(sizeof (bslabel_t), KM_SLEEP);
			if (DB_CRED(mp) != NULL)
				lmp = mp;
		else {
			ASSERT(mp->b_cont != NULL);
			lmp = mp->b_cont;
			ASSERT(DB_CRED(lmp) != NULL);
		}
		bcopy(label2bslabel(crgetlabel(DB_CRED(lmp))), r.rq_label,
		    sizeof (bslabel_t));
		} else {
			r.rq_label = NULL;
		}

		/*
		 * Now receive the message.
		 */
		if (SVC_RECV(clone_xprt, mp, &msg)) {
			void (*dispatchroutine) (struct svc_req *, SVCXPRT *);
			bool_t no_dispatch;
			enum auth_stat why;

			/*
			 * Find the registered program and call its
			 * dispatch routine.
			 */
			r.rq_xprt = clone_xprt;
			r.rq_prog = msg.rm_call.cb_prog;
			r.rq_vers = msg.rm_call.cb_vers;
			r.rq_proc = msg.rm_call.cb_proc;
			r.rq_cred = msg.rm_call.cb_cred;

			if ((why = sec_svc_msg(&r, &msg, &no_dispatch)) !=
			    AUTH_OK) {
				svcerr_auth(clone_xprt, why);
				(void) SVC_FREEARGS(clone_xprt, NULL, NULL);
			} else if (no_dispatch) {
				(void) SVC_FREEARGS(clone_xprt, NULL, NULL);
			} else {
				if (r.rq_vers >= cbi->cb_versmin &&
				    r.rq_vers <= cbi->cb_versmax) {
					dispatchroutine = cbi->cb_callback;
					(*dispatchroutine) (&r, clone_xprt);
				} else {
					svcerr_progvers(clone_xprt,
					    cbi->cb_versmin,
					    cbi->cb_versmax);
				}
			(void) SVC_FREEARGS(clone_xprt, NULL, NULL);
			}
			if (r.rq_cred.oa_flavor == RPCSEC_GSS)
				rpc_gss_cleanup(clone_xprt);
		}
		if (r.rq_label != NULL)
			kmem_free(r.rq_label, sizeof (bslabel_t));
		mutex_enter(&cbi->cb_rpc->r_lock);
	}
	cbi->cb_thread = NULL;
	mutex_exit(&cbi->cb_rpc->r_lock);
	mutex_enter(&cpr_lock);
	CALLB_CPR_EXIT(&cprinfo);

	nfs41_cbinfo_rele(cbi);

	/*
	 * Signal destroy_session that we are done.
	 */
	cv_signal(&cbi->cb_destroy_wait);

	zthread_exit();
}


/*
 * nfs4_cb_args - This function is used to construct the callback
 * portion of the arguments needed for setclientid.
 */

void
nfs4_cb_args(nfs4_server_t *np, struct knetconfig *knc, SETCLIENTID4args *args)
{
	struct nfs4_cb_port *p;
	bool_t found = FALSE;
	rpcprog_t pgm;
	struct nfs4_callback_globals *ncg = np->zone_globals;

	/*
	 * This server structure may already have a program number
	 * assigned to it.  This happens when the client has to
	 * re-issue SETCLIENTID.  Just re-use the information.
	 */
	if (np->s_program >= NFS4_CALLBACK &&
	    np->s_program < NFS4_CALLBACK + nfs4_num_prognums)
		nfs4callback_destroy(np);

	mutex_enter(&ncg->nfs4_cb_lock);

	p = list_head(&ncg->nfs4_cb_ports);
	for (; p != NULL; p = list_next(&ncg->nfs4_cb_ports, p)) {
		if (strcmp(p->protofmly, knc->knc_protofmly) == 0 &&
		    strcmp(p->proto, knc->knc_proto) == 0) {
			found = TRUE;
			break;
		}
	}

	if (found == FALSE) {

		NFS4_DEBUG(nfs4_callback_debug,
		    (CE_WARN, "nfs4_cb_args: could not find netid for %s/%s\n",
		    knc->knc_protofmly, knc->knc_proto));

		args->callback.cb_program = 0;
		args->callback.cb_location.r_netid = NULL;
		args->callback.cb_location.r_addr = NULL;
		args->callback_ident = 0;
		mutex_exit(&ncg->nfs4_cb_lock);
		return;
	}

	if ((pgm = nfs4_getnextprogram(ncg)) == 0) {
		CB_WARN("nfs4_cb_args: out of program numbers\n");

		args->callback.cb_program = 0;
		args->callback.cb_location.r_netid = NULL;
		args->callback.cb_location.r_addr = NULL;
		args->callback_ident = 0;
		mutex_exit(&ncg->nfs4_cb_lock);
		return;
	}

	ncg->nfs4prog2server[pgm-NFS4_CALLBACK] = np;
	args->callback.cb_program = pgm;
	args->callback.cb_location.r_netid = p->netid;
	args->callback.cb_location.r_addr = p->uaddr;
	args->callback_ident = pgm;

	np->s_program = pgm;

	mutex_exit(&ncg->nfs4_cb_lock);
}

/*
 * nfs4_cb_args - This function is used to construct the callback
 * portion of the arguments needed for create_session.
 */
/* ARGSUSED */
void
nfs41_cb_args(nfs4_server_t *np, struct knetconfig *knc,
	CREATE_SESSION4args *args)
{
	rpcprog_t pgm;
	struct nfs4_callback_globals *ncg = np->zone_globals;
	struct nfs41_cb_info	*cbi;

	/*
	 * This server structure may already have a program number
	 * assigned to it.  This happens when the client has to
	 * re-issue SETCLIENTID.  Just re-use the information.
	 */
	if (np->s_program >= NFS4_CALLBACK &&
	    np->s_program < NFS4_CALLBACK + nfs4_num_prognums)
		nfs4callback_destroy(np);

	mutex_enter(&ncg->nfs4_cb_lock);

	if ((pgm = nfs4_getnextprogram(ncg)) == 0) {
		CB_WARN("nfs4_cb_args: out of program numbers\n");

		args->csa_cb_program = 0;
		args->csa_sec_parms.csa_sec_parms_len = 0;
		args->csa_sec_parms.csa_sec_parms_val = NULL;
		mutex_exit(&ncg->nfs4_cb_lock);
		return;
	}

	if (ncg->nfs4prog2cbinfo[pgm-NFS4_CALLBACK] == NULL)
		cbi = kmem_zalloc(sizeof (struct nfs41_cb_info), KM_SLEEP);
	else
		cbi = ncg->nfs4prog2cbinfo[pgm-NFS4_CALLBACK];

	cbi->cb_prog = pgm;
	cbi->cb_versmin = NFS_CB;
	cbi->cb_versmax = NFS_CB;
	cbi->cb_callback = cb_dispatch;

	cv_init(&cbi->cb_destroy_wait, NULL, CV_DEFAULT, NULL);
	mutex_init(&cbi->cb_reflock, NULL, MUTEX_DEFAULT, NULL);

	cv_init(&cbi->cb_cbconn_wait, NULL, CV_DEFAULT, NULL);
	mutex_init(&cbi->cb_cbconn_lock, NULL, MUTEX_DEFAULT, NULL);

	/*
	 * set cb_refcnt to 2, 1 to account for it being in the
	 * nfs4prog2cbinfo table, and another for the nfs41_callback_thread.
	 */
	cbi->cb_refcnt = 2;
	ncg->nfs4prog2cbinfo[pgm-NFS4_CALLBACK] = cbi;
	ncg->nfs4prog2server[pgm-NFS4_CALLBACK] = np;
	np->s_program = pgm;
	mutex_exit(&ncg->nfs4_cb_lock);

	args->csa_cb_program = pgm;
	args->csa_sec_parms.csa_sec_parms_len = 1;
	args->csa_sec_parms.csa_sec_parms_val = (callback_sec_parms4 *)
	    kmem_zalloc(sizeof (callback_sec_parms4), KM_SLEEP);
	args->csa_sec_parms.csa_sec_parms_val->cb_secflavor = AUTH_NONE;
	cbi->cb_rpc = kmem_zalloc(sizeof (SVCCB), KM_SLEEP);
	mutex_init(&cbi->cb_rpc->r_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&cbi->cb_rpc->r_mlock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&cbi->cb_rpc->r_cbwait, NULL, CV_DEFAULT, NULL);
	cbi->cb_rpc->r_prog = pgm;
	if (!cbi->cb_thread) {
		cbi->cb_thread = zthread_create(NULL, 0,
		    nfs41_callback_thread,
		    cbi, 0, minclsyspri);
		ASSERT(cbi->cb_thread != NULL);
	}

}

static int
nfs4_dquery(struct nfs4_svc_args *arg, model_t model)
{
	file_t *fp;
	vnode_t *vp;
	rnode4_t *rp;
	int error;
	STRUCT_HANDLE(nfs4_svc_args, uap);

	STRUCT_SET_HANDLE(uap, model, arg);

	if ((fp = getf(STRUCT_FGET(uap, fd))) == NULL)
		return (EBADF);

	vp = fp->f_vnode;

	if (vp == NULL || vp->v_type != VREG ||
	    !vn_matchops(vp, nfs4_vnodeops)) {
		releasef(STRUCT_FGET(uap, fd));
		return (EBADF);
	}

	rp = VTOR4(vp);

	/*
	 * I can't convince myself that we need locking here.  The
	 * rnode cannot disappear and the value returned is instantly
	 * stale anway, so why bother?
	 */

	error = suword32(STRUCT_FGETP(uap, netid), rp->r_deleg_type);
	releasef(STRUCT_FGET(uap, fd));
	return (error);
}


/*
 * NFS4 client system call.  This service does the
 * necessary initialization for the callback program.
 * This is fashioned after the server side interaction
 * between nfsd and the kernel.  On the client, the
 * mount command forks and the child process does the
 * necessary interaction with the kernel.
 *
 * uap->fd is the fd of an open transport provider
 */
int
nfs4_svc(struct nfs4_svc_args *arg, model_t model)
{
	file_t *fp;
	int error;
	int readsize;
	char buf[KNC_STRSIZE], uaddr[KNC_STRSIZE];
	char protofmly[KNC_STRSIZE], proto[KNC_STRSIZE];
	size_t len;
	STRUCT_HANDLE(nfs4_svc_args, uap);
	struct netbuf addrmask;
	int cmd;
	SVCMASTERXPRT *cb_xprt;
	struct nfs4_callback_globals *ncg;

#ifdef lint
	model = model;		/* STRUCT macros don't always refer to it */
#endif

	STRUCT_SET_HANDLE(uap, model, arg);

	if (STRUCT_FGET(uap, cmd) == NFS4_DQUERY)
		return (nfs4_dquery(arg, model));

	if (secpolicy_nfs(CRED()) != 0)
		return (EPERM);

	if ((fp = getf(STRUCT_FGET(uap, fd))) == NULL)
		return (EBADF);

	/*
	 * Set read buffer size to rsize
	 * and add room for RPC headers.
	 */
	readsize = nfs3tsize() + (RPC_MAXDATASIZE - NFS_MAXDATA);
	if (readsize < RPC_MAXDATASIZE)
		readsize = RPC_MAXDATASIZE;

	error = copyinstr((const char *)STRUCT_FGETP(uap, netid), buf,
	    KNC_STRSIZE, &len);
	if (error) {
		releasef(STRUCT_FGET(uap, fd));
		return (error);
	}

	cmd = STRUCT_FGET(uap, cmd);

	if (cmd & NFS4_KRPC_START) {
		addrmask.len = STRUCT_FGET(uap, addrmask.len);
		addrmask.maxlen = STRUCT_FGET(uap, addrmask.maxlen);
		addrmask.buf = kmem_alloc(addrmask.maxlen, KM_SLEEP);
		error = copyin(STRUCT_FGETP(uap, addrmask.buf), addrmask.buf,
		    addrmask.len);
		if (error) {
			releasef(STRUCT_FGET(uap, fd));
			kmem_free(addrmask.buf, addrmask.maxlen);
			return (error);
		}
	}
	else
		addrmask.buf = NULL;

	error = copyinstr((const char *)STRUCT_FGETP(uap, addr), uaddr,
	    sizeof (uaddr), &len);
	if (error) {
		releasef(STRUCT_FGET(uap, fd));
		if (addrmask.buf)
			kmem_free(addrmask.buf, addrmask.maxlen);
		return (error);
	}

	error = copyinstr((const char *)STRUCT_FGETP(uap, protofmly), protofmly,
	    sizeof (protofmly), &len);
	if (error) {
		releasef(STRUCT_FGET(uap, fd));
		if (addrmask.buf)
			kmem_free(addrmask.buf, addrmask.maxlen);
		return (error);
	}

	error = copyinstr((const char *)STRUCT_FGETP(uap, proto), proto,
	    sizeof (proto), &len);
	if (error) {
		releasef(STRUCT_FGET(uap, fd));
		if (addrmask.buf)
			kmem_free(addrmask.buf, addrmask.maxlen);
		return (error);
	}

	ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
	ASSERT(ncg != NULL);

	mutex_enter(&ncg->nfs4_cb_lock);
	if (cmd & NFS4_SETPORT)
		nfs4_setport(buf, uaddr, protofmly, proto, ncg);

	if (cmd & NFS4_KRPC_START) {
		error = svc_tli_kcreate(fp, readsize, buf, &addrmask, &cb_xprt,
		    &nfs4_cb_sct, NULL, NFS_CB_SVCPOOL_ID, FALSE);
		if (error) {
			CB_WARN1("nfs4_svc: svc_tli_kcreate failed %d\n",
			    error);
			kmem_free(addrmask.buf, addrmask.maxlen);
		}
	}

	mutex_exit(&ncg->nfs4_cb_lock);
	releasef(STRUCT_FGET(uap, fd));
	return (error);
}

struct nfs4_callback_globals *
nfs4_get_callback_globals(void)
{
	return (zone_getspecific(nfs4_callback_zone_key, nfs_zone()));
}

static void *
nfs4_callback_init_zone(zoneid_t zoneid)
{
	kstat_t *nfs4_callback_kstat;
	struct nfs4_callback_globals *ncg;

	ncg = kmem_zalloc(sizeof (*ncg), KM_SLEEP);

	ncg->nfs4prog2server = kmem_zalloc(nfs4_num_prognums *
	    sizeof (struct nfs4_server *), KM_SLEEP);

	ncg->nfs4prog2cbinfo = kmem_zalloc(nfs4_num_prognums *
	    sizeof (struct nfs4_cb_info *), KM_SLEEP);

	/* initialize the dlist */
	mutex_init(&ncg->nfs4_dlist_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ncg->nfs4_dlist, sizeof (struct nfs4_dnode),
	    offsetof(struct nfs4_dnode, linkage));

	/* initialize cb_port list */
	mutex_init(&ncg->nfs4_cb_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ncg->nfs4_cb_ports, sizeof (struct nfs4_cb_port),
	    offsetof(struct nfs4_cb_port, linkage));

	/* get our own copy of the kstats */
	bcopy(&nfs4_callback_stats_tmpl, &ncg->nfs4_callback_stats,
	    sizeof (nfs4_callback_stats_tmpl));
	/* register "nfs:0:nfs4_callback_stats" for this zone */
	if ((nfs4_callback_kstat =
	    kstat_create_zone("nfs", 0, "nfs4_callback_stats", "misc",
	    KSTAT_TYPE_NAMED,
	    sizeof (ncg->nfs4_callback_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL | KSTAT_FLAG_WRITABLE,
	    zoneid)) != NULL) {
		nfs4_callback_kstat->ks_data = &ncg->nfs4_callback_stats;
		kstat_install(nfs4_callback_kstat);
	}
	return (ncg);
}

static void
nfs4_discard_delegations(struct nfs4_callback_globals *ncg)
{
	nfs4_server_t *sp;
	int i, num_removed;

	/*
	 * It's OK here to just run through the registered "programs", as
	 * servers without programs won't have any delegations to handle.
	 */
	for (i = 0; i < nfs4_num_prognums; i++) {
		rnode4_t *rp;

		mutex_enter(&ncg->nfs4_cb_lock);
		sp = ncg->nfs4prog2server[i];
		mutex_exit(&ncg->nfs4_cb_lock);

		if (nfs4_server_vlock(sp, 1) == FALSE)
			continue;
		num_removed = 0;
		while ((rp = list_head(&sp->s_deleg_list)) != NULL) {
			mutex_enter(&rp->r_statev4_lock);
			if (rp->r_deleg_type == OPEN_DELEGATE_NONE) {
				/*
				 * We need to take matters into our own hands,
				 * as nfs4delegreturn_cleanup_impl() won't
				 * remove this from the list.
				 */
				list_remove(&sp->s_deleg_list, rp);
				mutex_exit(&rp->r_statev4_lock);
				nfs4_dec_state_ref_count_nolock(sp,
				    VTOMI4(RTOV4(rp)));
				num_removed++;
				continue;
			}
			mutex_exit(&rp->r_statev4_lock);
			VN_HOLD(RTOV4(rp));
			mutex_exit(&sp->s_lock);
			/*
			 * The following will remove the node from the list.
			 */
			nfs4delegreturn_cleanup_impl(rp, sp, ncg);
			VN_RELE(RTOV4(rp));
			mutex_enter(&sp->s_lock);
		}
		mutex_exit(&sp->s_lock);
		/* each removed list node reles a reference */
		while (num_removed-- > 0)
			nfs4_server_rele(sp);
		/* remove our reference for nfs4_server_vlock */
		nfs4_server_rele(sp);
	}
}

/* ARGSUSED */
static void
nfs4_callback_shutdown_zone(zoneid_t zoneid, void *data)
{
	struct nfs4_callback_globals *ncg = data;

	/*
	 * Clean pending delegation return list.
	 */
	nfs4_dlistclean_impl(ncg, NFS4_DR_DISCARD);

	/*
	 * Discard all delegations.
	 */
	nfs4_discard_delegations(ncg);
}

static void
nfs4_callback_fini_zone(zoneid_t zoneid, void *data)
{
	struct nfs4_callback_globals *ncg = data;
	struct nfs4_cb_port *p;
	nfs4_server_t *sp, *next;
	nfs4_server_t freelist;
	int i;

	kstat_delete_byname_zone("nfs", 0, "nfs4_callback_stats", zoneid);

	/*
	 * Discard all delegations that may have crept in since we did the
	 * _shutdown.
	 */
	nfs4_discard_delegations(ncg);
	/*
	 * We're completely done with this zone and all associated
	 * nfs4_server_t's.  Any remaining nfs4_server_ts should only have one
	 * more reference outstanding -- the reference we didn't release in
	 * nfs4_renew_lease_thread().
	 *
	 * Here we need to run through the global nfs4_server_lst as we need to
	 * deal with nfs4_server_ts without programs, as they also have threads
	 * created for them, and so have outstanding references that we need to
	 * release.
	 */
	freelist.forw = &freelist;
	freelist.back = &freelist;
	mutex_enter(&nfs4_server_lst_lock);
	sp = nfs4_server_lst.forw;
	while (sp != &nfs4_server_lst) {
		next = sp->forw;
		if (sp->zoneid == zoneid) {
			remque(sp);
			insque(sp, &freelist);
		}
		sp = next;
	}
	mutex_exit(&nfs4_server_lst_lock);

	sp = freelist.forw;
	while (sp != &freelist) {
		next = sp->forw;
		nfs4_server_rele(sp);	/* free the list's reference */
		sp = next;
	}

#ifdef DEBUG
	for (i = 0; i < nfs4_num_prognums; i++) {
		ASSERT(ncg->nfs4prog2server[i] == NULL);
	}
#endif
	kmem_free(ncg->nfs4prog2server, nfs4_num_prognums *
	    sizeof (struct nfs4_server *));

	mutex_enter(&ncg->nfs4_cb_lock);
	while ((p = list_head(&ncg->nfs4_cb_ports)) != NULL) {
		list_remove(&ncg->nfs4_cb_ports, p);
		kmem_free(p, sizeof (*p));
	}
	list_destroy(&ncg->nfs4_cb_ports);
	mutex_destroy(&ncg->nfs4_cb_lock);
	list_destroy(&ncg->nfs4_dlist);
	mutex_destroy(&ncg->nfs4_dlist_lock);
	kmem_free(ncg, sizeof (*ncg));
}

void
nfs4_callback_init(void)
{
	int i;
	SVC_CALLOUT *nfs4_cb_sc;

	/* initialize the callback table */
	nfs4_cb_sc = kmem_alloc(nfs4_num_prognums *
	    sizeof (SVC_CALLOUT), KM_SLEEP);

	for (i = 0; i < nfs4_num_prognums; i++) {
		nfs4_cb_sc[i].sc_prog = NFS4_CALLBACK+i;
		nfs4_cb_sc[i].sc_versmin = NFS_CB;
		nfs4_cb_sc[i].sc_versmax = NFS_CB;
		nfs4_cb_sc[i].sc_dispatch = cb_dispatch;
	}

	nfs4_cb_sct.sct_size = nfs4_num_prognums;
	nfs4_cb_sct.sct_free = FALSE;
	nfs4_cb_sct.sct_sc = nfs4_cb_sc;

	/*
	 * Compute max bytes required for dyamically allocated parts
	 * of cb_getattr reply.  Only size and change are supported now.
	 * If CB_GETATTR is changed to reply with additional attrs,
	 * additional sizes must be added below.
	 *
	 * fattr4_change + fattr4_size == uint64_t + uint64_t
	 */
	cb_getattr_bytes = 2 * BYTES_PER_XDR_UNIT + 2 * BYTES_PER_XDR_UNIT;

	zone_key_create(&nfs4_callback_zone_key, nfs4_callback_init_zone,
	    nfs4_callback_shutdown_zone, nfs4_callback_fini_zone);
}

void
nfs4_callback_fini(void)
{
}

/*
 * NB: This function can be called from the *wrong* zone (ie, the zone that
 * 'rp' belongs to and the caller's zone may not be the same).  This can happen
 * if the zone is going away and we get called from nfs4_async_inactive().  In
 * this case the globals will be NULL and we won't update the counters, which
 * doesn't matter as the zone is going away anyhow.
 */
static void
nfs4delegreturn_cleanup_impl(rnode4_t *rp, nfs4_server_t *np,
	struct nfs4_callback_globals *ncg)
{
	mntinfo4_t *mi = VTOMI4(RTOV4(rp));
	boolean_t need_rele = B_FALSE;

	/*
	 * Caller must be holding mi_recovlock in read mode
	 * to call here.  This is provided by start_op.
	 * Delegation management requires to grab s_lock
	 * first and then r_statev4_lock.
	 */

	if (np == NULL) {
		np = find_nfs4_server_all(mi, 1);
		ASSERT(np != NULL);
		need_rele = B_TRUE;
	} else {
		mutex_enter(&np->s_lock);
	}

	mutex_enter(&rp->r_statev4_lock);

	if (rp->r_deleg_type == OPEN_DELEGATE_NONE) {
		mutex_exit(&rp->r_statev4_lock);
		mutex_exit(&np->s_lock);
		if (need_rele)
			nfs4_server_rele(np);
		return;
	}

	/*
	 * Free the cred originally held when
	 * the delegation was granted.  Caller must
	 * hold this cred if it wants to use it after
	 * this call.
	 */
	crfree(rp->r_deleg_cred);
	rp->r_deleg_cred = NULL;
	rp->r_deleg_type = OPEN_DELEGATE_NONE;
	rp->r_deleg_needs_recovery = OPEN_DELEGATE_NONE;
	rp->r_deleg_needs_recall = FALSE;
	rp->r_deleg_return_pending = FALSE;

	/*
	 * Remove the rnode from the server's list and
	 * update the ref counts.
	 */
	list_remove(&np->s_deleg_list, rp);
	mutex_exit(&rp->r_statev4_lock);
	nfs4_dec_state_ref_count_nolock(np, mi);
	mutex_exit(&np->s_lock);
	/* removed list node removes a reference */
	nfs4_server_rele(np);
	if (need_rele)
		nfs4_server_rele(np);
	if (ncg != NULL)
		ncg->nfs4_callback_stats.delegations.value.ui64--;
}

void
nfs4delegreturn_cleanup(rnode4_t *rp, nfs4_server_t *np)
{
	struct nfs4_callback_globals *ncg;

	if (np != NULL) {
		ncg = np->zone_globals;
	} else if (nfs_zone() == VTOMI4(RTOV4(rp))->mi_zone) {
		ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
		ASSERT(ncg != NULL);
	} else {
		/*
		 * Request coming from the wrong zone.
		 */
		ASSERT(getzoneid() == GLOBAL_ZONEID);
		ncg = NULL;
	}

	nfs4delegreturn_cleanup_impl(rp, np, ncg);
}

static void
nfs4delegreturn_save_lost_rqst(int error, nfs4_lost_rqst_t *lost_rqstp,
	cred_t *cr, vnode_t *vp)
{
	if (error != ETIMEDOUT && error != EINTR &&
	    !NFS4_FRC_UNMT_ERR(error, vp->v_vfsp)) {
		lost_rqstp->lr_op = 0;
		return;
	}

	NFS4_DEBUG(nfs4_lost_rqst_debug, (CE_NOTE,
	    "nfs4close_save_lost_rqst: error %d", error));

	lost_rqstp->lr_op = OP_DELEGRETURN;
	/*
	 * The vp is held and rele'd via the recovery code.
	 * See nfs4_save_lost_rqst.
	 */
	lost_rqstp->lr_vp = vp;
	lost_rqstp->lr_dvp = NULL;
	lost_rqstp->lr_oop = NULL;
	lost_rqstp->lr_osp = NULL;
	lost_rqstp->lr_lop = NULL;
	lost_rqstp->lr_cr = cr;
	lost_rqstp->lr_flk = NULL;
	lost_rqstp->lr_putfirst = FALSE;
}

static void
nfs4delegreturn_otw(rnode4_t *rp, cred_t *cr, nfs4_error_t *ep)
{
	COMPOUND4args_clnt args;
	COMPOUND4res_clnt res;
	nfs_argop4 argops[3];
	nfs4_ga_res_t *garp = NULL;
	hrtime_t t;
	int numops;
	int doqueue = 1;
	mntinfo4_t *mi = VTOMI4(RTOV4(rp));

	args.ctag = TAG_DELEGRETURN;

	numops = 3;		/* PUTFH, GETATTR, DELEGRETURN */

	args.array = argops;
	args.array_len = numops;

	argops[0].argop = OP_CPUTFH;
	argops[0].nfs_argop4_u.opcputfh.sfh = rp->r_fh;

	argops[1].argop = OP_GETATTR;
	argops[1].nfs_argop4_u.opgetattr.attr_request =
	    MI4_DEFAULT_ATTRMAP(mi);
	argops[1].nfs_argop4_u.opgetattr.mi = VTOMI4(RTOV4(rp));

	argops[2].argop = OP_DELEGRETURN;
	argops[2].nfs_argop4_u.opdelegreturn.deleg_stateid =
	    rp->r_deleg_stateid;

	t = gethrtime();
	rfs4call(VTOMI4(RTOV4(rp)), NULL, &args, &res, cr, &doqueue, 0, ep);

	if (ep->error)
		return;

	if (res.status == NFS4_OK) {
		garp = &res.array[1].nfs_resop4_u.opgetattr.ga_res;
		nfs4_attr_cache(RTOV4(rp), garp, t, cr, TRUE, NULL);

	}
	(void) xdr_free(xdr_COMPOUND4res_clnt, (caddr_t)&res);
}

int
nfs4_do_delegreturn(rnode4_t *rp, int flags, cred_t *cr,
	struct nfs4_callback_globals *ncg)
{
	vnode_t *vp = RTOV4(rp);
	mntinfo4_t *mi = VTOMI4(vp);
	nfs4_lost_rqst_t lost_rqst;
	nfs4_recov_state_t recov_state;
	bool_t needrecov = FALSE, recovonly, done = FALSE;
	nfs4_error_t e = { 0, NFS4_OK, RPC_SUCCESS };

	ncg->nfs4_callback_stats.delegreturn.value.ui64++;

	while (!done) {
		e.error = nfs4_start_fop(mi, vp, NULL, OH_DELEGRETURN,
		    &recov_state, &recovonly);

		if (e.error) {
			if (flags & NFS4_DR_FORCE) {
				(void) nfs_rw_enter_sig(&mi->mi_recovlock,
				    RW_READER, 0);
				nfs4delegreturn_cleanup_impl(rp, NULL, ncg);
				nfs_rw_exit(&mi->mi_recovlock);
			}
			break;
		}

		/*
		 * Check to see if the delegation has already been
		 * returned by the recovery thread.   The state of
		 * the delegation cannot change at this point due
		 * to start_fop and the r_deleg_recall_lock.
		 */
		if (rp->r_deleg_type == OPEN_DELEGATE_NONE) {
			e.error = 0;
			nfs4_end_op(mi, vp, NULL, &recov_state, needrecov);
			break;
		}

		if (recovonly) {
			/*
			 * Delegation will be returned via the
			 * recovery framework.  Build a lost request
			 * structure, start recovery and get out.
			 */
			nfs4_error_init(&e, EINTR);
			nfs4delegreturn_save_lost_rqst(e.error, &lost_rqst,
			    cr, vp);
			(void) nfs4_start_recovery(&e, mi, vp,
			    NULL, &rp->r_deleg_stateid,
			    lost_rqst.lr_op == OP_DELEGRETURN ?
			    &lost_rqst : NULL, OP_DELEGRETURN, NULL);
			nfs4_end_op(mi, vp, NULL, &recov_state, needrecov);
			break;
		}

		nfs4delegreturn_otw(rp, cr, &e);

		/*
		 * Ignore some errors on delegreturn; no point in marking
		 * the file dead on a state destroying operation.
		 */
		if (e.error == 0 && (nfs4_recov_marks_dead(e.stat) ||
		    e.stat == NFS4ERR_BADHANDLE ||
		    e.stat == NFS4ERR_STALE))
			needrecov = FALSE;
		else
			needrecov = nfs4_needs_recovery(&e, TRUE, vp->v_vfsp);

		if (needrecov) {
			nfs4delegreturn_save_lost_rqst(e.error, &lost_rqst,
			    cr, vp);
			(void) nfs4_start_recovery(&e, mi, vp,
			    NULL, &rp->r_deleg_stateid,
			    lost_rqst.lr_op == OP_DELEGRETURN ?
			    &lost_rqst : NULL, OP_DELEGRETURN, NULL);
		} else {
			nfs4delegreturn_cleanup_impl(rp, NULL, ncg);
			done = TRUE;
		}

		nfs4_end_op(mi, vp, NULL, &recov_state, needrecov);
	}
	return (e.error);
}

/*
 * nfs4_resend_delegreturn - used to drive the delegreturn
 * operation via the recovery thread.
 */
void
nfs4_resend_delegreturn(nfs4_lost_rqst_t *lorp, nfs4_error_t *ep,
	nfs4_server_t *np)
{
	rnode4_t *rp = VTOR4(lorp->lr_vp);

	/* If the file failed recovery, just quit. */
	mutex_enter(&rp->r_statelock);
	if (rp->r_flags & R4RECOVERR) {
		ep->error = EIO;
	}
	mutex_exit(&rp->r_statelock);

	if (!ep->error)
		nfs4delegreturn_otw(rp, lorp->lr_cr, ep);

	/*
	 * If recovery is now needed, then return the error
	 * and status and let the recovery thread handle it,
	 * including re-driving another delegreturn.  Otherwise,
	 * just give up and clean up the delegation.
	 */
	if (nfs4_needs_recovery(ep, TRUE, lorp->lr_vp->v_vfsp))
		return;

	if (rp->r_deleg_type != OPEN_DELEGATE_NONE)
		nfs4delegreturn_cleanup(rp, np);

	nfs4_error_zinit(ep);
}

/*
 * nfs4delegreturn - general function to return a delegation.
 *
 * NFS4_DR_FORCE - return the delegation even if start_op fails
 * NFS4_DR_PUSH - push modified data back to the server via VOP_PUTPAGE
 * NFS4_DR_DISCARD - discard the delegation w/o delegreturn
 * NFS4_DR_DID_OP - calling function already did nfs4_start_op
 * NFS4_DR_RECALL - delegreturned initiated via CB_RECALL
 * NFS4_DR_REOPEN - do file reopens, if applicable
 */
static int
nfs4delegreturn_impl(rnode4_t *rp, int flags, struct nfs4_callback_globals *ncg)
{
	int error = 0;
	cred_t *cr = NULL;
	vnode_t *vp;
	bool_t needrecov = FALSE;
	bool_t rw_entered = FALSE;
	bool_t do_reopen;

	vp = RTOV4(rp);

	/*
	 * If NFS4_DR_DISCARD is set by itself, take a short-cut and
	 * discard without doing an otw DELEGRETURN.  This may only be used
	 * by the recovery thread because it bypasses the synchronization
	 * with r_deleg_recall_lock and mi->mi_recovlock.
	 */
	if (flags == NFS4_DR_DISCARD) {
		nfs4delegreturn_cleanup_impl(rp, NULL, ncg);
		return (0);
	}

	if (flags & NFS4_DR_DID_OP) {
		/*
		 * Caller had already done start_op, which means the
		 * r_deleg_recall_lock is already held in READ mode
		 * so we cannot take it in write mode.  Return the
		 * delegation asynchronously.
		 *
		 * Remove the NFS4_DR_DID_OP flag so we don't
		 * get stuck looping through here.
		 */
		VN_HOLD(vp);
		nfs4delegreturn_async(rp, (flags & ~NFS4_DR_DID_OP), FALSE);
		return (0);
	}

	/*
	 * Verify we still have a delegation and crhold the credential.
	 */
	mutex_enter(&rp->r_statev4_lock);
	if (rp->r_deleg_type == OPEN_DELEGATE_NONE) {
		mutex_exit(&rp->r_statev4_lock);
		goto out;
	}
	cr = rp->r_deleg_cred;
	ASSERT(cr != NULL);
	crhold(cr);
	mutex_exit(&rp->r_statev4_lock);

	/*
	 * Push the modified data back to the server synchronously
	 * before doing DELEGRETURN.
	 */
	if (flags & NFS4_DR_PUSH)
		(void) VOP_PUTPAGE(vp, 0, 0, 0, cr, NULL);

	/*
	 * Take r_deleg_recall_lock in WRITE mode, this will prevent
	 * nfs4_is_otw_open_necessary from trying to use the delegation
	 * while the DELEGRETURN is in progress.
	 */
	(void) nfs_rw_enter_sig(&rp->r_deleg_recall_lock, RW_WRITER, FALSE);

	rw_entered = TRUE;

	if (rp->r_deleg_type == OPEN_DELEGATE_NONE)
		goto out;

	if (flags & NFS4_DR_REOPEN) {
		/*
		 * If R4RECOVERRP is already set, then skip re-opening
		 * the delegation open streams and go straight to doing
		 * delegreturn.  (XXX if the file has failed recovery, then the
		 * delegreturn attempt is likely to be futile.)
		 */
		mutex_enter(&rp->r_statelock);
		do_reopen = !(rp->r_flags & R4RECOVERRP);
		mutex_exit(&rp->r_statelock);

		if (do_reopen) {
			error = deleg_reopen(vp, &needrecov, ncg, flags);
			if (error != 0) {
				if ((flags & (NFS4_DR_FORCE | NFS4_DR_RECALL))
				    == 0)
					goto out;
			} else if (needrecov) {
				if ((flags & NFS4_DR_FORCE) == 0)
					goto out;
			}
		}
	}

	if (flags & NFS4_DR_DISCARD) {
		mntinfo4_t *mi = VTOMI4(RTOV4(rp));

		mutex_enter(&rp->r_statelock);
		/*
		 * deleg_return_pending is cleared inside of delegation_accept
		 * when a delegation is accepted.  if this flag has been
		 * cleared, then a new delegation has overwritten the one we
		 * were about to throw away.
		 */
		if (!rp->r_deleg_return_pending) {
			mutex_exit(&rp->r_statelock);
			goto out;
		}
		mutex_exit(&rp->r_statelock);
		(void) nfs_rw_enter_sig(&mi->mi_recovlock, RW_READER, FALSE);
		nfs4delegreturn_cleanup_impl(rp, NULL, ncg);
		nfs_rw_exit(&mi->mi_recovlock);
	} else {
		error = nfs4_do_delegreturn(rp, flags, cr, ncg);
	}

out:
	if (cr)
		crfree(cr);
	if (rw_entered)
		nfs_rw_exit(&rp->r_deleg_recall_lock);
	return (error);
}

int
nfs4delegreturn(rnode4_t *rp, int flags)
{
	struct nfs4_callback_globals *ncg;

	ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
	ASSERT(ncg != NULL);

	return (nfs4delegreturn_impl(rp, flags, ncg));
}

void
nfs4delegreturn_async(rnode4_t *rp, int flags, bool_t trunc)
{
	struct cb_recall_pass *pp;

	pp = kmem_alloc(sizeof (struct cb_recall_pass), KM_SLEEP);
	pp->rp = rp;
	pp->flags = flags;
	pp->truncate = trunc;

	/*
	 * Fire up a thread to do the actual delegreturn
	 * Caller must guarantee that the rnode doesn't
	 * vanish (by calling VN_HOLD).
	 */

	(void) zthread_create(NULL, 0, nfs4delegreturn_thread, pp, 0,
	    minclsyspri);
}

static void
delegreturn_all_thread(rpcprog_t *pp)
{
	nfs4_server_t *np;
	bool_t found = FALSE;
	rpcprog_t prog;
	rnode4_t *rp;
	vnode_t *vp;
	zoneid_t zoneid = getzoneid();
	struct nfs4_callback_globals *ncg;

	NFS4_DEBUG(nfs4_drat_debug,
	    (CE_NOTE, "delereturn_all_thread: prog %d\n", *pp));

	prog = *pp;
	kmem_free(pp, sizeof (*pp));
	pp = NULL;

	mutex_enter(&nfs4_server_lst_lock);
	for (np = nfs4_server_lst.forw; np != &nfs4_server_lst; np = np->forw) {
		if (np->zoneid == zoneid && np->s_program == prog) {
			mutex_enter(&np->s_lock);
			found = TRUE;
			break;
		}
	}
	mutex_exit(&nfs4_server_lst_lock);

	/*
	 * It's possible that the nfs4_server which was using this
	 * program number has vanished since this thread is async.
	 * If so, just return.  Your work here is finished, my friend.
	 */
	if (!found)
		goto out;

	ncg = np->zone_globals;
	while ((rp = list_head(&np->s_deleg_list)) != NULL) {
		vp = RTOV4(rp);
		VN_HOLD(vp);
		mutex_exit(&np->s_lock);
		(void) nfs4delegreturn_impl(rp, NFS4_DR_PUSH|NFS4_DR_REOPEN,
		    ncg);
		VN_RELE(vp);

		/* retake the s_lock for next trip through the loop */
		mutex_enter(&np->s_lock);
	}
	mutex_exit(&np->s_lock);
out:
	NFS4_DEBUG(nfs4_drat_debug,
	    (CE_NOTE, "delereturn_all_thread: complete\n"));
	zthread_exit();
}

void
nfs4_delegreturn_all(nfs4_server_t *sp)
{
	rpcprog_t pro, *pp;

	mutex_enter(&sp->s_lock);

	/* Check to see if the delegation list is empty */

	if (list_head(&sp->s_deleg_list) == NULL) {
		mutex_exit(&sp->s_lock);
		return;
	}
	/*
	 * Grab the program number; the async thread will use this
	 * to find the nfs4_server.
	 */
	pro = sp->s_program;
	mutex_exit(&sp->s_lock);
	pp = kmem_alloc(sizeof (rpcprog_t), KM_SLEEP);
	*pp = pro;
	(void) zthread_create(NULL, 0, delegreturn_all_thread, pp, 0,
	    minclsyspri);
}


/*
 * Discard any delegations
 *
 * Iterate over the servers s_deleg_list and
 * for matching mount-point rnodes discard
 * the delegation.
 */
void
nfs4_deleg_discard(mntinfo4_t *mi, nfs4_server_t *sp)
{
	rnode4_t *rp, *next;
	mntinfo4_t *r_mi;
	struct nfs4_callback_globals *ncg;

	ASSERT(mutex_owned(&sp->s_lock));
	ncg = sp->zone_globals;

	for (rp = list_head(&sp->s_deleg_list); rp != NULL; rp = next) {
		r_mi = VTOMI4(RTOV4(rp));
		next = list_next(&sp->s_deleg_list, rp);

		if (r_mi != mi) {
			/*
			 * Skip if this rnode is in not on the
			 * same mount-point
			 */
			continue;
		}

		ASSERT(rp->r_deleg_type == OPEN_DELEGATE_READ);

#ifdef DEBUG
		if (nfs4_client_recov_debug) {
			zprintf(getzoneid(),
			    "nfs4_deleg_discard: matched rnode %p "
			"-- discarding delegation\n", (void *)rp);
		}
#endif
		mutex_enter(&rp->r_statev4_lock);
		/*
		 * Free the cred originally held when the delegation
		 * was granted. Also need to decrement the refcnt
		 * on this server for each delegation we discard
		 */
		if (rp->r_deleg_cred)
			crfree(rp->r_deleg_cred);
		rp->r_deleg_cred = NULL;
		rp->r_deleg_type = OPEN_DELEGATE_NONE;
		rp->r_deleg_needs_recovery = OPEN_DELEGATE_NONE;
		rp->r_deleg_needs_recall = FALSE;
		ASSERT(sp->s_refcnt > 1);
		sp->s_refcnt--;
		list_remove(&sp->s_deleg_list, rp);
		mutex_exit(&rp->r_statev4_lock);
		nfs4_dec_state_ref_count_nolock(sp, mi);
		ncg->nfs4_callback_stats.delegations.value.ui64--;
	}
}

/*
 * Reopen any open streams that were covered by the given file's
 * delegation.
 * Returns zero or an errno value.  If there was no error, *recovp
 * indicates whether recovery was initiated.
 */

static int
deleg_reopen(vnode_t *vp, bool_t *recovp, struct nfs4_callback_globals *ncg,
	int flags)
{
	nfs4_open_stream_t *osp;
	nfs4_recov_state_t recov_state;
	bool_t needrecov = FALSE;
	mntinfo4_t *mi;
	rnode4_t *rp;
	nfs4_error_t e = { 0, NFS4_OK, RPC_SUCCESS };
	int claimnull;

	mi = VTOMI4(vp);
	rp = VTOR4(vp);

	recov_state.rs_flags = 0;
	recov_state.rs_num_retry_despite_err = 0;

retry:
	if ((e.error = nfs4_start_op(mi, vp, NULL, &recov_state)) != 0) {
		return (e.error);
	}

	/*
	 * if we mean to discard the delegation, it must be BAD, so don't
	 * use it when doing the reopen or it will fail too.
	 */
	claimnull = (flags & NFS4_DR_DISCARD);
	/*
	 * Loop through the open streams for this rnode to find
	 * all of the ones created using the delegation state ID.
	 * Each of these needs to be re-opened.
	 */

	while ((osp = get_next_deleg_stream(rp, claimnull)) != NULL) {

		if (claimnull) {
			nfs4_reopen(vp, osp, &e, CLAIM_NULL, FALSE, FALSE);
		} else {
			ncg->nfs4_callback_stats.claim_cur.value.ui64++;

			nfs4_reopen(vp, osp, &e, CLAIM_DELEGATE_CUR, FALSE,
			    FALSE);
			if (e.error == 0 && e.stat == NFS4_OK)
				ncg->nfs4_callback_stats.
				    claim_cur_ok.value.ui64++;
		}

		if (e.error == EAGAIN) {
			nfs4_end_op(mi, vp, NULL, &recov_state, TRUE);
			goto retry;
		}

		/*
		 * if error is EINTR, ETIMEDOUT, or NFS4_FRC_UNMT_ERR, then
		 * recovery has already been started inside of nfs4_reopen.
		 */
		if (e.error == EINTR || e.error == ETIMEDOUT ||
		    NFS4_FRC_UNMT_ERR(e.error, vp->v_vfsp)) {
			open_stream_rele(osp, rp);
			break;
		}

		needrecov = nfs4_needs_recovery(&e, TRUE, vp->v_vfsp);

		if (e.error != 0 && !needrecov) {
			/*
			 * Recovery is not possible, but don't give up yet;
			 * we'd still like to do delegreturn after
			 * reopening as many streams as possible.
			 * Continue processing the open streams.
			 */

			ncg->nfs4_callback_stats.recall_failed.value.ui64++;

		} else if (needrecov) {
			/*
			 * Start recovery and bail out.  The recovery
			 * thread will take it from here.
			 */
			(void) nfs4_start_recovery(&e, mi, vp, NULL, NULL,
			    NULL, OP_OPEN, NULL);
			open_stream_rele(osp, rp);
			*recovp = TRUE;
			break;
		}

		open_stream_rele(osp, rp);
	}

	nfs4_end_op(mi, vp, NULL, &recov_state, needrecov);

	return (e.error);
}

/*
 * get_next_deleg_stream - returns the next open stream which
 * represents a delegation for this rnode.  In order to assure
 * forward progress, the caller must guarantee that each open
 * stream returned is changed so that a future call won't return
 * it again.
 *
 * There are several ways for the open stream to change.  If the open
 * stream is !os_delegation, then we aren't interested in it.  Also, if
 * either os_failed_reopen or !os_valid, then don't return the osp.
 *
 * If claimnull is false (doing reopen CLAIM_DELEGATE_CUR) then return
 * the osp if it is an os_delegation open stream.  Also, if the rnode still
 * has r_deleg_return_pending, then return the os_delegation osp.  Lastly,
 * if the rnode's r_deleg_stateid is different from the osp's open_stateid,
 * then return the osp.
 *
 * We have already taken the 'r_deleg_recall_lock' as WRITER, which
 * prevents new OPENs from going OTW (as start_fop takes this
 * lock in READ mode); thus, no new open streams can be created
 * (which inherently means no new delegation open streams are
 * being created).
 */

static nfs4_open_stream_t *
get_next_deleg_stream(rnode4_t *rp, int claimnull)
{
	nfs4_open_stream_t	*osp;

	ASSERT(nfs_rw_lock_held(&rp->r_deleg_recall_lock, RW_WRITER));

	/*
	 * Search through the list of open streams looking for
	 * one that was created while holding the delegation.
	 */
	mutex_enter(&rp->r_os_lock);
	for (osp = list_head(&rp->r_open_streams); osp != NULL;
	    osp = list_next(&rp->r_open_streams, osp)) {
		mutex_enter(&osp->os_sync_lock);
		if (!osp->os_delegation || osp->os_failed_reopen ||
		    !osp->os_valid) {
			mutex_exit(&osp->os_sync_lock);
			continue;
		}
		if (!claimnull || rp->r_deleg_return_pending ||
		    !stateid4_cmp(&osp->open_stateid, &rp->r_deleg_stateid)) {
			osp->os_ref_count++;
			mutex_exit(&osp->os_sync_lock);
			mutex_exit(&rp->r_os_lock);
			return (osp);
		}
		mutex_exit(&osp->os_sync_lock);
	}
	mutex_exit(&rp->r_os_lock);

	return (NULL);
}

static void
nfs4delegreturn_thread(struct cb_recall_pass *args)
{
	rnode4_t *rp;
	vnode_t *vp;
	cred_t *cr;
	int dtype, error, flags;
	bool_t rdirty, rip;
	kmutex_t cpr_lock;
	callb_cpr_t cpr_info;
	struct nfs4_callback_globals *ncg;

	ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
	ASSERT(ncg != NULL);

	mutex_init(&cpr_lock, NULL, MUTEX_DEFAULT, NULL);

	CALLB_CPR_INIT(&cpr_info, &cpr_lock, callb_generic_cpr,
	    "nfsv4delegRtn");

	rp = args->rp;
	vp = RTOV4(rp);

	mutex_enter(&rp->r_statev4_lock);
	if (rp->r_deleg_type == OPEN_DELEGATE_NONE) {
		mutex_exit(&rp->r_statev4_lock);
		goto out;
	}
	mutex_exit(&rp->r_statev4_lock);

	/*
	 * Take the read-write lock in read mode to prevent other
	 * threads from modifying the data during the recall.  This
	 * doesn't affect mmappers.
	 */
	(void) nfs_rw_enter_sig(&rp->r_rwlock, RW_READER, FALSE);

	/* Proceed with delegreturn */

	mutex_enter(&rp->r_statev4_lock);
	if (rp->r_deleg_type == OPEN_DELEGATE_NONE) {
		mutex_exit(&rp->r_statev4_lock);
		nfs_rw_exit(&rp->r_rwlock);
		goto out;
	}
	dtype = rp->r_deleg_type;
	cr = rp->r_deleg_cred;
	ASSERT(cr != NULL);
	crhold(cr);
	mutex_exit(&rp->r_statev4_lock);

	flags = args->flags;

	/*
	 * If the file is being truncated at the server, then throw
	 * away all of the pages, it doesn't matter what flavor of
	 * delegation we have.
	 */

	if (args->truncate) {
		ncg->nfs4_callback_stats.recall_trunc.value.ui64++;
		nfs4_invalidate_pages(vp, 0, cr);
	} else if (dtype == OPEN_DELEGATE_WRITE) {

		mutex_enter(&rp->r_statelock);
		rdirty = rp->r_flags & R4DIRTY;
		mutex_exit(&rp->r_statelock);

		if (rdirty) {
			error = VOP_PUTPAGE(vp, 0, 0, 0, cr, NULL);

			if (error)
				CB_WARN1("nfs4delegreturn_thread:"
				" VOP_PUTPAGE: %d\n", error);
		}
		/* turn off NFS4_DR_PUSH because we just did that above. */
		flags &= ~NFS4_DR_PUSH;
	}

	mutex_enter(&rp->r_statelock);
	rip =  rp->r_flags & R4RECOVERRP;
	mutex_exit(&rp->r_statelock);

	/* If a failed recovery is indicated, discard the pages */

	if (rip) {

		error = VOP_PUTPAGE(vp, 0, 0, B_INVAL, cr, NULL);

		if (error)
			CB_WARN1("nfs4delegreturn_thread: VOP_PUTPAGE: %d\n",
			    error);
	}

	/*
	 * Pass the flags to nfs4delegreturn_impl, but be sure not to pass
	 * NFS4_DR_DID_OP, which just calls nfs4delegreturn_async again.
	 */
	flags &= ~NFS4_DR_DID_OP;

	(void) nfs4delegreturn_impl(rp, flags, ncg);

	nfs_rw_exit(&rp->r_rwlock);
	crfree(cr);
out:
	kmem_free(args, sizeof (struct cb_recall_pass));
	VN_RELE(vp);
	mutex_enter(&cpr_lock);
	CALLB_CPR_EXIT(&cpr_info);
	mutex_destroy(&cpr_lock);
	zthread_exit();
}

/*
 * This function has one assumption that the caller of this function is
 * either doing recovery (therefore cannot call nfs4_start_op) or has
 * already called nfs4_start_op().
 */
void
nfs4_delegation_accept(rnode4_t *rp, open_claim_type4 claim, OPEN4res *res,
	nfs4_ga_res_t *garp, cred_t *cr)
{
	open_read_delegation4 *orp;
	open_write_delegation4 *owp;
	nfs4_server_t *np;
	bool_t already = FALSE;
	bool_t recall = FALSE;
	bool_t valid_garp = TRUE;
	bool_t delegation_granted = FALSE;
	bool_t dr_needed = FALSE;
	bool_t recov;
	int dr_flags = 0;
	long mapcnt;
	uint_t rflag;
	mntinfo4_t *mi;
	struct nfs4_callback_globals *ncg;
	open_delegation_type4 odt;

	ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
	ASSERT(ncg != NULL);

	mi = VTOMI4(RTOV4(rp));

	/*
	 * Accept a delegation granted to the client via an OPEN.
	 * Set the delegation fields in the rnode and insert the
	 * rnode onto the list anchored in the nfs4_server_t.  The
	 * proper locking order requires the nfs4_server_t first,
	 * even though it may not be needed in all cases.
	 *
	 * NB: find_nfs4_server returns with s_lock held.
	 */

	if ((np = find_nfs4_server(mi)) == NULL)
		return;

	/* grab the statelock too, for examining r_mapcnt */
	mutex_enter(&rp->r_statelock);
	mutex_enter(&rp->r_statev4_lock);

	if (rp->r_deleg_type == OPEN_DELEGATE_READ ||
	    rp->r_deleg_type == OPEN_DELEGATE_WRITE)
		already = TRUE;

	odt = res->delegation.delegation_type;

	if (odt == OPEN_DELEGATE_READ) {

		rp->r_deleg_type = res->delegation.delegation_type;
		orp = &res->delegation.open_delegation4_u.read;
		rp->r_deleg_stateid = orp->stateid;
		rp->r_deleg_perms = orp->permissions;
		if (claim == CLAIM_PREVIOUS)
			if ((recall = orp->recall) != 0)
				dr_needed = TRUE;

		delegation_granted = TRUE;

		ncg->nfs4_callback_stats.delegations.value.ui64++;
		ncg->nfs4_callback_stats.delegaccept_r.value.ui64++;

	} else if (odt == OPEN_DELEGATE_WRITE) {

		rp->r_deleg_type = res->delegation.delegation_type;
		owp = &res->delegation.open_delegation4_u.write;
		rp->r_deleg_stateid = owp->stateid;
		rp->r_deleg_perms = owp->permissions;
		rp->r_deleg_limit = owp->space_limit;
		if (claim == CLAIM_PREVIOUS)
			if ((recall = owp->recall) != 0)
				dr_needed = TRUE;

		delegation_granted = TRUE;

		if (garp == NULL || !garp->n4g_change_valid) {
			valid_garp = FALSE;
			rp->r_deleg_change = 0;
			rp->r_deleg_change_grant = 0;
		} else {
			rp->r_deleg_change = garp->n4g_change;
			rp->r_deleg_change_grant = garp->n4g_change;
		}
		mapcnt = rp->r_mapcnt;
		rflag = rp->r_flags;

		/*
		 * Update the delegation change attribute if
		 * there are mappers for the file is dirty.  This
		 * might be the case during recovery after server
		 * reboot.
		 */
		if (mapcnt > 0 || rflag & R4DIRTY)
			rp->r_deleg_change++;

		NFS4_DEBUG(nfs4_callback_debug, (CE_NOTE,
		    "nfs4_delegation_accept: r_deleg_change: 0x%x\n",
		    (int)(rp->r_deleg_change >> 32)));
		NFS4_DEBUG(nfs4_callback_debug, (CE_NOTE,
		    "nfs4_delegation_accept: r_delg_change_grant: 0x%x\n",
		    (int)(rp->r_deleg_change_grant >> 32)));


		ncg->nfs4_callback_stats.delegations.value.ui64++;
		ncg->nfs4_callback_stats.delegaccept_rw.value.ui64++;
	} else if (already) {
		/*
		 * No delegation granted.  If the rnode currently has
		 * has one, then consider it tainted and return it.
		 */
		dr_needed = TRUE;
	}

	if (delegation_granted) {
		/* Add the rnode to the list. */
		if (!already) {
			crhold(cr);
			rp->r_deleg_cred = cr;

			ASSERT(mutex_owned(&np->s_lock));
			list_insert_head(&np->s_deleg_list, rp);
			/* added list node gets a reference */
			np->s_refcnt++;
			nfs4_inc_state_ref_count_nolock(np, mi);
		}
		rp->r_deleg_needs_recovery = OPEN_DELEGATE_NONE;
	}

	/*
	 * We've now safely accepted the delegation, if any.  Drop the
	 * locks and figure out what post-processing is needed.  We'd
	 * like to retain r_statev4_lock, but nfs4_server_rele takes
	 * s_lock which would be a lock ordering violation.
	 */
	mutex_exit(&rp->r_statev4_lock);
	mutex_exit(&rp->r_statelock);
	mutex_exit(&np->s_lock);
	nfs4_server_rele(np);

	/*
	 * Check to see if we are in recovery.  Remember that
	 * this function is protected by start_op, so a recovery
	 * cannot begin until we are out of here.
	 */
	mutex_enter(&mi->mi_lock);
	recov = mi->mi_recovflags & MI4_RECOV_ACTIV;
	mutex_exit(&mi->mi_lock);

	mutex_enter(&rp->r_statev4_lock);

	if (nfs4_delegreturn_policy == IMMEDIATE || !valid_garp)
		dr_needed = TRUE;

	if (dr_needed && rp->r_deleg_return_pending == FALSE) {
		if (recov) {
			/*
			 * We cannot call delegreturn from inside
			 * of recovery or VOP_PUTPAGE will hang
			 * due to nfs4_start_fop call in
			 * nfs4write.  Use dlistadd to add the
			 * rnode to the list of rnodes needing
			 * cleaning.  We do not need to do reopen
			 * here because recov_openfiles will do it.
			 * In the non-recall case, just discard the
			 * delegation as it is no longer valid.
			 */
			if (recall)
				dr_flags = NFS4_DR_PUSH;
			else
				dr_flags = NFS4_DR_PUSH|NFS4_DR_DISCARD;

			nfs4_dlistadd(rp, ncg, dr_flags);
			dr_flags = 0;
		} else {
			/*
			 * Push the modified data back to the server,
			 * reopen any delegation open streams, and return
			 * the delegation.  Drop the statev4_lock first!
			 */
			dr_flags =  NFS4_DR_PUSH|NFS4_DR_DID_OP|NFS4_DR_REOPEN;
		}
	}
	mutex_exit(&rp->r_statev4_lock);
	if (dr_flags)
		(void) nfs4delegreturn_impl(rp, dr_flags, ncg);
}

/*
 * nfs4delegabandon - Abandon the delegation on an rnode4.  This code
 * is called when the client receives EXPIRED, BAD_STATEID, OLD_STATEID
 * or BADSEQID and the recovery code is unable to recover.  Push any
 * dirty data back to the server and return the delegation (if any).
 */

void
nfs4delegabandon(rnode4_t *rp)
{
	vnode_t *vp;
	struct cb_recall_pass *pp;
	open_delegation_type4 dt;

	mutex_enter(&rp->r_statev4_lock);
	dt = rp->r_deleg_type;
	mutex_exit(&rp->r_statev4_lock);

	if (dt == OPEN_DELEGATE_NONE)
		return;

	vp = RTOV4(rp);
	VN_HOLD(vp);

	pp = kmem_alloc(sizeof (struct cb_recall_pass), KM_SLEEP);
	pp->rp = rp;
	/*
	 * Recovery on the file has failed and we want to return
	 * the delegation.  We don't want to reopen files and
	 * nfs4delegreturn_thread() figures out what to do about
	 * the data.  The only thing to do is attempt to return
	 * the delegation.
	 */
	pp->flags = 0;
	pp->truncate = FALSE;

	/*
	 * Fire up a thread to do the delegreturn; this is
	 * necessary because we could be inside a GETPAGE or
	 * PUTPAGE and we cannot do another one.
	 */

	(void) zthread_create(NULL, 0, nfs4delegreturn_thread, pp, 0,
	    minclsyspri);
}

static int
wait_for_recall1(vnode_t *vp, nfs4_op_hint_t op, nfs4_recov_state_t *rsp,
	int flg)
{
	rnode4_t *rp;
	int error = 0;

#ifdef lint
	op = op;
#endif

	if (vp && vp->v_type == VREG) {
		rp = VTOR4(vp);

		/*
		 * Take r_deleg_recall_lock in read mode to synchronize
		 * with delegreturn.
		 */
		error = nfs_rw_enter_sig(&rp->r_deleg_recall_lock,
		    RW_READER, INTR4(vp));

		if (error == 0)
			rsp->rs_flags |= flg;

	}
	return (error);
}

void
nfs4_end_op_recall(vnode_t *vp1, vnode_t *vp2, nfs4_recov_state_t *rsp)
{
	NFS4_DEBUG(nfs4_recall_debug,
	    (CE_NOTE, "nfs4_end_op_recall: 0x%p, 0x%p\n",
	    (void *)vp1, (void *)vp2));

	if (vp2 && rsp->rs_flags & NFS4_RS_RECALL_HELD2)
		nfs_rw_exit(&VTOR4(vp2)->r_deleg_recall_lock);
	if (vp1 && rsp->rs_flags & NFS4_RS_RECALL_HELD1)
		nfs_rw_exit(&VTOR4(vp1)->r_deleg_recall_lock);
}

int
wait_for_recall(vnode_t *vp1, vnode_t *vp2, nfs4_op_hint_t op,
	nfs4_recov_state_t *rsp)
{
	int error;

	NFS4_DEBUG(nfs4_recall_debug,
	    (CE_NOTE, "wait_for_recall:    0x%p, 0x%p\n",
	    (void *)vp1, (void *) vp2));

	rsp->rs_flags &= ~(NFS4_RS_RECALL_HELD1|NFS4_RS_RECALL_HELD2);

	if ((error = wait_for_recall1(vp1, op, rsp, NFS4_RS_RECALL_HELD1)) != 0)
		return (error);

	if ((error = wait_for_recall1(vp2, op, rsp, NFS4_RS_RECALL_HELD2))
	    != 0) {
		if (rsp->rs_flags & NFS4_RS_RECALL_HELD1) {
			nfs_rw_exit(&VTOR4(vp1)->r_deleg_recall_lock);
			rsp->rs_flags &= ~NFS4_RS_RECALL_HELD1;
		}

		return (error);
	}

	return (0);
}

/*
 * nfs4_dlistadd - Add this rnode to a list of rnodes to be
 * DELEGRETURN'd at the end of recovery.
 */

static void
nfs4_dlistadd(rnode4_t *rp, struct nfs4_callback_globals *ncg, int flags)
{
	struct nfs4_dnode *dp;

	ASSERT(mutex_owned(&rp->r_statev4_lock));
	/*
	 * Mark the delegation as having a return pending.
	 * This will prevent the use of the delegation stateID
	 * by read, write, setattr and open.
	 */
	rp->r_deleg_return_pending = TRUE;
	dp = kmem_alloc(sizeof (*dp), KM_SLEEP);
	VN_HOLD(RTOV4(rp));
	dp->rnodep = rp;
	dp->flags = flags;
	mutex_enter(&ncg->nfs4_dlist_lock);
	list_insert_head(&ncg->nfs4_dlist, dp);
#ifdef	DEBUG
	ncg->nfs4_dlistadd_c++;
#endif
	mutex_exit(&ncg->nfs4_dlist_lock);
}

/*
 * nfs4_dlistclean_impl - Do DELEGRETURN for each rnode on the list.
 * of files awaiting cleaning.  If the override_flags are non-zero
 * then use them rather than the flags that were set when the rnode
 * was added to the dlist.
 */
static void
nfs4_dlistclean_impl(struct nfs4_callback_globals *ncg, int override_flags)
{
	rnode4_t *rp;
	struct nfs4_dnode *dp;
	int flags;

	ASSERT(override_flags == 0 || override_flags == NFS4_DR_DISCARD);

	mutex_enter(&ncg->nfs4_dlist_lock);
	while ((dp = list_head(&ncg->nfs4_dlist)) != NULL) {
#ifdef	DEBUG
		ncg->nfs4_dlistclean_c++;
#endif
		list_remove(&ncg->nfs4_dlist, dp);
		mutex_exit(&ncg->nfs4_dlist_lock);
		rp = dp->rnodep;
		flags = (override_flags != 0) ? override_flags : dp->flags;
		kmem_free(dp, sizeof (*dp));
		(void) nfs4delegreturn_impl(rp, flags, ncg);
		VN_RELE(RTOV4(rp));
		mutex_enter(&ncg->nfs4_dlist_lock);
	}
	mutex_exit(&ncg->nfs4_dlist_lock);
}

void
nfs4_dlistclean(void)
{
	struct nfs4_callback_globals *ncg;

	ncg = zone_getspecific(nfs4_callback_zone_key, nfs_zone());
	ASSERT(ncg != NULL);

	nfs4_dlistclean_impl(ncg, 0);
}
