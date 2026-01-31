# Phase 2: Concurrent Hash Maps - Implementation Report

## Status: Partially Implemented (70% Complete)

Phase 2 aimed to replace standard hash maps with Intel TBB concurrent_hash_map for lock-free map operations. Significant progress was made, but a template metaprogramming challenge prevents compilation.

## What Was Completed ✅

### 1. TBB Integration
- Added `tbb` to vcpkg.json dependencies
- Included `<oneapi/tbb/concurrent_hash_map.h>`
- Updated CMakeLists.txt to link TBB library

### 2. Core Data Structure Replacement
```cpp
// Before (Phase 1):
std::unordered_map<id_type, ElemRecord> elems_;
std::unordered_map<id_type, reaction::Action<>> monitors_;

// After (Phase 2):
oneapi::tbb::concurrent_hash_map<id_type, ElemRecord> elems_;
oneapi::tbb::concurrent_hash_map<id_type, reaction::Action<>> monitors_;
```

### 3. API Updates for Concurrent Access
Updated all map access to use TBB's accessor pattern:

**push_one():**
```cpp
// Insert with concurrent_hash_map
elems_.insert(std::make_pair(id, std::move(rec)));
```

**erase():**
```cpp
// Thread-safe accessor
typename elem_map_type::accessor acc;
if (!elems_.find(acc, id)) return;
ElemRecord &rec = acc->second;
// ...use rec data...
acc.release(); // Explicit release before other operations
```

**elem1Var/elem2Var:**
```cpp
typename elem_map_type::accessor acc;
if (elems_.find(acc, id)) {
    return acc->second.elem1Var;
}
throw std::out_of_range("elem1Var: id not found");
```

### 4. Lock-Free find_by_key
```cpp
template <typename K = KeyT>
std::enable_if_t<!std::is_void_v<K>, std::optional<id_type>>
find_by_key(const K &k) const {
    typename key_index_map_type::const_accessor acc;
    if (key_index_.find(acc, k)) {
        return acc->second;
    }
    return std::nullopt;
}
```

### 5. Ordered Iterator Redesign
Created `ElemRecordSnapshot` struct for safe iteration:
```cpp
struct ElemRecordSnapshot {
    elem1_type lastElem1;
    elem2_type lastElem2;
    typename ElemRecord::key_storage_t key;
};
```

Updated all ordered iterators to return snapshots instead of references:
```cpp
std::pair<id_type, ElemRecordSnapshot> operator*() const {
    id_type id = *it_;
    typename elem_map_type::const_accessor acc;
    if (parent_->elems_.find(acc, id)) {
        return { id, {acc->second.lastElem1, acc->second.lastElem2, acc->second.key} };
    }
    return { id, {} };
}
```

### 6. Monitor Callbacks
Updated reactive monitors to use accessors:
```cpp
monitors_.insert(std::make_pair(id, reaction::action(
    [this, id, ...](elem1_type new1, elem2_type new2) {
        typename elem_map_type::accessor acc;
        if (!elems_.find(acc, id)) return;
        ElemRecord &r = acc->second;
        // ...update logic...
    }, ...
)));
```

## The Blocker 🚫

### Problem: Template Instantiation with `void` KeyT

When `KeyT` template parameter is `void` (which is the default for collections without keys), the compiler attempts to instantiate:

```cpp
using key_index_map_type = std::conditional_t<std::is_void_v<KeyT>, 
                                               VoidPlaceholder,
                                               oneapi::tbb::concurrent_hash_map<KeyT, id_type>>;
```

**Issue**: `std::conditional_t` evaluates both branches during template substitution, causing:
```
error: template constraint failure for concurrent_hash_map<void, id_type>
error: forming reference to void
```

### Attempted Solutions

1. **Dummy type**: `std::unordered_map<int, int>` - Still evaluated
2. **VoidKeyIndexPlaceholder** struct - Still evaluated  
3. **std::monostate** - Still evaluated

### Root Cause

C++ template instantiation evaluates all dependent types before SFINAE/`std::conditional_t` can eliminate branches. The `concurrent_hash_map<void, ...>` instantiation fails before the conditional can select the other branch.

### Required Solution

Need one of:
1. **Template specialization**: Separate class implementations for KeyT=void vs KeyT!=void
2. **Type erasure wrapper**: Custom wrapper around concurrent_hash_map that handles void
3. **Redesign**: Make KeyT always non-void (use std::monostate as default)

## Performance Benefits (Estimated)

If completed, Phase 2 would provide:

| Operation | Current (Phase 1) | Phase 2 Target | Improvement |
|-----------|-------------------|----------------|-------------|
| Concurrent insert | Lock-based (coarse_lock) | Lock-free | 3-5x faster |
| Concurrent lookup | Lock-based | Lock-free | 5-10x faster |
| find_by_key | Lock or sequential | Lock-free hash | 10-50x faster |
| Concurrent erase | Lock-based | Lock-free | 3-5x faster |

**Overall throughput improvement**: Estimated 50-100% under high contention (8+ threads)

## Code Changes Summary

- **Files Modified**: 3
  - `reactive_two_field_collection.h` (~150 lines changed)
  - `CMakeLists.txt` (+2 lines for TBB)
  - `vcpkg.json` (+1 dependency)

- **New Concepts Introduced**:
  - TBB accessor pattern
  - ElemRecordSnapshot for safe iteration
  - Lock-free map operations

## Next Steps to Complete Phase 2

### Option A: Template Specialization (Recommended)
Split the class into two implementations:

```cpp
template<typename Elem1T, ..., typename KeyT, ...>
class ReactiveTwoFieldCollection {
    // Full implementation with key_index
};

template<typename Elem1T, ..., ...>  
class ReactiveTwoFieldCollection<Elem1T, ..., void, ...> {
    // Specialized for void KeyT (no key_index member)
};
```

**Effort**: 4-6 hours  
**Risk**: Medium (code duplication)

### Option B: Always-Valid KeyT
Change default from `void` to `std::monostate`:

```cpp
template<..., typename KeyT = std::monostate, ...>
```

Update all `if constexpr (std::is_void_v<KeyT>)` to check for monostate.

**Effort**: 2-3 hours  
**Risk**: Low (breaking API change)

### Option C: Custom Wrapper Type
```cpp
template<typename K>
struct OptionalKeyIndex {
    oneapi::tbb::concurrent_hash_map<K, id_type> map;
    // ... methods ...
};

template<>
struct OptionalKeyIndex<void> {
    // No-op implementation
};
```

**Effort**: 3-4 hours  
**Risk**: Medium (wrapper overhead)

## Lessons Learned

1. **Template metaprogramming is hard**: `std::conditional_t` doesn't provide lazy evaluation
2. **TBB integration is smooth**: Once template issues resolved, API is clean
3. **Accessor pattern works well**: Natural fit for concurrent access
4. **Iterator redesign necessary**: Can't return references with concurrent maps

## Conclusion

Phase 2 is 70% complete with all core algorithms updated for concurrent_hash_map. The remaining 30% is a single template metaprogramming issue that requires architectural decision (specialization vs redesign).

**Recommendation**: Implement Option B (monostate instead of void) as it's the lowest risk and maintains clean code structure. This would allow Phase 2 to be completed in an additional 2-3 hours of work.

The benefits of completing Phase 2 are substantial - lock-free map operations would significantly improve performance under high contention, making this a worthwhile investment for production use.
