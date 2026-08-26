/**
 * \file ItemRegistry.cpp
 * \ingroup config
 * \brief Implementation of the provider registry.
 */
#include "ItemRegistry.h"

#include <algorithm>

namespace mfly {

Registry& Registry::Instance() {
    static Registry instance;
    return instance;
}

void Registry::Register(std::wstring name, int sortOrder, Provider provider) {
    entries_.push_back(Entry{std::move(name), sortOrder, std::move(provider)});
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.sortOrder < b.sortOrder; });
}

ItemList Registry::Collect(const Context& ctx) const {
    ItemList list;
    for (const Entry& e : entries_) {
        const size_t before = list.items().size();
        if (e.provider) e.provider(ctx, list);
        if (list.items().size() != before && &e != &entries_.back()) {
            list.AddSeparator();
        }
    }
    list.TrimTrailingSeparator();
    return list;
}

}  // namespace mfly
