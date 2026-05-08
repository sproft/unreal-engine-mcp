#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_inspect
 *
 * Read-only targeted query operations on a Blueprint asset. Sits between
 * `bp_brief` (one-shot orientation summary) and `read_blueprint_content`
 * (full dump). Each operation returns a focused payload so the agent can
 * answer specific questions without authoring Python every time.
 *
 * Operations (all keyed by `op` plus a required `blueprint` resolver):
 *   - "list_variables": typed Blueprint variables, with default value,
 *     editability, and category.
 *   - "list_functions": user-authored function graphs with node count and
 *     graph kind.
 *   - "list_events": event-graph events (UK2Node_Event +
 *     UK2Node_CustomEvent) with the owning graph name.
 *   - "list_components": SCS components with class, attach parent, root
 *     flag, and short attached-children count.
 *   - "find_node": node search across all graphs by case-insensitive
 *     substring against either node short class name (`class_pattern`) or
 *     node title (`title_pattern`). Returns owning graph name, node class,
 *     and node title.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBlueprint::NewVariables / FunctionGraphs / UbergraphPages.
 *   - UEdGraph::Nodes, UK2Node_Event / UK2Node_CustomEvent,
 *     UEdGraphNode::GetNodeTitle.
 *   - USCS_Node walk for components.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpInspectCommands
{
public:
    FSproftBpInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpInspect(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> ListVariables(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> ListFunctions(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> ListEvents(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> ListComponents(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> FindNode(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
};
