#pragma once
#include <algorithm>
#include <memory>
#include <vector>

// Copies share immutable pages. A mutation detaches only the affected page,
// allowing background history snapshots without copying every path on the UI.
template<class T, size_t PageSize = 256> class PagedVector {
    using Page = std::vector<T>;
    std::vector<std::shared_ptr<Page>> pages;
    size_t count = 0;
public:
    PagedVector() = default;
    PagedVector(std::vector<T>&& source) { for (auto& value : source) push_back(std::move(value)); }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const T& operator[](size_t i) const { return (*pages[i / PageSize])[i % PageSize]; }
    T& operator[](size_t i) {
        auto& page = pages[i / PageSize];
        if (page.use_count() != 1) page = std::make_shared<Page>(*page);
        return (*page)[i % PageSize];
    }
    const T& front() const { return (*this)[0]; }
    const T& back() const { return (*this)[count - 1]; }
    void push_back(T value) {
        if (count % PageSize == 0) { pages.push_back(std::make_shared<Page>()); pages.back()->reserve(PageSize); }
        auto& page = pages.back();
        if (page.use_count() != 1) page = std::make_shared<Page>(*page);
        page->push_back(std::move(value)); ++count;
    }
    struct Iterator {
        const PagedVector* owner;
        size_t index;
        const T& operator*() const { return (*owner)[index]; }
        Iterator& operator++() { ++index; return *this; }
        bool operator!=(const Iterator& other) const { return index != other.index; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, count}; }
};
