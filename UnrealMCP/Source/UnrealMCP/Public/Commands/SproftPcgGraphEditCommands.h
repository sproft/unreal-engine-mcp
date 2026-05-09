#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: pcg_graph_edit (read + edit slice)
 *
 * Inspect or mutate a `UPCGGraph` asset. Pairs with `niagara_inspect`
 * for the procedural-side asset surface. Walks the graph's public node
 * list, the graph's input / output exposed pins, and emits a flat edge
 * list stitched from each pin's `Edges` array.
 *
 * Operations:
 *   - `inspect` (default): the read-only walk shipped on the earlier
 *     pass.
 *   - `add_node`: NewObject's a UPCGNode under the graph for a chosen
 *     UPCGSettings subclass, optionally renames it, optionally writes
 *     a 2D editor position. Routes through
 *     `UPCGGraph::AddNodeOfType<T>`.
 *   - `connect_pins`: creates an edge between two named nodes / named
 *     pins through `UPCGGraph::AddEdge`. Source-name / target-name
 *     resolution falls back to substring on the node FName + node
 *     title before failing.
 *   - `remove_node`: removes one named node from the graph through
 *     `UPCGGraph::RemoveNode`. Cascades any hanging edges.
 *
 * Each mutating op runs `MarkPackageDirty` and (when the optional
 * `save` flag stays at its default true) writes the asset to disk
 * through `UEditorAssetLibrary::SaveAsset`.
 *
 * Required input:
 *   - `graph`: short asset name or `/Game/...` UPCGGraph path.
 *
 * Op-specific inputs:
 *   - add_node: `settings_class` (short name or
 *     `/Script/Module.ClassName` path) + optional `node_name` /
 *     `position` (`{x, y}`).
 *   - connect_pins: `from_node` + `to_node` (FName / substring), plus
 *     optional `from_pin` / `to_pin` (FName, defaults to the node's
 *     first matching pin).
 *   - remove_node: `node` (FName / substring).
 *
 * Optional inputs (mutating ops):
 *   - `save`: default true.
 *
 * Read-only inspect inputs:
 *   - `include_pins` / `include_edges` / `max_nodes` / `max_edges` as
 *     before.
 *
 * Clean-room implementation derived from the public UE5 PCG API:
 *   - `UPCGGraph::AddNodeOfType` /
 *     `UPCGGraph::AddEdge` / `UPCGGraph::RemoveNode`.
 *   - `UPCGNode::SetNodePosition`.
 *   - `UPCGSettings` subclass resolution through `FindObject<UClass>`
 *     + `FindObject<UClass>` substring fallback over the loaded
 *     class set.
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
    TSharedPtr<FJsonObject> HandleAddNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectPins(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetNodeSettings(const TSharedPtr<FJsonObject>& Params);
};
