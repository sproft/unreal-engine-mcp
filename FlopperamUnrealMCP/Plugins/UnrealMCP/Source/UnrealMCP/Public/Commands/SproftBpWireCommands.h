#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_wire
 *
 * Connect or disconnect named pins between named nodes inside a Blueprint
 * graph. Designed to chain after `bp_nodes` so a caller can author a small
 * graph in two calls (spawn the nodes, wire them) plus an explicit final
 * `compile_blueprint`.
 *
 * Operations (keyed by `op` plus a required `blueprint` resolver):
 *   - "connect" / "wire" (default): apply a list of {source_node, source_pin,
 *      dest_node, dest_pin, optional disconnect=true} entries to a chosen
 *      graph. Pin direction is validated (source must be Output, dest must
 *      be Input). Pin-category compatibility is validated through the K2
 *      schema's CanCreateConnection helper.
 *   - "disconnect": shortcut equivalent to passing `disconnect=true` on
 *      every entry.
 *
 * The entries can also pass a single object instead of a list when wiring
 * one edge.
 *
 * The default behaviour is to NOT compile the Blueprint after wiring.
 * Callers run `compile_blueprint` once at the end of their authoring batch.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UEdGraphPin::MakeLinkTo / BreakLinkTo / BreakAllPinLinks.
 *   - UEdGraphSchema_K2::CanCreateConnection for compatibility validation.
 *   - FBlueprintEditorUtils::MarkBlueprintAsModified.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpWireCommands
{
public:
    FSproftBpWireCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpWire(const TSharedPtr<FJsonObject>& Params);
};
