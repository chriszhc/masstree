/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef MASSTREE_STRUCT_HH
#define MASSTREE_STRUCT_HH
#include "masstree.hh"
#include "nodeversion.hh"
#include "stringbag.hh"
#include "mtcounters.hh"
#include "timestamp.hh"
#include <iostream>
#include <cmath>

namespace Masstree {

template <typename P>
// namespace mass is defined in compiler.hh
// in compiler.hh:
// template <bool B, typename T, typename F>
// using conditional = std::conditional<B, T, F>, where B is bool;
// Provides member typedef type, which is defined as T if B is true at compile time, or as F if B is false.
// in nodeversion.hh
// typedef basic_nodeversion<nodeversion32_parameters> nodeversion;
// typedef basic_singlethreaded_nodeversion<nodeversion32_parameters> singlethreaded_nodeversion;
struct make_nodeversion {
    typedef typename mass::conditional<P::concurrent,
				       nodeversion,
				       singlethreaded_nodeversion>::type type;
};

template <typename P>
// struct value_prefetcher, do_nothing is defined in compiler.hh
struct make_prefetcher {
    typedef typename mass::conditional<P::prefetch,
				       value_prefetcher<typename P::value_type>,
				       do_nothing>::type type;
};

template <typename P>
// class basic_nodeversion<P>, basic_singlethreaded_nodeversion<P> is defined in nodeversion.hh
class node_base : public make_nodeversion<P>::type {
  public:
  // constexpr allows computations to take place at compile time
    static constexpr bool concurrent = P::concurrent; // multithread or not
    static constexpr int nikey = 1; // ? appear only once in this file
    typedef leaf<P> leaf_type;
    typedef internode<P> internode_type;
    typedef node_base<P> base_type;
    typedef typename P::value_type value_type;
    typedef leafvalue<P> leafvalue_type;
    typedef typename P::ikey_type ikey_type;
    typedef key<ikey_type> key_type;
    typedef typename make_nodeversion<P>::type nodeversion_type;
    typedef typename P::threadinfo_type threadinfo;

  // single colon followed by a initialization list
    node_base(bool isleaf)
	: nodeversion_type(isleaf) {
    }

    int size() const {
	if (this->isleaf())
	    return static_cast<const leaf_type*>(this)->size();
	else
	    return static_cast<const internode_type*>(this)->size();
    }

    inline base_type* parent() const {
        // almost always an internode
	if (this->isleaf())
	    return static_cast<const leaf_type*>(this)->parent_;
	else
	    return static_cast<const internode_type*>(this)->parent_;
    }
    static inline base_type* parent_for_layer_root(base_type* higher_layer) {
        (void) higher_layer;
        return 0;
    }
  // helper function
    static inline bool parent_exists(base_type* p) {
        return p != 0;
    }
    inline bool has_parent() const {
        return parent_exists(parent());
    }
    inline internode_type* locked_parent(threadinfo& ti) const; // defined in internode class
    inline void set_parent(base_type* p) {
	if (this->isleaf())
	    static_cast<leaf_type*>(this)->parent_ = p;
	else
	    static_cast<internode_type*>(this)->parent_ = p;
    }
    inline base_type* unsplit_ancestor() const {
	base_type* x = const_cast<base_type*>(this), *p;
        // while the root_bit of nodeversion of x is NOT set && x has parent, walk up the tree
	while (x->has_split() && (p = x->parent()))
	    x = p;
	return x;
    }
    inline leaf_type* leftmost() const {
        base_type* x = unsplit_ancestor();
        // find the leftmost leaf in subtree x
        while (!x->isleaf()) {
            internode_type* in = static_cast<internode_type*>(x);
            x = in->child_[0];
        }
        return static_cast<leaf_type*>(x);
    }
  // defined in internode class
    inline leaf_type* reach_leaf(const key_type& k, nodeversion_type& version,
                                 threadinfo& ti) const;

  // prefetch instruction is defined in compiler.hh: 565 - 584
  // CACHE_LINE_SIZE is defined in configure and is currently 64
    void prefetch_full() const {
	for (int i = 0; i < std::min(16 * std::min(P::leaf_width, P::internode_width) + 1, 4 * 64); i += 64)
	    ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);
};

template <typename P>
class internode : public node_base<P> {
  public:
    static constexpr int width = P::internode_width; // width is fanout
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;

    uint8_t nkeys_;
    ikey_type ikey0_[width];
    node_base<P>* child_[width + 1];
    node_base<P>* parent_;
    kvtimestamp_t created_at_[P::debug_level > 0];

    internode()
	: node_base<P>(false), nkeys_(0), parent_() {
    }

    static internode<P>* make(threadinfo& ti) {
      // pool_allocate is defined in kvthread.hh
      // take a multiple cache-line sized memory chunk from the pool in threadinfo
	void* ptr = ti.pool_allocate(sizeof(internode<P>),
                                     memtag_masstree_internode);
	internode<P>* n = new(ptr) internode<P>;
	assert(n);
	if (P::debug_level > 0)
	    n->created_at_[0] = ti.operation_timestamp();
	return n;
    }

    int size() const {
	return nkeys_;
    }
  // return an object of class key
    key_type get_key(int p) const {
	return key_type(ikey0_[p]);
    }
  // return the key slice directly
    ikey_type ikey(int p) const {
	return ikey0_[p];
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    void prefetch() const {
      // prefetch 4 cache-line at most
	for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
	    ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);
  // free this node by putting the memory chunk back to the pool
    void deallocate(threadinfo& ti) {
	ti.pool_deallocate(this, sizeof(*this), memtag_masstree_internode);
    }
    void deallocate_rcu(threadinfo& ti) {
	ti.pool_deallocate_rcu(this, sizeof(*this), memtag_masstree_internode);
    }

  private:
  // given the position in the ikey0_ array, put the ikey and right child in the tree
    void assign(int p, ikey_type ikey, node_base<P>* child) {
	child->set_parent(this);
	child_[p + 1] = child;
	ikey0_[p] = ikey;
    }
  // partial node copy, used when internode splits
    void shift_from(int p, const internode<P>* x, int xp, int n) {
	masstree_precondition(x != this);
	if (n) {
	    memcpy(ikey0_ + p, x->ikey0_ + xp, sizeof(ikey0_[0]) * n);
	    memcpy(child_ + p + 1, x->child_ + xp + 1, sizeof(child_[0]) * n);
	}
    }
  // ikey and child array chunk copy within a node
    void shift_up(int p, int xp, int n) {
      // with memcpy, the destination CANNOT overlap the source. with memmove it can.
	memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
	for (node_base<P> **a = child_ + p + n, **b = child_ + xp + n; n; --a, --b, --n)
	    *a = *b;
    }
    void shift_down(int p, int xp, int n) {
	memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
	for (node_base<P> **a = child_ + p + 1, **b = child_ + xp + 1; n; ++a, ++b, --n)
	    *a = *b;
    }

    int split_into(internode<P>* nr, int p, ikey_type ka, node_base<P>* value,
                   ikey_type& split_ikey, int split_type);

    template <typename PP> friend class tcursor;
};

template <typename P>
class leafvalue {
  public:
    typedef typename P::value_type value_type;
    typedef typename make_prefetcher<P>::type prefetcher_type;

    leafvalue() {
    }
    leafvalue(value_type v) {
	u_.v = v;
    }
    leafvalue(node_base<P>* n) {
	u_.x = reinterpret_cast<uintptr_t>(n);
    }

    static leafvalue<P> make_empty() {
	return leafvalue<P>(value_type());
    }

    typedef bool (leafvalue<P>::*unspecified_bool_type)() const;
    operator unspecified_bool_type() const {
	return u_.x ? &leafvalue<P>::empty : 0;
    }
    bool empty() const {
	return !u_.x;
    }

    value_type value() const {
	return u_.v;
    }
    value_type& value() {
	return u_.v;
    }

    node_base<P>* layer() const {
	return reinterpret_cast<node_base<P>*>(u_.x);
    }

   uintptr_t getX() {
	return u_.x;
   }

   void setX(unsigned int id) {
	//u_.x = reinterpret_cast<uintptr_t>(id);
	u_.x = id;
   }

    void prefetch(int keylenx) const {
	if (keylenx < 128)
	    prefetcher_type()(u_.v);
	else
	    u_.n->prefetch_full();
    }

  private:
  // link-or-value structure
  // node_base<P> ptr is usually reinterpreted and stored in u_.x which guarantees
  // the size of a pointer
    union {
	node_base<P>* n;
	value_type v;
        uintptr_t x; // an unsigned int guaranteed to be the same size as a pointer
    } u_;
};

template <typename P>
class leaf : public node_base<P> {
  public:
    static constexpr int width = P::leaf_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename node_base<P>::leafvalue_type leafvalue_type;
    typedef kpermuter<P::leaf_width> permuter_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;

    int8_t extrasize64_;
    int8_t nremoved_;
    uint8_t keylenx_[width];
  // permutation is described in section 4.6.2 Border inserts in Masstree paper
  // kpermuter<int width> is defined in kpermuter.hh
    typename permuter_type::storage_type permutation_;
    ikey_type ikey0_[width];
    leafvalue_type lv_[width];
    stringbag<uint32_t>* ksuf_;	// a real rockstar would save this space
				// when it is unsed
    union {
	leaf<P>* ptr;
	uintptr_t x;
    } next_;
    leaf<P>* prev_;
    node_base<P>* parent_;
    kvtimestamp_t node_ts_;
    kvtimestamp_t created_at_[P::debug_level > 0];
    stringbag<uint16_t> iksuf_[0];

    leaf(size_t sz, kvtimestamp_t node_ts)
	: node_base<P>(true), nremoved_(0),
	  permutation_(permuter_type::make_empty()),
	  ksuf_(), parent_(), node_ts_(node_ts), iksuf_{} {
      // masstree_precondition is defined in config.h.in
	masstree_precondition(sz % 64 == 0 && sz / 64 < 128);
        // >> 6 actually means divided by CACHE_LINE_SIZE
        // sizeof(*this) is the fixed size of every leaf node
        // extrasize64_ = # extra cacheline needed to accommodate the suffixes and values
	extrasize64_ = (int(sz) >> 6) - ((int(sizeof(*this)) + 63) >> 6);
	if (extrasize64_ > 0)
	    new((void *)&iksuf_[0]) stringbag<uint16_t>(width, sz - sizeof(*this));
    }

    static leaf<P>* make(int ksufsize, kvtimestamp_t node_ts, threadinfo& ti) {
      // # cache-lines needed to store the leaf node, maximum total suffix size is 128
	size_t sz = iceil(sizeof(leaf<P>) + std::min(ksufsize, 128), 64);
	void* ptr = ti.pool_allocate(sz, memtag_masstree_leaf);
	leaf<P>* n = new(ptr) leaf<P>(sz, node_ts);
	assert(n);
	if (P::debug_level > 0)
	    n->created_at_[0] = ti.operation_timestamp();
	return n;
    }
    static leaf<P>* make_root(int ksufsize, leaf<P>* parent, threadinfo& ti) {
        leaf<P>* n = make(ksufsize, parent ? parent->node_ts_ : 0, ti);
        n->next_.ptr = n->prev_ = 0;
        n->parent_ = node_base<P>::parent_for_layer_root(parent);
        n->mark_root();
        return n;
    }
  // return # of cache lines allocated
    size_t allocated_size() const {
	int es = (extrasize64_ >= 0 ? extrasize64_ : -extrasize64_ - 1);
	return (sizeof(*this) + es * 64 + 63) & ~size_t(63);
    }
    int size() const {
        return permuter_type::size(permutation_); // permutation & 0x1111 (lower 4 bits = nkeys)
    }
    permuter_type permutation() const {
	return permuter_type(permutation_);
    }
  // knock off MS 4 bits (locked, inserting, splitting, deleted), append 4 size bits
    typename nodeversion_type::value_type full_version_value() const {
        static_assert(int(nodeversion_type::traits_type::top_stable_bits) >= int(permuter_type::size_bits), "not enough bits to add size to version");
        // version_value is defined in nodeversion.hh, returns v_
        // size_bits = 4 defined in kpermuter.hh
        return (this->version_value() << permuter_type::size_bits) + size();
    }

    using node_base<P>::has_changed;
    bool has_changed(nodeversion_type oldv,
                     typename permuter_type::storage_type oldperm) const {
      // has_changed(oldv) is defined in nodeversion.hh
      // To determine whether a leaf node has changed or not,
      // compare version # and permutation to old ones, respectively
        return this->has_changed(oldv) || oldperm != permutation_;
    }

    key_type get_key(int p) const {
	int kl = keylenx_[p];
	if (!keylenx_has_ksuf(kl))
	    return key_type(ikey0_[p], kl);
	else
	    return key_type(ikey0_[p], ksuf(p));
    }
    ikey_type ikey(int p) const {
	return ikey0_[p];
    }
    ikey_type ikey_bound() const {
	return ikey0_[0];
    }
    int ikeylen(int p) const {
	return keylenx_ikeylen(keylenx_[p]);
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    inline leaf<P>* advance_to_key(const key_type& k, nodeversion_type& version,
                                   threadinfo& ti) const;

    bool value_is_layer(int p) const {
	return keylenx_is_layer(keylenx_[p]);
    }
    bool value_is_stable_layer(int p) const {
	return keylenx_is_stable_layer(keylenx_[p]);
    }
    bool has_ksuf(int p) const {
	return keylenx_has_ksuf(keylenx_[p]);
    }
    Str ksuf(int p) const {
	masstree_precondition(has_ksuf(p));
	return ksuf_ ? ksuf_->get(p) : iksuf_[0].get(p);
    }
  // keylenx structure:
  // | is_stable_layer | is_unstable_layer | keylen |
  // |        7        |         6        | 5 - 0  |
  // bit 7, 6 is used to distinguish between values or pointers in the value slot
    static int keylenx_ikeylen(int keylenx) {
	return keylenx & 63;
    }
    static bool keylenx_is_layer(int keylenx) {
	return keylenx > 63;
    }
    static bool keylenx_is_unstable_layer(int keylenx) {
	return keylenx & 64;
    }
    static bool keylenx_is_stable_layer(int keylenx) {
	return keylenx > 127;	// see also leafvalue
    }
    static bool keylenx_has_ksuf(int keylenx) {
	return keylenx == (int) sizeof(ikey_type) + 1;
    }
    size_t ksuf_size() const {
	if (extrasize64_ <= 0)
	    return ksuf_ ? ksuf_->size() : 0;
	else
	    return iksuf_[0].size();
    }
    bool ksuf_equals(int p, const key_type& ka) {
	// Precondition: keylenx_[p] == ka.ikeylen() && ikey0_[p] == ka.ikey()
	return ksuf_equals(p, ka, keylenx_[p]);
    }
    bool ksuf_equals(int p, const key_type& ka, int keylenx) {
	// Precondition: keylenx_[p] == ka.ikeylen() && ikey0_[p] == ka.ikey()
	return !keylenx_has_ksuf(keylenx)
	    || (!ksuf_ && iksuf_[0].equals_sloppy(p, ka.suffix()))
	    || (ksuf_ && ksuf_->equals_sloppy(p, ka.suffix()));
    }
    int ksuf_compare(int p, const key_type& ka) {
	if (!has_ksuf(p))
	    return 0;
	else if (!ksuf_)
	    return iksuf_[0].compare(p, ka.suffix());
	else
	    return ksuf_->compare(p, ka.suffix());
    }

    bool deleted_layer() const {
	return nremoved_ > width;
    }

    void prefetch() const {
	for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
	    ::prefetch((const char *) this + i);
	if (extrasize64_ > 0)
	    ::prefetch((const char *) &iksuf_[0]);
	else if (extrasize64_ < 0) {
	    ::prefetch((const char *) ksuf_);
	    ::prefetch((const char *) ksuf_ + CACHE_LINE_SIZE);
	}
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);

    leaf<P>* safe_next() const {
	return reinterpret_cast<leaf<P>*>(next_.x & ~(uintptr_t) 1);
    }

    void deallocate(threadinfo& ti) {
	if (ksuf_)
	    ti.deallocate(ksuf_, ksuf_->allocated_size(),
                              memtag_masstree_ksuffixes);
	ti.pool_deallocate(this, allocated_size(), memtag_masstree_leaf);
    }
    void deallocate_rcu(threadinfo& ti) {
	if (ksuf_)
	    ti.deallocate_rcu(ksuf_, ksuf_->allocated_size(),
                              memtag_masstree_ksuffixes);
	ti.pool_deallocate_rcu(this, allocated_size(), memtag_masstree_leaf);
    }

  private:
    inline void mark_deleted_layer() {
	nremoved_ = width + 1;
    }

    inline void assign(int p, const key_type& ka, threadinfo& ti) {
	lv_[p] = leafvalue_type::make_empty();
	if (ka.has_suffix())
	    assign_ksuf(p, ka.suffix(), false, ti);
	ikey0_[p] = ka.ikey();
	keylenx_[p] = ka.ikeylen();
    }
    inline void assign_initialize(int p, const key_type& ka, threadinfo& ti) {
	lv_[p] = leafvalue_type::make_empty();
	if (ka.has_suffix())
	    assign_ksuf(p, ka.suffix(), true, ti);
	ikey0_[p] = ka.ikey();
	keylenx_[p] = ka.ikeylen();
    }
    inline void assign_initialize(int p, leaf<P>* x, int xp, threadinfo& ti) {
	lv_[p] = x->lv_[xp];
	if (x->has_ksuf(xp))
	    assign_ksuf(p, x->ksuf(xp), true, ti);
	ikey0_[p] = x->ikey0_[xp];
	keylenx_[p] = x->keylenx_[xp];
    }
    inline void assign_ksuf(int p, Str s, bool initializing, threadinfo& ti) {
	if (extrasize64_ <= 0 || !iksuf_[0].assign(p, s))
	    hard_assign_ksuf(p, s, initializing, ti);
    }
    void hard_assign_ksuf(int p, Str s, bool initializing, threadinfo& ti);

    inline ikey_type ikey_after_insert(const permuter_type& perm, int i,
                                       const key_type& ka, int ka_i) const;
    int split_into(leaf<P>* nr, int p, const key_type& ka, ikey_type& split_ikey,
                   threadinfo& ti);

    template <typename PP> friend class tcursor;
};


template <typename P>
void basic_table<P>::initialize(threadinfo& ti) {
    masstree_precondition(!root_);
    root_ = node_type::leaf_type::make_root(0, 0, ti);
}


/** @brief Return this node's parent in locked state.
    @pre this->locked()
    @post this->parent() == result && (!result || result->locked()) */
template <typename P>
internode<P>* node_base<P>::locked_parent(threadinfo& ti) const
{
    node_base<P>* p;
    masstree_precondition(!this->concurrent || this->locked());
    while (1) {
	p = this->parent();
	if (!node_base<P>::parent_exists(p))
            break;
	nodeversion_type pv = p->lock(*p, ti.lock_fence(tc_internode_lock));
	if (p == this->parent()) {
	    masstree_invariant(!p->isleaf());
	    break;
	}
	p->unlock(pv);
	relax_fence();
    }
    return static_cast<internode<P>*>(p);
}


/** @brief Return the result of key_compare(k, LAST KEY IN NODE).

    Reruns the comparison until a stable comparison is obtained. */
template <typename P>
inline int
internode<P>::stable_last_key_compare(const key_type& k, nodeversion_type v,
                                      threadinfo& ti) const
{
    while (1) {
	int cmp = key_compare(k, *this, size() - 1);
	if (likely(!this->has_changed(v)))
	    return cmp;
	v = this->stable_annotated(ti.stable_fence());
    }
}

template <typename P>
inline int
leaf<P>::stable_last_key_compare(const key_type& k, nodeversion_type v,
                                 threadinfo& ti) const
{
    while (1) {
	typename leaf<P>::permuter_type perm(permutation_);
	int p = perm[perm.size() - 1];
	int cmp = key_compare(k, *this, p);
	if (likely(!this->has_changed(v)))
	    return cmp;
	v = this->stable_annotated(ti.stable_fence());
    }
}


/** @brief Return the leaf in this tree layer responsible for @a ka.

    Returns a stable leaf. Sets @a version to the stable version. */
template <typename P>
inline leaf<P>* node_base<P>::reach_leaf(const key_type& ka,
                                         nodeversion_type& version,
                                         threadinfo& ti) const
{
    const node_base<P> *n[2];
    typename node_base<P>::nodeversion_type v[2];
    bool sense;

    // Get a non-stale root.
    // Detect staleness by checking whether n has ever split.
    // The true root has never split.
 retry:
    sense = false;
    n[sense] = this;
    while (1) {
      // stable_annotated is defined in nodeversion.hh
      // basically it acquire_fence() if node is NOT dirty
	v[sense] = n[sense]->stable_annotated(ti.stable_fence());
	if (!v[sense].has_split())
	    break;
	ti.mark(tc_root_retry);
	n[sense] = n[sense]->unsplit_ancestor();
    }

    // Loop over internal nodes.
    while (!v[sense].isleaf()) {
	const internode<P> *in = static_cast<const internode<P> *>(n[sense]);
	in->prefetch();
	int kp = internode<P>::bound_type::upper(ka, *in);
	n[!sense] = in->child_[kp];
	if (!n[!sense])
	    goto retry;
	v[!sense] = n[!sense]->stable_annotated(ti.stable_fence());

	if (likely(!in->has_changed(v[sense]))) {
	    sense = !sense;
	    continue;
	}

	typename node_base<P>::nodeversion_type oldv = v[sense];
	v[sense] = in->stable_annotated(ti.stable_fence());
	if (oldv.has_split(v[sense])
	    && in->stable_last_key_compare(ka, v[sense], ti) > 0) {
	    ti.mark(tc_root_retry);
	    goto retry;
	} else
	    ti.mark(tc_internode_retry);
    }

    version = v[sense];
    return const_cast<leaf<P> *>(static_cast<const leaf<P> *>(n[sense]));
}

/** @brief Return the leaf at or after *this responsible for @a ka.
    @pre *this was responsible for @a ka at version @a v

    Checks whether *this has split since version @a v. If it has split, then
    advances through the leaves using the B^link-tree pointers and returns
    the relevant leaf, setting @a v to the stable version for that leaf. */
template <typename P>
leaf<P>* leaf<P>::advance_to_key(const key_type& ka, nodeversion_type& v,
                                 threadinfo& ti) const
{
    const leaf<P>* n = this;
    nodeversion_type oldv = v;
    v = n->stable_annotated(ti.stable_fence());
    if (v.has_split(oldv)
	&& n->stable_last_key_compare(ka, v, ti) > 0) {
	leaf<P> *next;
	ti.mark(tc_leaf_walk);
	while (likely(!v.deleted()) && (next = n->safe_next())
	       && compare(ka.ikey(), next->ikey_bound()) >= 0) {
	    n = next;
	    v = n->stable_annotated(ti.stable_fence());
	}
    }
    return const_cast<leaf<P>*>(n);
}


/** @brief Assign position @a p's keysuffix to @a s.

    This version of assign_ksuf() is called when @a s might not fit into
    the current keysuffix container. It may allocate a new container, copying
    suffixes over.

    The @a initializing parameter determines which suffixes are copied. If @a
    initializing is false, then this is an insertion into a live node. The
    live node's permutation indicates which keysuffixes are active, and only
    active suffixes are copied. If @a initializing is true, then this
    assignment is part of the initialization process for a new node. The
    permutation might not be set up yet. In this case, it is assumed that key
    positions [0,p) are ready: keysuffixes in that range are copied. In either
    case, the key at position p is NOT copied; it is assigned to @a s. */
template <typename P>
void leaf<P>::hard_assign_ksuf(int p, Str s, bool initializing,
			       threadinfo& ti) {
    if (ksuf_ && ksuf_->assign(p, s)) {
		ti.ksufSize += s.len;
		//std::cout << s << "\n" << s.len << "\n";
		return;
    }
    ti.ksufSize += s.len;

    stringbag<uint16_t> *iksuf;
    stringbag<uint32_t> *oksuf;
    if (extrasize64_ > 0)
	iksuf = &iksuf_[0], oksuf = 0;
    else
	iksuf = 0, oksuf = ksuf_;

    size_t csz;
    if (iksuf)
	csz = iksuf->allocated_size() - iksuf->overhead(width);
    else if (oksuf)
	csz = oksuf->allocated_size() - oksuf->overhead(width);
    else
	csz = 0;
    size_t sz = iceil_log2(std::max(csz, size_t(4 * width)) * 2);
    //size_t before = sz;
	//std::cout << s << "\n" << s.len << "\n";
	//std::cout<<"csz: "<<csz<<", ";
    //std::cout<<"sz (before while): " << sz <<", Str len: "<< s.len << ", ";
    while (sz < csz + stringbag<uint32_t>::overhead(width) + s.len)
	sz *= 2;
    //size_t after = sz;
    //std::cout<<"sz (after while): " << sz << ", " << "\n";

    //if( before != after) {
      //std::cout<<"csz: "<<csz<<", sz (before while): " << before <<", Str len: "<< s.len << ", sz (after while): " << after << "\n";
    //}


    void *ptr = ti.allocate(sz, memtag_masstree_ksuffixes);
    	
    //hyw
    double base = 2.0;
    double arg = sz;
    int idx =(int)(std::log(arg) / std::log(base));
    ti.allocDist[idx - 1] += 1;
    ti.ksufAllocSize += sz;
    
    //std::cout << sz << "\n";

    stringbag<uint32_t> *nksuf = new(ptr) stringbag<uint32_t>(width, sz);
    permuter_type perm(permutation_);
    int n = initializing ? p : perm.size();
    for (int i = 0; i < n; ++i) {
	int mp = initializing ? i : perm[i];
	if (mp != p && has_ksuf(mp)) {
	    bool ok = nksuf->assign(mp, ksuf(mp));
            assert(ok); (void) ok;
        }
    }
    bool ok = nksuf->assign(p, s);
    assert(ok); (void) ok;
    //std::cout<< "real sufix size: " << nksuf->size()<< "\n";
    fence();

    if (nremoved_ > 0)		// removed ksufs are not copied to the new ksuf,
	this->mark_insert();	// but observers might need removed ksufs:
				// make them retry

    ksuf_ = nksuf;
    fence();

    if (extrasize64_ >= 0)	// now the new ksuf_ installed, mark old dead
	extrasize64_ = -extrasize64_ - 1;

    if (oksuf) {
	ti.ksufAllocSize -= oksuf->allocated_size();
	arg = oksuf->allocated_size();
    idx =(int)(std::log(arg) / std::log(base));
    ti.allocDist[idx - 1] -= 1;
    ti.deallocSize += oksuf->allocated_size(); 

	ti.deallocate_rcu(oksuf, oksuf->allocated_size(),
                          memtag_masstree_ksuffixes);
	}
}

//hyw
template <typename P>
inline void basic_table<P>::set_static_Root(node_type* nRoot) {
    root_ = nRoot;
}

//hyw
template <typename P>
inline node_base<P>* basic_table<P>::static_root() const {
    return static_root_;
}

template <typename P>
inline basic_table<P>::basic_table()
    : root_(0) {
}

template <typename P>
inline node_base<P>* basic_table<P>::root() const {
    return root_;
}

template <typename P>
inline node_base<P>* basic_table<P>::fix_root() {
    node_base<P>* root = root_;
    if (unlikely(root->has_split())) {
        node_base<P>* old_root = root;
        root = root->unsplit_ancestor();
        (void) cmpxchg(&root_, old_root, root);
    }
    return root;
}

// Huanchen's massnode class

template <typename P>
class massnode : public node_base<P> {
public:
  typedef typename P::ikey_type ikey_type;
  typedef key<typename P::ikey_type> key_type;
  typedef typename node_base<P>::leafvalue_type leafvalue_type;
  typedef typename P::threadinfo_type threadinfo;

  uint32_t nkeys_;
  uint32_t size_;
  uint8_t* keylenx_;
  ikey_type* ikey0_;
  leafvalue_type* lv_;
  uint32_t* ksuf_pos_offset_;
  char* ksuf_;

  massnode (uint32_t nkeys)
    :node_base<P>(false), nkeys_(nkeys) {
    keylenx_ = (uint8_t*)((char*)this + sizeof(massnode<P>));
	//keylenx_ = (uint8_t*)content_[0];
    ikey0_ = (ikey_type*)((char*)keylenx_ + nkeys_ * sizeof(uint8_t));
    lv_ = (leafvalue_type*)((char*)ikey0_ + nkeys_ * sizeof(ikey_type));
    ksuf_pos_offset_ = (uint32_t*)((char*)lv_ + nkeys_ * sizeof(leafvalue_type));
    ksuf_ = (char*)((char*)ksuf_pos_offset_ + (nkeys_ + 1) * sizeof(uint32_t));
  }

  static massnode<P>* make (size_t ksufSize, uint32_t nkeys, threadinfo& ti) {
    size_t sz = sizeof(massnode<P>) + sizeof(ikey_type) * nkeys + sizeof(uint8_t) * nkeys + sizeof(leafvalue_type) * (nkeys + 1) + sizeof(uint32_t) * nkeys + ksufSize;
    //std::cout << "massnode = " << sizeof(massnode<P>) << "\n";
    //std::cout << "ikey_type = " << sizeof(ikey_type) << "\n";
    std::cout << "nkeys = " << nkeys << "\t";
    //std::cout << "leafvalue_type = " << sizeof(leafvalue_type) << "\n";
    std::cout << "size = " << sz << "\n";
    void* ptr = ti.allocate(sz, memtag_masstree_leaf);
    massnode<P>* n = new(ptr) massnode<P>(nkeys);
    assert(n);
    return n;
  }

  size_t allocated_size() const {
    return size_;
  }

  uint32_t size() const {
    return nkeys_;
  }
  
  key_type get_key(int p) const {
    int kl = keylenx_[p];
    if (!keylenx_has_ksuf(kl))
      return key_type(ikey0_[p], kl);
    else
      return key_type(ikey0_[p], ksuf(p));
  }
  
  ikey_type ikey(int p) const {
    return ikey0_[p];
  }

  int ikeylen(int p) const {
    return keylenx_[p];
  }

  char* ksufPos(int p) const {
    return (char*)(ksuf_ + ksuf_pos_offset_[p]);
  }

  uint32_t ksufLen(int p) const {
    return ksuf_pos_offset_[p+1] - ksuf_pos_offset_[p];
  }

  Str ksuf(int p) const {
    return Str(ksufPos(p), ksufLen(p));
  }

  size_t ksuf_size() const {
    return (size_t)ksuf_pos_offset_[nkeys_];
  }

  bool has_ksuf(int p) const {
    return ksuf_pos_offset_[p] != 0;
  }
  static bool keylenx_is_layer(int keylenx) {
    return keylenx > 63;
  }
  static bool keylenx_is_unstable_layer(int keylenx) {
    return keylenx & 64;
  }
  static bool keylenx_is_stable_layer(int keylenx) {
    return keylenx > 127;   // see also leafvalue
  }

  static bool keylenx_has_ksuf(int keylenx) {
    return keylenx == (int) sizeof(ikey_type) + 1;
  }

  bool ksuf_equals(int p, const key_type& ka) {
   return ksuf_equals(p, ka, keylenx_[p]);
  }
  
  bool ksuf_equals(int p, const key_type& ka, int keylenx) {
    
    return !keylenx_has_ksuf(keylenx) || equals_sloppy(p, ka);
  }

  bool equals_sloppy(int p, const key_type& ka) {
    Str thisKsuf = ksuf(p);
    if(thisKsuf.len != ka.suffix().len) return false;
    return string_slice<uintptr_t>::equals_sloppy(thisKsuf.s, ka.suffix().s, ka.suffix().len);
  }

  int ksuf_compare(int p, const key_type& ka) {
    //TODO
    return 0;
  }

  void prefetch() const {
    //TODO
    for (int i = 64; i < std::min((int)size_, 4 * 64); i += 64)
      ::prefetch((const char *) this + i);
    if (ksuf_) {
      ::prefetch((const char *) ksuf_);
      ::prefetch((const char *) ksuf_ + CACHE_LINE_SIZE);
    }
  }

  void deallocate(threadinfo& ti) {
    //TODO
  }

private:
    template <typename PP> friend class tcursor;
};



} // namespace Masstree
#endif

