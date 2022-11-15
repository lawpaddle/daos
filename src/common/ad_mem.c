/**
 * (C) Copyright 2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#define D_LOGFAC        DD_FAC(common)

#include "ad_mem.h"

#define AD_MEM_DEBUG	0

static struct ad_arena *arena_alloc(struct ad_blob *blob, bool force, int sorter_sz);
static int arena_init_sorters(struct ad_arena *arena, int sorter_sz);
static void arena_free(struct ad_arena *arena, bool force);
static void arena_unbind(struct ad_arena *arena, bool reset);

static struct ad_group *alloc_group(struct ad_arena *arena, bool force);
static void group_free(struct ad_group *grp, bool force);
static void group_unbind(struct ad_group *grp, bool reset);

static void blob_fini(struct ad_blob *blob);
static int blob_register_arena(struct ad_blob *blob, unsigned int arena_type,
			       struct ad_group_spec *specs, unsigned int specs_nr,
			       struct ad_tx *tx);
static int arena_reserve(struct ad_blob *blob, unsigned int type,
			 struct umem_store *store_extern, struct ad_arena **arena_p);
static int arena_tx_publish(struct ad_arena *arena, struct ad_tx *tx);
static void arena_debug_sorter(struct ad_arena *arena);
static inline int group_unit_avail(const struct ad_group_df *gd);
static inline int group_weight(const struct ad_group_df *gd);
static int find_bits(uint64_t *used, uint64_t *reserved, int bmap_sz, int bits_min, int *bits);

/* default group specs */
static struct ad_group_spec grp_specs_def[] = {
	{
		.gs_unit	= 64,
		.gs_count	= 512,
	},	/* group size = 32K */
	{
		.gs_unit	= 128,
		.gs_count	= 512,
	},	/* group size = 64K */
	{
		.gs_unit	= 256,
		.gs_count	= 512,
	},	/* group size = 128K */
	{
		.gs_unit	= 512,
		.gs_count	= 512,
	},	/* group size = 256K */
	{
		.gs_unit	= 1024,
		.gs_count	= 256,
	},	/* group size = 256K */
	{
		.gs_unit	= 2048,
		.gs_count	= 128,
	},	/* group size = 256K */
	{
		.gs_unit	= 4096,
		.gs_count	= 64,
	},	/* group size = 256K */
};

static struct ad_blob	*dummy_blob;

static inline void setbits64(uint64_t *bmap, int at, int bits)
{
	setbit_range((uint8_t *)bmap, at, at + bits - 1);
}

#define setbit64(bm, at)	setbits64(bm, at, 1)

static inline void clrbits64(uint64_t *bmap, int at, int bits)
{
	clrbit_range((uint8_t *)bmap, at, at + bits - 1);
}

#define clrbit64(bm, at)	clrbits64(bm, at, 1)

static inline bool isset64(uint64_t *bmap, int at)
{
	return isset((uint8_t *)bmap, at);
}

static struct ad_group *
group_df2ptr(const struct ad_group_df *gd)
{
	return (struct ad_group *)(unsigned long)gd->gd_back_ptr;
}

static struct ad_arena *
arena_df2ptr(const struct ad_arena_df *ad)
{
	return (struct ad_arena *)(unsigned long)ad->ad_back_ptr;
}

static void
group_addref(struct ad_group *grp)
{
	grp->gp_ref++;
}

static void
group_decref(struct ad_group *grp)
{
	D_ASSERT(grp->gp_ref > 0);
	grp->gp_ref--;
	if (grp->gp_ref == 0)
		group_free(grp, false);
}

static void
arena_addref(struct ad_arena *arena)
{
	arena->ar_ref++;
}

static void
arena_decref(struct ad_arena *arena)
{
	D_ASSERT(arena->ar_ref > 0);
	arena->ar_ref--;
	if (arena->ar_ref == 0)
		arena_free(arena, false);
}

static inline int
arena2id(struct ad_arena *arena)
{
	return arena->ar_df->ad_id;
}

void
blob_addref(struct ad_blob *blob)
{
	blob->bb_ref++;
}

void
blob_decref(struct ad_blob *blob)
{
	D_ASSERT(blob->bb_ref > 0);
	blob->bb_ref--;
	if (blob->bb_ref == 0) {
		blob_fini(blob);
		D_FREE(blob);
	}
}

static int
blob_bmap_size(struct ad_blob *blob)
{
	return (blob->bb_pgs_nr + 7) >> 3;
}

#define GROUP_LRU_MAX	(256 << 10)
#define ARENA_LRU_MAX	(64 << 10)

static bool
arena_free_heap_node_cmp(struct d_binheap_node *a, struct d_binheap_node *b)
{
	struct ad_maxheap_node *nodea, *nodeb;

	nodea = container_of(a, struct ad_maxheap_node, mh_node);
	nodeb = container_of(b, struct ad_maxheap_node, mh_node);

	if (nodea->mh_weight == nodeb->mh_weight)
		return nodea->mh_arena_id < nodeb->mh_arena_id;

	/* Max heap, the largest free extent is heap root */
	return nodea->mh_weight > nodeb->mh_weight;
}

static int
arena_free_heap_node_enter(struct d_binheap *h, struct d_binheap_node *e)
{
	struct ad_maxheap_node *node;

	D_ASSERT(h != NULL);
	D_ASSERT(e != NULL);

	node = container_of(e, struct ad_maxheap_node, mh_node);
	node->mh_in_tree = 1;

	return 0;
}

static int
arena_free_heap_node_exit(struct d_binheap *h, struct d_binheap_node *e)
{
	struct ad_maxheap_node *node;

	D_ASSERT(h != NULL);
	D_ASSERT(e != NULL);

	node = container_of(e, struct ad_maxheap_node, mh_node);
	node->mh_in_tree = 0;

	return 0;
}

static struct d_binheap_ops arena_free_heap_ops = {
	.hop_enter	= arena_free_heap_node_enter,
	.hop_exit	= arena_free_heap_node_exit,
	.hop_compare	= arena_free_heap_node_cmp,
};

static int
blob_init(struct ad_blob *blob)
{
	char			*buf = NULL;
	int			i;
	int			rc;

	D_ASSERT(blob->bb_pgs_nr > 0);

	D_INIT_LIST_HEAD(&blob->bb_ars_lru);
	D_INIT_LIST_HEAD(&blob->bb_ars_rsv);
	D_INIT_LIST_HEAD(&blob->bb_gps_lru);
	D_INIT_LIST_HEAD(&blob->bb_gps_rsv);
	D_INIT_LIST_HEAD(&blob->bb_pgs_ckpt);
	D_INIT_LIST_HEAD(&blob->bb_pgs_extern);

	rc = d_binheap_create_inplace(DBH_FT_NOLOCK, 0, NULL, &arena_free_heap_ops,
				      &blob->bb_arena_free_heap);
	if (rc != 0)
		return rc;

	rc = -DER_NOMEM;
	D_ALLOC(blob->bb_pages, blob->bb_pgs_nr * sizeof(*blob->bb_pages));
	if (!blob->bb_pages)
		goto failed;
	D_ALLOC(blob->bb_mh_nodes, blob->bb_pgs_nr * sizeof(struct ad_maxheap_node));
	if (!blob->bb_mh_nodes)
		goto failed;

	if (blob->bb_fd < 0) { /* Test only */
		/* NB: buffer must align with arena size, because ptr2addr() depends on
		 * this to find address of ad_arena.
		 */
		D_ALIGNED_ALLOC(buf, ARENA_SIZE, blob->bb_pgs_nr << ARENA_SIZE_BITS);
		if (!buf)
			goto failed;

		blob->bb_mmap = buf;
		for (i = 0; i < blob->bb_pgs_nr; i++) {
			blob->bb_pages[i].pa_rpg = buf + (i << ARENA_SIZE_BITS);
			blob->bb_pages[i].pa_cpg = NULL; /* reserved for future use */
		}
	} else {
		D_ASSERT(0); /* XXX remove this while integrating with other components. */
	}

	/* NB: ad_blob_df (superblock) is stored right after header of arena[0],
	 * so it does not require any special code to handle checkpoint of superblock.
	 */
	blob->bb_df = (struct ad_blob_df *)&blob->bb_pages[0].pa_rpg[ARENA_HDR_SIZE];

	/* bitmap for reserving arena */
	D_ALLOC(blob->bb_bmap_rsv, blob_bmap_size(blob));
	if (!blob->bb_bmap_rsv)
		goto failed;

	blob->bb_ars_lru_cap = min(blob->bb_pgs_nr, ARENA_LRU_MAX);
	blob->bb_gps_lru_cap = min(blob->bb_pgs_nr * 256, GROUP_LRU_MAX);

	for (i = 0; i < blob->bb_ars_lru_cap; i++) {
		struct ad_arena *arena;

		arena = arena_alloc(NULL, true, ARENA_GRP_AVG);
		if (!arena)
			goto failed;

		blob->bb_ars_lru_size++;
		d_list_add(&arena->ar_link, &blob->bb_ars_lru);
	}

	for (i = 0; i < blob->bb_gps_lru_cap; i++) {
		struct ad_group *group;

		group = alloc_group(NULL, true);
		if (!group)
			goto failed;

		blob->bb_gps_lru_size++;
		d_list_add(&group->gp_link, &blob->bb_gps_lru);
	}

	return 0;
failed:
	blob_fini(blob);
	return rc;
}

static void
blob_fini(struct ad_blob *blob)
{
	struct ad_arena *arena;
	struct ad_group *group;

	D_DEBUG(DB_TRACE, "Finalizing blob\n");
	D_ASSERT(d_list_empty(&blob->bb_gps_rsv));
	D_ASSERT(d_list_empty(&blob->bb_ars_rsv));

	while ((group = d_list_pop_entry(&blob->bb_gps_lru, struct ad_group, gp_link)))
		group_free(group, true);
	while ((arena = d_list_pop_entry(&blob->bb_ars_lru, struct ad_arena, ar_link)))
		arena_free(arena, true);

	blob->bb_gps_lru_size = 0;
	blob->bb_ars_lru_size = 0;

	d_binheap_destroy_inplace(&blob->bb_arena_free_heap);
	D_FREE(blob->bb_mh_nodes);
	D_FREE(blob->bb_bmap_rsv);
	D_FREE(blob->bb_pages);
	D_FREE(blob->bb_mmap);
}

static uint32_t
arena_free_size(struct ad_arena_df *ad)
{
	int		    grp_idx;
	uint32_t	    free_size = 0;
	struct ad_group_df  *gd;
	uint32_t	    grp_bit_max = (ARENA_SIZE + ARENA_UNIT_SIZE - 1) / ARENA_UNIT_SIZE;

	for (grp_idx = 0; grp_idx < grp_bit_max; grp_idx++) {
		if (!isset64(ad->ad_bmap, grp_idx))
			free_size += (1 << GRP_SIZE_SHIFT);
	}

	for (grp_idx = 0; grp_idx < ARENA_GRP_MAX; grp_idx++) {
		gd = &ad->ad_groups[grp_idx];
		if (gd->gd_addr != 0)
			free_size += gd->gd_unit_free * gd->gd_unit;
	}

	return free_size;
}

#define ARENA_WEIGHT_BITS 14

/* Avoid to change weight of an arena on every alloc/free (reorder arena) */
static inline uint32_t
arena_weight(int free_size)
{
	return (free_size + (1 << ARENA_WEIGHT_BITS) - 1) >> ARENA_WEIGHT_BITS;
}

static int
arena_insert_free_entry(struct ad_blob *blob, uint32_t arena_id, int free_size)
{
	struct ad_maxheap_node	*mh_node;
	int			 rc;

	D_ASSERT(arena_id < blob->bb_pgs_nr);
	mh_node = &blob->bb_mh_nodes[arena_id];

	mh_node->mh_free_size = free_size;
	mh_node->mh_weight = arena_weight(free_size);
	mh_node->mh_arena_id = arena_id;
	rc = d_binheap_insert(&blob->bb_arena_free_heap, &mh_node->mh_node);
	D_ASSERT(rc == 0);

	return rc;
}

static void
arena_remove_free_entry(struct ad_blob *blob, uint32_t arena_id)
{
	struct ad_maxheap_node	*mh_node;

	D_ASSERT(arena_id < blob->bb_pgs_nr);
	mh_node = &blob->bb_mh_nodes[arena_id];
	if (mh_node->mh_in_tree)
		d_binheap_remove(&blob->bb_arena_free_heap, &mh_node->mh_node);
}

static int
blob_load(struct ad_blob *blob)
{
	struct ad_blob_df *bd = blob->bb_df;
	int		   i;
	int		   rc = 0;
	struct ad_arena_df *ad;

	for (i = 0; i < blob->bb_pgs_nr; i++) {
		struct ad_page		*page;
		struct umem_store_iod	 iod;
		d_sg_list_t		 sgl;
		d_iov_t			 iov;

		page = &blob->bb_pages[i];
		ad_iod_set(&iod, blob_addr(blob) + ARENA_SIZE * i, ARENA_SIZE);
		ad_sgl_set(&sgl, &iov, page->pa_rpg, ARENA_SIZE);

		/* XXX: submit multiple pages, otherwise it's too slow */
		rc = blob->bb_store.stor_ops->so_read(&blob->bb_store, &iod, &sgl);
		if (rc) {
			D_ERROR("Failed to load storage contents: %d\n", rc);
			goto out;
		}
		if (isset64(bd->bd_bmap, i)) {
			ad = (struct ad_arena_df *)blob->bb_pages[i].pa_rpg;

			rc = arena_insert_free_entry(blob, i, arena_free_size(ad));
			if (rc) {
				D_ERROR("Failed to insert arena free memory entry: %d\n", rc);
				goto out;
			}
		}
	}

	/* overwrite the old incarnation */
	bd->bd_incarnation = d_timeus_secdiff(0);
	/* NB: @bd points to the first page, its content has been brought in by the above read.*/
	for (i = 0; i < bd->bd_asp_nr; i++)
		blob->bb_arena_last[i] = bd->bd_asp[i].as_last_used;
out:
	return rc;
}

static uint64_t
blob_incarnation(struct ad_blob *blob)
{
	return blob->bb_df->bd_incarnation;
}

static void
blob_set_opened(struct ad_blob *blob)
{
	struct ad_blob_df *bd = blob->bb_df;

	bd->bd_back_ptr	= (unsigned long)blob;
	blob->bb_opened = 1;
	if (blob->bb_dummy) {
		D_ASSERT(!dummy_blob);
		dummy_blob = blob;
	}
}

static void
blob_close(struct ad_blob *blob)
{
	struct ad_arena *arena;
	struct ad_group *group;

	D_ASSERT(blob->bb_opened > 0);
	D_DEBUG(DB_TRACE, "Close blob, openers=%d\n", blob->bb_opened);
	blob->bb_opened--;
	if (blob->bb_opened > 0)
		return;

	D_DEBUG(DB_TRACE, "Evict unpublished groups and arenas\n");
	while ((group = d_list_pop_entry(&blob->bb_gps_rsv, struct ad_group, gp_link))) {
		D_ASSERT(group->gp_unpub);
		group_decref(group);
	}

	while ((arena = d_list_pop_entry(&blob->bb_ars_rsv, struct ad_arena, ar_link))) {
		D_ASSERT(arena->ar_unpub);
		arena_decref(arena);
	}

	D_DEBUG(DB_TRACE, "Unbind groups and arenas in LRU\n");
	d_list_for_each_entry(group, &blob->bb_gps_lru, gp_link)
		group_unbind(group, false);

	d_list_for_each_entry(arena, &blob->bb_ars_lru, ar_link)
		arena_unbind(arena, false);

	if (blob->bb_dummy) {
		D_ASSERT(dummy_blob == blob);
		dummy_blob = NULL;
	}
}


/**
 * API design is a little bit confusing for now, the process is like:
 * - VOS calls umem_pool_create()->blob_prep_create()
 * - VOS initialize BIO context
 * - VOS calls blob_post_create() to read blob (needs BIO context)
 */
int
ad_blob_prep_create(char *path, daos_size_t size, struct ad_blob_handle *bh)
{
	struct ad_blob	*blob;
	bool		 is_dummy = false;

	if (!strcmp(path, DUMMY_BLOB)) {
		if (dummy_blob)
			return -DER_EXIST;
		is_dummy = true;
	}
	D_ASSERT(is_dummy); /* XXX the only thing can be supported now */

	D_ALLOC_PTR(blob);
	if (!blob)
		return -DER_NOMEM;

	blob->bb_store.stor_size = size;
	blob->bb_fd  = -1; /* XXX, create file, falloc */
	blob->bb_ref = 1;
	blob->bb_dummy = is_dummy;

	bh->bh_blob = blob;
	return 0;
}

/**
 * Format superblock of the blob, create the first arena, write these metadata to storage.
 * NB: superblock is stored in the first arena.
 */
int
ad_blob_post_create(struct ad_blob_handle bh)
{
	struct ad_blob		*blob = bh.bh_blob;
	struct umem_store	*store = &blob->bb_store;
	struct ad_blob_df	*bd;
	struct ad_arena		*arena;
	struct umem_store_iod	 iod;
	d_sg_list_t		 sgl;
	d_iov_t			 iov;
	int			 rc;

	/* NB: store::stor_size can be changed by upper level stack */
	blob->bb_pgs_nr = ((blob_size(blob) + ARENA_SIZE_MASK) >> ARENA_SIZE_BITS);

	rc = blob_init(blob);
	if (rc)
		return rc;

	bd = blob->bb_df;
	bd->bd_magic		= BLOB_MAGIC;
	bd->bd_version		= AD_MEM_VERSION;
	bd->bd_addr		= store->stor_addr;
	bd->bd_size		= blob_size(blob);
	bd->bd_arena_size	= ARENA_SIZE;
	bd->bd_incarnation	= d_timeus_secdiff(0);

	/* register the default arena */
	rc = blob_register_arena(blob, 0, grp_specs_def, ARRAY_SIZE(grp_specs_def), NULL);
	D_ASSERT(rc == 0); /* no reason to fail */

	/* create arena 0 (ad_blob_df is stored in the first 32K of it) */
	rc = arena_reserve(blob, 0, NULL, &arena);
	D_ASSERT(rc == 0);
	D_ASSERT(arena->ar_df);
	D_ASSERT(arena->ar_df->ad_id == 0);

	/* NB: no transaction, write arena[0] and super block to storage straight away */
	rc = arena_tx_publish(arena, NULL);
	if (rc)
		goto failed;

	arena->ar_unpub = 0;

	blob->bb_arena_last[0]	= bd->bd_asp[0].as_last_used;
	/* already published arena[0], clear the reserved bit */
	clrbit64(blob->bb_bmap_rsv, arena->ar_df->ad_id);

	ad_iod_set(&iod, blob_ptr2addr(blob, arena->ar_df), ARENA_HDR_SIZE + BLOB_HDR_SIZE);
	ad_sgl_set(&sgl, &iov, arena->ar_df, ARENA_HDR_SIZE + BLOB_HDR_SIZE);

	D_ASSERT(store->stor_ops);
	rc = store->stor_ops->so_write(store, &iod, &sgl);
	if (rc) {
		D_ERROR("Failed to write ad_mem superblock\n");
		goto failed;
	}
	arena_decref(arena);
	D_DEBUG(DB_TRACE, "Ad-hoc memory blob created\n");
	blob_set_opened(blob);
	return 0;
failed:
	if (arena)
		arena_decref(arena);
	/* caller should call ad_blob_close() to actually free the handle */
	return rc;
}

int
ad_blob_prep_open(char *path, struct ad_blob_handle *bh)
{
	struct ad_blob	*blob;
	bool		 is_dummy = false;

	if (!strcmp(path, DUMMY_BLOB))
		is_dummy = true;

	if (!is_dummy) {
		/* XXX dummy is the only thing can be supported now */
		D_ASSERT(0);
	} else {
		if (dummy_blob) {
			blob = dummy_blob;
			D_DEBUG(DB_TRACE, "found dummy blob, refcount=%d\n", blob->bb_ref);
			blob_addref(blob);
		} else {
			D_ALLOC_PTR(blob);
			if (!blob)
				return -DER_NOMEM;

			blob->bb_fd = -1;
			blob->bb_ref = 1;
			blob->bb_dummy = true;
		}
	}
	bh->bh_blob = blob;
	return 0;
}

int
ad_blob_post_open(struct ad_blob_handle bh)
{
	struct ad_blob		*blob  = bh.bh_blob;
	struct umem_store	*store = &blob->bb_store;
	struct ad_blob_df	*bd;
	struct umem_store_iod	 iod;
	d_sg_list_t		 sgl;
	d_iov_t			 iov;
	int			 rc;

	if (blob->bb_opened) {
		blob->bb_opened++;
		return 0;
	}

	D_ALLOC_PTR(bd);
	if (!bd)
		return -DER_NOMEM;

	/* blob header is stored right after header of arena[0] */
	ad_iod_set(&iod, blob->bb_store.stor_addr + ARENA_HDR_SIZE, sizeof(*bd));
	ad_sgl_set(&sgl, &iov, bd, sizeof(*bd));

	/* read super block to temporary buffer */
	D_ASSERT(store->stor_ops);
	rc = store->stor_ops->so_read(store, &iod, &sgl);
	if (rc) {
		D_ERROR("Failed to read superblock of ad_mem\n");
		goto failed;
	}

	if (bd->bd_magic != BLOB_MAGIC || bd->bd_version == 0) {
		D_ERROR("Invalid superblock: magic=%x, version=%d\n",
			bd->bd_magic, bd->bd_version);
		goto failed;
	}

	store->stor_size = bd->bd_size;
	store->stor_addr = bd->bd_addr;
	blob->bb_pgs_nr = ((blob_size(blob) + ARENA_SIZE_MASK) >> ARENA_SIZE_BITS);

	rc = blob_init(blob);
	if (rc)
		goto failed;

	rc = blob_load(blob);
	if (rc)
		goto failed;

	blob_set_opened(blob);
	D_FREE(bd); /* free the temporary buffer */
	return 0;
failed:
	blob_decref(blob);
	D_FREE(bd);
	return rc;
}

int
ad_blob_close(struct ad_blob_handle bh)
{
	struct ad_blob *blob = bh.bh_blob;

	blob_close(blob);
	blob_decref(blob);
	return 0;
}

int
ad_blob_destroy(struct ad_blob_handle bh)
{
	struct ad_blob *blob = bh.bh_blob;

	if (blob->bb_opened > 1) {
		D_ERROR("blob is still in use, opened=%d\n", blob->bb_opened);
		return -DER_BUSY;
	}

	/* TODO: remove the file */
	blob_close(blob);
	blob_decref(blob);
	return 0;
}

struct umem_store *
ad_blob_hdl2store(struct ad_blob_handle bh)
{
	D_ASSERT(bh.bh_blob);
	return &bh.bh_blob->bb_store;
}

void *
blob_addr2ptr(struct ad_blob *blob, daos_off_t addr)
{
	struct ad_page	*pg;
	daos_off_t	 off;

	off  = addr & ARENA_SIZE_MASK;
	addr = addr - blob_addr(blob);
	pg   = &blob->bb_pages[addr >> ARENA_SIZE_BITS];
	return &pg->pa_rpg[off];
}

/** convert storage address to mapped memory address */
void *
ad_addr2ptr(struct ad_blob_handle bh, daos_off_t addr)
{
	return blob_addr2ptr(bh.bh_blob, addr);
}

daos_off_t
blob_ptr2addr(struct ad_blob *blob, void *ptr)
{
	struct ad_arena_df *ad;
	daos_off_t	    off;

	off = (unsigned long)ptr & ARENA_SIZE_MASK;
	ad = (struct ad_arena_df *)((unsigned long)ptr & ~ARENA_SIZE_MASK);
	return ad->ad_addr + off;
}

/** convert mapped memory address to storage address */
daos_off_t
ad_ptr2addr(struct ad_blob_handle bh, void *ptr)
{
	return blob_ptr2addr(bh.bh_blob, ptr);
}

static int
group_size_cmp(const void *p1, const void *p2)
{
	const struct ad_group_df *gd1 = p1;
	const struct ad_group_df *gd2 = p2;
	int			  w1;
	int			  w2;

	if (gd1->gd_unit < gd2->gd_unit)
		return -1;
	if (gd1->gd_unit > gd2->gd_unit)
		return 1;

	w1 = group_weight(gd1);
	w2 = group_weight(gd2);
	if (w1 < w2)
		return -1;
	if (w1 > w2)
		return 1;

	/* use address to identify a specified group */
	if (gd1->gd_addr < gd2->gd_addr)
		return -1;
	if (gd1->gd_addr > gd2->gd_addr)
		return 1;
	return 0;
}

static int
group_addr_cmp(const void *p1, const void *p2)
{
	const struct ad_group_df *gd1 = p1;
	const struct ad_group_df *gd2 = p2;

	if (gd1->gd_addr < gd2->gd_addr)
		return -1;
	if (gd1->gd_addr > gd2->gd_addr)
		return 1;

	/* two groups have the same address? */
	D_ASSERTF(0, "Two groups cannot have the same address\n");
	return 0;
}

static int
arena_find(struct ad_blob *blob, uint32_t *arena_id, struct ad_arena_df **ad_p)
{
	struct ad_blob_df  *bd = blob->bb_df;
	bool		    reserving = false;
	int		    id = *arena_id;

	if (id == AD_ARENA_ANY) {
		int	bits = 1;

		id = find_bits(bd->bd_bmap, blob->bb_bmap_rsv, blob_bmap_size(blob), 1, &bits);
		if (id < 0) {
			D_ERROR("Blob is full, cannot create more arena\n");
			return -DER_NOSPACE;
		}
		reserving = true;
	}

	if (((id + 1) << ARENA_SIZE_BITS) > blob_size(blob))
		return reserving ? -DER_NOSPACE : -DER_INVAL;

	if (!reserving &&
	    !isset64(bd->bd_bmap, id) &&
	    !isset64(blob->bb_bmap_rsv, id))
		return -DER_NONEXIST;

	/* Arena is the header of each page */
	*ad_p = (struct ad_arena_df *)blob->bb_pages[id].pa_rpg;
	if (reserving)
		*arena_id = id;

	return 0;
}

static inline struct ad_maxheap_node *
arena2heap_node(struct ad_arena *arena)
{
	D_ASSERT(arena->ar_blob);
	D_ASSERT(arena->ar_df);

	return &arena->ar_blob->bb_mh_nodes[arena->ar_df->ad_id];
}

static int
arena_load(struct ad_blob *blob, uint32_t arena_id, struct ad_arena **arena_p)
{
	struct ad_arena		*arena = NULL;
	struct ad_arena_df	*ad = NULL;
	struct ad_group_df	*gd;
	int			 i;
	int			 rc;
	struct ad_maxheap_node	*node;
	int32_t			 grp_nr = 0;

	D_ASSERT(arena_id != AD_ARENA_ANY);
	rc = arena_find(blob, &arena_id, &ad);
	if (rc) {
		D_ERROR("No available arena, id=%d\n", arena_id);
		return rc;
	}

	if (ad->ad_magic != ARENA_MAGIC) {
		D_ERROR("Invalid arena magic: %x/%x\n", ad->ad_magic, ARENA_MAGIC);
		return -DER_PROTO;
	}

	if (ad->ad_incarnation != blob_incarnation(blob)) {
		ad->ad_incarnation = blob_incarnation(blob);
		ad->ad_back_ptr = 0; /* clear stale back-pointer to DRAM */
	}

	if (ad->ad_back_ptr) {
		arena = arena_df2ptr(ad);
		D_ASSERT(arena->ar_df == ad);
		arena->ar_ref++;
		if (arena->ar_ref == 1) { /* remove from LRU */
			d_list_del_init(&arena->ar_link);
			blob->bb_ars_lru_size--;
		}
		goto out;
	}
	/* no cached arena, allocate it now */

	arena = arena_alloc(blob, false, ARENA_GRP_AVG);
	if (!arena)
		return -DER_NOMEM;

	/* NB: stale pointer can be detected by incarnation. */
	ad->ad_back_ptr = (unsigned long)arena;

	arena->ar_ref	 = 1; /* for caller */
	arena->ar_df	 = ad;
	arena->ar_type	 = ad->ad_type;

	for (i = 0; i < ARENA_GRP_MAX; i++) {
		gd = &ad->ad_groups[i];
		if (gd->gd_addr == 0)
			continue;
		if (gd->gd_incarnation != blob_incarnation(blob)) {
			/* reset stale backpointer */
			gd->gd_incarnation = blob_incarnation(blob);
			gd->gd_back_ptr = 0;
		}

		if (grp_nr == ARENA_GRP_AVG) {
			rc = arena_init_sorters(arena, ARENA_GRP_MAX);
			if (rc)
				goto failed;
		}
		arena->ar_size_sorter[grp_nr] =
		arena->ar_addr_sorter[grp_nr] = gd;
		grp_nr++;
	}
	arena->ar_grp_nr = grp_nr;

	if (arena->ar_grp_nr > 0) {
		qsort(arena->ar_size_sorter, arena->ar_grp_nr, sizeof(gd), group_size_cmp);
		qsort(arena->ar_addr_sorter, arena->ar_grp_nr, sizeof(gd), group_addr_cmp);
	}
	node = arena2heap_node(arena);
	if (!node->mh_in_tree) {
		node->mh_free_size = arena_free_size(ad);
		node->mh_weight = arena_weight(node->mh_free_size);
	}
out:
	if (arena_p)
		*arena_p = arena;
	return 0;
failed:
	arena_free(arena, false);
	return -DER_NOMEM;
}

/** Reserve a new arena for the specified type */
static int
arena_reserve(struct ad_blob *blob, unsigned int type, struct umem_store *store_extern,
	      struct ad_arena **arena_p)
{
	struct ad_blob_df	*bd = blob->bb_df;
	struct ad_arena_df	*ad;
	struct ad_arena		*arena;
	uint32_t		 id;
	int			 rc;

	D_ASSERT(store_extern == NULL); /* XXX: not supported yet */
	if (store_extern && store_extern->stor_addr == 0) {
		/* cannot register an arena starts from 0 address, because 0 is reserved
		 * for failed allocation.
		 */
		return -DER_INVAL;
	}

	if (type >= bd->bd_asp_nr) {
		D_ERROR("Invalid arena type=%d\n", type);
		return -DER_INVAL;
	}

	id = AD_ARENA_ANY;
	rc = arena_find(blob, &id, &ad);
	if (rc) {
		D_ASSERT(rc == -DER_NOSPACE);
		return rc;
	}
	D_ASSERT(id >= 0);

	D_DEBUG(DB_TRACE, "Reserved a new arena: type=%d, id=%d\n", type, id);
	blob->bb_arena_last[type] = id;
	D_ASSERT(ad->ad_magic != ARENA_MAGIC);

	/* This is new memory, no need to undo, arena_tx_publish() will add them to WAL */
	memset(ad, 0, sizeof(*ad));
	ad->ad_id	    = id;
	ad->ad_type	    = type;
	ad->ad_magic	    = ARENA_MAGIC;
	ad->ad_size	    = ARENA_SIZE;
	ad->ad_unit	    = ARENA_UNIT_SIZE;
	ad->ad_addr	    = blob_addr(blob) + id * ARENA_SIZE;
	ad->ad_incarnation  = blob_incarnation(blob);

	/* the first two bits (representing 64K) is reserved for arena header */
	setbits64(ad->ad_bmap, 0, 2);

	D_CASSERT(ARENA_UNIT_SIZE == BLOB_HDR_SIZE);
	if (id == 0) { /* the first arena */
		/* Blob header(superblock) is stored in arena zero, it consumes 64K.
		 * NB: the first arena is written straightway, no WAL
		 */
		setbit64(ad->ad_bmap, 2);
	}
	/* DRAM only operation, mark the arena as reserved */
	D_ASSERT(!isset64(blob->bb_bmap_rsv, id));
	setbit64(blob->bb_bmap_rsv, id);

	rc = arena_load(blob, id, &arena);
	D_ASSERT(rc == 0);

	arena->ar_unpub = 1;
	*arena_p = arena;
	return 0;
}

/** Publish a reserved arena */
static int
arena_tx_publish(struct ad_arena *arena, struct ad_tx *tx)
{
	struct ad_blob		*blob	= arena->ar_blob;
	struct ad_blob_df	*bd	= blob->bb_df;
	struct ad_arena_df	*ad	= arena->ar_df;
	struct ad_arena_spec	*spec	= &bd->bd_asp[ad->ad_type];
	int			 rc;

	D_DEBUG(DB_TRACE, "publishing arena=%d\n", ad->ad_id);
	rc = ad_tx_setbits(tx, bd->bd_bmap, ad->ad_id, 1);
	if (rc)
		return rc;

	rc = ad_tx_assign(tx, &spec->as_last_used, sizeof(spec->as_last_used), ad->ad_id);
	if (rc)
		return rc;

	D_DEBUG(DB_TRACE, "Published arena type = %u, ID = %u\n", ad->ad_type, spec->as_last_used);

	rc = ad_tx_set(tx, ad, 0, sizeof(*ad), AD_TX_REDO | AD_TX_LOG_ONLY);
	if (rc)
		return rc;

	rc = ad_tx_snap(tx, ad, offsetof(struct ad_arena_df, ad_bmap[0]), AD_TX_REDO);
	if (rc)
		return rc;

	return 0;
}

/** Convert size to group specification. */
struct ad_group_spec *
arena_size2gsp(struct ad_arena *arena, daos_size_t size, int *spec_id)
{
	struct ad_blob	     *bb  = arena->ar_blob;
	struct ad_arena_df   *ad  = arena->ar_df;
	struct ad_arena_spec *asp = &bb->bb_df->bd_asp[ad->ad_type];
	struct ad_group_spec *gsp = NULL;
	int		      start;
	int		      end;
	int		      cur;
	int		      len;
	int		      rc;

	len = asp->as_specs_nr;
	D_ASSERT(len > 0);

	/* check if there is a customized group for the size */
	for (start = cur = 0, end = len - 1; start <= end; ) {
		cur = (start + end) / 2;
		gsp = &asp->as_specs[cur];
		if (gsp->gs_unit < size)
			rc = -1;
		else if (gsp->gs_unit > size)
			rc = 1;
		else
			break;

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}
	D_ASSERT(gsp);

	if (gsp->gs_unit < size) {
		if (cur < len - 1) {
			cur += 1;
			gsp = &asp->as_specs[cur];
		} else {
			D_ERROR("size is too large: %d\n", (int)size);
			gsp = NULL;
		}
	}

	if (gsp) { /* matched */
		D_DEBUG(DB_TRACE, "Found spec: spec_unit=%d, size=%d\n",
			(int)gsp->gs_unit, (int)size);
		if (spec_id)
			*spec_id = cur;
	}
	return gsp;
}

/** number of available units in a group, reserved units is counted as occupied */
static inline int
group_unit_avail(const struct ad_group_df *gd)
{
	struct ad_group	*grp;
	int		 units;

	units = gd->gd_unit_free;
	grp = group_df2ptr(gd);
	if (grp) {
		D_ASSERTF(units >= grp->gp_unit_rsv, "grp(%p), gd(%p), reserved=%d, free=%d\n",
			  grp, gd, grp->gp_unit_rsv, units);
		units -= grp->gp_unit_rsv;
	}
	return units;
}

#define GRP_WEIGHT_BITS	3

/* Avoid to change weight of a group on every alloc/free (reorder groups) */
static inline int
group_weight(const struct ad_group_df *gd)
{
	int	units;

	units = group_unit_avail(gd);
	units = (units + (1 << GRP_WEIGHT_BITS) - 1) >> GRP_WEIGHT_BITS;
	return units;
}

static int
group_load(struct ad_group_df *gd, struct ad_arena *arena, struct ad_group **group_p)
{
	struct ad_group	   *grp;
	struct ad_arena_df *ad = arena->ar_df;

	if (gd->gd_back_ptr) {
		if (gd->gd_incarnation == blob_incarnation(arena->ar_blob)) {
			grp = group_df2ptr(gd);
			grp->gp_ref++;
			if (grp->gp_ref == 1) { /* remove from LRU */
				d_list_del_init(&grp->gp_link);
				arena->ar_blob->bb_gps_lru_size--;
			}
			goto out;
		}
		gd->gd_back_ptr = 0;
	}

	grp = alloc_group(arena, false);
	if (!grp)
		return -DER_NOMEM;

	gd->gd_incarnation = blob_incarnation(arena->ar_blob);
	gd->gd_back_ptr = (unsigned long)grp;
	grp->gp_ref = 1;
	grp->gp_df = gd;

	grp->gp_bit_at = (gd->gd_addr - ad->ad_addr) >> GRP_SIZE_SHIFT;
	grp->gp_bit_nr = ((gd->gd_unit_nr * gd->gd_unit) + gd->gd_unit - 1) >> GRP_SIZE_SHIFT;
out:
	*group_p = grp;
	return 0;
}

/** Find a group with free space for the requested allocate size @size */
static int
arena_find_grp(struct ad_arena *arena, daos_size_t size, int *pos, struct ad_group **grp_p)
{
	struct ad_group_df    *gd = NULL;
	struct ad_group_spec  *gsp;
	struct ad_group	      *grp;
	int		       start;
	int		       end;
	int		       len;
	int		       cur;
	int		       rc;

	len = arena->ar_grp_nr;
	if (len == 0) /* no group, non-fatal error */
		return -DER_ENOENT;

	gsp = arena_size2gsp(arena, size, NULL);
	if (!gsp) {
		D_ERROR("Cannot find matched group specification for size=%d\n", (int)size);
		return -DER_INVAL;
	}

	if (gsp->gs_unit != size) { /* no customized size, use the generic one */
		D_ASSERT(size < gsp->gs_unit);
		size = gsp->gs_unit;
	}

	/* binary search to find a group */
	for (start = cur = 0, end = len - 1; start <= end; ) {
		cur = (start + end) / 2;
		gd = arena->ar_size_sorter[cur];
		if (gd->gd_unit == size) {
			int weight = group_weight(gd);

			/* always try to use the group with the least free units */
			if (weight == 1)
				goto found;

			if (weight == 0)
				rc = -1;
			else
				rc = 1;

		} else if (gd->gd_unit < size) {
			rc = -1;
		} else {
			rc = 1;
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}
	D_DEBUG(DB_TRACE, "matched grp=%p, unit=%d, size=%d\n", gd, gd->gd_unit, (int)size);

	while (gd->gd_unit <= size) {
		if (gd->gd_unit == size && group_weight(gd) > 0)
			goto found;

		if (++cur == len) /* no more group */
			break;

		gd = arena->ar_size_sorter[cur];
	}
	return -DER_NOSPACE;
found:
	rc = group_load(gd, arena, &grp);
	if (rc)
		return -DER_NOMEM;

	*grp_p = grp;
	*pos = cur;
	return 0;
}

/** Locate the associated group for the provided address @addr */
static struct ad_group *
arena_addr2grp(struct ad_arena *arena, daos_off_t addr)
{
	struct ad_group_df *gd = NULL;
	struct ad_group	   *grp;
	int		    start;
	int		    end;
	int		    cur;
	int		    rc;
	bool		    found = false;

	/* binary search, should be quick */
	for (start = cur = 0, end = arena->ar_grp_nr - 1; start <= end; ) {
		daos_size_t	size;

		cur = (start + end) / 2;
		gd = arena->ar_addr_sorter[cur];

		size = gd->gd_unit_nr * gd->gd_unit;
		if (gd->gd_addr <= addr && gd->gd_addr + size > addr) {
			found = true;
			break;
		}

		if (gd->gd_addr + size <= addr) {
			rc = -1;
		} else {
			D_ASSERT(gd->gd_addr > addr);
			rc = 1;
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}
	D_ASSERT(found);
	rc = group_load(gd, arena, &grp);
	if (rc)
		return NULL;

	/* this function is called from tx_free(), only published group can actually
	 * allocate and free space.
	 */
	D_ASSERT(!grp->gp_unpub);
	return grp;
}

/* Locate group position in the size-sorter */
static int
arena_locate_grp(struct ad_arena *arena, struct ad_group *group)
{
	struct ad_group_df  *gd = group->gp_df;
	struct ad_group_df  *tmp = NULL;
	int		     weight;
	int		     start;
	int		     end;
	int		     cur;
	int		     rc;

	weight = group_weight(gd);
	/* binary search by @group->gd_unit to find @group */
	for (start = cur = 0, end = arena->ar_grp_nr - 1; start <= end; ) {
		cur = (start + end) / 2;
		tmp = arena->ar_size_sorter[cur];
		/* the same unit size */
		if (tmp->gd_unit == gd->gd_unit) {
			int	w;

			if (tmp == gd) /* found */
				return cur;

			w = group_weight(tmp);
			if (w < weight)
				rc = -1;
			else if (w > weight)
				rc = 1;
			else /* group address */
				rc = tmp->gd_addr < gd->gd_addr ? -1 : 1;

		} else if (tmp->gd_unit < gd->gd_unit) {
			rc = -1;
		} else {
			rc = 1;
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}
	D_ASSERT(0);
	return -1;
}

/* Adjust group position in the size-sorter after alloc/reserve/free (for binary search) */
static void
arena_reorder_grp(struct ad_arena *arena, struct ad_group *group, int pos, bool is_alloc)
{
	struct ad_group_df **sorter = arena->ar_size_sorter;
	struct ad_group_df  *gd = group->gp_df;
	struct ad_group_df  *tmp = NULL;
	int		     g_weight;
	int		     t_weight;
	int		     i;

	g_weight = group_weight(gd);
	/* swap pointers and order groups after alloc/free */
	if (is_alloc) { /* shift left */
		for (i = pos; i > 0;) {
			tmp = sorter[--i];
			if (tmp->gd_unit != gd->gd_unit) {
				D_ASSERT(tmp->gd_unit < gd->gd_unit);
				break;
			}

			t_weight = group_weight(tmp);
			if (t_weight < g_weight ||
			    (t_weight == g_weight && tmp->gd_addr < gd->gd_addr))
				break;

			sorter[pos] = tmp;
			sorter[i] = gd;
			pos = i;
		}
	} else { /* shift right */
		for (i = pos; i < arena->ar_grp_nr - 1;) {
			tmp = sorter[++i];
			if (tmp->gd_unit != gd->gd_unit) {
				D_ASSERT(tmp->gd_unit > gd->gd_unit);
				break;
			}

			t_weight = group_weight(tmp);
			if (t_weight > g_weight ||
			    (t_weight == g_weight && tmp->gd_addr > gd->gd_addr))
				break;

			sorter[pos] = tmp;
			sorter[i] = gd;
			pos = i;
		}
	}
}

static void
arena_debug_sorter(struct ad_arena *arena)
{
#if AD_MEM_DEBUG
	struct ad_group_df *gd;
	struct ad_group    *grp;
	int		    cur;

	D_DEBUG(DB_TRACE, "arena[%d]=%p, groups=%d\n", arena2id(arena), arena, arena->ar_grp_nr);
	D_DEBUG(DB_TRACE, "size sorted groups:\n");
	for (cur = 0; cur < arena->ar_grp_nr; cur++) {
		gd = arena->ar_size_sorter[cur];
		grp = group_df2ptr(gd);
		D_DEBUG(DB_TRACE,
			"group[%d]=%p, size=%d, addr=%lx, weight=%d, avail=%d, pub=%d\n",
			cur, gd, gd->gd_unit, (unsigned long)gd->gd_addr,
			group_weight(gd), group_unit_avail(gd), !grp->gp_unpub);
	}

	D_DEBUG(DB_TRACE, "address sorted groups:\n");
	for (cur = 0; cur < arena->ar_grp_nr; cur++) {
		gd = arena->ar_addr_sorter[cur];
		grp = group_df2ptr(gd);
		D_DEBUG(DB_TRACE,
			"group[%d]=%p, size=%d, addr=%lx, weight=%d, avail=%d, pub=%d\n",
			cur, gd, gd->gd_unit, (unsigned long)gd->gd_addr,
			group_weight(gd), group_unit_avail(gd), !grp->gp_unpub);
	}
#endif
}

/**
 * add a new group to arena, this function inserts the new group into two arrays to support
 * binary search:
 * - the first array is for searching by size and available units (alloc)
 * - the second array is for searching by address (free)
 */
static int
arena_add_grp(struct ad_arena *arena, struct ad_group *grp, int *pos)
{
	struct ad_group_df **size_sorter;
	struct ad_group_df **addr_sorter;
	struct ad_group_df  *gd   = grp->gp_df;
	struct ad_group_df  *tmp  = NULL;
	int		     weight;
	int		     start;
	int		     end;
	int		     cur;
	int		     len;
	int		     rc;

	/* no WAL, in DRAM */
	len = arena->ar_grp_nr++;
	D_ASSERT(arena->ar_grp_nr <= ARENA_GRP_MAX);
	if (len == 0) {
		arena->ar_addr_sorter[0] = gd;
		arena->ar_size_sorter[0] = gd;
		if (pos)
			*pos = 0;
		return 0;
	}

	if (arena->ar_grp_nr > arena->ar_sorter_sz) {
		/* unlikely, unless caller always allocates 64 bytes */
		D_ASSERTF(arena->ar_sorter_sz == ARENA_GRP_AVG, "%d\n",
			  arena->ar_sorter_sz);
		rc = arena_init_sorters(arena, ARENA_GRP_MAX);
		if (rc)
			return rc;
	}
	size_sorter = arena->ar_size_sorter;
	addr_sorter = arena->ar_addr_sorter;

	D_DEBUG(DB_TRACE, "Adding group to address sorter of arena=%d\n", arena2id(arena));
	/* step-1: add the group the address sorter */
	for (start = cur = 0, end = len - 1; start <= end; ) {
		cur = (start + end) / 2;
		tmp = addr_sorter[cur];
		if (tmp->gd_addr < gd->gd_addr) {
			rc = -1;
		} else if (tmp->gd_addr > gd->gd_addr) {
			rc = 1;
		} else {
			arena_debug_sorter(arena);
			D_ASSERT(0);
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}
	if (tmp->gd_addr < gd->gd_addr)
		cur += 1;

	if (cur < len) {
		memmove(&addr_sorter[cur + 1], &addr_sorter[cur],
			(len - cur) * sizeof(addr_sorter[0]));
	}
	addr_sorter[cur] = gd;

	/* step-2: add the group the size sorter */
	D_DEBUG(DB_TRACE, "Adding group to size sorter of arena=%d\n", arena2id(arena));
	weight = group_weight(gd);
	for (start = cur = 0, end = len - 1; start <= end; ) {
		cur = (start + end) / 2;
		tmp = size_sorter[cur];
		if (tmp->gd_unit == gd->gd_unit) {
			int	w = group_weight(tmp);

			if (w < weight)
				rc = -1;
			else if (w > weight)
				rc = 1;
			else
				rc = (tmp->gd_addr < gd->gd_addr) ? -1 : 1;

		} else if (tmp->gd_unit < gd->gd_unit) {
			rc = -1;
		} else {
			rc = 1;
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}

	if (tmp->gd_unit < gd->gd_unit ||
	    (tmp->gd_unit == gd->gd_unit && tmp->gd_addr < gd->gd_addr))
		cur += 1;

	if (cur < len) {
		memmove(&size_sorter[cur + 1], &size_sorter[cur],
			(len - cur) * sizeof(size_sorter[0]));
	}

	if (pos)
		*pos = cur;
	size_sorter[cur] = gd;
	arena_debug_sorter(arena);
	return 0;
}

/** Find requested number of unused bits (neither set it @used or @reserved */
static int
find_bits(uint64_t *used, uint64_t *reserved, int bmap_sz, int bits_min, int *bits)
{
	int	nr_saved;
	int	at_saved;
	int	nr;
	int	at;
	int	i;
	int	j;

	nr = nr_saved = 0;
	at = at_saved = -1;

	for (i = 0; i < bmap_sz; i++) {
		uint64_t free_bits = ~used[i];

		if (reserved)
			free_bits &= ~reserved[i];

		if (free_bits == 0) { /* no space in the current int64 */
			if (nr > nr_saved) {
				nr_saved = nr;
				at_saved = at;
			}
			nr = 0;
			at = -1;
			continue;
		}

		j = ffsll(free_bits);
		D_ASSERT(j > 0);
		if (at >= 0 && j == 1) {
			D_ASSERT(nr > 0);
			nr++;
		} else {
			at = i * 64 + j - 1;
			nr = 1;
		}

		for (; j < 64; j++) {
			if (nr == *bits) /* done */
				goto out;

			if (isset64(&free_bits, j)) {
				nr++;
				continue;
			}

			if (nr > nr_saved) {
				nr_saved = nr;
				at_saved = at;
			}
			nr = 0;
			at = -1;
			if ((free_bits >> j) == 0)
				break;
		}
		if (nr == *bits)
			goto out;
	}
 out:
	if (nr == *bits || nr > nr_saved) {
		nr_saved = nr;
		at_saved = at;
	}

	if (nr_saved >= bits_min)
		*bits = nr_saved;
	else
		at_saved = -1;

	return at_saved;
}

/** reserve a new group within @arena */
static int
arena_reserve_grp(struct ad_arena *arena, daos_size_t size, int *pos,
		  struct ad_group **grp_p)
{
	struct ad_blob		*blob = arena->ar_blob;
	struct ad_arena_df	*ad = arena->ar_df;
	struct ad_group_spec	*gsp;
	struct ad_group_df	*gd;
	struct ad_group		*grp;
	int			 bits;
	int			 bits_min;
	int			 bit_at;
	int			 grp_idx;

	gsp = arena_size2gsp(arena, size, NULL);
	if (!gsp) {
		D_ERROR("No matched group spec for size=%d\n", (int)size);
		return -DER_INVAL;
	}

	if (arena->ar_grp_nr == ARENA_GRP_MAX) {
		/* Too many small groups (e.g., 64 bytes), cannot store more metadata */
		D_DEBUG(DB_TRACE, "Arena %d has too many groups\n", arena2id(arena));
		return -DER_NOSPACE;
	}

	bits = ((gsp->gs_unit * gsp->gs_count) + GRP_SIZE_MASK) >> GRP_SIZE_SHIFT;
	D_ASSERT(bits >= 1);

	/* at least 8 units within a group */
	bits_min = (gsp->gs_unit * 8) >> GRP_SIZE_SHIFT;
	if (bits_min == 0)
		bits_min = 1;
	if (bits_min > bits)
		bits_min = bits;

	bit_at = find_bits(ad->ad_bmap, arena->ar_space_rsv, ARENA_GRP_BMSZ, bits_min, &bits);
	if (bit_at < 0)
		return -DER_NOSPACE;

	D_ASSERT(bits >= bits_min);
	grp = alloc_group(arena, false);
	if (!grp)
		return -DER_NOMEM;

	/* find an unused one */
	for (grp_idx = 0; grp_idx < ARENA_GRP_MAX; grp_idx++) {
		gd = &ad->ad_groups[grp_idx];
		if (gd->gd_addr)
			continue;
		if (!isset64(arena->ar_gpid_rsv, grp_idx))
			break;
	}
	/* run out of ad groups */
	if (grp_idx == ARENA_GRP_MAX)
		return -DER_NOSPACE;

	gd = &ad->ad_groups[grp_idx];
	gd->gd_addr	   = ad->ad_addr + (bit_at << GRP_SIZE_SHIFT);
	D_ASSERT(gd->gd_addr >= blob_addr(blob) + (ad->ad_id << ARENA_SIZE_BITS));
	D_ASSERT(gd->gd_addr < blob_addr(blob) + ((ad->ad_id + 1) << ARENA_SIZE_BITS));
	gd->gd_unit	   = gsp->gs_unit;
	gd->gd_unit_nr	   = (bits << GRP_SIZE_SHIFT) / gd->gd_unit;
	gd->gd_unit_free   = gd->gd_unit_nr;
	gd->gd_back_ptr	   = (unsigned long)grp;
	gd->gd_incarnation = blob_incarnation(blob);

	grp->gp_unpub	= 1;
	grp->gp_ref	= 1;
	grp->gp_df	= gd;
	grp->gp_bit_at	= bit_at;
	grp->gp_bit_nr	= bits;

	D_DEBUG(DB_TRACE, "Arena=%d reserved a new group (bit_at=%d, bits=%d, size=%d)\n",
		arena2id(arena), bit_at, bits, (int)size);

	setbits64(arena->ar_space_rsv, bit_at, bits);
	arena_add_grp(arena, grp, pos);
	if (grp_p)
		*grp_p = grp;

	return 0;
}

/** publish a reserved group */
static int
group_tx_publish(struct ad_group *group, struct ad_tx *tx)
{
	struct ad_arena		*arena = group->gp_arena;
	struct ad_arena_df	*ad = arena->ar_df;
	struct ad_group_df	*gd = group->gp_df;
	int			 bit_at;
	int			 bit_nr;
	int			 rc;

	bit_at = (gd->gd_addr - ad->ad_addr) >> GRP_SIZE_SHIFT;
	bit_nr = ((gd->gd_unit_nr * gd->gd_unit) + gd->gd_unit - 1) >> GRP_SIZE_SHIFT;
	D_DEBUG(DB_TRACE, "publishing group=%p, bit_at=%d, bits_nr=%d\n", group, bit_at, bit_nr);

	rc = ad_tx_setbits(tx, ad->ad_bmap, bit_at, bit_nr);
	if (rc)
		return rc;

	rc = ad_tx_set(tx, gd, 0, sizeof(*gd), AD_TX_REDO | AD_TX_LOG_ONLY);
	if (rc)
		return rc;

	rc = ad_tx_snap(tx, gd, offsetof(struct ad_group_df, gd_bmap[0]), AD_TX_REDO);
	if (rc)
		return rc;

	return 0;
}

/** reserve space within a group, the reservation actions are returned to @act */
static daos_off_t
group_reserve_addr(struct ad_group *grp, struct ad_reserv_act *act)
{
	struct ad_group_df *gd = grp->gp_df;
	int	b = 1;
	int	at;

	at = find_bits(gd->gd_bmap, grp->gp_bmap_rsv, GRP_UNIT_BMSZ, 1, &b);
	/* NB: bitmap may includes more bits than the actual number of units */
	if (at < 0 || at >= gd->gd_unit_nr)
		return 0;

	setbit64(grp->gp_bmap_rsv, at);
	D_ASSERT(gd->gd_unit_free > 0);
	grp->gp_unit_rsv++;

	group_addref(grp);
	act->ra_group = grp;
	act->ra_bit   = at;

	return gd->gd_addr + at * gd->gd_unit;
}

static int
group_tx_free_addr(struct ad_group *grp, daos_off_t addr, struct ad_tx *tx)
{
	struct ad_group_df *gd = grp->gp_df;
	struct ad_free_act *act;
	int		    at;
	int		    rc;

	D_ALLOC_PTR(act);
	if (!act)
		return -DER_NOMEM;

	at = (addr - gd->gd_addr) / gd->gd_unit;
	rc = ad_tx_clrbits(tx, gd->gd_bmap, at, 1);
	if (rc)
		goto failed;

	rc = ad_tx_increase(tx, &gd->gd_unit_free);
	if (rc)
		goto failed;

	/* lock the bit and prevent it from being reused before commit */
	setbit64(grp->gp_bmap_rsv, at);
	grp->gp_unit_rsv++;

	group_addref(grp);
	act->fa_group = grp;
	act->fa_at    = at;
	d_list_add_tail(&act->fa_link, &tx->tx_gp_free);
	return 0;
failed:
	D_FREE(act);
	return rc;
}

static void
arena_reorder_if_needed(struct ad_arena *arena)
{
	struct ad_blob		*blob = arena->ar_blob;
	struct ad_maxheap_node	*node = arena2heap_node(arena);
	int			 new_weight = arena_weight(node->mh_free_size);

	/* NB, handle this once */
	if (node->mh_in_tree) {
		if (new_weight == node->mh_weight)
			return;
		d_binheap_remove(&blob->bb_arena_free_heap, &node->mh_node);
		node->mh_weight = new_weight;
		d_binheap_insert(&blob->bb_arena_free_heap, &node->mh_node);
	} else {
		if (!node->mh_inactive || node->mh_free_size < ARENA_SIZE >> 2)
			return;
		/* bring arena back if free space more than 1/4 of total size */
		node->mh_weight = new_weight;
		node->mh_inactive = 0;
		node->mh_arena_id = arena->ar_df->ad_id;
		d_binheap_insert(&blob->bb_arena_free_heap, &node->mh_node);
	}
}

static int
arena_reserve_addr(struct ad_arena *arena, daos_size_t size, struct ad_reserv_act *act,
		   daos_off_t *addr_p)
{
	struct ad_group		*grp;
	daos_off_t		 addr;
	int			 grp_at;
	int			 rc;
	bool			 tried = false;
	struct ad_maxheap_node	*node;

	rc = arena_find_grp(arena, size, &grp_at, &grp);
	if (rc == -DER_ENOENT ||	/* no arena, no group */
	    rc == -DER_NOSPACE) {	/* no space in this arena */
		grp_at = 0;
		grp = NULL;
		rc = 0;
		/* fall through */
	} else if (rc != 0) {
		D_ERROR("Failed to find group, arena=%d, rc=%d\n", arena->ar_df->ad_id, rc);
		return rc; /* other errors, failed to reserve */
	}

	while (1) {
		if (grp == NULL) { /* full group */
			D_DEBUG(DB_TRACE,
				"No group(size=%d) found in arena=%d, reserve a new one\n",
				(int)size, arena2id(arena));

			node = arena2heap_node(arena);
			rc = arena_reserve_grp(arena, size, &grp_at, &grp);
			if (rc == -DER_NOSPACE) { /* cannot create a new group, full arena */
				/* XXX: other sized groups may have space. */
				D_DEBUG(DB_TRACE, "Full arena=%d\n", arena->ar_df->ad_id);
				node->mh_inactive = 1;
				return rc;
			}
			if (rc != 0) {
				D_ERROR("Failed to reserve group, rc=%d\n", rc);
				return rc;
			}
		}
		D_DEBUG(DB_TRACE, "Found group=%p [r=%d, f=%d] for size=%d in arena=%d\n",
			grp->gp_df, grp->gp_unit_rsv, grp->gp_df->gd_unit_free,
			(int)size, arena2id(arena));

		addr = group_reserve_addr(grp, act);
		if (addr)
			break;

		D_ASSERT(!tried);
		tried = true;

		group_decref(grp);
		grp = NULL;
	}
	/* NB: reorder only does minimum amount of works most of the time */
	arena_reorder_grp(arena, grp, grp_at, true);
	node = arena2heap_node(arena);
	D_ASSERT(node->mh_free_size >= grp->gp_df->gd_unit);
	node->mh_free_size -= grp->gp_df->gd_unit;
	/*
	 * current arena is out from the binheap, so we don't have
	 * to update it's position in binheap all the time
	 */
	D_ASSERT(arena2heap_node(arena)->mh_in_tree == 0);
	group_decref(grp);

	arena_addref(arena);
	act->ra_arena = arena;
	*addr_p = addr;
	return 0;
}

static void
arena_remove_sort_group(struct ad_group *group)
{
	struct ad_arena		*arena = group->gp_arena;
	struct ad_group_df	*gd;
	struct ad_group_df	*tmp = NULL;
	int			 start;
	int			 end;
	int			 cur;
	bool			 found = false;
	int			 weight;
	int			 rc;

	/* remove this group from addr sort groups */
	for (start = cur = 0, end = arena->ar_grp_nr - 1; start <= end; ) {
		cur = (start + end) / 2;
		gd = arena->ar_addr_sorter[cur];

		if (gd->gd_addr == group->gp_df->gd_addr) {
			found = true;
			break;
		}

		if (gd->gd_addr < group->gp_df->gd_addr) {
			rc = -1;
		} else {
			rc = 1;
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}

	D_ASSERT(found);
	if (cur != arena->ar_grp_nr - 1)
		memmove(&arena->ar_addr_sorter[cur], &arena->ar_addr_sorter[cur + 1],
			(arena->ar_grp_nr - cur - 1) * sizeof(arena->ar_addr_sorter[0]));

	weight = group_weight(group->gp_df);
	found = false;
	/* remove this group from size sort groups */
	gd = group->gp_df;
	for (start = cur = 0, end = arena->ar_grp_nr - 1; start <= end; ) {
		cur = (start + end) / 2;
		tmp = arena->ar_size_sorter[cur];
		/* the same unit size */
		if (tmp->gd_unit == gd->gd_unit) {
			int	w;

			if (tmp == gd) {
				found = true;
				break;
			}

			w = group_weight(tmp);
			if (w < weight)
				rc = -1;
			else if (w > weight)
				rc = 1;
			else /* group address */
				rc = tmp->gd_addr < gd->gd_addr ? -1 : 1;

		} else if (tmp->gd_unit < gd->gd_unit) {
			rc = -1;
		} else {
			rc = 1;
		}

		if (rc < 0)
			start = cur + 1;
		else
			end = cur - 1;
	}
	D_ASSERT(found);
	if (cur != arena->ar_grp_nr - 1)
		memmove(&arena->ar_size_sorter[cur], &arena->ar_size_sorter[cur + 1],
			(arena->ar_grp_nr - cur - 1) * sizeof(arena->ar_size_sorter[0]));

	arena->ar_grp_nr--;
}

static inline int
gp_df2index(struct ad_group *group)
{
	struct ad_arena_df *ad = group->gp_arena->ar_df;

	return group->gp_df - &ad->ad_groups[0];
}

static int
group_tx_reset(struct ad_tx *tx, struct ad_group *group)
{
	struct ad_arena		*arena = group->gp_arena;
	struct ad_arena_df	*ad = arena->ar_df;
	struct ad_group_df	*gd = group->gp_df;
	int			 rc;
	struct ad_free_act	*act;

	/* not published yet */
	if (group->gp_unpub && !group->gp_publishing)
		return 0;
	if (gd->gd_unit_free != gd->gd_unit_nr)
		return 0;

	D_ALLOC_PTR(act);
	if (!act)
		return -DER_NOMEM;

	/* lock the bit and prevent it from being reused before commit */
	setbits64(arena->ar_space_rsv, group->gp_bit_at, group->gp_bit_nr);
	setbits64(arena->ar_gpid_rsv, gp_df2index(group), 1);
	D_DEBUG(DB_TRACE, "resetting group=%p, bit_at=%d, bits_nr=%d\n",
		group, group->gp_bit_at, group->gp_bit_nr);

	rc = ad_tx_clrbits(tx, ad->ad_bmap, group->gp_bit_at, group->gp_bit_nr);
	if (rc)
		goto failed;

	group->gp_reset = 1;
	arena_remove_sort_group(group);
	gd->gd_addr = 0;
	rc = ad_tx_set(tx, gd, 0, sizeof(*gd), AD_TX_REDO | AD_TX_LOG_ONLY);
	if (rc)
		goto failed;

	group_addref(group);
	act->fa_group = group;
	d_list_add_tail(&act->fa_link, &tx->tx_gp_reset);

	return 0;
failed:
	D_FREE(act);
	return rc;
}

static int
arena_tx_free_addr(struct ad_arena *arena, daos_off_t addr, struct ad_tx *tx)
{
	struct ad_group	     *grp;
	int		      rc, rc1;

	/* convert to address to the group it belonging to */
	grp = arena_addr2grp(arena, addr);
	if (!grp)
		return -DER_NOMEM; /* the only possible failure */

	rc = group_tx_free_addr(grp, addr, tx);
	if (rc == 0) {
		rc1 = group_tx_reset(tx, grp);
		if (rc1)
			rc = rc1;
	}
	group_decref(grp);

	return rc;
}

/**
 * Reserve storage space with specified @size, the space should be allocated from
 * default arena if @arena_id is set to zero, otherwise it is allocated from the
 * provided arena.
 */
static daos_off_t
ad_reserve_addr(struct ad_blob *blob, int type, daos_size_t size,
		uint32_t *arena_id, struct ad_reserv_act *act)
{
	struct ad_arena		*arena = NULL;
	daos_off_t		 addr;
	uint32_t		 id;
	bool			 tried;
	int			 rc;
	struct d_binheap_node	*node;
	struct ad_maxheap_node  *ad_node;

	if (arena_id && *arena_id != AD_ARENA_ANY)
		id = *arena_id;
	else
		id = blob->bb_arena_last[type]; /* the last used arena */

	if (id != AD_ARENA_ANY) {
		D_DEBUG(DB_TRACE, "Loading arena=%u\n", id);
		arena_remove_free_entry(blob, id);
		rc = arena_load(blob, id, &arena);
		if (rc) {
			D_DEBUG(DB_TRACE, "Failed to load arena %u: %d\n", id, rc);
			/* fall through and create a new one */
		} else if (arena2heap_node(arena)->mh_inactive) {
			D_DEBUG(DB_TRACE, "Arena %u is full, create a new one\n", id);
			arena_decref(arena);
			arena = NULL;
		}
	}

	tried = false;
	while (1) {
		if (arena == NULL) {
again:
			node = d_binheap_remove_root(&blob->bb_arena_free_heap);
			if (node == NULL) {
				rc = arena_reserve(blob, type, NULL, &arena);
				if (rc) {
					D_ERROR("Failed to reserve new arena.\n");
					return 0;
				}
			} else {
				ad_node = container_of(node, struct ad_maxheap_node, mh_node);
				rc = arena_load(blob, ad_node->mh_arena_id, &arena);
				if (rc) {
					D_DEBUG(DB_TRACE, "failed to load arena %u: %d\n",
						ad_node->mh_arena_id, rc);
					goto again;
				}
				blob->bb_arena_last[type] = ad_node->mh_arena_id;
			}
			tried = true;
		}

		D_DEBUG(DB_TRACE, "reserve space in arena=%d\n", arena2id(arena));
		rc = arena_reserve_addr(arena, size, act, &addr);
		if (rc) {
			D_DEBUG(DB_TRACE, "failed to reserve space from arena=%d, rc=%d\n",
				arena2id(arena), rc);
			arena_decref(arena);
			if (tried || rc != -DER_NOSPACE)
				return 0;

			arena = NULL;
			continue;
		}

		/* completed */
		if (arena_id)
			*arena_id = arena->ar_df->ad_id;

		arena_decref(arena);
		return addr;
	}
}

daos_off_t
ad_reserve(struct ad_blob_handle bh, int type, daos_size_t size,
	   uint32_t *arena_id, struct ad_reserv_act *act)
{
	return ad_reserve_addr(bh.bh_blob, type, size, arena_id, act);
}

int
tx_complete(struct ad_tx *tx, int err)
{
	struct ad_blob	   *blob  = tx->tx_blob;
	struct umem_store  *store = &blob->bb_store;
	struct ad_arena    *arena, *arena_last = NULL;
	struct ad_group	   *group;
	struct ad_free_act *fac;
	int		    rc;
	struct ad_maxheap_node	*node;

	/* TODO: Get umem_wal_tx from ad_tx */
	if (!err)
		rc = store->stor_ops->so_wal_submit(store, ad_tx2umem_tx(tx), NULL);
	else
		rc = err;

	while ((arena = d_list_pop_entry(&tx->tx_ar_pub, struct ad_arena, ar_link))) {
		arena->ar_publishing = 0;
		if (err) { /* keep the refcount and pin it */
			d_list_add(&arena->ar_link, &blob->bb_ars_rsv);
			continue;
		}
		clrbit64(blob->bb_bmap_rsv, arena->ar_df->ad_id);
		D_ASSERT(arena->ar_unpub);
		arena->ar_unpub = 0;
		arena_decref(arena);
	}

	while ((group = d_list_pop_entry(&tx->tx_gp_pub, struct ad_group, gp_link))) {
		group->gp_publishing = 0;
		if (err) { /* keep the refcount and pin it */
			d_list_add(&group->gp_link, &blob->bb_gps_rsv);
			continue;
		}
		clrbits64(group->gp_arena->ar_space_rsv, group->gp_bit_at, group->gp_bit_nr);
		D_ASSERT(group->gp_unpub);
		group->gp_unpub = 0;
		group_decref(group);
	}

	arena = NULL;
	while ((fac = d_list_pop_entry(&tx->tx_gp_free, struct ad_free_act, fa_link))) {
		int	pos;

		group = fac->fa_group;
		/* unlock the free bit, it can be used by future allocation */
		D_ASSERT(isset64(group->gp_bmap_rsv, fac->fa_at));
		clrbit64(group->gp_bmap_rsv, fac->fa_at);

		D_ASSERT(group->gp_unit_rsv > 0);
		if (err == 0) {
			/* Find current position of the group in binary-search array, change the
			 * available units of it, adjust position of the group and make sure the
			 * array is in right order.
			 */
			arena = group->gp_arena;
			if (!group->gp_reset) {
				pos = arena_locate_grp(arena, group);
				group->gp_unit_rsv--;
				arena_reorder_grp(arena, group, pos, false);
			} else {
				group->gp_unit_rsv--;
			}
			/* NB: reorder only does minimum amount of works most of the time */
			node = arena2heap_node(arena);
			node->mh_free_size += group->gp_df->gd_unit;
			if (arena != arena_last) {
				if (arena_last != NULL) {
					arena_reorder_if_needed(arena_last);
					arena_decref(arena_last);
				}
				arena_last = arena;
				arena_addref(arena_last);
			}
		} else {
			/* cancel the delayed free, no reorder */
			group->gp_unit_rsv--;
		}
		group_decref(group);
		D_FREE(fac);
	}

	if (arena_last != NULL) {
		arena_reorder_if_needed(arena_last);
		arena_decref(arena_last);
	}

	while ((fac = d_list_pop_entry(&tx->tx_gp_reset, struct ad_free_act, fa_link))) {
		group = fac->fa_group;
		arena = group->gp_arena;

		/* unlock the free bit, it can be used by future allocation */
		clrbits64(arena->ar_space_rsv, group->gp_bit_at, group->gp_bit_nr);
		clrbits64(arena->ar_gpid_rsv, gp_df2index(group), 1);
		/* add it back if error */
		if (err)
			arena_add_grp(arena, group, NULL);
		group->gp_reset = 0;
		group_decref(group);
		D_FREE(fac);
	}

	/* TODO: if rc != 0, run all undo operations */
	return rc;
}

/** Publish all the space reservations in @acts */
int
ad_tx_publish(struct ad_tx *tx, struct ad_reserv_act *acts, int act_nr)
{
	int	i;
	int	rc = 0;

	for (i = 0; i < act_nr; i++) {
		struct ad_arena	   *arena = acts[i].ra_arena;
		struct ad_group    *group = acts[i].ra_group;
		struct ad_group_df *gd = group->gp_df;

		if (arena->ar_unpub && !arena->ar_publishing) {
			D_DEBUG(DB_TRACE, "publishing arena=%d\n", arena2id(arena));
			rc = arena_tx_publish(arena, tx);
			if (rc)
				break;

			arena->ar_publishing = 1;
			if (d_list_empty(&arena->ar_link)) {
				arena_addref(arena);
				d_list_add_tail(&arena->ar_link, &tx->tx_ar_pub);
			} else {
				/* still on bb_ars_rsv, take over the refcount */
				d_list_move_tail(&arena->ar_link, &tx->tx_ar_pub);
			}
		}
		acts[i].ra_arena = NULL;
		arena_decref(arena);

		if (group->gp_unpub && !group->gp_publishing) {
			D_DEBUG(DB_TRACE, "publishing a new group, size=%d\n",
				(int)group->gp_df->gd_unit);
			rc = group_tx_publish(group, tx);
			if (rc)
				break;

			group->gp_publishing = 1;
			if (d_list_empty(&group->gp_link)) {
				group_addref(group);
				d_list_add_tail(&group->gp_link, &tx->tx_gp_pub);
			} else {
				/* still on bb_gps_rsv, take over the refcount */
				d_list_move_tail(&group->gp_link, &tx->tx_gp_pub);
			}
		}

		D_DEBUG(DB_TRACE, "publishing reserved bit=%d\n", acts[i].ra_bit);
		rc = ad_tx_setbits(tx, gd->gd_bmap, acts[i].ra_bit, 1);
		if (rc)
			break;

		rc = ad_tx_decrease(tx, &gd->gd_unit_free);
		if (rc)
			break;

		clrbit64(group->gp_bmap_rsv, acts[i].ra_bit);
		group->gp_unit_rsv--;

		acts[i].ra_group = NULL;
		group_decref(group);
	}
	return rc;
}

/** Cancel all the space reservaction in @acts */
void
ad_cancel(struct ad_reserv_act *acts, int act_nr)
{
	struct ad_arena	*arena = NULL, *arena_last = NULL;
	struct ad_blob	*blob;
	struct ad_group *group;
	int		 i;
	int		 pos;
	struct ad_maxheap_node	*node;

	for (i = 0; i < act_nr; i++) {
		group = acts[i].ra_group;
		arena = acts[i].ra_arena;
		blob = arena->ar_blob;
		D_DEBUG(DB_TRACE, "cancel bit=%d\n", acts[i].ra_bit);
		clrbit64(group->gp_bmap_rsv, acts[i].ra_bit);

		/* Find current position of the group in binary-search array, change the
		 * available units of it, adjust position of the group and make sure the
		 * array is in right order.
		 */
		pos = arena_locate_grp(arena, group);
		group->gp_unit_rsv--;
		/* NB: reorder only does minimum amount of works most of the time */
		arena_reorder_grp(arena, group, pos, false);
		node = &blob->bb_mh_nodes[arena->ar_df->ad_id];
		node->mh_free_size += group->gp_df->gd_unit;
		if (arena != arena_last) {
			if (arena_last != NULL) {
				arena_reorder_if_needed(arena_last);
				arena_decref(arena_last);
			}
			arena_last = arena;
			arena_addref(arena_last);
		}

		/* NB: arena and group are remained as "reserved" for now */
		if (group->gp_unpub && d_list_empty(&group->gp_link)) { /* pin it */
			D_ASSERT(!group->gp_publishing);
			/* refcount is taken over by the list */
			d_list_add(&group->gp_link, &blob->bb_gps_rsv);
		} else {
			group_decref(group);
		}

		if (arena->ar_unpub && d_list_empty(&arena->ar_link)) { /* pin it */
			D_ASSERT(!arena->ar_publishing);
			/* refcount is taken over by the list */
			d_list_add(&arena->ar_link, &blob->bb_ars_rsv);
		} else {
			arena_decref(arena);
		}
		acts[i].ra_arena = NULL;
		acts[i].ra_group = NULL;
	}

	if (arena_last != NULL) {
		arena_reorder_if_needed(arena_last);
		arena_decref(arena_last);
	}
}

daos_off_t
ad_alloc(struct ad_blob_handle bh, int type, daos_size_t size, uint32_t *arena_id)
{
	struct ad_reserv_act act;
	struct ad_tx	     tx;
	daos_off_t	     addr;
	int		     rc;

	addr = ad_reserve_addr(bh.bh_blob, type, size, arena_id, &act);
	if (addr == 0)
		return 0; /* no space */

	rc = ad_tx_begin(bh, &tx);
	if (rc)
		goto failed;

	rc = ad_tx_publish(&tx, &act, 1);

	ad_tx_end(&tx, rc);
	if (rc)
		return 0;

	return addr;
failed:
	ad_cancel(&act, 1);
	return 0;
}

/** Free address in a transacton */
int
ad_tx_free(struct ad_tx *tx, daos_off_t addr)
{
	struct ad_blob	   *blob = tx->tx_blob;
	struct ad_arena_df *ad;
	struct ad_arena	   *arena;
	int		    rc;

	/* arena is stored as the page header */
	ad = blob_addr2ptr(blob, addr & ~ARENA_SIZE_MASK);
	D_ASSERT(ad->ad_magic == ARENA_MAGIC);

	D_DEBUG(DB_TRACE, "loading arena for free\n");
	rc = arena_load(blob, ad->ad_id, &arena);
	if (rc)
		return rc;

	rc = arena_tx_free_addr(arena, addr, tx);
	arena_decref(arena);
	return rc;
}

/** Register a new arena type */
static int
blob_register_arena(struct ad_blob *blob, unsigned int arena_type,
		    struct ad_group_spec *specs, unsigned int specs_nr, struct ad_tx *tx)
{
	struct ad_blob_df	*bd = blob->bb_df;
	struct ad_arena_spec	*spec;
	int			 i;
	int			 rc;

	if (arena_type >= ARENA_SPEC_MAX)
		return -DER_INVAL;

	if (specs_nr >= ARENA_GRP_SPEC_MAX)
		return -DER_INVAL;

	spec = &bd->bd_asp[arena_type];
	if (spec->as_specs_nr != 0)
		return -DER_EXIST;

	rc = ad_tx_increase(tx, &bd->bd_asp_nr);
	if (rc)
		return rc;

	spec->as_type	    = arena_type;
	spec->as_specs_nr   = specs_nr;
	spec->as_last_used  = AD_ARENA_ANY;
	for (i = 0; i < specs_nr; i++)
		spec->as_specs[i] = specs[i];

	blob->bb_arena_last[arena_type] = AD_ARENA_ANY;
	rc = ad_tx_snap(tx, spec, sizeof(*spec), AD_TX_REDO);
	if (rc)
		return rc;

	return 0;
}

int
ad_arena_register(struct ad_blob_handle bh, unsigned int arena_type,
		  struct ad_group_spec *specs, unsigned int specs_nr)
{
	struct ad_tx	tx;
	int		rc;

	rc = ad_tx_begin(bh, &tx);
	if (rc)
		return rc;

	rc = blob_register_arena(bh.bh_blob, arena_type, specs, specs_nr, &tx);
	rc = ad_tx_end(&tx, rc);
	return rc;
}

/****************************************************************************
 * A few helper functions:
 ****************************************************************************/
static void
group_unbind(struct ad_group *grp, bool reset)
{
	if (grp->gp_df && grp->gp_df->gd_back_ptr) {
		D_ASSERT(grp == group_df2ptr(grp->gp_df));
		grp->gp_df->gd_back_ptr = 0;
		grp->gp_df = NULL;
	}
	if (grp->gp_arena) {
		arena_decref(grp->gp_arena);
		grp->gp_arena = NULL;
	}

	if (reset)
		memset(grp, 0, sizeof(*grp));
}

static struct ad_group *
alloc_group(struct ad_arena *arena, bool force)
{
	struct ad_group *grp = NULL;

	if (!force) {
		struct ad_blob  *blob = arena->ar_blob;

		grp = d_list_pop_entry(&blob->bb_gps_lru, struct ad_group, gp_link);
		if (grp)
			blob->bb_gps_lru_size--;
	}

	if (!grp) {
		D_ALLOC_PTR(grp);
		if (!grp)
			return NULL;
	} else {
		group_unbind(grp, true);
	}
	D_INIT_LIST_HEAD(&grp->gp_link);
	if (arena) {
		arena_addref(arena);
		grp->gp_arena = arena;
	}
	return grp;
}

static void
group_free(struct ad_group *grp, bool force)
{
	D_ASSERT(grp->gp_ref == 0);
	D_ASSERT(d_list_empty(&grp->gp_link));

	if (!force) {
		struct ad_arena *arena = grp->gp_arena;
		struct ad_blob  *blob;

		D_ASSERT(arena);
		blob = arena->ar_blob;
		D_ASSERT(blob);

		d_list_add_tail(&grp->gp_link, &blob->bb_gps_lru);
		if (blob->bb_gps_lru_size < blob->bb_gps_lru_cap) {
			if (grp->gp_df) {
				grp->gp_df->gd_back_ptr = 0;
				grp->gp_df = NULL;
			}
			blob->bb_gps_lru_size++;
			return;
		}
		/* release an old one from the LRU */
		grp = d_list_pop_entry(&blob->bb_gps_lru, struct ad_group, gp_link);
	}
	group_unbind(grp, false);
	D_FREE(grp);
}

static void
arena_unbind(struct ad_arena *arena, bool reset)
{
	if (arena->ar_blob) {
		blob_decref(arena->ar_blob);
		arena->ar_blob = NULL;
	}

	if (arena->ar_df && arena->ar_df->ad_back_ptr) {
		D_ASSERT(arena_df2ptr(arena->ar_df) == arena);
		arena->ar_df->ad_back_ptr = 0;
		arena->ar_df = NULL;
	}

	if (reset) {
		struct ad_group_df **p1;
		struct ad_group_df **p2;
		int		     sz;

		sz = arena->ar_sorter_sz;
		p1 = arena->ar_size_sorter;
		p2 = arena->ar_addr_sorter;

		memset(arena, 0, sizeof(*arena));
		arena->ar_size_sorter = p1;
		arena->ar_addr_sorter = p2;
		arena->ar_sorter_sz = sz;
	}
}

static int
arena_init_sorters(struct ad_arena *arena, int sorter_sz)
{
	struct ad_group_df **size_sorter = NULL;
	struct ad_group_df **addr_sorter = NULL;

	if (arena->ar_sorter_sz >= sorter_sz) {
		D_ASSERT(arena->ar_size_sorter);
		D_ASSERT(arena->ar_addr_sorter);
		return 0;
	}

	D_REALLOC(size_sorter, arena->ar_size_sorter,
		  sizeof(*size_sorter) * arena->ar_sorter_sz, sizeof(*size_sorter) * sorter_sz);
	if (!size_sorter)
		return -DER_NOMEM;

	D_REALLOC(addr_sorter, arena->ar_addr_sorter,
		  sizeof(*addr_sorter) * arena->ar_sorter_sz, sizeof(*addr_sorter) * sorter_sz);
	if (!addr_sorter)
		goto failed;

	arena->ar_size_sorter = size_sorter;
	arena->ar_addr_sorter = addr_sorter;
	arena->ar_sorter_sz   = sorter_sz;
	return 0;
failed:
	D_FREE(size_sorter);
	D_FREE(addr_sorter);
	return -DER_NOMEM;
}

static struct ad_arena *
arena_alloc(struct ad_blob *blob, bool force, int sorter_sz)
{
	struct ad_arena *arena = NULL;
	int		 rc;

	sorter_sz = sorter_sz > ARENA_GRP_AVG ? ARENA_GRP_MAX : ARENA_GRP_AVG;
	if (!force) {
		D_ASSERT(blob);
		arena = d_list_pop_entry(&blob->bb_ars_lru, struct ad_arena, ar_link);
		if (arena)
			blob->bb_ars_lru_size--;
	}

	if (arena) {
		arena_unbind(arena, true);
	} else {
		D_ALLOC_PTR(arena);
		if (!arena)
			return NULL;
	}

	D_INIT_LIST_HEAD(&arena->ar_link);
	if (arena->ar_sorter_sz < sorter_sz) {
		rc = arena_init_sorters(arena, sorter_sz);
		if (rc)
			goto failed;
	}

	if (blob) {
		blob_addref(blob);
		arena->ar_blob = blob;
	}
	return arena;
failed:
	arena_free(arena, true);
	return NULL;
}

static void
arena_free(struct ad_arena *arena, bool force)
{
	D_ASSERT(arena->ar_ref == 0);
	D_ASSERT(d_list_empty(&arena->ar_link));

	if (!force) {
		struct ad_blob *blob = arena->ar_blob;

		D_ASSERT(blob);
		d_list_add_tail(&arena->ar_link, &blob->bb_ars_lru);
		if (blob->bb_ars_lru_size < blob->bb_ars_lru_cap) {
			blob->bb_ars_lru_size++;
			return;
		}
		/* release an old one from the LRU */
		arena = d_list_pop_entry(&blob->bb_ars_lru, struct ad_arena, ar_link);
	}

	D_FREE(arena->ar_addr_sorter);
	D_FREE(arena->ar_size_sorter);
	arena_unbind(arena, false);

	D_FREE(arena);
}
