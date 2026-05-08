#include "Commands/SproftBpInputCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
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

    /** Map a short value-type string to EInputActionValueType. Returns false if unrecognised. */
    bool TryParseInputValueType(const FString& InText, EInputActionValueType& OutType)
    {
        const FString Text = InText.ToLower().TrimStartAndEnd();
        if (Text == TEXT("bool") || Text == TEXT("boolean") || Text == TEXT("digital"))
        {
            OutType = EInputActionValueType::Boolean;
            return true;
        }
        if (Text == TEXT("axis1d") || Text == TEXT("axis_1d") || Text == TEXT("1d") || Text == TEXT("float"))
        {
            OutType = EInputActionValueType::Axis1D;
            return true;
        }
        if (Text == TEXT("axis2d") || Text == TEXT("axis_2d") || Text == TEXT("2d") || Text == TEXT("vector2d"))
        {
            OutType = EInputActionValueType::Axis2D;
            return true;
        }
        if (Text == TEXT("axis3d") || Text == TEXT("axis_3d") || Text == TEXT("3d") || Text == TEXT("vector3d") || Text == TEXT("vector"))
        {
            OutType = EInputActionValueType::Axis3D;
            return true;
        }
        return false;
    }

    /** Convert EInputActionValueType into a stable identifier for JSON results. */
    FString InputValueTypeToString(EInputActionValueType InType)
    {
        switch (InType)
        {
            case EInputActionValueType::Boolean: return TEXT("Boolean");
            case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
            case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
            case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
        }
        return TEXT("Unknown");
    }
}

FSproftBpInputCommands::FSproftBpInputCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_input"))
    {
        return HandleBpInput(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_input command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::HandleBpInput(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }
    Operation = Operation.ToLower();

    if (Operation == TEXT("create_input_action") || Operation == TEXT("create_action"))
    {
        return CreateInputAction(Params);
    }
    if (Operation == TEXT("create_input_mapping_context") || Operation == TEXT("create_imc")
        || Operation == TEXT("create_mapping_context"))
    {
        return CreateInputMappingContext(Params);
    }
    if (Operation == TEXT("add_mapping") || Operation == TEXT("map_key"))
    {
        return AddMapping(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_input operation '%s'. Supported: create_input_action, create_input_mapping_context, add_mapping"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::CreateInputAction(const TSharedPtr<FJsonObject>& Params)
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

    FString ValueTypeText = TEXT("Boolean");
    Params->TryGetStringField(TEXT("value_type"), ValueTypeText);

    EInputActionValueType ValueType = EInputActionValueType::Boolean;
    if (!TryParseInputValueType(ValueTypeText, ValueType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported value_type '%s'. Supported: bool, axis1d, axis2d, axis3d"), *ValueTypeText));
    }

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    bool bTriggerWhenPaused = false;
    Params->TryGetBoolField(TEXT("trigger_when_paused"), bTriggerWhenPaused);

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);

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

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UInputAction* NewAction = NewObject<UInputAction>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewAction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UInputAction"));
    }

    NewAction->ValueType = ValueType;
    NewAction->bTriggerWhenPaused = bTriggerWhenPaused;
    if (!Description.IsEmpty())
    {
        NewAction->ActionDescription = FText::FromString(Description);
    }

    FAssetRegistryModule::AssetCreated(NewAction);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_input_action"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetStringField(TEXT("value_type"), InputValueTypeToString(ValueType));
    ResultObj->SetBoolField(TEXT("trigger_when_paused"), bTriggerWhenPaused);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::CreateInputMappingContext(const TSharedPtr<FJsonObject>& Params)
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

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);

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

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UInputMappingContext* NewIMC = NewObject<UInputMappingContext>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewIMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UInputMappingContext"));
    }

    if (!Description.IsEmpty())
    {
        NewIMC->ContextDescription = FText::FromString(Description);
    }

    FAssetRegistryModule::AssetCreated(NewIMC);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_input_mapping_context"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftBpInputCommands::AddMapping(const TSharedPtr<FJsonObject>& Params)
{
    FString IMCPath;
    if (!Params->TryGetStringField(TEXT("input_mapping_context"), IMCPath)
        && !Params->TryGetStringField(TEXT("imc"), IMCPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_mapping_context' parameter"));
    }

    FString ActionPath;
    if (!Params->TryGetStringField(TEXT("input_action"), ActionPath)
        && !Params->TryGetStringField(TEXT("action"), ActionPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_action' parameter"));
    }

    FString KeyText;
    if (!Params->TryGetStringField(TEXT("key"), KeyText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'key' parameter (e.g. \"SpaceBar\", \"W\", \"Gamepad_FaceButton_Bottom\")"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* IMCAsset = UEditorAssetLibrary::LoadAsset(IMCPath);
    UInputMappingContext* IMC = Cast<UInputMappingContext>(IMCAsset);
    if (!IMC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputMappingContext: %s"), *IMCPath));
    }

    UObject* ActionAsset = UEditorAssetLibrary::LoadAsset(ActionPath);
    UInputAction* Action = Cast<UInputAction>(ActionAsset);
    if (!Action)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UInputAction: %s"), *ActionPath));
    }

    const FKey Key(*KeyText);
    if (!Key.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FKey '%s' is not a known engine key. Pass an FKey FName like 'SpaceBar', 'W', or 'Gamepad_FaceButton_Bottom'."), *KeyText));
    }

    // UInputMappingContext::MapKey is the documented public binding entry
    // point and returns a reference to the new FEnhancedActionKeyMapping.
    FEnhancedActionKeyMapping& NewMapping = IMC->MapKey(Action, Key);
    (void)NewMapping; // Currently unused; kept for clarity.

    if (UPackage* Package = IMC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(IMCPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_mapping"));
    ResultObj->SetStringField(TEXT("input_mapping_context"), IMC->GetPathName());
    ResultObj->SetStringField(TEXT("input_action"), Action->GetPathName());
    ResultObj->SetStringField(TEXT("key"), Key.ToString());
    ResultObj->SetNumberField(TEXT("mapping_count"), IMC->GetMappings().Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}
