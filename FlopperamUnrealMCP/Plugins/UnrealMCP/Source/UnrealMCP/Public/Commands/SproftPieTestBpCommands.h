#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: pie_test_bp (small variant)
 *
 * Blueprint-side assertion harness. Sits next to `pie_test_scene` (which
 * runs against actors in the active editor world) and lets a caller
 * verify properties on a Blueprint asset's CDO without needing a
 * running PIE session.
 *
 * Inputs:
 *   - blueprint:  short asset name or full `/Game/...` Blueprint path.
 *   - assertions: array of `{ kind, target, expected }` specs. The
 *                 only kind supported in this slice is
 *                 `default_value_equals`. `target` is a UPROPERTY
 *                 FName on the Blueprint's generated class (or its
 *                 parent class). `expected` is a JSON literal that we
 *                 canonicalize through the property's ImportText ->
 *                 ExportText round-trip and compare against the CDO's
 *                 ExportText output. Vector / rotator / transform /
 *                 FString / gameplay tag fields all flow through one
 *                 path because the canonicalisation happens on the
 *                 engine side.
 *
 * Per-assertion the response carries `index`, `kind`, `target`,
 * `passed` flag, optional `actual` / `expected` / `expected_raw` /
 * `property_class` / `expected_imported` for the relevant kinds, and
 * a human-readable `message`. Aggregate counts (`total`, `passed`,
 * `failed`, `unsupported`, `all_passed`) sit at the top of the
 * response.
 *
 * Skipped (need a running PIE session, deferred to BACKLOG):
 *   - `function_returns`: invoke a Blueprint function and assert on
 *     its return value.
 *   - `event_fired`: assert that a custom event was broadcast.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UEditorAssetLibrary::LoadAsset` for the Blueprint resolve.
 *   - `UBlueprint::GeneratedClass->GetDefaultObject()` for the CDO.
 *   - `FProperty::ImportText_Direct` / `ExportText_Direct` for the
 *     canonicalised compare.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftPieTestBpCommands
{
public:
    FSproftPieTestBpCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandlePieTestBp(const TSharedPtr<FJsonObject>& Params);
};
