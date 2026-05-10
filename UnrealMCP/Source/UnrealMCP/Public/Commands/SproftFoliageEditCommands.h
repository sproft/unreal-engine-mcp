#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: foliage_edit (small variant)
 *
 * Three ops on AInstancedFoliageActor / UFoliageType, keyed by `op`:
 *
 *   - `add_foliage_type`: register a UFoliageType asset on the
 *     AInstancedFoliageActor for a chosen level. Resolves (or
 *     spawns) the IFA through
 *     `AInstancedFoliageActor::Get(World, bCreateIfNone=true,
 *     Level)` and binds the type through
 *     `AInstancedFoliageActor::AddFoliageType`. Reuses an existing
 *     FFoliageInfo when the type is already registered. Returns
 *     the resolved `actor_path` plus the binding result.
 *   - `set_foliage_density`: write `Density`,
 *     `DensityAdjustmentFactor`, `Radius`, and the per-axis
 *     `ScaleX` / `ScaleY` / `ScaleZ` FFloatInterval pairs on a
 *     UFoliageType asset. All fields are optional; only the ones
 *     present in the call are written. Saves the asset by default.
 *   - `place_foliage_instances`: place N foliage instances at the
 *     given world-space `[x, y, z]` locations. Resolves (or spawns)
 *     the IFA for the chosen level, registers the foliage type
 *     when missing, and then walks the locations array building
 *     one `FFoliageInstance` per row through
 *     `FFoliageInfo::AddInstance(InSettings, NewInstance)`. Each
 *     instance picks up the foliage type's per-axis ScaleX / Y / Z
 *     interval midpoint as its DrawScale3D when no per-row
 *     override is supplied. Optional `rotations` (parallel array
 *     of `[pitch, yaw, roll]` triples) applies a per-instance
 *     rotation; when omitted every instance lands at zero rotation.
 *     Optional `scales` (parallel array of `[x, y, z]` triples)
 *     overrides the DrawScale3D per row. The IFA's owning level
 *     is dirtied + saved on success unless `save=false`.
 *
 * Inputs (add_foliage_type):
 *   - foliage_type: short asset name or `/Game/...` UFoliageType
 *     path. Required.
 *   - level: optional substring on owning ULevel name. When omitted
 *     we use the persistent level of the editor world. When
 *     present and unique, the IFA is fetched / spawned in that
 *     sublevel; on no-match we fail fast.
 *   - save: persist the IFA actor's level on success. Default
 *     False (the IFA is a level actor, not an asset).
 *
 * Inputs (set_foliage_density):
 *   - foliage_type: short asset name or `/Game/...` UFoliageType
 *     path. Required.
 *   - density: float. Optional.
 *   - density_adjustment_factor: float. Optional.
 *   - radius: float. Optional.
 *   - scale_x_min / scale_x_max: float. Optional. Both must be
 *     present together to write the X interval.
 *   - scale_y_min / scale_y_max / scale_z_min / scale_z_max: same
 *     shape for Y / Z.
 *   - save: persist the asset on success. Default True.
 *
 * Inputs (place_foliage_instances):
 *   - foliage_type: short asset name or `/Game/...` UFoliageType
 *     path. Required.
 *   - level: optional substring on owning ULevel name. Defaults to
 *     the persistent level.
 *   - locations: required, non-empty array of `[x, y, z]` world-
 *     space location triples (or `{x, y, z}` objects). One
 *     instance is placed per entry.
 *   - rotations: optional, parallel array of `[pitch, yaw, roll]`
 *     triples. When shorter than `locations`, the missing rows
 *     fall back to zero rotation.
 *   - scales: optional, parallel array of `[x, y, z]` triples.
 *     When shorter than `locations`, the missing rows fall back to
 *     the foliage type's per-axis scale interval midpoint.
 *   - save: persist the IFA's owning level on success. Default
 *     True.
 *
 * Returns op-specific dicts. `add_foliage_type` reports the
 * resolved actor name + path + level + foliage type count + a
 * `created_actor` flag when the IFA was spawned for this call;
 * `set_foliage_density` reports the previous and new field values
 * for every field actually written.
 *
 * Clean-room implementation derived from the public UE5 Foliage API:
 *   - AInstancedFoliageActor::Get(World, bCreateIfNone, Level) for
 *     IFA resolve / spawn.
 *   - AInstancedFoliageActor::AddFoliageType(InType, OutInfo) for
 *     the type binding.
 *   - AInstancedFoliageActor::FindInfo for "is the type already
 *     bound" probe.
 *   - UFoliageType::Density / DensityAdjustmentFactor / Radius /
 *     ScaleX / ScaleY / ScaleZ direct UPROPERTY writes.
 *   - FFoliageInstance struct + FFoliageInfo::AddInstance for
 *     per-instance placement (FOLIAGE_API).
 *   - UEditorAssetLibrary::LoadAsset / SaveAsset for the asset
 *     resolve / persist cycle.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftFoliageEditCommands
{
public:
    FSproftFoliageEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAddFoliageType(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetFoliageDensity(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandlePlaceFoliageInstances(const TSharedPtr<FJsonObject>& Params);
};
