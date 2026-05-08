#include "Commands/SproftMaterialEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Misc/PackageName.h"
#include "SceneTypes.h"
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

        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Pull an FLinearColor out of a JSON value. Accepts arrays of 3 or 4
     *  numbers, an object with named r/g/b/a keys, or a CSV string passed
     *  through FLinearColor::InitFromString. Returns false on failure. */
    bool TryParseLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() < 3)
            {
                return false;
            }
            OutColor.R = static_cast<float>(Arr[0]->AsNumber());
            OutColor.G = static_cast<float>(Arr[1]->AsNumber());
            OutColor.B = static_cast<float>(Arr[2]->AsNumber());
            OutColor.A = (Arr.Num() >= 4) ? static_cast<float>(Arr[3]->AsNumber()) : 1.0f;
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
            if (!Obj.IsValid())
            {
                return false;
            }
            double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
            if (!Obj->TryGetNumberField(TEXT("r"), R) && !Obj->TryGetNumberField(TEXT("R"), R))
            {
                return false;
            }
            Obj->TryGetNumberField(TEXT("g"), G);
            Obj->TryGetNumberField(TEXT("G"), G);
            Obj->TryGetNumberField(TEXT("b"), B);
            Obj->TryGetNumberField(TEXT("B"), B);
            Obj->TryGetNumberField(TEXT("a"), A);
            Obj->TryGetNumberField(TEXT("A"), A);
            OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
            return true;
        }
        if (Value->Type == EJson::String)
        {
            return OutColor.InitFromString(Value->AsString());
        }
        return false;
    }
}

FSproftMaterialEditCommands::FSproftMaterialEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("material_edit"))
    {
        return HandleMaterialEdit(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown material_edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::HandleMaterialEdit(const TSharedPtr<FJsonObject>& Params)
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

    if (Operation == TEXT("create_material") || Operation == TEXT("create"))
    {
        return CreateMaterial(Params);
    }
    if (Operation == TEXT("create_material_instance_constant") || Operation == TEXT("create_mic")
        || Operation == TEXT("create_instance"))
    {
        return CreateMaterialInstanceConstant(Params);
    }
    if (Operation == TEXT("set_instance_parameter") || Operation == TEXT("set_parameter"))
    {
        return SetInstanceParameter(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported material_edit operation '%s'. Supported: create_material, create_material_instance_constant, set_instance_parameter"), *Operation));
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::CreateMaterial(const TSharedPtr<FJsonObject>& Params)
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

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    UMaterial* NewMaterial = Cast<UMaterial>(Factory->FactoryCreateNew(
        UMaterial::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    if (!NewMaterial)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMaterial"));
    }

    // Wire a Constant3Vector node into BaseColor when a colour is provided.
    bool bWiredBaseColor = false;
    FLinearColor BaseColor(0.5f, 0.5f, 0.5f, 1.0f);
    if (Params->HasField(TEXT("base_color")))
    {
        TSharedPtr<FJsonValue> ColorVal = Params->TryGetField(TEXT("base_color"));
        if (TryParseLinearColor(ColorVal, BaseColor))
        {
            UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
                NewMaterial, UMaterialExpressionConstant3Vector::StaticClass(), -300, 0);
            if (UMaterialExpressionConstant3Vector* ConstExpr = Cast<UMaterialExpressionConstant3Vector>(Expr))
            {
                ConstExpr->Constant = BaseColor;
                if (UMaterialEditingLibrary::ConnectMaterialProperty(ConstExpr, FString(), MP_BaseColor))
                {
                    bWiredBaseColor = true;
                }
                UMaterialEditingLibrary::RecompileMaterial(NewMaterial);
            }
        }
    }

    FAssetRegistryModule::AssetCreated(NewMaterial);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_material"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetBoolField(TEXT("base_color_wired"), bWiredBaseColor);
    if (bWiredBaseColor)
    {
        TArray<TSharedPtr<FJsonValue>> ColorArr;
        ColorArr.Add(MakeShared<FJsonValueNumber>(BaseColor.R));
        ColorArr.Add(MakeShared<FJsonValueNumber>(BaseColor.G));
        ColorArr.Add(MakeShared<FJsonValueNumber>(BaseColor.B));
        ColorArr.Add(MakeShared<FJsonValueNumber>(BaseColor.A));
        ResultObj->SetArrayField(TEXT("base_color"), ColorArr);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::CreateMaterialInstanceConstant(const TSharedPtr<FJsonObject>& Params)
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

    FString ParentMaterialPath;
    if (!Params->TryGetStringField(TEXT("parent_material"), ParentMaterialPath)
        && !Params->TryGetStringField(TEXT("parent"), ParentMaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parent_material' parameter"));
    }

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    UMaterialInterface* Parent = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(ParentMaterialPath));
    if (!Parent)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("parent_material '%s' is not a UMaterialInterface"), *ParentMaterialPath));
    }

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

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;

    UMaterialInstanceConstant* NewMIC = Cast<UMaterialInstanceConstant>(Factory->FactoryCreateNew(
        UMaterialInstanceConstant::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    if (!NewMIC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMaterialInstanceConstant"));
    }

    FAssetRegistryModule::AssetCreated(NewMIC);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_material_instance_constant"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetStringField(TEXT("parent_material"), Parent->GetPathName());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetInstanceParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString InstancePath;
    if (!Params->TryGetStringField(TEXT("material_instance"), InstancePath)
        && !Params->TryGetStringField(TEXT("instance"), InstancePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_instance' parameter"));
    }

    FString ParameterName;
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName)
        && !Params->TryGetStringField(TEXT("name"), ParameterName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }

    if (!Params->HasField(TEXT("value")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    FString ParamTypeHint;
    Params->TryGetStringField(TEXT("parameter_type"), ParamTypeHint);
    ParamTypeHint = ParamTypeHint.ToLower();

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* InstanceAsset = UEditorAssetLibrary::LoadAsset(InstancePath);
    UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(InstanceAsset);
    if (!MIC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UMaterialInstanceConstant: %s"), *InstancePath));
    }

    const FName ParamName(*ParameterName);
    const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
    bool bApplied = false;
    FString AppliedAs;

    auto TryScalar = [&]()
    {
        if (ValueField->Type != EJson::Number)
        {
            return false;
        }
        const float V = static_cast<float>(ValueField->AsNumber());
        if (UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, ParamName, V))
        {
            bApplied = true;
            AppliedAs = TEXT("scalar");
            return true;
        }
        return false;
    };

    auto TryVector = [&]()
    {
        FLinearColor Color;
        if (!TryParseLinearColor(ValueField, Color))
        {
            return false;
        }
        if (UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, ParamName, Color))
        {
            bApplied = true;
            AppliedAs = TEXT("vector");
            return true;
        }
        return false;
    };

    auto TryTexture = [&]()
    {
        if (ValueField->Type != EJson::String)
        {
            return false;
        }
        UTexture* Texture = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(ValueField->AsString()));
        if (!Texture)
        {
            return false;
        }
        if (UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, ParamName, Texture))
        {
            bApplied = true;
            AppliedAs = TEXT("texture");
            return true;
        }
        return false;
    };

    if (ParamTypeHint == TEXT("scalar") || ParamTypeHint == TEXT("float"))
    {
        TryScalar();
    }
    else if (ParamTypeHint == TEXT("vector") || ParamTypeHint == TEXT("color"))
    {
        TryVector();
    }
    else if (ParamTypeHint == TEXT("texture"))
    {
        TryTexture();
    }
    else
    {
        // Auto-detect from JSON value type.
        if (ValueField->Type == EJson::Number)
        {
            TryScalar();
        }
        else if (ValueField->Type == EJson::Array || ValueField->Type == EJson::Object)
        {
            TryVector();
        }
        else if (ValueField->Type == EJson::String)
        {
            // Heuristic: starts with "/" -> texture path; otherwise try the
            // FLinearColor parser, then fall back to a scalar parse.
            const FString StrVal = ValueField->AsString();
            if (StrVal.StartsWith(TEXT("/")))
            {
                if (!TryTexture())
                {
                    TryVector();
                }
            }
            else
            {
                if (!TryVector())
                {
                    if (StrVal.IsNumeric())
                    {
                        const float V = FCString::Atof(*StrVal);
                        if (UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, ParamName, V))
                        {
                            bApplied = true;
                            AppliedAs = TEXT("scalar");
                        }
                    }
                }
            }
        }
    }

    if (!bApplied)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not set parameter '%s' on '%s'. Check that the parameter exists on the parent material and that the value type matches (scalar number, [r,g,b,a] for vector, or a texture asset path)."), *ParameterName, *InstancePath));
    }

    UMaterialEditingLibrary::UpdateMaterialInstance(MIC);

    if (UPackage* Package = MIC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(InstancePath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_instance_parameter"));
    ResultObj->SetStringField(TEXT("material_instance"), MIC->GetPathName());
    ResultObj->SetStringField(TEXT("parameter_name"), ParameterName);
    ResultObj->SetStringField(TEXT("parameter_type"), AppliedAs);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}
