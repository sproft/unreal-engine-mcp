#include "Commands/SproftMaterialEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/Texture2DArray.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureSampleParameter2DArray.h"
#include "Materials/MaterialExpressionTextureSampleParameterCube.h"
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
        if (Lower == TEXT("functioninput") || Lower == TEXT("input"))
            return UMaterialExpressionFunctionInput::StaticClass();
        if (Lower == TEXT("functionoutput") || Lower == TEXT("output"))
            return UMaterialExpressionFunctionOutput::StaticClass();

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
    if (Operation == TEXT("create_parameter_collection") || Operation == TEXT("create_mpc")
        || Operation == TEXT("create_collection"))
    {
        return CreateParameterCollection(Params);
    }
    if (Operation == TEXT("add_collection_parameter") || Operation == TEXT("add_mpc_parameter")
        || Operation == TEXT("add_parameter"))
    {
        return AddCollectionParameter(Params);
    }
    if (Operation == TEXT("create_material_function") || Operation == TEXT("create_function")
        || Operation == TEXT("create_mf"))
    {
        return CreateMaterialFunction(Params);
    }
    if (Operation == TEXT("add_function_call") || Operation == TEXT("add_material_function_call")
        || Operation == TEXT("add_function"))
    {
        return AddFunctionCall(Params);
    }
    if (Operation == TEXT("set_attribute_blendable") || Operation == TEXT("set_base_property_override")
        || Operation == TEXT("set_override"))
    {
        return SetAttributeBlendable(Params);
    }
    if (Operation == TEXT("add_texture_sample") || Operation == TEXT("add_texture")
        || Operation == TEXT("texture_sample"))
    {
        return AddTextureSample(Params);
    }
    if (Operation == TEXT("add_texture_sample_cube") || Operation == TEXT("add_cube_texture_sample")
        || Operation == TEXT("add_texture_cube") || Operation == TEXT("add_cube_sample")
        || Operation == TEXT("texture_sample_cube"))
    {
        return AddTextureSampleCube(Params);
    }
    if (Operation == TEXT("add_2d_array_sample") || Operation == TEXT("add_texture_2d_array_sample")
        || Operation == TEXT("add_texture_array_sample") || Operation == TEXT("texture_2d_array_sample")
        || Operation == TEXT("add_array_sample") || Operation == TEXT("add_texture_2darray"))
    {
        return AddTexture2DArraySample(Params);
    }
    if (Operation == TEXT("add_constant") || Operation == TEXT("add_const")
        || Operation == TEXT("add_value") || Operation == TEXT("add_scalar"))
    {
        return AddConstant(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported material_edit operation '%s'. Supported: create_material, create_material_instance_constant, set_instance_parameter, add_expression, add_expressions, connect_expressions, set_expression_property, create_parameter_collection, add_collection_parameter, create_material_function, add_function_call, set_attribute_blendable, add_texture_sample, add_texture_sample_cube, add_2d_array_sample, add_constant"), *Operation));
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

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::CreateParameterCollection(const TSharedPtr<FJsonObject>& Params)
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

    UMaterialParameterCollectionFactoryNew* Factory = NewObject<UMaterialParameterCollectionFactoryNew>();
    UMaterialParameterCollection* NewMPC = Cast<UMaterialParameterCollection>(Factory->FactoryCreateNew(
        UMaterialParameterCollection::StaticClass(),
        Package,
        *AssetName,
        RF_Public | RF_Standalone | RF_Transactional,
        nullptr,
        GWarn));
    if (!NewMPC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMaterialParameterCollection"));
    }

    FAssetRegistryModule::AssetCreated(NewMPC);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_parameter_collection"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetNumberField(TEXT("scalar_parameter_count"), NewMPC->ScalarParameters.Num());
    ResultObj->SetNumberField(TEXT("vector_parameter_count"), NewMPC->VectorParameters.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (!Params->TryGetStringField(TEXT("collection"), CollectionPath)
        && !Params->TryGetStringField(TEXT("parameter_collection"), CollectionPath)
        && !Params->TryGetStringField(TEXT("mpc"), CollectionPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'collection' parameter (path to a UMaterialParameterCollection)"));
    }

    FString ParameterName;
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName)
        && !Params->TryGetStringField(TEXT("name"), ParameterName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }

    FString ParameterTypeText;
    if (!Params->TryGetStringField(TEXT("parameter_type"), ParameterTypeText)
        && !Params->TryGetStringField(TEXT("type"), ParameterTypeText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_type' parameter (scalar / float / vector / color)"));
    }
    const FString TypeLower = ParameterTypeText.ToLower();
    const bool bIsScalar = (TypeLower == TEXT("scalar") || TypeLower == TEXT("float"));
    const bool bIsVector = (TypeLower == TEXT("vector") || TypeLower == TEXT("color")
                            || TypeLower == TEXT("linear_color") || TypeLower == TEXT("linearcolor"));
    if (!bIsScalar && !bIsVector)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported parameter_type '%s'. Use 'scalar' / 'float' or 'vector' / 'color'."), *ParameterTypeText));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* CollectionAsset = UEditorAssetLibrary::LoadAsset(CollectionPath);
    UMaterialParameterCollection* MPC = Cast<UMaterialParameterCollection>(CollectionAsset);
    if (!MPC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UMaterialParameterCollection: %s"), *CollectionPath));
    }

    const FName ParamFName(*ParameterName);

    // Refuse a duplicate name across both arrays. The asset's
    // PostEditChangeProperty has SanitizeParameters that auto-renames
    // duplicates, but emitting a clear error keeps the response surface
    // predictable for callers chaining several add_collection_parameter
    // calls.
    if (MPC->GetScalarParameterIndexByName(ParamFName) != INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Scalar parameter '%s' already exists on %s"), *ParameterName, *CollectionPath));
    }
    if (MPC->GetVectorParameterIndexByName(ParamFName) != INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Vector parameter '%s' already exists on %s"), *ParameterName, *CollectionPath));
    }

    // Cache the previous storage total so PostEditChangeProperty's
    // "if storage grew, regenerate StateId + recompile referencing
    // materials" branch fires. We trigger PreEditChange on a synthesized
    // FProperty pointer so the asset's bookkeeping reads the previous
    // total before we mutate the array.
    FProperty* TargetProperty = bIsScalar
        ? UMaterialParameterCollection::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters))
        : UMaterialParameterCollection::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));
    if (!TargetProperty)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve ScalarParameters / VectorParameters UPROPERTY on the parameter collection class"));
    }

    MPC->PreEditChange(TargetProperty);

    if (bIsScalar)
    {
        FCollectionScalarParameter NewParam;
        NewParam.ParameterName = ParamFName;
        NewParam.Id = FGuid::NewGuid();
        NewParam.DefaultValue = 0.0f;
        if (Params->HasField(TEXT("value")))
        {
            const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
            if (ValueField.IsValid() && ValueField->Type == EJson::Number)
            {
                NewParam.DefaultValue = static_cast<float>(ValueField->AsNumber());
            }
        }
        MPC->ScalarParameters.Add(NewParam);
    }
    else
    {
        FCollectionVectorParameter NewParam;
        NewParam.ParameterName = ParamFName;
        NewParam.Id = FGuid::NewGuid();
        NewParam.DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
        if (Params->HasField(TEXT("value")))
        {
            const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
            FLinearColor ParsedColor;
            if (TryParseLinearColor(ValueField, ParsedColor))
            {
                NewParam.DefaultValue = ParsedColor;
            }
        }
        MPC->VectorParameters.Add(NewParam);
    }

    // PostEditChangeProperty rebuilds the asset's uniform buffer layout
    // and regenerates StateId when total vector storage changes. It also
    // walks every loaded UMaterial referencing this collection and
    // requeues a recompile through FMaterialUpdateContext, which is the
    // canonical "I just changed an MPC, materials need to know" broadcast.
    FPropertyChangedEvent ChangeEvent(TargetProperty, EPropertyChangeType::ArrayAdd);
    MPC->PostEditChangeProperty(ChangeEvent);

    if (UPackage* Package = MPC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(CollectionPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_collection_parameter"));
    ResultObj->SetStringField(TEXT("collection"), MPC->GetPathName());
    ResultObj->SetStringField(TEXT("parameter_name"), ParameterName);
    ResultObj->SetStringField(TEXT("parameter_type"), bIsScalar ? TEXT("scalar") : TEXT("vector"));
    ResultObj->SetNumberField(TEXT("scalar_parameter_count"), MPC->ScalarParameters.Num());
    ResultObj->SetNumberField(TEXT("vector_parameter_count"), MPC->VectorParameters.Num());
    if (bIsScalar)
    {
        const int32 Idx = MPC->GetScalarParameterIndexByName(ParamFName);
        if (MPC->ScalarParameters.IsValidIndex(Idx))
        {
            ResultObj->SetNumberField(TEXT("default_value"), MPC->ScalarParameters[Idx].DefaultValue);
        }
    }
    else
    {
        const int32 Idx = MPC->GetVectorParameterIndexByName(ParamFName);
        if (MPC->VectorParameters.IsValidIndex(Idx))
        {
            const FLinearColor& C = MPC->VectorParameters[Idx].DefaultValue;
            TArray<TSharedPtr<FJsonValue>> ColorArr;
            ColorArr.Add(MakeShared<FJsonValueNumber>(C.R));
            ColorArr.Add(MakeShared<FJsonValueNumber>(C.G));
            ColorArr.Add(MakeShared<FJsonValueNumber>(C.B));
            ColorArr.Add(MakeShared<FJsonValueNumber>(C.A));
            ResultObj->SetArrayField(TEXT("default_value"), ColorArr);
        }
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::CreateMaterialFunction(const TSharedPtr<FJsonObject>& Params)
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

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);

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

    UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
    UMaterialFunction* NewFunction = Cast<UMaterialFunction>(Factory->FactoryCreateNew(
        UMaterialFunction::StaticClass(),
        Package,
        *AssetName,
        RF_Public | RF_Standalone | RF_Transactional,
        nullptr,
        GWarn));
    if (!NewFunction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMaterialFunction"));
    }

    // Optional initial expression list. Same shape as `add_expressions`:
    // each entry carries `class` plus optional `name` alias / `position`
    // (cascades down by 200px per entry when omitted) / `properties`
    // dict (applied through FProperty::ImportText).
    TArray<TSharedPtr<FJsonValue>> ExpressionLog;
    int32 ExpressionsCreated = 0;
    int32 ExpressionFailures = 0;

    const TArray<TSharedPtr<FJsonValue>>* ExpressionsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("expressions"), ExpressionsArray)
        || Params->TryGetArrayField(TEXT("initial_expressions"), ExpressionsArray))
    {
        int32 CascadeIndex = 0;
        for (const TSharedPtr<FJsonValue>& Entry : *ExpressionsArray)
        {
            TSharedPtr<FJsonObject> EntryRow = MakeShared<FJsonObject>();
            if (!Entry.IsValid() || Entry->Type != EJson::Object)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("entry must be an object"));
                ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                ++ExpressionFailures;
                continue;
            }
            const TSharedPtr<FJsonObject>& EntryObj = Entry->AsObject();

            FString ClassToken;
            if (!EntryObj->TryGetStringField(TEXT("class"), ClassToken)
                && !EntryObj->TryGetStringField(TEXT("expression_class"), ClassToken))
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("missing 'class'"));
                ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                ++ExpressionFailures;
                continue;
            }
            EntryRow->SetStringField(TEXT("class_token"), ClassToken);

            FString Alias;
            EntryObj->TryGetStringField(TEXT("name"), Alias);
            EntryObj->TryGetStringField(TEXT("alias"), Alias);

            TSubclassOf<UMaterialExpression> ExprClass = ResolveExpressionClass(ClassToken);
            if (!ExprClass)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"),
                    FString::Printf(TEXT("Could not resolve material expression class '%s'"), *ClassToken));
                ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                ++ExpressionFailures;
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

            UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                NewFunction, ExprClass, PosX, PosY);
            if (!NewExpr)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"),
                    FString::Printf(TEXT("CreateMaterialExpressionInFunction failed for class '%s'"), *ExprClass->GetName()));
                ExpressionLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                ++ExpressionFailures;
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

            EntryRow->SetBoolField(TEXT("success"), true);
            if (!Alias.IsEmpty())
            {
                EntryRow->SetStringField(TEXT("alias"), Alias);
            }
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
            ++ExpressionsCreated;
            ++CascadeIndex;
        }
    }

    FAssetRegistryModule::AssetCreated(NewFunction);
    Package->MarkPackageDirty();

    // UpdateMaterialFunction recompiles every UMaterial that already
    // references this function and refreshes the function's preview
    // graph so the asset opens cleanly in the Material Function editor.
    // Skip when the caller passed `recompile=false` to keep the create
    // call cheap on a freshly-spawned function with no consumers yet.
    if (bRecompile)
    {
        UMaterialEditingLibrary::UpdateMaterialFunction(NewFunction, /*PreviewMaterial=*/nullptr);
    }

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_material_function"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetNumberField(TEXT("expression_count"),
        UMaterialEditingLibrary::GetNumMaterialExpressionsInFunction(NewFunction));
    ResultObj->SetArrayField(TEXT("expressions"), ExpressionLog);
    ResultObj->SetNumberField(TEXT("expressions_created"), ExpressionsCreated);
    ResultObj->SetNumberField(TEXT("expression_failures"), ExpressionFailures);
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }
    UObject* MaterialAsset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(MaterialAsset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    FString FunctionToken;
    if (!Params->TryGetStringField(TEXT("function"), FunctionToken)
        && !Params->TryGetStringField(TEXT("function_path"), FunctionToken)
        && !Params->TryGetStringField(TEXT("material_function"), FunctionToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function' parameter (path to a UMaterialFunctionInterface)"));
    }
    UObject* FunctionAsset = UEditorAssetLibrary::LoadAsset(FunctionToken);
    UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(FunctionAsset);
    if (!Function)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterialFunctionInterface"), *FunctionToken));
    }

    // Position: explicit `position` field as [x, y] or {x, y}; default
    // cascades through the same helper add_expression uses.
    int32 PosX = 0;
    int32 PosY = 0;
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
            double X = 0.0;
            double Y = 0.0;
            if (PosObj.IsValid()
                && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
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

    // Spawn the function-call expression through the editor library so
    // the asset's expression list, package, and editor view stay
    // consistent. The library handles MaterialAttributes wiring,
    // expression list tracking, and PostEditChange downstream of the
    // create call.
    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression failed for UMaterialExpressionMaterialFunctionCall"));
    }
    UMaterialExpressionMaterialFunctionCall* CallExpr = Cast<UMaterialExpressionMaterialFunctionCall>(NewExpr);
    if (!CallExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression returned a non-function-call expression"));
    }

    // SetMaterialFunction is the public BlueprintCallable entry point
    // (ENGINE_API on MaterialExpressionMaterialFunctionCall.h line 157)
    // that wires the function reference and rebuilds the
    // FunctionInputs / FunctionOutputs arrays from the bound function's
    // declared input / output pins. Without that step the call node
    // renders without pins and downstream connect_expressions calls
    // have no input names to target.
    if (!CallExpr->SetMaterialFunction(Function))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("SetMaterialFunction failed for function '%s' on material '%s'"),
                *Function->GetPathName(), *Material->GetPathName()));
    }

    // Optional rename so the function-call node gets a designer-readable
    // FName the way add_expression's `name` field handles it. The
    // editor library's CreateMaterialExpression overload does not
    // accept a name arg, so we rename in place.
    FString DesiredName;
    if (Params->TryGetStringField(TEXT("name"), DesiredName) && !DesiredName.IsEmpty())
    {
        NewExpr->Rename(*DesiredName, nullptr, REN_DontCreateRedirectors);
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_function_call"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("function"), Function->GetPathName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetNumberField(TEXT("input_count"), CallExpr->FunctionInputs.Num());
    ResultObj->SetNumberField(TEXT("output_count"), CallExpr->FunctionOutputs.Num());
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

namespace
{
    /** Resolve a user-supplied attribute token to its paired
     *  bOverride_X / payload property names on
     *  FMaterialInstanceBasePropertyOverrides. The token list mirrors
     *  the editor's Material Instance details panel. */
    struct FBasePropertyOverrideSlot
    {
        FString OverrideField;
        FString PayloadField;
    };

    bool ResolveBasePropertyOverrideSlot(const FString& InToken, FBasePropertyOverrideSlot& OutSlot)
    {
        const FString T = InToken.ToLower().Replace(TEXT("_"), TEXT(""));
        // Order mirrors FMaterialInstanceBasePropertyOverrides field order.
        if (T == TEXT("blendmode"))
        {
            OutSlot = { TEXT("bOverride_BlendMode"), TEXT("BlendMode") };
            return true;
        }
        if (T == TEXT("shadingmodel"))
        {
            OutSlot = { TEXT("bOverride_ShadingModel"), TEXT("ShadingModel") };
            return true;
        }
        if (T == TEXT("opacitymaskclipvalue") || T == TEXT("opacityclip"))
        {
            OutSlot = { TEXT("bOverride_OpacityMaskClipValue"), TEXT("OpacityMaskClipValue") };
            return true;
        }
        if (T == TEXT("ditheredlodtransition") || T == TEXT("dithered"))
        {
            OutSlot = { TEXT("bOverride_DitheredLODTransition"), TEXT("DitheredLODTransition") };
            return true;
        }
        if (T == TEXT("castdynamicshadowasmasked") || T == TEXT("castshadowasmasked"))
        {
            OutSlot = { TEXT("bOverride_CastDynamicShadowAsMasked"), TEXT("bCastDynamicShadowAsMasked") };
            return true;
        }
        if (T == TEXT("twosided"))
        {
            OutSlot = { TEXT("bOverride_TwoSided"), TEXT("TwoSided") };
            return true;
        }
        if (T == TEXT("isthinsurface") || T == TEXT("thinsurface"))
        {
            OutSlot = { TEXT("bOverride_bIsThinSurface"), TEXT("bIsThinSurface") };
            return true;
        }
        if (T == TEXT("outputtranslucentvelocity") || T == TEXT("translucentvelocity"))
        {
            OutSlot = { TEXT("bOverride_OutputTranslucentVelocity"), TEXT("bOutputTranslucentVelocity") };
            return true;
        }
        if (T == TEXT("haspixelanimation") || T == TEXT("pixelanimation"))
        {
            OutSlot = { TEXT("bOverride_bHasPixelAnimation"), TEXT("bHasPixelAnimation") };
            return true;
        }
        if (T == TEXT("enabletessellation") || T == TEXT("tessellation"))
        {
            OutSlot = { TEXT("bOverride_bEnableTessellation"), TEXT("bEnableTessellation") };
            return true;
        }
        if (T == TEXT("displacementscaling"))
        {
            OutSlot = { TEXT("bOverride_DisplacementScaling"), TEXT("DisplacementScaling") };
            return true;
        }
        if (T == TEXT("enabledisplacementfade") || T == TEXT("displacementfade"))
        {
            OutSlot = { TEXT("bOverride_bEnableDisplacementFade"), TEXT("bEnableDisplacementFade") };
            return true;
        }
        if (T == TEXT("displacementfaderange"))
        {
            OutSlot = { TEXT("bOverride_DisplacementFadeRange"), TEXT("DisplacementFadeRange") };
            return true;
        }
        if (T == TEXT("maxworldpositionoffsetdisplacement") || T == TEXT("maxwpodisplacement")
            || T == TEXT("maxwpo"))
        {
            OutSlot = { TEXT("bOverride_MaxWorldPositionOffsetDisplacement"),
                        TEXT("MaxWorldPositionOffsetDisplacement") };
            return true;
        }
        if (T == TEXT("compatiblewithlumencardsharing") || T == TEXT("lumencardsharing"))
        {
            OutSlot = { TEXT("bOverride_CompatibleWithLumenCardSharing"),
                        TEXT("bCompatibleWithLumenCardSharing") };
            return true;
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetAttributeBlendable(const TSharedPtr<FJsonObject>& Params)
{
    FString InstancePath;
    if (!Params->TryGetStringField(TEXT("material_instance"), InstancePath)
        && !Params->TryGetStringField(TEXT("instance"), InstancePath)
        && !Params->TryGetStringField(TEXT("material"), InstancePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material_instance' parameter (path to a UMaterialInstanceConstant)"));
    }

    UObject* InstanceAsset = UEditorAssetLibrary::LoadAsset(InstancePath);
    UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(InstanceAsset);
    if (!MIC)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterialInstanceConstant"), *InstancePath));
    }

    FString AttributeToken;
    if (!Params->TryGetStringField(TEXT("attribute"), AttributeToken)
        && !Params->TryGetStringField(TEXT("name"), AttributeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'attribute' parameter (e.g. 'blend_mode', 'two_sided', 'shading_model')"));
    }

    FBasePropertyOverrideSlot Slot;
    if (!ResolveBasePropertyOverrideSlot(AttributeToken, Slot))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown attribute '%s'. Supported: blend_mode, shading_model, opacity_mask_clip_value, dithered_lod_transition, cast_dynamic_shadow_as_masked, two_sided, is_thin_surface, output_translucent_velocity, has_pixel_animation, enable_tessellation, displacement_scaling, enable_displacement_fade, displacement_fade_range, max_world_position_offset_displacement, compatible_with_lumen_card_sharing"),
                *AttributeToken));
    }

    // Default `enabled` to true: the common case is "I want to flip this
    // override on" with the payload value supplied alongside. Callers can
    // pass `enabled=false` to clear the override without changing the
    // payload field.
    bool bEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);

    // Locate the UScriptStruct fields by name on the live MIC. The
    // BasePropertyOverrides UPROPERTY is a struct, so we reflect into it
    // through the outer property to land FProperty::ImportText for the
    // payload write.
    FProperty* OuterStructProp = MIC->GetClass()->FindPropertyByName(TEXT("BasePropertyOverrides"));
    FStructProperty* StructProp = CastField<FStructProperty>(OuterStructProp);
    if (!StructProp || !StructProp->Struct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Reflection database does not expose BasePropertyOverrides on UMaterialInstance"));
    }
    void* StructContainer = StructProp->ContainerPtrToValuePtr<void>(MIC);

    FProperty* OverrideProp = StructProp->Struct->FindPropertyByName(*Slot.OverrideField);
    if (!OverrideProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("BasePropertyOverrides has no field '%s'"), *Slot.OverrideField));
    }
    FBoolProperty* OverrideBoolProp = CastField<FBoolProperty>(OverrideProp);
    if (!OverrideBoolProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Field '%s' is not a bool"), *Slot.OverrideField));
    }

    // Walk PreEditChange / PostEditChange around the writes so the MIC's
    // shader-resource state regenerates correctly. The MIC's own
    // PostEditChangeProperty invokes UpdateStaticPermutation, which is
    // what the editor UI does on the override checkbox toggle path.
    MIC->PreEditChange(StructProp);

    // Write the bOverride_X flag through the FBoolProperty so bitfield
    // packing stays correct (the override flags are uint8 bit-1 fields).
    OverrideBoolProp->SetPropertyValue_InContainer(StructContainer, bEnabled);

    // Optional payload write. We accept JSON number / bool / string /
    // array / object so callers can land BlendMode = "BLEND_Masked",
    // ShadingModel = "MSM_Unlit", OpacityMaskClipValue = 0.333,
    // TwoSided = true, etc. in the same call.
    FString PayloadAppliedAs;
    bool bPayloadApplied = false;
    if (Params->HasField(TEXT("value")))
    {
        FProperty* PayloadProp = StructProp->Struct->FindPropertyByName(*Slot.PayloadField);
        if (!PayloadProp)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("BasePropertyOverrides has no payload field '%s'"), *Slot.PayloadField));
        }
        const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
        FString TextValue;
        if (ValueField->Type == EJson::String)
        {
            TextValue = ValueField->AsString();
            PayloadAppliedAs = TEXT("string");
        }
        else if (ValueField->Type == EJson::Number)
        {
            TextValue = LexToString(ValueField->AsNumber());
            PayloadAppliedAs = TEXT("number");
        }
        else if (ValueField->Type == EJson::Boolean)
        {
            TextValue = ValueField->AsBool() ? TEXT("true") : TEXT("false");
            PayloadAppliedAs = TEXT("bool");
        }
        else if (ValueField->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
            TArray<FString> Parts;
            Parts.Reserve(Arr.Num());
            for (const TSharedPtr<FJsonValue>& V : Arr)
            {
                Parts.Add(V.IsValid() && V->Type == EJson::Number ? LexToString(V->AsNumber()) : TEXT(""));
            }
            TextValue = FString::Join(Parts, TEXT(","));
            PayloadAppliedAs = TEXT("array");
        }
        else if (ValueField->Type == EJson::Object)
        {
            // Render the object as a struct literal for FProperty::ImportText.
            const TSharedPtr<FJsonObject>& Obj = ValueField->AsObject();
            if (!Obj.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("'value' object is empty"));
            }
            TArray<FString> Parts;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
            {
                if (!Pair.Value.IsValid())
                {
                    continue;
                }
                FString Inner;
                if (Pair.Value->Type == EJson::String)
                {
                    Inner = Pair.Value->AsString();
                }
                else if (Pair.Value->Type == EJson::Number)
                {
                    Inner = LexToString(Pair.Value->AsNumber());
                }
                else if (Pair.Value->Type == EJson::Boolean)
                {
                    Inner = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
                }
                Parts.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Inner));
            }
            TextValue = FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
            PayloadAppliedAs = TEXT("object");
        }
        else
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Unsupported JSON type for 'value'"));
        }

        void* PayloadPtr = PayloadProp->ContainerPtrToValuePtr<void>(StructContainer);
        const TCHAR* ImportResult = PayloadProp->ImportText_Direct(*TextValue, PayloadPtr, MIC, PPF_None);
        if (ImportResult == nullptr)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("ImportText failed for '%s' = '%s'"), *Slot.PayloadField, *TextValue));
        }
        bPayloadApplied = true;
    }

    // Trigger the MIC's own PostEditChangeProperty. The MIC code path
    // calls UpdateStaticPermutation under the hood when an override
    // toggle changes, which recompiles the static permutation shaders
    // for the new BlendMode / ShadingModel / etc.
    FPropertyChangedEvent ChangeEvent(StructProp, EPropertyChangeType::ValueSet);
    MIC->PostEditChangeProperty(ChangeEvent);

    // Belt-and-braces: explicitly run UpdateOverridableBaseProperties so
    // the cached fields (BlendMode / TwoSided / ShadingModel / ...) on
    // the parent material's renderer-side override slot stay in sync.
    MIC->UpdateOverridableBaseProperties();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (UPackage* Package = MIC->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(MIC->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_attribute_blendable"));
    ResultObj->SetStringField(TEXT("material_instance"), MIC->GetPathName());
    ResultObj->SetStringField(TEXT("attribute"), AttributeToken);
    ResultObj->SetStringField(TEXT("override_field"), Slot.OverrideField);
    ResultObj->SetStringField(TEXT("payload_field"), Slot.PayloadField);
    ResultObj->SetBoolField(TEXT("enabled"), bEnabled);
    ResultObj->SetBoolField(TEXT("payload_applied"), bPayloadApplied);
    if (bPayloadApplied)
    {
        ResultObj->SetStringField(TEXT("payload_kind"), PayloadAppliedAs);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

namespace
{
    /** Resolve a UTexture asset from a `/Game/...` path or short name.
     *  Short names go through the asset registry's UTexture index;
     *  any subclass of UTexture passes. */
    UTexture* MaterialEdit_ResolveTexture(const FString& Token)
    {
        if (Token.IsEmpty()) { return nullptr; }
        if (Token.StartsWith(TEXT("/")))
        {
            return Cast<UTexture>(UEditorAssetLibrary::LoadAsset(Token));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UTexture::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Token, ESearchCase::IgnoreCase))
            {
                if (UTexture* T = Cast<UTexture>(Data.GetAsset()))
                {
                    return T;
                }
            }
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddTextureSample(const TSharedPtr<FJsonObject>& Params)
{
    // Resolve the host material.
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }
    UObject* MaterialAsset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(MaterialAsset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    // Resolve the texture asset.
    FString TextureToken;
    if (!Params->TryGetStringField(TEXT("texture"), TextureToken)
        && !Params->TryGetStringField(TEXT("texture_path"), TextureToken)
        && !Params->TryGetStringField(TEXT("texture_asset"), TextureToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'texture' parameter (/Game/... path or unique short name)"));
    }
    UTexture* Texture = MaterialEdit_ResolveTexture(TextureToken);
    if (!Texture)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UTexture '%s' (pass a /Game/... path or a unique short name)"), *TextureToken));
    }

    // Position cascade through the same helper add_expression /
    // add_function_call use. Accepts `position` as either [x, y] or
    // {x, y}; otherwise the default cascade picks a spot to the left
    // of any existing expression.
    int32 PosX = 0;
    int32 PosY = 0;
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
            double X = 0.0;
            double Y = 0.0;
            if (PosObj.IsValid()
                && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
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

    // Spawn the texture-sample expression through the editor library so
    // the asset's expression list, package, and editor view stay
    // consistent.
    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionTextureSample::StaticClass(), PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression failed for UMaterialExpressionTextureSample"));
    }
    UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(NewExpr);
    if (!TextureSample)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression returned a non-texture-sample expression"));
    }

    // Bind the texture and let the engine derive the sampler type.
    // AutoSetSampleType is the documented public path the editor's
    // "Refresh Sampler Type" right-click uses.
    TextureSample->Texture = Texture;
    TextureSample->AutoSetSampleType();

    // Optional `coordinates` knob wires a named expression's first
    // output pin into the texture sample's Coordinates input. Empty
    // `source_output` resolves to the named expression's default
    // output pin.
    bool bCoordinatesWired = false;
    FString CoordinatesToken;
    if (Params->TryGetStringField(TEXT("coordinates"), CoordinatesToken)
        || Params->TryGetStringField(TEXT("uv"), CoordinatesToken)
        || Params->TryGetStringField(TEXT("uvs"), CoordinatesToken))
    {
        if (!CoordinatesToken.IsEmpty())
        {
            UMaterialExpression* CoordExpr = FindExpressionByName(Material, CoordinatesToken);
            if (!CoordExpr)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("coordinates expression '%s' not found on material"), *CoordinatesToken));
            }
            FString CoordOutputPin;
            Params->TryGetStringField(TEXT("coordinates_output"), CoordOutputPin);
            // ConnectMaterialExpressions runs the canonical input-name
            // lookup; "Coordinates" matches the texture sample's input
            // FName so the connection lands on the right pin.
            if (!UMaterialEditingLibrary::ConnectMaterialExpressions(CoordExpr, CoordOutputPin, TextureSample, TEXT("Coordinates")))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("ConnectMaterialExpressions failed wiring '%s' -> texture sample 'Coordinates'"), *CoordinatesToken));
            }
            bCoordinatesWired = true;
        }
    }

    // Optional rename so the texture-sample node gets a designer-readable
    // FName the way add_expression / add_function_call do.
    FString DesiredName;
    if (Params->TryGetStringField(TEXT("name"), DesiredName) && !DesiredName.IsEmpty())
    {
        NewExpr->Rename(*DesiredName, nullptr, REN_DontCreateRedirectors);
    }

    // Optional one-shot connection to a material attribute or another
    // expression's named input, mirroring add_expression.
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
    if (Params->TryGetStringField(TEXT("connect_to"), ConnectToToken)
        && !ConnectToToken.IsEmpty())
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_texture_sample"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("texture"), Texture->GetPathName());
    ResultObj->SetStringField(TEXT("texture_class"), Texture->GetClass()->GetName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetNumberField(TEXT("sampler_type"), static_cast<int32>(TextureSample->SamplerType.GetValue()));
    ResultObj->SetBoolField(TEXT("coordinates_wired"), bCoordinatesWired);
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
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddTextureSampleCube(const TSharedPtr<FJsonObject>& Params)
{
    // Resolve the host material.
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }
    UObject* MaterialAsset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(MaterialAsset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    // Resolve the texture asset. UTexture covers UTextureCube and
    // UTexture2D, so we accept either and choose the spawn class
    // accordingly. The "cube" naming is the caller intent; if they
    // hand us a 2D texture we fall back to the 2D expression so
    // a generic "wire this image up" workflow doesn't fail on a
    // class mismatch.
    FString TextureToken;
    if (!Params->TryGetStringField(TEXT("texture"), TextureToken)
        && !Params->TryGetStringField(TEXT("texture_path"), TextureToken)
        && !Params->TryGetStringField(TEXT("texture_asset"), TextureToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'texture' parameter (/Game/... path or unique short name)"));
    }
    UTexture* Texture = MaterialEdit_ResolveTexture(TextureToken);
    if (!Texture)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UTexture '%s' (pass a /Game/... path or a unique short name)"), *TextureToken));
    }

    // Decide which expression subclass to spawn. UTextureCube ->
    // UMaterialExpressionTextureSampleParameterCube; anything else
    // (UTexture2D, UTextureRenderTarget2D, etc.) -> the 2D variant.
    const bool bIsCubeTexture = Texture->IsA<UTextureCube>();
    UClass* ExpressionClass = bIsCubeTexture
        ? static_cast<UClass*>(UMaterialExpressionTextureSampleParameterCube::StaticClass())
        : static_cast<UClass*>(UMaterialExpressionTextureSample::StaticClass());

    // Position cascade.
    int32 PosX = 0;
    int32 PosY = 0;
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
            double X = 0.0;
            double Y = 0.0;
            if (PosObj.IsValid()
                && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
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

    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, ExpressionClass, PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateMaterialExpression failed for %s"), *ExpressionClass->GetName()));
    }

    // UMaterialExpressionTextureBase is the shared base for both
    // UMaterialExpressionTextureSample (the 2D path) and
    // UMaterialExpressionTextureSampleParameterCube (which inherits
    // through UMaterialExpressionTextureSampleParameter). The
    // `Texture` UPROPERTY plus `AutoSetSampleType()` live on the
    // shared base, so the same writer path applies to both.
    UMaterialExpressionTextureBase* TextureBase = Cast<UMaterialExpressionTextureBase>(NewExpr);
    if (!TextureBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression returned a non-texture-base expression"));
    }
    TextureBase->Texture = Texture;
    TextureBase->AutoSetSampleType();

    // Optional `coordinates` knob wires a named expression's first
    // output into the texture sample's Coordinates input pin. For the
    // cube variant the input pin is named `Coordinates` and takes a
    // 3-vector (texcoord), matching the cube sample shader contract.
    bool bCoordinatesWired = false;
    FString CoordinatesToken;
    if (Params->TryGetStringField(TEXT("coordinates"), CoordinatesToken)
        || Params->TryGetStringField(TEXT("uv"), CoordinatesToken)
        || Params->TryGetStringField(TEXT("uvs"), CoordinatesToken))
    {
        if (!CoordinatesToken.IsEmpty())
        {
            UMaterialExpression* CoordExpr = FindExpressionByName(Material, CoordinatesToken);
            if (!CoordExpr)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("coordinates expression '%s' not found on material"), *CoordinatesToken));
            }
            FString CoordOutputPin;
            Params->TryGetStringField(TEXT("coordinates_output"), CoordOutputPin);
            if (!UMaterialEditingLibrary::ConnectMaterialExpressions(CoordExpr, CoordOutputPin, NewExpr, TEXT("Coordinates")))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("ConnectMaterialExpressions failed wiring '%s' -> %s 'Coordinates'"),
                        *CoordinatesToken, *ExpressionClass->GetName()));
            }
            bCoordinatesWired = true;
        }
    }

    // Optional rename so the cube sampler picks up a designer-readable
    // FName, mirroring add_expression / add_texture_sample. The cube
    // parameter variant carries a ParameterName UPROPERTY of its own;
    // callers can land that through the `properties` dict (see below).
    FString DesiredName;
    if (Params->TryGetStringField(TEXT("name"), DesiredName) && !DesiredName.IsEmpty())
    {
        NewExpr->Rename(*DesiredName, nullptr, REN_DontCreateRedirectors);
    }

    // Optional flat properties dict mirrors add_expression so callers
    // can populate `ParameterName`, `Group`, `SortPriority`, etc. on
    // the cube sampler in the same call.
    int32 PropertiesApplied = 0;
    TArray<FString> PropertiesSkipped;
    {
        const TSharedPtr<FJsonObject>* PropsObj = nullptr;
        if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
        {
            for (const auto& KV : (*PropsObj)->Values)
            {
                FProperty* Prop = NewExpr->GetClass()->FindPropertyByName(FName(*KV.Key));
                if (!Prop)
                {
                    PropertiesSkipped.Add(KV.Key);
                    continue;
                }
                FString Buf;
                if (KV.Value.IsValid())
                {
                    if (!KV.Value->TryGetString(Buf))
                    {
                        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
                            = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Buf);
                        FJsonSerializer::Serialize(KV.Value.ToSharedRef(), TEXT(""), Writer);
                    }
                }
                const TCHAR* P = *Buf;
                if (Prop->ImportText_InContainer(P, NewExpr, NewExpr, PPF_None))
                {
                    ++PropertiesApplied;
                }
                else
                {
                    PropertiesSkipped.Add(KV.Key);
                }
            }
        }
    }

    // Optional one-shot connect to a material attribute / another
    // expression input, same shape as add_texture_sample.
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
    if (Params->TryGetStringField(TEXT("connect_to"), ConnectToToken)
        && !ConnectToToken.IsEmpty())
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_texture_sample_cube"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("texture"), Texture->GetPathName());
    ResultObj->SetStringField(TEXT("texture_class"), Texture->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("expression_class"), ExpressionClass->GetName());
    ResultObj->SetBoolField(TEXT("is_cube_texture"), bIsCubeTexture);
    ResultObj->SetBoolField(TEXT("fell_back_to_2d"), !bIsCubeTexture);
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetNumberField(TEXT("sampler_type"), static_cast<int32>(TextureBase->SamplerType.GetValue()));
    ResultObj->SetBoolField(TEXT("coordinates_wired"), bCoordinatesWired);
    ResultObj->SetNumberField(TEXT("properties_applied"), PropertiesApplied);
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& S : PropertiesSkipped) { Arr.Add(MakeShared<FJsonValueString>(S)); }
        ResultObj->SetArrayField(TEXT("properties_skipped"), Arr);
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
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddTexture2DArraySample(const TSharedPtr<FJsonObject>& Params)
{
    // Resolve the host material.
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }
    UObject* MaterialAsset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(MaterialAsset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    // Resolve the texture asset. The helper accepts any UTexture
    // subclass, so a Texture2DArray path lands as a UTexture pointer
    // and we narrow against ::IsA below.
    FString TextureToken;
    if (!Params->TryGetStringField(TEXT("texture"), TextureToken)
        && !Params->TryGetStringField(TEXT("texture_path"), TextureToken)
        && !Params->TryGetStringField(TEXT("texture_asset"), TextureToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'texture' parameter (/Game/... path or unique short name)"));
    }
    UTexture* Texture = MaterialEdit_ResolveTexture(TextureToken);
    if (!Texture)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UTexture '%s' (pass a /Game/... path or a unique short name)"), *TextureToken));
    }

    // Optional parameter name. When set, the spawned expression is the
    // texture-sample-parameter variant so the resulting material exposes
    // a named slot the calling Material Instance can swap. When unset,
    // we spawn a plain UMaterialExpressionTextureSample and bind the
    // texture-array asset directly on the new node so a fresh sample
    // node renders without first wiring a parameter.
    FString ParameterName;
    Params->TryGetStringField(TEXT("parameter_name"), ParameterName);
    if (ParameterName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("parameter"), ParameterName);
    }
    const bool bAsParameter = !ParameterName.IsEmpty();

    // Auto-detect the right spawn class from the resolved asset class so
    // callers can stay flat. Texture2DArray + parameter name asks for
    // the TextureSampleParameter2DArray slot; Texture2DArray on its own
    // asks for the plain TextureSample so the engine wires the array
    // through the standard sampler. If the caller hands us a UTexture2D
    // instead we fall back to the 2D parameter path so a generic
    // "wire this texture up" call still lands (mirrors the cube
    // variant's fallback pattern).
    const bool bIsTextureArray = Texture->IsA<UTexture2DArray>();
    UClass* ExpressionClass = nullptr;
    if (bAsParameter)
    {
        ExpressionClass = bIsTextureArray
            ? static_cast<UClass*>(UMaterialExpressionTextureSampleParameter2DArray::StaticClass())
            : static_cast<UClass*>(UMaterialExpressionTextureSampleParameter2D::StaticClass());
    }
    else
    {
        ExpressionClass = UMaterialExpressionTextureSample::StaticClass();
    }

    // Position cascade through the same helper add_expression /
    // add_function_call / add_texture_sample use.
    int32 PosX = 0;
    int32 PosY = 0;
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
            double X = 0.0;
            double Y = 0.0;
            if (PosObj.IsValid()
                && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
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

    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, ExpressionClass, PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateMaterialExpression failed for %s"), *ExpressionClass->GetName()));
    }

    // UMaterialExpressionTextureBase is the shared base for plain
    // TextureSample and every TextureSampleParameter variant, so the
    // `Texture` UPROPERTY plus AutoSetSampleType() apply through it
    // regardless of which spawn class we chose above.
    UMaterialExpressionTextureBase* TextureBase = Cast<UMaterialExpressionTextureBase>(NewExpr);
    if (!TextureBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression returned a non-texture-base expression"));
    }
    TextureBase->Texture = Texture;
    TextureBase->AutoSetSampleType();

    // If we spawned the parameter variant, land the FName on the shared
    // ParameterName UPROPERTY (defined on the parent
    // UMaterialExpressionTextureSampleParameter). Casting through the
    // immediate parent covers the 2DArray slot and the 2D fallback case
    // with a single write.
    if (bAsParameter)
    {
        if (UMaterialExpressionTextureSampleParameter* ParamExpr = Cast<UMaterialExpressionTextureSampleParameter>(NewExpr))
        {
            ParamExpr->ParameterName = FName(*ParameterName);
        }
    }

    // Optional `coordinates` knob wires a named expression's first
    // output into the new sample's `Coordinates` input pin. Same
    // pattern as add_texture_sample / add_texture_sample_cube.
    bool bCoordinatesWired = false;
    FString CoordinatesToken;
    if (Params->TryGetStringField(TEXT("coordinates"), CoordinatesToken)
        || Params->TryGetStringField(TEXT("uv"), CoordinatesToken)
        || Params->TryGetStringField(TEXT("uvs"), CoordinatesToken))
    {
        if (!CoordinatesToken.IsEmpty())
        {
            UMaterialExpression* CoordExpr = FindExpressionByName(Material, CoordinatesToken);
            if (!CoordExpr)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("coordinates expression '%s' not found on material"), *CoordinatesToken));
            }
            FString CoordOutputPin;
            Params->TryGetStringField(TEXT("coordinates_output"), CoordOutputPin);
            if (!UMaterialEditingLibrary::ConnectMaterialExpressions(CoordExpr, CoordOutputPin, NewExpr, TEXT("Coordinates")))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("ConnectMaterialExpressions failed wiring '%s' -> %s 'Coordinates'"),
                        *CoordinatesToken, *ExpressionClass->GetName()));
            }
            bCoordinatesWired = true;
        }
    }

    // Optional rename so the spawned node picks up a designer-readable
    // FName, mirroring add_expression / add_texture_sample.
    FString DesiredName;
    if (Params->TryGetStringField(TEXT("name"), DesiredName) && !DesiredName.IsEmpty())
    {
        NewExpr->Rename(*DesiredName, nullptr, REN_DontCreateRedirectors);
    }

    // Optional flat properties dict mirrors add_expression so callers
    // can populate `Group` / `SortPriority` / etc. on the parameter
    // variant in the same call.
    int32 PropertiesApplied = 0;
    TArray<FString> PropertiesSkipped;
    {
        const TSharedPtr<FJsonObject>* PropsObj = nullptr;
        if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
        {
            for (const auto& KV : (*PropsObj)->Values)
            {
                FProperty* Prop = NewExpr->GetClass()->FindPropertyByName(FName(*KV.Key));
                if (!Prop)
                {
                    PropertiesSkipped.Add(KV.Key);
                    continue;
                }
                FString Buf;
                if (KV.Value.IsValid())
                {
                    if (!KV.Value->TryGetString(Buf))
                    {
                        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
                            = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Buf);
                        FJsonSerializer::Serialize(KV.Value.ToSharedRef(), TEXT(""), Writer);
                    }
                }
                const TCHAR* P = *Buf;
                if (Prop->ImportText_InContainer(P, NewExpr, NewExpr, PPF_None))
                {
                    ++PropertiesApplied;
                }
                else
                {
                    PropertiesSkipped.Add(KV.Key);
                }
            }
        }
    }

    // Optional one-shot connect to a material attribute / another
    // expression input, same shape as add_texture_sample.
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
    if (Params->TryGetStringField(TEXT("connect_to"), ConnectToToken)
        && !ConnectToToken.IsEmpty())
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_2d_array_sample"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("texture"), Texture->GetPathName());
    ResultObj->SetStringField(TEXT("texture_class"), Texture->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("expression_class"), ExpressionClass->GetName());
    ResultObj->SetBoolField(TEXT("is_texture_2d_array"), bIsTextureArray);
    ResultObj->SetBoolField(TEXT("fell_back_to_2d"), !bIsTextureArray && bAsParameter);
    ResultObj->SetBoolField(TEXT("as_parameter"), bAsParameter);
    if (bAsParameter)
    {
        ResultObj->SetStringField(TEXT("parameter_name"), ParameterName);
    }
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetNumberField(TEXT("sampler_type"), static_cast<int32>(TextureBase->SamplerType.GetValue()));
    ResultObj->SetBoolField(TEXT("coordinates_wired"), bCoordinatesWired);
    ResultObj->SetNumberField(TEXT("properties_applied"), PropertiesApplied);
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& S : PropertiesSkipped) { Arr.Add(MakeShared<FJsonValueString>(S)); }
        ResultObj->SetArrayField(TEXT("properties_skipped"), Arr);
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
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddConstant(const TSharedPtr<FJsonObject>& Params)
{
    // Wraps the existing add_expression for the common literal-constant
    // case so callers do not need to know the expression class name.
    // The op auto-picks between UMaterialExpressionConstant (1 channel),
    // UMaterialExpressionConstant2Vector (2 channels),
    // UMaterialExpressionConstant3Vector (3 channels), and
    // UMaterialExpressionConstant4Vector (4 channels) from the supplied
    // `value` (scalar number / 2-tuple / 3-tuple / 4-tuple). The
    // resulting node lands its literal on the matching node fields
    // (`R` for the scalar, `R` / `G` for the 2-channel, `Constant`
    // (FLinearColor) for the 3- / 4-channel variants).
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }
    UMaterial* Material = Cast<UMaterial>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial"), *MaterialPath));
    }

    // `value` accepts a JSON scalar (number) or a JSON array of length
    // 2 / 3 / 4. We keep the parsed channels in a 4-wide buffer so we
    // can map onto whichever constant subclass we pick.
    int32 ChannelCount = 0;
    float Channels[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'value' parameter (number or array of 2 / 3 / 4 numbers)"));
    }
    if (ValueField->Type == EJson::Number)
    {
        Channels[0] = static_cast<float>(ValueField->AsNumber());
        ChannelCount = 1;
    }
    else if (ValueField->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
        if (Arr.Num() < 1 || Arr.Num() > 4)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("'value' array length %d not supported (use 1, 2, 3, or 4 channels)"),
                    Arr.Num()));
        }
        ChannelCount = Arr.Num();
        for (int32 I = 0; I < ChannelCount; ++I)
        {
            if (!Arr[I].IsValid() || Arr[I]->Type != EJson::Number)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("'value[%d]' must be a number"), I));
            }
            Channels[I] = static_cast<float>(Arr[I]->AsNumber());
        }
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("'value' must be a number or an array of 2 / 3 / 4 numbers"));
    }

    // Auto-pick the expression class from the channel count.
    TSubclassOf<UMaterialExpression> ConstantClass = nullptr;
    FString ConstantClassTokenEcho;
    switch (ChannelCount)
    {
        case 1:
            ConstantClass = UMaterialExpressionConstant::StaticClass();
            ConstantClassTokenEcho = TEXT("Constant");
            break;
        case 2:
            ConstantClass = UMaterialExpressionConstant2Vector::StaticClass();
            ConstantClassTokenEcho = TEXT("Constant2Vector");
            break;
        case 3:
            ConstantClass = UMaterialExpressionConstant3Vector::StaticClass();
            ConstantClassTokenEcho = TEXT("Constant3Vector");
            break;
        case 4:
            ConstantClass = UMaterialExpressionConstant4Vector::StaticClass();
            ConstantClassTokenEcho = TEXT("Constant4Vector");
            break;
        default:
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Could not resolve a Constant subclass for the supplied 'value'"));
    }

    // Position cascade reuses the same DeriveDefaultPosition helper
    // that add_expression / add_texture_sample share so a chain of
    // single-call ops lays its nodes out left-to-right.
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
            if (PosObj.IsValid()
                && (PosObj->TryGetNumberField(TEXT("x"), X) || PosObj->TryGetNumberField(TEXT("X"), X))
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

    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, ConstantClass, PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateMaterialExpression failed for class '%s'"), *ConstantClass->GetName()));
    }

    // Land the literal on the matching node fields. The plain
    // Constant and Constant2Vector classes expose `R` / `G`
    // scalar fields; Constant3Vector / Constant4Vector expose an
    // FLinearColor `Constant` field. We write directly through the
    // typed pointer so the engine's value path stays the canonical
    // one.
    if (UMaterialExpressionConstant* AsScalar = Cast<UMaterialExpressionConstant>(NewExpr))
    {
        AsScalar->R = Channels[0];
    }
    else if (UMaterialExpressionConstant2Vector* As2 = Cast<UMaterialExpressionConstant2Vector>(NewExpr))
    {
        As2->R = Channels[0];
        As2->G = Channels[1];
    }
    else if (UMaterialExpressionConstant3Vector* As3 = Cast<UMaterialExpressionConstant3Vector>(NewExpr))
    {
        As3->Constant = FLinearColor(Channels[0], Channels[1], Channels[2], 1.0f);
    }
    else if (UMaterialExpressionConstant4Vector* As4 = Cast<UMaterialExpressionConstant4Vector>(NewExpr))
    {
        As4->Constant = FLinearColor(Channels[0], Channels[1], Channels[2], Channels[3]);
    }

    // Optional flat property dict mirrors add_expression so callers
    // can override the node's display name, the description, or the
    // colour channels through ImportText if a future engine drift
    // renames anything.
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

    // Optional one-shot wiring into a material attribute or another
    // expression's named input pin. Same shape as add_expression.
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

    TArray<TSharedPtr<FJsonValue>> ChannelArr;
    for (int32 I = 0; I < ChannelCount; ++I)
    {
        ChannelArr.Add(MakeShared<FJsonValueNumber>(Channels[I]));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_constant"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetStringField(TEXT("constant_class"), ConstantClassTokenEcho);
    ResultObj->SetNumberField(TEXT("channel_count"), ChannelCount);
    ResultObj->SetArrayField(TEXT("channels"), ChannelArr);
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
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}
