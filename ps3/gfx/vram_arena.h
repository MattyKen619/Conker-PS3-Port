// A simple allocator for one big block of RSX memory, with its bookkeeping in main memory.
//
// PSL1GHT's rsxMemalign keeps its block headers inside RSX memory itself, next to the allocations;
// anything the RSX writes past the end of a render target or texture corrupts them, and the next
// allocation can then loop forever. Here the free list lives in a std::map, and every allocation is
// followed by a small unused gap.
#pragma once
#include <cstdint>
#include <map>

namespace n64gfx {

class VramArena {
public:
    void init(uint8_t* base, uint32_t size) {
        base_ = base;
        size_ = size;
        free_.clear();
        used_.clear();
        free_[0] = size;
    }
    // Returns nullptr if there is no room.
    void* alloc(uint32_t bytes, uint32_t align) {
        if (!base_ || bytes == 0) return nullptr;
        if (align < 128) align = 128;
        const uint32_t need = bytes + kGap;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            const uint32_t start = it->first, len = it->second;
            const uint32_t at = (start + align - 1) & ~(align - 1);
            if (at < start || at - start > len || len - (at - start) < need) continue;
            const uint32_t end = at + need;
            free_.erase(it);
            if (at > start) free_[start] = at - start;
            if (start + len > end) free_[end] = start + len - end;
            used_[at] = need;
            in_use_ += need;
            return base_ + at;
        }
        return nullptr;
    }
    void free(void* p) {
        if (!p) return;
        const uint32_t at = (uint32_t)((uint8_t*)p - base_);
        auto u = used_.find(at);
        if (u == used_.end()) return;
        uint32_t start = at, len = u->second;
        in_use_ -= len;
        used_.erase(u);
        // Merge with the free neighbours.
        auto next = free_.find(start + len);
        if (next != free_.end()) { len += next->second; free_.erase(next); }
        auto prev = free_.lower_bound(start);
        if (prev != free_.begin()) {
            --prev;
            if (prev->first + prev->second == start) { start = prev->first; len += prev->second; free_.erase(prev); }
        }
        free_[start] = len;
    }
    uint32_t in_use() const { return in_use_; }
    uint32_t size() const { return size_; }
    size_t blocks() const { return used_.size(); }

private:
    static constexpr uint32_t kGap = 256;
    uint8_t* base_ = nullptr;
    uint32_t size_ = 0, in_use_ = 0;
    std::map<uint32_t, uint32_t> free_;  // offset -> length
    std::map<uint32_t, uint32_t> used_;  // offset -> length (including the gap)
};

}  // namespace n64gfx
