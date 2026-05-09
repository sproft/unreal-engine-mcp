#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_nodes
 *
 * Batched node creation for a Blueprint graph in one call. Sits next to the
 * local repo's `add_blueprint_node` (single-node, immediate compile) and the
 * read-only `bp_graph` traversal helper.
 *
 * Operations (keyed by `op` plus a required `blueprint` resolver):
 *   - "add" / "add_nodes": spawn one or more K2 nodes in a chosen graph.
 *      The graph resolver mirrors `bp_graph`: we look across UbergraphPages,
 *      FunctionGraphs, MacroGraphs, and the implemented-interface graphs.
 *      Each node entry takes a `class` short name (variable_get,
 *      variable_set, call_function, branch / if_then_else, dynamic_cast,
 *      self, format_text, execution_sequence, knot, make_array,
 *      custom_event, event), an optional FName, an optional position, and
 *      a class-specific small parameter dict (variable_name, function /
 *      function_path, target_class, event_name etc.).
 *
 * The default behaviour is to NOT compile the Blueprint after the batch.
 * Callers stitch wires through `bp_wire` first and then run
 * `compile_blueprint` once. Pass `compile=true` to opt back in.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - K2 node header set under Editor/BlueprintGraph/Classes/.
 *   - UEdGraph::AddNode + K2Node lifecycle (NewObject, CreateNewGuid,
 *     PostPlacedNewNode, AllocateDefaultPins, ReconstructNode).
 *   - FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpNodesCommands
{
public:
    FSproftBpNodesCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpNodes(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> AddNodes(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
};
