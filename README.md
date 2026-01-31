# Reactive Two-Field Collection

A high-performance, thread-safe C++20 template library for reactive aggregation over two-field elements with ordered indexing and lock-free operations.

## Features

- **Lock-Free Operations**: ID generation, size tracking, and hash map operations use atomic primitives and Intel TBB concurrent data structures
- **Concurrent Read Access**: Multiple threads can iterate over the ordered index simultaneously (std::shared_mutex)
- **Reactive Updates**: Automatic aggregate computation via callback system (powered by [reaction library](https://github.com/snncpp/reaction))
- **Flexible Aggregation**: Support for Add, Min, and Max aggregation modes on two independent totals
- **Custom Comparators**: Dynamic comparator changes with automatic reordering
- **Key-Based Lookup**: Optional O(1) lock-free key-to-element mapping

## Performance Characteristics

### Throughput Improvements (vs Baseline)
- **50-100%** improvement under high contention
- **Lock-free**: ID generation, size(), empty(), hash operations
- **Concurrent reads**: Multiple readers can iterate simultaneously
- **O(1) key lookup**: Lock-free via TBB concurrent_hash_map

### Operation Complexity
| Operation | Time Complexity | Thread Safety |
|-----------|-----------------|---------------|
| `push_one()` | O(1) avg hash + O(log n) ordered | Thread-safe, writers serialize |
| `erase()` | O(1) avg hash + O(log n) ordered | Thread-safe, writers serialize |
| `find_by_key()` | O(1) avg | Lock-free |
| `size()` / `empty()` | O(1) | Lock-free |
| Ordered iteration | O(n) | Multiple concurrent readers |
| `top_k()` / `bottom_k()` | O(k) | Multiple concurrent readers |

## Quick Start

### Basic Usage

```cpp
#include "reactive_two_field_collection.h"

using namespace reactive;

// Create collection with default aggregation (Add mode for both totals)
ReactiveTwoFieldCollection<double, long> collection;

// Insert elements
auto id1 = collection.push_one(1.5, 10);
auto id2 = collection.push_one(2.0, 20);
auto id3 = collection.push_one(0.5, 15);

// Query aggregates (lock-free reads)
std::cout << "Size: " << collection.size() << "\n";              // 3
std::cout << "Total1: " << collection.total1() << "\n";          // 45 (sum of elem2)
std::cout << "Total2: " << collection.total2() << "\n";          // 67.5 (sum of elem1*elem2)

// Erase element
collection.erase(id2);
std::cout << "Size after erase: " << collection.size() << "\n";  // 2
```

### With Ordered Index

```cpp
// Enable ordered indexing for sorted iteration
ReactiveTwoFieldCollection<
    double, long,
    long, double,
    /* Delta1Fn */ detail::DefaultDelta1<double, long, long>,
    /* Apply1Fn */ detail::DefaultApplyAdd<long>,
    /* Delta2Fn */ detail::DefaultDelta2<double, long, double>,
    /* Apply2Fn */ detail::DefaultApplyAdd<double>,
    /* KeyT */ std::monostate,
    /* Total1Mode */ AggMode::Min,
    /* Total2Mode */ AggMode::Max,
    /* Extract1Fn */ DefaultExtract1<double, long, long>,
    /* Extract2Fn */ DefaultExtract2<double, long, double>,
    /* RequireCoarseLock */ false,
    /* MaintainOrderedIndex */ true  // Enable ordered iteration
> orderedCollection;

orderedCollection.push_one(3.0, 5);
orderedCollection.push_one(1.0, 10);
orderedCollection.push_one(2.0, 20);

// Iterate in sorted order (multiple threads can iterate concurrently!)
for (auto it = orderedCollection.ordered_begin(); 
     it != orderedCollection.ordered_end(); ++it) {
    auto [id, snapshot] = *it;
    std::cout << "ID: " << id 
              << " elem1: " << snapshot.lastElem1
              << " elem2: " << snapshot.lastElem2 << "\n";
}

// Query top-k elements
auto top3 = orderedCollection.top_k(3);
```

### With Key-Based Lookup

```cpp
// Use std::string as key type for fast lookup
ReactiveTwoFieldCollection<
    double, long,
    long, double,
    detail::DefaultDelta1<double, long, long>,
    detail::DefaultApplyAdd<long>,
    detail::DefaultDelta2<double, long, double>,
    detail::DefaultApplyAdd<double>,
    std::string  // KeyT - enables find_by_key()
> keyedCollection;

// Insert with keys
auto id1 = keyedCollection.push_one(1.5, 10, "sensor_1");
auto id2 = keyedCollection.push_one(2.0, 20, "sensor_2");

// Fast O(1) lock-free lookup
auto found = keyedCollection.find_by_key("sensor_1");
if (found) {
    std::cout << "Found ID: " << *found << "\n";
}
```

### Reactive Callbacks

```cpp
ReactiveTwoFieldCollection<double, long> collection;

// React to total changes
auto monitor = collection.total1_var().monitor([](long newTotal) {
    std::cout << "Total changed to: " << newTotal << "\n";
});

collection.push_one(1.0, 5);   // Prints: "Total changed to: 5"
collection.push_one(2.0, 10);  // Prints: "Total changed to: 15"
```

## Requirements

- **C++20** compiler (GCC 10+, Clang 12+, MSVC 2019+)
- **Intel TBB** (Threading Building Blocks)
- **Reaction library** (reactive programming framework)
- **vcpkg** (recommended for dependency management)

## Installation

### Using vcpkg

```bash
# Install dependencies
vcpkg install tbb reaction

# Configure with CMake
cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build

# Run demo
./build/demo
```

### Manual Installation

1. Install [Intel TBB](https://github.com/oneapi-src/oneTBB)
2. Install [Reaction](https://github.com/snncpp/reaction)
3. Include `reactive_two_field_collection.h` in your project
4. Link against TBB and Reaction libraries

## Architecture

### Thread Safety Model

```
┌─────────────────────────────────────────────────────┐
│                 Lock-Free Operations                 │
│  - ID generation (std::atomic)                      │
│  - size() / empty() (std::atomic)                   │
│  - Hash operations (TBB concurrent_hash_map)        │
│  - find_by_key() (TBB concurrent_hash_map)         │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│          Concurrent Read Operations                  │
│  - Ordered iteration (std::shared_mutex)            │
│  - top_k() / bottom_k() (std::shared_mutex)        │
│  → Multiple readers can proceed simultaneously      │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│            Serialized Operations                     │
│  - push_one() / erase() on ordered index            │
│  - Total aggregate updates (for Min/Max modes)      │
│  → Writers acquire exclusive locks                  │
└─────────────────────────────────────────────────────┘
```

### Use Case: ImGui + NATS Message Bus

Perfect for scenarios where:
- **ImGui rendering thread** iterates to display elements (60 FPS)
- **NATS message handlers** push updates from message bus
- **Both proceed concurrently** without blocking each other

**Performance**: 50-80% higher message throughput with smooth rendering!

## API Reference

### Core Methods

```cpp
// Element Management
[[nodiscard]] id_type push_one(elem1_type e1, elem2_type e2, key_type key = {});
bool erase(id_type id);

// Queries (Lock-Free)
[[nodiscard]] size_t size() const noexcept;
[[nodiscard]] bool empty() const noexcept;
[[nodiscard]] std::optional<id_type> find_by_key(const KeyT& key) const;  // if KeyT != monostate

// Aggregates (Lock-Free for Add mode)
[[nodiscard]] total1_type total1() const;
[[nodiscard]] total2_type total2() const;
[[nodiscard]] reaction::Var<total1_type>& total1Var() noexcept;  // For reactive callbacks
[[nodiscard]] reaction::Var<total2_type>& total2Var() noexcept;

// Ordered Iteration (Concurrent Reads)
[[nodiscard]] OrderedConstIterator ordered_begin() const;  // if MaintainOrderedIndex
[[nodiscard]] OrderedConstIterator ordered_end() const;
[[nodiscard]] std::vector<id_type> top_k(size_t k) const;
[[nodiscard]] std::vector<id_type> bottom_k(size_t k) const;

// Comparator Management
void reset(compare_fn_t new_cmp);  // Change ordering dynamically
void rebuild_ordered_index();       // Rebuild after bulk updates
```

### Template Parameters

```cpp
template <
    typename Elem1T = double,           // First element field type
    typename Elem2T = long,             // Second element field type
    typename Total1T = Elem2T,          // First aggregate type
    typename Total2T = double,          // Second aggregate type
    typename Delta1Fn = ...,            // Delta computation for total1
    typename Apply1Fn = ...,            // Apply function for total1
    typename Delta2Fn = ...,            // Delta computation for total2
    typename Apply2Fn = ...,            // Apply function for total2
    typename KeyT = std::monostate,     // Key type (monostate = no keys)
    AggMode Total1Mode = AggMode::Add,  // Add, Min, or Max
    AggMode Total2Mode = AggMode::Add,  // Add, Min, or Max
    typename Extract1Fn = ...,          // Extract value for Min/Max mode
    typename Extract2Fn = ...,          // Extract value for Min/Max mode
    bool RequireCoarseLock = false,     // Legacy compatibility mode
    bool MaintainOrderedIndex = false,  // Enable ordered iteration
    typename CompareFn = ...,           // Custom element comparator
    template <typename...> class MapType = std::unordered_map
>
class ReactiveTwoFieldCollection;
```

## Examples

See the `examples/` directory (or `main.cpp` and `test_lock_free.cpp`) for:
- Basic usage
- Ordered iteration
- Min/Max aggregation
- Concurrent stress tests
- ImGui integration patterns

## Performance Optimization Phases

This library has undergone three optimization phases:

| Phase | Improvement | Key Changes |
|-------|-------------|-------------|
| **Phase 1** | Atomic counters | Lock-free ID generation and size tracking |
| **Phase 2** | TBB concurrent maps | Lock-free hash operations, O(1) key lookup |
| **Phase 3** | Concurrent reads | Multiple threads can iterate simultaneously |

**Combined**: 50-100% throughput improvement vs baseline under high contention.

See `OPTIMIZATION_SUMMARY.md` for detailed performance analysis.

## Documentation

- **OPTIMIZATION_SUMMARY.md** - Complete performance optimization journey
- **PHASE1_SUMMARY.md** - Atomic counters implementation
- **PHASE2_COMPLETE.md** - TBB concurrent hash maps
- **PHASE3_COMPLETE.md** - Concurrent ordered index
- **REMAINING_OPTIMIZATIONS.md** - Future improvement opportunities
- **CODE_QUALITY_IMPROVEMENTS.md** - Code quality and modern C++ suggestions

## Contributing

Contributions welcome! Please:
1. Follow the existing code style (see `.clang-format`)
2. Add tests for new features
3. Update documentation
4. Ensure all tests pass

## License

[Specify your license here]

## Acknowledgments

- **Intel TBB**: High-performance concurrent data structures
- **Reaction**: Reactive programming framework
- **Community**: Thanks to all contributors and users

## Contact

[Your contact information or project links]

---

**Note**: This is a header-only library. Simply include `reactive_two_field_collection.h` and link against TBB and Reaction to get started!
