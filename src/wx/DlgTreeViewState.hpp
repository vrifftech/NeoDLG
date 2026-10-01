#pragma once

#include "NeoTreeState.hpp"

#include <algorithm>
#include <wx/treectrl.h>

namespace neodlggui {

// NeoDLG-local extension: do not change the restore contract of other tools.
struct DlgTreeViewState : neotree::TreeViewState {
    int horizontalPosition = 0;
    int firstVisibleOffset = 0;

    void reset() {
        neotree::TreeViewState::reset();
        horizontalPosition = 0;
        firstVisibleOffset = 0;
    }
};

template <typename KeyForItem>
void captureDlgTreeState(wxTreeCtrl& tree, DlgTreeViewState& state,
                         KeyForItem&& keyForItem) {
    neotree::captureTreeViewState(tree, state, keyForItem);
    wxRect bounds;
    const auto first = tree.GetFirstVisibleItem();
    state.firstVisibleOffset = first.IsOk() && tree.GetBoundingRect(first, bounds)
        ? bounds.y : 0;
#if (!defined(__WXMSW__) && !defined(__WXQT__)) || defined(__WXUNIVERSAL__)
    // wx's generic tree is a scrolled window. Native Windows/Qt trees
    // use an item-based anchor instead of these generic scroll units.
    int vertical = 0;
    tree.GetViewStart(&state.horizontalPosition, &vertical);
#endif
}

// Call only AFTER the update locker / Freeze scope ends. Generic wx trees
// deliberately skip their dirty layout processing while frozen; scrolling
// then uses uncalculated item positions (usually zero).
template <typename ResolveItem>
bool restoreDlgTreeViewport(wxTreeCtrl& tree, const DlgTreeViewState& state,
                            ResolveItem&& resolveItem) {
    if (!state.initialized || state.firstVisibleKey.empty()) return false;
    const auto first = resolveItem(state.firstVisibleKey);
    if (!first.IsOk()) return false;
    tree.ScrollTo(first);
#if (!defined(__WXMSW__) && !defined(__WXQT__)) || defined(__WXUNIVERSAL__)
    // Generic ScrollTo merely brings a row into view, often at the bottom.
    // Restore the saved top-row offset and horizontal position as well.
    int x = 0, y = 0, xUnit = 0, yUnit = 0;
    tree.GetViewStart(&x, &y);
    tree.GetScrollPixelsPerUnit(&xUnit, &yUnit);
    wxRect bounds;
    if (yUnit > 0 && tree.GetBoundingRect(first, bounds))
        y += (bounds.y - state.firstVisibleOffset) / yUnit;
    tree.Scroll(state.horizontalPosition, std::max(0, y));
#endif
    return true;
}

} // namespace neodlggui
