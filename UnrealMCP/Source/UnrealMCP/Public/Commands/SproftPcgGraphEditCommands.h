#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: pcg_graph_edit (read-only first slice)
 *
 * Inspect a `UPCGGraph` asset. Pairs with `niagara_inspect` for the
 * procedural-side asset surface. Walks the graph's public node list,
 * the graph's input / output exposed pins, and emits a flat edge list
 * stitched from each pin's `Edges` array.
 *
 * Operation: single op (`inspect`, default). Edit-side ops (add /
 * remove node, add / remove edge, rename pin) stay on the BACKLOG.
 *
 * Required input:
 *   - `graph`: short asset name or `/Game/...` UPCGGraph path.
 *
 * Optional inputs:
 *   - `include_pins`:  default true. When false the per-node `inputs`
 *                      / `outputs` arrays are omitted and only the
 *                      pin counts ride along.
 *   - `include_edges`: default true. When false the top-level `edges`
 *                      array is omitted.
 *   - `max_nodes`:     cap on the node walk. Default 1024.
 *   - `max_edges`:     cap on the edge walk. Default 4096.
 *
 * Returns a structured payload:
 *   - `name`, `path`, `class`.
 *   - `node_count`, `edge_count`, `node_count_total`,
 *     `edge_count_total`, `nodes_truncated`, `edges_truncated`.
 *   - `nodes`: per-node dict with `index`, `name`, `title`,
 *     `settings_class`, `settings_class_path`, `position` (`{x, y}`),
 *     `input_pin_count`, `output_pin_count`, plus the `inputs` /
 *     `outputs` arrays (each with `index`, `label`, `type`,
 *     `usage`, `status`, `multiple_data`, `multiple_connections`,
 *     `edge_count`).
 *   - `graph_inputs` / `graph_outputs`: pin descriptors for the
 *     graph's exposed input / output node, same shape as a node's
 *     pin record.
 *   - `edges`: flat array, each row `{from_node, from_pin, to_node,
 *     to_pin}`. Edge direction is upstream -> downstream
 *     (UPCGEdge::InputPin is the upstream side, UPCGEdge::OutputPin
 *     is the downstream side; we surface this as `from` /
 *     `to` so a downstream consumer does not have to remember PCG's
 *     reversed pin labels).
 *
 * Read-only. We do not mutate the asset and we do not save anything.
 *
 * Clean-room implementation derived from the public UE5 PCG API:
 *   - `UPCGGraph::GetNodes` for the per-node walk.
 *   - `UPCGGraph::GetInputNode` / `GetOutputNode` for the exposed
 *     pin surface.
 *   - `UPCGNode::GetInputPins` / `GetOutputPins` /
 *     `GetNodeTitle(EPCGNodeTitleType::ListView)` /
 *     `GetSettings()` / `GetNodePosition` for per-node fields.
 *   - `UPCGPin::Edges` plus `UPCGEdge::InputPin` / `OutputPin` for
 *     the edge walk.
 *   - `FPCGPinProperties::Label` / `AllowedTypes` /
 *     `bAllowMultipleData` / `bAllowMultipleConnections` /
 *     `PinStatus` / `Usage` for the pin descriptors.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftPcgGraphEditCommands
{
public:
    FSproftPcgGraphEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandlePcgGraphInspect(const TSharedPtr<FJsonObject>& Params);
};
