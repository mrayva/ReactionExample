#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <cassert>
#include "reactive_two_field_collection.h"

using namespace reactive;

// Stress test for lock-free optimizations
void test_atomic_counters() {
    std::cout << "Testing atomic counters with high contention...\n";
    
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
        false, false, DefaultCompare<double, long>
    >;
    
    // Note: coarse_lock=true ensures map operations are synchronized
    // The atomic counters still provide lock-free size()/empty() queries
    Coll c({}, {}, {}, {}, false, true);
    
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 10000;
    
    std::atomic<int> ready_count{0};
    std::vector<std::thread> threads;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Spawn threads that push and erase concurrently
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready_count.fetch_add(1);
            while (ready_count.load() < NUM_THREADS) {
                std::this_thread::yield();
            }
            
            std::vector<size_t> ids;
            
            // Push elements
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                double e1 = t * 1000.0 + i;
                long e2 = t * 1000 + i;
                auto id = c.push_back(e1, e2);
                ids.push_back(id);
            }
            
            // Erase half
            for (size_t i = 0; i < ids.size() / 2; ++i) {
                c.erase(ids[i]);
            }
        });
    }
    
    for (auto &th : threads) th.join();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    size_t expected_size = NUM_THREADS * OPS_PER_THREAD / 2;
    size_t actual_size = c.size();
    
    std::cout << "  Expected size: " << expected_size << "\n";
    std::cout << "  Actual size:   " << actual_size << "\n";
    std::cout << "  Duration:      " << duration.count() << " ms\n";
    std::cout << "  Ops/sec:       " << (NUM_THREADS * OPS_PER_THREAD * 1.5 * 1000.0 / static_cast<double>(duration.count())) << "\n";
    
    assert(actual_size == expected_size && "Size mismatch - atomic counter broken!");
    std::cout << "  ✓ Atomic counters working correctly\n";
}

void test_size_and_empty() {
    std::cout << "\nTesting lock-free size() and empty()...\n";
    
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
        false, false, DefaultCompare<double, long>
    >;
    
    Coll c({}, {}, {}, {}, false, true); // coarse_lock for map safety
    
    assert(c.empty() && "Should be empty initially");
    assert(c.size() == 0 && "Size should be 0 initially");
    
    auto id1 = c.push_back(1.0, 10);
    assert(!c.empty() && "Should not be empty after insert");
    assert(c.size() == 1 && "Size should be 1");
    
    auto id2 = c.push_back(2.0, 20);
    assert(c.size() == 2 && "Size should be 2");
    
    c.erase(id1);
    assert(c.size() == 1 && "Size should be 1 after erase");
    
    c.erase(id2);
    assert(c.empty() && "Should be empty after all erased");
    assert(c.size() == 0 && "Size should be 0");
    
    std::cout << "  ✓ Lock-free size() and empty() working correctly\n";
}

void test_id_generation() {
    std::cout << "\nTesting atomic ID generation...\n";
    
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
        false, false, DefaultCompare<double, long>
    >;
    
    Coll c({}, {}, {}, {}, false, true); // coarse_lock for map safety
    
    const int NUM_THREADS = 16;
    const int IDS_PER_THREAD = 1000;
    
    std::vector<std::thread> threads;
    std::vector<std::vector<size_t>> all_ids(NUM_THREADS);
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < IDS_PER_THREAD; ++i) {
                auto id = c.push_back(double(t), long(i));
                all_ids[static_cast<size_t>(t)].push_back(id);
            }
        });
    }
    
    for (auto &th : threads) th.join();
    
    // Check all IDs are unique
    std::vector<size_t> all_ids_flat;
    for (const auto &vec : all_ids) {
        all_ids_flat.insert(all_ids_flat.end(), vec.begin(), vec.end());
    }
    
    std::sort(all_ids_flat.begin(), all_ids_flat.end());
    auto it = std::adjacent_find(all_ids_flat.begin(), all_ids_flat.end());
    
    assert(it == all_ids_flat.end() && "Duplicate IDs detected!");
    std::cout << "  Generated " << all_ids_flat.size() << " unique IDs across " 
              << NUM_THREADS << " threads\n";
    std::cout << "  ✓ Atomic ID generation working correctly (no duplicates)\n";
}

void benchmark_with_and_without_coarse_lock() {
    std::cout << "\nBenchmarking: Fine-grained Lock-Free vs Coarse Lock...\n";
    
    using CollNoLock = ReactiveTwoFieldCollection<
        double, long, long, double,
        detail::DefaultDelta1<double, long, long>,
        detail::DefaultApplyAdd<long>,
        detail::DefaultDelta2<double, long, double>,
        detail::DefaultApplyAdd<double>,
        std::monostate,
        AggMode::Add, AggMode::Add,
        DefaultExtract1<double, long, long>,
        DefaultExtract2<double, long, double>,
        false, false, DefaultCompare<double, long>
    >;
    
    const int NUM_THREADS = 4;
    const int OPS = 50000;
    
    // Benchmark with coarse lock
    // Note: Even with coarse lock, size() is still lock-free due to atomic counter!
    {
        CollNoLock c({}, {}, {}, {}, false, true); // coarse_lock = true
        std::vector<std::thread> threads;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < OPS; ++i) {
                    c.push_back(double(t), long(i));
                    if (i % 100 == 0) {
                        // This is lock-free even with coarse_lock=true!
                        volatile size_t s = c.size();
                        (void)s;
                    }
                }
            });
        }
        
        for (auto &th : threads) th.join();
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "  With coarse lock (but lock-free size()): " << duration.count() << " ms\n";
        std::cout << "    Throughput: " << (NUM_THREADS * OPS * 1000.0 / static_cast<double>(duration.count())) << " ops/sec\n";
        std::cout << "    Note: size() calls are lock-free via atomic counter\n";
    }
}

int main() {
    std::cout << "=== Lock-Free Optimization Tests ===\n\n";
    
    test_atomic_counters();
    test_size_and_empty();
    test_id_generation();
    benchmark_with_and_without_coarse_lock();
    
    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
