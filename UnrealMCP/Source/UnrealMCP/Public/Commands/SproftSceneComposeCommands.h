#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: scene_compose
 *
 * Declarative single-actor scene mutation. Three operations on one actor per
 * call to keep the surface and the failure modes narrow:
 *
 *   - "spawn":  spawn a new actor of a given class (Engine or /Game/ BP path)
 *               with an optional [location, rotation, scale] transform,
 *               actor tags, optional preferred FName, optional Outliner
 *               label, and an optional flat property dict applied through
 *               FProperty::ImportText on the new actor. Returns the new
 *               actor's resolved name.
 *   - "modify": apply a transform / tags / property dict patch to an
 *               existing actor resolved by name or label.
 *   - "delete": destroy an existing actor resolved by name or label.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UWorld::SpawnActor / AActor::Destroy
 *   - AActor::SetActorTransform / SetActorLabel / Tags
 *   - FProperty::ImportText_InContainer for property dict overrides
 *   - LoadClass<AActor> for class resolution (with a small Engine namespace
 *     fallback table)
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftSceneComposeCommands
{
public:
    FSproftSceneComposeCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSceneCompose(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> SpawnActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ModifyActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> DeleteActor(const TSharedPtr<FJsonObject>& Params);
};
