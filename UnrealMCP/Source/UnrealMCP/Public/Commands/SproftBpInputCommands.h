#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_input
 *
 * Manage Enhanced Input data assets and wire Enhanced Input event nodes into
 * Blueprint event graphs.
 *
 * Operations:
 *   - "create_input_action": create a UInputAction asset with a chosen
 *      EInputActionValueType (Boolean, Axis1D, Axis2D, Axis3D).
 *   - "create_input_mapping_context": create an empty UInputMappingContext.
 *   - "add_mapping": append one FEnhancedActionKeyMapping (action + key) to
 *      an existing IMC's default key-mapping list.
 *   - "add_action_event_node": spawn a UK2Node_EnhancedInputAction event
 *      node in a target Blueprint's event graph for a given UInputAction
 *      asset, and optionally MakeLinkTo from a chosen exec pin (default
 *      "Triggered") to a named function call on the same Blueprint.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UInputAction (UDataAsset subclass, EnhancedInput plugin)
 *   - UInputMappingContext::MapKey (the documented public binding entry point)
 *   - UK2Node_EnhancedInputAction (InputBlueprintNodes module)
 *   - UK2Node_CallFunction::SetFromFunction for the function-call follow-on
 *   - FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified +
 *     FKismetEditorUtilities::CompileBlueprint for the post-edit save path
 *   - FAssetRegistryModule::AssetCreated for content-browser registration
 *   - UEditorAssetLibrary for save / existence checks
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpInputCommands
{
public:
    FSproftBpInputCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpInput(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateInputAction(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> CreateInputMappingContext(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddMapping(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddActionEventNode(const TSharedPtr<FJsonObject>& Params);
};
