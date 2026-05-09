#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: asset_references
 *
 * Read-only dependency-graph dump for a single asset, backed by
 * `IAssetRegistry::GetReferencers` / `GetDependencies`. Useful when we
 * need to know what would break if we delete or rename an asset.
 *
 * Inputs:
 *   - "asset" (or "asset_path" / "path"): full asset path (e.g.
 *      "/Game/Foo/MyMaterial" or "/Game/Foo/MyMaterial.MyMaterial").
 *      Internally we resolve the package name (the part before the dot)
 *      because the dependency walk is keyed by package.
 *   - "direction": one of
 *        "hard_referencers"  (default; assets that hard-import this one)
 *        "soft_referencers"  (assets that soft-reference this one)
 *        "hard_dependencies" (assets this one hard-imports)
 *        "soft_dependencies" (assets this one soft-references)
 *        "all_referencers" / "all_dependencies" (any package category,
 *        no Hard / NotHard restriction)
 *   - "depth": int, default 1. When >1 we walk the graph transitively
 *      and return the union of every package reached. Capped at 6 to
 *      keep large content trees bounded.
 *   - "class_filter": optional class token (short name or full
 *      "/Script/Module.ClassName" path) used to drop rows whose asset
 *      class does not match. Mirrors the `search_assets` resolver but
 *      runs as a post-filter on the returned package set.
 *   - "limit": int, default 1024, hard-capped at 50000.
 *
 * Output:
 *   - "assets": array of `{ path, name, class, package }` rows for
 *      every package the walk reached.
 *   - "count": rows returned (post-filter, post-limit).
 *   - "matched_total": rows that passed the class filter before the
 *      limit was applied.
 *   - "limit_hit": true when matched_total > limit.
 *   - "depth_reached": the deepest level we actually walked
 *      (<= requested depth; can be lower if the graph terminated).
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `IAssetRegistry::Get`, `GetReferencers`, `GetDependencies`
 *   - `EDependencyCategory::Package`, `EDependencyQuery::Hard / Soft`
 *   - `IAssetRegistry::GetAssetsByPackageName` for the per-package
 *     class lookup that drives the optional class filter and the
 *     row's class field.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftAssetReferencesCommands
{
public:
    FSproftAssetReferencesCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAssetReferences(const TSharedPtr<FJsonObject>& Params);
};
