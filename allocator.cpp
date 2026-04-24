#include "allocator.hpp"
#include <cstring>
#include <cstdlib>
#include <algorithm>

TLSFAllocator::TLSFAllocator(std::size_t memoryPoolSize) : memoryPool(nullptr), poolSize(memoryPoolSize) {
    index.fliBitmap = 0;
    for (int i = 0; i < FLI_SIZE; ++i) {
        index.sliBitmaps[i] = 0;
        for (int j = 0; j < SLI_SIZE; ++j) {
            index.freeLists[i][j] = nullptr;
        }
    }
    initializeMemoryPool(memoryPoolSize);
}

TLSFAllocator::~TLSFAllocator() {
    if (memoryPool) {
        std::free(memoryPool);
    }
}

int TLSFAllocator::fls(std::size_t x) {
    if (x == 0) return -1;
    int position = 0;
    while (x > 1) {
        x >>= 1;
        position++;
    }
    return position;
}

void TLSFAllocator::mappingFunction(std::size_t size, int& fli, int& sli) {
    if (size < (1ULL << SLI_BITS)) {
        fli = 0;
        sli = static_cast<int>(size);
    } else {
        fli = fls(size);
        std::size_t base = 1ULL << fli;
        std::size_t offset = size - base;
        int divisions = std::min(static_cast<int>(base), SLI_SIZE);
        std::size_t slotSize = base / divisions;
        sli = static_cast<int>(offset / slotSize);
        
        if (fli >= FLI_SIZE) fli = FLI_SIZE - 1;
        if (sli >= SLI_SIZE) sli = SLI_SIZE - 1;
    }
}

void TLSFAllocator::initializeMemoryPool(std::size_t size) {
    memoryPool = std::malloc(size);
    if (!memoryPool) {
        return;
    }
    
    TLSFAllocator::FreeBlock* initialBlock = static_cast<TLSFAllocator::FreeBlock*>(memoryPool);
    initialBlock->data = static_cast<char*>(memoryPool) + sizeof(TLSFAllocator::FreeBlock);
    initialBlock->size = size;
    initialBlock->isFree = true;
    initialBlock->prevPhysBlock = nullptr;
    initialBlock->nextPhysBlock = nullptr;
    initialBlock->prevFree = nullptr;
    initialBlock->nextFree = nullptr;
    
    insertFreeBlock(initialBlock);
}

void TLSFAllocator::insertFreeBlock(TLSFAllocator::FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);
    
    block->nextFree = index.freeLists[fli][sli];
    block->prevFree = nullptr;
    
    if (index.freeLists[fli][sli]) {
        index.freeLists[fli][sli]->prevFree = block;
    }
    
    index.freeLists[fli][sli] = block;
    index.fliBitmap |= (1U << fli);
    index.sliBitmaps[fli] |= (1U << sli);
}

void TLSFAllocator::removeFreeBlock(TLSFAllocator::FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);
    
    if (block->prevFree) {
        block->prevFree->nextFree = block->nextFree;
    } else {
        index.freeLists[fli][sli] = block->nextFree;
    }
    
    if (block->nextFree) {
        block->nextFree->prevFree = block->prevFree;
    }
    
    if (!index.freeLists[fli][sli]) {
        index.sliBitmaps[fli] &= ~(1U << sli);
        if (index.sliBitmaps[fli] == 0) {
            index.fliBitmap &= ~(1U << fli);
        }
    }
    
    block->prevFree = nullptr;
    block->nextFree = nullptr;
}

TLSFAllocator::FreeBlock* TLSFAllocator::findSuitableBlock(std::size_t size) {
    int fli, sli;
    mappingFunction(size, fli, sli);
    
    std::uint16_t slBitmap = index.sliBitmaps[fli] & (~0U << sli);
    
    if (slBitmap == 0) {
        std::uint32_t flBitmap = index.fliBitmap & (~0U << (fli + 1));
        if (flBitmap == 0) {
            return nullptr;
        }
        
        fli = __builtin_ctz(flBitmap);
        slBitmap = index.sliBitmaps[fli];
    }
    
    sli = __builtin_ctz(slBitmap);
    return index.freeLists[fli][sli];
}

void TLSFAllocator::splitBlock(TLSFAllocator::FreeBlock* block, std::size_t size) {
    std::size_t minBlockSize = sizeof(TLSFAllocator::FreeBlock) + 8;
    
    if (block->size >= size + minBlockSize) {
        TLSFAllocator::FreeBlock* newBlock = reinterpret_cast<TLSFAllocator::FreeBlock*>(
            static_cast<char*>(static_cast<void*>(block)) + size
        );
        
        newBlock->size = block->size - size;
        newBlock->isFree = true;
        newBlock->prevPhysBlock = block;
        newBlock->nextPhysBlock = block->nextPhysBlock;
        newBlock->data = static_cast<char*>(static_cast<void*>(newBlock)) + sizeof(TLSFAllocator::FreeBlock);
        
        if (block->nextPhysBlock) {
            block->nextPhysBlock->prevPhysBlock = newBlock;
        }
        
        block->size = size;
        block->nextPhysBlock = newBlock;
        
        insertFreeBlock(newBlock);
    }
}

void TLSFAllocator::mergeAdjacentFreeBlocks(TLSFAllocator::FreeBlock* block) {
    if (block->nextPhysBlock && block->nextPhysBlock->isFree) {
        TLSFAllocator::FreeBlock* nextBlock = static_cast<TLSFAllocator::FreeBlock*>(block->nextPhysBlock);
        removeFreeBlock(nextBlock);
        
        block->size += nextBlock->size;
        block->nextPhysBlock = nextBlock->nextPhysBlock;
        
        if (block->nextPhysBlock) {
            block->nextPhysBlock->prevPhysBlock = block;
        }
    }
    
    if (block->prevPhysBlock && block->prevPhysBlock->isFree) {
        TLSFAllocator::FreeBlock* prevBlock = static_cast<TLSFAllocator::FreeBlock*>(block->prevPhysBlock);
        removeFreeBlock(prevBlock);
        
        prevBlock->size += block->size;
        prevBlock->nextPhysBlock = block->nextPhysBlock;
        
        if (prevBlock->nextPhysBlock) {
            prevBlock->nextPhysBlock->prevPhysBlock = prevBlock;
        }
        
        block = prevBlock;
    }
    
    insertFreeBlock(block);
}

void* TLSFAllocator::allocate(std::size_t size) {
    if (size == 0) return nullptr;
    
    std::size_t totalSize = size + sizeof(TLSFAllocator::FreeBlock);
    totalSize = (totalSize + 7) & ~7ULL;
    
    TLSFAllocator::FreeBlock* block = findSuitableBlock(totalSize);
    if (!block) {
        return nullptr;
    }
    
    removeFreeBlock(block);
    splitBlock(block, totalSize);
    
    block->isFree = false;
    return block->data;
}

void TLSFAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    
    TLSFAllocator::FreeBlock* block = reinterpret_cast<TLSFAllocator::FreeBlock*>(
        static_cast<char*>(ptr) - sizeof(TLSFAllocator::FreeBlock)
    );
    
    if (block->isFree) return;
    
    block->isFree = true;
    mergeAdjacentFreeBlocks(block);
}

void* TLSFAllocator::getMemoryPoolStart() const {
    return memoryPool;
}

std::size_t TLSFAllocator::getMemoryPoolSize() const {
    return poolSize;
}

std::size_t TLSFAllocator::getMaxAvailableBlockSize() const {
    if (index.fliBitmap == 0) return 0;
    
    int fli = 31 - __builtin_clz(index.fliBitmap);
    std::uint16_t slBitmap = index.sliBitmaps[fli];
    
    if (slBitmap == 0) return 0;
    
    int sli = 31 - __builtin_clz(slBitmap);
    TLSFAllocator::FreeBlock* block = index.freeLists[fli][sli];
    
    std::size_t maxSize = 0;
    while (block) {
        if (block->size > maxSize) {
            maxSize = block->size;
        }
        block = block->nextFree;
    }
    
    return maxSize > sizeof(TLSFAllocator::FreeBlock) ? maxSize - sizeof(TLSFAllocator::FreeBlock) : 0;
}
