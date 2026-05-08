#include "Commands/SproftAssetFactoryCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

namespace
{
    /** Resolve a UScriptStruct by short name or full object path. Loads if not yet in memory. */
    UScriptStruct* ResolveRowStruct(const FString& RowStructPath)
    {
        if (RowStructPath.IsEmpty())
        {
            return nullptr;
        }

        // If the input looks like an object path, load it directly.
        if (RowStructPath.StartsWith(TEXT("/")))
        {
            UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
            if (Loaded)
            {
                return Loaded;
            }
        }

        // Otherwise treat it as a short name and search loaded structs.
        if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *RowStructPath))
        {
            return Found;
        }

        // Try the conventional engine-global name for built-in row structs.
        const FString CoreUObjectPath = FString::Printf(TEXT("/Script/CoreUObject.%s"), *RowStructPath);
        if (UScriptStruct* CoreStruct = LoadObject<UScriptStruct>(nullptr, *CoreUObjectPath))
        {
            return CoreStruct;
        }

        const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *RowStructPath);
        if (UScriptStruct* EngineStruct = LoadObject<UScriptStruct>(nullptr, *EnginePath))
        {
            return EngineStruct;
        }

        return nullptr;
    }

    /** Split "/Game/Foo/Bar" into ("/Game/Foo/", "Bar"). */
    void SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
    {
        FString Trim = InPath;
        Trim.TrimEndInline();
        Trim.RemoveFromEnd(TEXT("/"));

        int32 LastSlash = INDEX_NONE;
        if (Trim.FindLastChar('/', LastSlash))
        {
            OutPackageDir = Trim.Left(LastSlash + 1);
            OutAssetName = Trim.Mid(LastSlash + 1);
        }
        else
        {
            OutPackageDir = TEXT("/Game/");
            OutAssetName = Trim;
        }

        // Strip a trailing ".AssetName" suffix if the caller passed an object path.
        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }
}

FSproftAssetFactoryCommands::FSproftAssetFactoryCommands()
{
}

TSharedPtr<FJsonObject> FSproftAssetFactoryCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("asset_factory"))
    {
        return HandleAssetFactory(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown asset factory command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftAssetFactoryCommands::HandleAssetFactory(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString AssetType;
    if (!Params->TryGetStringField(TEXT("asset_type"), AssetType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_type' parameter"));
    }

    AssetType = AssetType.ToLower();

    if (AssetType == TEXT("datatable") || AssetType == TEXT("data_table"))
    {
        return CreateDataTable(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported asset_type '%s'. Supported: datatable"), *AssetType));
}

TSharedPtr<FJsonObject> FSproftAssetFactoryCommands::CreateDataTable(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }

    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path like /Game/Foo/MyTable, got '%s'"), *PackagePath));
    }

    FString RowStructPath;
    if (!Params->TryGetStringField(TEXT("row_struct"), RowStructPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'row_struct' parameter"));
    }

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    FString PackageDir;
    FString AssetName;
    SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }

    const FString AssetObjectPath = PackageDir + AssetName;
    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    UScriptStruct* RowStruct = ResolveRowStruct(RowStructPath);
    if (!RowStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve row struct '%s'. Pass a full path like /Script/MyModule.MyRow or load the asset first."), *RowStructPath));
    }

    // Create the package and the asset.
    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UDataTable* DataTable = NewObject<UDataTable>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!DataTable)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UDataTable"));
    }

    DataTable->RowStruct = RowStruct;

    FAssetRegistryModule::AssetCreated(DataTable);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_type"), TEXT("DataTable"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}
