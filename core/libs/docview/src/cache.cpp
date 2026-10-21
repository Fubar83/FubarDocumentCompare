#include "printcmp/docview/cache.hpp"

namespace pc::docview {

Cache::Cache(size_t budget_bytes) : budget_(budget_bytes) {}

size_t Cache::used() const {
    std::lock_guard<std::mutex> l(m_);
    return used_;
}

void Cache::clear() {
    std::lock_guard<std::mutex> l(m_);
    lru_.clear();
    index_.clear();
    used_ = 0;
}

std::shared_ptr<const void> Cache::find(const std::string& key) {
    std::lock_guard<std::mutex> l(m_);
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->value;
}

std::shared_ptr<const void> Cache::insert(const std::string& key, std::shared_ptr<const void> value, size_t cost) {
    std::lock_guard<std::mutex> l(m_);
    if (auto it = index_.find(key); it != index_.end()) {   // någon annan hann före: använd deras
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->value;
    }
    if (cost > budget_) return value;   // ryms aldrig: lämna ut utan att cacha
    lru_.push_front({key, value, cost});
    index_[key] = lru_.begin();
    used_ += cost;
    while (used_ > budget_ && !lru_.empty()) {   // de som redan lämnats ut lever vidare hos anroparen
        used_ -= lru_.back().cost;
        index_.erase(lru_.back().key);
        lru_.pop_back();
    }
    return value;
}

}  // namespace pc::docview
