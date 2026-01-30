#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <cmath>
#include "reactive_two_field_collection.h"

using namespace reactive;

// Small helper functor used in one demo: return the new elem2 as the "delta" (interpreted as new value)
struct ReturnNewElem2 {
    using DeltaType = double;
    constexpr double operator()(const double& /*new1*/, const long &new2,
                                const double& /*last1*/, const long& /*last2*/) const noexcept {
        return static_cast<double>(new2);
    }
};

int main() {
    std::cout << "ReactiveTwoFieldCollection comprehensive demo\n";

    // -- Main collection --
    using Coll = ReactiveTwoFieldCollection<
        double, long,            // Elem1T, Elem2T
        long, double,            // Total1T, Total2T
        detail::DefaultDelta1<double,long,long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double,long,double>,
        detail::DefaultApplyAdd<double>,
        void,                    // KeyT
        AggMode::Min,            // Total1Mode -> Min (uses extract1 -> elem2)
        AggMode::Max,            // Total2Mode -> Max (uses extract2 -> elem1*elem2)
        DefaultExtract1<double,long,long>,
        DefaultExtract2<double,long,double>,
        false,                   // RequireCoarseLock
        true,                    // MaintainOrderedIndex (enable ordered view)
        DefaultCompare<double,long>
    >;

    Coll c(/*d1*/{}, /*a1*/{}, /*d2*/{}, /*a2*/{}, /*combined_atomic*/ true, /*coarse_lock*/ false);

    // Observer on totals - count notifications and print values
    int notif_count = 0;
    auto obs = reaction::action([&](long t1, double t2) {
        ++notif_count;
        std::cout << "[observer] total1(min elem2)=" << t1 << " total2(max elem1*elem2)=" << t2 << "\n";
    }, c.total1Var(), c.total2Var());

    // Insert elements
    auto id1 = c.push_back(1.5, 10); // elem1=1.5, elem2=10
    [[maybe_unused]] auto id2 = c.push_back(2.0, 20); // elem1=2.0, elem2=20
    auto id3 = c.push_back(0.5, 15); // elem1=0.5, elem2=15
    [[maybe_unused]] auto id4 = c.push_back(3.0, 5);  // elem1=3.0, elem2=5  -> minimum elem2
    [[maybe_unused]] auto id5 = c.push_back(2.5, 20); // duplicate extractor values to test count-map

    std::cout << "\nInitial ordered (smallest -> largest) by comparator (elem1, elem2):\n";
    for (auto it = c.ordered_begin(); it != c.ordered_end(); ++it) {
        auto [id, rec] = *it;
        std::cout << " id=" << id << " e1=" << rec.lastElem1 << " e2=" << rec.lastElem2 << "\n";
    }

    // Totals: total1 == min(elem2) -> expected 5; total2 == max(elem1*elem2)
    long t1 = c.total1();
    double t2 = c.total2();
    std::cout << "\nTotals: total1(min elem2) = " << t1 << ", total2(max elem1*elem2) = " << t2 << "\n";
    assert(t1 == 5);

    double expected_max = std::max({
        1.5 * 10.0,
        2.0 * 20.0,
        0.5 * 15.0,
        3.0 * 5.0,
        2.5 * 20.0
    });
    assert(std::fabs(t2 - expected_max) < 1e-12);

    // Top 3 (largest-first) using ordered reverse iterator
    std::cout << "\nTop 3 elements (by comparator, largest-first):\n";
    {
        int count = 0;
        for (auto it = c.ordered_rbegin(); it != c.ordered_rend() && count < 3; ++it, ++count) {
            auto [id, rec] = *it;
            std::cout << " id=" << id << " e1=" << rec.lastElem1 << " e2=" << rec.lastElem2 << "\n";
        }
    }

    // Bottom 3 (smallest-first) using ordered forward iterator
    std::cout << "\nBottom 3 elements (smallest-first):\n";
    {
        int count = 0;
        for (auto it = c.ordered_begin(); it != c.ordered_end() && count < 3; ++it, ++count) {
            auto [id, rec] = *it;
            std::cout << " id=" << id << " e1=" << rec.lastElem1 << " e2=" << rec.lastElem2 << "\n";
        }
    }

    // Mutate an element so ordering and totals change. Increase id1.elem2 from 10 -> 30
    std::cout << "\nUpdating id1.elem2 -> 30\n";
    c.elem2Var(id1).value(30);

    std::cout << "After update ordered (smallest -> largest):\n";
    for (auto it = c.ordered_begin(); it != c.ordered_end(); ++it) {
        auto [id, rec] = *it;
        std::cout << " id=" << id << " e1=" << rec.lastElem1 << " e2=" << rec.lastElem2 << "\n";
    }

    std::cout << "Totals now: total1(min elem2)=" << c.total1() << " total2(max elem1*elem2)=" << c.total2() << "\n";
    assert(c.total1() == 5); // id4 still minimum
    assert(notif_count >= 1);

    // Demonstrate change that likely does not reorder (modify elem1 of id3)
    std::cout << "\nUpdating id3.elem1 -> 0.7 (may or may not change ordering depending on comparator)\n";
    c.elem1Var(id3).value(0.7);

    std::cout << "Ordered after id3.elem1 update:\n";
    for (auto it = c.ordered_begin(); it != c.ordered_end(); ++it) {
        auto [id, rec] = *it;
        std::cout << " id=" << id << " e1=" << rec.lastElem1 << " e2=" << rec.lastElem2 << "\n";
    }

    // ----------------------------
    // Demo: index-only total (NoopDelta/NoopApply) - total1 driven solely by index
    // ----------------------------
    std::cout << "\nDemo: index-driven total1 (NoopDelta/NoopApply)\n";
    using CollIndexOnly = ReactiveTwoFieldCollection<
        double,long,long,double,
        detail::NoopDelta<double,long,long>,    // Delta1 is noop
        detail::NoopApply<long>,                // Apply1 is noop -> total1 only updated by index logic
        detail::DefaultDelta2<double,long,double>,
        detail::DefaultApplyAdd<double>,
        void,
        AggMode::Min,                           // total1 min (driven by index)
        AggMode::Add,                           // total2 additive
        DefaultExtract1<double,long,long>,
        DefaultExtract2<double,long,double>,
        false,
        true,
        DefaultCompare<double,long>
    >;

    CollIndexOnly ci(/*d1*/{}, /*a1*/{}, /*d2*/{}, /*a2*/{}, /*combined_atomic*/true, /*coarse_lock*/false);
    [[maybe_unused]] auto ia = ci.push_back(1.0, 100);
    [[maybe_unused]] auto ib = ci.push_back(2.0, 50);
    [[maybe_unused]] auto ic = ci.push_back(0.5, 75);
    std::cout << "ci.total1() (min elem2) = " << ci.total1() << " (expect 50)\n";
    assert(ci.total1() == 50);

    // ----------------------------
    // Demo: SetApply where total1 is set to the latest elem2 value
    // ----------------------------
    std::cout << "\nDemo: SetApply where total1 is set to latest elem2 value\n";
    using CollSet = ReactiveTwoFieldCollection<
        double,long,double,double,
        ReturnNewElem2,         // delta1 returns the new elem2 as "delta"
        detail::SetApply<double>, // apply1 sets total1 to delta (use detail::SetApply)
        detail::DefaultDelta2<double,long,double>,
        detail::DefaultApplyAdd<double>,
        void,
        AggMode::Add,           // total1 driven by SetApply (not index)
        AggMode::Add,
        DefaultExtract1<double,long,double>,
        DefaultExtract2<double,long,double>,
        false,
        false,
        DefaultCompare<double,long>
    >;

    CollSet cs(/*d1*/{}, /*a1*/{}, /*d2*/{}, /*a2*/{}, /*combined_atomic*/true, /*coarse_lock*/false);
    auto s1 = cs.push_back(1.0, 7);   // total1 -> 7
    assert(std::fabs(cs.total1() - 7.0) < 1e-12);
    cs.elem2Var(s1).value(42);        // SetApply will set total1 to 42
    assert(std::fabs(cs.total1() - 42.0) < 1e-12);
    std::cout << "cs.total1 after update = " << cs.total1() << " (expect 42)\n";

    // ----------------------------
    // Concurrent pushes with coarse lock enabled
    // ----------------------------
    std::cout << "\nConcurrent push test (coarse_lock enabled)\n";
    using CollConcurrent = ReactiveTwoFieldCollection<
        double,long,long,double,
        detail::DefaultDelta1<double,long,long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double,long,double>,
        detail::DefaultApplyAdd<double>,
        void,
        AggMode::Min,
        AggMode::Max,
        DefaultExtract1<double,long,long>,
        DefaultExtract2<double,long,double>,
        false,
        true,
        DefaultCompare<double,long>
    >;

    CollConcurrent cc(/*d1*/{}, /*a1*/{}, /*d2*/{}, /*a2*/{}, /*combined_atomic*/true, /*coarse_lock*/true);

    const int NTHREADS = 4;
    const int PER_THREAD = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < NTHREADS; ++t) {
        threads.emplace_back([&cc, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                double e1 = double(t) + 0.001 * i;
                long e2 = (t+1) * 10 + i % 50;
                cc.push_back(e1, e2);
            }
        });
    }
    for (auto &th : threads) th.join();
    std::cout << "cc.size() = " << cc.size() << " (expect " << (NTHREADS * PER_THREAD) << ")\n";
    assert(cc.size() == static_cast<size_t>(NTHREADS * PER_THREAD));

    // Print top 5 via ordered reverse iterator
    std::cout << "cc top 5 (id,elem1,elem2):\n";
    {
        int count = 0;
        for (auto it = cc.ordered_rbegin(); it != cc.ordered_rend() && count < 5; ++it, ++count) {
            auto [id, rec] = *it;
            std::cout << " id=" << id << " e1=" << rec.lastElem1 << " e2=" << rec.lastElem2 << "\n";
        }
    }

    std::cout << "\nAll demos completed successfully.\n";
    return 0;
}