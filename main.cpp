#include "allocator.hpp"
#include <iostream>
#include <string>
#include <map>

int main() {
    std::size_t poolSize;
    int numOperations;
    
    std::cin >> poolSize >> numOperations;
    
    TLSFAllocator allocator(poolSize);
    std::map<int, void*> allocations;
    
    for (int i = 0; i < numOperations; ++i) {
        std::string operation;
        std::cin >> operation;
        
        if (operation == "allocate") {
            int id;
            std::size_t size;
            std::cin >> id >> size;
            
            void* ptr = allocator.allocate(size);
            if (ptr) {
                allocations[id] = ptr;
                std::cout << "1" << std::endl;
            } else {
                std::cout << "0" << std::endl;
            }
        } else if (operation == "deallocate") {
            int id;
            std::cin >> id;
            
            if (allocations.find(id) != allocations.end()) {
                allocator.deallocate(allocations[id]);
                allocations.erase(id);
                std::cout << "1" << std::endl;
            } else {
                std::cout << "0" << std::endl;
            }
        } else if (operation == "max") {
            std::cout << allocator.getMaxAvailableBlockSize() << std::endl;
        }
    }
    
    return 0;
}
