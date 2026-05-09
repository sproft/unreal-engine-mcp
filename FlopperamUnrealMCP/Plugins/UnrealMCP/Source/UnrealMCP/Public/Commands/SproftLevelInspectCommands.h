#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: level_inspect
 *
 * Read-only structured per-actor record list for the active editor world plus
 * any loaded sublevels. Sits between scene_brief (one designer-readable
 * summary) and scene_query (filtered subset of actor records). Always returns
 * a uniformly-shaped per-actor block, with optional component listings.
 *
 * One operation: "inspect". Optional filters narrow the result set:
 *   - class: substring match against the actor's class short name + path.
 *     Use match_class_substring=false for an exact (case-insensitive) match
 *     on the short class name.
 *   - name_pattern / label_pattern: case-insensitive substring matches
 *     against GetName() and GetActorLabel().
 *   - tag: a single FName actor tag the actor must carry.
 *   - level_filter: include only actors whose owning ULevel name matches.
 *     Defaults to None (every loaded level).
 *   - include_components: emit a compact component list per actor. Defaults
 *     False to keep responses small.
 *   - limit: cap on returned actor records. Defaults 512.
 *
 * Result includes:
 *   - level_name / level_path of the persistent level.
 *   - levels: every loaded ULevel with its actor count.
 *   - actors: per-actor records (name, label, class, transform, tags,
 *     hidden flags, mobility, level, optional components).
 *   - total_matches / returned / truncated counters.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UWorld::GetLevels for the level walk
 *   - ULevel::Actors for the per-level enumeration
 *   - AActor / UActorComponent accessors for per-record fields
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftLevelInspectCommands
{
public:
    FSproftLevelInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleLevelInspect(const TSharedPtr<FJsonObject>& Params);
};
