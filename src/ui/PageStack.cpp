#include "ui/PageStack.hpp"

#include <algorithm>

namespace mc::ui {

PageStack::PageStack(PageId root) : pages_{root} {}

PageId PageStack::current() const { return pages_.back(); }
PageId PageStack::root() const { return pages_.front(); }

bool PageStack::contains(PageId page) const {
    return std::ranges::find(pages_, page) != pages_.end();
}

void PageStack::push(PageId page) {
    if (pages_.back() != page) pages_.push_back(page);
}

bool PageStack::pop() {
    if (pages_.size() <= 1U) return false;
    pages_.pop_back();
    return true;
}

void PageStack::replace(PageId page) { pages_.back() = page; }

void PageStack::reset(PageId root) {
    pages_.clear();
    pages_.push_back(root);
}

} // namespace mc::ui
