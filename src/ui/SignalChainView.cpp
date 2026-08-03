#include "SignalChainView.h"

#include "../PluginProcessor.h"
#include "BlockIcons.h"
#include "TubampLookAndFeel.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace tubamp
{
namespace
{
constexpr int squareSize = 60;   // the tile face
constexpr int tilePad    = 6;    // room around the face for the selection glow
constexpr int labelH     = 14;
constexpr int tileW      = squareSize + tilePad * 2;   // 72
constexpr int tileH      = tilePad + squareSize + 4 + labelH;  // 84
constexpr int addW       = 56;
constexpr int addSquare  = 44;
constexpr int nodeSize   = 34;
constexpr int laneGap    = 8;

constexpr int stateTimer = 0;
constexpr int animTimer  = 1;

/** The 60x60 face inside a tile slot. */
juce::Rectangle<int> faceForSlot (juce::Rectangle<int> slot)
{
    return { slot.getX() + tilePad, slot.getY() + tilePad, squareSize, squareSize };
}
} // namespace

//==============================================================================
class SignalChainView::Tile : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    Tile (SignalChainView& ownerIn, chain::BlockId idIn)
        : owner (ownerIn),
          blockId (idIn),
          icon (icons::makeIcon (idIn)),
          accentColour (TubampLookAndFeel::blockAccent (idIn))
    {
        setTooltip (juce::String (chain::infoFor (idIn).displayName)
                    + " - drag to reorder, double-click to bypass");
    }

    chain::BlockId getBlockId() const noexcept { return blockId; }

    void setSelected (bool shouldBeSelected)
    {
        if (selected == shouldBeSelected)
            return;

        selected = shouldBeSelected;
        repaint();
    }

    void setBlockEnabled (bool shouldBeEnabled)
    {
        if (enabled == shouldBeEnabled)
            return;

        enabled = shouldBeEnabled;
        repaint();
    }

    void setDragging (bool shouldBeDragging)
    {
        if (dragging == shouldBeDragging)
            return;

        dragging = shouldBeDragging;
        repaint();
    }

    juce::Rectangle<int> getFaceBounds() const
    {
        return getLocalBounds().reduced (tilePad).removeFromTop (squareSize);
    }

    void paint (juce::Graphics& g) override
    {
        const auto face = getFaceBounds().toFloat();
        const auto contentAlpha = enabled ? 1.0f : 0.45f;

        if (dragging)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (face.translated (0.0f, 3.0f).expanded (1.0f), 13.0f);
        }

        if (selected)
        {
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (accentColour.withAlpha (0.24f / (float) i));
                g.drawRoundedRectangle (face.expanded ((float) i * 1.6f),
                                        12.0f + (float) i * 1.6f, 2.0f);
            }
        }

        g.setColour (selected || hovered ? TubampLookAndFeel::panelHi : TubampLookAndFeel::panel);
        g.fillRoundedRectangle (face, 12.0f);

        if (selected)
        {
            g.setColour (accentColour);
            g.drawRoundedRectangle (face.reduced (1.0f), 11.0f, 2.0f);
        }
        else
        {
            g.setColour (hovered ? TubampLookAndFeel::strokeHi : TubampLookAndFeel::stroke);
            g.drawRoundedRectangle (face.reduced (0.5f), 12.0f, 1.0f);
        }

        g.setColour ((enabled ? accentColour : TubampLookAndFeel::textFaint)
                         .withMultipliedAlpha (contentAlpha));
        icons::drawUnitPath (g, icon, face.reduced (17.0f), 2.0f);

        if (! enabled)
        {
            g.setColour (TubampLookAndFeel::strokeHi.withAlpha (0.55f));
            g.drawLine (face.getX() + 13.0f, face.getBottom() - 13.0f,
                        face.getRight() - 13.0f, face.getY() + 13.0f, 1.5f);
        }

        // Power LED (its own hit zone: toggles without selecting).
        const auto led = juce::Rectangle<float> (6.0f, 6.0f)
                             .withCentre (getLedBounds().toFloat().getCentre());
        if (enabled)
        {
            g.setColour (accentColour.withAlpha (0.28f));
            g.fillEllipse (led.expanded (3.0f));
            g.setColour (accentColour);
            g.fillEllipse (led);
        }
        else
        {
            g.setColour (TubampLookAndFeel::bg);
            g.fillEllipse (led);
            g.setColour (TubampLookAndFeel::strokeHi);
            g.drawEllipse (led.reduced (0.5f), 1.0f);
        }

        g.setColour (selected ? accentColour
                              : (enabled ? TubampLookAndFeel::textDim : TubampLookAndFeel::textFaint));
        g.setFont (TubampLookAndFeel::kernedFont (10.0f, true));
        g.drawText (chain::infoFor (blockId).shortName,
                    getLocalBounds().removeFromBottom (labelH),
                    juce::Justification::centred, false);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        suppressGesture = false;

        if (getLedBounds().contains (e.getPosition()))
        {
            suppressGesture = true;
            owner.toggleBypass (blockId);
            return;
        }

        if (e.mods.isPopupMenu())
        {
            suppressGesture = true;
            owner.showBlockMenu (blockId, this);
            return;
        }

        owner.selectBlock (blockId);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! suppressGesture)
            owner.tileDragged (*this, e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! suppressGesture)
            owner.tileDragEnded (*this, e);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! suppressGesture)
            owner.toggleBypass (blockId);
    }

private:
    juce::Rectangle<int> getLedBounds() const
    {
        const auto face = getFaceBounds();
        return { face.getRight() - 19, face.getY() + 3, 16, 16 };
    }

    SignalChainView& owner;
    chain::BlockId blockId;
    juce::Path icon;
    juce::Colour accentColour;
    bool selected = false, enabled = true, hovered = false, dragging = false, suppressGesture = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Tile)
};

//==============================================================================
class SignalChainView::AddTile : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    explicit AddTile (SignalChainView& ownerIn) : owner (ownerIn)
    {
        setTooltip ("Add a block to the chain");
    }

    void paint (juce::Graphics& g) override
    {
        auto face = getLocalBounds().withSizeKeepingCentre (addSquare, addSquare)
                        .withY (tilePad + (squareSize - addSquare) / 2)
                        .toFloat();

        juce::Path outline;
        outline.addRoundedRectangle (face.reduced (0.5f), 11.0f);

        juce::Path dashed;
        const float dashLengths[] = { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashLengths, 2);

        if (hovered)
        {
            g.setColour (TubampLookAndFeel::panel);
            g.fillRoundedRectangle (face, 11.0f);
        }

        g.setColour (hovered ? TubampLookAndFeel::strokeHi : TubampLookAndFeel::stroke);
        g.fillPath (dashed);

        g.setColour (hovered ? TubampLookAndFeel::text : TubampLookAndFeel::textDim);
        icons::drawUnitPath (g, icons::makePlusGlyph(), face.reduced (13.0f), 2.0f);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()))
            owner.showAddMenu();
    }

private:
    SignalChainView& owner;
    bool hovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddTile)
};

//==============================================================================
SignalChainView::SignalChainView (TubampAudioProcessor& processor) : proc (processor)
{
    addTile = std::make_unique<AddTile> (*this);
    addChildComponent (addTile.get());

    order = proc.getChainOrder();
    rebuildTiles();
    ensureValidSelection();

    // Preset/state loads publish a new order from the processor; rebuild when they do.
    proc.onChainChanged = [this]
    {
        if (! applyingOrder)
            syncFromProcessor();
    };

    startTimer (stateTimer, 100);
}

SignalChainView::~SignalChainView()
{
    proc.onChainChanged = nullptr;
    animator.cancelAllAnimations (false);
}

//==============================================================================
void SignalChainView::syncFromProcessor()
{
    order = proc.getChainOrder();
    rebuildTiles();   // lays the lane out as part of the rebuild
    ensureValidSelection();
}

void SignalChainView::rebuildTiles()
{
    dragIndex = -1;
    animator.cancelAllAnimations (false);
    tiles.clear();

    for (auto id : order)
    {
        auto* tile = tiles.add (new Tile (*this, id));
        addAndMakeVisible (tile);
    }

    addTile->setVisible ((int) order.size() < chain::numBlockTypes);
    updateTileStates();
    layoutLane (false);
}

void SignalChainView::reorderTilesTo (const chain::Order& desired)
{
    for (int target = 0; target < (int) desired.size() && target < tiles.size(); ++target)
    {
        for (int i = target; i < tiles.size(); ++i)
        {
            if (tiles[i]->getBlockId() == desired[(size_t) target])
            {
                if (i != target)
                    tiles.move (i, target);

                break;
            }
        }
    }
}

void SignalChainView::updateTileStates()
{
    for (auto* tile : tiles)
    {
        tile->setBlockEnabled (isBlockEnabled (tile->getBlockId()));
        tile->setSelected (selection.has_value() && *selection == tile->getBlockId());
    }
}

void SignalChainView::resized()
{
    layoutLane (false);
}

void SignalChainView::layoutLane (bool animate)
{
    laneBounds = getLocalBounds().reduced (16, 0);

    const int n = tiles.size();
    const bool showAdd = n < chain::numBlockTypes;
    const int elementCount = 2 + n + (showAdd ? 1 : 0);

    int total = nodeSize * 2 + n * tileW + (showAdd ? addW : 0) + laneGap * (elementCount - 1);

    const int top = laneBounds.getCentreY() - tileH / 2;
    rowCentreY = top + tilePad + squareSize / 2;
    rowLabelY = top + tileH - labelH;

    int x = laneBounds.getCentreX() - total / 2;
    inNodeBounds = { x, rowCentreY - nodeSize / 2, nodeSize, nodeSize };
    x += nodeSize + laneGap;

    slots.clear();
    slots.reserve ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        slots.push_back ({ x, top, tileW, tileH });
        x += tileW + laneGap;
    }

    addTileBounds = { x, top, addW, tileH };

    if (showAdd)
        x += addW + laneGap;

    outNodeBounds = { x, rowCentreY - nodeSize / 2, nodeSize, nodeSize };

    for (int i = 0; i < n; ++i)
    {
        if (i == dragIndex)
            continue;

        const auto target = slots[(size_t) i];

        if (animate)
        {
            animator.animateComponent (tiles[i], target, 1.0f, 120, false, 1.0, 0.0);
        }
        else
        {
            animator.cancelAnimation (tiles[i], false);
            tiles[i]->setBounds (target);
        }
    }

    if (showAdd)
        addTile->setBounds (addTileBounds);

    if (animate)
        startAnimationTimer();

    repaint();
}

//==============================================================================
void SignalChainView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    g.setColour (TubampLookAndFeel::bgElevated);
    g.fillRect (bounds);
    g.setColour (TubampLookAndFeel::stroke);
    g.fillRect (bounds.removeFromBottom (1));

    // --- connector: IN -> tiles -> [+] -> OUT ---------------------------------
    struct Element
    {
        juce::Rectangle<int> face;
        bool isSelected = false;
    };

    std::vector<Element> elements;
    elements.reserve (slots.size() + 3);
    elements.push_back ({ inNodeBounds, false });

    for (size_t i = 0; i < slots.size() && i < order.size(); ++i)
        elements.push_back ({ faceForSlot (slots[i]),
                              selection.has_value() && *selection == order[i] });

    if (addTile->isVisible())
        elements.push_back ({ addTileBounds.withSizeKeepingCentre (addSquare, addSquare)
                                  .withY (addTileBounds.getY() + tilePad + (squareSize - addSquare) / 2),
                              false });

    elements.push_back ({ outNodeBounds, false });

    for (size_t i = 0; i + 1 < elements.size(); ++i)
    {
        const auto from = (float) elements[i].face.getRight();
        const auto to = (float) elements[i + 1].face.getX();

        if (to <= from)
            continue;

        const auto accentSegment = elements[i + 1].isSelected;
        g.setColour (accentSegment && selection.has_value()
                         ? TubampLookAndFeel::blockAccent (*selection).withAlpha (0.85f)
                         : TubampLookAndFeel::strokeHi);
        g.drawLine (from, (float) rowCentreY, to, (float) rowCentreY, 2.0f);

        if (to - from >= 14.0f)
        {
            const auto arrow = juce::Rectangle<float> (7.0f, 7.0f)
                                   .withCentre ({ (from + to) * 0.5f, (float) rowCentreY });
            g.setColour (TubampLookAndFeel::textFaint);
            icons::drawUnitPath (g, icons::makeFlowArrow(), arrow, 1.4f);
        }
    }

    // --- IN / OUT anchor nodes -----------------------------------------------
    auto drawNode = [&g, this] (juce::Rectangle<int> nodeBounds, const juce::Path& glyph,
                                const juce::String& caption)
    {
        const auto face = nodeBounds.toFloat();
        g.setColour (TubampLookAndFeel::panel);
        g.fillRoundedRectangle (face, 8.0f);
        g.setColour (TubampLookAndFeel::stroke);
        g.drawRoundedRectangle (face.reduced (0.5f), 8.0f, 1.0f);

        g.setColour (TubampLookAndFeel::textDim);
        icons::drawUnitPath (g, glyph, face.reduced (9.0f), 1.6f);

        g.setColour (TubampLookAndFeel::textFaint);
        g.setFont (TubampLookAndFeel::kernedFont (9.0f, true));
        // Sits on the same baseline as the tile labels.
        g.drawText (caption, nodeBounds.withY (rowLabelY).withHeight (labelH).expanded (10, 0),
                    juce::Justification::centred, false);
    };

    drawNode (inNodeBounds, icons::makeInputIcon(), "IN");
    drawNode (outNodeBounds, icons::makeOutputIcon(), "OUT");
}

//==============================================================================
bool SignalChainView::chainContains (chain::BlockId id) const
{
    return std::find (order.begin(), order.end(), id) != order.end();
}

bool SignalChainView::isBlockEnabled (chain::BlockId id) const
{
    if (auto* value = proc.apvts.getRawParameterValue (chain::infoFor (id).enableParamId))
        return value->load() > 0.5f;

    return true;
}

void SignalChainView::setBlockEnabled (chain::BlockId id, bool shouldBeEnabled)
{
    if (auto* param = proc.apvts.getParameter (chain::infoFor (id).enableParamId))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (shouldBeEnabled ? 1.0f : 0.0f);
        param->endChangeGesture();
    }
}

void SignalChainView::toggleBypass (chain::BlockId id)
{
    setBlockEnabled (id, ! isBlockEnabled (id));
    updateTileStates();
}

void SignalChainView::selectBlock (chain::BlockId id)
{
    if (selection.has_value() && *selection == id)
        return;

    selection = id;
    updateTileStates();
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged (selection);
}

void SignalChainView::ensureValidSelection()
{
    auto next = selection;

    if (! next.has_value() || ! chainContains (*next))
    {
        if (chainContains (chain::BlockId::amp))
            next = chain::BlockId::amp;
        else if (! order.empty())
            next = order.front();
        else
            next.reset();
    }

    if (next == selection)
    {
        updateTileStates();
        return;
    }

    selection = next;
    updateTileStates();
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged (selection);
}

void SignalChainView::publishOrder()
{
    // setChainOrder() calls back into onChainChanged synchronously; we already hold
    // the authoritative order, so ignore that round trip.
    const juce::ScopedValueSetter<bool> guard (applyingOrder, true);
    proc.setChainOrder (order);
}

void SignalChainView::removeBlock (chain::BlockId id)
{
    const auto it = std::find (order.begin(), order.end(), id);

    if (it == order.end())
        return;

    order.erase (it);
    publishOrder();
    rebuildTiles();
    ensureValidSelection();
}

void SignalChainView::appendBlock (chain::BlockId id)
{
    if (chainContains (id) || (int) order.size() >= chain::numBlockTypes)
        return;

    order.push_back (id);
    publishOrder();
    rebuildTiles();
    selectBlock (id);
}

//==============================================================================
void SignalChainView::showBlockMenu (chain::BlockId id, juce::Component* target)
{
    const auto enabled = isBlockEnabled (id);

    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addSectionHeader (chain::infoFor (id).displayName);
    menu.addItem (1, enabled ? "Bypass" : "Enable");
    menu.addSeparator();
    menu.addItem (2, "Remove from chain");

    juce::Component::SafePointer<SignalChainView> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMinimumWidth (180),
        [safeThis, id] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)
                safeThis->toggleBypass (id);
            else if (result == 2)
                safeThis->removeBlock (id);
        });
}

void SignalChainView::showAddMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());

    for (const auto& info : chain::blockInfos)
    {
        if (chainContains (info.id))
            continue;

        juce::PopupMenu::Item item (info.displayName);
        item.itemID = (int) info.id + 1;
        item.colour = TubampLookAndFeel::blockAccent (info.id);
        menu.addItem (item);
    }

    juce::Component::SafePointer<SignalChainView> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addTile.get()).withMinimumWidth (180),
        [safeThis] (int result)
        {
            if (safeThis == nullptr || result <= 0 || result > chain::numBlockTypes)
                return;

            safeThis->appendBlock ((chain::BlockId) (result - 1));
        });
}

//==============================================================================
void SignalChainView::tileDragged (Tile& tile, const juce::MouseEvent& e)
{
    if (dragIndex < 0)
    {
        if (std::abs (e.getDistanceFromDragStartX()) < 5)
            return;

        const auto index = tiles.indexOf (&tile);

        if (index < 0)
            return;

        dragIndex = index;
        dragGrabOffsetX = e.getMouseDownX();
        orderBeforeDrag = order;
        tile.setDragging (true);
        tile.toFront (false);
        startAnimationTimer();
    }

    const auto relative = e.getEventRelativeTo (this);
    const int newX = juce::jlimit (laneBounds.getX(), juce::jmax (laneBounds.getX(), laneBounds.getRight() - tileW),
                                   relative.x - dragGrabOffsetX);

    animator.cancelAnimation (&tile, false);
    tile.setTopLeftPosition (newX, (dragIndex < (int) slots.size() ? slots[(size_t) dragIndex].getY()
                                                                  : tile.getY()) - 3);

    // Nearest slot wins: uniform spacing makes this the intuitive drop target.
    const auto centreX = newX + tileW / 2;
    int nearest = dragIndex;
    int bestDistance = std::numeric_limits<int>::max();

    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto distance = std::abs (slots[(size_t) i].getCentreX() - centreX);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            nearest = i;
        }
    }

    nearest = juce::jlimit (0, juce::jmax (0, tiles.size() - 1), nearest);

    if (nearest != dragIndex)
    {
        const auto moved = order[(size_t) dragIndex];
        order.erase (order.begin() + dragIndex);
        order.insert (order.begin() + nearest, moved);
        tiles.move (dragIndex, nearest);
        dragIndex = nearest;
        layoutLane (true);
    }
    else
    {
        repaint();
    }
}

void SignalChainView::tileDragEnded (Tile& tile, const juce::MouseEvent& e)
{
    if (dragIndex < 0)
        return;

    // Letting go well outside the lane cancels the whole reorder.
    const auto relative = e.getEventRelativeTo (this);
    const bool cancelled = ! getLocalBounds().expanded (24, 48).contains (relative.getPosition());

    tile.setDragging (false);
    dragIndex = -1;

    if (cancelled)
    {
        order = orderBeforeDrag;
        reorderTilesTo (order);
    }
    else if (order != orderBeforeDrag)
    {
        publishOrder();
    }

    layoutLane (true);
}

//==============================================================================
void SignalChainView::refreshFromParams()
{
    for (auto* tile : tiles)
        tile->setBlockEnabled (isBlockEnabled (tile->getBlockId()));
}

void SignalChainView::startAnimationTimer()
{
    if (! isTimerRunning (animTimer))
        startTimer (animTimer, 16);
}

void SignalChainView::timerCallback (int timerID)
{
    if (timerID == stateTimer)
    {
        refreshFromParams();
        return;
    }

    repaint();

    if (! animator.isAnimating() && dragIndex < 0)
        stopTimer (animTimer);
}
} // namespace tubamp
