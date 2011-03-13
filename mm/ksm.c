/*
 * Memory merging support.
 *
 * This code enables dynamic sharing of identical pages found in different
 * memory areas, even if they are not shared by fork()
 *
 * Copyright (C) 2008-2009 Red Hat, Inc.
 * Authors:
 *	Izik Eidus
 *	Andrea Arcangeli
 *	Chris Wright
 *	Hugh Dickins
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 *
 *
 * Ultra KSM. Copyright (C) 2011 Nai Xia
 *
 * This is an improvement upon KSM. Its features:
 * 1. Full system scan:
 *      It automatically scans all user processes' anonymous VMAs. Kernel-user
 *      interaction to submit a memory area to KSM is no longer needed.
 *
 * 2. Rich area detection based on random sampling:
 *      It automatically detects rich areas containing abundant duplicated
 *      pages based on their randomly-sampled history. Rich areas are given
 *      a full scan speed. Poor areas are sampled at a reasonable speed with
 *      very low CPU consumption.
 *
 * 3. Per-page scan speed improvement:
 *      A new hash algorithm(random_sample_hash) is proposed. Quite usually,
 *      it's enough to distinguish pages by hashing their partial content
 *      instead of full pages. This algorithm can automatically adapt to this
 *      situation. For the best case, only one 32-bit-word/page is needed to
 *      get the hash value for distinguishing pages. For the worst case, it's as
 *      fast as SuperFastHash.
 *
 * 4. Thrashing area avoidance:
 *      Thrashing area(an VMA that has frequent Ksm page break-out) can be
 *      filtered out. My benchmark shows it's more efficient than KSM's per-page
 *      hash value based volatile page detection.
 *
 * 5. Hash-value-based identical page detection:
 *      It no longer uses "memcmp" based page detection any more.
 *
 * 6. Misc changes upon KSM:
 *      * It has a fully x86-opitmized memcmp dedicated for 4-byte-aligned page
 *        comparison. It's much faster than default C version on x86.
 *      * rmap_item now has an struct *page member to loosely cache a
 *        address-->page mapping, which reduces too much time-costly
 *        follow_page().
 *      * The VMA creation/exit procedures are hooked to let the Ultra KSM know.
 *      * try_to_merge_two_pages() now can revert a pte if it fails. No break_
 *        ksm is needed for this case.
 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/memory.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/ksm.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <crypto/hash.h>
#include <linux/random.h>
#include <linux/math64.h>
#include <linux/gcd.h>

#include <asm/tlbflush.h>
#include "internal.h"



#ifdef CONFIG_X86
#undef memcmp
#define memcmp memcmpx86

/*
 * Compare 4-byte-aligned pages at address s1 and s2, with length n
 */
int memcmpx86(void *s1, void *s2, size_t n)
{
   size_t num = n / 4;
   register int res;
  __asm__ __volatile__
    ("cld\n\t"
     "testl %3,%3\n\t"
     "repe; cmpsd\n\t"
     "je        1f\n\t"
     "sbbl      %0,%0\n\t"
     "orl       $1,%0\n"
     "1:"
     : "=&a" (res), "+&S" (s1), "+&D" (s2), "+&c" (num)
     : "0" (0)
     : "cc");

  return res;
}
#endif

/*
 * Flags for rmap_item to judge if it's listed in the stable/unstable tree.
 * The flags use the low bits of rmap_item.address
 */
#define UNSTABLE_FLAG	0x1
#define STABLE_FLAG	0x2
#define get_rmap_addr(x)	((x)->address & PAGE_MASK)

/*
 * rmap_list_entry helpers
 */
#define IS_ADDR_FLAG	1
#define is_addr(ptr)		((unsigned long)(ptr) & IS_ADDR_FLAG)
#define set_is_addr(ptr)	((ptr) |= IS_ADDR_FLAG)
#define get_clean_addr(ptr)	(((ptr) & ~(__typeof__(ptr))IS_ADDR_FLAG))


/*
 * High speed caches for frequently allocated and freed structs
 */
static struct kmem_cache *rmap_item_cache;
static struct kmem_cache *stable_node_cache;
static struct kmem_cache *node_vma_cache;
static struct kmem_cache *vma_slot_cache;
static struct kmem_cache *tree_node_cache;
#define KSM_KMEM_CACHE(__struct, __flags) kmem_cache_create("ksm_"#__struct,\
		sizeof(struct __struct), __alignof__(struct __struct),\
		(__flags), NULL)

/* The scan rounds ksmd is currently in */
static unsigned long long ksm_scan_round = 1;

/* The number of pages has been scanned since the start up */
static unsigned long long ksm_pages_scanned;

/* The number of pages has been scanned when last scan round finished */
static unsigned long long ksm_pages_scanned_last;

/* The number of nodes in the stable tree */
static unsigned long ksm_pages_shared;

/* The number of page slots additionally sharing those nodes */
static unsigned long ksm_pages_sharing;

/* The number of nodes in the unstable tree */
static unsigned long ksm_pages_unshared;

/*
 * Number of pages ksmd should scan in one batch. This is the top speed for
 * richly duplicated areas.
 */
static unsigned long ksm_scan_batch_pages = 60000;

/* Milliseconds ksmd should sleep between batches */
static unsigned int ksm_sleep_jiffies = 2;

/*
 * The threshold used to filter out thrashing areas,
 * If it == 0, filtering is disabled, otherwise it's the percentage up-bound
 * of the thrashing ratio of all areas. Any area with a bigger thrashing ratio
 * will be considered as having a zero duplication ratio.
 */
static unsigned int ksm_thrash_threshold;

/* To avoid the float point arithmetic, this is the scale of a
 * deduplication ratio number.
 */
#define KSM_DEDUP_RATIO_SCALE	100


#define KSM_SCAN_RATIO_MAX	125

/* minimum scan ratio for a vma, in unit of 1/KSM_SCAN_RATIO_MAX */
static unsigned int ksm_min_scan_ratio = 1;

/*
 * After each scan round, the scan ratio of an area with a big deduplication
 * ratio is upgraded by *=ksm_scan_ratio_delta
 */
static unsigned int ksm_scan_ratio_delta = 5;

/*
 * Inter-vma duplication number table page pointer array, initialized at
 * startup. Whenever ksmd finds that two areas have an identical page,
 * their corresponding table entry is increased. After each scan round
 * is finished, this table is scanned to calculate the estimated
 * duplication ratio for VMAs. Limited number(2048) of VMAs are
 * supported by now. We will migrate it to more scalable data structures
 * in the future.
 */
#define KSM_DUP_VMA_MAX		2048
static unsigned int *ksm_inter_vma_table;

/*
 * For mapping of vma_slot and its index in inter-vma duplication number
 * table
 */
static struct vma_slot **ksm_vma_table;
static unsigned int ksm_vma_table_size = 2048;
static unsigned long ksm_vma_table_num;
static unsigned long ksm_vma_table_index_end;

/* Array of all scan_rung, ksm_scan_ladder[0] having the minimum scan ratio */
static struct scan_rung *ksm_scan_ladder;
static unsigned int ksm_scan_ladder_size;

/* The number of VMAs we are keeping track of */
static unsigned long ksm_vma_slot_num;

/* How many times the ksmd has slept since startup */
static u64 ksm_sleep_times;

#define KSM_RUN_STOP	0
#define KSM_RUN_MERGE	1
static unsigned int ksm_run = KSM_RUN_STOP;

static DECLARE_WAIT_QUEUE_HEAD(ksm_thread_wait);
static DEFINE_MUTEX(ksm_thread_mutex);

/*
 * List vma_slot_new is for newly created vma_slot waiting to be added by
 * ksmd. If one cannot be added(e.g. due to it's too small), it's moved to
 * vma_slot_noadd. vma_slot_del is the list for vma_slot whose corresponding
 * VMA has been removed/freed.
 */
struct list_head vma_slot_new = LIST_HEAD_INIT(vma_slot_new);
struct list_head vma_slot_noadd = LIST_HEAD_INIT(vma_slot_noadd);
struct list_head vma_slot_del = LIST_HEAD_INIT(vma_slot_del);
static DEFINE_SPINLOCK(vma_slot_list_lock);

/* The unstable tree heads */
static struct rb_root root_unstable_tree = RB_ROOT;

/*
 * All tree_nodes are in a list to be freed at once when unstable tree is
 * freed after each scan round.
 */
static struct list_head unstable_tree_node_list =
				LIST_HEAD_INIT(unstable_tree_node_list);

/* List contains all stable nodes */
static struct list_head stable_node_list = LIST_HEAD_INIT(stable_node_list);

/*
 * When the hash strength is changed, the stable tree must be delta_hashed and
 * re-structured. We use two set of below structs to speed up the
 * re-structuring of stable tree.
 */
static struct list_head
stable_tree_node_list[2] = {LIST_HEAD_INIT(stable_tree_node_list[0]),
			    LIST_HEAD_INIT(stable_tree_node_list[1])};

static struct list_head *stable_tree_node_listp = &stable_tree_node_list[0];
static struct rb_root root_stable_tree[2] = {RB_ROOT, RB_ROOT};
static struct rb_root *root_stable_treep = &root_stable_tree[0];
static unsigned long stable_tree_index;

/* The hash strength needed to hash a full page */
#define HASH_STRENGTH_FULL		(PAGE_SIZE / sizeof(u32))

/* The hash strength needed for loop-back hashing */
#define HASH_STRENGTH_MAX		(HASH_STRENGTH_FULL + 10)

/* The random offsets in a page */
static u32 *random_nums;

/* The hash strength */
static unsigned long hash_strength = HASH_STRENGTH_FULL >> 4;

/* The delta value each time the hash strength increases or decreases */
static unsigned long hash_strength_delta;
#define HASH_STRENGTH_DELTA_MAX	5

/* The time we have saved due to random_sample_hash */
static u64 rshash_pos;

/* The time we have wasted due to hash collision */
static u64 rshash_neg;

/*
 * The relative cost of memcmp, compared to 1 time unit of random sample
 * hash, this value is tested when ksm module is initialized
 */
static unsigned long memcmp_cost;

static unsigned long  rshash_neg_cont_zero;
static unsigned long  rshash_cont_obscure;

/* The possible states of hash strength adjustment heuristic */
enum rshash_states {
		RSHASH_STILL,
		RSHASH_TRYUP,
		RSHASH_TRYDOWN,
		RSHASH_NEW,
		RSHASH_PRE_STILL,
};

/* The possible direction we are about to adjust hash strength */
enum rshash_direct {
	GO_UP,
	GO_DOWN,
	OBSCURE,
	STILL,
};

/* random sampling hash state machine */
static struct {
	enum rshash_states state;
	enum rshash_direct pre_direct;
	u8 below_count;
	/* Keep a lookup window of size 5, iff above_count/below_count > 3
	 * in this window we stop trying.
	 */
	u8 lookup_window_index;
	u64 stable_benefit;
	unsigned long turn_point_down;
	unsigned long turn_benefit_down;
	unsigned long turn_point_up;
	unsigned long turn_benefit_up;
	unsigned long stable_point;
} rshash_state;



static inline struct node_vma *alloc_node_vma(void)
{
	struct node_vma *node_vma;
	node_vma = kmem_cache_zalloc(node_vma_cache, GFP_KERNEL);
	if (node_vma) {
		INIT_HLIST_HEAD(&node_vma->rmap_hlist);
		INIT_HLIST_NODE(&node_vma->hlist);
		node_vma->last_update = 0;
	}
	return node_vma;
}

static inline void free_node_vma(struct node_vma *node_vma)
{
	kmem_cache_free(node_vma_cache, node_vma);
}


static inline struct vma_slot *alloc_vma_slot(void)
{
	struct vma_slot *slot;

	/*
	 * In case ksm is not initialized by now.
	 * Oops, we need to consider the call site of ksm_init() in the future.
	 */
	if (!vma_slot_cache)
		return NULL;

	slot = kmem_cache_zalloc(vma_slot_cache, GFP_KERNEL);
	if (slot) {
		INIT_LIST_HEAD(&slot->ksm_list);
		INIT_LIST_HEAD(&slot->slot_list);
		slot->ksm_index = -1;
		slot->need_rerand = 1;
	}
	return slot;
}

static inline void free_vma_slot(struct vma_slot *vma_slot)
{
	kmem_cache_free(vma_slot_cache, vma_slot);
}



static inline struct rmap_item *alloc_rmap_item(void)
{
	struct rmap_item *rmap_item;

	rmap_item = kmem_cache_zalloc(rmap_item_cache, GFP_KERNEL);
	if (rmap_item) {
		/* bug on lowest bit is not clear for flag use */
		BUG_ON(is_addr(rmap_item));
	}
	return rmap_item;
}

static inline void free_rmap_item(struct rmap_item *rmap_item)
{
	rmap_item->slot = NULL;	/* debug safety */
	kmem_cache_free(rmap_item_cache, rmap_item);
}

static inline struct stable_node *alloc_stable_node(void)
{
	struct stable_node *node;
	node = kmem_cache_alloc(stable_node_cache, GFP_KERNEL | GFP_ATOMIC);
	if (!node)
		return NULL;

	INIT_HLIST_HEAD(&node->hlist);
	list_add(&node->all_list, &stable_node_list);
	return node;
}

static inline void free_stable_node(struct stable_node *stable_node)
{
	list_del(&stable_node->all_list);
	kmem_cache_free(stable_node_cache, stable_node);
}

static inline struct tree_node *alloc_tree_node(struct list_head *list)
{
	struct tree_node *node;
	node = kmem_cache_zalloc(tree_node_cache, GFP_KERNEL | GFP_ATOMIC);
	if (!node)
		return NULL;

	list_add(&node->all_list, list);
	return node;
}

static inline void free_tree_node(struct tree_node *node)
{
	list_del(&node->all_list);
	kmem_cache_free(tree_node_cache, node);
}

static void drop_anon_vma(struct rmap_item *rmap_item)
{
	struct anon_vma *anon_vma = rmap_item->anon_vma;

	if (atomic_dec_and_lock(&anon_vma->external_refcount, &anon_vma->root->lock)) {
		int empty = list_empty(&anon_vma->head);
		anon_vma_unlock(anon_vma);
		if (empty)
			anon_vma_free(anon_vma);
	}
}


/**
 * Remove a stable node from stable_tree, may unlink from its tree_node and
 * may remove its parent tree_node if no other stable node is pending.
 *
 * @stable_node 	The node need to be removed
 * @unlink_rb 		Will this node be unlinked from the rbtree?
 * @remove_tree_	node Will its tree_node be removed if empty?
 */
static void remove_node_from_stable_tree(struct stable_node *stable_node,
					 int unlink_rb,  int remove_tree_node)
{
	struct node_vma *node_vma;
	struct rmap_item *rmap_item;
	struct hlist_node *hlist, *rmap_hlist, *n;

	if (!hlist_empty(&stable_node->hlist)) {
		hlist_for_each_entry_safe(node_vma, hlist, n,
					  &stable_node->hlist, hlist) {
			hlist_for_each_entry(rmap_item, rmap_hlist,
					     &node_vma->rmap_hlist, hlist) {
				ksm_pages_sharing--;

				drop_anon_vma(rmap_item);
				rmap_item->address &= PAGE_MASK;
			}
			free_node_vma(node_vma);
			cond_resched();
		}

		/* the last one is counted as shared */
		ksm_pages_shared--;
		ksm_pages_sharing++;
	}

	if (stable_node->tree_node && unlink_rb) {
		rb_erase(&stable_node->node,
			 &stable_node->tree_node->sub_root);

		if (RB_EMPTY_ROOT(&stable_node->tree_node->sub_root) &&
		    remove_tree_node) {
			rb_erase(&stable_node->tree_node->node,
				 root_stable_treep);
			free_tree_node(stable_node->tree_node);
		} else {
			stable_node->tree_node->count--;
		}
	}

	free_stable_node(stable_node);
}


/*
 * get_ksm_page: checks if the page indicated by the stable node
 * is still its ksm page, despite having held no reference to it.
 * In which case we can trust the content of the page, and it
 * returns the gotten page; but if the page has now been zapped,
 * remove the stale node from the stable tree and return NULL.
 *
 * You would expect the stable_node to hold a reference to the ksm page.
 * But if it increments the page's count, swapping out has to wait for
 * ksmd to come around again before it can free the page, which may take
 * seconds or even minutes: much too unresponsive.  So instead we use a
 * "keyhole reference": access to the ksm page from the stable node peeps
 * out through its keyhole to see if that page still holds the right key,
 * pointing back to this stable node.  This relies on freeing a PageAnon
 * page to reset its page->mapping to NULL, and relies on no other use of
 * a page to put something that might look like our key in page->mapping.
 *
 * include/linux/pagemap.h page_cache_get_speculative() is a good reference,
 * but this is different - made simpler by ksm_thread_mutex being held, but
 * interesting for assuming that no other use of the struct page could ever
 * put our expected_mapping into page->mapping (or a field of the union which
 * coincides with page->mapping).  The RCU calls are not for KSM at all, but
 * to keep the page_count protocol described with page_cache_get_speculative.
 *
 * Note: it is possible that get_ksm_page() will return NULL one moment,
 * then page the next, if the page is in between page_freeze_refs() and
 * page_unfreeze_refs(): this shouldn't be a problem anywhere, the page
 * is on its way to being freed; but it is an anomaly to bear in mind.
 *
 * @unlink_rb: 		if the removal of this node will firstly unlink from
 * its rbtree. stable_node_reinsert will prevent this when restructuring the
 * node from its old tree.
 *
 * @remove_tree_node:	if this is the last one of its tree_node, will the
 * tree_node be freed ? If we are inserting stable node, this tree_node may
 * be reused, so don't free it.
 */
static struct page *get_ksm_page(struct stable_node *stable_node,
				 int unlink_rb, int remove_tree_node)
{
	struct page *page;
	void *expected_mapping;

	page = pfn_to_page(stable_node->kpfn);
	expected_mapping = (void *)stable_node +
				(PAGE_MAPPING_ANON | PAGE_MAPPING_KSM);
	rcu_read_lock();
	if (page->mapping != expected_mapping)
		goto stale;
	if (!get_page_unless_zero(page))
		goto stale;
	if (page->mapping != expected_mapping) {
		put_page(page);
		goto stale;
	}
	rcu_read_unlock();
	return page;
stale:
	rcu_read_unlock();
	remove_node_from_stable_tree(stable_node, unlink_rb, remove_tree_node);

	return NULL;
}

/*
 * Removing rmap_item from stable or unstable tree.
 * This function will clean the information from the stable/unstable tree.
 */
static inline void remove_rmap_item_from_tree(struct rmap_item *rmap_item)
{
	if (rmap_item->address & STABLE_FLAG) {
		struct stable_node *stable_node;
		struct node_vma *node_vma;
		struct page *page;

		node_vma = rmap_item->head;
		stable_node = node_vma->head;
		page = get_ksm_page(stable_node, 1, 1);
		if (!page)
			goto out;

		/*
		 * page lock is needed because it's racing with
		 * try_to_unmap_ksm(), etc.
		 */
		lock_page(page);
		hlist_del(&rmap_item->hlist);

		if (hlist_empty(&node_vma->rmap_hlist)) {
			hlist_del(&node_vma->hlist);
			free_node_vma(node_vma);
		}
		unlock_page(page);

		put_page(page);
		if (hlist_empty(&stable_node->hlist)) {
			/* do NOT call remove_node_from_stable_tree() here,
			 * it's possible for a forked rmap_item not in
			 * stable tree while the in-tree rmap_items were
			 * deleted.
			 */
			ksm_pages_shared--;
		} else
			ksm_pages_sharing--;


		drop_anon_vma(rmap_item);
	} else if (rmap_item->address & UNSTABLE_FLAG) {
		/*
		 * Usually ksmd can and must skip the rb_erase, because
		 * root_unstable_tree was already reset to RB_ROOT.
		 * But be careful when an mm is exiting: do the rb_erase
		 * if this rmap_item was inserted by this scan, rather
		 * than left over from before.
		 */
		if (rmap_item->append_round == ksm_scan_round) {
			rb_erase(&rmap_item->node,
				 &rmap_item->tree_node->sub_root);
			if (RB_EMPTY_ROOT(&rmap_item->tree_node->sub_root)) {
				rb_erase(&rmap_item->tree_node->node,
					 &root_unstable_tree);

				free_tree_node(rmap_item->tree_node);
			} else
				rmap_item->tree_node->count--;
		}
		ksm_pages_unshared--;
	}

	rmap_item->address &= PAGE_MASK;
	rmap_item->hash_max = 0;

out:
	cond_resched();		/* we're called from many long loops */
}

/**
 * Need to do two things:
 * 1. check if slot was moved to del list
 * 2. make sure the mmap_sem is manipulated under valid vma.
 *
 * My concern here is that in some cases, this may make
 * vma_slot_list_lock() waiters to serialized further by some
 * sem->wait_lock, can this really be expensive?
 *
 *
 * @return
 * 0: if successfully locked mmap_sem
 * -ENOENT: this slot was moved to del list
 * -EBUSY: vma lock failed
 */
static int try_down_read_slot_mmap_sem(struct vma_slot *slot)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	struct rw_semaphore *sem;

	spin_lock(&vma_slot_list_lock);

	/* the slot_list was removed and inited from new list, when it enters
	 * ksm_list. If now it's not empty, then it must be moved to del list
	 */
	if (!list_empty(&slot->slot_list)) {
		spin_unlock(&vma_slot_list_lock);
		return -ENOENT;
	}

	BUG_ON(slot->pages != vma_pages(slot->vma));
	/* Ok, vma still valid */
	vma = slot->vma;
	mm = vma->vm_mm;
	sem = &mm->mmap_sem;
	if (down_read_trylock(sem)) {
		spin_unlock(&vma_slot_list_lock);
		return 0;
	}

	spin_unlock(&vma_slot_list_lock);
	return -EBUSY;
}

static inline unsigned long
vma_page_address(struct page *page, struct vm_area_struct *vma)
{
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	unsigned long address;

	address = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	if (unlikely(address < vma->vm_start || address >= vma->vm_end)) {
		/* page should be within @vma mapping range */
		return -EFAULT;
	}
	return address;
}

/*
 * Test if the mm is exiting
 */
static inline bool ksm_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

/* return 0 on success with the item's mmap_sem locked */
static inline int get_mergeable_page_lock_mmap(struct rmap_item *item)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct vma_slot *slot = item->slot;
	int err = -EINVAL;

	struct page *page;

	BUG_ON(!item->slot);
	/*
	 * try_down_read_slot_mmap_sem() returns non-zero if the slot
	 * has been removed by ksm_remove_vma().
	 */
	if (try_down_read_slot_mmap_sem(slot))
		return -EBUSY;

	mm = slot->vma->vm_mm;
	vma = slot->vma;

	if (ksm_test_exit(mm))
		goto failout_up;

	page = item->page;
	rcu_read_lock();
	if (!get_page_unless_zero(page)) {
		rcu_read_unlock();
		goto failout_up;
	}
	if (item->slot->vma->anon_vma != page_anon_vma(page) ||
	    vma_page_address(page, item->slot->vma) != get_rmap_addr(item)) {
		put_page(page);
		rcu_read_unlock();
		goto failout_up;
	}
	rcu_read_unlock();
	return 0;

failout_up:
	up_read(&mm->mmap_sem);
	return err;
}

/*
 * What kind of VMA is considered ?
 */
static inline int vma_can_enter(struct vm_area_struct *vma)
{
	return !(vma->vm_flags & (VM_PFNMAP | VM_IO  | VM_DONTEXPAND |
				  VM_RESERVED  | VM_HUGETLB | VM_INSERTPAGE |
				  VM_NONLINEAR | VM_MIXEDMAP | VM_SAO |
				  VM_SHARED  | VM_MAYSHARE | VM_GROWSUP
				  | VM_GROWSDOWN));
}

/*
 * Called whenever a fresh new vma is created A new vma_slot.
 * is created and inserted into a global list Must be called.
 * after vma is inserted to its mm      		    .
 */
inline void ksm_vma_add_new(struct vm_area_struct *vma)
{
	struct vma_slot *slot;

	if (!vma_can_enter(vma)) {
		vma->ksm_vma_slot = NULL;
		return;
	}

	slot = alloc_vma_slot();
	if (!slot) {
		vma->ksm_vma_slot = NULL;
		return;
	}

	vma->ksm_vma_slot = slot;
	slot->vma = vma;
	slot->mm = vma->vm_mm;
	slot->ctime_j = jiffies;
	slot->pages = vma_pages(vma);
	spin_lock(&vma_slot_list_lock);
	list_add_tail(&slot->slot_list, &vma_slot_new);
	spin_unlock(&vma_slot_list_lock);
}

/*
 * Called after vma is unlinked from its mm
 */
void ksm_remove_vma(struct vm_area_struct *vma)
{
	struct vma_slot *slot;

	if (!vma->ksm_vma_slot)
		return;

	slot = vma->ksm_vma_slot;
	spin_lock(&vma_slot_list_lock);
	if (list_empty(&slot->slot_list)) {
		/**
		 * This slot has been added by ksmd, so move to the del list
		 * waiting ksmd to free it.
		 */
		list_add_tail(&slot->slot_list, &vma_slot_del);
	} else {
		/**
		 * It's still on new list. It's ok to free slot directly.
		 */
		list_del(&slot->slot_list);
		free_vma_slot(slot);
	}
	spin_unlock(&vma_slot_list_lock);
	vma->ksm_vma_slot = NULL;
}

/*   32/3 < they < 32/2 */
#define shiftl	8
#define shiftr	12

#define HASH_FROM_TO(from, to) 				\
for (index = from; index < to; index++) {		\
	pos = random_nums[index];			\
	hash += key[pos];				\
	hash += (hash << shiftl);			\
	hash ^= (hash >> shiftr);			\
}


#define HASH_FROM_DOWN_TO(from, to) 			\
for (index = from - 1; index >= to; index--) {		\
	hash ^= (hash >> shiftr);			\
	hash ^= (hash >> (shiftr*2));			\
	hash -= (hash << shiftl);			\
	hash += (hash << (shiftl*2));			\
	pos = random_nums[index];			\
	hash -= key[pos];				\
}

/*
 * The main random sample hash function.
 */
static u32 random_sample_hash(void *addr, u32 hash_strength)
{
	u32 hash = 0xdeadbeef;
	int index, pos, loop = hash_strength;
	u32 *key = (u32 *)addr;

	if (loop > HASH_STRENGTH_FULL)
		loop = HASH_STRENGTH_FULL;

	HASH_FROM_TO(0, loop);

	if (hash_strength > HASH_STRENGTH_FULL) {
		loop = hash_strength - HASH_STRENGTH_FULL;
		HASH_FROM_TO(0, loop);
	}

	return hash;
}


/**
 * It's used when hash strength is adjusted
 *
 * @addr The page's virtual address
 * @from The original hash strength
 * @to   The hash strength changed to
 * @hash The hash value generated with "from" hash value
 *
 * return the hash value
 */
static u32 delta_hash(void *addr, int from, int to, u32 hash)
{
	u32 *key = (u32 *)addr;
	int index, pos; /* make sure they are int type */

	if (to > from) {
		if (from >= HASH_STRENGTH_FULL) {
			from -= HASH_STRENGTH_FULL;
			to -= HASH_STRENGTH_FULL;
			HASH_FROM_TO(from, to);
		} else if (to <= HASH_STRENGTH_FULL) {
			HASH_FROM_TO(from, to);
		} else {
			HASH_FROM_TO(from, HASH_STRENGTH_FULL);
			HASH_FROM_TO(0, to - HASH_STRENGTH_FULL);
		}
	} else {
		if (from <= HASH_STRENGTH_FULL) {
			HASH_FROM_DOWN_TO(from, to);
		} else if (to >= HASH_STRENGTH_FULL) {
			from -= HASH_STRENGTH_FULL;
			to -= HASH_STRENGTH_FULL;
			HASH_FROM_DOWN_TO(from, to);
		} else {
			HASH_FROM_DOWN_TO(from - HASH_STRENGTH_FULL, 0);
			HASH_FROM_DOWN_TO(HASH_STRENGTH_FULL, to);
		}
	}

	return hash;
}


static inline u32 page_hash(struct page *page, unsigned long hash_strength,
			    int cost_accounting)
{
	u32 val;
	unsigned long tmp;

	void *addr = kmap_atomic(page, KM_USER0);

	val = random_sample_hash(addr, hash_strength);
	kunmap_atomic(addr, KM_USER0);

	if (cost_accounting) {
		tmp = rshash_pos;
		/* it's sucessfully identified by random sampling
		 * hash, so increase the positive count.
		 */
		rshash_pos += (HASH_STRENGTH_FULL - hash_strength);
		BUG_ON(tmp > rshash_pos);
	}

	return val;
}

static int memcmp_pages(struct page *page1, struct page *page2,
			int cost_accounting)
{
	char *addr1, *addr2;
	int ret;

	addr1 = kmap_atomic(page1, KM_USER0);
	addr2 = kmap_atomic(page2, KM_USER1);
	ret = memcmp(addr1, addr2, PAGE_SIZE);
	kunmap_atomic(addr2, KM_USER1);
	kunmap_atomic(addr1, KM_USER0);

	if (cost_accounting)
		rshash_neg += memcmp_cost;

	return ret;
}

static inline int pages_identical(struct page *page1, struct page *page2)
{
	return !memcmp_pages(page1, page2, 0);
}

static int write_protect_page(struct vm_area_struct *vma, struct page *page,
			      pte_t *orig_pte, pte_t *old_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;
	pte_t *ptep;
	spinlock_t *ptl;
	int swapped;
	int err = -EFAULT;

	addr = page_address_in_vma(page, vma);
	if (addr == -EFAULT)
		goto out;

	ptep = page_check_address(page, mm, addr, &ptl, 0);
	if (!ptep)
		goto out;

	if (old_pte)
		*old_pte = *ptep;

	if (pte_write(*ptep)) {
		pte_t entry;

		swapped = PageSwapCache(page);
		flush_cache_page(vma, addr, page_to_pfn(page));
		/*
		 * Ok this is tricky, when get_user_pages_fast() run it doesnt
		 * take any lock, therefore the check that we are going to make
		 * with the pagecount against the mapcount is racey and
		 * O_DIRECT can happen right after the check.
		 * So we clear the pte and flush the tlb before the check
		 * this assure us that no O_DIRECT can happen after the check
		 * or in the middle of the check.
		 */
		entry = ptep_clear_flush(vma, addr, ptep);
		/*
		 * Check that no O_DIRECT or similar I/O is in progress on the
		 * page
		 */
		if (page_mapcount(page) + 1 + swapped != page_count(page)) {
			goto out_unlock;
		}

		entry = pte_wrprotect(entry);
		set_pte_at_notify(mm, addr, ptep, entry);
	}
	*orig_pte = *ptep;
	err = 0;

out_unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	return err;
}

#define MERGE_ERR_PGERR		1 /* the page is invalid cannot continue */
#define MERGE_ERR_COLLI		2 /* there is a collision */
#define MERGE_ERR_CHANGED	3 /* the page has changed since last hash */


/**
 * replace_page - replace page in vma by new ksm page
 * @vma:      vma that holds the pte pointing to page
 * @page:     the page we are replacing by kpage
 * @kpage:    the ksm page we replace page by
 * @orig_pte: the original value of the pte
 *
 * Returns 0 on success, MERGE_ERR_PGERR on failure.
 */
static int replace_page(struct vm_area_struct *vma, struct page *page,
			struct page *kpage, pte_t orig_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;
	unsigned long addr;
	int err = MERGE_ERR_PGERR;

	addr = page_address_in_vma(page, vma);
	if (addr == -EFAULT)
		goto out;

	pgd = pgd_offset(mm, addr);
	if (!pgd_present(*pgd))
		goto out;

	pud = pud_offset(pgd, addr);
	if (!pud_present(*pud))
		goto out;

	pmd = pmd_offset(pud, addr);
	if (!pmd_present(*pmd))
		goto out;

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte_same(*ptep, orig_pte)) {
		pte_unmap_unlock(ptep, ptl);
		goto out;
	}

	get_page(kpage);
	page_add_anon_rmap(kpage, vma, addr);

	flush_cache_page(vma, addr, pte_pfn(*ptep));
	ptep_clear_flush(vma, addr, ptep);
	set_pte_at_notify(mm, addr, ptep, mk_pte(kpage, vma->vm_page_prot));

	page_remove_rmap(page);
	put_page(page);

	pte_unmap_unlock(ptep, ptl);
	err = 0;
out:
	return err;
}


/**
 *  Fully hash a page with HASH_STRENGTH_MAX return a non-zero hash value. The
 *  zero hash value at HASH_STRENGTH_MAX is used to indicated that its
 *  hash_max member has not been calculated.
 *
 * @page The page needs to be hashed
 * @hash_old The hash value calculated with current hash strength
 *
 * return the new hash value calculated at HASH_STRENGTH_MAX
 */
static inline u32 page_hash_max(struct page *page, u32 hash_old)
{
	u32 hash_max = 0;
	void *addr;

	addr = kmap_atomic(page, KM_USER0);
	hash_max = delta_hash(addr, hash_strength,
			      HASH_STRENGTH_MAX, hash_old);

	kunmap_atomic(addr, KM_USER0);

	if (!hash_max)
		hash_max = 1;

	rshash_neg += (HASH_STRENGTH_MAX - hash_strength);
	return hash_max;
}

/*
 * We compare the hash again, to ensure that it is really a hash collision
 * instead of being caused by page write.
 */
static inline int check_collision(struct rmap_item *rmap_item,
				  u32 hash)
{
	int err;
	unsigned long tmp;
	struct page *page = rmap_item->page;

	/* if this rmap_item has already been hash_maxed, then the collision
	 * must appears in the second-level rbtree search. In this case we check
	 * if its hash_max value has been changed. Otherwise, the collision
	 * happens in the first-level rbtree search, so we check against it's
	 * current hash value.
	 */
	if (rmap_item->hash_max) {
		tmp = rshash_neg;
		rshash_neg += memcmp_cost;
		rshash_neg += (HASH_STRENGTH_MAX - hash_strength);
		BUG_ON(tmp > rshash_neg);

		if (rmap_item->hash_max == page_hash_max(page, hash))
			err = MERGE_ERR_COLLI;
		else
			err = MERGE_ERR_CHANGED;
	} else {
		tmp = rshash_neg;
		rshash_neg += memcmp_cost + hash_strength;
		BUG_ON(tmp > rshash_neg);

		if (page_hash(page, hash_strength, 0) == hash)
			err = MERGE_ERR_COLLI;
		else
			err = MERGE_ERR_CHANGED;
	}

	return err;
}

/**
 * Try to merge a rmap_item.page with a kpage in stable node. kpage must
 * already be a ksm page.
 *
 * @return 0 if the pages were merged, -EFAULT otherwise.
 */
static int try_to_merge_with_ksm_page(struct rmap_item *rmap_item,
				      struct page *kpage, u32 hash)
{
	struct vm_area_struct *vma = rmap_item->slot->vma;
	struct mm_struct *mm = vma->vm_mm;
	pte_t orig_pte = __pte(0);
	int err = MERGE_ERR_PGERR;
	struct page *page;

	if (ksm_test_exit(mm))
		goto out;

	page = rmap_item->page;

	if (page == kpage) { /* ksm page forked */
		err = 0;
		goto out;
	}

	if (!PageAnon(page) || !PageKsm(kpage))
		goto out;

	/*
	 * We need the page lock to read a stable PageSwapCache in
	 * write_protect_page().  We use trylock_page() instead of
	 * lock_page() because we don't want to wait here - we
	 * prefer to continue scanning and merging different pages,
	 * then come back to this page when it is unlocked.
	 */
	if (!trylock_page(page))
		goto out;
	/*
	 * If this anonymous page is mapped only here, its pte may need
	 * to be write-protected.  If it's mapped elsewhere, all of its
	 * ptes are necessarily already write-protected.  But in either
	 * case, we need to lock and check page_count is not raised.
	 */
	if (write_protect_page(vma, page, &orig_pte, NULL) == 0) {
		if (!kpage) {
			long map_sharing = atomic_read(&page->_mapcount);
			/*
			 * While we hold page lock, upgrade page from
			 * PageAnon+anon_vma to PageKsm+NULL stable_node:
			 * stable_tree_insert() will update stable_node.
			 */
			set_page_stable_node(page, NULL);
			if (map_sharing)
				add_zone_page_state(page_zone(page),
						    NR_KSM_PAGES_SHARING,
						    map_sharing);
			mark_page_accessed(page);
			err = 0;
		} else {
			if (pages_identical(page, kpage))
				err = replace_page(vma, page, kpage, orig_pte);
			else
				err = check_collision(rmap_item, hash);
		}
	}

	if ((vma->vm_flags & VM_LOCKED) && kpage && !err) {
		munlock_vma_page(page);
		if (!PageMlocked(kpage)) {
			unlock_page(page);
			lock_page(kpage);
			mlock_vma_page(kpage);
			page = kpage;		/* for final unlock */
		}
	}

	unlock_page(page);
out:
	return err;
}



/**
 * If two pages fail to merge in try_to_merge_two_pages, then we have a chance
 * to restore a page mapping that has been changed in try_to_merge_two_pages.
 *
 * @return 0 on success.
 */
static int restore_ksm_page_pte(struct vm_area_struct *vma, unsigned long addr,
			     pte_t orig_pte, pte_t wprt_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;

	int err = -EFAULT;

	pgd = pgd_offset(mm, addr);
	if (!pgd_present(*pgd))
		goto out;

	pud = pud_offset(pgd, addr);
	if (!pud_present(*pud))
		goto out;

	pmd = pmd_offset(pud, addr);
	if (!pmd_present(*pmd))
		goto out;

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte_same(*ptep, wprt_pte)) {
		/* already copied, let it be */
		pte_unmap_unlock(ptep, ptl);
		goto out;
	}

	/*
	 * Good boy, still here. When we still get the ksm page, it does not
	 * return to the free page pool, there is no way that a pte was changed
	 * to other page and gets back to this page. And remind that ksm page
	 * do not reuse in do_wp_page(). So it's safe to restore the original
	 * pte.
	 */
	flush_cache_page(vma, addr, pte_pfn(*ptep));
	ptep_clear_flush(vma, addr, ptep);
	set_pte_at_notify(mm, addr, ptep, orig_pte);

	pte_unmap_unlock(ptep, ptl);
	err = 0;
out:
	return err;
}

/**
 * try_to_merge_two_pages() - take two identical pages and prepare
 * them to be merged into one page(rmap_item->page)
 *
 * @return 0 if we successfully merged two identical pages into
 *         one ksm page. MERGE_ERR_COLLI if it's only a hash collision
 *         search in rbtree. MERGE_ERR_CHANGED if rmap_item has been
 *         changed since it's hashed. MERGE_ERR_PGERR otherwise.
 *
 */
static int try_to_merge_two_pages(struct rmap_item *rmap_item,
				  struct rmap_item *tree_rmap_item,
				  u32 hash)
{
	pte_t orig_pte1 = __pte(0), orig_pte2 = __pte(0);
	pte_t wprt_pte1 = __pte(0), wprt_pte2 = __pte(0);
	struct vm_area_struct *vma1 = rmap_item->slot->vma;
	struct vm_area_struct *vma2 = tree_rmap_item->slot->vma;
	struct page *page = rmap_item->page;
	struct page *tree_page = tree_rmap_item->page;
	int err = MERGE_ERR_PGERR;

	long map_sharing;
	struct address_space *saved_mapping;


	if (rmap_item->page == tree_rmap_item->page)
		goto out;

	if (!PageAnon(page) || !PageAnon(tree_page))
		goto out;

	if (!trylock_page(page))
		goto out;


	if (write_protect_page(vma1, page, &wprt_pte1, &orig_pte1) != 0) {
		unlock_page(page);
		goto out;
	}

	/*
	 * While we hold page lock, upgrade page from
	 * PageAnon+anon_vma to PageKsm+NULL stable_node:
	 * stable_tree_insert() will update stable_node.
	 */
	saved_mapping = page->mapping;
	map_sharing = atomic_read(&page->_mapcount);
	set_page_stable_node(page, NULL);
	if (map_sharing)
		add_zone_page_state(page_zone(page),
				    NR_KSM_PAGES_SHARING,
				    map_sharing);
	mark_page_accessed(page);
	unlock_page(page);

	if (!trylock_page(tree_page))
		goto restore_out;

	if (write_protect_page(vma2, tree_page, &wprt_pte2, &orig_pte2) != 0) {
		unlock_page(tree_page);
		goto restore_out;
	}

	if (pages_identical(page, tree_page)) {
		err = replace_page(vma2, tree_page, page, wprt_pte2);
		if (err)
			goto restore_out;

		if ((vma2->vm_flags & VM_LOCKED)) {
			munlock_vma_page(tree_page);
			if (!PageMlocked(page)) {
				unlock_page(tree_page);
				lock_page(page);
				mlock_vma_page(page);
				tree_page = page; /* for final unlock */
			}
		}

		unlock_page(tree_page);

		goto out; /* success */

	} else {
		if (page_hash(page, hash_strength, 0) ==
		    page_hash(tree_page, hash_strength, 0)) {
			rshash_neg += memcmp_cost + hash_strength * 2;
			err = MERGE_ERR_COLLI;
		} else
			err = MERGE_ERR_CHANGED;

		unlock_page(tree_page);
	}

restore_out:
	lock_page(page);
	if (!restore_ksm_page_pte(vma1, get_rmap_addr(rmap_item),
				  orig_pte1, wprt_pte1))
		page->mapping = saved_mapping;

	unlock_page(page);
out:
	return err;
}

static inline int hash_cmp(u32 new_val, u32 node_val)
{
	if (new_val > node_val)
		return 1;
	else if (new_val < node_val)
		return -1;
	else
		return 0;
}

static inline u32 rmap_item_hash_max(struct rmap_item *item, u32 hash)
{
	u32 hash_max = item->hash_max;

	if (!hash_max) {
		hash_max = page_hash_max(item->page, hash);

		item->hash_max = hash_max;
	}

	return hash_max;
}



/**
 * stable_tree_search() - search the stable tree for a page
 *
 * @item: 	the rmap_item we are comparing with
 * @hash: 	the hash value of this item->page already calculated
 *
 * @return 	the page we have found, NULL otherwise. The page returned has
 *         	been gotten.
 */
static struct page *stable_tree_search(struct rmap_item *item, u32 hash)
{
	struct rb_node *node = root_stable_treep->rb_node;
	struct tree_node *tree_node;
	unsigned long hash_max;
	struct page *page = item->page;
	struct stable_node *stable_node;

	stable_node = page_stable_node(page);
	if (stable_node) {
		/* ksm page forked, that is
		 * if (PageKsm(page) && !in_stable_tree(rmap_item))
		 * it's actually gotten once outside.
		 */
		get_page(page);
		return page;
	}

	while (node) {
		int cmp;

		tree_node = rb_entry(node, struct tree_node, node);

		cmp = hash_cmp(hash, tree_node->hash);

		if (cmp < 0)
			node = node->rb_left;
		else if (cmp > 0)
			node = node->rb_right;
		else
			break;
	}

	if (!node)
		return NULL;

	if (tree_node->count == 1) {
		stable_node = rb_entry(tree_node->sub_root.rb_node,
				       struct stable_node, node);
		BUG_ON(!stable_node);

		goto get_page_out;
	}

	/*
	 * ok, we have to search the second
	 * level subtree, hash the page to a
	 * full strength.
	 */
	node = tree_node->sub_root.rb_node;
	BUG_ON(!node);
	hash_max = rmap_item_hash_max(item, hash);

	while (node) {
		int cmp;

		stable_node = rb_entry(node, struct stable_node, node);

		cmp = hash_cmp(hash_max, stable_node->hash_max);

		if (cmp < 0)
			node = node->rb_left;
		else if (cmp > 0)
			node = node->rb_right;
		else
			goto get_page_out;
	}

	return NULL;

get_page_out:
	page = get_ksm_page(stable_node, 1, 1);
	return page;
}


/**
 * try_to_merge_with_stable_page() - when two rmap_items need to be inserted
 * into stable tree, the page was found to be identical to a stable ksm page,
 * this is the last chance we can merge them into one.
 *
 * @item1:	the rmap_item holding the page which we wanted to insert
 *       	into stable tree.
 * @item2:	the other rmap_item we found when unstable tree search
 * @oldpage:	the page currently mapped by the two rmap_items
 * @tree_page: 	the page we found identical in stable tree node
 * @success1:	return if item1 is successfully merged
 * @success2:	return if item2 is successfully merged
 */
static void try_merge_with_stable(struct rmap_item *item1,
				  struct rmap_item *item2,
				  struct page *oldpage,
				  struct page *tree_page,
				  int *success1, int *success2)
{
	spinlock_t *ptl1, *ptl2;
	pte_t *ptep1, *ptep2;
	unsigned long addr1, addr2;
	struct vm_area_struct *vma1 = item1->slot->vma;
	struct vm_area_struct *vma2 = item2->slot->vma;

	*success1 = 0;
	*success2 = 0;

	if (unlikely(oldpage == tree_page)) {
		/* I don't think this can really happen */
		goto success_both;
	}

	if (!PageAnon(oldpage) || !PageKsm(oldpage))
		goto failed;

	/* If the oldpage is still ksm and still pointed
	 * to in the right place, and still write protected,
	 * we are confident it's not changed, no need to
	 * memcmp anymore.
	 * be ware, we cannot take nested pte locks,
	 * deadlock risk.
	 */
	addr1 = get_rmap_addr(item1);

	ptep1 = page_check_address(oldpage, vma1->vm_mm, addr1, &ptl1, 0);
	if (!ptep1)
		goto failed;

	if (pte_write(*ptep1)) {
		/* has changed, abort! */
		pte_unmap_unlock(ptep1, ptl1);
		goto failed;
	}

	get_page(tree_page);
	page_add_anon_rmap(tree_page, vma1, addr1);

	flush_cache_page(vma1, addr1, pte_pfn(*ptep1));
	ptep_clear_flush(vma1, addr1, ptep1);
	set_pte_at_notify(vma1->vm_mm, addr1, ptep1,
			  mk_pte(tree_page, vma1->vm_page_prot));

	page_remove_rmap(oldpage);
	put_page(oldpage);

	pte_unmap_unlock(ptep1, ptl1);


	/* ok, then vma2, remind that pte1 already set */
	addr2 = get_rmap_addr(item2);

	ptep2 = page_check_address(oldpage, vma2->vm_mm, addr2, &ptl2, 0);
	if (!ptep2)
		goto success1;

	if (pte_write(*ptep2)) {
		/* has changed, abort! */
		pte_unmap_unlock(ptep2, ptl2);
		goto success1;
	}

	get_page(tree_page);
	page_add_anon_rmap(tree_page, vma2, addr2);

	flush_cache_page(vma2, addr2, pte_pfn(*ptep2));
	ptep_clear_flush(vma2, addr2, ptep2);
	set_pte_at_notify(vma2->vm_mm, addr2, ptep2,
			  mk_pte(tree_page, vma2->vm_page_prot));

	page_remove_rmap(oldpage);
	put_page(oldpage);

	pte_unmap_unlock(ptep2, ptl2);


success_both:
	*success2 = 1;
success1:
	*success1 = 1;

	if ((*success1 && vma1->vm_flags & VM_LOCKED) ||
	    (*success2 && vma2->vm_flags & VM_LOCKED)) {
		munlock_vma_page(oldpage);
		if (!PageMlocked(tree_page)) {

			/*ok, we do not need oldpage any more in the caller
			 * we can break the lock now.
			 */
			unlock_page(oldpage);
			lock_page(tree_page);
			mlock_vma_page(tree_page);
			unlock_page(tree_page);
			lock_page(oldpage); /* unlocked outside */
		}
	}

failed:
	return;
}

static inline void stable_node_hash_max(struct stable_node *node,
					 struct page *page, u32 hash)
{
	u32 hash_max = node->hash_max;

	if (!hash_max) {
		hash_max = page_hash_max(page, hash);
		node->hash_max = hash_max;
	}
}

static inline
struct stable_node *new_stable_node(struct tree_node *tree_node,
				    struct page *kpage, u32 hash_max)
{
	struct stable_node *new_stable_node;

	new_stable_node = alloc_stable_node();
	if (!new_stable_node)
		return NULL;

	new_stable_node->kpfn = page_to_pfn(kpage);
	new_stable_node->hash_max = hash_max;
	new_stable_node->tree_node = tree_node;
	set_page_stable_node(kpage, new_stable_node);

	return new_stable_node;
}

static inline
struct stable_node *first_level_insert(struct tree_node *tree_node,
				       struct rmap_item *rmap_item,
				       struct rmap_item *tree_rmap_item,
				       struct page *kpage, u32 hash,
				       int *success1, int *success2)
{
	int cmp;
	struct page *tree_page;
	u32 hash_max = 0;
	struct stable_node *stable_node, *new_snode;
	struct rb_node *parent = NULL, **new;

	/* this tree node contains no sub-tree yet */
	stable_node = rb_entry(tree_node->sub_root.rb_node,
			       struct stable_node, node);

	tree_page = get_ksm_page(stable_node, 1, 0);
	if (tree_page) {
		cmp = memcmp_pages(kpage, tree_page, 1);
		if (!cmp) {
			try_merge_with_stable(rmap_item, tree_rmap_item, kpage,
					      tree_page, success1, success2);
			put_page(tree_page);
			if (!*success1 && !*success2)
				goto failed;

			return stable_node;

		} else {
			/*
			 * collision in first level try to create a subtree.
			 * A new node need to be created.
			 */
			put_page(tree_page);

			stable_node_hash_max(stable_node, tree_page,
					     tree_node->hash);
			hash_max = rmap_item_hash_max(rmap_item, hash);
			cmp = hash_cmp(hash_max, stable_node->hash_max);

			parent = &stable_node->node;
			if (cmp < 0) {
				new = &parent->rb_left;
			} else if (cmp > 0) {
				new = &parent->rb_right;
			} else {
				printk(KERN_ERR "KSM collision1 "
						"hash_max=%u\n",
				       hash_max);
				goto failed;
			}
		}

	} else {
		/* the only stable_node deleted, we reuse its tree_node.
		 */
		parent = NULL;
		new = &tree_node->sub_root.rb_node;
	}

	new_snode = new_stable_node(tree_node, kpage, hash_max);
	if (!new_snode)
		goto failed;

	rb_link_node(&new_snode->node, parent, new);
	rb_insert_color(&new_snode->node, &tree_node->sub_root);
	tree_node->count++;
	*success1 = *success2 = 1;

	return new_snode;

failed:
	return NULL;
}

static inline
struct stable_node *stable_subtree_insert(struct tree_node *tree_node,
					  struct rmap_item *rmap_item,
					  struct rmap_item *tree_rmap_item,
					  struct page *kpage, u32 hash,
					  int *success1, int *success2)
{
	struct page *tree_page;
	u32 hash_max;
	struct stable_node *stable_node, *new_snode;
	struct rb_node *parent, **new;

research:
	parent = NULL;
	new = &tree_node->sub_root.rb_node;
	BUG_ON(!*new);
	hash_max = rmap_item_hash_max(rmap_item, hash);
	while (*new) {
		int cmp;

		stable_node = rb_entry(*new, struct stable_node, node);

		cmp = hash_cmp(hash_max, stable_node->hash_max);

		if (cmp < 0) {
			parent = *new;
			new = &parent->rb_left;
		} else if (cmp > 0) {
			parent = *new;
			new = &parent->rb_right;
		} else {
			tree_page = get_ksm_page(stable_node, 1, 0);
			if (tree_page) {
				cmp = memcmp_pages(kpage, tree_page, 1);
				if (!cmp) {
					try_merge_with_stable(rmap_item,
						tree_rmap_item, kpage,
						tree_page, success1, success2);

					put_page(tree_page);
					if (!*success1 && !*success2)
						goto failed;
					/*
					 * successfully merged with a stable
					 * node
					 */
					return stable_node;
				} else {
					put_page(tree_page);
					goto failed;
				}
			} else {
				/*
				 * stable node may be deleted,
				 * and subtree maybe
				 * restructed, cannot
				 * continue, research it.
				 */
				if (tree_node->count) {
					goto research;
				} else {
					/* reuse the tree node*/
					parent = NULL;
					new = &tree_node->sub_root.rb_node;
				}
			}
		}
	}

	new_snode = new_stable_node(tree_node, kpage, hash_max);
	if (!new_snode)
		goto failed;

	rb_link_node(&new_snode->node, parent, new);
	rb_insert_color(&new_snode->node, &tree_node->sub_root);
	tree_node->count++;
	*success1 = *success2 = 1;

	return new_snode;

failed:
	return NULL;
}


/**
 * stable_tree_insert() - try to insert a merged page in unstable tree to
 * the stable tree
 *
 * @kpage:		the page need to be inserted
 * @hash:		the current hash of this page
 * @rmap_item:		the rmap_item being scanned
 * @tree_rmap_item:	the rmap_item found on unstable tree
 * @success1:		return if rmap_item is merged
 * @success2:		return if tree_rmap_item is merged
 *
 * @return 		the stable_node on stable tree if at least one
 *      		rmap_item is inserted into stable tree, NULL
 *      		otherwise.
 */
static struct stable_node *
stable_tree_insert(struct page *kpage, u32 hash,
		   struct rmap_item *rmap_item,
		   struct rmap_item *tree_rmap_item,
		   int *success1, int *success2)
{
	struct rb_node **new = &root_stable_treep->rb_node;
	struct rb_node *parent = NULL;
	struct stable_node *stable_node;
	struct tree_node *tree_node;
	u32 hash_max = 0;

	*success1 = *success2 = 0;

	while (*new) {
		int cmp;

		tree_node = rb_entry(*new, struct tree_node, node);

		cmp = hash_cmp(hash, tree_node->hash);

		if (cmp < 0) {
			parent = *new;
			new = &parent->rb_left;
		} else if (cmp > 0) {
			parent = *new;
			new = &parent->rb_right;
		} else
			break;
	}

	if (*new) {
		if (tree_node->count == 1) {
			stable_node = first_level_insert(tree_node, rmap_item,
						tree_rmap_item, kpage,
						hash, success1, success2);
		} else {
			stable_node = stable_subtree_insert(tree_node,
					rmap_item, tree_rmap_item, kpage,
					hash, success1, success2);
		}
	} else {

		/* no tree node found */
		tree_node = alloc_tree_node(stable_tree_node_listp);
		if (!tree_node) {
			stable_node = NULL;
			goto out;
		}

		stable_node = new_stable_node(tree_node, kpage, hash_max);
		if (!stable_node) {
			free_tree_node(tree_node);
			goto out;
		}

		tree_node->hash = hash;
		rb_link_node(&tree_node->node, parent, new);
		rb_insert_color(&tree_node->node, root_stable_treep);
		parent = NULL;
		new = &tree_node->sub_root.rb_node;

		rb_link_node(&stable_node->node, parent, new);
		rb_insert_color(&stable_node->node, &tree_node->sub_root);
		tree_node->count++;
		*success1 = *success2 = 1;
	}

out:
	return stable_node;
}


/**
 * get_tree_rmap_item_page() - try to get the page and lock the mmap_sem
 *
 * @return 	0 on success, -EBUSY if unable to lock the mmap_sem,
 *         	-EINVAL if the page mapping has been changed.
 */
static inline int get_tree_rmap_item_page(struct rmap_item *tree_rmap_item)
{
	int err;

	err = get_mergeable_page_lock_mmap(tree_rmap_item);

	if (err == -EINVAL) {
		/* its page map has been changed, remove it */
		remove_rmap_item_from_tree(tree_rmap_item);
	}

	/* The page is gotten and mmap_sem is locked now. */
	return err;
}


/**
 * unstable_tree_search_insert() - search an unstable tree rmap_item with the
 * same hash value. Get its page and trylock the mmap_sem
 */
static inline
struct rmap_item *unstable_tree_search_insert(struct rmap_item *rmap_item,
					      u32 hash)

{
	struct rb_node **new = &root_unstable_tree.rb_node;
	struct rb_node *parent = NULL;
	struct tree_node *tree_node;
	u32 hash_max;
	struct rmap_item *tree_rmap_item;

	while (*new) {
		int cmp;

		tree_node = rb_entry(*new, struct tree_node, node);

		cmp = hash_cmp(hash, tree_node->hash);

		if (cmp < 0) {
			parent = *new;
			new = &parent->rb_left;
		} else if (cmp > 0) {
			parent = *new;
			new = &parent->rb_right;
		} else
			break;
	}

	if (*new) {
		/* got the tree_node */
		if (tree_node->count == 1) {
			tree_rmap_item = rb_entry(tree_node->sub_root.rb_node,
						  struct rmap_item, node);
			BUG_ON(!tree_rmap_item);

			goto get_page_out;
		}

		/* well, search the collision subtree */
		new = &tree_node->sub_root.rb_node;
		BUG_ON(!*new);
		hash_max = rmap_item_hash_max(rmap_item, hash);

		while (*new) {
			int cmp;

			tree_rmap_item = rb_entry(*new, struct rmap_item,
						  node);

			cmp = hash_cmp(hash_max, tree_rmap_item->hash_max);
			parent = *new;
			if (cmp < 0)
				new = &parent->rb_left;
			else if (cmp > 0)
				new = &parent->rb_right;
			else
				goto get_page_out;
		}
	} else {
		/* alloc a new tree_node */
		tree_node = alloc_tree_node(&unstable_tree_node_list);
		if (!tree_node)
			return NULL;

		tree_node->hash = hash;
		rb_link_node(&tree_node->node, parent, new);
		rb_insert_color(&tree_node->node, &root_unstable_tree);
		parent = NULL;
		new = &tree_node->sub_root.rb_node;
	}

	/* did not found even in sub-tree */
	rmap_item->tree_node = tree_node;
	rmap_item->address |= UNSTABLE_FLAG;
	rmap_item->append_round = ksm_scan_round;
	rb_link_node(&rmap_item->node, parent, new);
	rb_insert_color(&rmap_item->node, &tree_node->sub_root);

	ksm_pages_unshared++;
	return NULL;

get_page_out:
	if (tree_rmap_item->page == rmap_item->page)
		return NULL;

	if (get_tree_rmap_item_page(tree_rmap_item))
		return NULL;

	return tree_rmap_item;
}

static void enter_inter_vma_table(struct vma_slot *slot)
{
	unsigned int i;

	for (i = 0; i <= ksm_vma_table_size; i++) {
		if (!ksm_vma_table[i])
			break;
	}
	BUG_ON(ksm_vma_table[i]);
	slot->ksm_index = i;
	ksm_vma_table[i] = slot;
	ksm_vma_table_num++;

	BUG_ON(i > ksm_vma_table_index_end);
	if (i == ksm_vma_table_index_end)
		ksm_vma_table_index_end++;

	BUG_ON(ksm_vma_table_index_end > ksm_vma_table_size - 1);
}

static inline unsigned int intertab_vma_offset(int i, int j)
{
	int swap;
	if (i < j) {
		swap = i;
		i = j;
		j = swap;
	}

	return i * (i+1) / 2 + j;
}

static inline void inc_vma_intertab_pair(struct vma_slot *slot1,
					 struct vma_slot *slot2)
{
	unsigned long offset;

	if (slot1->ksm_index == -1)
		enter_inter_vma_table(slot1);

	if (slot2->ksm_index == -1)
		enter_inter_vma_table(slot2);

	offset = intertab_vma_offset(slot1->ksm_index, slot2->ksm_index);
	ksm_inter_vma_table[offset]++;
	BUG_ON(!ksm_inter_vma_table[offset]);
}

static inline void dec_vma_intertab_pair(struct vma_slot *slot1,
					 struct vma_slot *slot2)
{
	unsigned long offset;
	BUG_ON(slot1->ksm_index == -1 || slot2->ksm_index == -1);

	offset = intertab_vma_offset(slot1->ksm_index, slot2->ksm_index);
	BUG_ON(!ksm_inter_vma_table[offset]);
	ksm_inter_vma_table[offset]--;
}

static void hold_anon_vma(struct rmap_item *rmap_item,
			  struct anon_vma *anon_vma)
{
	rmap_item->anon_vma = anon_vma;
	atomic_inc(&anon_vma->external_refcount);
}


/**
 * stable_tree_append() - append a rmap_item to a stable node. Deduplication
 * ratio statistics is done in this function.
 *
 */
static void stable_tree_append(struct rmap_item *rmap_item,
			       struct stable_node *stable_node)
{
	struct node_vma *node_vma = NULL, *new_node_vma, *node_vma_iter = NULL;
	struct hlist_node *hlist, *cont_p = NULL;
	unsigned long key = (unsigned long)rmap_item->slot;

	BUG_ON(!stable_node);
	rmap_item->address |= STABLE_FLAG;
	rmap_item->append_round = ksm_scan_round;

	if (hlist_empty(&stable_node->hlist)) {
		ksm_pages_shared++;
		goto node_vma_new;
	} else {
		ksm_pages_sharing++;
	}

	hlist_for_each_entry(node_vma, hlist, &stable_node->hlist, hlist) {
		if (node_vma->last_update == ksm_scan_round)
			inc_vma_intertab_pair(rmap_item->slot, node_vma->slot);

		if (node_vma->key >= key)
			break;
	}

	cont_p = hlist;

	if (node_vma && node_vma->key == key) {
		if (node_vma->last_update == ksm_scan_round) {
			/**
			 * we consider this page a inner duplicate, cancel
			 * other updates
			 */
			hlist_for_each_entry(node_vma_iter, hlist,
					     &stable_node->hlist, hlist) {
				if (node_vma_iter->key == key)
					break;

				/* only need to increase the same vma */
				if (node_vma_iter->last_update ==
				    ksm_scan_round) {
					dec_vma_intertab_pair(rmap_item->slot,
							  node_vma_iter->slot);
				}
			}
		} else {
			/**
			 * Although it's same vma, it contains no duplicate for this
			 * round. Continue scan other vma.
			 */
			hlist_for_each_entry_continue(node_vma_iter,
						      hlist, hlist) {
				if (node_vma_iter->last_update ==
				    ksm_scan_round) {
					inc_vma_intertab_pair(rmap_item->slot,
							  node_vma_iter->slot);
				}
			}

		}

		goto node_vma_ok;
	}

node_vma_new:
	/* no same vma already in node, alloc a new node_vma */
	new_node_vma = alloc_node_vma();
	BUG_ON(!new_node_vma);
	new_node_vma->head = stable_node;
	new_node_vma->slot = rmap_item->slot;

	if (!node_vma) {
		hlist_add_head(&new_node_vma->hlist, &stable_node->hlist);
	} else if (node_vma->key != key) {
		if (node_vma->key < key)
			hlist_add_after(&node_vma->hlist, &new_node_vma->hlist);
		else {
			hlist_for_each_entry_continue(node_vma_iter, cont_p,
						      hlist) {
				if (node_vma_iter->last_update ==
				    ksm_scan_round) {
					inc_vma_intertab_pair(rmap_item->slot,
							  node_vma_iter->slot);
				}
			}
			hlist_add_before(&new_node_vma->hlist,
					 &node_vma->hlist);
		}

	}
	node_vma = new_node_vma;

node_vma_ok: /* ok, ready to add to the list */
	rmap_item->head = node_vma;
	hlist_add_head(&rmap_item->hlist, &node_vma->rmap_hlist);
	node_vma->last_update = ksm_scan_round;
	hold_anon_vma(rmap_item, rmap_item->slot->vma->anon_vma);
	rmap_item->slot->pages_merged++;
}

/*
 * We use break_ksm to break COW on a ksm page: it's a stripped down
 *
 *	if (get_user_pages(current, mm, addr, 1, 1, 1, &page, NULL) == 1)
 *		put_page(page);
 *
 * but taking great care only to touch a ksm page, in a VM_MERGEABLE vma,
 * in case the application has unmapped and remapped mm,addr meanwhile.
 * Could a ksm page appear anywhere else?  Actually yes, in a VM_PFNMAP
 * mmap of /dev/mem or /dev/kmem, where we would not want to touch it.
 */
static int break_ksm(struct vm_area_struct *vma, unsigned long addr)
{
	struct page *page;
	int ret = 0;

	do {
		cond_resched();
		page = follow_page(vma, addr, FOLL_GET);
		if (IS_ERR_OR_NULL(page))
			break;
		if (PageKsm(page)) {
			ret = handle_mm_fault(vma->vm_mm, vma, addr,
					      FAULT_FLAG_WRITE);
		} else
			ret = VM_FAULT_WRITE;
		put_page(page);
	} while (!(ret & (VM_FAULT_WRITE | VM_FAULT_SIGBUS | VM_FAULT_OOM)));
	/*
	 * We must loop because handle_mm_fault() may back out if there's
	 * any difficulty e.g. if pte accessed bit gets updated concurrently.
	 *
	 * VM_FAULT_WRITE is what we have been hoping for: it indicates that
	 * COW has been broken, even if the vma does not permit VM_WRITE;
	 * but note that a concurrent fault might break PageKsm for us.
	 *
	 * VM_FAULT_SIGBUS could occur if we race with truncation of the
	 * backing file, which also invalidates anonymous pages: that's
	 * okay, that truncation will have unmapped the PageKsm for us.
	 *
	 * VM_FAULT_OOM: at the time of writing (late July 2009), setting
	 * aside mem_cgroup limits, VM_FAULT_OOM would only be set if the
	 * current task has TIF_MEMDIE set, and will be OOM killed on return
	 * to user; and ksmd, having no mm, would never be chosen for that.
	 *
	 * But if the mm is in a limited mem_cgroup, then the fault may fail
	 * with VM_FAULT_OOM even if the current task is not TIF_MEMDIE; and
	 * even ksmd can fail in this way - though it's usually breaking ksm
	 * just to undo a merge it made a moment before, so unlikely to oom.
	 *
	 * That's a pity: we might therefore have more kernel pages allocated
	 * than we're counting as nodes in the stable tree; but ksm_do_scan
	 * will retry to break_cow on each pass, so should recover the page
	 * in due course.  The important thing is to not let VM_MERGEABLE
	 * be cleared while any such pages might remain in the area.
	 */
	return (ret & VM_FAULT_OOM) ? -ENOMEM : 0;
}

static void break_cow(struct rmap_item *rmap_item)
{
	struct vm_area_struct *vma = rmap_item->slot->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr = get_rmap_addr(rmap_item);

	if (ksm_test_exit(mm))
		goto out;

	break_ksm(vma, addr);
out:
	return;
}

/*
 * Though it's very tempting to unmerge in_stable_tree(rmap_item)s rather
 * than check every pte of a given vma, the locking doesn't quite work for
 * that - an rmap_item is assigned to the stable tree after inserting ksm
 * page and upping mmap_sem.  Nor does it fit with the way we skip dup'ing
 * rmap_items from parent to child at fork time (so as not to waste time
 * if exit comes before the next scan reaches it).
 *
 * Similarly, although we'd like to remove rmap_items (so updating counts
 * and freeing memory) when unmerging an area, it's easier to leave that
 * to the next pass of ksmd - consider, for example, how ksmd might be
 * in cmp_and_merge_page on one of the rmap_items we would be removing.
 */
inline int unmerge_ksm_pages(struct vm_area_struct *vma,
		      unsigned long start, unsigned long end)
{
	unsigned long addr;
	int err = 0;

	for (addr = start; addr < end && !err; addr += PAGE_SIZE) {
		if (ksm_test_exit(vma->vm_mm))
			break;
		if (signal_pending(current))
			err = -ERESTARTSYS;
		else
			err = break_ksm(vma, addr);
	}
	return err;
}

/*
 * cmp_and_merge_page() - first see if page can be merged into the stable
 * tree; if not, compare hash to previous and if it's the same, see if page
 * can be inserted into the unstable tree, or merged with a page already there
 * and both transferred to the stable tree.
 *
 * @page: the page that we are searching identical page to.
 * @rmap_item: the reverse mapping into the virtual address of this page
 */
static void cmp_and_merge_page(struct rmap_item *rmap_item)
{
	struct rmap_item *tree_rmap_item;
	struct page *page;
	struct page *kpage = NULL;
	u32 hash, hash_max;
	int err;
	unsigned int success1, success2;
	struct stable_node *snode;
	int cmp;
	struct rb_node *parent = NULL, **new;

	remove_rmap_item_from_tree(rmap_item);

	page = rmap_item->page;
	hash = page_hash(page, hash_strength, 1);

	ksm_pages_scanned++;

	/* We first start with searching the page inside the stable tree */
	kpage = stable_tree_search(rmap_item, hash);
	if (kpage) {
		err = try_to_merge_with_ksm_page(rmap_item, kpage,
						 hash);
		if (!err) {
			/*
			 * The page was successfully merged, add
			 * its rmap_item to the stable tree.
			 * page lock is needed because it's
			 * racing with try_to_unmap_ksm(), etc.
			 */
			lock_page(kpage);
			stable_tree_append(rmap_item, page_stable_node(kpage));
			unlock_page(kpage);
			put_page(kpage);
			return; /* success */
		}
		put_page(kpage);

		/*
		 * if it's a collision and it has been search in sub-rbtree
		 * (hash_max != 0), we want to abort, because if it is
		 * successfully merged in unstable tree, the collision trends to
		 * happen again.
		 */
		if (err == MERGE_ERR_COLLI && rmap_item->hash_max)
			return;
	}

	tree_rmap_item =
		unstable_tree_search_insert(rmap_item, hash);
	if (tree_rmap_item) {
		err = try_to_merge_two_pages(rmap_item, tree_rmap_item, hash);
		/*
		 * As soon as we merge this page, we want to remove the
		 * rmap_item of the page we have merged with from the unstable
		 * tree, and insert it instead as new node in the stable tree.
		 */
		if (!err) {
			kpage = page;
			remove_rmap_item_from_tree(tree_rmap_item);
			lock_page(kpage);
			snode = stable_tree_insert(kpage, hash,
						   rmap_item, tree_rmap_item,
						   &success1, &success2);

			if (success1)
				stable_tree_append(rmap_item, snode);
			else
				break_cow(rmap_item);

			if (success2)
				stable_tree_append(tree_rmap_item, snode);
			else
				break_cow(tree_rmap_item);

			unlock_page(kpage);

		} else if (err == MERGE_ERR_COLLI) {
			if (tree_rmap_item->tree_node->count == 1) {
				rmap_item_hash_max(tree_rmap_item,
				tree_rmap_item->tree_node->hash);
			} else
				BUG_ON(!(tree_rmap_item->hash_max));

			hash_max = rmap_item_hash_max(rmap_item, hash);
			cmp = hash_cmp(hash_max, tree_rmap_item->hash_max);
			parent = &tree_rmap_item->node;
			if (cmp < 0)
				new = &parent->rb_left;
			else if (cmp > 0)
				new = &parent->rb_right;
			else
				goto put_up_out;

			rmap_item->tree_node = tree_rmap_item->tree_node;
			rmap_item->address |= UNSTABLE_FLAG;
			rmap_item->append_round = ksm_scan_round;
			rb_link_node(&rmap_item->node, parent, new);
			rb_insert_color(&rmap_item->node,
					&tree_rmap_item->tree_node->sub_root);
		}
put_up_out:
		put_page(tree_rmap_item->page);
		up_read(&tree_rmap_item->slot->vma->vm_mm->mmap_sem);
	}
}




static inline unsigned long get_pool_index(struct vma_slot *slot,
					   unsigned long index)
{
	unsigned long pool_index;

	pool_index = (sizeof(struct rmap_list_entry *) * index) >> PAGE_SHIFT;
	if (pool_index >= slot->pool_size)
		BUG();
	return pool_index;
}

static inline unsigned long index_page_offset(unsigned long index)
{
	return offset_in_page(sizeof(struct rmap_list_entry *) * index);
}

static inline
struct rmap_list_entry *get_rmap_list_entry(struct vma_slot *slot,
					    unsigned long index, int need_alloc)
{
	unsigned long pool_index;
	void *addr;


	pool_index = get_pool_index(slot, index);
	if (!slot->rmap_list_pool[pool_index]) {
		if (!need_alloc)
			return NULL;

		slot->rmap_list_pool[pool_index] =
			alloc_page(GFP_KERNEL | __GFP_ZERO);
		BUG_ON(!slot->rmap_list_pool[pool_index]);
	}

	addr = kmap(slot->rmap_list_pool[pool_index]);
	addr += index_page_offset(index);

	return addr;
}

static inline void put_rmap_list_entry(struct vma_slot *slot,
				       unsigned long index)
{
	unsigned long pool_index;

	pool_index = get_pool_index(slot, index);
	BUG_ON(!slot->rmap_list_pool[pool_index]);
	kunmap(slot->rmap_list_pool[pool_index]);
}

static inline int entry_is_new(struct rmap_list_entry *entry)
{
	return !entry->item;
}

static inline unsigned long get_index_orig_addr(struct vma_slot *slot,
						unsigned long index)
{
	return slot->vma->vm_start + (index << PAGE_SHIFT);
}

static inline unsigned long get_entry_address(struct rmap_list_entry *entry)
{
	unsigned long addr;

	if (is_addr(entry->addr))
		addr = get_clean_addr(entry->addr);
	else if (entry->item)
		addr = get_rmap_addr(entry->item);
	else
		BUG();

	return addr;
}

static inline struct rmap_item *get_entry_item(struct rmap_list_entry *entry)
{
	if (is_addr(entry->addr))
		return NULL;

	return entry->item;
}

static inline void inc_rmap_list_pool_count(struct vma_slot *slot,
					    unsigned long index)
{
	unsigned long pool_index;

	pool_index = get_pool_index(slot, index);
	BUG_ON(!slot->rmap_list_pool[pool_index]);
	slot->pool_counts[pool_index]++;
}

static inline void dec_rmap_list_pool_count(struct vma_slot *slot,
					    unsigned long index)
{
	unsigned long pool_index;

	pool_index = get_pool_index(slot, index);
	BUG_ON(!slot->rmap_list_pool[pool_index]);
	BUG_ON(!slot->pool_counts[pool_index]);
	slot->pool_counts[pool_index]--;
}

static inline int entry_has_rmap(struct rmap_list_entry *entry)
{
	return !is_addr(entry->addr) && entry->item;
}

static inline void swap_entries(struct rmap_list_entry *entry1,
				unsigned long index1,
				struct rmap_list_entry *entry2,
				unsigned long index2)
{
	struct rmap_list_entry tmp;

	/* swapping two new entries is meaningless */
	BUG_ON(entry_is_new(entry1) && entry_is_new(entry2));

	tmp = *entry1;
	*entry1 = *entry2;
	*entry2 = tmp;

	if (entry_has_rmap(entry1))
		entry1->item->entry_index = index1;

	if (entry_has_rmap(entry2))
		entry2->item->entry_index = index2;

	if (entry_has_rmap(entry1) && !entry_has_rmap(entry2)) {
		inc_rmap_list_pool_count(entry1->item->slot, index1);
		dec_rmap_list_pool_count(entry1->item->slot, index2);
	} else if (!entry_has_rmap(entry1) && entry_has_rmap(entry2)) {
		inc_rmap_list_pool_count(entry2->item->slot, index2);
		dec_rmap_list_pool_count(entry2->item->slot, index1);
	}
}

static inline void free_entry_item(struct rmap_list_entry *entry)
{
	unsigned long index;
	struct rmap_item *item;

	if (!is_addr(entry->addr)) {
		BUG_ON(!entry->item);
		item = entry->item;
		entry->addr = get_rmap_addr(item);
		set_is_addr(entry->addr);
		index = item->entry_index;
		remove_rmap_item_from_tree(item);
		dec_rmap_list_pool_count(item->slot, index);
		free_rmap_item(item);
	}
}

static inline int pool_entry_boundary(unsigned long index)
{
	unsigned long linear_addr;

	linear_addr = sizeof(struct rmap_list_entry *) * index;
	return index && !offset_in_page(linear_addr);
}

static inline void try_free_last_pool(struct vma_slot *slot,
				      unsigned long index)
{
	unsigned long pool_index;

	pool_index = get_pool_index(slot, index);
	if (slot->rmap_list_pool[pool_index] &&
	    !slot->pool_counts[pool_index]) {
		__free_page(slot->rmap_list_pool[pool_index]);
		slot->rmap_list_pool[pool_index] = NULL;
		slot->need_sort = 1;
	}

}

static inline unsigned long vma_item_index(struct vm_area_struct *vma,
					   struct rmap_item *item)
{
	return (get_rmap_addr(item) - vma->vm_start) >> PAGE_SHIFT;
}

static int within_same_pool(struct vma_slot *slot,
			    unsigned long i, unsigned long j)
{
	unsigned long pool_i, pool_j;

	pool_i = get_pool_index(slot, i);
	pool_j = get_pool_index(slot, j);

	return (pool_i == pool_j);
}

static void sort_rmap_entry_list(struct vma_slot *slot)
{
	unsigned long i, j;
	struct rmap_list_entry *entry, *swap_entry;

	entry = get_rmap_list_entry(slot, 0, 0);
	for (i = 0; i < slot->pages; ) {

		if (!entry)
			goto skip_whole_pool;

		if (entry_is_new(entry))
			goto next_entry;

		if (is_addr(entry->addr)) {
			entry->addr = 0;
			goto next_entry;
		}

		j = vma_item_index(slot->vma, entry->item);
		if (j == i)
			goto next_entry;

		if (within_same_pool(slot, i, j))
			swap_entry = entry + j - i;
		else
			swap_entry = get_rmap_list_entry(slot, j, 1);

		swap_entries(entry, i, swap_entry, j);
		if (!within_same_pool(slot, i, j))
			put_rmap_list_entry(slot, j);
		continue;

skip_whole_pool:
		i += PAGE_SIZE / sizeof(*entry);
		if (i < slot->pages)
			entry = get_rmap_list_entry(slot, i, 0);
		continue;

next_entry:
		if (i >= slot->pages - 1 ||
		    !within_same_pool(slot, i, i + 1)) {
			put_rmap_list_entry(slot, i);
			if (i + 1 < slot->pages)
				entry = get_rmap_list_entry(slot, i + 1, 0);
		} else
			entry++;
		i++;
		continue;
	}

	/* free empty pool entries which contain no rmap_item */
	/* CAN be simplied to based on only pool_counts when bug freed !!!!! */
	for (i = 0; i < slot->pool_size; i++) {
		unsigned char has_rmap;
		void *addr;

		if (!slot->rmap_list_pool[i])
			continue;

		has_rmap = 0;
		addr = kmap(slot->rmap_list_pool[i]);
		BUG_ON(!addr);
		for (j = 0; j < PAGE_SIZE / sizeof(*entry); j++) {
			entry = (struct rmap_list_entry *)addr + j;
			if (is_addr(entry->addr))
				continue;
			if (!entry->item)
				continue;
			has_rmap = 1;
		}
		kunmap(slot->rmap_list_pool[i]);
		if (!has_rmap) {
			BUG_ON(slot->pool_counts[i]);
			__free_page(slot->rmap_list_pool[i]);
			slot->rmap_list_pool[i] = NULL;
		}
	}

	slot->need_sort = 0;
}

/*
 * vma_fully_scanned() - if all the pages in this slot have been scanned.
 */
static inline int vma_fully_scanned(struct vma_slot *slot)
{
	return slot->pages_scanned && !(slot->pages_scanned % slot->pages);
}

/**
 * get_next_rmap_item() - Get the next rmap_item in a vma_slot according to
 * its random permutation. This function is embedded with the random
 * permutation index management code.
 */
static struct rmap_item *get_next_rmap_item(struct vma_slot *slot)
{
	unsigned long rand_range, addr, swap_index, scan_index;
	struct rmap_item *item = NULL;
	struct rmap_list_entry *scan_entry, *swap_entry = NULL;
	struct page *page;

	scan_index = swap_index = slot->pages_scanned % slot->pages;

	if (pool_entry_boundary(scan_index))
		try_free_last_pool(slot, scan_index - 1);

	if (vma_fully_scanned(slot)) {
		slot->need_rerand = slot->need_sort;
		if (slot->need_sort)
			sort_rmap_entry_list(slot);
	}

	scan_entry = get_rmap_list_entry(slot, scan_index, 1);
	if (entry_is_new(scan_entry)) {
		scan_entry->addr = get_index_orig_addr(slot, scan_index);
		set_is_addr(scan_entry->addr);
	}

	if (slot->need_rerand) {
		rand_range = slot->pages - scan_index;
		BUG_ON(!rand_range);
		swap_index = scan_index + (random32() % rand_range);
	}

	if (swap_index != scan_index) {
		swap_entry = get_rmap_list_entry(slot, swap_index, 1);
		if (entry_is_new(swap_entry)) {
			swap_entry->addr = get_index_orig_addr(slot,
							       swap_index);
			set_is_addr(swap_entry->addr);
		}
		swap_entries(scan_entry, scan_index, swap_entry, swap_index);
	}

	addr = get_entry_address(scan_entry);
	item = get_entry_item(scan_entry);
	BUG_ON(addr > slot->vma->vm_end || addr < slot->vma->vm_start);

	page = follow_page(slot->vma, addr, FOLL_GET);
	if (IS_ERR_OR_NULL(page))
		goto nopage;

	if (!PageAnon(page))
		goto putpage;

	flush_anon_page(slot->vma, page, addr);
	flush_dcache_page(page);

	if (!item) {
		item = alloc_rmap_item();
		if (item) {
			/* It has already been zeroed */
			item->slot = slot;
			item->address = addr;
			item->entry_index = scan_index;
			scan_entry->item = item;
			inc_rmap_list_pool_count(slot, scan_index);
		} else
			goto putpage;
	}

	BUG_ON(item->slot != slot);
	/* the page may have changed */
	item->page = page;
	put_rmap_list_entry(slot, scan_index);
	if (swap_entry)
		put_rmap_list_entry(slot, swap_index);
	return item;

putpage:
	put_page(page);
	page = NULL;
nopage:
	/* no page, store addr back and free rmap_item if possible */
	free_entry_item(scan_entry);
	put_rmap_list_entry(slot, scan_index);
	if (swap_entry)
		put_rmap_list_entry(slot, swap_index);
	return NULL;
}

static inline int in_stable_tree(struct rmap_item *rmap_item)
{
	return rmap_item->address & STABLE_FLAG;
}

/**
 * scan_vma_one_page() - scan the next page in a vma_slot. Called with
 * mmap_sem locked.
 */
static void scan_vma_one_page(struct vma_slot *slot)
{
	struct mm_struct *mm;
	struct rmap_item *rmap_item = NULL;
	struct vm_area_struct *vma = slot->vma;

	mm = vma->vm_mm;
	BUG_ON(!mm);
	BUG_ON(!slot);

	rmap_item = get_next_rmap_item(slot);
	if (!rmap_item)
		goto out1;

	if (PageKsm(rmap_item->page) && in_stable_tree(rmap_item))
		goto out2;

	cmp_and_merge_page(rmap_item);
out2:
	put_page(rmap_item->page);
out1:
	slot->pages_scanned++;
	slot->slot_scanned = 1;
	if (vma_fully_scanned(slot)) {
		slot->fully_scanned = 1;
		slot->rung->fully_scanned_slots++;
		BUG_ON(!slot->rung->fully_scanned_slots);
	}
}

static unsigned long get_vma_random_scan_num(struct vma_slot *slot,
					     unsigned long scan_ratio)
{
	return slot->pages * scan_ratio / KSM_SCAN_RATIO_MAX;
}

static inline void vma_rung_enter(struct vma_slot *slot,
				  struct scan_rung *rung)
{
	unsigned long pages_to_scan;
	struct scan_rung *old_rung = slot->rung;

	/* leave the old rung it was in */
	BUG_ON(list_empty(&slot->ksm_list));

	if (old_rung->current_scan == &slot->ksm_list)
		old_rung->current_scan = slot->ksm_list.next;
	list_del_init(&slot->ksm_list);
	old_rung->vma_num--;
	if (slot->fully_scanned)
		old_rung->fully_scanned_slots--;

	if (old_rung->current_scan == &old_rung->vma_list) {
		/* This rung finishes a round */
		old_rung->round_finished = 1;
		old_rung->current_scan = old_rung->vma_list.next;
		BUG_ON(old_rung->current_scan == &old_rung->vma_list &&
		       !list_empty(&old_rung->vma_list));
	}

	/* enter the new rung */
	while (!(pages_to_scan =
		get_vma_random_scan_num(slot, rung->scan_ratio))) {
		rung++;
		BUG_ON(rung > &ksm_scan_ladder[ksm_scan_ladder_size - 1]);
	}
	if (list_empty(&rung->vma_list))
		rung->current_scan = &slot->ksm_list;
	list_add(&slot->ksm_list, &rung->vma_list);
	slot->rung = rung;
	slot->pages_to_scan = pages_to_scan;
	slot->rung->vma_num++;
	if (slot->fully_scanned)
		rung->fully_scanned_slots++;

	BUG_ON(rung->current_scan == &rung->vma_list &&
	       !list_empty(&rung->vma_list));
}

static inline void vma_rung_up(struct vma_slot *slot)
{
	if (slot->rung == &ksm_scan_ladder[ksm_scan_ladder_size-1])
		return;

	vma_rung_enter(slot, slot->rung + 1);
}

static inline void vma_rung_down(struct vma_slot *slot)
{
	if (slot->rung == &ksm_scan_ladder[0])
		return;

	vma_rung_enter(slot, slot->rung - 1);
}

/**
 * cal_dedup_ratio() - Calculate the deduplication ratio for this slot.
 */
static inline unsigned long cal_dedup_ratio(struct vma_slot *slot)
{
	int i;
	unsigned long dedup_num = 0, pages1 = slot->pages, scanned1;
	unsigned int index;
	unsigned long ret;

	if (!slot->pages_scanned)
		return 0;

	scanned1 = slot->pages_scanned - slot->last_scanned;
	BUG_ON(scanned1 > slot->pages_scanned);

	for (i = 0; i < ksm_vma_table_index_end; i++) {
		struct vma_slot *slot2 = ksm_vma_table[i];
		unsigned long pages2, scanned2;

		if (!slot2 || i == slot->ksm_index || !slot2->pages_scanned)
			continue;

		pages2 = slot2->pages;

		scanned2 = slot2->pages_scanned - slot2->last_scanned;
		BUG_ON(scanned2 > slot2->pages_scanned);

		index = intertab_vma_offset(slot->ksm_index, i);
		BUG_ON(ksm_inter_vma_table[index] && (!scanned1 || !scanned2));
		if (ksm_inter_vma_table[index]) {
			dedup_num += ksm_inter_vma_table[index] *
				     pages1 / scanned1 * pages2 / scanned2;
		}
	}

	index = intertab_vma_offset(slot->ksm_index, slot->ksm_index);
	BUG_ON(ksm_inter_vma_table[index] && !scanned1);
	if (ksm_inter_vma_table[index])
		dedup_num += ksm_inter_vma_table[index] * pages1 / scanned1;

	ret = (dedup_num * KSM_DEDUP_RATIO_SCALE / pages1);

	/* Thrashing area filtering */
	if (ksm_thrash_threshold) {
		if (slot->pages_cowed * 100 / slot->pages_merged
		    > ksm_thrash_threshold) {
			ret = 0;
		} else {
			ret = ret * (slot->pages_merged - slot->pages_cowed)
			      / slot->pages_merged;
		}
	}

	return ret;
}


/**
 * stable_node_reinsert() - When the hash_strength has been adjusted, the
 * stable tree need to be restructured, this is the function re-inserting the
 * stable node.
 */
static inline void stable_node_reinsert(struct stable_node *new_node,
					struct page *page,
					struct rb_root *root_treep,
					struct list_head *tree_node_listp,
					u32 hash)
{
	struct rb_node **new = &root_treep->rb_node;
	struct rb_node *parent = NULL;
	struct stable_node *stable_node;
	struct tree_node *tree_node;
	struct page *tree_page;
	int cmp;

	while (*new) {
		int cmp;

		tree_node = rb_entry(*new, struct tree_node, node);

		cmp = hash_cmp(hash, tree_node->hash);

		if (cmp < 0) {
			parent = *new;
			new = &parent->rb_left;
		} else if (cmp > 0) {
			parent = *new;
			new = &parent->rb_right;
		} else
			break;
	}

	if (*new) {
		/* find a stable tree node with same first level hash value */
		stable_node_hash_max(new_node, page, hash);
		if (tree_node->count == 1) {
			stable_node = rb_entry(tree_node->sub_root.rb_node,
					       struct stable_node, node);
			tree_page = get_ksm_page(stable_node, 1, 0);
			if (tree_page) {
				stable_node_hash_max(stable_node,
						      tree_page, hash);
				put_page(tree_page);

				/* prepare for stable node insertion */

				cmp = hash_cmp(new_node->hash_max,
						   stable_node->hash_max);
				parent = &stable_node->node;
				if (cmp < 0)
					new = &parent->rb_left;
				else if (cmp > 0)
					new = &parent->rb_right;
				else
					goto failed;

				goto add_node;
			} else {
				/* the only stable_node deleted, the tree node
				 * was not deleted.
				 */
				goto tree_node_reuse;
			}
		}

		/* well, search the collision subtree */
		new = &tree_node->sub_root.rb_node;
		parent = NULL;
		BUG_ON(!*new);
		while (*new) {
			int cmp;

			stable_node = rb_entry(*new, struct stable_node, node);

			cmp = hash_cmp(new_node->hash_max,
					   stable_node->hash_max);

			if (cmp < 0) {
				parent = *new;
				new = &parent->rb_left;
			} else if (cmp > 0) {
				parent = *new;
				new = &parent->rb_right;
			} else {
				/* oh, no, still a collision */
				goto failed;
			}
		}

		goto add_node;
	}

	/* no tree node found */
	tree_node = alloc_tree_node(tree_node_listp);
	if (!tree_node) {
		printk(KERN_ERR "KSM: memory allocation error!\n");
		goto failed;
	} else {
		tree_node->hash = hash;
		rb_link_node(&tree_node->node, parent, new);
		rb_insert_color(&tree_node->node, root_treep);

tree_node_reuse:
		/* prepare for stable node insertion */
		parent = NULL;
		new = &tree_node->sub_root.rb_node;
	}

add_node:
	rb_link_node(&new_node->node, parent, new);
	rb_insert_color(&new_node->node, &tree_node->sub_root);
	new_node->tree_node = tree_node;
	tree_node->count++;
	return;

failed:
	/* This can only happen when two nodes have collided
	 * in two levels.
	 */
	new_node->tree_node = NULL;
	return;
}

static inline void free_all_tree_nodes(struct list_head *list)
{
	struct tree_node *node, *tmp;

	list_for_each_entry_safe(node, tmp, list, all_list) {
		free_tree_node(node);
	}
}

/**
 * stable_tree_delta_hash() - Delta hash the stable tree from previous hash
 * strength to the current hash_strength. It re-structures the hole tree.
 */
static inline void stable_tree_delta_hash(u32 prev_hash_strength)
{
	struct stable_node *node, *tmp;
	struct rb_root *root_new_treep;
	struct list_head *new_tree_node_listp;

	stable_tree_index = (stable_tree_index + 1) % 2;
	root_new_treep = &root_stable_tree[stable_tree_index];
	new_tree_node_listp = &stable_tree_node_list[stable_tree_index];
	*root_new_treep = RB_ROOT;
	BUG_ON(!list_empty(new_tree_node_listp));

	/*
	 * we need to be safe, the node could be removed by get_ksm_page()
	 */
	list_for_each_entry_safe(node, tmp, &stable_node_list, all_list) {
		void *addr;
		struct page *node_page;
		u32 hash;

		/*
		 * We are completely re-structuring the stable nodes to a new
		 * stable tree. We don't want to touch the old tree unlinks and
		 * old tree_nodes. The old tree_nodes will be freed at once.
		 */
		node_page = get_ksm_page(node, 0, 0);
		if (!node_page)
			continue;

		if (node->tree_node) {
			hash = node->tree_node->hash;

			addr = kmap_atomic(node_page, KM_USER0);

			hash = delta_hash(addr, prev_hash_strength,
					  hash_strength, hash);
			kunmap_atomic(addr, KM_USER0);
		} else {
			/*
			 *it was not inserted to rbtree due to collision in last
			 *round scan.
			 */
			hash = page_hash(node_page, hash_strength, 0);
		}

		stable_node_reinsert(node, node_page, root_new_treep,
				     new_tree_node_listp, hash);
		put_page(node_page);
	}

	root_stable_treep = root_new_treep;
	free_all_tree_nodes(stable_tree_node_listp);
	BUG_ON(!list_empty(stable_tree_node_listp));
	stable_tree_node_listp = new_tree_node_listp;
}

static inline void inc_hash_strength(unsigned long delta)
{
	hash_strength += 1 << delta;
	if (hash_strength > HASH_STRENGTH_MAX)
		hash_strength = HASH_STRENGTH_MAX;
}

static inline void dec_hash_strength(unsigned long delta)
{
	unsigned long change = 1 << delta;

	if (hash_strength <= change + 1)
		hash_strength = 1;
	else
		hash_strength -= change;
}

static inline void inc_hash_strength_delta(void)
{
	hash_strength_delta++;
	if (hash_strength_delta > HASH_STRENGTH_DELTA_MAX)
		hash_strength_delta = HASH_STRENGTH_DELTA_MAX;
}

static inline unsigned long get_current_neg_ratio(void)
{
	if (!rshash_pos || rshash_neg > rshash_pos)
		return 100;

	return div64_u64(100 * rshash_neg , rshash_pos);
}

static inline u64 get_current_benefit(void)
{
	if (rshash_neg > rshash_pos)
		return 0;

	return div64_u64((rshash_pos - rshash_neg),
			 ksm_pages_scanned - ksm_pages_scanned_last);
}

static inline int judge_rshash_direction(void)
{
	u64 current_neg_ratio, stable_benefit;
	u64 current_benefit, delta = 0;
	int ret;

	current_neg_ratio = get_current_neg_ratio();

	if (current_neg_ratio == 0) {
		rshash_neg_cont_zero++;
		if (rshash_neg_cont_zero > 2)
			return GO_DOWN;
		else
			return STILL;
	}
	rshash_neg_cont_zero = 0;

	if (current_neg_ratio > 90) {
		ret = GO_UP;
		goto out;
	}

	/* In case the system are still for a long time. */
	if (ksm_scan_round % 1024 == 3) {
		ret = OBSCURE;
		goto out;
	}


	current_benefit = get_current_benefit();
	stable_benefit = rshash_state.stable_benefit;

	if (!stable_benefit) {
		ret = OBSCURE;
		goto out;
	}

	if (current_benefit > stable_benefit)
		delta = current_benefit - stable_benefit;
	else if (current_benefit < stable_benefit)
		delta = stable_benefit - current_benefit;

	delta = div64_u64(100 * delta , stable_benefit);

	if (delta > 50) {
		rshash_cont_obscure++;
		if (rshash_cont_obscure > 2)
			return OBSCURE;
		else
			return STILL;
	}

out:
	rshash_cont_obscure = 0;
	return STILL;
}

/**
 * rshash_adjust() - The main function to control the random sampling state
 * machine for hash strength adapting.
 */
static inline void rshash_adjust(void)
{
	unsigned long prev_hash_strength = hash_strength;

	if (ksm_pages_scanned == ksm_pages_scanned_last)
		return;

	switch (rshash_state.state) {
	case RSHASH_STILL:
		switch (judge_rshash_direction()) {
		case GO_UP:
			if (rshash_state.pre_direct == GO_DOWN)
				hash_strength_delta = 0;

			inc_hash_strength(hash_strength_delta);
			inc_hash_strength_delta();
			rshash_state.stable_benefit = get_current_benefit();
			rshash_state.pre_direct = GO_UP;
			break;

		case GO_DOWN:
			if (rshash_state.pre_direct == GO_UP)
				hash_strength_delta = 0;

			dec_hash_strength(hash_strength_delta);
			inc_hash_strength_delta();
			rshash_state.stable_benefit = get_current_benefit();
			rshash_state.pre_direct = GO_DOWN;
			break;

		case OBSCURE:
			rshash_state.stable_point = hash_strength;
			rshash_state.turn_point_down = hash_strength;
			rshash_state.turn_point_up = hash_strength;
			rshash_state.turn_benefit_down = get_current_benefit();
			rshash_state.turn_benefit_up = get_current_benefit();
			rshash_state.lookup_window_index = 0;
			rshash_state.state = RSHASH_TRYDOWN;
			dec_hash_strength(hash_strength_delta);
			inc_hash_strength_delta();

			break;

		case STILL:
			break;
		default:
			BUG();
		}
		break;

	case RSHASH_TRYDOWN:
		if (rshash_state.lookup_window_index++ % 5 == 0)
			rshash_state.below_count = 0;

		if (get_current_benefit() < rshash_state.stable_benefit)
			rshash_state.below_count++;
		else if (get_current_benefit() >
			 rshash_state.turn_benefit_down) {
			rshash_state.turn_point_down = hash_strength;
			rshash_state.turn_benefit_down = get_current_benefit();
		}

		if (rshash_state.below_count >= 3 ||
		    judge_rshash_direction() == GO_UP) {
			hash_strength = rshash_state.stable_point;
			hash_strength_delta = 0;
			inc_hash_strength(hash_strength_delta);
			inc_hash_strength_delta();
			rshash_state.lookup_window_index = 0;
			rshash_state.state = RSHASH_TRYUP;
			hash_strength_delta = 0;
		} else {
			dec_hash_strength(hash_strength_delta);
			inc_hash_strength_delta();
		}
		break;

	case RSHASH_TRYUP:
		if (rshash_state.lookup_window_index++ % 5 == 0)
			rshash_state.below_count = 0;

		if (get_current_benefit() < rshash_state.stable_benefit)
			rshash_state.below_count++;
		else if (get_current_benefit() > rshash_state.turn_benefit_up) {
			rshash_state.turn_point_up = hash_strength;
			rshash_state.turn_benefit_up = get_current_benefit();
		}

		if (rshash_state.below_count >= 3 ||
		    judge_rshash_direction() == GO_DOWN) {
			hash_strength = rshash_state.turn_benefit_up >
				rshash_state.turn_benefit_down ?
				rshash_state.turn_point_up :
				rshash_state.turn_point_down;

			rshash_state.state = RSHASH_PRE_STILL;
		} else {
			inc_hash_strength(hash_strength_delta);
			inc_hash_strength_delta();
		}

		break;

	case RSHASH_NEW:
	case RSHASH_PRE_STILL:
		rshash_state.stable_benefit = get_current_benefit();
		rshash_state.state = RSHASH_STILL;
		hash_strength_delta = 0;
		break;
	default:
		BUG();
	}

	rshash_neg = rshash_pos = 0;

	if (prev_hash_strength != hash_strength)
		stable_tree_delta_hash(prev_hash_strength);
}

static void ksm_intertab_clear(struct vma_slot *slot)
{
	int i;
	unsigned index;

	for (i = 0; i <= slot->ksm_index; i++) {
		index = intertab_vma_offset(slot->ksm_index, i);
		ksm_inter_vma_table[index] = 0;
	}

	for (i = slot->ksm_index + 1; i < ksm_vma_table_index_end; i++) {
		index = intertab_vma_offset(slot->ksm_index, i);
		ksm_inter_vma_table[index] = 0;
	}
}


/**
 * round_update_ladder() - The main function to do update of all the
 * adjustments whenever a scan round is finished.
 */
static void round_update_ladder(void)
{
	int i;
	struct vma_slot *slot, *tmp_slot;
	unsigned long dedup_ratio_max = 0, dedup_ratio_mean = 0;
	unsigned long threshold;
	struct list_head tmp_list;

	for (i = 0; i < ksm_vma_table_index_end; i++) {
		slot = ksm_vma_table[i];

		if (slot) {
			slot->dedup_ratio = cal_dedup_ratio(slot);
			if (dedup_ratio_max < slot->dedup_ratio)
				dedup_ratio_max = slot->dedup_ratio;
			dedup_ratio_mean += slot->dedup_ratio;
		}
	}

	dedup_ratio_mean /= ksm_vma_slot_num;
	threshold = dedup_ratio_mean;

	for (i = 0; i < ksm_vma_table_index_end; i++) {
		slot = ksm_vma_table[i];

		if (slot) {
			if (slot->dedup_ratio  &&
			    slot->dedup_ratio >= threshold) {
				vma_rung_up(slot);
			} else {
				vma_rung_down(slot);
			}

			ksm_intertab_clear(slot);
			ksm_vma_table_num--;
			ksm_vma_table[i] = NULL;
			slot->ksm_index = -1;
			slot->slot_scanned = 0;
			slot->dedup_ratio = 0;
		}
	}

	INIT_LIST_HEAD(&tmp_list);
	for (i = 0; i < ksm_scan_ladder_size; i++) {
		list_for_each_entry_safe(slot, tmp_slot,
					 &ksm_scan_ladder[i].vma_list,
					 ksm_list) {
			/*
			 * The slots were scanned but not in inter_tab, their
			 * dedup must be 0.
			 */
			if (slot->slot_scanned) {
				BUG_ON(slot->dedup_ratio != 0);
				vma_rung_down(slot);
			}

			slot->dedup_ratio = 0;
		}
	}

	BUG_ON(ksm_vma_table_num != 0);
	ksm_vma_table_index_end = 0;

	for (i = 0; i < ksm_scan_ladder_size; i++) {
		ksm_scan_ladder[i].round_finished = 0;

		list_for_each_entry(slot, &ksm_scan_ladder[i].vma_list,
				    ksm_list) {
			slot->last_scanned = slot->pages_scanned;
			slot->slot_scanned = 0;
			slot->pages_cowed = 0;
			slot->pages_merged = 0;
			if (slot->fully_scanned) {
				slot->fully_scanned = 0;
				ksm_scan_ladder[i].fully_scanned_slots--;
			}
			BUG_ON(slot->ksm_index != -1);
		}

		BUG_ON(ksm_scan_ladder[i].fully_scanned_slots);
	}

	rshash_adjust();

	ksm_pages_scanned_last = ksm_pages_scanned;
}

static inline unsigned int ksm_pages_to_scan(unsigned int batch_pages)
{
	return totalram_pages * batch_pages / 1000000;
}

static inline void cal_ladder_pages_to_scan(unsigned int num)
{
	int i;

	for (i = 0; i < ksm_scan_ladder_size; i++) {
		ksm_scan_ladder[i].pages_to_scan = num
			* ksm_scan_ladder[i].scan_ratio / KSM_SCAN_RATIO_MAX;
	}
	ksm_scan_ladder[0].pages_to_scan /= 16;
	ksm_scan_ladder[1].pages_to_scan /= 4;
}

static inline void ksm_del_vma_slot(struct vma_slot *slot)
{
	int i, j;
	struct rmap_list_entry *entry;

	/* mutex lock contention maybe intensive, other idea ? */
	BUG_ON(list_empty(&slot->ksm_list) || !slot->rung);

	if (slot->rung->current_scan == &slot->ksm_list)
		slot->rung->current_scan = slot->rung->current_scan->next;

	list_del_init(&slot->ksm_list);
	slot->rung->vma_num--;
	if (slot->fully_scanned)
		slot->rung->fully_scanned_slots--;

	if (slot->rung->current_scan == &slot->rung->vma_list) {
		/* This rung finishes a round */
		slot->rung->round_finished = 1;
		slot->rung->current_scan = slot->rung->vma_list.next;
		BUG_ON(slot->rung->current_scan == &slot->rung->vma_list
		       && !list_empty(&slot->rung->vma_list));
	}

	for (i = 0; i < ksm_vma_table_index_end; i++) {
		if ((slot == ksm_vma_table[i])) {
			ksm_intertab_clear(slot);
			ksm_vma_table_num--;
			ksm_vma_table[i] = NULL;
			if (i == ksm_vma_table_index_end - 1)
				ksm_vma_table_index_end--;
			break;
		}
	}

	if (!slot->rmap_list_pool)
		goto out;

	for (i = 0; i < slot->pool_size; i++) {
		void *addr;

		if (!slot->rmap_list_pool[i])
			continue;

		addr = kmap(slot->rmap_list_pool[i]);
		BUG_ON(!addr);
		for (j = 0; j < PAGE_SIZE / sizeof(*entry); j++) {
			entry = (struct rmap_list_entry *)addr + j;
			if (is_addr(entry->addr))
				continue;
			if (!entry->item)
				continue;

			remove_rmap_item_from_tree(entry->item);
			free_rmap_item(entry->item);
			slot->pool_counts[i]--;
		}
		BUG_ON(slot->pool_counts[i]);
		kunmap(slot->rmap_list_pool[i]);
		__free_page(slot->rmap_list_pool[i]);
	}
	kfree(slot->rmap_list_pool);
	kfree(slot->pool_counts);

out:
	slot->rung = NULL;
	free_vma_slot(slot);
	BUG_ON(!ksm_vma_slot_num);
	ksm_vma_slot_num--;
}


static inline void cleanup_vma_slots(void)
{
	struct vma_slot *slot;

	spin_lock(&vma_slot_list_lock);
	while (!list_empty(&vma_slot_del)) {
		slot = list_entry(vma_slot_del.next,
				  struct vma_slot, slot_list);
		list_del(&slot->slot_list);
		spin_unlock(&vma_slot_list_lock);
		ksm_del_vma_slot(slot);
		spin_lock(&vma_slot_list_lock);
	}
	spin_unlock(&vma_slot_list_lock);
}

static inline int rung_fully_scanned(struct scan_rung *rung)
{
	return (rung->fully_scanned_slots == rung->vma_num &&
		rung->fully_scanned_slots);
}

/**
 * ksm_do_scan()  - the main worker function.
 */
static void ksm_do_scan(void)
{
	struct vma_slot *slot, *iter;
	struct list_head *next_scan, *iter_head;
	struct mm_struct *busy_mm;
	unsigned char round_finished, all_rungs_emtpy;
	int i, err;
	unsigned long rest_pages;

	might_sleep();

	rest_pages = 0;

repeat_all:
	for (i = ksm_scan_ladder_size - 1; i >= 0; i--) {
		struct scan_rung *rung = &ksm_scan_ladder[i];

		if (!rung->pages_to_scan)
			continue;

		if (list_empty(&rung->vma_list)) {
			rung->pages_to_scan = 0;
			continue;
		}

		/*
		 * if a higher rung is fully scanned, its rest pages should be
		 * propagated to the lower rungs. This can prevent the higher
		 * rung from waiting a long time while it still has its
		 * pages_to_scan quota.
		 *
		 */
		if (rung_fully_scanned(rung)) {
			rest_pages += rung->pages_to_scan;
			rung->pages_to_scan = 0;
			continue;
		}

		rung->pages_to_scan += rest_pages;
		rest_pages = 0;
		while (rung->pages_to_scan) {
			rung->pages_to_scan--;

cleanup:
			cleanup_vma_slots();

			if (list_empty(&rung->vma_list))
				break;

rescan:
			BUG_ON(rung->current_scan == &rung->vma_list &&
			       !list_empty(&rung->vma_list));

			slot = list_entry(rung->current_scan,
					 struct vma_slot, ksm_list);

			err = try_down_read_slot_mmap_sem(slot);
			if (err == -ENOENT)
				goto cleanup;

			busy_mm = slot->mm;

busy:
			if (err == -EBUSY) {
				/* skip other vmas on the same mm */
				iter = slot;
				iter_head = slot->ksm_list.next;

				while (iter_head != &rung->vma_list) {
					iter = list_entry(iter_head,
							  struct vma_slot,
							  ksm_list);
					if (iter->vma->vm_mm != busy_mm)
						break;
					iter_head = iter_head->next;
				}

				if (iter->vma->vm_mm != busy_mm) {
					rung->current_scan = &iter->ksm_list;
					goto rescan;
				} else {
					/* This is the only vma on this rung */
					break;
				}
			}

			BUG_ON(!vma_can_enter(slot->vma));
			if (ksm_test_exit(slot->vma->vm_mm)) {
				busy_mm = slot->vma->vm_mm;
				up_read(&slot->vma->vm_mm->mmap_sem);
				err = -EBUSY;
				goto busy;
			}


			/* Ok, we have take the mmap_sem, ready to scan */
			if (!slot->fully_scanned)
				scan_vma_one_page(slot);
			up_read(&slot->vma->vm_mm->mmap_sem);

			if ((slot->pages_scanned &&
			     slot->pages_scanned % slot->pages_to_scan == 0)
			    || slot->fully_scanned) {
				next_scan = rung->current_scan->next;
				if (next_scan == &rung->vma_list) {
					/*
					 * All the slots in this rung
					 * have been traveled in this
					 * round.
					 */
					rung->round_finished = 1;
					rung->current_scan =
						rung->vma_list.next;
					if (rung_fully_scanned(rung)) {
						/*
						 * All the pages in all slots
						 * have been scanned.
						 */
						rest_pages +=
							rung->pages_to_scan;
						rung->pages_to_scan = 0;
						break;
					}
				} else {
					rung->current_scan = next_scan;
				}
			}

			cond_resched();
		}
	}

	round_finished = 1;
	all_rungs_emtpy = 1;
	for (i = 0; i < ksm_scan_ladder_size; i++) {
		struct scan_rung *rung = &ksm_scan_ladder[i];

		if (!list_empty(&rung->vma_list)) {
			all_rungs_emtpy = 0;
			if (!rung->round_finished)
				round_finished = 0;
			break;
		}
	}

	if (all_rungs_emtpy)
		round_finished = 0;

	cleanup_vma_slots();

	if (round_finished) {
		round_update_ladder();

		/* sync with ksm_remove_vma for rb_erase */
		ksm_scan_round++;
		root_unstable_tree = RB_ROOT;
		free_all_tree_nodes(&unstable_tree_node_list);
	}

	for (i = 0; i < ksm_scan_ladder_size; i++) {
		struct scan_rung *rung = &ksm_scan_ladder[i];

		/*
		 * Before we can go sleep, we should make sure that all the
		 * pages_to_scan quota for this scan has been finished
		 */
		if (!list_empty(&rung->vma_list) && rung->pages_to_scan)
			goto repeat_all;
	}

	cal_ladder_pages_to_scan(ksm_scan_batch_pages);
}

static int ksmd_should_run(void)
{
	return ksm_run & KSM_RUN_MERGE;
}

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)

static inline unsigned long vma_pool_size(struct vm_area_struct *vma)
{
	return round_up(sizeof(struct rmap_list_entry) * vma_pages(vma),
			PAGE_SIZE) >> PAGE_SHIFT;
}

/**
 *
 *
 *
 * @param slot
 *
 * @return int , 1 on success, 0 on failure
 */
static int ksm_vma_enter(struct vma_slot *slot)
{
	struct scan_rung *rung;
	unsigned long pages_to_scan, pool_size;

	BUG_ON(slot->pages != vma_pages(slot->vma));
	rung = &ksm_scan_ladder[0];

	pages_to_scan = get_vma_random_scan_num(slot, rung->scan_ratio);
	if (pages_to_scan) {
		if (list_empty(&rung->vma_list))
			rung->current_scan = &slot->ksm_list;
		BUG_ON(!list_empty(&slot->ksm_list));

		list_add(&slot->ksm_list, &rung->vma_list);
		slot->rung = rung;
		slot->pages_to_scan = pages_to_scan;
		slot->rung->vma_num++;
		BUG_ON(PAGE_SIZE % sizeof(struct rmap_list_entry) != 0);

		pool_size = vma_pool_size(slot->vma);

		slot->rmap_list_pool = kzalloc(sizeof(struct page *) *
					       pool_size, GFP_NOWAIT);
		slot->pool_counts = kzalloc(sizeof(unsigned long) * pool_size,
					    GFP_NOWAIT);
		slot->pool_size = pool_size;
		if (!slot->rmap_list_pool)
			goto failed;

		if (!slot->pool_counts) {
			kfree(slot->rmap_list_pool);
			goto failed;
		}

		BUG_ON(rung->current_scan == &rung->vma_list &&
		       !list_empty(&rung->vma_list));

		ksm_vma_slot_num++;
		BUG_ON(!ksm_vma_slot_num);
		return 1;
	}

failed:
	return 0;
}


static void ksm_enter_all_slots(void)
{
	struct vma_slot *slot;
	int added;

	spin_lock(&vma_slot_list_lock);
	while (!list_empty(&vma_slot_new)) {
		slot = list_entry(vma_slot_new.next,
				  struct vma_slot, slot_list);
		/**
		 * slots are sorted by ctime_j, if one found to be too
		 * young, just stop scanning the rest ones.
		 */
		/*

			if (time_before(jiffies, slot->ctime_j +
					msecs_to_jiffies(1000))) {
				spin_unlock(&vma_slot_list_lock);
				return;
			}
		*/

		list_del_init(&slot->slot_list);
		added = 0;
		if (vma_can_enter(slot->vma))
			added = ksm_vma_enter(slot);

		if (!added) {
			/* Put back to new list to be del by its creator */
			slot->ctime_j = jiffies;
			list_del(&slot->slot_list);
			list_add_tail(&slot->slot_list, &vma_slot_noadd);
		}
		spin_unlock(&vma_slot_list_lock);
		cond_resched();
		spin_lock(&vma_slot_list_lock);
	}
	spin_unlock(&vma_slot_list_lock);
}

static int ksm_scan_thread(void *nothing)
{
	set_user_nice(current, 5);

	while (!kthread_should_stop()) {
		mutex_lock(&ksm_thread_mutex);
		if (ksmd_should_run()) {
			ksm_enter_all_slots();
			ksm_do_scan();
		}
		mutex_unlock(&ksm_thread_mutex);

		if (ksmd_should_run()) {
			schedule_timeout_interruptible(ksm_sleep_jiffies);
			ksm_sleep_times++;
		} else {
			wait_event_interruptible(ksm_thread_wait,
				ksmd_should_run() || kthread_should_stop());
		}
	}
	return 0;
}

struct page *ksm_does_need_to_copy(struct page *page,
			struct vm_area_struct *vma, unsigned long address)
{
	struct page *new_page;

	unlock_page(page);	/* any racers will COW it, not modify it */

	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
	if (new_page) {
		copy_user_highpage(new_page, page, address, vma);

		SetPageDirty(new_page);
		__SetPageUptodate(new_page);
		SetPageSwapBacked(new_page);
		__set_page_locked(new_page);

		if (page_evictable(new_page, vma))
			lru_cache_add_lru(new_page, LRU_ACTIVE_ANON);
		else
			add_page_to_unevictable_list(new_page);
	}

	page_cache_release(page);
	return new_page;
}

int page_referenced_ksm(struct page *page, struct mem_cgroup *memcg,
			unsigned long *vm_flags)
{
	struct stable_node *stable_node;
	struct node_vma *node_vma;
	struct rmap_item *rmap_item;
	struct hlist_node *hlist, *rmap_hlist;
	unsigned int mapcount = page_mapcount(page);
	int referenced = 0;
	int search_new_forks = 0;
	unsigned long address;

	VM_BUG_ON(!PageKsm(page));
	VM_BUG_ON(!PageLocked(page));

	stable_node = page_stable_node(page);
	if (!stable_node)
		return 0;


again:
	hlist_for_each_entry(node_vma, hlist, &stable_node->hlist, hlist) {
		hlist_for_each_entry(rmap_item, rmap_hlist,
				     &node_vma->rmap_hlist, hlist) {
			struct anon_vma *anon_vma = rmap_item->anon_vma;
			struct anon_vma_chain *vmac;
			struct vm_area_struct *vma;

			anon_vma_lock(anon_vma);
			list_for_each_entry(vmac, &anon_vma->head,
					    same_anon_vma) {
				vma = vmac->vma;
				address = get_rmap_addr(rmap_item);

				if (address < vma->vm_start ||
				    address >= vma->vm_end)
					continue;
				/*
				 * Initially we examine only the vma which
				 * covers this rmap_item; but later, if there
				 * is still work to do, we examine covering
				 * vmas in other mms: in case they were forked
				 * from the original since ksmd passed.
				 */
				if ((rmap_item->slot->vma == vma) ==
				    search_new_forks)
					continue;

				if (memcg &&
				    !mm_match_cgroup(vma->vm_mm, memcg))
					continue;

				referenced +=
					page_referenced_one(page, vma,
						address, &mapcount, vm_flags);
				if (!search_new_forks || !mapcount)
					break;
			}

			anon_vma_unlock(anon_vma);
			if (!mapcount)
				goto out;
		}
	}
	if (!search_new_forks++)
		goto again;
out:
	return referenced;
}

int try_to_unmap_ksm(struct page *page, enum ttu_flags flags)
{
	struct stable_node *stable_node;
	struct node_vma *node_vma;
	struct hlist_node *hlist, *rmap_hlist;
	struct rmap_item *rmap_item;
	int ret = SWAP_AGAIN;
	int search_new_forks = 0;
	unsigned long address;

	VM_BUG_ON(!PageKsm(page));
	VM_BUG_ON(!PageLocked(page));

	stable_node = page_stable_node(page);
	if (!stable_node)
		return SWAP_FAIL;
again:
	hlist_for_each_entry(node_vma, hlist, &stable_node->hlist, hlist) {
		hlist_for_each_entry(rmap_item, rmap_hlist,
				     &node_vma->rmap_hlist, hlist) {
			struct anon_vma *anon_vma = rmap_item->anon_vma;
			struct anon_vma_chain *vmac;
			struct vm_area_struct *vma;

			anon_vma_lock(anon_vma);
			list_for_each_entry(vmac, &anon_vma->head,
					    same_anon_vma) {
				vma = vmac->vma;
				address = get_rmap_addr(rmap_item);

				if (address < vma->vm_start ||
				    address >= vma->vm_end)
					continue;
				/*
				 * Initially we examine only the vma which
				 * covers this rmap_item; but later, if there
				 * is still work to do, we examine covering
				 * vmas in other mms: in case they were forked
				 * from the original since ksmd passed.
				 */
				if ((rmap_item->slot->vma == vma) ==
				    search_new_forks)
					continue;

				ret = try_to_unmap_one(page, vma,
						       address, flags);
				if (ret != SWAP_AGAIN || !page_mapped(page)) {
					anon_vma_unlock(anon_vma);
					goto out;
				}
			}
			anon_vma_unlock(anon_vma);
		}
	}
	if (!search_new_forks++)
		goto again;
out:
	return ret;
}

#ifdef CONFIG_MIGRATION
int rmap_walk_ksm(struct page *page, int (*rmap_one)(struct page *,
		  struct vm_area_struct *, unsigned long, void *), void *arg)
{
	struct stable_node *stable_node;
	struct node_vma *node_vma;
	struct hlist_node *hlist, *rmap_hlist;
	struct rmap_item *rmap_item;
	int ret = SWAP_AGAIN;
	int search_new_forks = 0;
	unsigned long address;

	VM_BUG_ON(!PageKsm(page));
	VM_BUG_ON(!PageLocked(page));

	stable_node = page_stable_node(page);
	if (!stable_node)
		return ret;
again:
	hlist_for_each_entry(node_vma, hlist, &stable_node->hlist, hlist) {
		hlist_for_each_entry(rmap_item, rmap_hlist,
				     &node_vma->rmap_hlist, hlist) {
			struct anon_vma *anon_vma = rmap_item->anon_vma;
			struct anon_vma_chain *vmac;
			struct vm_area_struct *vma;

			anon_vma_lock(anon_vma);
			list_for_each_entry(vmac, &anon_vma->head,
					    same_anon_vma) {
				vma = vmac->vma;
				address = get_rmap_addr(rmap_item);

				if (address < vma->vm_start ||
				    address >= vma->vm_end)
					continue;

				if ((rmap_item->slot->vma == vma) ==
				    search_new_forks)
					continue;

				ret = rmap_one(page, vma, address, arg);
				if (ret != SWAP_AGAIN) {
					anon_vma_unlock(anon_vma);
					goto out;
				}
			}
			anon_vma_unlock(anon_vma);
		}
	}
	if (!search_new_forks++)
		goto again;
out:
	return ret;
}

void ksm_migrate_page(struct page *newpage, struct page *oldpage)
{
	struct stable_node *stable_node;

	VM_BUG_ON(!PageLocked(oldpage));
	VM_BUG_ON(!PageLocked(newpage));
	VM_BUG_ON(newpage->mapping != oldpage->mapping);

	stable_node = page_stable_node(newpage);
	if (stable_node) {
		VM_BUG_ON(stable_node->kpfn != page_to_pfn(oldpage));
		stable_node->kpfn = page_to_pfn(newpage);
	}
}
#endif /* CONFIG_MIGRATION */

#ifdef CONFIG_MEMORY_HOTREMOVE
static struct stable_node *ksm_check_stable_tree(unsigned long start_pfn,
						 unsigned long end_pfn)
{
	struct rb_node *node;

	for (node = rb_first(root_stable_treep); node; node = rb_next(node)) {
		struct stable_node *stable_node;

		stable_node = rb_entry(node, struct stable_node, node);
		if (stable_node->kpfn >= start_pfn &&
		    stable_node->kpfn < end_pfn)
			return stable_node;
	}
	return NULL;
}

static int ksm_memory_callback(struct notifier_block *self,
			       unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	struct stable_node *stable_node;

	switch (action) {
	case MEM_GOING_OFFLINE:
		/*
		 * Keep it very simple for now: just lock out ksmd and
		 * MADV_UNMERGEABLE while any memory is going offline.
		 */
		mutex_lock(&ksm_thread_mutex);
		break;

	case MEM_OFFLINE:
		/*
		 * Most of the work is done by page migration; but there might
		 * be a few stable_nodes left over, still pointing to struct
		 * pages which have been offlined: prune those from the tree.
		 */
		while ((stable_node = ksm_check_stable_tree(mn->start_pfn,
					mn->start_pfn + mn->nr_pages)) != NULL)
			remove_node_from_stable_tree(stable_node, 1, 1);
		/* fallthrough */

	case MEM_CANCEL_OFFLINE:
		mutex_unlock(&ksm_thread_mutex);
		break;
	}
	return NOTIFY_OK;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

#ifdef CONFIG_SYSFS
/*
 * This all compiles without CONFIG_SYSFS, but is a waste of space.
 */

#define KSM_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)
#define KSM_ATTR(_name) \
	static struct kobj_attribute _name##_attr = \
		__ATTR(_name, 0644, _name##_show, _name##_store)

static ssize_t sleep_millisecs_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", jiffies_to_msecs(ksm_sleep_jiffies));
}

static ssize_t sleep_millisecs_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long msecs;
	int err;

	err = strict_strtoul(buf, 10, &msecs);
	if (err || msecs > UINT_MAX)
		return -EINVAL;

	ksm_sleep_jiffies = msecs_to_jiffies(msecs);
	printk(KERN_INFO "KSM: sleep interval changed to %u jiffies\n",
	       ksm_sleep_jiffies);

	return count;
}
KSM_ATTR(sleep_millisecs);

static ssize_t min_scan_ratio_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_min_scan_ratio);
}

static ssize_t min_scan_ratio_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long msr;
	int err;

	err = strict_strtoul(buf, 10, &msr);
	if (err || msr > UINT_MAX)
		return -EINVAL;

	ksm_min_scan_ratio = msr;

	return count;
}
KSM_ATTR(min_scan_ratio);

static ssize_t scan_batch_pages_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_scan_batch_pages);
}

static ssize_t scan_batch_pages_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long batch_pages;

	err = strict_strtoul(buf, 10, &batch_pages);
	if (err || batch_pages > UINT_MAX)
		return -EINVAL;

	ksm_scan_batch_pages = batch_pages;
	cal_ladder_pages_to_scan(ksm_scan_batch_pages);

	return count;
}
KSM_ATTR(scan_batch_pages);

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", ksm_run);
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int err;
	unsigned long flags;

	err = strict_strtoul(buf, 10, &flags);
	if (err || flags > UINT_MAX)
		return -EINVAL;
	if (flags > KSM_RUN_MERGE)
		return -EINVAL;

	mutex_lock(&ksm_thread_mutex);
	if (ksm_run != flags) {
		ksm_run = flags;
	}
	mutex_unlock(&ksm_thread_mutex);

	if (flags & KSM_RUN_MERGE)
		wake_up_interruptible(&ksm_thread_wait);

	return count;
}
KSM_ATTR(run);


static ssize_t thrash_threshold_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_thrash_threshold);
}

static ssize_t thrash_threshold_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int err;
	unsigned long flags;

	err = strict_strtoul(buf, 10, &flags);
	if (err || flags > 99)
		return -EINVAL;

	ksm_thrash_threshold = flags;

	return count;
}
KSM_ATTR(thrash_threshold);

static ssize_t pages_shared_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_shared);
}
KSM_ATTR_RO(pages_shared);

static ssize_t pages_sharing_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_sharing);
}
KSM_ATTR_RO(pages_sharing);

static ssize_t pages_unshared_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_unshared);
}
KSM_ATTR_RO(pages_unshared);

static ssize_t full_scans_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", ksm_scan_round);
}
KSM_ATTR_RO(full_scans);

static ssize_t pages_scanned_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
       return sprintf(buf, "%llu\n", ksm_pages_scanned);
}
KSM_ATTR_RO(pages_scanned);

static ssize_t hash_strength_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", hash_strength);
}
KSM_ATTR_RO(hash_strength);

static ssize_t sleep_times_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", ksm_sleep_times);
}
KSM_ATTR_RO(sleep_times);


static struct attribute *ksm_attrs[] = {
	&sleep_millisecs_attr.attr,
	&scan_batch_pages_attr.attr,
	&run_attr.attr,
	&pages_shared_attr.attr,
	&pages_sharing_attr.attr,
	&pages_unshared_attr.attr,
	&full_scans_attr.attr,
	&min_scan_ratio_attr.attr,
	&pages_scanned_attr.attr,
	&hash_strength_attr.attr,
	&sleep_times_attr.attr,
	&thrash_threshold_attr.attr,
	NULL,
};

static struct attribute_group ksm_attr_group = {
	.attrs = ksm_attrs,
	.name = "ksm",
};
#endif /* CONFIG_SYSFS */

static inline void init_scan_ladder(void)
{
	int i;
	unsigned long mul = 1;

	unsigned long pages_to_scan;

	pages_to_scan = ksm_scan_batch_pages;

	for (i = 0; i < ksm_scan_ladder_size; i++,
	      mul *= ksm_scan_ratio_delta) {

		ksm_scan_ladder[i].scan_ratio = ksm_min_scan_ratio * mul;
		INIT_LIST_HEAD(&ksm_scan_ladder[i].vma_list);
		ksm_scan_ladder[i].vma_num = 0;
		ksm_scan_ladder[i].round_finished = 0;
		ksm_scan_ladder[i].fully_scanned_slots = 0;
	}

	cal_ladder_pages_to_scan(ksm_scan_batch_pages);
}

static inline int cal_positive_negative_costs(void)
{
	struct page *p1, *p2;
	unsigned char *addr1, *addr2;
	unsigned long i, time_start, hash_cost, rshash_cost_unit;
	unsigned long loopnum = 0;
	u32 hash;

	p1 = alloc_page(GFP_KERNEL);
	if (!p1)
		return -ENOMEM;

	p2 = alloc_page(GFP_KERNEL);
	if (!p2)
		return -ENOMEM;

	addr1 = kmap_atomic(p1, KM_USER0);
	addr2 = kmap_atomic(p2, KM_USER1);
	memset(addr1, random32(), PAGE_SIZE);
	memcpy(addr2, addr1, PAGE_SIZE);

	/* make sure that the two pages differ in last byte */
	addr2[PAGE_SIZE-1] = ~addr2[PAGE_SIZE-1];
	kunmap_atomic(addr2, KM_USER1);
	kunmap_atomic(addr1, KM_USER0);

	time_start = jiffies;
	while (jiffies - time_start < HASH_STRENGTH_FULL / 10) {
		for (i = 0; i < 100; i++)
			hash = page_hash(p1, HASH_STRENGTH_FULL, 0);
		loopnum += 100;
	}
	hash_cost = 100 * (jiffies - time_start);
	rshash_cost_unit = hash_cost / HASH_STRENGTH_FULL;

	time_start = jiffies;
	for (i = 0; i < loopnum; i++)
		pages_identical(p1, p2);
	memcmp_cost = 100 * (jiffies - time_start);
	memcmp_cost /= rshash_cost_unit;
	printk(KERN_INFO "KSM: relative memcmp_cost = %lu.\n", memcmp_cost);

	__free_page(p1);
	__free_page(p2);
	return 0;
}

static inline int init_random_sampling(void)
{
	unsigned long i;
	random_nums = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!random_nums)
		return -ENOMEM;

	for (i = 0; i < HASH_STRENGTH_FULL; i++)
		random_nums[i] = i;

	for (i = 0; i < HASH_STRENGTH_FULL; i++) {
		unsigned long rand_range, swap_index, tmp;

		rand_range = HASH_STRENGTH_FULL - i;
		swap_index = random32() % rand_range;
		tmp = random_nums[i];
		random_nums[i] =  random_nums[swap_index];
		random_nums[swap_index] = tmp;
	}

	rshash_state.state = RSHASH_NEW;
	rshash_state.below_count = 0;
	rshash_state.lookup_window_index = 0;

	return cal_positive_negative_costs();
}

static int __init ksm_slab_init(void)
{
	rmap_item_cache = KSM_KMEM_CACHE(rmap_item, 0);
	if (!rmap_item_cache)
		goto out;

	stable_node_cache = KSM_KMEM_CACHE(stable_node, 0);
	if (!stable_node_cache)
		goto out_free1;

	node_vma_cache = KSM_KMEM_CACHE(node_vma, 0);
	if (!node_vma_cache)
		goto out_free2;

	vma_slot_cache = KSM_KMEM_CACHE(vma_slot, 0);
	if (!vma_slot_cache)
		goto out_free3;

	tree_node_cache = KSM_KMEM_CACHE(tree_node, 0);
	if (!tree_node_cache)
		goto out_free4;

	return 0;

out_free4:
	kmem_cache_destroy(vma_slot_cache);
out_free3:
	kmem_cache_destroy(node_vma_cache);
out_free2:
	kmem_cache_destroy(stable_node_cache);
out_free1:
	kmem_cache_destroy(rmap_item_cache);
out:
	return -ENOMEM;
}

static void __init ksm_slab_free(void)
{
	kmem_cache_destroy(stable_node_cache);
	kmem_cache_destroy(rmap_item_cache);
	kmem_cache_destroy(node_vma_cache);
	kmem_cache_destroy(vma_slot_cache);
	kmem_cache_destroy(tree_node_cache);
}

static int __init ksm_init(void)
{
	struct task_struct *ksm_thread;
	int err;
	unsigned int allocsize;
	unsigned int sr = ksm_min_scan_ratio;

	ksm_scan_ladder_size = 1;
	while (sr < KSM_SCAN_RATIO_MAX) {
		sr *= ksm_scan_ratio_delta;
		ksm_scan_ladder_size++;
	}
	ksm_scan_ladder = kzalloc(sizeof(struct scan_rung) *
				  ksm_scan_ladder_size, GFP_KERNEL);
	if (!ksm_scan_ladder) {
		printk(KERN_ERR "ksm scan ladder allocation failed, size=%d\n",
		       ksm_scan_ladder_size);
		err = ENOMEM;
		goto out;
	}
	init_scan_ladder();

	allocsize = KSM_DUP_VMA_MAX * KSM_DUP_VMA_MAX *
		sizeof(unsigned int) / 2;

	ksm_inter_vma_table = vmalloc(allocsize);
	if (!ksm_inter_vma_table) {
		err = ENOMEM;
		goto out_free3;
	}

	memset(ksm_inter_vma_table, 0, allocsize);

	ksm_vma_table = kzalloc(sizeof(struct vma_slot *) *
				ksm_vma_table_size, GFP_KERNEL);

	if (!ksm_vma_table) {
		printk(KERN_ERR "ksm_vma_table allocation failed, size=%d\n",
		       ksm_vma_table_size);
		err = ENOMEM;
		goto out;
	}

	err = init_random_sampling();
	if (err)
		goto out_free;

	err = ksm_slab_init();
	if (err)
		goto out_free4;

	ksm_thread = kthread_run(ksm_scan_thread, NULL, "ksmd");
	if (IS_ERR(ksm_thread)) {
		printk(KERN_ERR "ksm: creating kthread failed\n");
		err = PTR_ERR(ksm_thread);
		goto out_free1;
	}

#ifdef CONFIG_SYSFS
	err = sysfs_create_group(mm_kobj, &ksm_attr_group);
	if (err) {
		printk(KERN_ERR "ksm: register sysfs failed\n");
		kthread_stop(ksm_thread);
		goto out_free1;
	}
#else
	ksm_run = KSM_RUN_MERGE;	/* no way for user to start it */

#endif /* CONFIG_SYSFS */

#ifdef CONFIG_MEMORY_HOTREMOVE
	/*
	 * Choose a high priority since the callback takes ksm_thread_mutex:
	 * later callbacks could only be taking locks which nest within that.
	 */
	hotplug_memory_notifier(ksm_memory_callback, 100);
#endif
	return 0;

out_free1:
	ksm_slab_free();
out_free4:
	kfree(random_nums);
out_free:
	vfree(ksm_inter_vma_table);
out_free3:
	kfree(ksm_scan_ladder);

out:
	return err;
}

#ifdef MODULE
module_init(ksm_init)
#else
late_initcall(ksm_init);
#endif

