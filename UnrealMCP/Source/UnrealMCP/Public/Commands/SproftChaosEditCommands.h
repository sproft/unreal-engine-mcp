#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: chaos_edit (read-only first slice)
 *
 * Inspect a `UGeometryCollection` asset. Geometry Collections drive
 * Chaos destruction and authoring; the read-only slice walks the
 * asset's public surface plus the underlying `FGeometryCollection`
 * managed-array data and reports:
 *
 *   - asset path / name / class
 *   - geometry source list (each `{source_path, transform,
 *     materials, split_components, set_internal_from_material_index,
 *     add_internal_materials}`)
 *   - aggregate counts: vertex / face / geometry / transform / level
 *     count plus a `max_level` (the deepest fracture level present
 *     across the transform group).
 *   - per-fracture-level histogram: count of transforms at each level
 *     index plus a `cluster_count_per_level` parallel array (count
 *     of transforms at that level whose SimulationType == Clustered).
 *   - bone hierarchy depth (max level + 1) plus cached `root_index`.
 *   - `simulation` block: clustering toggle + cluster group index +
 *     max cluster level + cluster connection type token + damage
 *     model token + damage threshold list + per-cluster-only
 *     damage threshold flag + minimum mass clamp + total mass +
 *     mass-as-density flag + density toggles. Plus the removal
 *     surface (scale-on-removal flag, remove-on-max-sleep flag,
 *     sleep-time interval, removal duration interval).
 *   - `materials` array: each entry's path + class.
 *   - `nanite` block: enable flag + fallback flag + minimum
 *     residency value.
 *   - aggregate counts (`source_count`, `material_count`,
 *     `cluster_count`, `rigid_count`, `none_sim_count`,
 *     `embedded_geometry_count`, `auto_instance_mesh_count`).
 *
 * One op (`inspect`, default).
 *
 * Inputs:
 *   - collection / path / asset / asset_path: required. Accepts a
 *     `/Game/...` UGeometryCollection path or a short asset name
 *     (resolved via the asset registry).
 *   - include_geometry_sources: default true.
 *   - include_per_level_histogram: default true.
 *   - max_sources / max_materials: per-list caps.
 *
 * Returns the structure described above.
 *
 * Read-only. We never mutate the asset.
 *
 * Edit-side ops (the fracture / authoring write side, dataflow
 * driver) remain on the BACKLOG.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UGeometryCollection` from
 *     `Runtime/Experimental/GeometryCollectionEngine/Public/GeometryCollection/GeometryCollectionObject.h`.
 *   - `FGeometryCollection::Parent` /
 *     `FGeometryCollection::SimulationType` /
 *     `FGeometryCollection::TransformToGeometryIndex` from
 *     `Runtime/Experimental/Chaos/Public/GeometryCollection/GeometryCollection.h`.
 *   - `FTransformCollection::LevelAttribute` from
 *     `Runtime/Experimental/Chaos/Public/GeometryCollection/TransformCollection.h`.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftChaosEditCommands
{
public:
    FSproftChaosEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleInspect(const TSharedPtr<FJsonObject>& Params);
};
