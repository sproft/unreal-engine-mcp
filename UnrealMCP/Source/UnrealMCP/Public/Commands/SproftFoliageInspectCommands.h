#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: foliage_inspect (read-only)
 *
 * Read-only structured dump of every AInstancedFoliageActor in the
 * editor world. The hosted Flop tool surface promised "for each IFA:
 * foliage type list with mesh path, total instance count per type,
 * density radius, scale range, plus optional sample of N world
 * locations"; this clean-room implementation walks the public
 * AInstancedFoliageActor::GetFoliageInfos map and produces the same
 * shape so a caller can reason about a level's foliage budget without
 * round-tripping through Python.
 *
 * Operation: single op (`inspect`, default).
 *
 * Optional inputs:
 *   - `name_pattern`: case-insensitive substring filter on the IFA's
 *     actor name and outliner label.
 *   - `level_filter`: case-insensitive substring filter on the owning
 *     ULevel name.
 *   - `sample_locations`: include up to N per-foliage-type instance
 *     world-space locations (capped at instance count). Default 0.
 *   - `sample_seed`: seed for the sampling RNG when
 *     `sample_locations` > 0. Default 0 (deterministic).
 *
 * Returns a structured payload with:
 *   - `level_name` / `level_path` of the persistent level.
 *   - `foliage_actors`: array of per-IFA records. Each record reports
 *     `name`, `label`, `class`, `class_path`, `level`, `location`,
 *     `total_instance_count`, `foliage_type_count`, and a
 *     `foliage_types` array. Each foliage_type carries
 *     `foliage_type_name`, `foliage_type_path`,
 *     `foliage_type_class`, `source_path` (Static Mesh package or
 *     Actor class path), `source_kind` (`static_mesh` /
 *     `actor` / `unknown`), `density`, `density_adjustment_factor`,
 *     `radius`, `scale_x_min` / `scale_x_max` / `scale_y_min` /
 *     `scale_y_max` / `scale_z_min` / `scale_z_max`, `instance_count`,
 *     `placed_instance_count`, the optional `sample_locations` array
 *     (each sample is `{index, location: [x, y, z]}`), and an
 *     `approximated_bounds_min` / `approximated_bounds_max` /
 *     `approximated_bounds_size` triple when the IFA can answer
 *     `GetApproximatedInstanceBounds`.
 *
 * Read-only. We do not mutate the IFA and we do not save anything.
 *
 * Clean-room implementation derived from the public UE5 Foliage API:
 *   - AInstancedFoliageActor::GetFoliageInfos (the public read-only
 *     accessor that wraps the private FoliageInfos map).
 *   - UFoliageType::Density / DensityAdjustmentFactor / Radius /
 *     ScaleX / ScaleY / ScaleZ / GetSource.
 *   - UFoliageType_InstancedStaticMesh::Mesh and
 *     UFoliageType_Actor::ActorClass for the source type discriminator.
 *   - FFoliageInfo::Instances / GetPlacedInstanceCount /
 *     GetApproximatedInstanceBounds.
 *   - FFoliageInstancePlacementInfo::Location for the optional sample.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftFoliageInspectCommands
{
public:
    FSproftFoliageInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleFoliageInspect(const TSharedPtr<FJsonObject>& Params);
};
