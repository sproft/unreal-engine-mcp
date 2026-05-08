#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: widget_inspect
 *
 * Read-only counterpart to widget_edit. Dumps a UWidgetBlueprint's tree
 * (hierarchy of widget names, classes, children) plus its Blueprint
 * variables. The existing read_blueprint_content tool returns an empty
 * components list for Widget Blueprints because UMG widgets do not live in
 * a SimpleConstructionScript; this fills that gap.
 *
 * One operation: "inspect" (alias "tree"). Returns:
 *   - widget_blueprint, name, parent_class
 *   - tree: nested tree starting at the root widget
 *   - widgets: flat list (name + class) of every UWidget in the tree
 *   - variables: NewVariables that are not also widget tree members
 *   - functions: a short list of function graph names
 *   - named_slots: any UNamedSlot widgets exposed by the asset
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UWidgetBlueprint, UWidgetTree, UPanelWidget child accessors
 *   - UEditorAssetLibrary::LoadAsset for path resolution
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftWidgetInspectCommands
{
public:
    FSproftWidgetInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleWidgetInspect(const TSharedPtr<FJsonObject>& Params);
};
