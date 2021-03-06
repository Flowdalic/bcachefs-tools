/*
 * Code for manipulating bucket marks for garbage collection.
 *
 * Copyright 2014 Datera, Inc.
 *
 * Bucket states:
 * - free bucket: mark == 0
 *   The bucket contains no data and will not be read
 *
 * - allocator bucket: owned_by_allocator == 1
 *   The bucket is on a free list, or it is an open bucket
 *
 * - cached bucket: owned_by_allocator == 0 &&
 *                  dirty_sectors == 0 &&
 *                  cached_sectors > 0
 *   The bucket contains data but may be safely discarded as there are
 *   enough replicas of the data on other cache devices, or it has been
 *   written back to the backing device
 *
 * - dirty bucket: owned_by_allocator == 0 &&
 *                 dirty_sectors > 0
 *   The bucket contains data that we must not discard (either only copy,
 *   or one of the 'main copies' for data requiring multiple replicas)
 *
 * - metadata bucket: owned_by_allocator == 0 && is_metadata == 1
 *   This is a btree node, journal or gen/prio bucket
 *
 * Lifecycle:
 *
 * bucket invalidated => bucket on freelist => open bucket =>
 *     [dirty bucket =>] cached bucket => bucket invalidated => ...
 *
 * Note that cache promotion can skip the dirty bucket step, as data
 * is copied from a deeper tier to a shallower tier, onto a cached
 * bucket.
 * Note also that a cached bucket can spontaneously become dirty --
 * see below.
 *
 * Only a traversal of the key space can determine whether a bucket is
 * truly dirty or cached.
 *
 * Transitions:
 *
 * - free => allocator: bucket was invalidated
 * - cached => allocator: bucket was invalidated
 *
 * - allocator => dirty: open bucket was filled up
 * - allocator => cached: open bucket was filled up
 * - allocator => metadata: metadata was allocated
 *
 * - dirty => cached: dirty sectors were copied to a deeper tier
 * - dirty => free: dirty sectors were overwritten or moved (copy gc)
 * - cached => free: cached sectors were overwritten
 *
 * - metadata => free: metadata was freed
 *
 * Oddities:
 * - cached => dirty: a device was removed so formerly replicated data
 *                    is no longer sufficiently replicated
 * - free => cached: cannot happen
 * - free => dirty: cannot happen
 * - free => metadata: cannot happen
 */

#include "bcachefs.h"
#include "alloc_background.h"
#include "bset.h"
#include "btree_gc.h"
#include "btree_update.h"
#include "buckets.h"
#include "ec.h"
#include "error.h"
#include "movinggc.h"

#include <linux/preempt.h>
#include <trace/events/bcachefs.h>

static inline u64 __bch2_fs_sectors_used(struct bch_fs *, struct bch_fs_usage);

#ifdef DEBUG_BUCKETS

#define lg_local_lock	lg_global_lock
#define lg_local_unlock	lg_global_unlock

static void bch2_fs_stats_verify(struct bch_fs *c)
{
	struct bch_fs_usage stats =_bch2_fs_usage_read(c);
	unsigned i, j;

	for (i = 0; i < ARRAY_SIZE(stats.replicas); i++) {
		for (j = 0; j < ARRAY_SIZE(stats.replicas[i].data); j++)
			if ((s64) stats.replicas[i].data[j] < 0)
				panic("replicas %u %s sectors underflow: %lli\n",
				      i + 1, bch_data_types[j],
				      stats.replicas[i].data[j]);

		if ((s64) stats.replicas[i].persistent_reserved < 0)
			panic("replicas %u reserved underflow: %lli\n",
			      i + 1, stats.replicas[i].persistent_reserved);
	}

	for (j = 0; j < ARRAY_SIZE(stats.buckets); j++)
		if ((s64) stats.replicas[i].data_buckets[j] < 0)
			panic("%s buckets underflow: %lli\n",
			      bch_data_types[j],
			      stats.buckets[j]);

	if ((s64) stats.s.online_reserved < 0)
		panic("sectors_online_reserved underflow: %lli\n",
		      stats.s.online_reserved);
}

static void bch2_dev_stats_verify(struct bch_dev *ca)
{
	struct bch_dev_usage stats =
		__bch2_dev_usage_read(ca);
	u64 n = ca->mi.nbuckets - ca->mi.first_bucket;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(stats.buckets); i++)
		BUG_ON(stats.buckets[i]		> n);
	BUG_ON(stats.buckets_alloc		> n);
	BUG_ON(stats.buckets_unavailable	> n);
}

static void bch2_disk_reservations_verify(struct bch_fs *c, int flags)
{
	if (!(flags & BCH_DISK_RESERVATION_NOFAIL)) {
		u64 used = __bch2_fs_sectors_used(c);
		u64 cached = 0;
		u64 avail = atomic64_read(&c->sectors_available);
		int cpu;

		for_each_possible_cpu(cpu)
			cached += per_cpu_ptr(c->usage_percpu, cpu)->available_cache;

		if (used + avail + cached > c->capacity)
			panic("used %llu avail %llu cached %llu capacity %llu\n",
			      used, avail, cached, c->capacity);
	}
}

#else

static void bch2_fs_stats_verify(struct bch_fs *c) {}
static void bch2_dev_stats_verify(struct bch_dev *ca) {}
static void bch2_disk_reservations_verify(struct bch_fs *c, int flags) {}

#endif

/*
 * Clear journal_seq_valid for buckets for which it's not needed, to prevent
 * wraparound:
 */
void bch2_bucket_seq_cleanup(struct bch_fs *c)
{
	u64 journal_seq = atomic64_read(&c->journal.seq);
	u16 last_seq_ondisk = c->journal.last_seq_ondisk;
	struct bch_dev *ca;
	struct bucket_array *buckets;
	struct bucket *g;
	struct bucket_mark m;
	unsigned i;

	if (journal_seq - c->last_bucket_seq_cleanup <
	    (1U << (BUCKET_JOURNAL_SEQ_BITS - 2)))
		return;

	c->last_bucket_seq_cleanup = journal_seq;

	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for_each_bucket(g, buckets) {
			bucket_cmpxchg(g, m, ({
				if (!m.journal_seq_valid ||
				    bucket_needs_journal_commit(m, last_seq_ondisk))
					break;

				m.journal_seq_valid = 0;
			}));
		}
		up_read(&ca->bucket_lock);
	}
}

#define bch2_usage_add(_acc, _stats)					\
do {									\
	typeof(_acc) _a = (_acc), _s = (_stats);			\
	unsigned i;							\
									\
	for (i = 0; i < sizeof(*_a) / sizeof(u64); i++)			\
		((u64 *) (_a))[i] += ((u64 *) (_s))[i];			\
} while (0)

#define bch2_usage_read_raw(_stats)					\
({									\
	typeof(*this_cpu_ptr(_stats)) _acc;				\
	int cpu;							\
									\
	memset(&_acc, 0, sizeof(_acc));					\
									\
	for_each_possible_cpu(cpu)					\
		bch2_usage_add(&_acc, per_cpu_ptr((_stats), cpu));	\
									\
	_acc;								\
})

struct bch_dev_usage __bch2_dev_usage_read(struct bch_dev *ca, bool gc)
{
	return bch2_usage_read_raw(ca->usage[gc]);
}

struct bch_dev_usage bch2_dev_usage_read(struct bch_fs *c, struct bch_dev *ca)
{
	return bch2_usage_read_raw(ca->usage[0]);
}

struct bch_fs_usage __bch2_fs_usage_read(struct bch_fs *c, bool gc)
{
	return bch2_usage_read_raw(c->usage[gc]);
}

struct bch_fs_usage bch2_fs_usage_read(struct bch_fs *c)
{
	return bch2_usage_read_raw(c->usage[0]);
}

#define RESERVE_FACTOR	6

static u64 reserve_factor(u64 r)
{
	return r + (round_up(r, (1 << RESERVE_FACTOR)) >> RESERVE_FACTOR);
}

static u64 avail_factor(u64 r)
{
	return (r << RESERVE_FACTOR) / ((1 << RESERVE_FACTOR) + 1);
}

static inline u64 __bch2_fs_sectors_used(struct bch_fs *c, struct bch_fs_usage fs_usage)
{
	return fs_usage.s.hidden +
		fs_usage.s.data +
		reserve_factor(fs_usage.s.reserved +
			       fs_usage.s.online_reserved);
}

u64 bch2_fs_sectors_used(struct bch_fs *c, struct bch_fs_usage fs_usage)
{
	return min(c->capacity, __bch2_fs_sectors_used(c, fs_usage));
}

struct bch_fs_usage_short
bch2_fs_usage_read_short(struct bch_fs *c)
{
	struct bch_fs_usage_summarized usage =
		bch2_usage_read_raw(&c->usage[0]->s);
	struct bch_fs_usage_short ret;

	ret.capacity	= READ_ONCE(c->capacity) - usage.hidden;
	ret.used	= min(ret.capacity, usage.data +
			      reserve_factor(usage.reserved +
					     usage.online_reserved));
	ret.nr_inodes	= usage.nr_inodes;

	return ret;
}

static inline int is_unavailable_bucket(struct bucket_mark m)
{
	return !is_available_bucket(m);
}

static inline int is_fragmented_bucket(struct bucket_mark m,
				       struct bch_dev *ca)
{
	if (!m.owned_by_allocator &&
	    m.data_type == BCH_DATA_USER &&
	    bucket_sectors_used(m))
		return max_t(int, 0, (int) ca->mi.bucket_size -
			     bucket_sectors_used(m));
	return 0;
}

static inline enum bch_data_type bucket_type(struct bucket_mark m)
{
	return m.cached_sectors && !m.dirty_sectors
		? BCH_DATA_CACHED
		: m.data_type;
}

static bool bucket_became_unavailable(struct bucket_mark old,
				      struct bucket_mark new)
{
	return is_available_bucket(old) &&
	       !is_available_bucket(new);
}

void bch2_fs_usage_apply(struct bch_fs *c,
			 struct bch_fs_usage *fs_usage,
			 struct disk_reservation *disk_res,
			 struct gc_pos gc_pos)
{
	s64 added = fs_usage->s.data + fs_usage->s.reserved;
	s64 should_not_have_added;

	percpu_rwsem_assert_held(&c->mark_lock);

	/*
	 * Not allowed to reduce sectors_available except by getting a
	 * reservation:
	 */
	should_not_have_added = added - (s64) (disk_res ? disk_res->sectors : 0);
	if (WARN_ONCE(should_not_have_added > 0,
		      "disk usage increased without a reservation")) {
		atomic64_sub(should_not_have_added, &c->sectors_available);
		added -= should_not_have_added;
	}

	if (added > 0) {
		disk_res->sectors		-= added;
		fs_usage->s.online_reserved	-= added;
	}

	bch2_usage_add(this_cpu_ptr(c->usage[0]), fs_usage);

	if (gc_visited(c, gc_pos))
		bch2_usage_add(this_cpu_ptr(c->usage[1]), fs_usage);

	bch2_fs_stats_verify(c);

	memset(fs_usage, 0, sizeof(*fs_usage));
}

static inline void account_bucket(struct bch_fs_usage *fs_usage,
				  struct bch_dev_usage *dev_usage,
				  enum bch_data_type type,
				  int nr, s64 size)
{
	if (type == BCH_DATA_SB || type == BCH_DATA_JOURNAL)
		fs_usage->s.hidden	+= size;

	fs_usage->buckets[type]		+= size;
	dev_usage->buckets[type]	+= nr;
}

static void bch2_dev_usage_update(struct bch_fs *c, struct bch_dev *ca,
				  struct bch_fs_usage *fs_usage,
				  struct bucket_mark old, struct bucket_mark new,
				  bool gc)
{
	struct bch_dev_usage *dev_usage;

	percpu_rwsem_assert_held(&c->mark_lock);

	bch2_fs_inconsistent_on(old.data_type && new.data_type &&
				old.data_type != new.data_type, c,
		"different types of data in same bucket: %s, %s",
		bch2_data_types[old.data_type],
		bch2_data_types[new.data_type]);

	dev_usage = this_cpu_ptr(ca->usage[gc]);

	if (bucket_type(old))
		account_bucket(fs_usage, dev_usage, bucket_type(old),
			       -1, -ca->mi.bucket_size);

	if (bucket_type(new))
		account_bucket(fs_usage, dev_usage, bucket_type(new),
			       1, ca->mi.bucket_size);

	dev_usage->buckets_alloc +=
		(int) new.owned_by_allocator - (int) old.owned_by_allocator;
	dev_usage->buckets_ec +=
		(int) new.stripe - (int) old.stripe;
	dev_usage->buckets_unavailable +=
		is_unavailable_bucket(new) - is_unavailable_bucket(old);

	dev_usage->sectors[old.data_type] -= old.dirty_sectors;
	dev_usage->sectors[new.data_type] += new.dirty_sectors;
	dev_usage->sectors[BCH_DATA_CACHED] +=
		(int) new.cached_sectors - (int) old.cached_sectors;
	dev_usage->sectors_fragmented +=
		is_fragmented_bucket(new, ca) - is_fragmented_bucket(old, ca);

	if (!is_available_bucket(old) && is_available_bucket(new))
		bch2_wake_allocator(ca);

	bch2_dev_stats_verify(ca);
}

void bch2_dev_usage_from_buckets(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_mark old = { .v.counter = 0 };
	struct bch_fs_usage *fs_usage;
	struct bucket_array *buckets;
	struct bucket *g;

	percpu_down_read_preempt_disable(&c->mark_lock);
	fs_usage = this_cpu_ptr(c->usage[0]);
	buckets = bucket_array(ca);

	for_each_bucket(g, buckets)
		if (g->mark.data_type)
			bch2_dev_usage_update(c, ca, fs_usage, old, g->mark, false);
	percpu_up_read_preempt_enable(&c->mark_lock);
}

#define bucket_data_cmpxchg(c, ca, fs_usage, g, new, expr)	\
({								\
	struct bucket_mark _old = bucket_cmpxchg(g, new, expr);	\
								\
	bch2_dev_usage_update(c, ca, fs_usage, _old, new, gc);	\
	_old;							\
})

static void __bch2_invalidate_bucket(struct bch_fs *c, struct bch_dev *ca,
				     size_t b, struct bucket_mark *old,
				     bool gc)
{
	struct bch_fs_usage *fs_usage = this_cpu_ptr(c->usage[gc]);
	struct bucket *g = __bucket(ca, b, gc);
	struct bucket_mark new;

	*old = bucket_data_cmpxchg(c, ca, fs_usage, g, new, ({
		BUG_ON(!is_available_bucket(new));

		new.owned_by_allocator	= 1;
		new.data_type		= 0;
		new.cached_sectors	= 0;
		new.dirty_sectors	= 0;
		new.gen++;
	}));

	fs_usage->replicas[0].data[BCH_DATA_CACHED]	-= old->cached_sectors;
	fs_usage->s.cached				-= old->cached_sectors;
}

void bch2_invalidate_bucket(struct bch_fs *c, struct bch_dev *ca,
			    size_t b, struct bucket_mark *old)
{
	percpu_rwsem_assert_held(&c->mark_lock);

	__bch2_invalidate_bucket(c, ca, b, old, false);

	if (!old->owned_by_allocator && old->cached_sectors)
		trace_invalidate(ca, bucket_to_sector(ca, b),
				 old->cached_sectors);
}

static void __bch2_mark_alloc_bucket(struct bch_fs *c, struct bch_dev *ca,
				     size_t b, bool owned_by_allocator,
				     bool gc)
{
	struct bch_fs_usage *fs_usage = this_cpu_ptr(c->usage[gc]);
	struct bucket *g = __bucket(ca, b, gc);
	struct bucket_mark old, new;

	old = bucket_data_cmpxchg(c, ca, fs_usage, g, new, ({
		new.owned_by_allocator	= owned_by_allocator;
	}));

	BUG_ON(!gc &&
	       !owned_by_allocator && !old.owned_by_allocator);
}

void bch2_mark_alloc_bucket(struct bch_fs *c, struct bch_dev *ca,
			    size_t b, bool owned_by_allocator,
			    struct gc_pos pos, unsigned flags)
{
	percpu_rwsem_assert_held(&c->mark_lock);

	if (!(flags & BCH_BUCKET_MARK_GC))
		__bch2_mark_alloc_bucket(c, ca, b, owned_by_allocator, false);

	if ((flags & BCH_BUCKET_MARK_GC) ||
	    gc_visited(c, pos))
		__bch2_mark_alloc_bucket(c, ca, b, owned_by_allocator, true);
}

#define checked_add(a, b)					\
do {								\
	unsigned _res = (unsigned) (a) + (b);			\
	(a) = _res;						\
	BUG_ON((a) != _res);					\
} while (0)

static void __bch2_mark_metadata_bucket(struct bch_fs *c, struct bch_dev *ca,
					size_t b, enum bch_data_type type,
					unsigned sectors, bool gc)
{
	struct bch_fs_usage *fs_usage = this_cpu_ptr(c->usage[gc]);
	struct bucket *g = __bucket(ca, b, gc);
	struct bucket_mark new;

	BUG_ON(type != BCH_DATA_SB &&
	       type != BCH_DATA_JOURNAL);

	bucket_data_cmpxchg(c, ca, fs_usage, g, new, ({
		new.data_type	= type;
		checked_add(new.dirty_sectors, sectors);
	}));

	if (type == BCH_DATA_BTREE ||
	    type == BCH_DATA_USER)
		fs_usage->s.data		+= sectors;
	fs_usage->replicas[0].data[type]	+= sectors;
}

void bch2_mark_metadata_bucket(struct bch_fs *c, struct bch_dev *ca,
			       size_t b, enum bch_data_type type,
			       unsigned sectors, struct gc_pos pos,
			       unsigned flags)
{
	BUG_ON(type != BCH_DATA_SB &&
	       type != BCH_DATA_JOURNAL);

	if (likely(c)) {
		percpu_rwsem_assert_held(&c->mark_lock);

		if (!(flags & BCH_BUCKET_MARK_GC))
			__bch2_mark_metadata_bucket(c, ca, b, type, sectors,
						    false);
		if ((flags & BCH_BUCKET_MARK_GC) ||
		    gc_visited(c, pos))
			__bch2_mark_metadata_bucket(c, ca, b, type, sectors,
						    true);
	} else {
		struct bucket *g;
		struct bucket_mark old, new;

		rcu_read_lock();

		g = bucket(ca, b);
		old = bucket_cmpxchg(g, new, ({
			new.data_type = type;
			checked_add(new.dirty_sectors, sectors);
		}));

		rcu_read_unlock();
	}
}

static s64 ptr_disk_sectors_delta(struct extent_ptr_decoded p,
				  s64 delta)
{
	if (delta > 0) {
		/*
		 * marking a new extent, which _will have size_ @delta
		 *
		 * in the bch2_mark_update -> BCH_EXTENT_OVERLAP_MIDDLE
		 * case, we haven't actually created the key we'll be inserting
		 * yet (for the split) - so we don't want to be using
		 * k->size/crc.live_size here:
		 */
		return __ptr_disk_sectors(p, delta);
	} else {
		BUG_ON(-delta > p.crc.live_size);

		return (s64) __ptr_disk_sectors(p, p.crc.live_size + delta) -
			(s64) ptr_disk_sectors(p);
	}
}

/*
 * Checking against gc's position has to be done here, inside the cmpxchg()
 * loop, to avoid racing with the start of gc clearing all the marks - GC does
 * that with the gc pos seqlock held.
 */
static void bch2_mark_pointer(struct bch_fs *c,
			      struct extent_ptr_decoded p,
			      s64 sectors, enum bch_data_type data_type,
			      struct bch_fs_usage *fs_usage,
			      unsigned journal_seq, unsigned flags,
			      bool gc)
{
	struct bucket_mark old, new;
	struct bch_dev *ca = bch_dev_bkey_exists(c, p.ptr.dev);
	size_t b = PTR_BUCKET_NR(ca, &p.ptr);
	struct bucket *g = __bucket(ca, b, gc);
	u64 v;

	v = atomic64_read(&g->_mark.v);
	do {
		new.v.counter = old.v.counter = v;

		/*
		 * Check this after reading bucket mark to guard against
		 * the allocator invalidating a bucket after we've already
		 * checked the gen
		 */
		if (gen_after(new.gen, p.ptr.gen)) {
			BUG_ON(!test_bit(BCH_FS_ALLOC_READ_DONE, &c->flags));
			EBUG_ON(!p.ptr.cached &&
				test_bit(JOURNAL_REPLAY_DONE, &c->journal.flags));
			return;
		}

		if (!p.ptr.cached)
			checked_add(new.dirty_sectors, sectors);
		else
			checked_add(new.cached_sectors, sectors);

		if (!new.dirty_sectors &&
		    !new.cached_sectors) {
			new.data_type	= 0;

			if (journal_seq) {
				new.journal_seq_valid = 1;
				new.journal_seq = journal_seq;
			}
		} else {
			new.data_type = data_type;
		}

		if (flags & BCH_BUCKET_MARK_NOATOMIC) {
			g->_mark = new;
			break;
		}
	} while ((v = atomic64_cmpxchg(&g->_mark.v,
			      old.v.counter,
			      new.v.counter)) != old.v.counter);

	bch2_dev_usage_update(c, ca, fs_usage, old, new, gc);

	BUG_ON(!gc && bucket_became_unavailable(old, new));
}

static int bch2_mark_stripe_ptr(struct bch_fs *c,
				struct bch_extent_stripe_ptr p,
				s64 sectors, unsigned flags,
				s64 *adjusted_disk_sectors,
				unsigned *redundancy,
				bool gc)
{
	struct stripe *m;
	unsigned old, new, nr_data;
	int blocks_nonempty_delta;
	s64 parity_sectors;

	m = genradix_ptr(&c->stripes[gc], p.idx);

	if (!m || !m->alive) {
		bch_err_ratelimited(c, "pointer to nonexistent stripe %llu",
				    (u64) p.idx);
		return -1;
	}

	nr_data = m->nr_blocks - m->nr_redundant;

	parity_sectors = DIV_ROUND_UP(abs(sectors) * m->nr_redundant, nr_data);

	if (sectors < 0)
		parity_sectors = -parity_sectors;

	*adjusted_disk_sectors += parity_sectors;

	*redundancy = max_t(unsigned, *redundancy, m->nr_redundant + 1);

	new = atomic_add_return(sectors, &m->block_sectors[p.block]);
	old = new - sectors;

	blocks_nonempty_delta = (int) !!new - (int) !!old;
	if (!blocks_nonempty_delta)
		return 0;

	atomic_add(blocks_nonempty_delta, &m->blocks_nonempty);

	BUG_ON(atomic_read(&m->blocks_nonempty) < 0);

	if (!gc)
		bch2_stripes_heap_update(c, m, p.idx);

	return 0;
}

static int bch2_mark_extent(struct bch_fs *c, struct bkey_s_c k,
			    s64 sectors, enum bch_data_type data_type,
			    struct bch_fs_usage *fs_usage,
			    unsigned journal_seq, unsigned flags,
			    bool gc)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;
	s64 cached_sectors	= 0;
	s64 dirty_sectors	= 0;
	s64 ec_sectors		= 0;
	unsigned replicas	= 0;
	unsigned ec_redundancy	= 0;
	unsigned i;
	int ret;

	BUG_ON(!sectors);

	bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
		s64 disk_sectors = data_type == BCH_DATA_BTREE
			? sectors
			: ptr_disk_sectors_delta(p, sectors);
		s64 adjusted_disk_sectors = disk_sectors;

		bch2_mark_pointer(c, p, disk_sectors, data_type,
				  fs_usage, journal_seq, flags, gc);

		if (!p.ptr.cached)
			for (i = 0; i < p.ec_nr; i++) {
				ret = bch2_mark_stripe_ptr(c, p.ec[i],
						disk_sectors, flags,
						&adjusted_disk_sectors,
						&ec_redundancy, gc);
				if (ret)
					return ret;
			}
		if (!p.ptr.cached)
			replicas++;

		if (p.ptr.cached)
			cached_sectors	+= adjusted_disk_sectors;
		else if (!p.ec_nr)
			dirty_sectors	+= adjusted_disk_sectors;
		else
			ec_sectors	+= adjusted_disk_sectors;
	}

	replicas	= clamp_t(unsigned,	replicas,
				  1, ARRAY_SIZE(fs_usage->replicas));
	ec_redundancy	= clamp_t(unsigned,	ec_redundancy,
				  1, ARRAY_SIZE(fs_usage->replicas));

	fs_usage->s.cached					+= cached_sectors;
	fs_usage->replicas[0].data[BCH_DATA_CACHED]		+= cached_sectors;

	fs_usage->s.data					+= dirty_sectors;
	fs_usage->replicas[replicas - 1].data[data_type]	+= dirty_sectors;

	fs_usage->s.data					+= ec_sectors;
	fs_usage->replicas[ec_redundancy - 1].ec_data		+= ec_sectors;

	return 0;
}

static void bucket_set_stripe(struct bch_fs *c,
			      const struct bch_stripe *v,
			      bool enabled,
			      struct bch_fs_usage *fs_usage,
			      u64 journal_seq,
			      bool gc)
{
	unsigned i;

	for (i = 0; i < v->nr_blocks; i++) {
		const struct bch_extent_ptr *ptr = v->ptrs + i;
		struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
		size_t b = PTR_BUCKET_NR(ca, ptr);
		struct bucket *g = __bucket(ca, b, gc);
		struct bucket_mark new, old;

		BUG_ON(ptr_stale(ca, ptr));

		old = bucket_data_cmpxchg(c, ca, fs_usage, g, new, ({
			new.stripe			= enabled;
			if (journal_seq) {
				new.journal_seq_valid	= 1;
				new.journal_seq		= journal_seq;
			}
		}));

		BUG_ON(old.stripe == enabled);
	}
}

static int bch2_mark_stripe(struct bch_fs *c, struct bkey_s_c k,
			    bool inserting,
			    struct bch_fs_usage *fs_usage,
			    u64 journal_seq, unsigned flags,
			    bool gc)
{
	struct bkey_s_c_stripe s = bkey_s_c_to_stripe(k);
	size_t idx = s.k->p.offset;
	struct stripe *m = genradix_ptr(&c->stripes[gc], idx);
	unsigned i;

	if (!m || (!inserting && !m->alive)) {
		bch_err_ratelimited(c, "error marking nonexistent stripe %zu",
				    idx);
		return -1;
	}

	if (inserting && m->alive) {
		bch_err_ratelimited(c, "error marking stripe %zu: already exists",
				    idx);
		return -1;
	}

	BUG_ON(atomic_read(&m->blocks_nonempty));

	for (i = 0; i < EC_STRIPE_MAX; i++)
		BUG_ON(atomic_read(&m->block_sectors[i]));

	if (inserting) {
		m->sectors	= le16_to_cpu(s.v->sectors);
		m->algorithm	= s.v->algorithm;
		m->nr_blocks	= s.v->nr_blocks;
		m->nr_redundant	= s.v->nr_redundant;
	}

	if (!gc) {
		if (inserting)
			bch2_stripes_heap_insert(c, m, idx);
		else
			bch2_stripes_heap_del(c, m, idx);
	} else {
		m->alive = inserting;
	}

	bucket_set_stripe(c, s.v, inserting, fs_usage, 0, gc);
	return 0;
}

static int __bch2_mark_key(struct bch_fs *c, struct bkey_s_c k,
			   bool inserting, s64 sectors,
			   struct bch_fs_usage *fs_usage,
			   unsigned journal_seq, unsigned flags,
			   bool gc)
{
	int ret = 0;

	switch (k.k->type) {
	case KEY_TYPE_btree_ptr:
		ret = bch2_mark_extent(c, k, inserting
				       ?  c->opts.btree_node_size
				       : -c->opts.btree_node_size,
				       BCH_DATA_BTREE,
				       fs_usage, journal_seq, flags, gc);
		break;
	case KEY_TYPE_extent:
		ret = bch2_mark_extent(c, k, sectors, BCH_DATA_USER,
				       fs_usage, journal_seq, flags, gc);
		break;
	case KEY_TYPE_stripe:
		ret = bch2_mark_stripe(c, k, inserting,
				       fs_usage, journal_seq, flags, gc);
		break;
	case KEY_TYPE_alloc:
		if (inserting)
			fs_usage->s.nr_inodes++;
		else
			fs_usage->s.nr_inodes--;
		break;
	case KEY_TYPE_reservation: {
		unsigned replicas = bkey_s_c_to_reservation(k).v->nr_replicas;

		sectors *= replicas;
		replicas = clamp_t(unsigned, replicas,
				   1, ARRAY_SIZE(fs_usage->replicas));

		fs_usage->s.reserved					+= sectors;
		fs_usage->replicas[replicas - 1].persistent_reserved	+= sectors;
		break;
	}
	default:
		break;
	}

	return ret;
}

int bch2_mark_key_locked(struct bch_fs *c,
		   struct bkey_s_c k,
		   bool inserting, s64 sectors,
		   struct gc_pos pos,
		   struct bch_fs_usage *fs_usage,
		   u64 journal_seq, unsigned flags)
{
	int ret;

	if (!(flags & BCH_BUCKET_MARK_GC)) {
		ret = __bch2_mark_key(c, k, inserting, sectors,
				      fs_usage ?: this_cpu_ptr(c->usage[0]),
				      journal_seq, flags, false);
		if (ret)
			return ret;
	}

	if ((flags & BCH_BUCKET_MARK_GC) ||
	    gc_visited(c, pos)) {
		ret = __bch2_mark_key(c, k, inserting, sectors,
				      this_cpu_ptr(c->usage[1]),
				      journal_seq, flags, true);
		if (ret)
			return ret;
	}

	return 0;
}

int bch2_mark_key(struct bch_fs *c, struct bkey_s_c k,
		  bool inserting, s64 sectors,
		  struct gc_pos pos,
		  struct bch_fs_usage *fs_usage,
		  u64 journal_seq, unsigned flags)
{
	int ret;

	percpu_down_read_preempt_disable(&c->mark_lock);
	ret = bch2_mark_key_locked(c, k, inserting, sectors,
				   pos, fs_usage, journal_seq, flags);
	percpu_up_read_preempt_enable(&c->mark_lock);

	return ret;
}

void bch2_mark_update(struct btree_insert *trans,
		      struct btree_insert_entry *insert)
{
	struct bch_fs		*c = trans->c;
	struct btree_iter	*iter = insert->iter;
	struct btree		*b = iter->l[0].b;
	struct btree_node_iter	node_iter = iter->l[0].iter;
	struct bch_fs_usage	fs_usage = { 0 };
	struct gc_pos		pos = gc_pos_btree_node(b);
	struct bkey_packed	*_k;

	if (!btree_node_type_needs_gc(iter->btree_id))
		return;

	percpu_down_read_preempt_disable(&c->mark_lock);

	if (!(trans->flags & BTREE_INSERT_JOURNAL_REPLAY))
		bch2_mark_key_locked(c, bkey_i_to_s_c(insert->k), true,
			bpos_min(insert->k->k.p, b->key.k.p).offset -
			bkey_start_offset(&insert->k->k),
			pos, &fs_usage, trans->journal_res.seq, 0);

	while ((_k = bch2_btree_node_iter_peek_filter(&node_iter, b,
						      KEY_TYPE_discard))) {
		struct bkey		unpacked;
		struct bkey_s_c		k;
		s64			sectors = 0;

		k = bkey_disassemble(b, _k, &unpacked);

		if (btree_node_is_extents(b)
		    ? bkey_cmp(insert->k->k.p, bkey_start_pos(k.k)) <= 0
		    : bkey_cmp(insert->k->k.p, k.k->p))
			break;

		if (btree_node_is_extents(b)) {
			switch (bch2_extent_overlap(&insert->k->k, k.k)) {
			case BCH_EXTENT_OVERLAP_ALL:
				sectors = -((s64) k.k->size);
				break;
			case BCH_EXTENT_OVERLAP_BACK:
				sectors = bkey_start_offset(&insert->k->k) -
					k.k->p.offset;
				break;
			case BCH_EXTENT_OVERLAP_FRONT:
				sectors = bkey_start_offset(k.k) -
					insert->k->k.p.offset;
				break;
			case BCH_EXTENT_OVERLAP_MIDDLE:
				sectors = k.k->p.offset - insert->k->k.p.offset;
				BUG_ON(sectors <= 0);

				bch2_mark_key_locked(c, k, true, sectors,
					pos, &fs_usage, trans->journal_res.seq, 0);

				sectors = bkey_start_offset(&insert->k->k) -
					k.k->p.offset;
				break;
			}

			BUG_ON(sectors >= 0);
		}

		bch2_mark_key_locked(c, k, false, sectors,
			pos, &fs_usage, trans->journal_res.seq, 0);

		bch2_btree_node_iter_advance(&node_iter, b);
	}

	bch2_fs_usage_apply(c, &fs_usage, trans->disk_res, pos);

	percpu_up_read_preempt_enable(&c->mark_lock);
}

/* Disk reservations: */

static u64 bch2_recalc_sectors_available(struct bch_fs *c)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu_ptr(c->pcpu, cpu)->sectors_available = 0;

	return avail_factor(bch2_fs_sectors_free(c));
}

void __bch2_disk_reservation_put(struct bch_fs *c, struct disk_reservation *res)
{
	percpu_down_read_preempt_disable(&c->mark_lock);
	this_cpu_sub(c->usage[0]->s.online_reserved,
		     res->sectors);

	bch2_fs_stats_verify(c);
	percpu_up_read_preempt_enable(&c->mark_lock);

	res->sectors = 0;
}

#define SECTORS_CACHE	1024

int bch2_disk_reservation_add(struct bch_fs *c, struct disk_reservation *res,
			      unsigned sectors, int flags)
{
	struct bch_fs_pcpu *pcpu;
	u64 old, v, get;
	s64 sectors_available;
	int ret;

	percpu_down_read_preempt_disable(&c->mark_lock);
	pcpu = this_cpu_ptr(c->pcpu);

	if (sectors <= pcpu->sectors_available)
		goto out;

	v = atomic64_read(&c->sectors_available);
	do {
		old = v;
		get = min((u64) sectors + SECTORS_CACHE, old);

		if (get < sectors) {
			percpu_up_read_preempt_enable(&c->mark_lock);
			goto recalculate;
		}
	} while ((v = atomic64_cmpxchg(&c->sectors_available,
				       old, old - get)) != old);

	pcpu->sectors_available		+= get;

out:
	pcpu->sectors_available		-= sectors;
	this_cpu_add(c->usage[0]->s.online_reserved, sectors);
	res->sectors			+= sectors;

	bch2_disk_reservations_verify(c, flags);
	bch2_fs_stats_verify(c);
	percpu_up_read_preempt_enable(&c->mark_lock);
	return 0;

recalculate:
	/*
	 * GC recalculates sectors_available when it starts, so that hopefully
	 * we don't normally end up blocking here:
	 */

	/*
	 * Piss fuck, we can be called from extent_insert_fixup() with btree
	 * locks held:
	 */

	if (!(flags & BCH_DISK_RESERVATION_GC_LOCK_HELD)) {
		if (!(flags & BCH_DISK_RESERVATION_BTREE_LOCKS_HELD))
			down_read(&c->gc_lock);
		else if (!down_read_trylock(&c->gc_lock))
			return -EINTR;
	}

	percpu_down_write(&c->mark_lock);
	sectors_available = bch2_recalc_sectors_available(c);

	if (sectors <= sectors_available ||
	    (flags & BCH_DISK_RESERVATION_NOFAIL)) {
		atomic64_set(&c->sectors_available,
			     max_t(s64, 0, sectors_available - sectors));
		this_cpu_add(c->usage[0]->s.online_reserved, sectors);
		res->sectors			+= sectors;
		ret = 0;

		bch2_disk_reservations_verify(c, flags);
	} else {
		atomic64_set(&c->sectors_available, sectors_available);
		ret = -ENOSPC;
	}

	bch2_fs_stats_verify(c);
	percpu_up_write(&c->mark_lock);

	if (!(flags & BCH_DISK_RESERVATION_GC_LOCK_HELD))
		up_read(&c->gc_lock);

	return ret;
}

/* Startup/shutdown: */

static void buckets_free_rcu(struct rcu_head *rcu)
{
	struct bucket_array *buckets =
		container_of(rcu, struct bucket_array, rcu);

	kvpfree(buckets,
		sizeof(struct bucket_array) +
		buckets->nbuckets * sizeof(struct bucket));
}

int bch2_dev_buckets_resize(struct bch_fs *c, struct bch_dev *ca, u64 nbuckets)
{
	struct bucket_array *buckets = NULL, *old_buckets = NULL;
	unsigned long *buckets_nouse = NULL;
	unsigned long *buckets_written = NULL;
	u8 *oldest_gens = NULL;
	alloc_fifo	free[RESERVE_NR];
	alloc_fifo	free_inc;
	alloc_heap	alloc_heap;
	copygc_heap	copygc_heap;

	size_t btree_reserve	= DIV_ROUND_UP(BTREE_NODE_RESERVE,
			     ca->mi.bucket_size / c->opts.btree_node_size);
	/* XXX: these should be tunable */
	size_t reserve_none	= max_t(size_t, 1, nbuckets >> 9);
	size_t copygc_reserve	= max_t(size_t, 2, nbuckets >> 7);
	size_t free_inc_nr	= max(max_t(size_t, 1, nbuckets >> 12),
				      btree_reserve);
	bool resize = ca->buckets[0] != NULL,
	     start_copygc = ca->copygc_thread != NULL;
	int ret = -ENOMEM;
	unsigned i;

	memset(&free,		0, sizeof(free));
	memset(&free_inc,	0, sizeof(free_inc));
	memset(&alloc_heap,	0, sizeof(alloc_heap));
	memset(&copygc_heap,	0, sizeof(copygc_heap));

	if (!(buckets		= kvpmalloc(sizeof(struct bucket_array) +
					    nbuckets * sizeof(struct bucket),
					    GFP_KERNEL|__GFP_ZERO)) ||
	    !(oldest_gens	= kvpmalloc(nbuckets * sizeof(u8),
					    GFP_KERNEL|__GFP_ZERO)) ||
	    !(buckets_nouse	= kvpmalloc(BITS_TO_LONGS(nbuckets) *
					    sizeof(unsigned long),
					    GFP_KERNEL|__GFP_ZERO)) ||
	    !(buckets_written	= kvpmalloc(BITS_TO_LONGS(nbuckets) *
					    sizeof(unsigned long),
					    GFP_KERNEL|__GFP_ZERO)) ||
	    !init_fifo(&free[RESERVE_BTREE], btree_reserve, GFP_KERNEL) ||
	    !init_fifo(&free[RESERVE_MOVINGGC],
		       copygc_reserve, GFP_KERNEL) ||
	    !init_fifo(&free[RESERVE_NONE], reserve_none, GFP_KERNEL) ||
	    !init_fifo(&free_inc,	free_inc_nr, GFP_KERNEL) ||
	    !init_heap(&alloc_heap,	ALLOC_SCAN_BATCH(ca) << 1, GFP_KERNEL) ||
	    !init_heap(&copygc_heap,	copygc_reserve, GFP_KERNEL))
		goto err;

	buckets->first_bucket	= ca->mi.first_bucket;
	buckets->nbuckets	= nbuckets;

	bch2_copygc_stop(ca);

	if (resize) {
		down_write(&c->gc_lock);
		down_write(&ca->bucket_lock);
		percpu_down_write(&c->mark_lock);
	}

	old_buckets = bucket_array(ca);

	if (resize) {
		size_t n = min(buckets->nbuckets, old_buckets->nbuckets);

		memcpy(buckets->b,
		       old_buckets->b,
		       n * sizeof(struct bucket));
		memcpy(oldest_gens,
		       ca->oldest_gens,
		       n * sizeof(u8));
		memcpy(buckets_nouse,
		       ca->buckets_nouse,
		       BITS_TO_LONGS(n) * sizeof(unsigned long));
		memcpy(buckets_written,
		       ca->buckets_written,
		       BITS_TO_LONGS(n) * sizeof(unsigned long));
	}

	rcu_assign_pointer(ca->buckets[0], buckets);
	buckets = old_buckets;

	swap(ca->oldest_gens, oldest_gens);
	swap(ca->buckets_nouse, buckets_nouse);
	swap(ca->buckets_written, buckets_written);

	if (resize)
		percpu_up_write(&c->mark_lock);

	spin_lock(&c->freelist_lock);
	for (i = 0; i < RESERVE_NR; i++) {
		fifo_move(&free[i], &ca->free[i]);
		swap(ca->free[i], free[i]);
	}
	fifo_move(&free_inc, &ca->free_inc);
	swap(ca->free_inc, free_inc);
	spin_unlock(&c->freelist_lock);

	/* with gc lock held, alloc_heap can't be in use: */
	swap(ca->alloc_heap, alloc_heap);

	/* and we shut down copygc: */
	swap(ca->copygc_heap, copygc_heap);

	nbuckets = ca->mi.nbuckets;

	if (resize) {
		up_write(&ca->bucket_lock);
		up_write(&c->gc_lock);
	}

	if (start_copygc &&
	    bch2_copygc_start(c, ca))
		bch_err(ca, "error restarting copygc thread");

	ret = 0;
err:
	free_heap(&copygc_heap);
	free_heap(&alloc_heap);
	free_fifo(&free_inc);
	for (i = 0; i < RESERVE_NR; i++)
		free_fifo(&free[i]);
	kvpfree(buckets_nouse,
		BITS_TO_LONGS(nbuckets) * sizeof(unsigned long));
	kvpfree(buckets_written,
		BITS_TO_LONGS(nbuckets) * sizeof(unsigned long));
	kvpfree(oldest_gens,
		nbuckets * sizeof(u8));
	if (buckets)
		call_rcu(&old_buckets->rcu, buckets_free_rcu);

	return ret;
}

void bch2_dev_buckets_free(struct bch_dev *ca)
{
	unsigned i;

	free_heap(&ca->copygc_heap);
	free_heap(&ca->alloc_heap);
	free_fifo(&ca->free_inc);
	for (i = 0; i < RESERVE_NR; i++)
		free_fifo(&ca->free[i]);
	kvpfree(ca->buckets_written,
		BITS_TO_LONGS(ca->mi.nbuckets) * sizeof(unsigned long));
	kvpfree(ca->buckets_nouse,
		BITS_TO_LONGS(ca->mi.nbuckets) * sizeof(unsigned long));
	kvpfree(ca->oldest_gens, ca->mi.nbuckets * sizeof(u8));
	kvpfree(rcu_dereference_protected(ca->buckets[0], 1),
		sizeof(struct bucket_array) +
		ca->mi.nbuckets * sizeof(struct bucket));

	free_percpu(ca->usage[0]);
}

int bch2_dev_buckets_alloc(struct bch_fs *c, struct bch_dev *ca)
{
	if (!(ca->usage[0] = alloc_percpu(struct bch_dev_usage)))
		return -ENOMEM;

	return bch2_dev_buckets_resize(c, ca, ca->mi.nbuckets);;
}
