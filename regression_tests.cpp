#include <cassert>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "reactive_two_field_collection.h"

using namespace reactive;

struct DeltaWithoutTypeAlias {
    long operator()(double /*new1*/, long new2, double /*old1*/, long old2) const noexcept {
        return new2 - old2;
    }
};

static_assert(std::is_same_v<detail::deduced_delta_t<long, DeltaWithoutTypeAlias>, long>);

void test_default_arithmetic_wraps_without_signed_overflow() {
    detail::DefaultApplyAdd<int> apply;
    int total = std::numeric_limits<int>::max();
    assert(apply(total, 1));
    assert(total == std::numeric_limits<int>::lowest());

    detail::DefaultDelta1<int, int, int> delta1;
    assert(delta1(0, std::numeric_limits<int>::lowest(), 0, std::numeric_limits<int>::max()) == 1);

    detail::DefaultDelta2<int, int, int> delta2;
    assert(delta2(std::numeric_limits<int>::max(), 2, 0, 0) == -2);
}

void test_saturating_apply_avoids_integral_overflow() {
    detail::SaturatingApply<int> apply;

    int total = std::numeric_limits<int>::max() - 1;
    assert(apply(total, 10));
    assert(total == std::numeric_limits<int>::max());

    total = std::numeric_limits<int>::lowest() + 1;
    assert(apply(total, -10));
    assert(total == std::numeric_limits<int>::lowest());

    detail::SaturatingApply<int> bounded(-100, 100);
    total = 90;
    assert(bounded(total, 20));
    assert(total == 100);
    assert(!bounded(total, 20));
}

void test_delta_without_type_alias_uses_total_type() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        DeltaWithoutTypeAlias
    >;

    Coll c;
    const auto id = c.push_back(2.0, 10);
    assert(c.total1() == 10);

    c.elem2Var(id).value(25);
    assert(c.total1() == 25);
}

void test_erase_by_key_no_deadlock() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::string
    >;

    Coll c({}, {}, {}, {}, false, true);
    c.push_back(1.0, 10, "k1");
    c.erase_by_key(std::string("k1"));
    assert(c.size() == 0);
}

void test_ordered_iterator_holds_shared_lock() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::monostate,
        AggMode::Add, AggMode::Add,
        DefaultExtract1<double, long, long>,
        DefaultExtract2<double, long, double>,
        false, true
    >;

    Coll c({}, {}, {}, {}, false, false);
    c.push_back(1.0, 1);

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::thread writer;

    {
        auto ordered = c.ordered();
        auto it = ordered.begin();
        writer = std::thread([&]() {
            started.store(true, std::memory_order_release);
            c.push_back(2.0, 2);
            done.store(true, std::memory_order_release);
        });

        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!done.load(std::memory_order_acquire));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writer.join();
    assert(done.load(std::memory_order_acquire));
}

void test_duplicate_key_rejected() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::string
    >;

    Coll c({}, {}, {}, {}, false, true);
    auto id1 = c.push_back(1.0, 10, "dup");

    bool threw = false;
    try {
        (void)c.push_back(2.0, 20, "dup");
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
    assert(c.size() == 1);
    auto found = c.find_by_key(std::string("dup"));
    assert(found.has_value());
    assert(found.value() == id1);
}

void test_set_compare_reorders() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::monostate,
        AggMode::Add, AggMode::Add,
        DefaultExtract1<double, long, long>,
        DefaultExtract2<double, long, double>,
        false, true
    >;

    Coll c({}, {}, {}, {}, false, false);
    c.push_back(1.0, 1);
    c.push_back(2.0, 1);
    c.push_back(3.0, 1);

    c.set_compare([](double a1, long /*a2*/, double b1, long /*b2*/) {
        return a1 > b1;
    });

    auto ordered = c.ordered();
    auto first = ordered.begin();
    auto [id, rec] = *first;
    (void)id;
    assert(rec.lastElem1 == 3.0);
}

void test_batch_duplicate_key_rejected_before_writes() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::string
    >;

    Coll c({}, {}, {}, {}, false, true);
    const std::vector<std::pair<double, long>> vals{{1.0, 10}, {2.0, 20}};
    const std::vector<std::string> keys{"k1", "k1"};

    bool threw = false;
    try {
        c.push_back(vals, &keys);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    assert(c.size() == 0);
}

void test_batch_existing_key_rejected_before_writes() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::string
    >;

    Coll c({}, {}, {}, {}, false, true);
    c.push_back(1.0, 10, "k1");
    const std::vector<std::pair<double, long>> vals{{2.0, 20}, {3.0, 30}};
    const std::vector<std::string> keys{"k1", "k2"};

    bool threw = false;
    try {
        c.push_back(vals, &keys);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    assert(c.size() == 1);
}

void test_set_compare_concurrent_updates_stress() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::monostate,
        AggMode::Add, AggMode::Add,
        DefaultExtract1<double, long, long>,
        DefaultExtract2<double, long, double>,
        false, true
    >;

    Coll c({}, {}, {}, {}, false, false);
    std::vector<size_t> ids;
    ids.reserve(64);
    for (size_t i = 0; i < 64; ++i) {
        ids.push_back(c.push_back(static_cast<double>(i), static_cast<long>(i)));
    }

    std::atomic<bool> stop{false};
    std::thread updater([&]() {
        for (size_t iter = 0; iter < 5000 && !stop.load(std::memory_order_relaxed); ++iter) {
            const size_t idx = iter % ids.size();
            c.elem2Var(ids[idx]).value(static_cast<long>(iter % 97));
        }
    });

    std::thread comparator([&]() {
        for (size_t iter = 0; iter < 1000 && !stop.load(std::memory_order_relaxed); ++iter) {
            if ((iter % 2) == 0) {
                c.set_compare([](double a1, long, double b1, long) { return a1 < b1; });
            } else {
                c.set_compare([](double a1, long, double b1, long) { return a1 > b1; });
            }
        }
    });

    updater.join();
    comparator.join();
    stop.store(true, std::memory_order_relaxed);

    assert(c.size() == ids.size());
    auto ordered = c.ordered();
    size_t count = 0;
    for (auto it = ordered.begin(); it != ordered.end(); ++it) {
        ++count;
    }
    assert(count == ids.size());
}

void test_keyed_batch_concurrent_conflict_consistency() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::string
    >;

    Coll c({}, {}, {}, {}, false, false);
    std::vector<std::pair<double, long>> vals;
    std::vector<std::string> keys;
    vals.reserve(1000);
    keys.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        vals.emplace_back(static_cast<double>(i), static_cast<long>(i));
        keys.push_back("k" + std::to_string(i));
    }

    const std::string contested = "k750";
    std::atomic<bool> batch_done{false};
    std::atomic<bool> batch_threw{false};

    std::thread batch_writer([&]() {
        try {
            c.push_back(vals, &keys);
        } catch (const std::invalid_argument&) {
            batch_threw.store(true, std::memory_order_relaxed);
        }
        batch_done.store(true, std::memory_order_release);
    });

    std::thread racer([&]() {
        while (!batch_done.load(std::memory_order_acquire)) {
            try {
                (void)c.push_back(-1.0, -1, contested);
            } catch (const std::invalid_argument&) {
                // Expected if key already inserted elsewhere.
            }
            std::this_thread::yield();
        }
    });

    batch_writer.join();
    racer.join();

    std::unordered_set<std::string> seen_keys;
    seen_keys.reserve(c.size());
    for (auto it = c.begin(); it != c.end(); ++it) {
        const std::string &k = it->second.key;
        auto [_, inserted] = seen_keys.insert(k);
        assert(inserted && "Duplicate key found in element map after concurrent writes");
        auto found = c.find_by_key(k);
        assert(found.has_value());
        assert(found.value() == it->first);
    }
    assert(seen_keys.size() == c.size());
    assert(c.size() <= 1000);
    if (batch_threw.load(std::memory_order_relaxed)) {
        // Under concurrency, batch insert may fail after partial progress.
        assert(c.size() < 1000);
    }
}

void test_concurrent_duplicate_erase_applies_once() {
    using Coll = ReactiveTwoFieldCollection<double, long>;
    Coll c({}, {}, {}, {}, false, false);
    auto id = c.push_back(3.0, 4);

    constexpr int thread_count = 16;
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) != thread_count) {
                std::this_thread::yield();
            }
            c.erase(id);
        });
    }
    for (auto &thread : threads) thread.join();

    assert(c.size() == 0);
    assert(c.total1() == 0);
    assert(c.total2() == 0.0);
}

void test_var_handle_survives_erase_without_updating_collection() {
    using Coll = ReactiveTwoFieldCollection<double, long>;
    Coll c({}, {}, {}, {}, false, false);
    auto id = c.push_back(2.0, 5);
    auto elem2 = c.elem2Var(id);

    c.erase(id);
    elem2.value(99);

    assert(c.size() == 0);
    assert(c.total1() == 0);
    assert(c.total2() == 0.0);
}

void test_concurrent_min_max_updates_without_coarse_lock() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::monostate,
        AggMode::Min, AggMode::Max
    >;

    Coll c({}, {}, {}, {}, false, false);
    constexpr int thread_count = 8;
    constexpr int per_thread = 200;
    std::vector<std::vector<size_t>> ids(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            ids[static_cast<size_t>(t)].reserve(per_thread);
            for (int i = 0; i < per_thread; ++i) {
                const long value = static_cast<long>(t * per_thread + i + 1);
                ids[static_cast<size_t>(t)].push_back(c.push_back(static_cast<double>(value), value));
            }
        });
    }
    for (auto &thread : threads) thread.join();

    constexpr long max_value = thread_count * per_thread;
    assert(c.size() == static_cast<size_t>(max_value));
    assert(c.total1() == 1);
    assert(c.total2() == static_cast<double>(max_value) * static_cast<double>(max_value));

    threads.clear();
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            for (size_t id : ids[static_cast<size_t>(t)]) c.erase(id);
        });
    }
    for (auto &thread : threads) thread.join();

    assert(c.empty());
    assert(c.total1() == 0);
    assert(c.total2() == 0.0);
}

void test_ordered_view_remains_sorted_during_updates() {
    using Coll = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::monostate,
        AggMode::Add, AggMode::Add,
        DefaultExtract1<double, long, long>,
        DefaultExtract2<double, long, double>,
        false, true
    >;

    Coll c({}, {}, {}, {}, false, false);
    std::vector<size_t> ids;
    ids.reserve(64);
    for (int i = 0; i < 64; ++i) ids.push_back(c.push_back(static_cast<double>(i), i));

    std::atomic<bool> done{false};
    std::thread updater([&]() {
        for (int iter = 0; iter < 3000; ++iter) {
            const size_t index = static_cast<size_t>(iter) % ids.size();
            c.elem1Var(ids[index]).value(static_cast<double>((iter * 17) % 101));
        }
        done.store(true, std::memory_order_release);
    });

    while (!done.load(std::memory_order_acquire)) {
        auto ordered = c.ordered();
        bool have_previous = false;
        double previous1 = 0.0;
        long previous2 = 0;
        for (auto it = ordered.begin(); it != ordered.end(); ++it) {
            auto [id, record] = *it;
            (void)id;
            if (have_previous) {
                const bool in_order = previous1 < record.lastElem1 ||
                    (previous1 == record.lastElem1 && previous2 <= record.lastElem2);
                assert(in_order);
            }
            previous1 = record.lastElem1;
            previous2 = record.lastElem2;
            have_previous = true;
        }
    }
    updater.join();
}

int main() {
    test_default_arithmetic_wraps_without_signed_overflow();
    test_saturating_apply_avoids_integral_overflow();
    test_delta_without_type_alias_uses_total_type();
    test_erase_by_key_no_deadlock();
    test_ordered_iterator_holds_shared_lock();
    test_duplicate_key_rejected();
    test_batch_duplicate_key_rejected_before_writes();
    test_batch_existing_key_rejected_before_writes();
    test_set_compare_reorders();
    test_set_compare_concurrent_updates_stress();
    test_keyed_batch_concurrent_conflict_consistency();
    test_concurrent_duplicate_erase_applies_once();
    test_var_handle_survives_erase_without_updating_collection();
    test_concurrent_min_max_updates_without_coarse_lock();
    test_ordered_view_remains_sorted_during_updates();
    return 0;
}
