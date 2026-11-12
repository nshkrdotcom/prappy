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
        if (count % PageSize == 0) {
            // Prepare the entire first element before publishing a new page.
            // On allocation/element failure, a ghost empty page would make the
            // next successful operator[] index past that page's end.
            auto page = std::make_shared<Page>();
            page->reserve(PageSize);
            page->push_back(std::move(value));
            pages.push_back(std::move(page));
        } else {
            auto& page = pages.back();
            if (page.use_count() != 1) {
                // Snapshot readers must never observe a partially mutated page.
                auto replacement = std::make_shared<Page>(*page);
                replacement->push_back(std::move(value));
                page = std::move(replacement);
            } else page->push_back(std::move(value));
        }
        ++count;
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
