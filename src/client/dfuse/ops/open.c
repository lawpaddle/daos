/**
 * (C) Copyright 2016-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "dfuse_common.h"
#include "dfuse.h"

void
dfuse_cb_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct dfuse_projection_info *fs_handle = fuse_req_userdata(req);
	struct dfuse_inode_entry     *ie;
	d_list_t                     *rlink;
	struct dfuse_obj_hdl         *oh     = NULL;
	struct fuse_file_info         fi_out = {0};
	int                           rc;
	bool                          prefetch = false;

	rlink = d_hash_rec_find(&fs_handle->dpi_iet, &ino, sizeof(ino));
	if (!rlink) {
		DFUSE_REPLY_ERR_RAW(fs_handle, req, ENOENT);
		return;
	}
	ie = container_of(rlink, struct dfuse_inode_entry, ie_htl);

	D_ALLOC_PTR(oh);
	if (!oh)
		D_GOTO(err, rc = ENOMEM);

	DFUSE_TRA_UP(oh, ie, "open handle");

	dfuse_open_handle_init(oh, ie);

	/* Upgrade fd permissions from O_WRONLY to O_RDWR if wb caching is
	 * enabled so the kernel can do read-modify-write
	 */
	if (ie->ie_dfs->dfc_data_timeout != 0 && fs_handle->dpi_info->di_wb_cache &&
	    (fi->flags & O_ACCMODE) == O_WRONLY) {
		DFUSE_TRA_DEBUG(ie, "Upgrading fd to O_RDRW");
		fi->flags &= ~O_ACCMODE;
		fi->flags |= O_RDWR;
	}

	/** duplicate the file handle for the fuse handle */
	rc = dfs_dup(ie->ie_dfs->dfs_ns, ie->ie_obj, fi->flags, &oh->doh_obj);
	if (rc)
		D_GOTO(err, rc);

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		oh->doh_writeable = true;

	if (ie->ie_dfs->dfc_data_timeout != 0) {
		if (fi->flags & O_DIRECT)
			fi_out.direct_io = 1;

		/* If the file is already open or (potentially) in cache then allow any existing
		 * kernel cache to be used.  If not then use pre-read.
		 * This should mean that pre-read is only used on the first read, and on files
		 * which pre-existed in the container.
		 */
		if (atomic_load_relaxed(&ie->ie_open_count) > 0 ||
		    dfuse_dcache_get_valid(ie, ie->ie_dfs->dfc_data_timeout)) {
			fi_out.keep_cache = 1;
		} else {
			prefetch = true;
		}
	} else {
		fi_out.direct_io = 1;
	}

	if (ie->ie_dfs->dfc_direct_io_disable)
		fi_out.direct_io = 0;

	if (!fi_out.direct_io)
		oh->doh_caching = true;

	fi_out.fh = (uint64_t)oh;

	LOG_FLAGS(ie, fi->flags);

	/*
	 * dfs_dup() just locally duplicates the file handle. If we have
	 * O_TRUNC flag, we need to truncate the file manually.
	 */
	if (fi->flags & O_TRUNC) {
		rc = dfs_punch(ie->ie_dfs->dfs_ns, ie->ie_obj, 0, DFS_MAX_FSIZE);
		if (rc)
			D_GOTO(err, rc);
		dfuse_dcache_evict(oh->doh_ie);
	}

	atomic_fetch_add_relaxed(&ie->ie_open_count, 1);

	/* Enable this for files up to the max read size. */
	if (prefetch && ie->ie_stat.st_size > 0 && ie->ie_stat.st_size <= DFUSE_MAX_READ) {
		D_ALLOC_PTR(oh->doh_readahead);
		if (oh->doh_readahead) {
			D_MUTEX_INIT(&oh->doh_readahead->dra_lock, 0);
			D_MUTEX_LOCK(&oh->doh_readahead->dra_lock);
		}
	}

	d_hash_rec_decref(&fs_handle->dpi_iet, rlink);
	DFUSE_REPLY_OPEN(oh, req, &fi_out);

	if (oh->doh_readahead)
		dfuse_pre_read(fs_handle, oh);

	return;
err:
	d_hash_rec_decref(&fs_handle->dpi_iet, rlink);
	D_FREE(oh);
	DFUSE_REPLY_ERR_RAW(ie, req, rc);
}

void
dfuse_cb_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct dfuse_obj_hdl *oh = (struct dfuse_obj_hdl *)fi->fh;
	int                   rc;
	uint32_t              il_calls;

	/* Perform the opposite of what the ioctl call does, always change the open handle count
	 * but the inode only tracks number of open handles with non-zero ioctl counts
	 */

	DFUSE_TRA_DEBUG(oh, "Closing %d", oh->doh_caching);

	if (oh->doh_readahead) {
		struct dfuse_event *ev;

		/* Grab this lock first to ensure that the read cb has been completed.  The
		 * callback might register an error and release ev so do not read it's value
		 * until after this has completed.
		 */
		D_MUTEX_LOCK(&oh->doh_readahead->dra_lock);
		D_MUTEX_UNLOCK(&oh->doh_readahead->dra_lock);

		ev = oh->doh_readahead->dra_ev;

		D_MUTEX_DESTROY(&oh->doh_readahead->dra_lock);
		if (ev) {
			daos_event_fini(&ev->de_ev);
			d_slab_release(ev->de_eqt->de_read_slab, ev);
		}
		D_FREE(oh->doh_readahead);
	}

	/* If the file was read from then set the data cache time for future use, however if the
	 * file was written to then evict the metadata cache.
	 * The problem here is that if the file was written to then the contents will be in the
	 * kernel cache however dfuse has no visibility over file size and before replying with
	 * data from the cache the kernel will call stat() to get an up-to-date file size, so
	 * after write dfuse may continue to tell the kernel incorrect file sizes.  To avoid this
	 * evict the metadata cache so the size is refreshed on next access.
	 */
	il_calls = atomic_load_relaxed(&oh->doh_il_calls);

	if (atomic_load_relaxed(&oh->doh_write_count) != 0) {
		if (oh->doh_caching) {
			if (il_calls == 0) {
				DFUSE_TRA_DEBUG(oh, "Evicting metadata cache, setting data cache");
				dfuse_mcache_evict(oh->doh_ie);
				dfuse_dcache_set_time(oh->doh_ie);
			} else {
				DFUSE_TRA_DEBUG(oh, "Evicting cache");
				dfuse_cache_evict(oh->doh_ie);
			}
		}
		atomic_fetch_sub_relaxed(&oh->doh_ie->ie_open_write_count, 1);
	} else {
		if (oh->doh_caching) {
			if (il_calls == 0) {
				DFUSE_TRA_DEBUG(oh, "Saving data cache");
				dfuse_dcache_set_time(oh->doh_ie);
			} else {
				DFUSE_TRA_DEBUG(oh, "Evicting cache");
				dfuse_cache_evict(oh->doh_ie);
			}
		}
	}
	DFUSE_TRA_DEBUG(oh, "il_calls %d, caching %d,", il_calls, oh->doh_caching);
	if (il_calls != 0) {
		atomic_fetch_sub_relaxed(&oh->doh_ie->ie_il_count, 1);
	}
	atomic_fetch_sub_relaxed(&oh->doh_ie->ie_open_count, 1);

	rc = dfs_release(oh->doh_obj);
	if (rc == 0)
		DFUSE_REPLY_ZERO(oh, req);
	else
		DFUSE_REPLY_ERR_RAW(oh, req, rc);
	D_FREE(oh);
}
