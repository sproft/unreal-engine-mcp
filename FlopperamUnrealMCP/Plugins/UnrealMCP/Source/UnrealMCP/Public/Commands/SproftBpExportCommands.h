#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_export
 *
 * Canonical Blueprint-to-JSON snapshot. Returns a single GraphSpec-style
 * payload covering every event / function / macro / interface graph as a
 * node list with pin defaults plus an edge list, every SCS component
 * with class + relative transform + flat property dump, every variable
 * with type + default, the parent class, and the implemented interface
 * list. Read-only and diff-able. Useful when the agent wants to verify
 * that an MCP-driven authoring session left a Blueprint in the expected
 * state.
 *
 * Inputs (`op` is implicit; the tool only has one operation):
 *   - blueprint:           short asset name or full `/Game/...` path.
 *   - include_events:      include event-graph (UbergraphPages) graphs.
 *                          Default true.
 *   - include_functions:   include FunctionGraphs. Default true.
 *   - include_macros:      include MacroGraphs. Default true.
 *   - include_components:  include the SimpleConstructionScript dump.
 *                          Default true.
 *   - include_variables:   include the Blueprint's NewVariables list.
 *                          Default true.
 *   - include_interfaces:  include the implemented-interfaces list and
 *                          their override graphs. Default true.
 *   - include_defaults:    include each component's flat property dump.
 *                          Default true.
 *   - max_pins_per_node:   per-node pin cap. Default 64.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBlueprint::UbergraphPages / FunctionGraphs / MacroGraphs /
 *     ImplementedInterfaces[*].Graphs / NewVariables /
 *     SimpleConstructionScript / ParentClass.
 *   - UEdGraph::Nodes, UEdGraphNode::Pins, UEdGraphPin::LinkedTo,
 *     UEdGraphSchema_K2::PC_Exec.
 *   - USCS_Node::ComponentTemplate / GetChildNodes / GetVariableName.
 *   - FProperty::ExportText_InContainer for the per-component default
 *     dump.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpExportCommands
{
public:
    FSproftBpExportCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpExport(const TSharedPtr<FJsonObject>& Params);
};
