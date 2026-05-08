#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_graph
 *
 * Read-only graph traversal beyond `bp_inspect`. Useful when the agent
 * needs to debug node wiring without running `analyze_blueprint_graph`
 * over the whole asset.
 *
 * Operations (keyed by `op` plus a required `blueprint` resolver):
 *   - "list_graphs": every graph on the Blueprint, grouped by kind:
 *     event graphs (UbergraphPages), function graphs (FunctionGraphs),
 *     macro graphs (MacroGraphs), interface override graphs from
 *     `ImplementedInterfaces[*].Graphs`, and the construction script
 *     reference. Each entry returns name, graph kind, owning class, and
 *     node count.
 *   - "list_nodes": every node in a chosen graph (resolved by `graph`
 *     name). Returns node short class, full node title, node FName,
 *     position, and pin count. Optional `include_class_pattern` /
 *     `include_title_pattern` filters narrow the result.
 *   - "get_node": full pin readback for one node, resolved by FName
 *     against the chosen graph. Returns each pin's name, direction,
 *     pin type description, default value, and connected pins
 *     (target node FName + target pin name + target node title).
 *   - "list_connections": flat edge list for a chosen graph. Each edge
 *     records source/target node FName + node title + pin name and a
 *     boolean for whether the edge is on an exec or data pin.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBlueprint::UbergraphPages / FunctionGraphs / MacroGraphs /
 *     ImplementedInterfaces[*].Graphs.
 *   - UEdGraph::Nodes, UEdGraphNode::Pins, UEdGraphPin::LinkedTo,
 *     UEdGraphSchema_K2::PC_Exec.
 *   - UEdGraphNode::GetNodeTitle.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpGraphCommands
{
public:
    FSproftBpGraphCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpGraph(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> ListGraphs(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> ListNodes(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> GetNode(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ListConnections(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
};
