#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <vector>

#include "../dsp/ChainOrder.h"

namespace tubamp
{
class TubampAudioProcessor;

/**
    The centrepiece: a horizontal, user-buildable signal path.

      IN ─[GATE]─[COMP]─[DRIVE]─[AMP]─ … ─[+]─ OUT

    - click a tile to select it (the parameter panel follows via onSelectionChanged)
    - click the power LED to bypass without selecting
    - double-click to bypass, right-click for the block menu
    - drag horizontally to reorder (siblings animate out of the way)
    - [+] appends a block that is not in the chain yet

    Order changes are published through `processor.setChainOrder()`; the view also
    subscribes to `processor.onChainChanged` so preset/state loads rebuild it. A guard
    flag stops our own publish from re-entering the rebuild.
*/
class SignalChainView : public juce::Component,
                        private juce::MultiTimer
{
public:
    explicit SignalChainView (TubampAudioProcessor&);
    ~SignalChainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Fires when the selected block changes; empty when the chain is empty. */
    std::function<void (std::optional<chain::BlockId>)> onSelectionChanged;

    std::optional<chain::BlockId> getSelection() const noexcept { return selection; }

    void selectBlock (chain::BlockId id);
    void removeBlock (chain::BlockId id);
    void toggleBypass (chain::BlockId id);

private:
    class Tile;
    class AddTile;

    void syncFromProcessor();
    void rebuildTiles();
    void reorderTilesTo (const chain::Order& desired);
    void layoutLane (bool animate);
    void refreshFromParams();
    void publishOrder();
    void ensureValidSelection();
    void updateTileStates();

    void showBlockMenu (chain::BlockId id, juce::Component* target);
    void showAddMenu();
    void appendBlock (chain::BlockId id);

    void tileDragged (Tile& tile, const juce::MouseEvent& e);
    void tileDragEnded (Tile& tile, const juce::MouseEvent& e);

    bool isBlockEnabled (chain::BlockId id) const;
    void setBlockEnabled (chain::BlockId id, bool shouldBeEnabled);
    bool chainContains (chain::BlockId id) const;

    void timerCallback (int timerID) override;
    void startAnimationTimer();

    TubampAudioProcessor& proc;

    chain::Order order;
    juce::OwnedArray<Tile> tiles;              // parallel to `order`
    std::unique_ptr<AddTile> addTile;
    std::optional<chain::BlockId> selection;

    std::vector<juce::Rectangle<int>> slots;   // target bounds, parallel to `tiles`
    juce::Rectangle<int> inNodeBounds, outNodeBounds, addTileBounds, laneBounds;
    int rowCentreY = 0, rowLabelY = 0;

    juce::ComponentAnimator animator;
    bool applyingOrder = false;                // re-entrancy guard for onChainChanged
    int dragIndex = -1;
    int dragGrabOffsetX = 0;
    chain::Order orderBeforeDrag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalChainView)
};
} // namespace tubamp
