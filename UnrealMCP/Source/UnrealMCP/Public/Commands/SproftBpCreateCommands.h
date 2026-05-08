#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_create
 *
 * Create a new UBlueprint asset with a chosen parent class. Sits next to the
 * existing `create_blueprint` helper but adds:
 *
 *   - Parent class resolution by short name (Actor, Pawn, Character, GameMode,
 *     ActorComponent, SceneComponent, UserWidget, AIController, etc.) or a
 *     full `/Script/Module.ClassName` path or a `/Game/`-rooted Blueprint
 *     class path. Existing tool only accepted A-prefixed Engine classes.
 *
 *   - Configurable output asset path under `/Game/...`. Existing tool was
 *     hard-coded to `/Game/Blueprints/`.
 *
 *   - Optional flat property dict applied via `FProperty::ImportText` on the
 *     CDO before the first compile, so callers can land defaults in one shot.
 *
 *   - Compile + save in the same call so the asset is ready to use on return.
 *
 * One operation: "create". Inputs:
 *   - name: short asset name. Required.
 *   - path: optional `/Game/...` package path; defaults to `/Game/Blueprints`.
 *   - parent_class: short or full class name; defaults to AActor.
 *   - properties: optional flat dict applied through reflection on the CDO.
 *   - compile / save: defaults true.
 *   - overwrite: defaults false. When false, returns an error if the asset
 *     already exists; when true, opens and returns the existing asset.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `FKismetEditorUtilities::CreateBlueprint`.
 *   - `UBlueprint`, `BPTYPE_Normal`.
 *   - `FProperty::ImportText_InContainer` on the generated CDO.
 *   - `IAssetRegistry::AssetCreated` and `UEditorAssetLibrary::SaveAsset`.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpCreateCommands
{
public:
    FSproftBpCreateCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpCreate(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateBlueprint(const TSharedPtr<FJsonObject>& Params);
};
