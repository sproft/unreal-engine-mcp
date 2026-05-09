#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_commit
 *
 * Convenience wrapper for the standard "I am done editing this Blueprint"
 * cycle: mark the asset structurally modified, run the Kismet compiler,
 * and save the package. Most of our `bp_*` mutating tools already compile
 * + save individually; this tool exists so a designer chaining
 * `bp_nodes` -> `bp_wire` -> `bp_commit` (with `compile=false` /
 * `save=false` on the intermediate calls) gets one consolidated outcome
 * with the compiler's error and warning lists separated for downstream
 * consumption.
 *
 * Operation (single op `commit`, default):
 *   - Optional `mark_structurally` (default true): drive
 *     `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified` so
 *     SCS / function signature changes propagate. When false, runs the
 *     lighter `MarkBlueprintAsModified` instead.
 *   - Optional `compile` (default true): run
 *     `FKismetEditorUtilities::CompileBlueprint` with a captured
 *     `FCompilerResultsLog`. Compiler errors / warnings / info messages
 *     are returned as separate string arrays so the caller can decide
 *     whether to retry.
 *   - Optional `save` (default true): write the asset out through
 *     `UEditorAssetLibrary::SaveAsset`. Skipped automatically when the
 *     compile fails so we do not pin a broken Blueprint to disk.
 *
 * Returns a structured payload with `compiled`, `saved`, `errors`,
 * `warnings`, `infos`, `error_count`, and `warning_count` fields. The
 * `success` of the response is true iff compile succeeded (or compile
 * was skipped).
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified`
 *   - `FBlueprintEditorUtils::MarkBlueprintAsModified`
 *   - `FKismetEditorUtilities::CompileBlueprint(BP, Flags, &ResultsLog)`
 *   - `FCompilerResultsLog` + `FTokenizedMessage::ToText`
 *   - `UEditorAssetLibrary::SaveAsset`
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpCommitCommands
{
public:
    FSproftBpCommitCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpCommit(const TSharedPtr<FJsonObject>& Params);
};
