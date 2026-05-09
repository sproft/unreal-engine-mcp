#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: landscape_inspect (read-only)
 *
 * Read-only structured dump of every ALandscape actor in the editor world.
 * The hosted Flop tool surface promised "for each ALandscape actor: name,
 * transform, components, materials per layer, layer info objects, total
 * resolution, and the bounding region in world space"; this clean-room
 * implementation walks the public LandscapeProxy API and produces the
 * same shape so a caller can reason about a level's terrain without
 * round-tripping through Python.
 *
 * Operation: single op (`inspect`, default).
 *
 * Optional inputs:
 *   - `include_components`: emit a per-LandscapeComponent record. Default
 *     False because a single landscape can have hundreds of components.
 *   - `include_heightmaps`: emit the deduplicated heightmap-texture
 *     package list per landscape. Default True.
 *   - `include_weightmaps`: emit the deduplicated weightmap-texture
 *     package list per landscape. Default False.
 *   - `name_pattern`: case-insensitive substring filter on actor name
 *     and outliner label.
 *   - `level_filter`: case-insensitive substring filter on the owning
 *     ULevel name.
 *
 * Returns a structured payload with:
 *   - `level_name` / `level_path` of the persistent level.
 *   - `landscapes`: array of per-actor records. Each record reports
 *     `name`, `label`, `class`, `class_path`, `level`, `location`,
 *     `rotation`, `scale`, `landscape_guid`, `component_size_quads`,
 *     `subsection_size_quads`, `num_subsections`, `component_count`,
 *     `landscape_material` (path), `landscape_hole_material` (path
 *     when set), `proxy_bounds_min` / `proxy_bounds_max` /
 *     `proxy_bounds_size` (world space FBox in [x, y, z]),
 *     `streaming_distance_multiplier`, `xy_extent_min` / `xy_extent_max`
 *     / `xy_extent_size` (component-space FIntRect when ULandscapeInfo
 *     is registered), the `layers` array (one entry per
 *     FLandscapeInfoLayerSettings: `layer_name`,
 *     `layer_info_object_name`, `layer_info_object_path`,
 *     `phys_material` (path), `is_no_blend`,
 *     `is_visibility_layer`), and the optional
 *     `heightmap_textures` / `weightmap_textures` /
 *     `components` arrays.
 *
 * Read-only. We do not mutate the actor and we do not save anything.
 *
 * Clean-room implementation derived from the public UE5 Landscape API:
 *   - ALandscapeProxy::LandscapeComponents / LandscapeMaterial /
 *     LandscapeHoleMaterial / StreamingDistanceMultiplier /
 *     ComponentSizeQuads / SubsectionSizeQuads / NumSubsections /
 *     GetProxyBounds / GetLandscapeInfo.
 *   - ULandscapeComponent::HeightmapTexture / WeightmapTextures /
 *     WeightmapLayerAllocations / GetSectionBase.
 *   - FWeightmapLayerAllocationInfo::GetLayerName.
 *   - ULandscapeInfo::GetLandscapeExtent / Layers (FLandscapeInfoLayerSettings).
 *   - ULandscapeLayerInfoObject::GetLayerName / PhysMaterial accessors
 *     (deprecation-aware: the property is being made private in 5.7
 *     so we read through the public getter where available).
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftLandscapeInspectCommands
{
public:
    FSproftLandscapeInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleLandscapeInspect(const TSharedPtr<FJsonObject>& Params);
};
