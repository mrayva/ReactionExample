#include <cassert>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "reactive_two_field_collection.h"

using namespace reactive;

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

int main() {
    test_erase_by_key_no_deadlock();
    test_ordered_iterator_holds_shared_lock();
    test_duplicate_key_rejected();
    test_batch_duplicate_key_rejected_before_writes();
    test_batch_existing_key_rejected_before_writes();
    test_set_compare_reorders();
    test_set_compare_concurrent_updates_stress();
    test_keyed_batch_concurrent_conflict_consistency();
    return 0;
}
