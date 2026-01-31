#pragma once
/*
  reactive_two_field_collection.h

  Reactive two-field collection (single-file header).
  
  Phase 2: Uses Intel TBB concurrent_hash_map for lock-free map operations.
  Phase 3: Uses std::shared_mutex for concurrent ordered index (multiple readers, single writer).
*/

//==============================================================================
// INCLUDES
//==============================================================================

#include <algorithm>
#include <atomic>
#include <vector>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <variant>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <set>
#include <memory>
#include <limits>
#include <functional>
#include <reaction/reaction.h>
#include <oneapi/tbb/concurrent_hash_map.h>

namespace reactive {

//==============================================================================
// FORWARD DECLARATIONS & ENUMS
//==============================================================================

enum class AggMode { Add, Min, Max };

//==============================================================================
// DETAIL NAMESPACE - HELPER FUNCTORS & UTILITIES
//==============================================================================

namespace detail {

// DefaultDelta1: Δ = new2 - last2 (typical)
template <typename Elem1T, typename Elem2T, typename Total1T = Elem2T>
struct DefaultDelta1 {
    using DeltaType = Total1T;
    constexpr DeltaType operator()(const Elem1T& /*new1*/, const Elem2T &new2,
                                   const Elem1T& /*last1*/, const Elem2T &last2) const noexcept {
        return static_cast<DeltaType>(new2 - last2);
    }
};

// DefaultDelta2: Δ = new2*new1 - last2*last1
template <typename Elem1T, typename Elem2T, typename Total2T = Elem1T>
struct DefaultDelta2 {
    using DeltaType = Total2T;
    constexpr DeltaType operator()(const Elem1T &new1, const Elem2T &new2,
                                   const Elem1T &last1, const Elem2T &last2) const noexcept {
        return static_cast<DeltaType>(static_cast<Total2T>(new2) * static_cast<Total2T>(new1)
                                      - static_cast<Total2T>(last2) * static_cast<Total2T>(last1));
    }
};

// DefaultApply: addition helper (used by fast path)
template <typename TotalT, typename DeltaT = TotalT>
struct DefaultApplyAdd {
    using DeltaType = DeltaT;
    constexpr bool operator()(TotalT &total, const DeltaT &d) const noexcept {
        total += d;
        return true;
    }
};

// Noop functors
template <typename Elem1T, typename Elem2T, typename TotalT>
struct NoopDelta {
    using DeltaType = TotalT;
    constexpr DeltaType operator()(const Elem1T&, const Elem2T&, const Elem1T&, const Elem2T&) const noexcept {
        return DeltaType{};
    }
};

template <typename TotalT, typename DeltaT = TotalT>
struct NoopApply {
    using DeltaType = DeltaT;
    constexpr bool operator()(TotalT& /*total*/, const DeltaT& /*d*/) const noexcept {
        return false;
    }
};

// SetApply: set total = incoming delta (interpreted as new value)
template <typename TotalT, typename DeltaT = TotalT>
struct SetApply {
    using DeltaType = DeltaT;
    constexpr bool operator()(TotalT &total, const DeltaT &d) const noexcept {
        TotalT v = static_cast<TotalT>(d);
        if (total == v) return false;
        total = v;
        return true;
    }
};

// SaturatingApply: add then clamp
template <typename TotalT, typename DeltaT = TotalT>
struct SaturatingApply {
    using DeltaType = DeltaT;
    TotalT minv;
    TotalT maxv;
    SaturatingApply(TotalT lo = std::numeric_limits<TotalT>::lowest(), TotalT hi = std::numeric_limits<TotalT>::max())
        : minv(lo), maxv(hi) {}
    bool operator()(TotalT &total, const DeltaT &d) const noexcept {
        TotalT nv = total + static_cast<TotalT>(d);
        if (nv < minv) nv = minv;
        if (nv > maxv) nv = maxv;
        if (nv == total) return false;
        total = nv;
        return true;
    }
};

// Deduce delta type: prefer DeltaFn::DeltaType if present, otherwise fallback to TotalT
template <typename TotalT, typename DeltaFn>
using deduced_delta_t = std::conditional_t<
    std::is_class_v<DeltaFn> && !std::is_same_v<void, typename DeltaFn::DeltaType>,
    typename DeltaFn::DeltaType,
    TotalT>;

} // namespace detail

// ============================================================================
// Main Class Declaration
// ============================================================================

// Default extractors
template <typename Elem1T, typename Elem2T, typename Total1T>
struct DefaultExtract1 {
    constexpr Total1T operator()(const Elem1T&, const Elem2T &e2) const noexcept {
        return static_cast<Total1T>(e2);
    }
};
template <typename Elem1T, typename Elem2T, typename Total2T>
struct DefaultExtract2 {
    constexpr Total2T operator()(const Elem1T &e1, const Elem2T &e2) const noexcept {
        return static_cast<Total2T>(static_cast<Total2T>(e2) * static_cast<Total2T>(e1));
    }
};

// Default comparator (lexicographic by elem1 then elem2)
template <typename Elem1T, typename Elem2T>
struct DefaultCompare {
    bool operator()(const Elem1T &a1, const Elem2T &a2, const Elem1T &b1, const Elem2T &b2) const noexcept {
        if (a1 < b1) return true;
        if (b1 < a1) return false;
        return a2 < b2;
    }
};

//==============================================================================
// MAIN CLASS - REACTIVE TWO-FIELD COLLECTION
//==============================================================================

template <
    typename Elem1T = double,
    typename Elem2T = long,
    typename Total1T = Elem2T,
    typename Total2T = double,
    typename Delta1Fn = detail::DefaultDelta1<Elem1T, Elem2T, Total1T>,
    typename Apply1Fn = detail::DefaultApplyAdd<Total1T, detail::deduced_delta_t<Total1T, Delta1Fn>>,
    typename Delta2Fn = detail::DefaultDelta2<Elem1T, Elem2T, Total2T>,
    typename Apply2Fn = detail::DefaultApplyAdd<Total2T, detail::deduced_delta_t<Total2T, Delta2Fn>>,
    typename KeyT = std::monostate,
    AggMode Total1Mode = AggMode::Add,
    AggMode Total2Mode = AggMode::Add,
    typename Extract1Fn = DefaultExtract1<Elem1T, Elem2T, Total1T>,
    typename Extract2Fn = DefaultExtract2<Elem1T, Elem2T, Total2T>,
    bool RequireCoarseLock = false,
    bool MaintainOrderedIndex = false,
    typename CompareFn = DefaultCompare<Elem1T, Elem2T>,
    template <typename...> class MapType = std::unordered_map
>
class ReactiveTwoFieldCollection {
public:
    using elem1_type = Elem1T;
    using elem2_type = Elem2T;
    using total1_type = Total1T;
    using total2_type = Total2T;
    using id_type = std::size_t;
    using key_type = KeyT;

    using delta1_type = detail::deduced_delta_t<Total1T, Delta1Fn>;
    using delta2_type = detail::deduced_delta_t<Total2T, Delta2Fn>;

    static_assert(std::is_default_constructible_v<elem1_type>, "Elem1T must be default-constructible");
    static_assert(std::is_default_constructible_v<elem2_type>, "Elem2T must be default-constructible");

    // runtime comparator function type (accepts elem1, elem2 pairs for two elements)
    using compare_fn_t = std::function<bool(const elem1_type&, const elem2_type&, const elem1_type&, const elem2_type&)>;

    // Per-element record
    struct ElemRecord {
        reaction::Var<elem1_type> elem1Var;
        reaction::Var<elem2_type> elem2Var;
        elem1_type lastElem1{};
        elem2_type lastElem2{};
        using key_storage_t = KeyT;  // KeyT is now always monostate or a real type
        key_storage_t key{};

        ElemRecord() = default;
        ElemRecord(reaction::Var<elem1_type> a, reaction::Var<elem2_type> b, key_storage_t k = key_storage_t{})
            : elem1Var(std::move(a)), elem2Var(std::move(b)),
              lastElem1(elem1Var.get()), lastElem2(elem2Var.get()), key(std::move(k)) {}
    };
    
    // Snapshot of ElemRecord data for ordered iteration (no reactive vars)
    struct ElemRecordSnapshot {
        elem1_type lastElem1;
        elem2_type lastElem2;
        typename ElemRecord::key_storage_t key;
    };

    // Use TBB concurrent_hash_map for lock-free map operations
    using elem_map_type = oneapi::tbb::concurrent_hash_map<id_type, ElemRecord>;
    using monitor_map_type = oneapi::tbb::concurrent_hash_map<id_type, reaction::Action<>>;
    
    // key_index always uses concurrent_hash_map (KeyT is monostate when no key needed)
    using key_index_map_type = oneapi::tbb::concurrent_hash_map<KeyT, id_type>;
    
    // Helper to check if keys are used (not monostate)
    static constexpr bool has_keys = !std::is_same_v<KeyT, std::monostate>;
    
    // Legacy type aliases for compatibility  
    using map_type = elem_map_type;
    using iterator = typename elem_map_type::iterator;
    using const_iterator = typename elem_map_type::const_iterator;
    using lock_type = std::unique_lock<std::mutex>;

    // -------- Ordered-index support types (must be declared early) ----------
    // IdComparator: calls runtime compare_fn_t on element snapshots; tie-break by id
    struct IdComparator {
        const ReactiveTwoFieldCollection *parent;
        compare_fn_t cmp;
        IdComparator() : parent(nullptr), cmp() {}
        IdComparator(const ReactiveTwoFieldCollection *p, compare_fn_t c) : parent(p), cmp(std::move(c)) {}
        bool operator()(const id_type &a, const id_type &b) const {
            if (a == b) return false;
            // Use concurrent_hash_map accessor for thread-safe lookup
            typename elem_map_type::const_accessor acc_a, acc_b;
            if (!parent->elems_.find(acc_a, a) || !parent->elems_.find(acc_b, b)) {
                return a < b; // fallback if element not found
            }
            const ElemRecord &A = acc_a->second;
            const ElemRecord &B = acc_b->second;
            if (cmp(A.lastElem1, A.lastElem2, B.lastElem1, B.lastElem2)) return true;
            if (cmp(B.lastElem1, B.lastElem2, A.lastElem1, A.lastElem2)) return false;
            return a < b;
        }
    };
    using ordered_set_type = std::set<id_type, IdComparator>;
    // -----------------------------------------------------------------------

    /*
      Constructor:
        d1,a1,d2,a2 : optional functors
        combined_atomic : if true, updates to both totals are applied together and notify once
        coarse_lock (runtime) : respected only when RequireCoarseLock == false
    */
    ReactiveTwoFieldCollection(Delta1Fn d1 = Delta1Fn{}, Apply1Fn a1 = Apply1Fn{},
                               Delta2Fn d2 = Delta2Fn{}, Apply2Fn a2 = Apply2Fn{},
                               bool combined_atomic = false,
                               bool coarse_lock = false)
        : total1_(reaction::var(total1_type{})),
          total2_(reaction::var(total2_type{})),
          delta1_(std::move(d1)), apply1_(std::move(a1)),
          delta2_(std::move(d2)), apply2_(std::move(a2)),
          extract1_(), extract2_(),
          // initialize runtime comparator from the compile-time CompareFn default
          cmp_(CompareFn{}),
          coarse_lock_enabled_(RequireCoarseLock ? true : coarse_lock), // initialize in same order as member decl
          combined_atomic_(combined_atomic),
          nextId_(1)
    {
        // Initialize ordered index after elems_ exists (ordered_index_ declared after elems_).
        // Phase 3: std::shared_mutex allows concurrent reads
        if constexpr (MaintainOrderedIndex) {
            std::unique_lock<std::shared_mutex> lock(ordered_mtx_);
            ordered_index_.emplace(IdComparator(this, cmp_));
        }
    }

    // Destructor: explicit teardown to avoid comparator use-after-free during implicit member destruction.
    ~ReactiveTwoFieldCollection() {
        // Destroy monitors first so callbacks won't try to access state during destruction.
        try {
            monitors_.clear();
        } catch (...) {}

        // Destroy ordered index while elems_ and other members are still alive.
        // Phase 3: Use unique_lock for destruction
        try {
            std::unique_lock<std::shared_mutex> lock(ordered_mtx_);
            ordered_index_.reset();
        } catch (...) {}

        // Clear key index if present.
        if constexpr (!std::is_same_v<KeyT, std::monostate>) {
            try { key_index_.clear(); } catch (...) {}
        }
    }

    // Replace the stored comparator (any callable convertible to compare_fn_t) and rebuild the ordered index atomically.
    template <typename NewCompare>
    void set_compare(NewCompare new_cmp) {
        // Update stored comparator with the same coarse-lock policy used elsewhere
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            cmp_ = compare_fn_t(new_cmp);
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                cmp_ = compare_fn_t(new_cmp);
            } else {
                cmp_ = compare_fn_t(new_cmp);
            }
        }

        if constexpr (MaintainOrderedIndex) {
            // Phase 3: unique_lock for write operations
            std::unique_lock<std::shared_mutex> lock(ordered_mtx_);
            std::optional<ordered_set_type> new_set;
            new_set.emplace(IdComparator(this, cmp_));
            // Iterate over concurrent hash map
            for (typename elem_map_type::const_iterator it = elems_.begin(); it != elems_.end(); ++it) {
                new_set->insert(it->first);
            }
            ordered_index_.swap(new_set);
            // new_set (previous ordered_index_) destructs here
        }
    }

    // Rebuild the ordered index using the current runtime comparator (cmp_).
    // Useful when element state was bulk-updated or comparator semantics are unchanged.
    void rebuild_ordered_index() {
        if constexpr (!MaintainOrderedIndex) return;
        // Phase 3: unique_lock for write operations
        std::unique_lock<std::shared_mutex> lock(ordered_mtx_);
        std::optional<ordered_set_type> new_set;
        new_set.emplace(IdComparator(this, cmp_));
        for (typename elem_map_type::const_iterator it = elems_.begin(); it != elems_.end(); ++it) {
            new_set->insert(it->first);
        }
        ordered_index_.swap(new_set);
    }

    // Acquire coarse-grained lock (owns the lock only if coarse locking active)
    lock_type lock_public() {
        if constexpr (RequireCoarseLock) return lock_type(coarse_mtx_);
        return coarse_lock_enabled_ ? lock_type(coarse_mtx_) : lock_type(coarse_mtx_, std::defer_lock);
    }

    // push
    id_type push_back(const elem1_type &e1, const elem2_type &e2) {
        auto lk = maybe_lock();
        return push_one(e1, e2, typename ElemRecord::key_storage_t{});
    }
    id_type push_back(elem1_type &&e1, elem2_type &&e2) {
        auto lk = maybe_lock();
        return push_one(std::move(e1), std::move(e2), typename ElemRecord::key_storage_t{});
    }
    // push with key
    id_type push_back(const elem1_type &e1, const elem2_type &e2, typename ElemRecord::key_storage_t key) {
        auto lk = maybe_lock();
        return push_one(e1, e2, std::move(key));
    }

    // batch push
    void push_back(const std::vector<std::pair<elem1_type, elem2_type>> &vals, const std::vector<key_type> *keys = nullptr) {
        auto lk = maybe_lock();
        if (vals.empty()) return;
        reaction::batchExecute([this, &vals, keys]() {
            for (size_t i = 0; i < vals.size(); ++i) {
                if constexpr (std::is_same_v<KeyT, std::monostate>) {
                    push_one_no_batch(vals[i].first, vals[i].second, typename ElemRecord::key_storage_t{});
                } else {
                    typename ElemRecord::key_storage_t k = (keys && i < keys->size()) ? (*keys)[i] : typename ElemRecord::key_storage_t{};
                    push_one_no_batch(vals[i].first, vals[i].second, std::move(k));
                }
            }
        });
    }

    // erase by id
    void erase(id_type id) {
        auto lk = maybe_lock();
        
        // Use concurrent_hash_map accessor for thread-safe access
        typename elem_map_type::accessor acc;
        if (!elems_.find(acc, id)) return;
        
        ElemRecord &rec = acc->second;

        std::optional<total1_type> old_ext1;
        std::optional<total2_type> old_ext2;
        if constexpr (Total1Mode != AggMode::Add) old_ext1 = extract1_(rec.lastElem1, rec.lastElem2);
        if constexpr (Total2Mode != AggMode::Add) old_ext2 = extract2_(rec.lastElem1, rec.lastElem2);

        delta1_type rem1 = delta1_(elem1_type{}, elem2_type{}, rec.lastElem1, rec.lastElem2);
        delta2_type rem2 = delta2_(elem1_type{}, elem2_type{}, rec.lastElem1, rec.lastElem2);
        
        // Store key before releasing accessor
        typename ElemRecord::key_storage_t key_to_erase;
        if constexpr (!std::is_same_v<KeyT, std::monostate>) {
            key_to_erase = rec.key;
        }
        
        // Release accessor before further operations
        acc.release();

        if constexpr (MaintainOrderedIndex) {
            // Phase 3: unique_lock for write operations
            std::unique_lock<std::shared_mutex> lock(ordered_mtx_);
            if (ordered_index_) {
                ordered_index_->erase(id);
            }
        }

        apply_pair(rem1, rem2,
                   /*have_old1*/ bool(old_ext1), old_ext1 ? &*old_ext1 : nullptr,
                   /*have_new1*/ false, nullptr,
                   /*have_old2*/ bool(old_ext2), old_ext2 ? &*old_ext2 : nullptr,
                   /*have_new2*/ false, nullptr);

        if constexpr (!std::is_same_v<KeyT, std::monostate>) {
            key_index_.erase(key_to_erase);
        }

        // Erase monitor
        typename monitor_map_type::accessor mon_acc;
        if (monitors_.find(mon_acc, id)) {
            mon_acc->second.close();
            monitors_.erase(mon_acc);
        }
        
        // Finally erase element
        elems_.erase(id);
        
        // Decrement atomic element counter
        elem_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    // erase by key (enabled if KeyT != void)
    template <typename K = KeyT>
    std::enable_if_t<!std::is_same_v<K, std::monostate>, void>
    erase_by_key(const K &k) {
        auto lk = maybe_lock();
        typename key_index_map_type::const_accessor acc;
        if (key_index_.find(acc, k)) {
            id_type id = acc->second;
            acc.release();
            erase(id);
        }
    }

    // find_by_key (fast) - enabled if KeyT != void, lock-free with concurrent_hash_map
    template <typename K = KeyT>
    [[nodiscard]] std::enable_if_t<!std::is_same_v<K, std::monostate>, std::optional<id_type>>
    find_by_key(const K &k) const {
        std::optional<id_type> res;
        if constexpr (!std::is_same_v<K, std::monostate>) {
            typename key_index_map_type::const_accessor acc;
            if (key_index_.find(acc, k)) {
                res = acc->second;
            }
        }
        return res;
    }

    // linear fallback find_by_key (enabled if KeyT != void)
    template <typename K = KeyT>
    [[nodiscard]] std::enable_if_t<!std::is_same_v<K, std::monostate>, std::optional<id_type>>
    find_by_key_linear(const K &k) const {
        // Iterate over concurrent_hash_map
        for (typename elem_map_type::const_iterator it = elems_.begin(); it != elems_.end(); ++it) {
            if (it->second.key == k) return it->first;
        }
        return std::nullopt;
    }

    // Var accessors - thread-safe with concurrent_hash_map
    reaction::Var<elem1_type> &elem1Var(id_type id) {
        auto lk = maybe_lock();
        typename elem_map_type::accessor acc;
        if (elems_.find(acc, id)) {
            return acc->second.elem1Var;
        }
        throw std::out_of_range("elem1Var: id not found");
    }
    reaction::Var<elem2_type> &elem2Var(id_type id) {
        auto lk = maybe_lock();
        typename elem_map_type::accessor acc;
        if (elems_.find(acc, id)) {
            return acc->second.elem2Var;
        }
        throw std::out_of_range("elem2Var: id not found");
    }

    // totals - optimized: when coarse lock not enabled, direct read (reaction::Var handles thread-safety)
    [[nodiscard]] total1_type total1() const {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            return total1_.get();
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                return total1_.get();
            } else {
                // Lock-free fast path - reaction::Var is thread-safe
                return total1_.get();
            }
        }
    }
    [[nodiscard]] total2_type total2() const {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            return total2_.get();
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                return total2_.get();
            } else {
                // Lock-free fast path - reaction::Var is thread-safe
                return total2_.get();
            }
        }
    }
    [[nodiscard]] reaction::Var<total1_type> &total1Var() { return total1_; }
    [[nodiscard]] reaction::Var<total2_type> &total2Var() { return total2_; }

    // size / empty - lock-free using atomic counter
    [[nodiscard]] size_t size() const noexcept {
        return elem_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool empty() const noexcept {
        return elem_count_.load(std::memory_order_relaxed) == 0;
    }

    // Basic iteration over the underlying map (id -> ElemRecord)
    iterator begin() { auto lk = maybe_lock(); return elems_.begin(); }
    iterator end()   { auto lk = maybe_lock(); return elems_.end(); }
    const_iterator begin() const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.begin(); }
    const_iterator end()   const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.end(); }
    const_iterator cbegin() const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.cbegin(); }
    const_iterator cend()   const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.cend(); }

    //==============================================================================
    // ORDERED INDEX ITERATORS
    //==============================================================================
public:
    using ordered_underlying_it = typename ordered_set_type::iterator;
    using ordered_underlying_const_it = typename ordered_set_type::const_iterator;
    using ordered_underlying_rit = typename ordered_set_type::reverse_iterator;
    using ordered_underlying_const_rit = typename ordered_set_type::const_reverse_iterator;

    // Forward iterator over ordered index (const)
    class OrderedConstIterator {
        ordered_underlying_const_it it_;
        const ReactiveTwoFieldCollection *parent_;
    public:
        OrderedConstIterator() : it_(), parent_(nullptr) {}
        OrderedConstIterator(const ReactiveTwoFieldCollection *p, ordered_underlying_const_it it) : it_(it), parent_(p) {}

        OrderedConstIterator& operator++() { ++it_; return *this; }
        OrderedConstIterator operator++(int) { OrderedConstIterator tmp = *this; ++it_; return tmp; }

        bool operator==(const OrderedConstIterator &o) const {
            if (parent_ == nullptr && o.parent_ == nullptr) return true;
            if (parent_ != o.parent_) return false;
            return it_ == o.it_;
        }
        bool operator!=(const OrderedConstIterator &o) const { return !(*this == o); }

        std::pair<id_type, ElemRecordSnapshot> operator*() const {
            id_type id = *it_;
            typename elem_map_type::const_accessor acc;
            if (parent_->elems_.find(acc, id)) {
                return { id, {acc->second.lastElem1, acc->second.lastElem2, acc->second.key} };
            }
            return { id, {} };
        }
    };

    // Forward iterator over ordered index (mutable)
    class OrderedIterator {
        ordered_underlying_it it_;
        ReactiveTwoFieldCollection *parent_;
    public:
        OrderedIterator() : it_(), parent_(nullptr) {}
        OrderedIterator(ReactiveTwoFieldCollection *p, ordered_underlying_it it) : it_(it), parent_(p) {}

        OrderedIterator& operator++() { ++it_; return *this; }
        OrderedIterator operator++(int) { OrderedIterator tmp = *this; ++it_; return tmp; }

        bool operator==(const OrderedIterator &o) const {
            if (parent_ == nullptr && o.parent_ == nullptr) return true;
            if (parent_ != o.parent_) return false;
            return it_ == o.it_;
        }
        bool operator!=(const OrderedIterator &o) const { return !(*this == o); }

        std::pair<id_type, ElemRecordSnapshot> operator*() const {
            id_type id = *it_;
            typename elem_map_type::const_accessor acc;
            if (parent_->elems_.find(acc, id)) {
                return { id, {acc->second.lastElem1, acc->second.lastElem2, acc->second.key} };
            }
            return { id, {} }; // fallback if not found
        }
    };

    // Reverse iterators
    class OrderedConstReverseIterator {
        ordered_underlying_const_rit it_;
        const ReactiveTwoFieldCollection *parent_;
    public:
        OrderedConstReverseIterator() : it_(), parent_(nullptr) {}
        OrderedConstReverseIterator(const ReactiveTwoFieldCollection *p, ordered_underlying_const_rit it) : it_(it), parent_(p) {}

        OrderedConstReverseIterator& operator++() { ++it_; return *this; }
        OrderedConstReverseIterator operator++(int) { OrderedConstReverseIterator tmp = *this; ++it_; return tmp; }

        bool operator==(const OrderedConstReverseIterator &o) const {
            if (parent_ == nullptr && o.parent_ == nullptr) return true;
            if (parent_ != o.parent_) return false;
            return it_ == o.it_;
        }
        bool operator!=(const OrderedConstReverseIterator &o) const { return !(*this == o); }

        std::pair<id_type, ElemRecordSnapshot> operator*() const {
            id_type id = *it_;
            typename elem_map_type::const_accessor acc;
            if (parent_->elems_.find(acc, id)) {
                return { id, {acc->second.lastElem1, acc->second.lastElem2, acc->second.key} };
            }
            return { id, {} };
        }
    };

    class OrderedReverseIterator {
        ordered_underlying_rit it_;
        ReactiveTwoFieldCollection *parent_;
    public:
        OrderedReverseIterator() : it_(), parent_(nullptr) {}
        OrderedReverseIterator(ReactiveTwoFieldCollection *p, ordered_underlying_rit it) : it_(it), parent_(p) {}

        OrderedReverseIterator& operator++() { ++it_; return *this; }
        OrderedReverseIterator operator++(int) { OrderedReverseIterator tmp = *this; ++it_; return tmp; }

        bool operator==(const OrderedReverseIterator &o) const {
            if (parent_ == nullptr && o.parent_ == nullptr) return true;
            if (parent_ != o.parent_) return false;
            return it_ == o.it_;
        }
        bool operator!=(const OrderedReverseIterator &o) const { return !(*this == o); }

        std::pair<id_type, ElemRecordSnapshot> operator*() const {
            id_type id = *it_;
            typename elem_map_type::const_accessor acc;
            if (parent_->elems_.find(acc, id)) {
                return { id, {acc->second.lastElem1, acc->second.lastElem2, acc->second.key} };
            }
            return { id, {} };
        }
    };

    // Ordered iterator accessors (const)
    [[nodiscard]] OrderedConstIterator ordered_begin() const {
        if constexpr (!MaintainOrderedIndex) return OrderedConstIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedConstIterator();
        return OrderedConstIterator(this, ordered_index_->cbegin());
    }
    [[nodiscard]] OrderedConstIterator ordered_end() const {
        if constexpr (!MaintainOrderedIndex) return OrderedConstIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedConstIterator();
        return OrderedConstIterator(this, ordered_index_->cend());
    }

    OrderedConstReverseIterator ordered_rbegin() const {
        if constexpr (!MaintainOrderedIndex) return OrderedConstReverseIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedConstReverseIterator();
        return OrderedConstReverseIterator(this, ordered_index_->crbegin());
    }
    OrderedConstReverseIterator ordered_rend() const {
        if constexpr (!MaintainOrderedIndex) return OrderedConstReverseIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedConstReverseIterator();
        return OrderedConstReverseIterator(this, ordered_index_->crend());
    }

    // Ordered iterator accessors (mutable)
    [[nodiscard]] OrderedIterator ordered_begin() {
        if constexpr (!MaintainOrderedIndex) return OrderedIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedIterator();
        return OrderedIterator(this, ordered_index_->begin());
    }
    [[nodiscard]] OrderedIterator ordered_end() {
        if constexpr (!MaintainOrderedIndex) return OrderedIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedIterator();
        return OrderedIterator(this, ordered_index_->end());
    }
    OrderedReverseIterator ordered_rbegin() {
        if constexpr (!MaintainOrderedIndex) return OrderedReverseIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedReverseIterator();
        return OrderedReverseIterator(this, ordered_index_->rbegin());
    }
    OrderedReverseIterator ordered_rend() {
        if constexpr (!MaintainOrderedIndex) return OrderedReverseIterator();
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return OrderedReverseIterator();
        return OrderedReverseIterator(this, ordered_index_->rend());
    }

    // top_k / bottom_k helpers (ids)
    [[nodiscard]] std::vector<id_type> top_k(size_t k) const {
        std::vector<id_type> out;
        if constexpr (!MaintainOrderedIndex) return out;
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return out;
        for (auto it = ordered_index_->rbegin(); it != ordered_index_->rend() && out.size() < k; ++it) out.push_back(*it);
        return out;
    }
    [[nodiscard]] std::vector<id_type> bottom_k(size_t k) const {
        std::vector<id_type> out;
        if constexpr (!MaintainOrderedIndex) return out;
        std::shared_lock<std::shared_mutex> lock(ordered_mtx_);
        if (!ordered_index_) return out;
        for (auto it = ordered_index_->begin(); it != ordered_index_->end() && out.size() < k; ++it) out.push_back(*it);
        return out;
    }

private:
    static constexpr bool apply1_is_default_add() {
        using default_t = detail::DefaultApplyAdd<Total1T, delta1_type>;
        return std::is_same_v<std::remove_cv_t<std::remove_reference_t<Apply1Fn>>, default_t>;
    }
    static constexpr bool apply2_is_default_add() {
        using default_t = detail::DefaultApplyAdd<Total2T, delta2_type>;
        return std::is_same_v<std::remove_cv_t<std::remove_reference_t<Apply2Fn>>, default_t>;
    }

    lock_type maybe_lock() const {
        if constexpr (RequireCoarseLock) return lock_type(coarse_mtx_);
        return coarse_lock_enabled_ ? lock_type(coarse_mtx_) : lock_type(coarse_mtx_, std::defer_lock);
    }

    // Count-map index helpers (value -> count) used when AggMode is Min/Max and ordered_index is not enabled.
    void insert_index1(const total1_type &v) { ++idx1_[v]; }
    void erase_one_index1(const total1_type &v) {
        auto it = idx1_.find(v);
        if (it != idx1_.end()) {
            if (--(it->second) == 0) idx1_.erase(it);
        }
    }
	
	std::optional<total1_type> top_index1() const {
        if constexpr (Total1Mode == AggMode::Add) return std::nullopt;
        // use count map to derive min/max of extractor values
        if (idx1_.empty()) return std::nullopt;
        if constexpr (Total1Mode == AggMode::Min) {
            return idx1_.begin()->first;
        } else { // Max
            return idx1_.rbegin()->first;
        }
    }

    void insert_index2(const total2_type &v) { ++idx2_[v]; }
    void erase_one_index2(const total2_type &v) {
        auto it = idx2_.find(v);
        if (it != idx2_.end()) {
            if (--(it->second) == 0) idx2_.erase(it);
        }
    }
	
	std::optional<total2_type> top_index2() const {
        if constexpr (Total2Mode == AggMode::Add) return std::nullopt;
        if (idx2_.empty()) return std::nullopt;
        if constexpr (Total2Mode == AggMode::Min) {
            return idx2_.begin()->first;
        } else {
            return idx2_.rbegin()->first;
        }
    }

    // apply_pair handles additive and index modes; parameters describe optional old/new extractor values
    void apply_pair(const delta1_type &d1, const delta2_type &d2,
                    bool have_old1 = false, const total1_type *old1 = nullptr,
                    bool have_new1 = false, const total1_type *new1 = nullptr,
                    bool have_old2 = false, const total2_type *old2 = nullptr,
                    bool have_new2 = false, const total2_type *new2 = nullptr)
    {
        if (!combined_atomic_) {
            // non-combined path: apply/update each total separately

            // Total1: Add vs Min/Max
            if constexpr (Total1Mode == AggMode::Add) {
                apply_total1(d1);
            } else {
                // Update count-map indices unconditionally when extractor values provided
                if (have_old1 && old1) erase_one_index1(*old1);
                if (have_new1 && new1) insert_index1(*new1);

                auto top1 = top_index1();
                if (top1) total1_.value(*top1);
                else total1_.value(total1_type{});
            }

            // Total2: Add vs Min/Max
            if constexpr (Total2Mode == AggMode::Add) {
                apply_total2(d2);
            } else {
                if (have_old2 && old2) erase_one_index2(*old2);
                if (have_new2 && new2) insert_index2(*new2);

                auto top2 = top_index2();
                if (top2) total2_.value(*top2);
                else total2_.value(total2_type{});
            }
            return;
        }

        // Combined-atomic path: update indices or apply delta under combined mutex and write both totals in one batch
        std::lock_guard<std::mutex> g(combined_mtx_);
        total1_type cur1 = total1_.get();
        total2_type cur2 = total2_.get();

        bool changed1 = false;
        bool changed2 = false;

        // For Min/Max: update the idx maps first (so top_index* sees latest values)
        if constexpr (Total1Mode != AggMode::Add) {
            if (have_old1 && old1) erase_one_index1(*old1);
            if (have_new1 && new1) insert_index1(*new1);

            auto top1 = top_index1();
            if (top1) {
                if (cur1 != *top1) { cur1 = *top1; changed1 = true; }
            } else {
                if (cur1 != total1_type{}) { cur1 = total1_type{}; changed1 = true; }
            }
        } else {
            // Add mode
            if constexpr (apply1_is_default_add()) {
                cur1 += d1;
                changed1 = true;
            } else {
                changed1 = apply1_(cur1, d1);
            }
        }

        if constexpr (Total2Mode != AggMode::Add) {
            if (have_old2 && old2) erase_one_index2(*old2);
            if (have_new2 && new2) insert_index2(*new2);

            auto top2 = top_index2();
            if (top2) {
                if (cur2 != *top2) { cur2 = *top2; changed2 = true; }
            } else {
                if (cur2 != total2_type{}) { cur2 = total2_type{}; changed2 = true; }
            }
        } else {
            if constexpr (apply2_is_default_add()) {
                cur2 += d2;
                changed2 = true;
            } else {
                changed2 = apply2_(cur2, d2);
            }
        }

        if (changed1 || changed2) {
            reaction::batchExecute([&]{
                if (changed1) total1_.value(cur1);
                if (changed2) total2_.value(cur2);
            });
        }
    }

    void apply_total1(const delta1_type &d) {
        if constexpr (apply1_is_default_add()) {
            total1_ += d;
        } else {
            if constexpr (RequireCoarseLock) {
                total1_type cur = total1_.get();
                bool changed = apply1_(cur, d);
                if (changed) total1_.value(cur);
            } else {
                std::lock_guard<std::mutex> g(total1_mtx_);
                total1_type cur = total1_.get();
                bool changed = apply1_(cur, d);
                if (changed) total1_.value(cur);
            }
        }
    }

    void apply_total2(const delta2_type &d) {
        if constexpr (apply2_is_default_add()) {
            total2_ += d;
        } else {
            if constexpr (RequireCoarseLock) {
                total2_type cur = total2_.get();
                bool changed = apply2_(cur, d);
                if (changed) total2_.value(cur);
            } else {
                std::lock_guard<std::mutex> g(total2_mtx_);
                total2_type cur = total2_.get();
                bool changed = apply2_(cur, d);
                if (changed) total2_.value(cur);
            }
        }
    }

    //==============================================================================
    // ELEMENT INSERTION & MODIFICATION
    //==============================================================================
    
    // push helper
    [[nodiscard]] id_type push_one(elem1_type e1, elem2_type e2, typename ElemRecord::key_storage_t key) {
        id_type id = nextId_.fetch_add(1, std::memory_order_relaxed);

        reaction::Var<elem1_type> v1 = reaction::var(e1);
        reaction::Var<elem2_type> v2 = reaction::var(e2);

        ElemRecord rec(std::move(v1), std::move(v2), std::move(key));
        rec.lastElem1 = rec.elem1Var.get();
        rec.lastElem2 = rec.elem2Var.get();
        
        // Insert into concurrent_hash_map
        elems_.insert(std::make_pair(id, std::move(rec)));

        // Increment atomic element counter
        elem_count_.fetch_add(1, std::memory_order_relaxed);

        if constexpr (!std::is_same_v<KeyT, std::monostate>) {
            typename elem_map_type::const_accessor acc;
            if (elems_.find(acc, id)) {
                key_index_.insert(std::make_pair(acc->second.key, id));
            }
        }

        if constexpr (MaintainOrderedIndex) {
            // Phase 3: unique_lock for write operations
            std::unique_lock<std::shared_mutex> lock(ordered_mtx_);
            if (ordered_index_) {
                ordered_index_->insert(id);
            }
        }

        delta1_type d1 = delta1_(e1, e2, elem1_type{}, elem2_type{});
        delta2_type d2 = delta2_(e1, e2, elem1_type{}, elem2_type{});

        std::optional<total1_type> new_ext1;
        std::optional<total2_type> new_ext2;
        if constexpr (Total1Mode != AggMode::Add) new_ext1 = extract1_(e1, e2);
        if constexpr (Total2Mode != AggMode::Add) new_ext2 = extract2_(e1, e2);

        apply_pair(d1, d2,
                   /*old1*/ false, nullptr, /*new1*/ bool(new_ext1), new_ext1 ? &*new_ext1 : nullptr,
                   /*old2*/ false, nullptr, /*new2*/ bool(new_ext2), new_ext2 ? &*new_ext2 : nullptr);

        // Get references to the Vars before creating the monitor
        typename elem_map_type::accessor acc_for_monitor;
        if (!elems_.find(acc_for_monitor, id)) {
            throw std::runtime_error("push_one: element not found after insert");
        }
        reaction::Var<elem1_type> &var1_ref = acc_for_monitor->second.elem1Var;
        reaction::Var<elem2_type> &var2_ref = acc_for_monitor->second.elem2Var;
        acc_for_monitor.release(); // Release before creating monitor

        auto delta1_copy = delta1_;
        auto delta2_copy = delta2_;
        auto extract1_copy = extract1_;
        auto extract2_copy = extract2_;

        monitors_.insert(std::make_pair(id, reaction::action(
            [this, id, delta1_copy, delta2_copy, extract1_copy, extract2_copy](elem1_type new1, elem2_type new2) {
                // Use concurrent_hash_map accessor
                typename elem_map_type::accessor acc;
                if (!elems_.find(acc, id)) return;
                ElemRecord &r = acc->second;

                elem1_type ne1 = static_cast<elem1_type>(new1);
                elem2_type ne2 = static_cast<elem2_type>(new2);
                
                // Copy old values before modification
                elem1_type old_e1 = r.lastElem1;
                elem2_type old_e2 = r.lastElem2;

                // Check if reordering needed (while holding accessor)
                bool need_reinsert = true;
                if constexpr (MaintainOrderedIndex) {
                    bool equiv = (!cmp_(old_e1, old_e2, ne1, ne2) && !cmp_(ne1, ne2, old_e1, old_e2));
                    need_reinsert = !equiv;
                }

                // Compute deltas
                delta1_type dd1 = delta1_copy(ne1, ne2, old_e1, old_e2);
                delta2_type dd2 = delta2_copy(ne1, ne2, old_e1, old_e2);

                // Compute extractors
                std::optional<total1_type> old_ext1, new_ext1;
                std::optional<total2_type> old_ext2, new_ext2;
                if constexpr (Total1Mode != AggMode::Add) {
                    old_ext1 = extract1_copy(old_e1, old_e2);
                    new_ext1 = extract1_copy(ne1, ne2);
                }
                if constexpr (Total2Mode != AggMode::Add) {
                    old_ext2 = extract2_copy(old_e1, old_e2);
                    new_ext2 = extract2_copy(ne1, ne2);
                }

                // Update values in place
                r.lastElem1 = ne1;
                r.lastElem2 = ne2;
                
                // Release accessor BEFORE manipulating ordered index
                acc.release();

                // Update ordered index (may cause IdComparator to acquire new accessors)
                // Phase 3: unique_lock for write operations
                if constexpr (MaintainOrderedIndex) {
                    if (ordered_index_) {
                        if (need_reinsert) {
                            std::unique_lock<std::shared_mutex> lock(this->ordered_mtx_);
                            ordered_index_->erase(id);
                            ordered_index_->insert(id);
                        }
                    }
                }

                apply_pair(dd1, dd2,
                           /*old1*/ bool(old_ext1), old_ext1 ? &*old_ext1 : nullptr,
                           /*new1*/ bool(new_ext1), new_ext1 ? &*new_ext1 : nullptr,
                           /*old2*/ bool(old_ext2), old_ext2 ? &*old_ext2 : nullptr,
                           /*new2*/ bool(new_ext2), new_ext2 ? &*new_ext2 : nullptr);
            },
            var1_ref, var2_ref
        )));

        return id;
    }

    [[nodiscard]] id_type push_one_no_batch(const elem1_type &e1, const elem2_type &e2, typename ElemRecord::key_storage_t key) {
        return push_one(e1, e2, std::move(key));
    }

    //==============================================================================
    // MEMBER VARIABLES
    //==============================================================================
    
    // Members (order chosen so elems_ outlives ordered_index_ on destruction)
    reaction::Var<total1_type> total1_;
    reaction::Var<total2_type> total2_;

    Delta1Fn delta1_;
    Apply1Fn apply1_;
    Delta2Fn delta2_;
    Apply2Fn apply2_;

    Extract1Fn extract1_;
    Extract2Fn extract2_;

    // runtime comparator (stores any callable convertible to compare_fn_t)
    compare_fn_t cmp_;

    // Underlying storage - TBB concurrent hash maps for lock-free operations
    elem_map_type elems_;
    monitor_map_type monitors_;

    // Ordered index is declared after elems_ so elems_ outlives it during member destruction.
    // Phase 3: std::shared_mutex for concurrent reads (multiple readers, single writer)
    std::optional<ordered_set_type> ordered_index_;
    mutable std::shared_mutex ordered_mtx_;  // Reader-writer lock

    std::map<total1_type, std::size_t> idx1_;
    std::map<total2_type, std::size_t> idx2_;

    std::mutex total1_mtx_;
    std::mutex total2_mtx_;
    std::mutex combined_mtx_;

    mutable std::mutex coarse_mtx_;
    bool coarse_lock_enabled_;

    key_index_map_type key_index_{};

    bool combined_atomic_;
    
    // Lock-free atomic counters (Phase 1 optimization)
    std::atomic<id_type> nextId_;
    std::atomic<size_t> elem_count_{0};
};

} // namespace reactive