#pragma once
/*
  reactive_two_field_collection.h

  Generic two-field reactive collection (header-only).

  Changes in this version:
    - Replaced std::multiset index structures with count-maps (std::map<value, size_t>).
      This reduces memory overhead for many duplicate values while keeping O(log N)
      updates and O(1) min/max retrieval via begin()/rbegin().

  Features:
    - Built-in Aggregate modes for each total: Add, Min, Max.
    - Extractor functors for index-based modes.
    - Combined-atomic-update mode.
    - Mutex-backed ApplyFn fallback for arbitrary Apply semantics.
    - Optional coarse-grained locking (compile-time or runtime).
*/

#include <vector>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <variant>
#include <optional>
#include <mutex>
#include <map>
#include <reaction/reaction.h>

namespace reactive {

namespace detail {

// DefaultDelta1: change in elem2
template <typename Elem1T, typename Elem2T, typename Total1T = Elem2T>
struct DefaultDelta1 {
    using DeltaType = Total1T;
    constexpr DeltaType operator()(const Elem1T & /*new1*/, const Elem2T &new2,
                                   const Elem1T & /*last1*/, const Elem2T &last2) const noexcept {
        return static_cast<DeltaType>(new2 - last2);
    }
};

// DefaultDelta2: new2*new1 - last2*last1
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

// Deduce delta type: prefer DeltaFn::DeltaType if present, otherwise fallback to TotalT
template <typename TotalT, typename DeltaFn>
using deduced_delta_t = std::conditional_t<
    std::is_class_v<DeltaFn> && !std::is_same_v<void, typename DeltaFn::DeltaType>,
    typename DeltaFn::DeltaType,
    TotalT>;


// No-op delta: returns default-constructed DeltaType (usually zero)
template <typename Elem1T, typename Elem2T, typename TotalT>
struct NoopDelta {
    using DeltaType = TotalT;
    constexpr DeltaType operator()(const Elem1T&, const Elem2T&, const Elem1T&, const Elem2T&) const noexcept {
        return DeltaType{};
    }
};

// No-op apply: leaves total unchanged and reports "no change"
template <typename TotalT, typename DeltaT = TotalT>
struct NoopApply {
    using DeltaType = DeltaT;
    constexpr bool operator()(TotalT& /*total*/, const DeltaT& /*d*/) const noexcept {
        return false;
    }
};

} // namespace detail

// Aggregate mode enum
enum class AggMode { Add, Min, Max };

// Default extractors
template <typename Elem1T, typename Elem2T, typename Total1T>
struct DefaultExtract1 {
    constexpr Total1T operator()(const Elem1T & /*e1*/, const Elem2T &e2) const noexcept {
        return static_cast<Total1T>(e2);
    }
};

template <typename Elem1T, typename Elem2T, typename Total2T>
struct DefaultExtract2 {
    constexpr Total2T operator()(const Elem1T &e1, const Elem2T &e2) const noexcept {
        return static_cast<Total2T>(static_cast<Total2T>(e2) * static_cast<Total2T>(e1));
    }
};

template <
    typename Elem1T = double,
    typename Elem2T = long,
    typename Total1T = Elem2T,
    typename Total2T = double,
    typename Delta1Fn = detail::DefaultDelta1<Elem1T, Elem2T, Total1T>,
    typename Apply1Fn = detail::DefaultApplyAdd<Total1T, detail::deduced_delta_t<Total1T, Delta1Fn>>,
    typename Delta2Fn = detail::DefaultDelta2<Elem1T, Elem2T, Total2T>,
    typename Apply2Fn = detail::DefaultApplyAdd<Total2T, detail::deduced_delta_t<Total2T, Delta2Fn>>,
    typename KeyT = void,
    AggMode Total1Mode = AggMode::Add,
    AggMode Total2Mode = AggMode::Add,
    typename Extract1Fn = DefaultExtract1<Elem1T, Elem2T, Total1T>,
    typename Extract2Fn = DefaultExtract2<Elem1T, Elem2T, Total2T>,
    bool RequireCoarseLock = false,
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

    // Per-element record
    struct ElemRecord {
        reaction::Var<elem1_type> elem1Var;
        reaction::Var<elem2_type> elem2Var;
        elem1_type lastElem1{};
        elem2_type lastElem2{};
        using key_storage_t = std::conditional_t<std::is_void_v<KeyT>, std::monostate, KeyT>;
        key_storage_t key{};

        ElemRecord() = default;
        ElemRecord(reaction::Var<elem1_type> a, reaction::Var<elem2_type> b, key_storage_t k = key_storage_t{})
            : elem1Var(std::move(a)), elem2Var(std::move(b)),
              lastElem1(elem1Var.get()), lastElem2(elem2Var.get()), key(std::move(k)) {}
    };

    using map_type = MapType<id_type, ElemRecord>;
    using iterator = typename map_type::iterator;
    using const_iterator = typename map_type::const_iterator;
    using value_type = typename map_type::value_type;
    using key_index_map_type = std::conditional_t<std::is_void_v<KeyT>, std::monostate, MapType<KeyT, id_type>>;
    using lock_type = std::unique_lock<std::mutex>;

    ReactiveTwoFieldCollection(Delta1Fn d1 = Delta1Fn{}, Apply1Fn a1 = Apply1Fn{},
                               Delta2Fn d2 = Delta2Fn{}, Apply2Fn a2 = Apply2Fn{},
                               bool combined_atomic = false,
                               bool coarse_lock = false)
        : total1_(reaction::var(total1_type{})),
          total2_(reaction::var(total2_type{})),
          delta1_(std::move(d1)), apply1_(std::move(a1)),
          delta2_(std::move(d2)), apply2_(std::move(a2)),
          extract1_(), extract2_(),
          combined_atomic_(combined_atomic),
          coarse_lock_enabled_(RequireCoarseLock ? true : coarse_lock),
          nextId_(1) {}

    // Acquire coarse-grained lock for external iteration or multi-call sequences.
    lock_type lock_public() {
        if constexpr (RequireCoarseLock) return lock_type(coarse_mtx_);
        return coarse_lock_enabled_ ? lock_type(coarse_mtx_) : lock_type(coarse_mtx_, std::defer_lock);
    }

    // push (no key)
    id_type push_back(const elem1_type &e1, const elem2_type &e2) {
        auto lk = maybe_lock();
        return push_one(e1, e2, typename ElemRecord::key_storage_t{});
    }
    id_type push_back(elem1_type &&e1, elem2_type &&e2) {
        auto lk = maybe_lock();
        return push_one(std::move(e1), std::move(e2), typename ElemRecord::key_storage_t{});
    }
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
                if constexpr (std::is_void_v<KeyT>) {
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
        auto it = elems_.find(id);
        if (it == elems_.end()) return;
        ElemRecord &rec = it->second;

        std::optional<total1_type> old_ext1;
        std::optional<total2_type> old_ext2;
        if constexpr (Total1Mode != AggMode::Add) old_ext1 = extract1_(rec.lastElem1, rec.lastElem2);
        if constexpr (Total2Mode != AggMode::Add) old_ext2 = extract2_(rec.lastElem1, rec.lastElem2);

        delta1_type rem1 = delta1_(elem1_type{}, elem2_type{}, rec.lastElem1, rec.lastElem2);
        delta2_type rem2 = delta2_(elem1_type{}, elem2_type{}, rec.lastElem1, rec.lastElem2);

        apply_pair(rem1, rem2,
                   /*have_old1*/ bool(old_ext1), old_ext1 ? &*old_ext1 : nullptr,
                   /*have_new1*/ false, nullptr,
                   /*have_old2*/ bool(old_ext2), old_ext2 ? &*old_ext2 : nullptr,
                   /*have_new2*/ false, nullptr);

        if constexpr (!std::is_void_v<KeyT>) key_index_.erase(rec.key);

        if (auto mit = monitors_.find(id); mit != monitors_.end()) mit->second.close();
        monitors_.erase(id);
        elems_.erase(it);
    }

    template <typename K = KeyT>
    std::enable_if_t<!std::is_void_v<K>, void>
    erase_by_key(const K &k) {
        auto lk = maybe_lock();
        auto it = key_index_.find(k);
        if (it == key_index_.end()) return;
        erase(it->second);
    }

    template <typename K = KeyT>
    std::enable_if_t<!std::is_void_v<K>, std::optional<id_type>>
    find_by_key(const K &k) const {
        std::optional<id_type> res;
        if constexpr (!std::is_void_v<K>) {
            if constexpr (RequireCoarseLock) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                auto it = key_index_.find(k);
                if (it != key_index_.end()) res = it->second;
            } else {
                if (coarse_lock_enabled_) {
                    std::lock_guard<std::mutex> g(coarse_mtx_);
                    auto it = key_index_.find(k);
                    if (it != key_index_.end()) res = it->second;
                } else {
                    auto it = key_index_.find(k);
                    if (it != key_index_.end()) res = it->second;
                }
            }
        }
        return res;
    }

    template <typename K = KeyT>
    std::enable_if_t<!std::is_void_v<K>, std::optional<id_type>>
    find_by_key_linear(const K &k) const {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            for (const auto &kv : elems_) if (kv.second.key == k) return kv.first;
            return std::nullopt;
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                for (const auto &kv : elems_) if (kv.second.key == k) return kv.first;
                return std::nullopt;
            } else {
                for (const auto &kv : elems_) if (kv.second.key == k) return kv.first;
                return std::nullopt;
            }
        }
    }

    reaction::Var<elem1_type> &elem1Var(id_type id) {
        auto lk = maybe_lock();
        return elems_.at(id).elem1Var;
    }
    reaction::Var<elem2_type> &elem2Var(id_type id) {
        auto lk = maybe_lock();
        return elems_.at(id).elem2Var;
    }

    total1_type total1() const {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            return total1_.get();
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                return total1_.get();
            } else {
                return total1_.get();
            }
        }
    }
    total2_type total2() const {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            return total2_.get();
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                return total2_.get();
            } else {
                return total2_.get();
            }
        }
    }
    reaction::Var<total1_type> &total1Var() { return total1_; }
    reaction::Var<total2_type> &total2Var() { return total2_; }

    iterator begin() { auto lk = maybe_lock(); return elems_.begin(); }
    iterator end()   { auto lk = maybe_lock(); return elems_.end(); }
    const_iterator begin() const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.begin(); }
    const_iterator end()   const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.end(); }
    const_iterator cbegin() const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.cbegin(); }
    const_iterator cend()   const { if constexpr (RequireCoarseLock) std::lock_guard<std::mutex> g(coarse_mtx_); return elems_.cend(); }

    size_t size() const noexcept {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            return elems_.size();
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                return elems_.size();
            } else {
                return elems_.size();
            }
        }
    }
    bool empty() const noexcept {
        if constexpr (RequireCoarseLock) {
            std::lock_guard<std::mutex> g(coarse_mtx_);
            return elems_.empty();
        } else {
            if (coarse_lock_enabled_) {
                std::lock_guard<std::mutex> g(coarse_mtx_);
                return elems_.empty();
            } else {
                return elems_.empty();
            }
        }
    }

    void clear() {
        auto lk = maybe_lock();
        if (elems_.empty()) return;

        delta1_type sumRem1{};
        delta2_type sumRem2{};
        for (const auto &kv : elems_) {
            const ElemRecord &r = kv.second;
            sumRem1 = sumRem1 + delta1_(elem1_type{}, elem2_type{}, r.lastElem1, r.lastElem2);
            sumRem2 = sumRem2 + delta2_(elem1_type{}, elem2_type{}, r.lastElem1, r.lastElem2);
        }

        apply_pair(sumRem1, sumRem2,
                   /*no ext changes*/ false, nullptr, false, nullptr,
                   false, nullptr, false, nullptr);

        for (auto &p : monitors_) p.second.close();
        monitors_.clear();
        elems_.clear();
        if constexpr (!std::is_void_v<KeyT>) key_index_.clear();
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

    // Count-map index helpers (ordered map value -> count)
    void insert_index1(const total1_type &v) {
        ++idx1_[v];
    }
    void erase_one_index1(const total1_type &v) {
        auto it = idx1_.find(v);
        if (it != idx1_.end()) {
            if (--(it->second) == 0) idx1_.erase(it);
        }
    }
    std::optional<total1_type> top_index1() const {
        if (idx1_.empty()) return std::nullopt;
        if constexpr (Total1Mode == AggMode::Min) return idx1_.begin()->first;
        else return idx1_.rbegin()->first;
    }

    void insert_index2(const total2_type &v) {
        ++idx2_[v];
    }
    void erase_one_index2(const total2_type &v) {
        auto it = idx2_.find(v);
        if (it != idx2_.end()) {
            if (--(it->second) == 0) idx2_.erase(it);
        }
    }
    std::optional<total2_type> top_index2() const {
        if (idx2_.empty()) return std::nullopt;
        if constexpr (Total2Mode == AggMode::Min) return idx2_.begin()->first;
        else return idx2_.rbegin()->first;
    }

    // apply_pair handles additive and index modes; parameters describe optional old/new extractor values
    void apply_pair(const delta1_type &d1, const delta2_type &d2,
                    bool have_old1 = false, const total1_type *old1 = nullptr,
                    bool have_new1 = false, const total1_type *new1 = nullptr,
                    bool have_old2 = false, const total2_type *old2 = nullptr,
                    bool have_new2 = false, const total2_type *new2 = nullptr)
    {
        if (!combined_atomic_) {
            if constexpr (Total1Mode == AggMode::Add) {
                apply_total1(d1);
            } else {
                if (have_old1 && old1) erase_one_index1(*old1);
                if (have_new1 && new1) insert_index1(*new1);
                auto top1 = top_index1();
                if (top1) total1_.value(*top1);
                else total1_.value(total1_type{});
            }

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

        std::lock_guard<std::mutex> g(combined_mtx_);
        total1_type cur1 = total1_.get();
        total2_type cur2 = total2_.get();

        bool changed1 = false;
        bool changed2 = false;

        if constexpr (Total1Mode == AggMode::Add) {
            if constexpr (apply1_is_default_add()) {
                cur1 += d1;
                changed1 = true;
            } else {
                changed1 = apply1_(cur1, d1);
            }
        } else {
            if (have_old1 && old1) erase_one_index1(*old1);
            if (have_new1 && new1) insert_index1(*new1);
            auto top1 = top_index1();
            if (top1) { if (cur1 != *top1) { cur1 = *top1; changed1 = true; } }
            else { if (cur1 != total1_type{}) { cur1 = total1_type{}; changed1 = true; } }
        }

        if constexpr (Total2Mode == AggMode::Add) {
            if constexpr (apply2_is_default_add()) {
                cur2 += d2;
                changed2 = true;
            } else {
                changed2 = apply2_(cur2, d2);
            }
        } else {
            if (have_old2 && old2) erase_one_index2(*old2);
            if (have_new2 && new2) insert_index2(*new2);
            auto top2 = top_index2();
            if (top2) { if (cur2 != *top2) { cur2 = *top2; changed2 = true; } }
            else { if (cur2 != total2_type{}) { cur2 = total2_type{}; changed2 = true; } }
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
            std::lock_guard<std::mutex> g(total1_mtx_);
            total1_type cur = total1_.get();
            bool changed = apply1_(cur, d);
            if (changed) total1_.value(cur);
        }
    }

    void apply_total2(const delta2_type &d) {
        if constexpr (apply2_is_default_add()) {
            total2_ += d;
        } else {
            std::lock_guard<std::mutex> g(total2_mtx_);
            total2_type cur = total2_.get();
            bool changed = apply2_(cur, d);
            if (changed) total2_.value(cur);
        }
    }

    id_type push_one(elem1_type e1, elem2_type e2, typename ElemRecord::key_storage_t key) {
        id_type id = nextId_++;

        reaction::Var<elem1_type> v1 = reaction::var(e1);
        reaction::Var<elem2_type> v2 = reaction::var(e2);

        ElemRecord rec(std::move(v1), std::move(v2), std::move(key));
        rec.lastElem1 = rec.elem1Var.get();
        rec.lastElem2 = rec.elem2Var.get();
        elems_.emplace(id, std::move(rec));

        if constexpr (!std::is_void_v<KeyT>) key_index_.emplace(elems_.at(id).key, id);

        delta1_type d1 = delta1_(e1, e2, elem1_type{}, elem2_type{});
        delta2_type d2 = delta2_(e1, e2, elem1_type{}, elem2_type{});

        std::optional<total1_type> new_ext1;
        std::optional<total2_type> new_ext2;
        if constexpr (Total1Mode != AggMode::Add) new_ext1 = extract1_(e1, e2);
        if constexpr (Total2Mode != AggMode::Add) new_ext2 = extract2_(e1, e2);

        apply_pair(d1, d2,
                   /*old1*/ false, nullptr, /*new1*/ bool(new_ext1), new_ext1 ? &*new_ext1 : nullptr,
                   /*old2*/ false, nullptr, /*new2*/ bool(new_ext2), new_ext2 ? &*new_ext2 : nullptr);

        auto delta1_copy = delta1_;
        auto delta2_copy = delta2_;
        auto extract1_copy = extract1_;
        auto extract2_copy = extract2_;

        monitors_.emplace(id, reaction::action(
            [this, id, delta1_copy, delta2_copy, extract1_copy, extract2_copy](elem1_type new1, elem2_type new2) {
                auto it = elems_.find(id);
                if (it == elems_.end()) return;
                ElemRecord &r = it->second;

                elem1_type ne1 = static_cast<elem1_type>(new1);
                elem2_type ne2 = static_cast<elem2_type>(new2);

                delta1_type dd1 = delta1_copy(ne1, ne2, r.lastElem1, r.lastElem2);
                delta2_type dd2 = delta2_copy(ne1, ne2, r.lastElem1, r.lastElem2);

                std::optional<total1_type> old_ext1, new_ext1;
                std::optional<total2_type> old_ext2, new_ext2;
                if constexpr (Total1Mode != AggMode::Add) {
                    old_ext1 = extract1_copy(r.lastElem1, r.lastElem2);
                    new_ext1 = extract1_copy(ne1, ne2);
                }
                if constexpr (Total2Mode != AggMode::Add) {
                    old_ext2 = extract2_copy(r.lastElem1, r.lastElem2);
                    new_ext2 = extract2_copy(ne1, ne2);
                }

                r.lastElem1 = ne1;
                r.lastElem2 = ne2;

                apply_pair(dd1, dd2,
                           /*old1*/ bool(old_ext1), old_ext1 ? &*old_ext1 : nullptr,
                           /*new1*/ bool(new_ext1), new_ext1 ? &*new_ext1 : nullptr,
                           /*old2*/ bool(old_ext2), old_ext2 ? &*old_ext2 : nullptr,
                           /*new2*/ bool(new_ext2), new_ext2 ? &*new_ext2 : nullptr);
            },
            elems_.at(id).elem1Var, elems_.at(id).elem2Var
        ));

        return id;
    }

    id_type push_one_no_batch(const elem1_type &e1, const elem2_type &e2, typename ElemRecord::key_storage_t key) {
        return push_one(e1, e2, std::move(key));
    }

private:
    MapType<id_type, ElemRecord> elems_;
    MapType<id_type, reaction::Action<>> monitors_;

    reaction::Var<total1_type> total1_;
    reaction::Var<total2_type> total2_;

    // count-maps: value -> count (ordered map for min/max extraction)
    std::map<total1_type, std::size_t> idx1_;
    std::map<total2_type, std::size_t> idx2_;

    Extract1Fn extract1_;
    Extract2Fn extract2_;

    std::mutex total1_mtx_;
    std::mutex total2_mtx_;
    std::mutex combined_mtx_;

    mutable std::mutex coarse_mtx_;
    bool coarse_lock_enabled_;

    key_index_map_type key_index_{};

    Delta1Fn delta1_;
    Apply1Fn apply1_;
    Delta2Fn delta2_;
    Apply2Fn apply2_;

    bool combined_atomic_;
    id_type nextId_;
};

} // namespace reactive