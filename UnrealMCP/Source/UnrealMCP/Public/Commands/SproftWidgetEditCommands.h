#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: widget_edit
 *
 * A trimmed clone of the hosted Flop "widget_edit" surface. Three operations
 * are supported:
 *   - "create_widget_blueprint": create a new UWidgetBlueprint with a parent
 *      class and an optional root panel widget class.
 *   - "add_child_widget": construct a named widget (Vertical Box, Horizontal
 *      Box, Progress Bar, Text Block, Button, Image) and attach it as a child
 *      of an existing widget inside a Widget Blueprint's tree.
 *   - "set_slot_property": apply a flat property dict to the UPanelSlot of
 *      an existing widget. Covers UCanvasPanelSlot anchors / offsets / size,
 *      UVerticalBoxSlot / UHorizontalBoxSlot padding / fill, and any other
 *      UPanelSlot-derived class without us spelling out each property by
 *      name. Properties go through `FProperty::ImportText_InContainer`.
 *
 * The full hosted surface (animations, MVVM bindings, advanced styles, event
 * binding) is out of scope and tracked in BACKLOG.md.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FKismetEditorUtilities::CreateBlueprint for asset creation
 *   - UWidgetBlueprint / UWidgetBlueprintGeneratedClass for the asset class
 *   - UWidgetTree::ConstructWidget + UPanelWidget::AddChild for the tree
 *   - UWidget::Slot for the per-widget UPanelSlot pointer
 *   - FProperty::ImportText_InContainer for the property dict surface
 *   - FBlueprintEditorUtils::MarkBlueprintAsModified for change notification
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftWidgetEditCommands
{
public:
    FSproftWidgetEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleWidgetEdit(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddChildWidget(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetSlotProperty(const TSharedPtr<FJsonObject>& Params);
};
