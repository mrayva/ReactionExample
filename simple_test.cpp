#include <iostream>
#include "reactive_two_field_collection.h"

using namespace reactive;

int main() {
    std::cout << "Simple phmap test\n";
    
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
        false, false  // No ordered index
    >;
    
    Coll c({}, {}, {}, {}, false, false);
    
    std::cout << "Inserting element...\n";
    auto id1 = c.push_back(1.5, 10);
    std::cout << "Inserted id=" << id1 << "\n";
    
    std::cout << "Size: " << c.size() << "\n";
    
    std::cout << "Updating element...\n";
    c.elem2Var(id1).value(20);
    std::cout << "Updated!\n";
    
    std::cout << "Success!\n";
    return 0;
}
