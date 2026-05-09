#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: metasound_edit (small + graph authoring slice)
 *
 * Multi-op tool keyed by `op`:
 *   - `create_metasound_source`: creates a new UMetaSoundSource at a
 *     `/Game/...` package path.
 *   - `create_metasound_patch`: creates a new UMetaSoundPatch.
 *   - `add_node`: append a node to a MetaSound asset's document
 *     through `UMetaSoundBuilderBase::AddNodeByClassName`. The node
 *     class is identified by `class_name` (e.g.
 *     `Audio.Add` / `Add` / `Mix`) and the canonical UE5 metasound
 *     namespace lookup. Returns the new node handle GUID.
 *   - `connect_nodes`: connect a named output of one node to a named
 *     input of another via
 *     `UMetaSoundBuilderBase::ConnectNodes(SourceNode, OutputName,
 *     DestinationNode, InputName)`.
 *
 * Both create ops route through
 * `UMetaSoundEditorSubsystem::GetChecked()`'s public `InitAsset` +
 * `RegisterGraphWithFrontend`. The graph-authoring ops route through
 * `UMetaSoundBuilderSubsystem::AttachBuilderToAssetChecked` to obtain
 * a `UMetaSoundBuilderBase` over an existing asset; the builder's
 * `AddNodeByClassName` / `ConnectNodes` are the BlueprintCallable
 * public APIs.
 *
 * Inputs (op-dependent):
 *   - path / asset:   target asset (create / graph ops). Required.
 *   - class_name:     `Namespace.Name[.Variant]` token for add_node.
 *                     Mirrors the editor's class palette names. Required.
 *   - major_version:  integer (default 1) major class version.
 *   - from_node / to_node: GUID strings (from add_node return) or a
 *                     case-insensitive substring match on the node's
 *                     class name. Required for connect_nodes.
 *   - from_output / to_input: FName strings for the pin names. Required.
 *   - overwrite:      reuse an existing asset at the path. Default false
 *                     (create only).
 *   - save:           save after the edit. Default true.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UMetaSoundSource` / `UMetaSoundPatch` from MetasoundEngine.
 *   - `UMetaSoundBuilderSubsystem::AttachBuilderToAssetChecked` for
 *     the existing-asset builder lookup.
 *   - `UMetaSoundBuilderBase::AddNodeByClassName` /
 *     `ConnectNodes(SourceNode, OutputName, DestinationNode, InputName)`
 *     for the graph mutations.
 *   - `UMetaSoundEditorSubsystem::InitAsset` /
 *     `RegisterGraphWithFrontend` from MetasoundEditor.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftMetaSoundEditCommands
{
public:
    FSproftMetaSoundEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleCreateSource(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreatePatch(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectNodes(const TSharedPtr<FJsonObject>& Params);
};
