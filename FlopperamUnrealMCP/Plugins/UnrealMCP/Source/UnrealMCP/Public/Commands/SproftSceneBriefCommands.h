#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: scene_brief
 *
 * Read-only one-shot orientation summary of the active editor world. Mirrors
 * the documented "scene_brief" entry on the hosted Flop tool surface. The goal
 * is a designer-readable snapshot without paying for a full per-actor list.
 *
 * One operation: "brief". No required parameters. Returns:
 *   - level_name / level_path: the persistent level's short name + asset path.
 *   - sublevels: streaming-level paths attached to the world.
 *   - actor_count: total actors in the world.
 *   - class_counts: a compact { ClassName: count } map covering every class
 *     present in the world (capped to keep responses small).
 *   - bounds: { min, max, center, extent } of the union of every component
 *     bounds in the world. Returned as null when nothing has bounds.
 *   - game_mode_class: world settings' configured GameMode override, when set.
 *   - default_pawn_class: GameMode's default pawn class, when set.
 *   - level_blueprint_has_events: yes / no flag indicating whether the level
 *     blueprint declares any custom event nodes.
 *   - tags_in_use: deduplicated list of FName tags on any actor.
 *   - notable_actors: short list of "interesting" actors (player start,
 *     directional light, post-process volume).
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UWorld / ULevel / AWorldSettings
 *   - AActor::GetActorBounds for the world bounds union
 *   - UGameplayStatics::GetAllActorsOfClass for enumeration
 *   - FBlueprintEditorUtils::FindUserConstructionScript / GetAllGraphs for
 *     the level blueprint event probe
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftSceneBriefCommands
{
public:
    FSproftSceneBriefCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSceneBrief(const TSharedPtr<FJsonObject>& Params);
};
