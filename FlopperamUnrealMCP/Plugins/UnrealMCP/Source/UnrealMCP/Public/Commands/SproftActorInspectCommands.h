#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: actor_inspect
 *
 * Read-only counterpart to scene_query for a single actor. Returns the actor's
 * name / label / class / class path / transform / tags / mobility / hidden
 * flags, plus the full attached component tree with each component's class,
 * relative transform (when scene component), tags, and a small selection of
 * visible UPROPERTY values rendered through FProperty::ExportText.
 *
 * One operation: "inspect". The actor is resolved by name (FName) first and
 * then by Outliner label (case-insensitive, exact). Optional flags toggle
 * the heavier work (component listing, per-component property dump).
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UGameplayStatics::GetAllActorsOfClass for the editor world walk
 *   - AActor::GetComponents / GetActorLabel / GetRootComponent accessors
 *   - FProperty::ExportText_InContainer for the property snapshots
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftActorInspectCommands
{
public:
    FSproftActorInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleActorInspect(const TSharedPtr<FJsonObject>& Params);
};
