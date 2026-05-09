#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: chaos_edit (read + edit slice)
 *
 * Inspect or mutate a `UGeometryCollection` asset. Geometry
 * Collections drive Chaos destruction and authoring; the inspect
 * slice walks the asset's public surface plus the underlying
 * `FGeometryCollection` managed-array data and reports:
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
 * Operations:
 *   - `inspect` (default): the read-only walk above.
 *   - `set_simulation_settings`: writes a flat property dict against
 *     the asset's reflected simulation surface (`Mass`,
 *     `MinimumMassClamp`, `bMassAsDensity`, `EnableClustering`,
 *     `MaxClusterLevel`, `DamageModel`, etc.). Each entry routes
 *     through `FProperty::ImportText_InContainer`. After the writes
 *     `InvalidateCollection` runs so the cached simulation data
 *     rebuilds on the next access.
 *   - `import_static_mesh`: appends a UStaticMesh into the
 *     collection through
 *     `FGeometryCollectionConversion::AppendStaticMesh`. Editor-only
 *     API. Optional `transform` lays the mesh down at a chosen
 *     world-space transform; the default is identity.
 *
 * Inputs:
 *   - collection / path / asset / asset_path: required. Accepts a
 *     `/Game/...` UGeometryCollection path or a short asset name
 *     (resolved via the asset registry).
 *   - include_geometry_sources: default true.
 *   - include_per_level_histogram: default true.
 *   - max_sources / max_materials: per-list caps.
 *
 * Op-specific inputs:
 *   - set_simulation_settings: `properties` (flat dict). Keys are
 *     UPROPERTY FNames; values are JSON literals routed through
 *     `FProperty::ImportText_InContainer`.
 *   - import_static_mesh: `static_mesh` (`/Game/...` path or short
 *     name) plus optional `transform` (`{location, rotation, scale}`
 *     dict; missing components default to zero / zero / one).
 *
 * Optional inputs (mutating ops):
 *   - `save`: default true.
 *
 * Returns the inspect structure for the inspect op, or a per-op
 * dict carrying `operation`, `collection`, op-specific fields, and
 * a `saved` flag.
 *
 * Edit-side ops still on the BACKLOG: the fracture / authoring
 * write side beyond mesh append, dataflow driver, per-instance
 * damage threshold override, and the re-cluster ops.
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
    TSharedPtr<FJsonObject> HandleSetSimulationSettings(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleImportStaticMesh(const TSharedPtr<FJsonObject>& Params);
};
