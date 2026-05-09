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
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "SceneTypes.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace
{
    /** Split "/Game/Foo/Bar" into ("/Game/Foo/", "Bar"). */
    void MaterialEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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

    /** Resolve a UMaterialExpression subclass from a user-provided
     *  short-name token. Falls back to a UClass lookup so callers can
     *  also pass `MaterialExpressionFoo` or `/Script/Engine.UMaterialExpressionFoo`. */
    TSubclassOf<UMaterialExpression> ResolveExpressionClass(const FString& Token)
    {
        FString Norm = Token;
        Norm.TrimStartAndEndInline();
        if (Norm.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Norm.ToLower().Replace(TEXT("_"), TEXT(""));

        // Most-used short names. The map is intentionally small and
        // declarative; consumers can always fall back to the full path.
        if (Lower == TEXT("constant") || Lower == TEXT("scalar"))
            return UMaterialExpressionConstant::StaticClass();
        if (Lower == TEXT("constant3vector") || Lower == TEXT("vector3") || Lower == TEXT("color"))
            return UMaterialExpressionConstant3Vector::StaticClass();
        if (Lower == TEXT("constant4vector") || Lower == TEXT("vector4") || Lower == TEXT("color4"))
            return UMaterialExpressionConstant4Vector::StaticClass();
        if (Lower == TEXT("scalarparameter"))
            return UMaterialExpressionScalarParameter::StaticClass();
        if (Lower == TEXT("vectorparameter"))
            return UMaterialExpressionVectorParameter::StaticClass();
        if (Lower == TEXT("texturesample"))
            return UMaterialExpressionTextureSample::StaticClass();
        if (Lower == TEXT("texturesampleparameter2d") || Lower == TEXT("textureparameter")
            || Lower == TEXT("textureparam") || Lower == TEXT("texturesampleparam"))
            return UMaterialExpressionTextureSampleParameter2D::StaticClass();
        if (Lower == TEXT("textureobjectparameter") || Lower == TEXT("textureobjectparam"))
            return UMaterialExpressionTextureObjectParameter::StaticClass();
        if (Lower == TEXT("multiply") || Lower == TEXT("mul"))
            return UMaterialExpressionMultiply::StaticClass();
        if (Lower == TEXT("add"))
            return UMaterialExpressionAdd::StaticClass();
        if (Lower == TEXT("subtract") || Lower == TEXT("sub"))
            return UMaterialExpressionSubtract::StaticClass();
        if (Lower == TEXT("divide") || Lower == TEXT("div"))
            return UMaterialExpressionDivide::StaticClass();
        if (Lower == TEXT("lerp") || Lower == TEXT("linearinterpolate"))
            return UMaterialExpressionLinearInterpolate::StaticClass();
        if (Lower == TEXT("time"))
            return UMaterialExpressionTime::StaticClass();
        if (Lower == TEXT("panner"))
            return UMaterialExpressionPanner::StaticClass();
        if (Lower == TEXT("texturecoordinate") || Lower == TEXT("texcoord")
            || Lower == TEXT("uv"))
            return UMaterialExpressionTextureCoordinate::StaticClass();
        if (Lower == TEXT("oneminus") || Lower == TEXT("invert"))
            return UMaterialExpressionOneMinus::StaticClass();
        if (Lower == TEXT("saturate"))
            return UMaterialExpressionSaturate::StaticClass();
        if (Lower == TEXT("clamp"))
            return UMaterialExpressionClamp::StaticClass();
        if (Lower == TEXT("fresnel"))
            return UMaterialExpressionFresnel::StaticClass();
        if (Lower == TEXT("power") || Lower == TEXT("pow"))
            return UMaterialExpressionPower::StaticClass();
        if (Lower == TEXT("sine") || Lower == TEXT("sin"))
            return UMaterialExpressionSine::StaticClass();
        if (Lower == TEXT("cosine") || Lower == TEXT("cos"))
            return UMaterialExpressionCosine::StaticClass();
        if (Lower == TEXT("componentmask") || Lower == TEXT("mask"))
            return UMaterialExpressionComponentMask::StaticClass();
        if (Lower == TEXT("if"))
            return UMaterialExpressionIf::StaticClass();
        if (Lower == TEXT("makematerialattributes") || Lower == TEXT("matattribs"))
            return UMaterialExpressionMakeMaterialAttributes::StaticClass();

        // Path-style fallback. Try the full `/Script/Engine.UMaterialExpressionFoo` path first.
        if (Norm.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Cls = FindObject<UClass>(nullptr, *Norm))
            {
                if (Cls->IsChildOf(UMaterialExpression::StaticClass()))
                {
                    return Cls;
                }
            }
        }

        // Bare class name fallback ("MaterialExpressionMultiply" or "UMaterialExpressionMultiply").
        FString CandidateName = Norm;
        if (CandidateName.StartsWith(TEXT("U")) && !CandidateName.StartsWith(TEXT("UMaterialExpression")))
        {
            // The "U" prefix is rarely typed by humans but harmless if present.
        }
        if (!CandidateName.StartsWith(TEXT("UMaterialExpression")) && !CandidateName.StartsWith(TEXT("MaterialExpression")))
        {
            CandidateName = FString(TEXT("MaterialExpression")) + CandidateName;
        }

        UClass* FoundClass = nullptr;
        ForEachObjectOfClass(UClass::StaticClass(), [&FoundClass, &CandidateName](UObject* Obj)
        {
            UClass* Cls = Cast<UClass>(Obj);
            if (!Cls) { return; }
            if (!Cls->IsChildOf(UMaterialExpression::StaticClass())) { return; }
            const FString ClsName = Cls->GetName();
            if (ClsName == CandidateName || (FString(TEXT("U")) + ClsName) == CandidateName)
            {
                FoundClass = Cls;
            }
        });
        return FoundClass;
    }

    /** Resolve a user token like "BaseColor" to the matching EMaterialProperty. */
    bool TryParseMaterialProperty(const FString& InToken, EMaterialProperty& OutProperty)
    {
        const FString T = InToken.ToLower().Replace(TEXT("_"), TEXT(""));
        if (T == TEXT("basecolor")) { OutProperty = MP_BaseColor; return true; }
        if (T == TEXT("metallic")) { OutProperty = MP_Metallic; return true; }
        if (T == TEXT("specular")) { OutProperty = MP_Specular; return true; }
        if (T == TEXT("roughness")) { OutProperty = MP_Roughness; return true; }
        if (T == TEXT("anisotropy")) { OutProperty = MP_Anisotropy; return true; }
        if (T == TEXT("emissivecolor") || T == TEXT("emissive")) { OutProperty = MP_EmissiveColor; return true; }
        if (T == TEXT("opacity")) { OutProperty = MP_Opacity; return true; }
        if (T == TEXT("opacitymask")) { OutProperty = MP_OpacityMask; return true; }
        if (T == TEXT("normal")) { OutProperty = MP_Normal; return true; }
        if (T == TEXT("tangent")) { OutProperty = MP_Tangent; return true; }
        if (T == TEXT("worldpositionoffset") || T == TEXT("wpo")) { OutProperty = MP_WorldPositionOffset; return true; }
        if (T == TEXT("ambientocclusion") || T == TEXT("ao")) { OutProperty = MP_AmbientOcclusion; return true; }
        if (T == TEXT("refraction")) { OutProperty = MP_Refraction; return true; }
        if (T == TEXT("displacement")) { OutProperty = MP_Displacement; return true; }
        return false;
    }

    /** Find a UMaterialExpression on a UMaterial by its FName (case-sensitive). */
    UMaterialExpression* FindExpressionByName(UMaterial* Material, const FString& Name)
    {
        if (!Material) { return nullptr; }
        const FName Target(*Name);
        for (UMaterialExpression* Expr : Material->GetExpressions())
        {
            if (Expr && Expr->GetFName() == Target)
            {
                return Expr;
            }
        }
        // Case-insensitive fallback.
        for (UMaterialExpression* Expr : Material->GetExpressions())
        {
            if (Expr && Expr->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Expr;
            }
        }
        return nullptr;
    }

    /** Cascade default position. We start at -300, 0 and step down 200 px
     *  per expression already on the material so a no-position add does
     *  not stack every node on top of itself. */
    void DeriveDefaultPosition(UMaterial* Material, int32& X, int32& Y)
    {
        if (!Material)
        {
            X = -300; Y = 0;
            return;
        }
        int32 Count = 0;
        for (UMaterialExpression* Expr : Material->GetExpressions())
        {
            if (Expr) { ++Count; }
        }
        X = -300;
        Y = 200 * Count;
    }

    /** Apply a flat property dict through FProperty::ImportText. Returns
     *  the count of properties applied; appends per-property errors to
     *  OutErrors. */
    int32 MaterialEdit_ApplyPropertyDict(UObject* Object, const TSharedPtr<FJsonObject>& Properties, TArray<FString>& OutErrors)
    {
        if (!Object || !Properties.IsValid()) { return 0; }
        int32 Applied = 0;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;
            FProperty* Prop = Object->GetClass()->FindPropertyByName(*PropName);
            if (!Prop)
            {
                OutErrors.Add(FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Object->GetClass()->GetName()));
                continue;
            }

            FString TextValue;
            if (JsonVal->Type == EJson::String)
            {
                TextValue = JsonVal->AsString();
            }
            else if (JsonVal->Type == EJson::Number)
            {
                TextValue = LexToString(JsonVal->AsNumber());
            }
            else if (JsonVal->Type == EJson::Boolean)
            {
                TextValue = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
            }
            else if (JsonVal->Type == EJson::Array)
            {
                // Render arrays through FJsonSerializer so structs like
                // FLinearColor accept "(R=1.0,G=0.5,B=0.5,A=1.0)" and
                // arrays of numbers stringify as "1.0,0.5,0.5,1.0".
                FStructProperty* StructProp = CastField<FStructProperty>(Prop);
                if (StructProp && StructProp->Struct == TBaseStructure<FLinearColor>::Get())
                {
                    FLinearColor Color;
                    if (TryParseLinearColor(JsonVal, Color))
                    {
                        TextValue = FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), Color.R, Color.G, Color.B, Color.A);
                    }
                }
                if (TextValue.IsEmpty())
                {
                    const TArray<TSharedPtr<FJsonValue>>& Arr = JsonVal->AsArray();
                    TArray<FString> Parts;
                    Parts.Reserve(Arr.Num());
                    for (const TSharedPtr<FJsonValue>& V : Arr)
                    {
                        Parts.Add(V.IsValid() && V->Type == EJson::Number ? LexToString(V->AsNumber()) : TEXT(""));
                    }
                    TextValue = FString::Join(Parts, TEXT(","));
                }
            }
            else
            {
                OutErrors.Add(FString::Printf(TEXT("Unsupported JSON value type for property '%s'"), *PropName));
                continue;
            }

            void* PropPtr = Prop->ContainerPtrToValuePtr<void>(Object);
            const TCHAR* Result = Prop->ImportText_Direct(*TextValue, PropPtr, Object, PPF_None);
            if (Result == nullptr)
            {
                OutErrors.Add(FString::Printf(TEXT("ImportText failed for '%s' = '%s'"), *PropName, *TextValue));
                continue;
            }
            ++Applied;
        }
        return Applied;
    }

    TSharedPtr<FJsonObject> ExpressionToSummary(UMaterialExpression* Expr)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Expr) { return Out; }
        Out->SetStringField(TEXT("name"), Expr->GetName());
        Out->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
        Out->SetNumberField(TEXT("position_x"), Expr->MaterialExpressionEditorX);
        Out->SetNumberField(TEXT("position_y"), Expr->MaterialExpressionEditorY);
        const FName ParamName = Expr->GetParameterName();
        if (!ParamName.IsNone())
        {
            Out->SetStringField(TEXT("parameter_name"), ParamName.ToString());
        }
        return Out;
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
    if (Operation == TEXT("add_expression") || Operation == TEXT("create_expression"))
    {
        return AddExpression(Params);
    }
    if (Operation == TEXT("connect_expressions") || Operation == TEXT("connect"))
    {
        return ConnectExpressions(Params);
    }
    if (Operation == TEXT("set_expression_property") || Operation == TEXT("set_expression"))
    {
        return SetExpressionProperty(Params);
    }
    if (Operation == TEXT("add_expressions") || Operation == TEXT("bulk_add_expressions")
        || Operation == TEXT("build_graph"))
    {
        return AddExpressionsBulk(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported material_edit operation '%s'. Supported: create_material, create_material_instance_constant, set_instance_parameter, add_expression, add_expressions, connect_expressions, set_expression_property"), *Operation));
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
    MaterialEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
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
    MaterialEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
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

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(Asset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    FString ClassToken;
    if (!Params->TryGetStringField(TEXT("class"), ClassToken)
        && !Params->TryGetStringField(TEXT("expression_class"), ClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'class' parameter (e.g. 'multiply', 'lerp', 'scalar_parameter', or '/Script/Engine.UMaterialExpressionMultiply')"));
    }
    TSubclassOf<UMaterialExpression> ExprClass = ResolveExpressionClass(ClassToken);
    if (!ExprClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve material expression class '%s'"), *ClassToken));
    }

    int32 PosX = 0, PosY = 0;
    bool bExplicitPos = false;
    if (Params->HasField(TEXT("position")))
    {
        const TSharedPtr<FJsonValue> PosVal = Params->TryGetField(TEXT("position"));
        if (PosVal.IsValid() && PosVal->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = PosVal->AsArray();
            if (Arr.Num() >= 2)
            {
                PosX = static_cast<int32>(Arr[0]->AsNumber());
                PosY = static_cast<int32>(Arr[1]->AsNumber());
                bExplicitPos = true;
            }
        }
        else if (PosVal.IsValid() && PosVal->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> PosObj = PosVal->AsObject();
            double X = 0.0, Y = 0.0;
            if (PosObj.IsValid() && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
                && (PosObj->TryGetNumberField(TEXT("y"), Y) || PosObj->TryGetNumberField(TEXT("Y"), Y)))
            {
                PosX = static_cast<int32>(X);
                PosY = static_cast<int32>(Y);
                bExplicitPos = true;
            }
        }
    }
    if (!bExplicitPos)
    {
        DeriveDefaultPosition(Material, PosX, PosY);
    }

    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExprClass, PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateMaterialExpression failed for class '%s'"), *ExprClass->GetName()));
    }

    // Optional initial property dict, applied through ImportText. This
    // is how callers set ConstA / ConstB on a Multiply, R on a Constant,
    // ParameterName on a Scalar / Vector / Texture parameter, etc.
    TArray<FString> PropertyErrors;
    int32 PropertyAppliedCount = 0;
    if (Params->HasField(TEXT("properties")))
    {
        const TSharedPtr<FJsonValue> PropsVal = Params->TryGetField(TEXT("properties"));
        if (PropsVal.IsValid() && PropsVal->Type == EJson::Object)
        {
            PropertyAppliedCount = MaterialEdit_ApplyPropertyDict(NewExpr, PropsVal->AsObject(), PropertyErrors);
        }
    }

    // Optional one-shot connection.
    bool bConnectedToProperty = false;
    bool bConnectedToExpression = false;
    FString PropertyConnected;
    FString ExpressionConnected;

    FString PropertyToken;
    if (Params->TryGetStringField(TEXT("property"), PropertyToken)
        || Params->TryGetStringField(TEXT("connect_property"), PropertyToken))
    {
        EMaterialProperty MaterialProperty;
        if (!TryParseMaterialProperty(PropertyToken, MaterialProperty))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown material property token '%s'. Use BaseColor, Metallic, Roughness, EmissiveColor, etc."), *PropertyToken));
        }
        FString FromOutput;
        Params->TryGetStringField(TEXT("source_output"), FromOutput);
        if (UMaterialEditingLibrary::ConnectMaterialProperty(NewExpr, FromOutput, MaterialProperty))
        {
            bConnectedToProperty = true;
            PropertyConnected = PropertyToken;
        }
    }

    FString ConnectToToken;
    FString ConnectInputToken;
    if (Params->TryGetStringField(TEXT("connect_to"), ConnectToToken))
    {
        Params->TryGetStringField(TEXT("connect_input"), ConnectInputToken);
        UMaterialExpression* ToExpr = FindExpressionByName(Material, ConnectToToken);
        if (!ToExpr)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("connect_to expression '%s' not found on material"), *ConnectToToken));
        }
        FString FromOutput;
        Params->TryGetStringField(TEXT("source_output"), FromOutput);
        if (UMaterialEditingLibrary::ConnectMaterialExpressions(NewExpr, FromOutput, ToExpr, ConnectInputToken))
        {
            bConnectedToExpression = true;
            ExpressionConnected = ToExpr->GetName();
        }
    }

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (UPackage* Package = Material->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Material->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_expression"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetNumberField(TEXT("properties_applied"), PropertyAppliedCount);
    if (PropertyErrors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& E : PropertyErrors)
        {
            Arr.Add(MakeShared<FJsonValueString>(E));
        }
        ResultObj->SetArrayField(TEXT("property_errors"), Arr);
    }
    ResultObj->SetBoolField(TEXT("connected_to_property"), bConnectedToProperty);
    if (bConnectedToProperty)
    {
        ResultObj->SetStringField(TEXT("property"), PropertyConnected);
    }
    ResultObj->SetBoolField(TEXT("connected_to_expression"), bConnectedToExpression);
    if (bConnectedToExpression)
    {
        ResultObj->SetStringField(TEXT("connect_to"), ExpressionConnected);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::ConnectExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter"));
    }
    UMaterial* Material = Cast<UMaterial>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    FString FromName;
    Params->TryGetStringField(TEXT("source"), FromName);
    if (FromName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("from"), FromName);
    }
    if (FromName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source' (source expression FName)"));
    }
    FString FromOutput;
    Params->TryGetStringField(TEXT("source_output"), FromOutput);

    UMaterialExpression* SourceExpr = FindExpressionByName(Material, FromName);
    if (!SourceExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("source expression '%s' not found on '%s'"), *FromName, *MaterialPath));
    }

    // Two destination forms: a material attribute (property) or another expression.
    FString PropertyToken;
    bool bConnectedProperty = false;
    if (Params->TryGetStringField(TEXT("property"), PropertyToken)
        || Params->TryGetStringField(TEXT("dest_property"), PropertyToken))
    {
        EMaterialProperty MaterialProperty;
        if (!TryParseMaterialProperty(PropertyToken, MaterialProperty))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown property token '%s'"), *PropertyToken));
        }
        if (!UMaterialEditingLibrary::ConnectMaterialProperty(SourceExpr, FromOutput, MaterialProperty))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("ConnectMaterialProperty failed for property '%s'"), *PropertyToken));
        }
        bConnectedProperty = true;
    }
    else
    {
        FString ToName;
        Params->TryGetStringField(TEXT("dest"), ToName);
        if (ToName.IsEmpty())
        {
            Params->TryGetStringField(TEXT("to"), ToName);
        }
        if (ToName.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'dest' (destination expression FName) or 'property'"));
        }
        FString ToInput;
        Params->TryGetStringField(TEXT("dest_input"), ToInput);
        UMaterialExpression* DestExpr = FindExpressionByName(Material, ToName);
        if (!DestExpr)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("dest expression '%s' not found on '%s'"), *ToName, *MaterialPath));
        }
        if (!UMaterialEditingLibrary::ConnectMaterialExpressions(SourceExpr, FromOutput, DestExpr, ToInput))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("ConnectMaterialExpressions failed: %s.%s -> %s.%s"), *FromName, *FromOutput, *ToName, *ToInput));
        }
    }

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (UPackage* Package = Material->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Material->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("connect_expressions"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("source"), SourceExpr->GetName());
    ResultObj->SetStringField(TEXT("source_output"), FromOutput);
    ResultObj->SetBoolField(TEXT("to_property"), bConnectedProperty);
    if (bConnectedProperty)
    {
        ResultObj->SetStringField(TEXT("property"), PropertyToken);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetExpressionProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter"));
    }
    UMaterial* Material = Cast<UMaterial>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    FString ExpressionName;
    Params->TryGetStringField(TEXT("expression"), ExpressionName);
    if (ExpressionName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("expression_name"), ExpressionName);
    }
    if (ExpressionName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'expression' (target expression FName)"));
    }

    UMaterialExpression* TargetExpr = FindExpressionByName(Material, ExpressionName);
    if (!TargetExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("expression '%s' not found on '%s'"), *ExpressionName, *MaterialPath));
    }

    if (!Params->HasField(TEXT("properties")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'properties' object (e.g. {\"R\": 0.5, \"ParameterName\": \"Roughness\"})"));
    }
    const TSharedPtr<FJsonValue> PropsVal = Params->TryGetField(TEXT("properties"));
    if (!PropsVal.IsValid() || PropsVal->Type != EJson::Object)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'properties' must be a JSON object"));
    }

    TArray<FString> PropertyErrors;
    const int32 Applied = MaterialEdit_ApplyPropertyDict(TargetExpr, PropsVal->AsObject(), PropertyErrors);

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (UPackage* Package = Material->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Material->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_expression_property"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(TargetExpr));
    ResultObj->SetNumberField(TEXT("properties_applied"), Applied);
    if (PropertyErrors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& E : PropertyErrors)
        {
            Arr.Add(MakeShared<FJsonValueString>(E));
        }
        ResultObj->SetArrayField(TEXT("property_errors"), Arr);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddExpressionsBulk(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter"));
    }
    UMaterial* Material = Cast<UMaterial>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    const TArray<TSharedPtr<FJsonValue>>* ExpressionsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("expressions"), ExpressionsArray) || !ExpressionsArray)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'expressions' array (each entry needs at least 'class')"));
    }

    // Track every expression created in this call so a connection spec
    // can address it by either the spec's `name` field (a friendly tag)
    // or the engine's resolved FName (the `name` field in the response).
    TMap<FString, UMaterialExpression*> AliasToExpr;
    TArray<TSharedPtr<FJsonValue>> ExpressionLog;

    int32 CascadeIndex = 0;
    for (const TSharedPtr<FJsonValue>& Entry : *ExpressionsArray)
    {
        TSharedPtr<FJsonObject> EntryRow = MakeShared<FJsonObject>();
        if (!Entry.IsValid() || Entry->Type != EJson::Object)
        {
            EntryRow->SetBoolField(TEXT("success"), false);
            EntryRow->SetStringField(TEXT("error"), TEXT("entry must be an object"));
            ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
            continue;
        }
        const TSharedPtr<FJsonObject>& EntryObj = Entry->AsObject();

        FString Alias;
        EntryObj->TryGetStringField(TEXT("name"), Alias);
        if (!Alias.IsEmpty())
        {
            EntryRow->SetStringField(TEXT("alias"), Alias);
        }

        FString ClassToken;
        if (!EntryObj->TryGetStringField(TEXT("class"), ClassToken)
            && !EntryObj->TryGetStringField(TEXT("expression_class"), ClassToken))
        {
            EntryRow->SetBoolField(TEXT("success"), false);
            EntryRow->SetStringField(TEXT("error"), TEXT("missing 'class'"));
            ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
            continue;
        }
        EntryRow->SetStringField(TEXT("class_requested"), ClassToken);

        TSubclassOf<UMaterialExpression> ExprClass = ResolveExpressionClass(ClassToken);
        if (!ExprClass)
        {
            EntryRow->SetBoolField(TEXT("success"), false);
            EntryRow->SetStringField(TEXT("error"),
                FString::Printf(TEXT("Could not resolve expression class '%s'"), *ClassToken));
            ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
            continue;
        }

        int32 PosX = -300;
        int32 PosY = 200 * CascadeIndex;
        if (EntryObj->HasField(TEXT("position")))
        {
            const TSharedPtr<FJsonValue> PosVal = EntryObj->TryGetField(TEXT("position"));
            if (PosVal.IsValid() && PosVal->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = PosVal->AsArray();
                if (Arr.Num() >= 2)
                {
                    PosX = static_cast<int32>(Arr[0]->AsNumber());
                    PosY = static_cast<int32>(Arr[1]->AsNumber());
                }
            }
            else if (PosVal.IsValid() && PosVal->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> PosObj = PosVal->AsObject();
                double X = 0.0, Y = 0.0;
                if (PosObj.IsValid() && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
                    && (PosObj->TryGetNumberField(TEXT("y"), Y) || PosObj->TryGetNumberField(TEXT("Y"), Y)))
                {
                    PosX = static_cast<int32>(X);
                    PosY = static_cast<int32>(Y);
                }
            }
        }

        UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExprClass, PosX, PosY);
        if (!NewExpr)
        {
            EntryRow->SetBoolField(TEXT("success"), false);
            EntryRow->SetStringField(TEXT("error"),
                FString::Printf(TEXT("CreateMaterialExpression failed for class '%s'"), *ExprClass->GetName()));
            ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
            continue;
        }

        TArray<FString> PropertyErrors;
        int32 PropertyAppliedCount = 0;
        if (EntryObj->HasField(TEXT("properties")))
        {
            const TSharedPtr<FJsonValue> PropsVal = EntryObj->TryGetField(TEXT("properties"));
            if (PropsVal.IsValid() && PropsVal->Type == EJson::Object)
            {
                PropertyAppliedCount = MaterialEdit_ApplyPropertyDict(NewExpr, PropsVal->AsObject(), PropertyErrors);
            }
        }

        if (!Alias.IsEmpty())
        {
            AliasToExpr.Add(Alias, NewExpr);
        }
        // Index the resolved name too so a downstream connection spec
        // can reference the expression by either alias or actual FName.
        AliasToExpr.Add(NewExpr->GetName(), NewExpr);

        EntryRow->SetBoolField(TEXT("success"), true);
        EntryRow->SetStringField(TEXT("name"), NewExpr->GetName());
        EntryRow->SetStringField(TEXT("class"), NewExpr->GetClass()->GetName());
        EntryRow->SetNumberField(TEXT("position_x"), NewExpr->MaterialExpressionEditorX);
        EntryRow->SetNumberField(TEXT("position_y"), NewExpr->MaterialExpressionEditorY);
        EntryRow->SetNumberField(TEXT("properties_applied"), PropertyAppliedCount);
        if (PropertyErrors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FString& E : PropertyErrors)
            {
                Arr.Add(MakeShared<FJsonValueString>(E));
            }
            EntryRow->SetArrayField(TEXT("property_errors"), Arr);
        }
        ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
        ++CascadeIndex;
    }

    // Optional list of edges. Each edge can be either expression-to-
    // expression (`{source, source_output?, dest, dest_input?}`) or
    // expression-to-material-attribute (`{source, property}`).
    TArray<TSharedPtr<FJsonValue>> ConnectionLog;
    const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("connections"), ConnectionsArray) && ConnectionsArray)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *ConnectionsArray)
        {
            TSharedPtr<FJsonObject> EntryRow = MakeShared<FJsonObject>();
            if (!Entry.IsValid() || Entry->Type != EJson::Object)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("entry must be an object"));
                ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            const TSharedPtr<FJsonObject>& EntryObj = Entry->AsObject();

            FString Source;
            if (!EntryObj->TryGetStringField(TEXT("source"), Source)
                && !EntryObj->TryGetStringField(TEXT("from"), Source))
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("missing 'source'"));
                ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            UMaterialExpression* SourceExpr = nullptr;
            if (UMaterialExpression** Found = AliasToExpr.Find(Source))
            {
                SourceExpr = *Found;
            }
            if (!SourceExpr)
            {
                SourceExpr = FindExpressionByName(Material, Source);
            }
            if (!SourceExpr)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"),
                    FString::Printf(TEXT("source expression '%s' not found"), *Source));
                ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            EntryRow->SetStringField(TEXT("source"), SourceExpr->GetName());

            FString SourceOutput;
            EntryObj->TryGetStringField(TEXT("source_output"), SourceOutput);
            EntryRow->SetStringField(TEXT("source_output"), SourceOutput);

            FString PropertyToken;
            if (EntryObj->TryGetStringField(TEXT("property"), PropertyToken)
                || EntryObj->TryGetStringField(TEXT("dest_property"), PropertyToken))
            {
                EMaterialProperty MaterialProperty;
                if (!TryParseMaterialProperty(PropertyToken, MaterialProperty))
                {
                    EntryRow->SetBoolField(TEXT("success"), false);
                    EntryRow->SetStringField(TEXT("error"),
                        FString::Printf(TEXT("Unknown property token '%s'"), *PropertyToken));
                    ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                    continue;
                }
                if (UMaterialEditingLibrary::ConnectMaterialProperty(SourceExpr, SourceOutput, MaterialProperty))
                {
                    EntryRow->SetBoolField(TEXT("success"), true);
                    EntryRow->SetStringField(TEXT("property"), PropertyToken);
                }
                else
                {
                    EntryRow->SetBoolField(TEXT("success"), false);
                    EntryRow->SetStringField(TEXT("error"),
                        FString::Printf(TEXT("ConnectMaterialProperty failed for '%s'"), *PropertyToken));
                }
                ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }

            FString Dest;
            if (!EntryObj->TryGetStringField(TEXT("dest"), Dest)
                && !EntryObj->TryGetStringField(TEXT("to"), Dest))
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("missing 'dest' or 'property'"));
                ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            UMaterialExpression* DestExpr = nullptr;
            if (UMaterialExpression** Found = AliasToExpr.Find(Dest))
            {
                DestExpr = *Found;
            }
            if (!DestExpr)
            {
                DestExpr = FindExpressionByName(Material, Dest);
            }
            if (!DestExpr)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"),
                    FString::Printf(TEXT("dest expression '%s' not found"), *Dest));
                ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            EntryRow->SetStringField(TEXT("dest"), DestExpr->GetName());

            FString DestInput;
            EntryObj->TryGetStringField(TEXT("dest_input"), DestInput);
            EntryRow->SetStringField(TEXT("dest_input"), DestInput);

            if (UMaterialEditingLibrary::ConnectMaterialExpressions(SourceExpr, SourceOutput, DestExpr, DestInput))
            {
                EntryRow->SetBoolField(TEXT("success"), true);
            }
            else
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"),
                    FString::Printf(TEXT("ConnectMaterialExpressions failed: %s.%s -> %s.%s"),
                        *SourceExpr->GetName(), *SourceOutput, *DestExpr->GetName(), *DestInput));
            }
            ConnectionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
        }
    }

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);
    if (bRecompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (UPackage* Package = Material->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Material->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    int32 ExpressionsCreated = 0;
    int32 ExpressionFailures = 0;
    for (const TSharedPtr<FJsonValue>& V : ExpressionLog)
    {
        if (V.IsValid() && V->Type == EJson::Object && V->AsObject().IsValid())
        {
            bool bOk = false;
            V->AsObject()->TryGetBoolField(TEXT("success"), bOk);
            if (bOk) { ++ExpressionsCreated; } else { ++ExpressionFailures; }
        }
    }
    int32 ConnectionsApplied = 0;
    int32 ConnectionFailures = 0;
    for (const TSharedPtr<FJsonValue>& V : ConnectionLog)
    {
        if (V.IsValid() && V->Type == EJson::Object && V->AsObject().IsValid())
        {
            bool bOk = false;
            V->AsObject()->TryGetBoolField(TEXT("success"), bOk);
            if (bOk) { ++ConnectionsApplied; } else { ++ConnectionFailures; }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_expressions"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetArrayField(TEXT("expressions"), ExpressionLog);
    ResultObj->SetArrayField(TEXT("connections"), ConnectionLog);
    ResultObj->SetNumberField(TEXT("expressions_created"), ExpressionsCreated);
    ResultObj->SetNumberField(TEXT("expression_failures"), ExpressionFailures);
    ResultObj->SetNumberField(TEXT("connections_applied"), ConnectionsApplied);
    ResultObj->SetNumberField(TEXT("connection_failures"), ConnectionFailures);
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}
