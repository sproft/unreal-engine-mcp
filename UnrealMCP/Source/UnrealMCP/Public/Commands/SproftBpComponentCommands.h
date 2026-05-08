#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_component
 *
 * Add a UActorComponent subclass to an existing UBlueprint's
 * SimpleConstructionScript. The hosted Flop "bp_component" tool covers a
 * larger surface (move, rename, remove, set physics, etc.); this first cut
 * focuses on the documented "add a component to a Blueprint" verb that the
 * existing add_component_to_blueprint helper already implements but with a
 * looser class resolver, optional parent component for attachment, and an
 * optional flat property dict applied through reflection on the component
 * template before compile.
 *
 * One operation: "add_component". Inputs:
 *   - blueprint: short name or absolute path (`/Game/...`).
 *   - component_class: short name (`StaticMeshComponent`, `SpringArm`,
 *     `CameraComponent`) or full path (`/Script/Engine.StaticMeshComponent`).
 *   - component_name: variable name for the new SCS node.
 *   - parent_component: optional FName of an existing scene component to
 *     attach the new node under. Defaults to the SCS root.
 *   - properties: optional flat dict of property names to JSON values applied
 *     via FProperty::ImportText_InContainer on the component template.
 *   - location / rotation / scale: optional relative transform vectors.
 *   - compile: compile the Blueprint after the change. Defaults true.
 *   - save: save the Blueprint asset after compile. Defaults true.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBlueprint, USimpleConstructionScript, USCS_Node
 *   - UActorComponent / USceneComponent reflection
 *   - FKismetEditorUtilities::CompileBlueprint
 *   - FBlueprintEditorUtils::MarkBlueprintAsModified
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpComponentCommands
{
public:
    FSproftBpComponentCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpComponent(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> AddComponent(const TSharedPtr<FJsonObject>& Params);
};
