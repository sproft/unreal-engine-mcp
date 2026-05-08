#include "Commands/SproftAssetFactoryCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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
    if (AssetType == TEXT("enum") || AssetType == TEXT("user_defined_enum"))
    {
        return CreateEnum(Params);
    }
    if (AssetType == TEXT("struct") || AssetType == TEXT("user_defined_struct"))
    {
        return CreateStruct(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported asset_type '%s'. Supported: datatable, enum, struct"), *AssetType));
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

namespace
{
    /** Map a short type string (matching the existing BPVariables vocabulary)
     *  to an FEdGraphPinType. Returns false if the type is unrecognised so
     *  callers can refuse the entry instead of silently changing types. */
    bool TryGetPinTypeFromString(const FString& InType, FEdGraphPinType& OutPinType, FString& OutError)
    {
        FString Type = InType.ToLower().TrimStartAndEnd();

        if (Type == TEXT("bool") || Type == TEXT("boolean"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
            return true;
        }
        if (Type == TEXT("int") || Type == TEXT("int32") || Type == TEXT("integer"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
            return true;
        }
        if (Type == TEXT("int64"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
            return true;
        }
        if (Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("real"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
            return true;
        }
        if (Type == TEXT("string"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
            return true;
        }
        if (Type == TEXT("name"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
            return true;
        }
        if (Type == TEXT("text"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
            return true;
        }
        if (Type == TEXT("vector"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
            return true;
        }
        if (Type == TEXT("rotator"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
            return true;
        }
        if (Type == TEXT("transform"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
            return true;
        }
        if (Type == TEXT("color") || Type == TEXT("linearcolor") || Type == TEXT("linear_color"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
            return true;
        }

        // Allow callers to reference an existing UScriptStruct asset by full path.
        if (InType.StartsWith(TEXT("/")))
        {
            if (UScriptStruct* AsStruct = LoadObject<UScriptStruct>(nullptr, *InType))
            {
                OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                OutPinType.PinSubCategoryObject = AsStruct;
                return true;
            }
        }

        OutError = FString::Printf(TEXT("Unsupported field type '%s'. Supported: bool, int, int64, float, string, name, text, vector, rotator, transform, color, or a /Game/-rooted UScriptStruct path."), *InType);
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftAssetFactoryCommands::CreateEnum(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path, got '%s'"), *PackagePath));
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
    if (!FEnumEditorUtils::IsNameAvailebleForUserDefinedEnum(*AssetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Enum name '%s' is already in use"), *AssetName));
    }

    // Collect entries before we create the asset so we can reject early.
    const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("entries"), EntriesArray) || EntriesArray == nullptr || EntriesArray->Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'entries' parameter (expected a non-empty array of strings)"));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UEnum* CreatedEnumBase = FEnumEditorUtils::CreateUserDefinedEnum(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    UUserDefinedEnum* CreatedEnum = Cast<UUserDefinedEnum>(CreatedEnumBase);
    if (!CreatedEnum)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UUserDefinedEnum"));
    }

    TArray<FString> AddedEntries;
    for (const TSharedPtr<FJsonValue>& EntryVal : *EntriesArray)
    {
        if (!EntryVal.IsValid() || EntryVal->Type != EJson::String)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("'entries' must be an array of strings"));
        }
        const FString EntryName = EntryVal->AsString().TrimStartAndEnd();
        if (EntryName.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Enum entry names must not be empty"));
        }
        if (!FEnumEditorUtils::IsProperNameForUserDefinedEnumerator(CreatedEnum, EntryName))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Enum entry '%s' is not a valid display name (duplicate or invalid)"), *EntryName));
        }

        FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(CreatedEnum);

        // The new entry is the second-to-last (the last is the auto _MAX guard).
        const int32 NewEntryIndex = CreatedEnum->NumEnums() - 2;
        if (NewEntryIndex < 0)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Internal error: enum had no entries after AddNewEnumeratorForUserDefinedEnum"));
        }
        FEnumEditorUtils::SetEnumeratorDisplayName(CreatedEnum, NewEntryIndex, FText::FromString(EntryName));
        AddedEntries.Add(EntryName);
    }

    FEnumEditorUtils::EnsureAllDisplayNamesExist(CreatedEnum);

    FAssetRegistryModule::AssetCreated(CreatedEnum);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TArray<TSharedPtr<FJsonValue>> EntriesJson;
    for (const FString& Entry : AddedEntries)
    {
        EntriesJson.Add(MakeShared<FJsonValueString>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_type"), TEXT("Enum"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetArrayField(TEXT("entries"), EntriesJson);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftAssetFactoryCommands::CreateStruct(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path, got '%s'"), *PackagePath));
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

    // Validate the field list before we touch the package.
    const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("fields"), FieldsArray) || FieldsArray == nullptr || FieldsArray->Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'fields' parameter (expected a non-empty array of {name, type} objects)"));
    }

    struct FFieldSpec
    {
        FString Name;
        FString TypeText;
        FEdGraphPinType PinType;
    };
    TArray<FFieldSpec> Specs;
    Specs.Reserve(FieldsArray->Num());
    for (const TSharedPtr<FJsonValue>& FieldVal : *FieldsArray)
    {
        if (!FieldVal.IsValid() || FieldVal->Type != EJson::Object)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("'fields' must be an array of objects with 'name' and 'type'"));
        }
        const TSharedPtr<FJsonObject> FieldObj = FieldVal->AsObject();
        FFieldSpec Spec;
        if (!FieldObj->TryGetStringField(TEXT("name"), Spec.Name))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Field is missing 'name'"));
        }
        if (!FieldObj->TryGetStringField(TEXT("type"), Spec.TypeText))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Field is missing 'type'"));
        }
        FString PinTypeError;
        if (!TryGetPinTypeFromString(Spec.TypeText, Spec.PinType, PinTypeError))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(PinTypeError);
        }
        Specs.Add(MoveTemp(Spec));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UUserDefinedStruct* CreatedStruct = FStructureEditorUtils::CreateUserDefinedStruct(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    if (!CreatedStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UUserDefinedStruct"));
    }

    // CreateUserDefinedStruct seeds a default first member so the struct
    // compiles. Drop it before we add the caller's fields.
    auto& InitialDescriptions = FStructureEditorUtils::GetVarDesc(CreatedStruct);
    while (InitialDescriptions.Num() > 0)
    {
        const FGuid SeedGuid = InitialDescriptions[0].VarGuid;
        if (!FStructureEditorUtils::RemoveVariable(CreatedStruct, SeedGuid))
        {
            break;
        }
    }

    TArray<TSharedPtr<FJsonValue>> AddedFieldsJson;
    for (const FFieldSpec& Spec : Specs)
    {
        const int32 BeforeNum = FStructureEditorUtils::GetVarDesc(CreatedStruct).Num();
        if (!FStructureEditorUtils::AddVariable(CreatedStruct, Spec.PinType))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to add field '%s' of type '%s'"), *Spec.Name, *Spec.TypeText));
        }
        const auto& Descs = FStructureEditorUtils::GetVarDesc(CreatedStruct);
        if (Descs.Num() <= BeforeNum)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("AddVariable did not append a new field for '%s'"), *Spec.Name));
        }
        const FGuid NewGuid = Descs.Last().VarGuid;
        if (!FStructureEditorUtils::RenameVariable(CreatedStruct, NewGuid, Spec.Name))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to rename new field to '%s' (must be unique)"), *Spec.Name));
        }

        TSharedPtr<FJsonObject> FieldJson = MakeShared<FJsonObject>();
        FieldJson->SetStringField(TEXT("name"), Spec.Name);
        FieldJson->SetStringField(TEXT("type"), Spec.TypeText);
        AddedFieldsJson.Add(MakeShared<FJsonValueObject>(FieldJson));
    }

    FStructureEditorUtils::CompileStructure(CreatedStruct);

    FAssetRegistryModule::AssetCreated(CreatedStruct);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_type"), TEXT("Struct"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetArrayField(TEXT("fields"), AddedFieldsJson);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}
