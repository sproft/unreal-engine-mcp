#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: asset_factory
 *
 * A trimmed clone of the hosted Flop "asset_factory" surface. Supported asset
 * types in this fork:
 *   - "datatable": create a UDataTable with a configurable row UScriptStruct.
 *   - "enum": create a UUserDefinedEnum with named entries.
 *   - "struct": create a UUserDefinedStruct with typed fields.
 *
 * DataAsset and Enhanced Input bundles remain in BACKLOG.md.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UPackage / FAssetRegistryModule for asset registration
 *   - UDataTable / UScriptStruct for the row schema
 *   - FEnumEditorUtils::CreateUserDefinedEnum + AddNewEnumeratorForUserDefinedEnum
 *   - FStructureEditorUtils::CreateUserDefinedStruct + AddVariable
 *   - UEditorAssetLibrary for save / existence checks
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftAssetFactoryCommands
{
public:
    FSproftAssetFactoryCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAssetFactory(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateDataTable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> CreateEnum(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> CreateStruct(const TSharedPtr<FJsonObject>& Params);
};
