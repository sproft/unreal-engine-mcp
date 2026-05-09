#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_brief
 *
 * Read-only one-page orientation summary of a Blueprint asset. Smaller and
 * faster than `read_blueprint_content` and `analyze_blueprint_graph`. Useful
 * when the agent just needs to know "what kind of BP is this" before a deeper
 * pass.
 *
 * One operation: "brief". Inputs:
 *   - blueprint: short name or absolute `/Game/` path.
 *
 * Returns:
 *   - blueprint name and path.
 *   - parent class short name and full path.
 *   - blueprint type (normal / interface / function library / etc.).
 *   - variable count.
 *   - function count (excluding the constructor / construction script).
 *   - macro count.
 *   - event-graph node count and a small named-event list.
 *   - components_summary: short list of "Variable: ClassShortName" pairs.
 *   - implemented Blueprint interface short names.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBlueprint, UEdGraph, UK2Node_Event, UK2Node_FunctionEntry.
 *   - UEditorAssetLibrary::LoadAsset for path-based access.
 *   - USimpleConstructionScript walk for components.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpBriefCommands
{
public:
    FSproftBpBriefCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpBrief(const TSharedPtr<FJsonObject>& Params);
};
