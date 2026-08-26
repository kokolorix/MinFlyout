/**
 * \file ItemRegistry.h
 * \ingroup config
 * \brief Provider registry: collects the items for a target window.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Collecting container the providers write their items into.
 *
 * Providers append their items here; the controller passes the finished list on
 * to the \ref FlyoutWindow.
 */
class ItemList {
public:
    /**
     * \brief Appends a ready-made item.
     * \param item The item; is taken over.
     */
    void Add(Item item) { items_.push_back(std::move(item)); }

    /**
     * \brief Appends an item with an action.
     * \param text  Label.
     * \param fn    Action on click.
     * \param flags Combination of the \c kItem* flags.
     */
    void AddAction(std::wstring text, std::function<void(const Context&)> fn,
                   UINT32 flags = kItemNone) {
        Item it;
        it.text = std::move(text);
        it.flags = flags;
        it.action = std::move(fn);
        items_.push_back(std::move(it));
    }

    /// Appends a separator; duplicate separators and a leading one are suppressed.
    void AddSeparator() {
        if (items_.empty() || items_.back().separator()) return;
        Item it;
        it.flags = kItemSeparator;
        items_.push_back(std::move(it));
    }

    /// Removes separators at the end of the list.
    void TrimTrailingSeparator() {
        while (!items_.empty() && items_.back().separator()) items_.pop_back();
    }

    /// \return The collected items.
    const std::vector<Item>& items() const { return items_; }

    /// \return The collected items (mutable, e.g. to pass them on via \c std::move).
    std::vector<Item>& items() { return items_; }

    /// \return \c true if nothing has been collected yet.
    bool empty() const { return items_.empty(); }

private:
    std::vector<Item> items_;  ///< Collected items in display order.
};

/**
 * \brief A provider contributes items for a concrete target window.
 *
 * Providers are queried again on every open of the flyout and can therefore
 * make their items depend on the state of the target window (e.g. set a check
 * mark for \c WS_EX_TOPMOST).
 */
using Provider = std::function<void(const Context&, ItemList&)>;

/**
 * \brief Manages all providers.
 *
 * They are registered by \ref RegisterBuiltinProviders; the items themselves
 * come partly from hard-coded code and partly from the configuration file
 * (\ref ConfigStore).
 */
class Registry {
public:
    /// \return The process-wide instance.
    static Registry& Instance();

    /**
     * \brief Registers a provider.
     * \param name      Display name (for diagnostics only).
     * \param sortOrder Smaller values appear further up; built-ins use 0 and 10.
     * \param provider  The callback function.
     */
    void Register(std::wstring name, int sortOrder, Provider provider);

    /**
     * \brief Queries all providers and merges their items.
     *
     * A separator is inserted automatically between the contributions of two
     * providers.
     *
     * \param ctx Context of the target window.
     * \return The merged list.
     */
    ItemList Collect(const Context& ctx) const;

private:
    /// A registered provider along with its sort key.
    struct Entry {
        std::wstring name;      ///< Display name.
        int          sortOrder; ///< Sort key.
        Provider     provider;  ///< Callback function.
    };
    std::vector<Entry> entries_;  ///< Stably sorted by \c sortOrder.
};

/**
 * \brief Registers the built-in providers.
 *
 * Implemented in BuiltinProviders.cpp.
 */
void RegisterBuiltinProviders();

}  // namespace mfly
