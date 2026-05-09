#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: metasound_edit (small variant)
 *
 * Two ops, keyed by `op`:
 *   - `create_metasound_source`: creates a new UMetaSoundSource at a
 *     `/Game/...` package path. Optional `output_format` token
 *     (`mono` / `stereo` / `quad` / `5_1` / `7_1`) plus optional
 *     `sample_rate` and `block_rate` overrides land on the asset's
 *     OutputFormat / SampleRateOverride / BlockRateOverride
 *     (per-platform) before InitAsset wires the document.
 *   - `create_metasound_patch`: creates a new UMetaSoundPatch (a
 *     reusable graph asset, no audio output) at a `/Game/...` path.
 *
 * Both ops route through `UMetaSoundEditorSubsystem::GetChecked()`'s
 * public `InitAsset` + `RegisterGraphWithFrontend` so the new asset
 * has a fresh document plus an editor graph that opens cleanly in
 * the MetaSound editor.
 *
 * The graph-authoring surface (add nodes, connect pins, set member
 * defaults) stays on the BACKLOG; that surface lives behind the
 * UMetaSoundBuilder API which has its own learning curve.
 *
 * Inputs (op-dependent):
 *   - path:           target /Game/... package path. Required.
 *   - output_format:  one of `mono` / `stereo` / `quad` / `5_1` /
 *                     `7_1`. Default `stereo`. (create_source only.)
 *   - sample_rate:    integer, in Hz. 0 keeps the device default.
 *                     (create_source only.)
 *   - block_rate:     float, in Hz. 0 keeps the device default.
 *                     (create_source only.)
 *   - overwrite:      reuse an existing asset at the path.
 *   - save:           save after the edit. Default true.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UMetaSoundSource` / `UMetaSoundPatch` from MetasoundEngine.
 *   - `UMetaSoundEditorSubsystem::InitAsset` /
 *     `RegisterGraphWithFrontend` from MetasoundEditor.
 *   - `EMetaSoundOutputAudioFormat` from
 *     `MetasoundOutputFormatInterfaces.h`.
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
};
