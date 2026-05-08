#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_input
 *
 * Manage Enhanced Input data assets. The hosted Flop "bp_input" tool also
 * wires Enhanced Input events into Blueprint event graphs; that piece is
 * tracked separately in BACKLOG.md and is a clean follow-up for the existing
 * BlueprintGraph node helpers in this repo. This first cut covers the
 * documented data-asset surface that designers ask for:
 *
 *   - "create_input_action": create a UInputAction asset with a chosen
 *      EInputActionValueType (Boolean, Axis1D, Axis2D, Axis3D).
 *   - "create_input_mapping_context": create an empty UInputMappingContext.
 *   - "add_mapping": append one FEnhancedActionKeyMapping (action + key) to
 *      an existing IMC's default key-mapping list.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UInputAction (UDataAsset subclass, EnhancedInput plugin)
 *   - UInputMappingContext::MapKey (the documented public binding entry point)
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
};
