/****************************************************************************
 *  flashcache_main.c
 *  FlashCache: Device mapper target for block-level disk caching
 *
 *  Copyright 2010 Facebook, Inc.
 *  Author: Mohan Srinivasan (mohan@facebook.com)
 *
 *  Based on DM-Cache:
 *   Copyright (C) International Business Machines Corp., 2006
 *   Author: Ming Zhao (mingzhao@ufl.edu)
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include <asm/atomic.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/hardirq.h>
#include <linux/sysctl.h>
#include <linux/version.h>

#include "dm.h"
#include "dm-io.h"
#include "dm-bio-list.h"
#include "kcopyd.h"
#include "flashcache.h"
#include "flashcache_ioctl.h"

/*
 * TODO List :
 * 1) sysctls : Create per-cache device sysctls instead of global sysctls.
 * 2) Management of non cache pids : Needs improvement. Remove registration
 * on process exits (with  a pseudo filesstem'ish approach perhaps) ?
 * 3) Breaking up the cache spinlock : Right now contention on the spinlock
 * is not a problem. Might need change in future.
 * 4) Use the standard linked list manipulation macros instead rolling our own.
 * 5) Fix a security hole : A malicious process with 'ro' access to a file can 
 * potentially corrupt file data. This can be fixed by copying the data on a
 * cache read miss.
 */

static int flashcache_read_miss(struct cache_c *dmc, struct bio* bio,
				int index);
static int flashcache_write(struct cache_c *dmc, struct bio* bio);
static int flashcache_inval_blocks(struct cache_c *dmc, struct bio *bio);
static void flashcache_dirty_writeback(struct cache_c *dmc, int index);
void flashcache_sync_blocks(struct cache_c *dmc);

static int flashcache_find_nc_pid_locked(struct cache_c *dmc, pid_t pid);
static void flashcache_del_nc_pid_locked(struct cache_c *dmc, pid_t pid);
static void flashcache_nc_pid_expiry_locked(struct cache_c *dmc);

extern struct work_struct _kcached_wq;
extern u_int64_t size_hist[];

extern int sysctl_flashcache_max_nc_pids;
extern int sysctl_nc_pid_do_expiry;
extern int sysctl_nc_pid_expiry_check;
extern int sysctl_flashcache_error_inject;

extern int sysctl_flashcache_reclaim_policy;

void 
flashcache_io_callback(unsigned long error, void *context)
{
	struct kcached_job *job = (struct kcached_job *) context;
	struct cache_c *dmc = job->dmc;
	struct bio *bio;
	unsigned long flags;
	int index = job->index;
	struct cacheblock *cacheblk = &dmc->cache[index];
		
	bio = job->bio;
	VERIFY(bio != NULL);
	if (error)
		DMERR("flashcache_io_callback: io error %ld block %lu action %d", 
		      error, job->disk.sector, job->action);
	job->error = error;
	switch (job->action) {
	case READDISK:
		DPRINTK("flashcache_io_callback: READDISK  %d",
			index);
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		if (unlikely(sysctl_flashcache_error_inject & READDISK_ERROR)) {
			job->error = error = -EIO;
			sysctl_flashcache_error_inject &= ~READDISK_ERROR;
		}
		VERIFY(cacheblk->cache_state & DISKREADINPROG);
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		if (likely(error == 0)) {
			/* Kick off the write to the cache */
			job->action = READFILL;
			push_io(job);
			schedule_work(&_kcached_wq);
			return;
		} else {
			dmc->disk_read_errors++;			
			bio_endio(bio, bio->bi_size, error);
		}
		break;
	case READCACHE:
		DPRINTK("flashcache_io_callback: READCACHE %d",
			index);
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		if (unlikely(sysctl_flashcache_error_inject & READCACHE_ERROR)) {
			job->error = error = -EIO;
			sysctl_flashcache_error_inject &= ~READCACHE_ERROR;
		}
		VERIFY(cacheblk->cache_state & CACHEREADINPROG);
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		if (unlikely(error))
			dmc->ssd_read_errors++;
#ifdef FLASHCACHE_DO_CHECKSUMS
		if (likely(error == 0)) {
			if (flashcache_validate_checksum(job)) {
				DMERR("flashcache_io_callback: Checksum mismatch at disk offset %lu", 
				      job->disk.sector);
				error = -EIO;
			}
		}
#endif
		bio_endio(bio, bio->bi_size, error);
		break;		       
	case READFILL:
		DPRINTK("flashcache_io_callback: READFILL %d",
			index);
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		if (unlikely(sysctl_flashcache_error_inject & READFILL_ERROR)) {
			job->error = error = -EIO;
			sysctl_flashcache_error_inject &= ~READFILL_ERROR;
		}
		if (unlikely(error))
			dmc->ssd_write_errors++;
		VERIFY(cacheblk->cache_state & DISKREADINPROG);
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		bio_endio(bio, bio->bi_size, error);
		break;
	case WRITECACHE:
		DPRINTK("flashcache_io_callback: WRITECACHE %d",
			index);
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		if (unlikely(sysctl_flashcache_error_inject & WRITECACHE_ERROR)) {
			job->error = error = -EIO;
			sysctl_flashcache_error_inject &= ~WRITECACHE_ERROR;
		}
		VERIFY(cacheblk->cache_state & CACHEWRITEINPROG);
		if (likely(error == 0)) {
#ifdef FLASHCACHE_DO_CHECKSUMS
			dmc->checksum_store++;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
			flashcache_store_checksum(job);
			/* 
			 * We need to update the metadata on a DIRTY->DIRTY as well 
			 * since we save the checksums.
			 */
			push_md_io(job);
			schedule_work(&_kcached_wq);
			return;
#else
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
			/* Only do cache metadata update on a non-DIRTY->DIRTY transition */
			if ((cacheblk->cache_state & DIRTY) == 0) {
				push_md_io(job);
				schedule_work(&_kcached_wq);
				return;
			}
#endif
		} else {
			dmc->ssd_write_errors++;			
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		}
		bio_endio(bio, bio->bi_size, error);
		break;
	}
	/* 
	 * The INPROG flag is still set. We cannot turn that off until all the pending requests
	 * processed. We need to loop the pending requests back to a workqueue. We have the job,
	 * add it to the pending req queue.
	 */
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	if (unlikely(error || cacheblk->head)) {
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		push_pending(job);
		schedule_work(&_kcached_wq);
	} else {
		cacheblk->cache_state &= ~BLOCK_IO_INPROG;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		flashcache_free_cache_job(job);
		if (atomic_dec_and_test(&dmc->nr_jobs))
			wake_up(&dmc->destroyq);
	}
}

/* 
 * Common error handling for everything.
 * 1) If the block isn't dirty, invalidate it.
 * 2) Error all pending IOs that totally or partly overlap this block.
 * 3) Free the job.
 */
static void
flashcache_do_pending_error(struct kcached_job *job)
{
	struct cache_c *dmc = job->dmc;
	unsigned long flags;
	struct pending_job *pending_job;
	struct bio *bio;
	struct cacheblock *cacheblk = &dmc->cache[job->index];

	DMERR("flashcache_do_pending_error: error %d block %lu action %d", 
	      -job->error, job->disk.sector, job->action);
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	VERIFY(cacheblk->cache_state & VALID);
	/* Invalidate block if possible */
	if ((cacheblk->cache_state & DIRTY) == 0) {
		dmc->cached_blocks--;
		dmc->pending_inval++;
		cacheblk->cache_state &= ~VALID;
		cacheblk->cache_state |= INVALID;
	}
	while (cacheblk->head) {
		pending_job = cacheblk->head;
		cacheblk->head = pending_job->next;
		VERIFY(cacheblk->nr_queued > 0);
		cacheblk->nr_queued--;
		bio = pending_job->bio;
		bio_endio(bio, bio->bi_size, job->error);
		flashcache_free_pending_job(pending_job);
	}
	VERIFY(cacheblk->nr_queued == 0);
	cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	flashcache_free_cache_job(job);
	if (atomic_dec_and_test(&dmc->nr_jobs))
		wake_up(&dmc->destroyq);

}

static void
flashcache_do_pending_noerror(struct kcached_job *job)
{
	struct cache_c *dmc = job->dmc;
	int index = job->index;
	unsigned long flags;
	struct pending_job *pending_job;
	int queued;
	struct cacheblock *cacheblk = &dmc->cache[index];

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	if (cacheblk->cache_state & DIRTY) {
		cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
		cacheblk->cache_state |= DISKWRITEINPROG;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		flashcache_dirty_writeback(dmc, index);
		goto out;
	}
	DPRINTK("flashcache_do_pending: Index %d %lx",
		index, cacheblk->cache_state);
	VERIFY(cacheblk->cache_state & VALID);
	dmc->cached_blocks--;
	dmc->pending_inval++;
	cacheblk->cache_state &= ~VALID;
	cacheblk->cache_state |= INVALID;
	while (cacheblk->head) {
		VERIFY(!(cacheblk->cache_state & DIRTY));
		pending_job = cacheblk->head;
		cacheblk->head = pending_job->next;
		VERIFY(cacheblk->nr_queued > 0);
		cacheblk->nr_queued--;
		if (pending_job->action == INVALIDATE) {
			DPRINTK("flashcache_do_pending: INVALIDATE  %llu",
				next_job->bio->bi_sector);
			VERIFY(pending_job->bio != NULL);
			queued = flashcache_inval_blocks(dmc, pending_job->bio);
			if (queued) {
				if (unlikely(queued < 0)) {
					/*
					 * Memory allocation failure inside inval_blocks.
					 * Fail this io.
					 */
					bio_endio(pending_job->bio, pending_job->bio->bi_size, 
						  -EIO);
				}
				flashcache_free_pending_job(pending_job);
				continue;
			}
		}
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		pending_job->bio->bi_bdev = dmc->disk_dev->bdev;
		DPRINTK("flashcache_do_pending: Sending down IO %llu",
			pending_job->bio->bi_sector);
		generic_make_request(pending_job->bio);
		flashcache_free_pending_job(pending_job);
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	}
	VERIFY(cacheblk->nr_queued == 0);
	cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
out:
	flashcache_free_cache_job(job);
	if (atomic_dec_and_test(&dmc->nr_jobs))
		wake_up(&dmc->destroyq);
}

void
flashcache_do_pending(struct kcached_job *job)
{
	if (job->error)
		flashcache_do_pending_error(job);
	else
		flashcache_do_pending_noerror(job);
}

void
flashcache_do_io(struct kcached_job *job)
{
	int r = 0;
	struct bio *bio = job->bio;

	VERIFY(job->action == READFILL);
	/* Write to cache device */
#ifdef FLASHCACHE_DO_CHECKSUMS
	flashcache_store_checksum(job);
	job->dmc->checksum_store++;
#endif
	r = dm_io_async_bvec(1, &job->cache, WRITE, bio->bi_io_vec + bio->bi_idx,
			     flashcache_io_callback, job);
	flashcache_unplug_device(job->dmc->cache_dev->bdev);
	VERIFY(r == 0); /* In our case, dm_io_async_bvec() must always return 0 */
}

/*
 * Map a block from the source device to a block in the cache device.
 */
static unsigned long 
hash_block(struct cache_c *dmc, sector_t dbn)
{
	unsigned long set_number, value;

	value = (unsigned long)
		(dbn >> (dmc->block_shift + dmc->consecutive_shift));
	set_number = value % (dmc->size >> dmc->consecutive_shift);
	DPRINTK("Hash: %llu(%lu)->%lu", dbn, value, set_number);
	return set_number;
}

static void
find_valid_dbn(struct cache_c *dmc, sector_t dbn, 
	       int start_index, int *index)
{
	int i;
	int end_index = start_index + dmc->assoc;

	for (i = start_index ; i < end_index ; i++) {
		if (dbn == dmc->cache[i].dbn &&
		    (dmc->cache[i].cache_state & VALID)) {
			*index = i;
			if (sysctl_flashcache_reclaim_policy == FLASHCACHE_LRU &&
			    ((dmc->cache[i].cache_state & BLOCK_IO_INPROG) == 0))
				flashcache_reclaim_lru_movetail(dmc, i);
			return;
		}
	}
	*index = -1;
}

static int
find_invalid_dbn(struct cache_c *dmc, int start_index)
{
	int i;
	int end_index = start_index + dmc->assoc;
	
	/* Find INVALID slot that we can reuse */
	for (i = start_index ; i < end_index ; i++) {
		if (dmc->cache[i].cache_state == INVALID) {
			if (sysctl_flashcache_reclaim_policy == FLASHCACHE_LRU)
				flashcache_reclaim_lru_movetail(dmc, i);
			return i;
		}
	}
	return -1;
}

/* 
 * Search for a slot that we can reclaim.
 * The strategy is ~FIFO right now. sweep thru the set looking for
 * a block to recycle.
 * But we can come up with at least 2 other (configurable at cache create
 * time alternatives) for future.
 * LRU within a set : Maintain either a timestamp per block and do 
 * approximated LRU within each set. Or chain the LRU blocks within a 
 * set.
 */
static void
find_reclaim_dbn(struct cache_c *dmc, int start_index, int *index)
{
	int set = start_index / dmc->assoc;
	
	if (sysctl_flashcache_reclaim_policy == FLASHCACHE_FIFO) {
		int end_index = start_index + dmc->assoc;
		int slots_searched = 0;
		int i;

		i = dmc->cache_sets[set].set_fifo_next;
		while (slots_searched < dmc->assoc) {
			VERIFY(i >= start_index);
			VERIFY(i < end_index);
			if (dmc->cache[i].cache_state == VALID) {
				*index = i;
				break;
			}
			slots_searched++;
			i++;
			if (i == end_index)
				i = start_index;
		}
		i++;
		if (i == end_index)
			i = start_index;
		dmc->cache_sets[set].set_fifo_next = i;
	} else { /* flashcache_reclaim_policy == FLASHCACHE_LRU */
		struct cacheblock *cacheblk;
		int lru_rel_index;

		lru_rel_index = dmc->cache_sets[set].lru_head;
		while (lru_rel_index != FLASHCACHE_LRU_NULL) {
			cacheblk = &dmc->cache[lru_rel_index + start_index];
			if (cacheblk->cache_state == VALID) {
				VERIFY((cacheblk - &dmc->cache[0]) == 
				       (lru_rel_index + start_index));
				*index = cacheblk - &dmc->cache[0];
				flashcache_reclaim_lru_movetail(dmc, *index);
				break;
			}
			lru_rel_index = cacheblk->lru_next;
		}
	}
}

/* 
 * dbn is the starting sector, io_size is the number of sectors.
 */
static int 
flashcache_lookup(struct cache_c *dmc, struct bio *bio, int *index)
{
	sector_t dbn = bio->bi_sector;
#if DMC_DEBUG
	int io_size = to_sector(bio->bi_size);
#endif
	unsigned long set_number = hash_block(dmc, dbn);
	int invalid, oldest_clean = -1;
	int start_index;

	start_index = dmc->assoc * set_number;
	DPRINTK("Cache lookup : dbn %llu(%lu), set = %d",
		dbn, io_size, set_number);
	find_valid_dbn(dmc, dbn, start_index, index);
	if (*index > 0) {
		DPRINTK("Cache lookup HIT: Block %llu(%lu): VALID index %d",
			     dbn, io_size, *index);
		/* We found the exact range of blocks we are looking for */
		return VALID;
	}
	invalid = find_invalid_dbn(dmc, start_index);
	if (invalid == -1) {
		/* We didn't find an invalid entry, search for oldest valid entry */
		find_reclaim_dbn(dmc, start_index, &oldest_clean);
	}
	/* 
	 * Cache miss :
	 * We can't choose an entry marked INPROG, but choose the oldest
	 * INVALID or the oldest VALID entry.
	 */
	*index = start_index + dmc->assoc;
	if (invalid != -1) {
		DPRINTK("Cache lookup MISS (INVALID): dbn %llu(%lu), set = %d, index = %d, start_index = %d",
			     dbn, io_size, set_number, invalid, start_index);
		*index = invalid;
	} else if (oldest_clean != -1) {
		DPRINTK("Cache lookup MISS (VALID): dbn %llu(%lu), set = %d, index = %d, start_index = %d",
			     dbn, io_size, set_number, oldest_clean, start_index);
		*index = oldest_clean;
	} else {
		DPRINTK_LITE("Cache read lookup MISS (NOROOM): dbn %llu(%lu), set = %d",
			dbn, io_size, set_number);
	}
	if (*index < (start_index + dmc->assoc))
		return INVALID;
	else {
		dmc->noroom++;
		return -1;
	}
}

static void 
flashcache_enq_pending(struct cache_c *dmc, struct bio* bio,
		       int index, int action, struct pending_job *job)
{
	struct pending_job **nodepp;

	DPRINTK("flashcache_enq_pending: Queue to pending Q Index %d %llu",
		index, bio->bi_sector);
	VERIFY(job != NULL);
	job->action = action;
	job->next = NULL;
	job->bio = bio;
	nodepp = &dmc->cache[index].head;
	while (*nodepp != NULL)
		nodepp = &((*nodepp)->next);
	*nodepp = job;
	dmc->cache[index].nr_queued++;
	dmc->enqueues++;
}

/*
 * Cache Metadata Update functions 
 */
void 
flashcache_md_write_callback(unsigned long error, void *context)
{
	struct kcached_job *job = (struct kcached_job *)context;

	job->error = error;
	push_md_complete(job);
	schedule_work(&_kcached_wq);
}

static int
flashcache_alloc_md_sector(struct kcached_job *job)
{
	struct page *page;
	
	if (likely((sysctl_flashcache_error_inject & MD_ALLOC_SECTOR_ERROR) == 0))
		page = alloc_pages(GFP_NOIO, 0);
	else {
		sysctl_flashcache_error_inject &= ~MD_ALLOC_SECTOR_ERROR;
		page = NULL;
	}
	job->md_io_bvec.bv_page = page;
	if (unlikely(page == NULL)) {
		job->dmc->memory_alloc_errors++;
		return -ENOMEM;
	}
	job->md_io_bvec.bv_len = 512;
	job->md_io_bvec.bv_offset = 0;
	job->md_sector = (struct flash_cacheblock *)page_address(page);
	return 0;
}

static void
flashcache_free_md_sector(struct kcached_job *job)
{
	if (job->md_io_bvec.bv_page != NULL)
		__free_page(job->md_io_bvec.bv_page);
}

static void
flashcache_md_write_kickoff(struct kcached_job *job)
{
	struct cache_c *dmc = job->dmc;	
	struct flash_cacheblock *md_sector;
	int md_sector_ix;
	struct io_region where;
	int i;
	struct cache_md_sector_head *md_sector_head;
	struct kcached_job *orig_job = job;
	unsigned long flags;

	if (flashcache_alloc_md_sector(job)) {
		DMERR("flashcache: %d: Cache metadata write failed, cannot alloc page ! block %lu", 
		      job->action, job->disk.sector);
		flashcache_md_write_callback(-EIO, job);
		return;
	}
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	/*
	 * Transfer whatever is on the pending queue to the md_io_inprog queue.
	 */
	md_sector_head = &dmc->md_sectors_buf[INDEX_TO_MD_SECTOR(job->index)];
	md_sector_head->md_io_inprog = md_sector_head->pending_jobs;
	md_sector_head->pending_jobs = NULL;
	md_sector = job->md_sector;
	md_sector_ix = INDEX_TO_MD_SECTOR(job->index) * MD_BLOCKS_PER_SECTOR;
	/* First copy out the entire sector */
	for (i = 0 ; 
	     i < MD_BLOCKS_PER_SECTOR && md_sector_ix < dmc->size ; 
	     i++, md_sector_ix++) {
		md_sector[i].dbn = dmc->cache[md_sector_ix].dbn;
#ifdef FLASHCACHE_DO_CHECKSUMS
		md_sector[i].checksum = dmc->cache[md_sector_ix].checksum;
#endif
		md_sector[i].cache_state = 
			dmc->cache[md_sector_ix].cache_state & (VALID | INVALID | DIRTY);
	}
	/* Then set/clear the DIRTY bit for the "current" index */
	if (job->action == WRITECACHE) {
		/* DIRTY the cache block */
		md_sector[INDEX_TO_MD_SECTOR_OFFSET(job->index)].cache_state = 
			(VALID | DIRTY);
	} else { /* job->action == WRITEDISK* */
		/* un-DIRTY the cache block */
		md_sector[INDEX_TO_MD_SECTOR_OFFSET(job->index)].cache_state = VALID;
	}

	for (job = md_sector_head->md_io_inprog ; 
	     job != NULL ;
	     job = job->next) {
		if (job->action == WRITECACHE) {
			/* DIRTY the cache block */
			md_sector[INDEX_TO_MD_SECTOR_OFFSET(job->index)].cache_state = 
				(VALID | DIRTY);
		} else { /* job->action == WRITEDISK* */
			/* un-DIRTY the cache block */
			md_sector[INDEX_TO_MD_SECTOR_OFFSET(job->index)].cache_state = VALID;
		}
	}
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	where.bdev = dmc->cache_dev->bdev;
	where.count = 1;
	where.sector = 1 + INDEX_TO_MD_SECTOR(orig_job->index);
	dm_io_async_bvec(1, &where, WRITE,
			 &orig_job->md_io_bvec,
			 flashcache_md_write_callback, orig_job);
	flashcache_unplug_device(dmc->cache_dev->bdev);
}

void
flashcache_md_write_done(struct kcached_job *job)
{
	struct cache_c *dmc = job->dmc;
	struct cache_md_sector_head *md_sector_head;
	int index;
	unsigned long flags;
	struct kcached_job *job_list;
	int error = job->error;
	struct kcached_job *next;
	struct cacheblock *cacheblk;
		
	VERIFY(!in_interrupt());
	VERIFY(job->action == WRITEDISK || job->action == WRITECACHE || 
	       job->action == WRITEDISK_SYNC);
	flashcache_free_md_sector(job);
	job->md_sector = NULL;
	md_sector_head = &dmc->md_sectors_buf[INDEX_TO_MD_SECTOR(job->index)];
	job_list = job;
	job->next = md_sector_head->md_io_inprog;
	md_sector_head->md_io_inprog = NULL;
	for (job = job_list ; job != NULL ; job = next) {
		next = job->next;
		job->error = error;
		index = job->index;
		cacheblk = &dmc->cache[index];
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		if (job->action == WRITECACHE) {
			if (unlikely(sysctl_flashcache_error_inject & WRITECACHE_MD_ERROR)) {
				job->error = -EIO;
				sysctl_flashcache_error_inject &= ~WRITECACHE_MD_ERROR;
			}
			if (likely(job->error == 0)) {
				if ((cacheblk->cache_state & DIRTY) == 0) {
					dmc->cache_sets[index / dmc->assoc].nr_dirty++;
					dmc->nr_dirty++;
				}
				dmc->md_write_dirty++;
				cacheblk->cache_state |= DIRTY;
			} else
				dmc->ssd_write_errors++;
			bio_endio(job->bio, job->bio->bi_size, job->error);
			if (job->error || cacheblk->head) {
				if (job->error) {
					DMERR("flashcache: WRITE: Cache metadata write failed ! error %d block %lu", 
					      -job->error, cacheblk->dbn);
				}
				spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
				flashcache_do_pending(job);
			} else {
				cacheblk->cache_state &= ~BLOCK_IO_INPROG;
				spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
				flashcache_free_cache_job(job);
				if (atomic_dec_and_test(&dmc->nr_jobs))
					wake_up(&dmc->destroyq);
			}
		} else {
			int action = job->action;

			if (unlikely(sysctl_flashcache_error_inject & WRITEDISK_MD_ERROR)) {
				job->error = -EIO;
				sysctl_flashcache_error_inject &= ~WRITEDISK_MD_ERROR;
			}
			/*
			 * If we have an error on a WRITEDISK*, no choice but to preserve the 
			 * dirty block in cache. Fail any IOs for this block that occurred while
			 * the block was being cleaned.
			 */
			if (likely(job->error == 0)) {
				dmc->md_write_clean++;
				cacheblk->cache_state &= ~DIRTY;
				VERIFY(dmc->cache_sets[index / dmc->assoc].nr_dirty > 0);
				VERIFY(dmc->nr_dirty > 0);
				dmc->cache_sets[index / dmc->assoc].nr_dirty--;
				dmc->nr_dirty--;
			} else 
				dmc->ssd_write_errors++;
			VERIFY(dmc->cache_sets[index / dmc->assoc].clean_inprog > 0);
			VERIFY(dmc->clean_inprog > 0);
			dmc->cache_sets[index / dmc->assoc].clean_inprog--;
			dmc->clean_inprog--;
			if (job->error || cacheblk->head) {
				if (job->error) {
					DMERR("flashcache: CLEAN: Cache metadata write failed ! error %d block %lu", 
					      -job->error, cacheblk->dbn);
				}
				spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
				flashcache_do_pending(job);
				/* Kick off more cleanings */
				if (action == WRITEDISK)
					flashcache_clean_set(dmc, index / dmc->assoc);
				else
					flashcache_sync_blocks(dmc);
			} else {
				cacheblk->cache_state &= ~BLOCK_IO_INPROG;
				spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
				flashcache_free_cache_job(job);
				if (atomic_dec_and_test(&dmc->nr_jobs))
					wake_up(&dmc->destroyq);
				/* Kick off more cleanings */
				if (action == WRITEDISK)
					flashcache_clean_set(dmc, index / dmc->assoc);
				else
					flashcache_sync_blocks(dmc);
			}
			dmc->cleanings++;
			if (action == WRITEDISK_SYNC)
				flashcache_update_sync_progress(dmc);
		}
	}
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	if (md_sector_head->pending_jobs != NULL) {
		/* peel off the first job from the pending queue and kick that off */
		job = md_sector_head->pending_jobs;
		md_sector_head->pending_jobs = job->next;
		job->next = NULL;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		VERIFY(job->action == WRITEDISK || job->action == WRITECACHE ||
		       job->action == WRITEDISK_SYNC);
		flashcache_md_write_kickoff(job);
	} else {
		md_sector_head->nr_in_prog = 0;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}
}

/* 
 * Kick off a cache metadata update (called from workqueue).
 * Cache metadata update IOs to a given metadata sector are serialized using the 
 * nr_in_prog bit in the md sector bufhead.
 * If a metadata IO is already in progress, we queue up incoming metadata updates
 * on the pending_jobs list of the md sector bufhead. When kicking off an IO, we
 * cluster all these pending updates and do all of them as 1 flash write (that 
 * logic is in md_write_kickoff), where it switches out the entire pending_jobs
 * list and does all of those updates.
 */
void
flashcache_md_write(struct kcached_job *job)
{
	struct cache_c *dmc = job->dmc;
	struct cache_md_sector_head *md_sector_head;
	unsigned long flags;
	
	VERIFY(!in_interrupt());
	VERIFY(job->action == WRITEDISK || job->action == WRITECACHE || 
	       job->action == WRITEDISK_SYNC);
	md_sector_head = &dmc->md_sectors_buf[INDEX_TO_MD_SECTOR(job->index)];
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	/* If a write is in progress for this metadata sector, queue this update up */
	if (md_sector_head->nr_in_prog != 0) {
		struct kcached_job **nodepp;
		
		/* A MD update is already in progress, queue this one up for later */
		nodepp = &md_sector_head->pending_jobs;
		while (*nodepp != NULL)
			nodepp = &((*nodepp)->next);
		job->next = NULL;
		*nodepp = job;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	} else {
		md_sector_head->nr_in_prog = 1;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		flashcache_md_write_kickoff(job);
	}
}

static void 
flashcache_kcopyd_callback(int read_err, unsigned int write_err, void *context)
{
	struct kcached_job *job = (struct kcached_job *)context;
	struct cache_c *dmc = job->dmc;
	int index = job->index;
	unsigned long flags;

	VERIFY(!in_interrupt());
	DPRINTK("kcopyd_callback: Index %d", index);
	VERIFY(job->bio == NULL);
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	VERIFY(dmc->cache[index].cache_state & (DISKWRITEINPROG | VALID | DIRTY));
	if (unlikely(sysctl_flashcache_error_inject & KCOPYD_CALLBACK_ERROR)) {
		read_err = -EIO;
		sysctl_flashcache_error_inject &= ~KCOPYD_CALLBACK_ERROR;
	}
	if (likely(read_err == 0 && write_err == 0)) {
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		flashcache_md_write(job);
	} else {
		/* Disk write failed. We can not purge this block from flash */
		DMERR("flashcache: Disk writeback failed ! read error %d write error %d block %lu", 
		      -read_err, -write_err, job->disk.sector);
		VERIFY(dmc->cache_sets[index / dmc->assoc].clean_inprog > 0);
		VERIFY(dmc->clean_inprog > 0);
		dmc->cache_sets[index / dmc->assoc].clean_inprog--;
		dmc->clean_inprog--;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		/* Set the error in the job and let do_pending() handle the error */
		if (read_err) {
			dmc->ssd_read_errors++;			
			job->error = read_err;
		} else {
			dmc->disk_write_errors++;			
			job->error = write_err;
		}
		flashcache_do_pending(job);
		flashcache_clean_set(dmc, index / dmc->assoc); /* Kick off more cleanings */
		dmc->cleanings++;
	}
}

static void
flashcache_dirty_writeback(struct cache_c *dmc, int index)
{
	struct kcached_job *job;
	unsigned long flags;
	struct cacheblock *cacheblk = &dmc->cache[index];
	struct pending_job *pending_job;
	struct bio* bio;
	int device_removal = 0;
	
	DPRINTK("flashcache_dirty_writeback: Index %d", index);
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	VERIFY((cacheblk->cache_state & BLOCK_IO_INPROG) == DISKWRITEINPROG);
	VERIFY(cacheblk->cache_state & DIRTY);
	dmc->cache_sets[index / dmc->assoc].clean_inprog++;
	dmc->clean_inprog++;
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	job = new_kcached_job(dmc, NULL, index);
	if (unlikely(sysctl_flashcache_error_inject & DIRTY_WRITEBACK_JOB_ALLOC_FAIL)) {
		if (job)
			flashcache_free_cache_job(job);
		job = NULL;
		sysctl_flashcache_error_inject &= ~DIRTY_WRITEBACK_JOB_ALLOC_FAIL;
	}
	/*
	 * If the device is being (fast) removed, do not kick off any more cleanings.
	 */
	if (unlikely(atomic_read(&dmc->fast_remove_in_prog))) {
		DMERR("flashcache: Dirty Writeback (for set cleaning) aborted for device removal, block %lu", 
		      cacheblk->dbn);
		if (job)
			flashcache_free_cache_job(job);
		job = NULL;
		device_removal = 1;
	}
	if (unlikely(job == NULL)) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->cache_sets[index / dmc->assoc].clean_inprog--;
		dmc->clean_inprog--;
		while (cacheblk->head) {
			pending_job = cacheblk->head;
			cacheblk->head = pending_job->next;
			VERIFY(cacheblk->nr_queued > 0);
			cacheblk->nr_queued--;
			bio = pending_job->bio;
			bio_endio(bio, bio->bi_size, -EIO);
			flashcache_free_pending_job(pending_job);
		}
		VERIFY(cacheblk->nr_queued == 0);
		cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		if (device_removal == 0)
			DMERR("flashcache: Dirty Writeback (for set cleaning) failed ! Can't allocate memory, block %lu", 
			      cacheblk->dbn);
	} else {
		job->bio = NULL;
		job->action = WRITEDISK;
		atomic_inc(&dmc->nr_jobs);
		kcopyd_copy(dmc->kcp_client, &job->cache, 1, &job->disk, 0, 
			    flashcache_kcopyd_callback, job);
	}
}

/*
 * Clean dirty blocks in this set as needed.
 *
 * 1) Select the n blocks that we want to clean (choosing whatever policy), sort them.
 * 2) Then sweep the entire set looking for other DIRTY blocks that can be tacked onto
 * any of these blocks to form larger contigous writes. The idea here is that if you 
 * are going to do a write anyway, then we might as well opportunistically write out 
 * any contigous blocks for free (Bob's idea).
 */
void
flashcache_clean_set(struct cache_c *dmc, int set)
{
	unsigned long flags;
	int to_clean = 0;
	struct dbn_index_pair *writes_list;
	int nr_writes = 0;
	int start_index = set * dmc->assoc;
	
	/* 
	 * If a (fast) removal of this device is in progress, don't kick off 
	 * any more cleanings. This isn't sufficient though. We still need to
	 * stop cleanings inside flashcache_dirty_writeback() because we could
	 * have started a device remove after tested this here.
	 */
	if (atomic_read(&dmc->fast_remove_in_prog))
		return;
	writes_list = kmalloc(dmc->assoc * sizeof(struct dbn_index_pair), GFP_NOIO);
	if (unlikely(sysctl_flashcache_error_inject & WRITES_LIST_ALLOC_FAIL)) {
		if (writes_list)
			kfree(writes_list);
		writes_list = NULL;
		sysctl_flashcache_error_inject &= ~WRITES_LIST_ALLOC_FAIL;
	}
	if (writes_list == NULL) {
		dmc->memory_alloc_errors++;
		return;
	}
	dmc->clean_set_calls++;
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	if (dmc->cache_sets[set].nr_dirty < dmc->dirty_thresh_set) {
		dmc->clean_set_less_dirty++;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		kfree(writes_list);
		return;
	} else
		to_clean = dmc->cache_sets[set].nr_dirty - dmc->dirty_thresh_set;
	if (sysctl_flashcache_reclaim_policy == FLASHCACHE_FIFO) {
		int i, scanned;
		int start_index, end_index;

		start_index = set * dmc->assoc;
		end_index = start_index + dmc->assoc;
		scanned = 0;
		i = dmc->cache_sets[set].set_clean_next;
		DPRINTK("flashcache_clean_set: Set %d", set);
		while (scanned < dmc->assoc &&
		       ((dmc->cache_sets[set].clean_inprog + nr_writes) < dmc->max_clean_ios_set) &&
		       ((nr_writes + dmc->clean_inprog) < dmc->max_clean_ios_total) &&
		       nr_writes < to_clean) {
			if ((dmc->cache[i].cache_state & (DIRTY | BLOCK_IO_INPROG)) == DIRTY) {	
				dmc->cache[i].cache_state |= DISKWRITEINPROG;
				writes_list[nr_writes].dbn = dmc->cache[i].dbn;
				writes_list[nr_writes].index = i;
				nr_writes++;
			}
			scanned++;
			i++;
			if (i == end_index)
				i = start_index;
		}
		dmc->cache_sets[set].set_clean_next = i;
	} else { /* flashcache_reclaim_policy == FLASHCACHE_LRU */
		struct cacheblock *cacheblk;
		int lru_rel_index;

		lru_rel_index = dmc->cache_sets[set].lru_head;
		while (lru_rel_index != FLASHCACHE_LRU_NULL && 
		       ((dmc->cache_sets[set].clean_inprog + nr_writes) < dmc->max_clean_ios_set) &&
		       ((nr_writes + dmc->clean_inprog) < dmc->max_clean_ios_total) &&
		       dmc->cache_sets[set].clean_inprog < dmc->max_clean_ios_set &&
		       nr_writes < to_clean) {
			cacheblk = &dmc->cache[lru_rel_index + start_index];			
			if ((cacheblk->cache_state & (DIRTY | BLOCK_IO_INPROG)) == DIRTY) {
				cacheblk->cache_state |= DISKWRITEINPROG;
				writes_list[nr_writes].dbn = cacheblk->dbn;
				writes_list[nr_writes].index = cacheblk - &dmc->cache[0];
				nr_writes++;
			}
			lru_rel_index = cacheblk->lru_next;
		}
	}
	if (nr_writes > 0) {
		int i;

		flashcache_merge_writes(dmc, writes_list, &nr_writes, set);
		dmc->clean_set_ios += nr_writes;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		for (i = 0 ; i < nr_writes ; i++)
			flashcache_dirty_writeback(dmc, writes_list[i].index);
	} else {
		int do_delayed_clean = 0;

		if (dmc->cache_sets[set].nr_dirty > dmc->dirty_thresh_set)
			do_delayed_clean = 1;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		if (dmc->cache_sets[set].clean_inprog >= dmc->max_clean_ios_set)
			dmc->set_limit_reached++;
		if (dmc->clean_inprog >= dmc->max_clean_ios_total)
			dmc->total_limit_reached++;
		if (do_delayed_clean)
			schedule_delayed_work(&dmc->delayed_clean, 1*HZ);
		dmc->clean_set_fails++;
	}
	kfree(writes_list);
}

static int 
flashcache_read_miss(struct cache_c *dmc, struct bio* bio,
			  int index)
{
	struct kcached_job *job;
	struct cacheblock *cacheblk = &dmc->cache[index];
	struct pending_job *pending_job;

	job = new_kcached_job(dmc, bio, index);
	if (unlikely(sysctl_flashcache_error_inject & READ_MISS_JOB_ALLOC_FAIL)) {
		if (job)
			flashcache_free_cache_job(job);
		job = NULL;
		sysctl_flashcache_error_inject &= ~READ_MISS_JOB_ALLOC_FAIL;
	}
	if (unlikely(job == NULL)) {
		/* 
		 * We have a read miss, and can't allocate a job.
		 * Since we dropped the spinlock, we have to drain any 
		 * pending jobs.
		 */
		DMERR("flashcache: Read (miss) failed ! Can't allocate memory for cache IO, block %lu", 
		      cacheblk->dbn);
		bio_endio(bio, bio->bi_size, -EIO);
		spin_lock_irq(&dmc->cache_spin_lock);
		dmc->cached_blocks--;
		cacheblk->cache_state &= ~VALID;
		cacheblk->cache_state |= INVALID;
		while (cacheblk->head) {
			pending_job = cacheblk->head;
			cacheblk->head = pending_job->next;
			VERIFY(cacheblk->nr_queued > 0);
			cacheblk->nr_queued--;
			bio = pending_job->bio;
			bio_endio(bio, bio->bi_size, -EIO);
			flashcache_free_pending_job(pending_job);
		}
		VERIFY(cacheblk->nr_queued == 0);
		cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
		spin_unlock_irq(&dmc->cache_spin_lock);
	} else {
		job->action = READDISK; /* Fetch data from the source device */
		atomic_inc(&dmc->nr_jobs);
		dm_io_async_bvec(1, &job->disk, READ,
				 bio->bi_io_vec + bio->bi_idx,
				 flashcache_io_callback, job);
		flashcache_clean_set(dmc, index / dmc->assoc);
	}
	return DM_MAPIO_SUBMITTED;
}

static int
flashcache_read(struct cache_c *dmc, struct bio *bio)
{
	int index;
	int res;
	struct cacheblock *cacheblk;
	int queued;
	struct pending_job *pjob;

	DPRINTK("Got a %s for %llu  %u bytes)",
	        (bio_rw(bio) == READ ? "READ":"READA"), 
		bio->bi_sector, bio->bi_size);

	dmc->reads++;
	spin_lock_irq(&dmc->cache_spin_lock);
	res = flashcache_lookup(dmc, bio, &index);
	/* 
	 * Handle Cache Hit case first.
	 * We need to handle 2 cases, BUSY and !BUSY. If BUSY, we enqueue the
	 * bio for later.
	 */
	if (res > 0) {
		cacheblk = &dmc->cache[index];
		if ((cacheblk->cache_state & VALID) && 
		    (cacheblk->dbn == bio->bi_sector)) {
			if (!(cacheblk->cache_state & BLOCK_IO_INPROG) && (cacheblk->head == NULL)) {
				struct kcached_job *job;
			
				cacheblk->cache_state |= CACHEREADINPROG;
				dmc->read_hits++;
				spin_unlock_irq(&dmc->cache_spin_lock);
				DPRINTK("Cache read: Block %llu(%lu), index = %d:%s",
					bio->bi_sector, bio->bi_size, index, "CACHE HIT");
				job = new_kcached_job(dmc, bio, index);
				if (unlikely(sysctl_flashcache_error_inject & READ_HIT_JOB_ALLOC_FAIL)) {
					if (job)
						flashcache_free_cache_job(job);
					job = NULL;
					sysctl_flashcache_error_inject &= ~READ_HIT_JOB_ALLOC_FAIL;
				}
				if (unlikely(job == NULL)) {
					/* 
					 * We have a read hit, and can't allocate a job.
					 * Since we dropped the spinlock, we have to drain any 
					 * pending jobs.
					 */
					DMERR("flashcache: Read (hit) failed ! Can't allocate memory for cache IO, block %lu", 
					      cacheblk->dbn);
					bio_endio(bio, bio->bi_size, -EIO);
					spin_lock_irq(&dmc->cache_spin_lock);
					while (cacheblk->head) {
						pjob = cacheblk->head;
						cacheblk->head = pjob->next;
						VERIFY(cacheblk->nr_queued > 0);
						cacheblk->nr_queued--;
						bio = pjob->bio;
						bio_endio(bio, bio->bi_size, -EIO);
						flashcache_free_pending_job(pjob);
					}
					VERIFY(cacheblk->nr_queued == 0);
					cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
					spin_unlock_irq(&dmc->cache_spin_lock);
				} else {
					job->action = READCACHE; /* Fetch data from cache */
					atomic_inc(&dmc->nr_jobs);
					dm_io_async_bvec(1, &job->cache, READ,
							 bio->bi_io_vec + bio->bi_idx,
							 flashcache_io_callback, job);
					flashcache_unplug_device(dmc->cache_dev->bdev);
				}
			} else {
				pjob = flashcache_alloc_pending_job(dmc);
				if (unlikely(sysctl_flashcache_error_inject & READ_HIT_PENDING_JOB_ALLOC_FAIL)) {
					if (pjob) {
						flashcache_free_pending_job(pjob);
						pjob = NULL;
					}
					sysctl_flashcache_error_inject &= ~READ_HIT_PENDING_JOB_ALLOC_FAIL;
				}
				if (pjob == NULL)
					bio_endio(bio, bio->bi_size, -EIO);
				else
					flashcache_enq_pending(dmc, bio, index, READCACHE, pjob);
				spin_unlock_irq(&dmc->cache_spin_lock);
			}
			return DM_MAPIO_SUBMITTED;
		}
	}
	/*
	 * In all cases except for a cache hit (and VALID), test for potential 
	 * invalidations that we need to do.
	 */
	queued = flashcache_inval_blocks(dmc, bio);
	if (queued) {
		if (unlikely(queued < 0))
			bio_endio(bio, bio->bi_size, -EIO);
		spin_unlock_irq(&dmc->cache_spin_lock);
		return DM_MAPIO_SUBMITTED;
	}
	if (res == -1 || flashcache_find_nc_pid_locked(dmc, current->pid)) {
		/* No room or non-cacheable */
		spin_unlock_irq(&dmc->cache_spin_lock);
		DPRINTK("Cache read: Block %llu(%lu):%s",
			bio->bi_sector, bio->bi_size, "CACHE MISS & NO ROOM");
		if (res == -1)
			flashcache_clean_set(dmc, hash_block(dmc, bio->bi_sector));
		/* Forward to source device */
		bio->bi_bdev = dmc->disk_dev->bdev;
		return DM_MAPIO_REMAPPED;
	}
	/* 
	 * (res == INVALID) Cache Miss 
	 * And we found cache blocks to replace
	 * Claim the cache blocks before giving up the spinlock
	 */
	if (dmc->cache[index].cache_state & VALID)
		dmc->replace++;
	else
		dmc->cached_blocks++;
	dmc->cache[index].cache_state = VALID | DISKREADINPROG;
	dmc->cache[index].dbn = bio->bi_sector;
	spin_unlock_irq(&dmc->cache_spin_lock);

	DPRINTK("Cache read: Block %llu(%lu), index = %d:%s",
		bio->bi_sector, bio->bi_size, index, "CACHE MISS & REPLACE");
	return flashcache_read_miss(dmc, bio, index);
}

/*
 * Invalidate any colliding blocks if they are !BUSY and !DIRTY. If the colliding
 * block is DIRTY, we need to kick off a write. In both cases, we need to wait 
 * until the underlying IO is finished, and then proceed with the invalidation.
 */
static int
flashcache_inval_block_set(struct cache_c *dmc, int set, struct bio *bio, int rw,
			   struct pending_job *pjob)
{
	sector_t io_start = bio->bi_sector;
	sector_t io_end = bio->bi_sector + (to_sector(bio->bi_size) - 1);
	int start_index, end_index, i;
	struct cacheblock *cacheblk;
	
	start_index = dmc->assoc * set;
	end_index = start_index + dmc->assoc;
	for (i = start_index ; i < end_index ; i++) {
		sector_t start_dbn = dmc->cache[i].dbn;
		sector_t end_dbn = start_dbn + dmc->block_size;
		
		cacheblk = &dmc->cache[i];
		if (cacheblk->cache_state & INVALID)
			continue;
		if ((io_start >= start_dbn && io_start < end_dbn) ||
		    (io_end >= start_dbn && io_end < end_dbn)) {
			/* We have a match */
			if (rw == WRITE)
				dmc->wr_invalidates++;
			else
				dmc->rd_invalidates++;
			if (!(cacheblk->cache_state & (BLOCK_IO_INPROG | DIRTY)) &&
			    (cacheblk->head == NULL)) {
				dmc->cached_blocks--;			
				DPRINTK("Cache invalidate (!BUSY): Block %llu %lx",
					start_dbn, cacheblk->cache_state);
				cacheblk->cache_state = INVALID;
				continue;
			}
			/*
			 * The conflicting block has either IO in progress or is 
			 * Dirty. In all cases, we need to add ourselves to the 
			 * pending queue. Then if the block is dirty, we kick off
			 * an IO to clean the block. 
			 * Note that if the block is dirty and IO is in progress
			 * on it, the do_pending handler will clean the block
			 * and then process the pending queue.
			 */
			flashcache_enq_pending(dmc, bio, i, INVALIDATE, pjob);
			if ((cacheblk->cache_state & (DIRTY | BLOCK_IO_INPROG)) == DIRTY) {
				/* 
				 * Kick off block write.
				 * We can't kick off the write under the spinlock.
				 * Instead, we mark the slot DISKWRITEINPROG, drop 
				 * the spinlock and kick off the write. A block marked
				 * DISKWRITEINPROG cannot change underneath us. 
				 * to enqueue ourselves onto it's pending queue.
				 *
				 * XXX - The dropping of the lock here can be avoided if
				 * we punt the cleaning of the block to the worker thread,
				 * at the cost of a context switch.
				 */
				cacheblk->cache_state |= DISKWRITEINPROG;
				spin_unlock_irq(&dmc->cache_spin_lock);
				flashcache_dirty_writeback(dmc, i); /* Must inc nr_jobs */
				spin_lock_irq(&dmc->cache_spin_lock);
			}
			return 1;
		}
	}
	return 0;
}

/* 
 * Since md will break up IO into blocksize pieces, we only really need to check 
 * the start set and the end set for overlaps.
 */
static int
flashcache_inval_blocks(struct cache_c *dmc, struct bio *bio)
{	
	sector_t io_start = bio->bi_sector;
	sector_t io_end = bio->bi_sector + (to_sector(bio->bi_size) - 1);
	int start_set, end_set;
	int queued;
	struct pending_job *pjob1, *pjob2;

	pjob1 = flashcache_alloc_pending_job(dmc);
	if (unlikely(sysctl_flashcache_error_inject & INVAL_PENDING_JOB_ALLOC_FAIL)) {
		if (pjob1) {
			flashcache_free_pending_job(pjob1);
			pjob1 = NULL;
		}
		sysctl_flashcache_error_inject &= ~INVAL_PENDING_JOB_ALLOC_FAIL;
	}
	if (pjob1 == NULL) {
		queued = -ENOMEM;
		goto out;
	}
	pjob2 = flashcache_alloc_pending_job(dmc);
	if (pjob2 == NULL) {
		flashcache_free_pending_job(pjob1);
		queued = -ENOMEM;
		goto out;
	}
	start_set = hash_block(dmc, io_start);
	end_set = hash_block(dmc, io_end);
	queued = flashcache_inval_block_set(dmc, start_set, bio, 
					    bio_data_dir(bio), pjob1);
	if (queued) {
		flashcache_free_pending_job(pjob2);
		goto out;
	} else
		flashcache_free_pending_job(pjob1);		
	if (start_set != end_set) {
		queued = flashcache_inval_block_set(dmc, end_set, 
						    bio, bio_data_dir(bio), pjob2);
		if (!queued)
			flashcache_free_pending_job(pjob2);
	} else
		flashcache_free_pending_job(pjob2);		
out:
	return queued;
}

static int
flashcache_write(struct cache_c *dmc, struct bio *bio)
{
	int index;
	int res;
	struct kcached_job *job;
	struct cacheblock *cacheblk;
	struct pending_job *pjob;
	int queued;
	
	spin_lock_irq(&dmc->cache_spin_lock);
	dmc->writes++;
	res = flashcache_lookup(dmc, bio, &index);
	/*
	 * If cache hit and !BUSY, simply redirty page.
	 * If cache hit and BUSY, must wait for IO in prog to complete.
	 * If cache miss and found a block to recycle, we need to 
	 * (a) invalidate any partial hits, 
	 * (b) write to cache.
	 */
	if (res != -1) {
		/* Cache Hit */
		cacheblk = &dmc->cache[index];		
		if ((cacheblk->cache_state & VALID) && 
		    (cacheblk->dbn == bio->bi_sector)) {
			if (!(cacheblk->cache_state & BLOCK_IO_INPROG) &&
			    (cacheblk->head == NULL)) {
				if (cacheblk->cache_state & DIRTY)
					dmc->dirty_write_hits++;
				dmc->write_hits++;
				cacheblk->cache_state |= CACHEWRITEINPROG;
				spin_unlock_irq(&dmc->cache_spin_lock);
				job = new_kcached_job(dmc, bio, index);
				if (unlikely(sysctl_flashcache_error_inject & WRITE_HIT_JOB_ALLOC_FAIL)) {
					if (job)
						flashcache_free_cache_job(job);
					job = NULL;
					sysctl_flashcache_error_inject &= ~WRITE_HIT_JOB_ALLOC_FAIL;
				}
				if (unlikely(job == NULL)) {
					/* 
					 * We have a write hit, and can't allocate a job.
					 * Since we dropped the spinlock, we have to drain any 
					 * pending jobs.
					 */
					DMERR("flashcache: Write (hit) failed ! Can't allocate memory for cache IO, block %lu", 
					      cacheblk->dbn);
					bio_endio(bio, bio->bi_size, -EIO);
					spin_lock_irq(&dmc->cache_spin_lock);
					while (cacheblk->head) {
						pjob = cacheblk->head;
						cacheblk->head = pjob->next;
						VERIFY(cacheblk->nr_queued > 0);
						cacheblk->nr_queued--;
						bio = pjob->bio;
						bio_endio(bio, bio->bi_size, -EIO);
						flashcache_free_pending_job(pjob);
					}
					VERIFY(cacheblk->nr_queued == 0);
					cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
					spin_unlock_irq(&dmc->cache_spin_lock);
				} else {
					job->action = WRITECACHE; /* Write data to the source device */
					DPRINTK("Queue job for %llu", bio->bi_sector);
					atomic_inc(&dmc->nr_jobs);
					dm_io_async_bvec(1, &job->cache, WRITE, 
							 bio->bi_io_vec + bio->bi_idx,
							 flashcache_io_callback, job);
					flashcache_unplug_device(dmc->cache_dev->bdev);
					flashcache_clean_set(dmc, index / dmc->assoc);
				}
			} else {
				pjob = flashcache_alloc_pending_job(dmc);
				if (unlikely(sysctl_flashcache_error_inject & WRITE_HIT_PENDING_JOB_ALLOC_FAIL)) {
					if (pjob) {
						flashcache_free_pending_job(pjob);
						pjob = NULL;
					}
					sysctl_flashcache_error_inject &= ~WRITE_HIT_PENDING_JOB_ALLOC_FAIL;
				}
				if (unlikely(pjob == NULL))
					bio_endio(bio, bio->bi_size, -EIO);
				else
					flashcache_enq_pending(dmc, bio, index, WRITECACHE, pjob);
				spin_unlock_irq(&dmc->cache_spin_lock);
			}
			return DM_MAPIO_SUBMITTED;
		}
		/* Cache Miss, found block to recycle */
		queued = flashcache_inval_blocks(dmc, bio);
		if (queued) {
			if (unlikely(queued < 0))
				bio_endio(bio, bio->bi_size, -EIO);
			spin_unlock_irq(&dmc->cache_spin_lock);
			return DM_MAPIO_SUBMITTED;
		}
		if (cacheblk->cache_state & VALID)
			dmc->wr_replace++;
		else
			dmc->cached_blocks++;
		cacheblk->cache_state = VALID | CACHEWRITEINPROG;
		cacheblk->dbn = bio->bi_sector;
		spin_unlock_irq(&dmc->cache_spin_lock);
		job = new_kcached_job(dmc, bio, index);
		if (unlikely(sysctl_flashcache_error_inject & WRITE_MISS_JOB_ALLOC_FAIL)) {
			if (job)
				flashcache_free_cache_job(job);
			job = NULL;
			sysctl_flashcache_error_inject &= ~WRITE_MISS_JOB_ALLOC_FAIL;
		}
		if (unlikely(job == NULL)) {
			/* 
			 * We have a write miss, and can't allocate a job.
			 * Since we dropped the spinlock, we have to drain any 
			 * pending jobs.
			 */
			DMERR("flashcache: Write (miss) failed ! Can't allocate memory for cache IO, block %lu", 
			      cacheblk->dbn);
			bio_endio(bio, bio->bi_size, -EIO);
			spin_lock_irq(&dmc->cache_spin_lock);
			dmc->cached_blocks--;
			cacheblk->cache_state &= ~VALID;
			cacheblk->cache_state |= INVALID;
			while (cacheblk->head) {
				pjob = cacheblk->head;
				cacheblk->head = pjob->next;
				VERIFY(cacheblk->nr_queued > 0);
				cacheblk->nr_queued--;
				bio = pjob->bio;
				bio_endio(bio, bio->bi_size, -EIO);
				flashcache_free_pending_job(pjob);
			}
			VERIFY(cacheblk->nr_queued == 0);
			cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
			spin_unlock_irq(&dmc->cache_spin_lock);
		} else {
			job->action = WRITECACHE; 
			atomic_inc(&dmc->nr_jobs);
			dm_io_async_bvec(1, &job->cache, WRITE, 
					 bio->bi_io_vec + bio->bi_idx,
					 flashcache_io_callback, job);
			flashcache_unplug_device(dmc->cache_dev->bdev);
			flashcache_clean_set(dmc, index / dmc->assoc);
		}
		return DM_MAPIO_SUBMITTED;
	}
	/*
	 * No room in the set. We cannot write to the cache and have to 
	 * send the request to disk. Before we do that, we must check 
	 * for potential invalidations !
	 */
	queued = flashcache_inval_blocks(dmc, bio);
	if (queued) {
		if (unlikely(queued < 0))
			bio_endio(bio, bio->bi_size, -EIO);
		spin_unlock_irq(&dmc->cache_spin_lock);
		return DM_MAPIO_SUBMITTED;
	}
	spin_unlock_irq(&dmc->cache_spin_lock);
	flashcache_clean_set(dmc, hash_block(dmc, bio->bi_sector));
	/* Forward to source device */
	bio->bi_bdev = dmc->disk_dev->bdev;
	return DM_MAPIO_REMAPPED;
}

/*
 * Decide the mapping and perform necessary cache operations for a bio request.
 */
int 
flashcache_map(struct dm_target *ti, struct bio *bio,
	       union map_info *map_context)
{
	struct cache_c *dmc = (struct cache_c *) ti->private;
	int sectors = to_sector(bio->bi_size);
	int queued;
	
	if (sectors <= 32)
		size_hist[sectors]++;

	if (bio_barrier(bio))
		return -EOPNOTSUPP;

	VERIFY(to_sector(bio->bi_size) <= dmc->block_size);

	spin_lock_irq(&dmc->cache_spin_lock);
	if (unlikely(sysctl_nc_pid_do_expiry && 
		     (dmc->nc_pid_list_head != NULL)))
		flashcache_nc_pid_expiry_locked(dmc);
	if ((to_sector(bio->bi_size) != dmc->block_size) ||
	    (bio_data_dir(bio) == WRITE && 
	     flashcache_find_nc_pid_locked(dmc, current->pid))) {
		queued = flashcache_inval_blocks(dmc, bio);
		spin_unlock_irq(&dmc->cache_spin_lock);
		if (queued) {
			if (unlikely(queued < 0))
				bio_endio(bio, bio->bi_size, -EIO);
			return DM_MAPIO_SUBMITTED;
		} else {
			/* Forward to source device */
			bio->bi_bdev = dmc->disk_dev->bdev;
			return DM_MAPIO_REMAPPED;
		}
	} else
		spin_unlock_irq(&dmc->cache_spin_lock);		

	if (bio_data_dir(bio) == READ)
		return flashcache_read(dmc, bio);
	else
		return flashcache_write(dmc, bio);
}

/* Block sync support functions */
static void 
flashcache_kcopyd_callback_sync(int read_err, unsigned int write_err, void *context)
{
	struct kcached_job *job = (struct kcached_job *)context;
	struct cache_c *dmc = job->dmc;
	int index = job->index;
	unsigned long flags;

	VERIFY(!in_interrupt());
	DPRINTK("kcopyd_callback_sync: Index %d", index);
	VERIFY(job->bio == NULL);
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	VERIFY(dmc->cache[index].cache_state & (DISKWRITEINPROG | VALID | DIRTY));
	if (likely(read_err == 0 && write_err == 0)) {
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		flashcache_md_write(job);
	} else {
		/* Disk write failed. We can not purge this cache from flash */
		DMERR("flashcache: Disk writeback failed ! read error %d write error %d block %lu", 
		      -read_err, -write_err, job->disk.sector);
		VERIFY(dmc->cache_sets[index / dmc->assoc].clean_inprog > 0);
		VERIFY(dmc->clean_inprog > 0);
		dmc->cache_sets[index / dmc->assoc].clean_inprog--;
		dmc->clean_inprog--;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		/* Set the error in the job and let do_pending() handle the error */
		if (read_err) {
			dmc->ssd_read_errors++;
			job->error = read_err;
		} else {
			dmc->disk_write_errors++;			
			job->error = write_err;
		}
		flashcache_do_pending(job);
		flashcache_sync_blocks(dmc);  /* Kick off more cleanings */
		dmc->cleanings++;
	}
}

static void
flashcache_dirty_writeback_sync(struct cache_c *dmc, int index)
{
	struct kcached_job *job;
	unsigned long flags;
	struct cacheblock *cacheblk = &dmc->cache[index];
	struct pending_job *pending_job;
	struct bio* bio;
	int device_removal = 0;
	
	DPRINTK("flashcache_dirty_writeback_sync: Index %d", index);
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	VERIFY((cacheblk->cache_state & BLOCK_IO_INPROG) == DISKWRITEINPROG);
	VERIFY(cacheblk->cache_state & DIRTY);
	dmc->cache_sets[index / dmc->assoc].clean_inprog++;
	dmc->clean_inprog++;
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	job = new_kcached_job(dmc, NULL, index);
	/*
	 * If the device is being (fast) removed, do not kick off any more cleanings.
	 */
	if (unlikely(atomic_read(&dmc->fast_remove_in_prog))) {
		DMERR("flashcache: Dirty Writeback (for set cleaning) aborted for device removal, block %lu", 
		      cacheblk->dbn);
		if (job)
			flashcache_free_cache_job(job);
		job = NULL;
		device_removal = 1;
	}
	if (unlikely(job == NULL)) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->cache_sets[index / dmc->assoc].clean_inprog--;
		dmc->clean_inprog--;
		while (cacheblk->head) {
			pending_job = cacheblk->head;
			cacheblk->head = pending_job->next;
			VERIFY(cacheblk->nr_queued > 0);
			cacheblk->nr_queued--;
			bio = pending_job->bio;
			bio_endio(bio, bio->bi_size, -EIO);
			flashcache_free_pending_job(pending_job);
		}
		VERIFY(cacheblk->nr_queued == 0);
		cacheblk->cache_state &= ~(BLOCK_IO_INPROG);
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		if (device_removal == 0)
			DMERR("flashcache: Dirty Writeback (for sync) failed ! Can't allocate memory, block %lu", 
			      cacheblk->dbn);
	} else {
		job->bio = NULL;
		job->action = WRITEDISK_SYNC;
		atomic_inc(&dmc->nr_jobs);
		kcopyd_copy(dmc->kcp_client, &job->cache, 1, &job->disk, 0, 
			    flashcache_kcopyd_callback_sync, job);
	}
}

/* 
 * Sync all dirty blocks. We pick off dirty blocks, sort them, merge them with 
 * any contigous blocks we can within the set and fire off the writes.
 */
void
flashcache_sync_blocks(struct cache_c *dmc)
{
	unsigned long flags;
	int index;
	struct dbn_index_pair *writes_list;
	int nr_writes;
	int i, set;
	struct cacheblock *cacheblk;

	/* 
	 * If a (fast) removal of this device is in progress, don't kick off 
	 * any more cleanings. This isn't sufficient though. We still need to
	 * stop cleanings inside flashcache_dirty_writeback_sync() because we could
	 * have started a device remove after tested this here.
	 */
	if (atomic_read(&dmc->fast_remove_in_prog))
		return;
	writes_list = kmalloc(dmc->assoc * sizeof(struct dbn_index_pair), GFP_NOIO);
	if (writes_list == NULL) {
		dmc->memory_alloc_errors++;
		return;
	}
	nr_writes = 0;
	set = -1;
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);	
	index = dmc->sync_index;
	while (index < dmc->size && 
	       (nr_writes + dmc->clean_inprog) < dmc->max_clean_ios_total) {
		VERIFY(nr_writes <= dmc->assoc);
		if (((index % dmc->assoc) == 0) && (nr_writes > 0)) {
			/*
			 * Crossing a set, sort/merge all the IOs collected so
			 * far and issue the writes.
			 */
			VERIFY(set != -1);
			flashcache_merge_writes(dmc, writes_list, &nr_writes, set);
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
			for (i = 0 ; i < nr_writes ; i++)
				flashcache_dirty_writeback_sync(dmc, writes_list[i].index);
			nr_writes = 0;
			set = -1;
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		}
		cacheblk = &dmc->cache[index];
		if ((cacheblk->cache_state & (DIRTY | BLOCK_IO_INPROG)) == DIRTY) {
			cacheblk->cache_state |= DISKWRITEINPROG;
			writes_list[nr_writes].dbn = cacheblk->dbn;
			writes_list[nr_writes].index = cacheblk - &dmc->cache[0];
			set = index / dmc->assoc;
			nr_writes++;
		}
		index++;
	}
	dmc->sync_index = index;
	if (nr_writes > 0) {
		VERIFY(set != -1);
		flashcache_merge_writes(dmc, writes_list, &nr_writes, set);
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		for (i = 0 ; i < nr_writes ; i++)
			flashcache_dirty_writeback_sync(dmc, writes_list[i].index);
	} else
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	kfree(writes_list);
}

void
flashcache_sync_all(struct cache_c *dmc)
{
	unsigned long flags;

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	dmc->sync_index = 0;
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);	
	flashcache_sync_blocks(dmc);
}

static int
flashcache_find_nc_pid_locked(struct cache_c *dmc, pid_t pid)
{
	struct flashcache_non_cacheable_pid *node;

	for (node = dmc->nc_pid_list_head ; node != NULL ; node = node->next) {
		if (node->pid == pid)
			return 1;
	}
	return 0;
}

static void
flashcache_drop_nc_pids(struct cache_c *dmc)
{
	while (dmc->num_nc_pids >= sysctl_flashcache_max_nc_pids) {
		VERIFY(dmc->nc_pid_list_tail != NULL);
		flashcache_del_nc_pid_locked(dmc, dmc->nc_pid_list_tail->pid);
		dmc->nc_pid_drops++;
	}
}

static void
flashcache_add_nc_pid(struct cache_c *dmc, pid_t pid)
{
	struct flashcache_non_cacheable_pid *new;
	unsigned long flags;

	new = kmalloc(sizeof(struct flashcache_non_cacheable_pid), 
		      GFP_KERNEL);
	new->pid = pid;
	new->next = NULL;
	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	if (dmc->num_nc_pids > sysctl_flashcache_max_nc_pids)
		flashcache_drop_nc_pids(dmc);
	if (flashcache_find_nc_pid_locked(dmc, pid) == 0) {
		/* Add the new nc pid to the tail */
		new->prev = dmc->nc_pid_list_tail;
		if (dmc->nc_pid_list_head == NULL) {
			VERIFY(dmc->nc_pid_list_tail == NULL);
			dmc->nc_pid_list_head = new;
		} else {
			VERIFY(dmc->nc_pid_list_tail != NULL);
			dmc->nc_pid_list_tail->next = new;
		}
		dmc->nc_pid_list_tail = new;
		dmc->num_nc_pids++;
		dmc->nc_pid_adds++;
		/* When adding the first entry to list, set expiry check timeout */
		if (dmc->nc_pid_list_head == new)
			dmc->nc_pid_expire_check = 
				jiffies + ((sysctl_nc_pid_expiry_check + 1) * HZ);
	} else
		kfree(new);
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	return;
}

static void
flashcache_del_nc_pid_locked(struct cache_c *dmc, pid_t pid)
{
	struct flashcache_non_cacheable_pid *node;

	for (node = dmc->nc_pid_list_tail ; node != NULL ; node = node->prev) {
		VERIFY(dmc->num_nc_pids > 0);
		if (node->pid == pid) {
			if (node->prev == NULL) {
				dmc->nc_pid_list_head = node->next;
				if (node->next)
					node->next->prev = NULL;
			} else
				node->prev->next = node->next;
			if (node->next == NULL) {
				dmc->nc_pid_list_tail = node->prev;
				if (node->prev)
					node->prev->next = NULL;
			} else
				node->next->prev = node->prev;
			kfree(node);
			dmc->num_nc_pids--;
			return;
		}
	}
}

static void
flashcache_del_nc_pid(struct cache_c *dmc, pid_t pid)
{
	unsigned long flags;

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	flashcache_del_nc_pid_locked(dmc, pid);
	dmc->nc_pid_dels++;
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
}

void
flashcache_del_nc_all(struct cache_c *dmc)
{
	struct flashcache_non_cacheable_pid *node, *next;
	unsigned long flags;

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	node = dmc->nc_pid_list_head;
	while (node != NULL) {
		VERIFY(dmc->num_nc_pids > 0);
		next = node->next;
		kfree(node);
		dmc->num_nc_pids--;
		node = next;
	}
	dmc->nc_pid_list_head = dmc->nc_pid_list_tail = NULL;
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
}

static void
flashcache_nc_pid_expiry_locked(struct cache_c *dmc)
{
	struct flashcache_non_cacheable_pid *node;

	if (likely(time_before(jiffies, dmc->nc_pid_expire_check)))
		return;
	for (node = dmc->nc_pid_list_head ; node != NULL ; node = node->next) {
		VERIFY(dmc->num_nc_pids > 0);
		if (time_after(node->expiry, jiffies))
			continue;
		if (node->prev == NULL) {
			dmc->nc_pid_list_head = node->next;
			if (node->next)
				node->next->prev = NULL;
		} else
			node->prev->next = node->next;
		if (node->next == NULL) {
			dmc->nc_pid_list_tail = node->prev;
			if (node->prev)
				node->prev->next = NULL;
		} else
			node->next->prev = node->prev;
		kfree(node);
		dmc->num_nc_pids--;
		dmc->nc_expiry++;
	}
	dmc->nc_pid_expire_check = jiffies + (sysctl_nc_pid_expiry_check + 1) * HZ;
}

/*
 * Add/del pids whose IOs should be non-cacheable.
 * We limit this number to 100 (arbitrary and sysctl'able).
 * We also add an expiry to each entry (defaluts at 60 sec,
 * arbitrary and sysctlable).
 * This is needed because Linux lacks an "at_exit()" hook
 * that modules can supply to do any cleanup on process 
 * exit, for cases where the process dies after marking itself
 * non-cacheable.
 */
int 
flashcache_ioctl(struct dm_target *ti, struct inode *inode,
		 struct file *filp, unsigned int cmd,
		 unsigned long arg)
{
	struct cache_c *dmc = (struct cache_c *) ti->private;
	struct block_device *bdev = dmc->disk_dev->bdev;
	struct file fake_file = {};
	struct dentry fake_dentry = {};
	pid_t pid;

	switch(cmd) {
	case FLASHCACHEADDNCPID:
		if (copy_from_user(&pid, (pid_t *)arg, sizeof(pid_t)))
			return -EFAULT;
		flashcache_add_nc_pid(dmc, pid);
		return 0;
	case FLASHCACHEDELNCPID:
		if (copy_from_user(&pid, (pid_t *)arg, sizeof(pid_t)))
			return -EFAULT;
		flashcache_del_nc_pid(dmc, pid);
		return 0;
	case FLASHCACHEDELNCALL:
		flashcache_del_nc_all(dmc);
		return 0;
	default:
		fake_file.f_mode = dmc->disk_dev->mode;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
		fake_file.f_dentry = &fake_dentry;
#else
		fake_file.f_path.dentry = &fake_dentry;
#endif
		fake_dentry.d_inode = bdev->bd_inode;
		return blkdev_driver_ioctl(bdev->bd_inode, &fake_file, bdev->bd_disk, cmd, arg);
	}
}

EXPORT_SYMBOL(flashcache_io_callback);
EXPORT_SYMBOL(flashcache_do_pending_error);
EXPORT_SYMBOL(flashcache_do_pending_noerror);
EXPORT_SYMBOL(flashcache_do_pending);
EXPORT_SYMBOL(flashcache_do_io);
EXPORT_SYMBOL(flashcache_map);
EXPORT_SYMBOL(flashcache_write);
EXPORT_SYMBOL(flashcache_inval_blocks);
EXPORT_SYMBOL(flashcache_inval_block_set);
EXPORT_SYMBOL(flashcache_read);
EXPORT_SYMBOL(flashcache_read_miss);
EXPORT_SYMBOL(flashcache_clean_set);
EXPORT_SYMBOL(flashcache_dirty_writeback);
EXPORT_SYMBOL(flashcache_kcopyd_callback);
EXPORT_SYMBOL(flashcache_enq_pending);
EXPORT_SYMBOL(flashcache_lookup);
EXPORT_SYMBOL(flashcache_alloc_md_sector);
EXPORT_SYMBOL(flashcache_free_md_sector);
EXPORT_SYMBOL(flashcache_md_write_callback);
EXPORT_SYMBOL(flashcache_md_write_kickoff);
EXPORT_SYMBOL(flashcache_md_write_done);
EXPORT_SYMBOL(flashcache_md_write);


