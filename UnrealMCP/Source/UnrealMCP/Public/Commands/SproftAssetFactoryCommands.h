#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: asset_factory
 *
 * A trimmed clone of the hosted Flop "asset_factory" surface. The first
 * supported asset type is DataTable. Other asset types (Enum, Struct,
 * DataAsset, Enhanced Input) are tracked in BACKLOG.md for future passes.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UPackage / FAssetRegistryModule for asset registration
 *   - UDataTable / UScriptStruct for the row schema
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
};
