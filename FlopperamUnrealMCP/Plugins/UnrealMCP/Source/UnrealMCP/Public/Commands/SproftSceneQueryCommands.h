#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: scene_query
 *
 * Read-only multiplexed actor query for the editor world. Mirrors the small
 * cut of the hosted Flop "scene_query" tool: combine class, name, label, and
 * tag filters with an optional spatial sphere around a centre point and a
 * result limit. Returns name / label / class / location / rotation / scale /
 * tags / mobility / hidden flags for each match.
 *
 * One operation: "query". All filters are optional and combine with AND.
 *   - class: substring or full class name. Use match_class_substring=false
 *     for exact (case-insensitive) class-name matching.
 *   - name_pattern / label_pattern: case-insensitive substring filters
 *     against GetName() and GetActorLabel().
 *   - tag: a single FName actor tag the actor must carry.
 *   - center / radius: spatial filter, both required together.
 *   - limit: cap on returned results. Defaults 256.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UGameplayStatics::GetAllActorsOfClass for the world walk
 *   - AActor::Tags / GetActorLabel / GetActorLocation accessors
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftSceneQueryCommands
{
public:
    FSproftSceneQueryCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSceneQuery(const TSharedPtr<FJsonObject>& Params);
};
