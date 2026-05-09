#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_class
 *
 * Manage class-level settings on an existing UBlueprint asset. Sits next to
 * `bp_create` (which creates the asset) and `bp_brief` (which reads class
 * metadata for orientation).
 *
 * Operations (keyed by `op` plus a required `blueprint` resolver):
 *   - "read": dump current class-level settings: parent class, blueprint
 *     type, display name, description, category, namespace, hide
 *     categories list, and the implemented Blueprint interface paths.
 *   - "set_parent": re-parent the Blueprint to a new parent class. The
 *     parent resolver mirrors `bp_create`: short names, full
 *     `/Script/Module.ClassName` paths, and `/Game/...` Blueprint class
 *     paths all resolve. Goes through `BlueprintObj->ParentClass`,
 *     `FBlueprintEditorUtils::RefreshAllNodes`, and a recompile.
 *   - "set_class_settings": write any subset of `description`,
 *     `display_name`, `category`, `namespace`, and `hide_categories`
 *     (FString list, replaces the whole array) onto the Blueprint.
 *   - "add_interface": implement a Blueprint Interface by full path. We
 *     accept `/Script/Module.IName`, `/Game/...` UBlueprint Interface
 *     paths, and short names that resolve under the loaded class set.
 *     Wraps `FBlueprintEditorUtils::ImplementNewInterface` with the
 *     `FTopLevelAssetPath` overload.
 *   - "remove_interface": tear down an implemented interface. Optional
 *     `preserve_functions` defaults to false (drops the function graphs
 *     wholesale).
 *
 * All mutating ops compile + save the asset on success unless
 * `compile=false` or `save=false` is passed.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UBlueprint::ParentClass and the BlueprintOptions UPROPERTY surface
 *     (BlueprintDescription, BlueprintDisplayName, BlueprintCategory,
 *     BlueprintNamespace, HideCategories).
 *   - FBlueprintEditorUtils::RefreshAllNodes /
 *     MarkBlueprintAsStructurallyModified.
 *   - FBlueprintEditorUtils::ImplementNewInterface / RemoveInterface
 *     (FTopLevelAssetPath overload).
 *   - FKismetEditorUtilities::CanCreateBlueprintOfClass for parent gating.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpClassCommands
{
public:
    FSproftBpClassCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpClass(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> ReadClass(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> SetParent(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetClassSettings(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddInterface(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> RemoveInterface(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
};
