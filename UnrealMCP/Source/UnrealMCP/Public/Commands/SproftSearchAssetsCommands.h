#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: search_assets
 *
 * Content-Browser-style asset search backed by `IAssetRegistry`. Read-only.
 *
 * Inputs (all optional except the limit guard, which defaults to 256):
 *   - "class": single class short name ("StaticMesh") or full
 *      `/Script/Module.ClassName` path. Combined with `class_list` if both
 *      are supplied.
 *   - "class_list": array of class names (mixed short and full). Each
 *      class is added to `FARFilter::ClassPaths`.
 *   - "class_pattern": case-insensitive substring matched against the
 *      asset's class short name. Applied as a post-filter on top of the
 *      `FARFilter` result.
 *   - "include_subclasses": true to set `FARFilter::bRecursiveClasses`.
 *   - "path": single content-root prefix ("/Game/Crafting"). Combined
 *      with `path_list` if both are supplied.
 *   - "path_list": array of content-root prefixes.
 *   - "recursive_paths": true (default) to set
 *      `FARFilter::bRecursivePaths` so subfolders are also searched.
 *   - "name_pattern": case-insensitive substring matched against the
 *      asset's short name (no path).
 *   - "tag": object form `{ "name": "X", "value": "Y" }` or array of
 *      such pairs. Applied as `FARFilter::TagsAndValues` so callers can
 *      filter on package tags (e.g. ParentClass for Blueprints).
 *   - "limit": integer cap on returned rows. Default 256.
 *   - "include_disk_size": true to include `disk_size` per row from
 *      `IAssetRegistry::TryGetAssetPackageData` (one extra lookup per
 *      result).
 *
 * Output:
 *   - "assets": array of `{ path, name, class, package, package_path }`
 *      rows; `disk_size` is included only when `include_disk_size=true`.
 *   - "count": int (rows returned, after the limit / post-filter).
 *   - "limit_hit": bool flag set when the cap was the reason rows
 *      stopped, so a caller can paginate by tightening the filter.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `IAssetRegistry::Get`, `FARFilter`, `IAssetRegistry::GetAssets`
 *   - `IAssetRegistry::TryGetAssetPackageData` for the disk-size lookup
 *   - Standard FName / FTopLevelAssetPath / FName-based lookups for
 *     class resolution.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftSearchAssetsCommands
{
public:
    FSproftSearchAssetsCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSearchAssets(const TSharedPtr<FJsonObject>& Params);
};
