// Begränsad, trådsäker LRU-cache med minnesbudget. Nycklarna härleds ur innehållet (SHA-256 och
// parametrar), så cachen påverkar bara hastigheten – aldrig resultatet – och kan tömmas när som helst.
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pc::docview {

class Cache {
public:
    explicit Cache(size_t budget_bytes = size_t(1) << 30);
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    // Hämta eller skapa. make() körs utanför låset och ger (värde, kostnad i byte).
    template <class T>
    std::shared_ptr<const T> get(const std::string& key, const std::function<std::pair<std::shared_ptr<const T>, size_t>()>& make) {
        if (auto v = find(key)) return std::static_pointer_cast<const T>(v);
        auto [value, cost] = make();
        return std::static_pointer_cast<const T>(insert(key, value, cost));
    }
    template <class T>
    std::shared_ptr<const T> peek(const std::string& key) {
        return std::static_pointer_cast<const T>(find(key));
    }
    template <class T>
    std::shared_ptr<const T> put(const std::string& key, std::shared_ptr<const T> value, size_t cost) {
        return std::static_pointer_cast<const T>(insert(key, std::move(value), cost));
    }

    size_t used() const;
    size_t budget() const { return budget_; }
    void clear();

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const void> value;
        size_t cost;
    };
    std::shared_ptr<const void> find(const std::string& key);
    std::shared_ptr<const void> insert(const std::string& key, std::shared_ptr<const void> value, size_t cost);

    size_t budget_;
    size_t used_ = 0;
    mutable std::mutex m_;
    std::list<Entry> lru_;   // senast använd först
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
};

}  // namespace pc::docview
