#include <iostream>
#include <vector>
#include <string>

#include <reaction/reaction.h>
#include "reactive_two_field_collection.h"

/*
  - Demonstrates non-keyed and keyed collections, updates, batch push, iteration, and erase.
*/

int main() {
    using namespace reaction;

    // Non-keyed collection: elem1 = double, elem2 = long
    using Coll = reactive::ReactiveTwoFieldCollection<double, long>;
    Coll c;

    // Observer: parameter-binding style with explicit types (long, double)
    auto printer = action(
        [](long tot1, double tot2) {
            std::cout << "[observer] total1=" << tot1 << " total2=" << tot2 << "\n";
        },
        c.total1Var(), c.total2Var()
    );

    // Push elements
    auto id1 = c.push_back(1.2, 10); // elem1=1.2, elem2=10
    auto id2 = c.push_back(2.5, 3);  // elem1=2.5, elem2=3

    std::cout << "After pushes: total1=" << c.total1() << " total2=" << c.total2() << "\n";

    // Update one field (elem1) of id1 -> affects total2 (weighted sum by default)
    c.elem1Var(id1).value(1.5); // elem1 changed from 1.2 -> 1.5

    // Update elem2 of id2 -> affects both totals
    c.elem2Var(id2).value(5);

    // Batch multiple pushes (uses internal batchExecute)
    std::vector<std::pair<double,long>> extra = {{3.0,2}, {4.0,1}};
    c.push_back(extra);

    std::cout << "After updates & batch push: total1=" << c.total1() << " total2=" << c.total2() << "\n";

    // Iterate over elements (id -> ElemRecord)
    std::cout << "Elements:\n";
    for (auto &kv : c) {
        auto id = kv.first;
        const auto &rec = kv.second;
        std::cout << "  id=" << id
                  << " elem1=" << rec.elem1Var.get()
                  << " elem2=" << rec.elem2Var.get() << "\n";
    }

    // Erase an element
    c.erase(id1);
    std::cout << "After erase id1: total1=" << c.total1() << " total2=" << c.total2() << "\n";

    // ---------------- Keyed collection example ----------------
	using KeyColl = reactive::ReactiveTwoFieldCollection<
		double, long,                    // Elem1T, Elem2T
		double, double,                  // Total1T, Total2T
		reactive::detail::DefaultDelta1<double,long,double>,
		reactive::detail::DefaultApplyAdd<double>,
		reactive::detail::DefaultDelta2<double,long,double>,
		reactive::detail::DefaultApplyAdd<double>,
		std::string                      // KeyT
		// all remaining template parameters use their defaults:
		// Total1Mode=Add, Total2Mode=Add, Extractors=defaults, RequireCoarseLock=false, MapType=std::unordered_map
	>;

    KeyColl kc;

    // Observer for keyed collection (explicit types double,double to match totals)
    auto kprinter = action(
        [](double tot1, double tot2) {
            std::cout << "[kc observer] total1=" << tot1 << " total2=" << tot2 << "\n";
        },
        kc.total1Var(), kc.total2Var()
    );

    // push keyed record
    auto kid = kc.push_back(1.5, 4, std::string("rec-A"));

    // lookup by key (fast O(1))
    auto found = kc.find_by_key(std::string("rec-A"));
    if (found) {
        std::cout << "Found rec-A -> id = " << *found << "\n";
        // update fields via id
        kc.elem2Var(*found).value(6); // change elem2
        std::cout << "kc totals: total1=" << kc.total1() << " total2=" << kc.total2() << "\n";

        // erase by key
        kc.erase_by_key(std::string("rec-A"));
        std::cout << "After erase_by_key: kc.total1=" << kc.total1() << " kc.total2=" << kc.total2() << "\n";
    } else {
        std::cout << "rec-A not found\n";
    }

    // cleanup
    printer.close();
    kprinter.close();

    return 0;
}