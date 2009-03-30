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
 * This file contains the functions implementing the client side of the
 * RPC program (PNFS_CTL_MDS) which defines the MDS to DS control for pNFS.
 * All PNFS_CTL_MDS messages originate on the MDS, therefore ctlmds_clnt_xxx
 * related functions will be those that are called by the MDS.
 */
#include <nfs/ds.h>
#include <nfs/ctl_mds_clnt.h>
#include <nfs/mds_state.h>
#include <rpc/rpc.h>
#include <sys/sdt.h>

static int
ctl_mds_clnt_call(ds_addrlist_t *dp, rpcproc_t proc,
    xdrproc_t xdrarg, void * argp,
    xdrproc_t xdrres, void * resp)
{
	enum clnt_stat status;
	struct timeval wait;
	int error = 0;
	CLIENT *client;

	wait.tv_sec = CTL_MDS_TIMEO;
	wait.tv_usec = 0;

	error = clnt_tli_kcreate(dp->dev_knc, dp->dev_nb,
	    PNFS_CTL_MDS, PNFS_CTL_MDS_V1, 0, 0, CRED(), &client);
	if (error)
		goto out;

	status = CLNT_CALL(client, proc,
	    xdrarg, argp,
	    xdrres, resp,
	    wait);

	if (status != RPC_SUCCESS) {
		DTRACE_PROBE1(nfss__e__ctl_mds_clnt_call_failed, int,
		    status);
		error = EIO;
	}

	AUTH_DESTROY(client->cl_auth);
	CLNT_DESTROY(client);

out:
	return (error);
}

/*
 * ctl_mds_clnt_remove_file()
 *
 * Removes one pNFS file.  The file to be removed will cause
 * CTL_MDS_REMOVE messages to get sent to the data servers involved in the
 * storage of the file.  This will prompt the data server to free the
 * storage allocated to the file.
 */
/* ARGSUSED */
int
ctl_mds_clnt_remove_file(nfs_server_instance_t *instp, fsid_t fsid,
    nfs41_fid_t fid, mds_layout_t *lop)
{
	CTL_MDS_REMOVEargs args;
	CTL_MDS_REMOVEres res;
	int i, error = 0;

	/*
	 * Right now all of the data server file handles for a single
	 * file on the MDS are the same.  Due to this the CTL_MDS_REMOVEargs
	 * can be formed once for all calls to the data servers.
	 * This will need to change in the future in order to handle
	 * when the MDS_SID is included in the file handle.  The MDS_SID
	 * will be different for each different data server/pNFS dataset
	 * that the layout involves.
	 */
	args.type = CTL_MDS_RM_OBJ;
	args.CTL_MDS_REMOVEargs_u.obj.obj_len = 1;
	args.CTL_MDS_REMOVEargs_u.obj.obj_val = kmem_alloc(sizeof (nfs_fh4),
	    KM_SLEEP);

	error = mds_alloc_ds_fh(fsid, fid,
	    &(args.CTL_MDS_REMOVEargs_u.obj.obj_val[0]));
	if (error)
		return (error);

	/*
	 * Take the layout, extract the data server information and send
	 * the DS_REMOVEs to the appropriate places.
	 */
	for (i = 0; i < lop->stripe_count; i++) {
		ds_addrlist_t *ds_addr;
		nfs_server_instance_t *instp;

		instp = dbe_to_instp(lop->dbe);
		ds_addr = mds_find_ds_addrlist(instp, lop->devs[i]);

		error = ctl_mds_clnt_call(ds_addr, CTL_MDS_REMOVE,
		    xdr_CTL_MDS_REMOVEargs, (caddr_t)&args,
		    xdr_CTL_MDS_REMOVEres, (caddr_t)&res);
		/* For now, ignore the error and results from REMOVE */
	}

	/* Allocated from mds_alloc_ds_fh */
	xdr_free_ds_fh(&(args.CTL_MDS_REMOVEargs_u.obj.obj_val[0]));
	kmem_free(args.CTL_MDS_REMOVEargs_u.obj.obj_val, sizeof (nfs_fh4));

	return (0);
}
