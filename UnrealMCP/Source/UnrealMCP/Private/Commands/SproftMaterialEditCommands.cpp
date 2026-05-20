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
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionWorldPosition.h"
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
    if (Operation == TEXT("add_math") || Operation == TEXT("add_math_op")
        || Operation == TEXT("add_math_node") || Operation == TEXT("add_op"))
    {
        return AddMath(Params);
    }
    if (Operation == TEXT("add_uv_node") || Operation == TEXT("add_uv")
        || Operation == TEXT("add_uv_expression") || Operation == TEXT("uv_node"))
    {
        return AddUVNode(Params);
    }
    if (Operation == TEXT("add_dynamic_parameter") || Operation == TEXT("add_dynamicparam")
        || Operation == TEXT("dynamic_parameter") || Operation == TEXT("add_dynamic_param"))
    {
        return AddDynamicParameter(Params);
    }
    if (Operation == TEXT("add_fresnel") || Operation == TEXT("fresnel")
        || Operation == TEXT("add_fresnel_node"))
    {
        return AddFresnel(Params);
    }
    if (Operation == TEXT("set_blend_mode") || Operation == TEXT("set_blendmode")
        || Operation == TEXT("blend_mode") || Operation == TEXT("set_material_blend_mode"))
    {
        return SetBlendMode(Params);
    }
    if (Operation == TEXT("set_material_flags") || Operation == TEXT("set_flags")
        || Operation == TEXT("material_flags") || Operation == TEXT("set_material_bools"))
    {
        return SetMaterialFlags(Params);
    }
    if (Operation == TEXT("set_shading_model") || Operation == TEXT("set_shadingmodel")
        || Operation == TEXT("shading_model") || Operation == TEXT("set_material_shading_model"))
    {
        return SetShadingModel(Params);
    }
    if (Operation == TEXT("set_translucency_settings") || Operation == TEXT("set_translucency")
        || Operation == TEXT("translucency_settings") || Operation == TEXT("set_material_translucency"))
    {
        return SetTranslucencySettings(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported material_edit operation '%s'. Supported: create_material, create_material_instance_constant, set_instance_parameter, add_expression, add_expressions, connect_expressions, set_expression_property, create_parameter_collection, add_collection_parameter, create_material_function, add_function_call, set_attribute_blendable, add_texture_sample, add_texture_sample_cube, add_2d_array_sample, add_constant, add_math, add_uv_node, add_dynamic_parameter, add_fresnel, set_blend_mode, set_material_flags, set_shading_model, set_translucency_settings"), *Operation));
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

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddMath(const TSharedPtr<FJsonObject>& Params)
{
    // Wraps the common math expression-node creates so callers do not
    // need to spell out the long `UMaterialExpressionXyz` class names.
    // The `op` token picks the subclass; optional input names wire
    // sibling expressions on the same material into the matching
    // FExpressionInput slot through `UMaterialEditingLibrary::
    // ConnectMaterialExpressions`. The Const* fallback slots accept
    // literal floats so a "* 0.5" multiply lands in one call.
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

    FString OpToken;
    if (!Params->TryGetStringField(TEXT("op"), OpToken)
        && !Params->TryGetStringField(TEXT("math_op"), OpToken)
        && !Params->TryGetStringField(TEXT("math"), OpToken)
        && !Params->TryGetStringField(TEXT("operation_token"), OpToken)
        && !Params->TryGetStringField(TEXT("op_name"), OpToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'op' parameter (Add / Subtract / Multiply / Divide / Min / Max / Lerp / Power / Sin / Cos / Abs / Saturate / OneMinus / Normalize / DotProduct / CrossProduct)"));
    }

    // Resolve the math subclass + classify its input slot shape in
    // one pass so the rest of the op can fan out from a single token.
    enum class EMathShape
    {
        TwoInputAB,        // Add / Subtract / Multiply / Divide / Min / Max
        ThreeInputLerp,    // LinearInterpolate (A / B / Alpha)
        TwoInputBase,      // Power (Base / Exponent)
        OneInput,          // Sin / Cos / Abs / Saturate / OneMinus
        OneInputNormalize, // Normalize (VectorInput slot)
        TwoInputDotCross,  // DotProduct / CrossProduct (no Const slots)
    };

    TSubclassOf<UMaterialExpression> MathClass = nullptr;
    EMathShape Shape = EMathShape::OneInput;
    FString CanonicalToken;
    const FString OpLower = OpToken.ToLower();

    if (OpLower == TEXT("add") || OpLower == TEXT("+"))
    {
        MathClass = UMaterialExpressionAdd::StaticClass();
        Shape = EMathShape::TwoInputAB;
        CanonicalToken = TEXT("Add");
    }
    else if (OpLower == TEXT("subtract") || OpLower == TEXT("sub") || OpLower == TEXT("-"))
    {
        MathClass = UMaterialExpressionSubtract::StaticClass();
        Shape = EMathShape::TwoInputAB;
        CanonicalToken = TEXT("Subtract");
    }
    else if (OpLower == TEXT("multiply") || OpLower == TEXT("mul") || OpLower == TEXT("*"))
    {
        MathClass = UMaterialExpressionMultiply::StaticClass();
        Shape = EMathShape::TwoInputAB;
        CanonicalToken = TEXT("Multiply");
    }
    else if (OpLower == TEXT("divide") || OpLower == TEXT("div") || OpLower == TEXT("/"))
    {
        MathClass = UMaterialExpressionDivide::StaticClass();
        Shape = EMathShape::TwoInputAB;
        CanonicalToken = TEXT("Divide");
    }
    else if (OpLower == TEXT("min"))
    {
        MathClass = UMaterialExpressionMin::StaticClass();
        Shape = EMathShape::TwoInputAB;
        CanonicalToken = TEXT("Min");
    }
    else if (OpLower == TEXT("max"))
    {
        MathClass = UMaterialExpressionMax::StaticClass();
        Shape = EMathShape::TwoInputAB;
        CanonicalToken = TEXT("Max");
    }
    else if (OpLower == TEXT("lerp") || OpLower == TEXT("linear_interpolate")
        || OpLower == TEXT("linearinterpolate") || OpLower == TEXT("mix"))
    {
        MathClass = UMaterialExpressionLinearInterpolate::StaticClass();
        Shape = EMathShape::ThreeInputLerp;
        CanonicalToken = TEXT("Lerp");
    }
    else if (OpLower == TEXT("power") || OpLower == TEXT("pow") || OpLower == TEXT("^"))
    {
        MathClass = UMaterialExpressionPower::StaticClass();
        Shape = EMathShape::TwoInputBase;
        CanonicalToken = TEXT("Power");
    }
    else if (OpLower == TEXT("sin") || OpLower == TEXT("sine"))
    {
        MathClass = UMaterialExpressionSine::StaticClass();
        Shape = EMathShape::OneInput;
        CanonicalToken = TEXT("Sin");
    }
    else if (OpLower == TEXT("cos") || OpLower == TEXT("cosine"))
    {
        MathClass = UMaterialExpressionCosine::StaticClass();
        Shape = EMathShape::OneInput;
        CanonicalToken = TEXT("Cos");
    }
    else if (OpLower == TEXT("abs") || OpLower == TEXT("absolute"))
    {
        MathClass = UMaterialExpressionAbs::StaticClass();
        Shape = EMathShape::OneInput;
        CanonicalToken = TEXT("Abs");
    }
    else if (OpLower == TEXT("saturate") || OpLower == TEXT("clamp01"))
    {
        MathClass = UMaterialExpressionSaturate::StaticClass();
        Shape = EMathShape::OneInput;
        CanonicalToken = TEXT("Saturate");
    }
    else if (OpLower == TEXT("oneminus") || OpLower == TEXT("one_minus")
        || OpLower == TEXT("1-x") || OpLower == TEXT("one-minus"))
    {
        MathClass = UMaterialExpressionOneMinus::StaticClass();
        Shape = EMathShape::OneInput;
        CanonicalToken = TEXT("OneMinus");
    }
    else if (OpLower == TEXT("normalize"))
    {
        MathClass = UMaterialExpressionNormalize::StaticClass();
        Shape = EMathShape::OneInputNormalize;
        CanonicalToken = TEXT("Normalize");
    }
    else if (OpLower == TEXT("dot") || OpLower == TEXT("dot_product")
        || OpLower == TEXT("dotproduct"))
    {
        MathClass = UMaterialExpressionDotProduct::StaticClass();
        Shape = EMathShape::TwoInputDotCross;
        CanonicalToken = TEXT("DotProduct");
    }
    else if (OpLower == TEXT("cross") || OpLower == TEXT("cross_product")
        || OpLower == TEXT("crossproduct"))
    {
        MathClass = UMaterialExpressionCrossProduct::StaticClass();
        Shape = EMathShape::TwoInputDotCross;
        CanonicalToken = TEXT("CrossProduct");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown 'op' token '%s'. Use Add / Subtract / Multiply / Divide / Min / Max / Lerp / Power / Sin / Cos / Abs / Saturate / OneMinus / Normalize / DotProduct / CrossProduct"),
                *OpToken));
    }

    // Position cascade reuses the same DeriveDefaultPosition helper
    // that add_expression / add_constant share.
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
        Material, MathClass, PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateMaterialExpression failed for class '%s'"), *MathClass->GetName()));
    }

    // Optional `name` renames the spawned node so follow-up wiring
    // calls can address it by FName.
    FString NameOverride;
    if (Params->TryGetStringField(TEXT("name"), NameOverride) && !NameOverride.IsEmpty())
    {
        NewExpr->Rename(*NameOverride, NewExpr->GetOuter(), REN_DontCreateRedirectors);
    }

    // Helper lambda: resolve a sibling expression name + optional
    // explicit output pin, then wire it into the new node's named
    // input slot through ConnectMaterialExpressions.
    TArray<FString> InputWireErrors;
    auto TryWireSlot = [&](const FString& SlotName, const FString& InputToken, const FString& OutputToken) -> bool
    {
        if (InputToken.IsEmpty())
        {
            return false;
        }
        UMaterialExpression* SourceExpr = FindExpressionByName(Material, InputToken);
        if (!SourceExpr)
        {
            InputWireErrors.Add(FString::Printf(TEXT("input '%s' for slot '%s' not found"), *InputToken, *SlotName));
            return false;
        }
        if (!UMaterialEditingLibrary::ConnectMaterialExpressions(SourceExpr, OutputToken, NewExpr, SlotName))
        {
            InputWireErrors.Add(FString::Printf(TEXT("ConnectMaterialExpressions failed: %s -> %s.%s"),
                *InputToken, *NewExpr->GetName(), *SlotName));
            return false;
        }
        return true;
    };

    TArray<TSharedPtr<FJsonValue>> InputsConnectedJson;
    auto RecordInputConnection = [&](const FString& SlotName, const FString& SourceName)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("slot"), SlotName);
        Row->SetStringField(TEXT("source"), SourceName);
        InputsConnectedJson.Add(MakeShared<FJsonValueObject>(Row));
    };

    // Per-shape input wiring + literal land.
    if (Shape == EMathShape::TwoInputAB)
    {
        FString AToken, BToken, AOut, BOut;
        Params->TryGetStringField(TEXT("A"), AToken) || Params->TryGetStringField(TEXT("a"), AToken)
            || Params->TryGetStringField(TEXT("input_a"), AToken);
        Params->TryGetStringField(TEXT("B"), BToken) || Params->TryGetStringField(TEXT("b"), BToken)
            || Params->TryGetStringField(TEXT("input_b"), BToken);
        Params->TryGetStringField(TEXT("a_output"), AOut);
        Params->TryGetStringField(TEXT("b_output"), BOut);
        if (TryWireSlot(TEXT("A"), AToken, AOut)) { RecordInputConnection(TEXT("A"), AToken); }
        if (TryWireSlot(TEXT("B"), BToken, BOut)) { RecordInputConnection(TEXT("B"), BToken); }

        // Const slots: ConstA / ConstB float fallbacks when the input
        // pin is not wired up. `constant_a` / `constant_b` map onto
        // those slots; a bare `constant` populates ConstB for the "*
        // scalar" idiom.
        double ConstA = 0.0, ConstB = 0.0, BareConstant = 0.0;
        bool bHasA = Params->TryGetNumberField(TEXT("constant_a"), ConstA)
            || Params->TryGetNumberField(TEXT("const_a"), ConstA)
            || Params->TryGetNumberField(TEXT("ConstA"), ConstA);
        bool bHasB = Params->TryGetNumberField(TEXT("constant_b"), ConstB)
            || Params->TryGetNumberField(TEXT("const_b"), ConstB)
            || Params->TryGetNumberField(TEXT("ConstB"), ConstB);
        bool bHasBare = Params->TryGetNumberField(TEXT("constant"), BareConstant)
            || Params->TryGetNumberField(TEXT("value"), BareConstant)
            || Params->TryGetNumberField(TEXT("scalar"), BareConstant);
        // Each concrete two-input math node carries its own ConstA /
        // ConstB pair. Branch by class so the writes hit the typed
        // member without needing reflection.
        if (UMaterialExpressionAdd* AsAdd = Cast<UMaterialExpressionAdd>(NewExpr))
        {
            if (bHasA) AsAdd->ConstA = static_cast<float>(ConstA);
            if (bHasB) AsAdd->ConstB = static_cast<float>(ConstB);
            if (bHasBare && !bHasB) AsAdd->ConstB = static_cast<float>(BareConstant);
        }
        else if (UMaterialExpressionSubtract* AsSub = Cast<UMaterialExpressionSubtract>(NewExpr))
        {
            if (bHasA) AsSub->ConstA = static_cast<float>(ConstA);
            if (bHasB) AsSub->ConstB = static_cast<float>(ConstB);
            if (bHasBare && !bHasB) AsSub->ConstB = static_cast<float>(BareConstant);
        }
        else if (UMaterialExpressionMultiply* AsMul = Cast<UMaterialExpressionMultiply>(NewExpr))
        {
            if (bHasA) AsMul->ConstA = static_cast<float>(ConstA);
            if (bHasB) AsMul->ConstB = static_cast<float>(ConstB);
            if (bHasBare && !bHasB) AsMul->ConstB = static_cast<float>(BareConstant);
        }
        else if (UMaterialExpressionDivide* AsDiv = Cast<UMaterialExpressionDivide>(NewExpr))
        {
            if (bHasA) AsDiv->ConstA = static_cast<float>(ConstA);
            if (bHasB) AsDiv->ConstB = static_cast<float>(ConstB);
            if (bHasBare && !bHasB) AsDiv->ConstB = static_cast<float>(BareConstant);
        }
        else if (UMaterialExpressionMin* AsMin = Cast<UMaterialExpressionMin>(NewExpr))
        {
            if (bHasA) AsMin->ConstA = static_cast<float>(ConstA);
            if (bHasB) AsMin->ConstB = static_cast<float>(ConstB);
            if (bHasBare && !bHasB) AsMin->ConstB = static_cast<float>(BareConstant);
        }
        else if (UMaterialExpressionMax* AsMax = Cast<UMaterialExpressionMax>(NewExpr))
        {
            if (bHasA) AsMax->ConstA = static_cast<float>(ConstA);
            if (bHasB) AsMax->ConstB = static_cast<float>(ConstB);
            if (bHasBare && !bHasB) AsMax->ConstB = static_cast<float>(BareConstant);
        }
    }
    else if (Shape == EMathShape::ThreeInputLerp)
    {
        FString AToken, BToken, AlphaToken, AOut, BOut, AlphaOut;
        Params->TryGetStringField(TEXT("A"), AToken) || Params->TryGetStringField(TEXT("a"), AToken)
            || Params->TryGetStringField(TEXT("input_a"), AToken);
        Params->TryGetStringField(TEXT("B"), BToken) || Params->TryGetStringField(TEXT("b"), BToken)
            || Params->TryGetStringField(TEXT("input_b"), BToken);
        Params->TryGetStringField(TEXT("Alpha"), AlphaToken) || Params->TryGetStringField(TEXT("alpha"), AlphaToken)
            || Params->TryGetStringField(TEXT("T"), AlphaToken) || Params->TryGetStringField(TEXT("t"), AlphaToken);
        Params->TryGetStringField(TEXT("a_output"), AOut);
        Params->TryGetStringField(TEXT("b_output"), BOut);
        Params->TryGetStringField(TEXT("alpha_output"), AlphaOut);
        if (TryWireSlot(TEXT("A"), AToken, AOut)) { RecordInputConnection(TEXT("A"), AToken); }
        if (TryWireSlot(TEXT("B"), BToken, BOut)) { RecordInputConnection(TEXT("B"), BToken); }
        if (TryWireSlot(TEXT("Alpha"), AlphaToken, AlphaOut)) { RecordInputConnection(TEXT("Alpha"), AlphaToken); }

        UMaterialExpressionLinearInterpolate* AsLerp = Cast<UMaterialExpressionLinearInterpolate>(NewExpr);
        if (AsLerp)
        {
            double ConstA = 0.0, ConstB = 0.0, ConstAlpha = 0.0;
            if (Params->TryGetNumberField(TEXT("constant_a"), ConstA)
                || Params->TryGetNumberField(TEXT("const_a"), ConstA)
                || Params->TryGetNumberField(TEXT("ConstA"), ConstA))
            {
                AsLerp->ConstA = static_cast<float>(ConstA);
            }
            if (Params->TryGetNumberField(TEXT("constant_b"), ConstB)
                || Params->TryGetNumberField(TEXT("const_b"), ConstB)
                || Params->TryGetNumberField(TEXT("ConstB"), ConstB))
            {
                AsLerp->ConstB = static_cast<float>(ConstB);
            }
            if (Params->TryGetNumberField(TEXT("constant_alpha"), ConstAlpha)
                || Params->TryGetNumberField(TEXT("const_alpha"), ConstAlpha)
                || Params->TryGetNumberField(TEXT("ConstAlpha"), ConstAlpha)
                || Params->TryGetNumberField(TEXT("constant_t"), ConstAlpha)
                || Params->TryGetNumberField(TEXT("constant"), ConstAlpha))
            {
                AsLerp->ConstAlpha = static_cast<float>(ConstAlpha);
            }
        }
    }
    else if (Shape == EMathShape::TwoInputBase)
    {
        FString BaseToken, ExpToken, BaseOut, ExpOut;
        Params->TryGetStringField(TEXT("Base"), BaseToken) || Params->TryGetStringField(TEXT("base"), BaseToken)
            || Params->TryGetStringField(TEXT("input"), BaseToken)
            || Params->TryGetStringField(TEXT("A"), BaseToken) || Params->TryGetStringField(TEXT("a"), BaseToken);
        Params->TryGetStringField(TEXT("Exponent"), ExpToken) || Params->TryGetStringField(TEXT("exponent"), ExpToken)
            || Params->TryGetStringField(TEXT("B"), ExpToken) || Params->TryGetStringField(TEXT("b"), ExpToken);
        Params->TryGetStringField(TEXT("base_output"), BaseOut);
        Params->TryGetStringField(TEXT("exponent_output"), ExpOut);
        if (TryWireSlot(TEXT("Base"), BaseToken, BaseOut)) { RecordInputConnection(TEXT("Base"), BaseToken); }
        if (TryWireSlot(TEXT("Exponent"), ExpToken, ExpOut)) { RecordInputConnection(TEXT("Exponent"), ExpToken); }

        UMaterialExpressionPower* AsPow = Cast<UMaterialExpressionPower>(NewExpr);
        if (AsPow)
        {
            double ConstExp = 0.0;
            if (Params->TryGetNumberField(TEXT("constant_exponent"), ConstExp)
                || Params->TryGetNumberField(TEXT("const_exponent"), ConstExp)
                || Params->TryGetNumberField(TEXT("ConstExponent"), ConstExp)
                || Params->TryGetNumberField(TEXT("constant"), ConstExp)
                || Params->TryGetNumberField(TEXT("exponent_value"), ConstExp))
            {
                AsPow->ConstExponent = static_cast<float>(ConstExp);
            }
        }
    }
    else if (Shape == EMathShape::OneInput)
    {
        FString InputToken, InputOut;
        Params->TryGetStringField(TEXT("input"), InputToken)
            || Params->TryGetStringField(TEXT("Input"), InputToken)
            || Params->TryGetStringField(TEXT("A"), InputToken)
            || Params->TryGetStringField(TEXT("a"), InputToken);
        Params->TryGetStringField(TEXT("input_output"), InputOut);
        if (TryWireSlot(TEXT("Input"), InputToken, InputOut)) { RecordInputConnection(TEXT("Input"), InputToken); }
    }
    else if (Shape == EMathShape::OneInputNormalize)
    {
        FString InputToken, InputOut;
        Params->TryGetStringField(TEXT("input"), InputToken)
            || Params->TryGetStringField(TEXT("Input"), InputToken)
            || Params->TryGetStringField(TEXT("VectorInput"), InputToken)
            || Params->TryGetStringField(TEXT("vector_input"), InputToken)
            || Params->TryGetStringField(TEXT("A"), InputToken)
            || Params->TryGetStringField(TEXT("a"), InputToken);
        Params->TryGetStringField(TEXT("input_output"), InputOut);
        if (TryWireSlot(TEXT("VectorInput"), InputToken, InputOut)) { RecordInputConnection(TEXT("VectorInput"), InputToken); }
    }
    else if (Shape == EMathShape::TwoInputDotCross)
    {
        FString AToken, BToken, AOut, BOut;
        Params->TryGetStringField(TEXT("A"), AToken) || Params->TryGetStringField(TEXT("a"), AToken)
            || Params->TryGetStringField(TEXT("input_a"), AToken);
        Params->TryGetStringField(TEXT("B"), BToken) || Params->TryGetStringField(TEXT("b"), BToken)
            || Params->TryGetStringField(TEXT("input_b"), BToken);
        Params->TryGetStringField(TEXT("a_output"), AOut);
        Params->TryGetStringField(TEXT("b_output"), BOut);
        if (TryWireSlot(TEXT("A"), AToken, AOut)) { RecordInputConnection(TEXT("A"), AToken); }
        if (TryWireSlot(TEXT("B"), BToken, BOut)) { RecordInputConnection(TEXT("B"), BToken); }
    }

    // Optional flat properties dict applies any remaining UPROPERTY
    // values (Period on Sin / Cos, Description, etc.) through
    // ImportText_InContainer; mirrors add_expression.
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

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_math"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("math_op"), CanonicalToken);
    ResultObj->SetStringField(TEXT("expression_class"), MathClass->GetName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetArrayField(TEXT("inputs_connected"), InputsConnectedJson);
    if (InputWireErrors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& E : InputWireErrors)
        {
            Arr.Add(MakeShared<FJsonValueString>(E));
        }
        ResultObj->SetArrayField(TEXT("input_errors"), Arr);
    }
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

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddUVNode(const TSharedPtr<FJsonObject>& Params)
{
    // Spawn one of the common UV-flow expressions by short token so
    // callers do not have to spell out the long class names. Wraps
    // TextureCoordinate / Panner / Rotator plus the position family
    // (WorldPosition / ObjectPosition / CameraPosition / ScreenPosition)
    // since these all share the same flat-property dict shape and
    // downstream wiring needs.
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

    FString OpToken;
    if (!Params->TryGetStringField(TEXT("op"), OpToken)
        && !Params->TryGetStringField(TEXT("uv_op"), OpToken)
        && !Params->TryGetStringField(TEXT("node"), OpToken)
        && !Params->TryGetStringField(TEXT("uv_node"), OpToken)
        && !Params->TryGetStringField(TEXT("kind"), OpToken)
        && !Params->TryGetStringField(TEXT("type"), OpToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'op' parameter (TextureCoordinate / Panner / Rotator / WorldPosition / ObjectPosition / CameraPosition / ScreenPosition)"));
    }

    TSubclassOf<UMaterialExpression> NodeClass = nullptr;
    FString CanonicalToken;
    const FString OpLower = OpToken.ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));

    if (OpLower == TEXT("texturecoordinate") || OpLower == TEXT("texcoord")
        || OpLower == TEXT("uv") || OpLower == TEXT("uvs"))
    {
        NodeClass = UMaterialExpressionTextureCoordinate::StaticClass();
        CanonicalToken = TEXT("TextureCoordinate");
    }
    else if (OpLower == TEXT("panner"))
    {
        NodeClass = UMaterialExpressionPanner::StaticClass();
        CanonicalToken = TEXT("Panner");
    }
    else if (OpLower == TEXT("rotator"))
    {
        NodeClass = UMaterialExpressionRotator::StaticClass();
        CanonicalToken = TEXT("Rotator");
    }
    else if (OpLower == TEXT("worldposition") || OpLower == TEXT("worldpos")
        || OpLower == TEXT("absoluteworldposition") || OpLower == TEXT("worldpositionws"))
    {
        NodeClass = UMaterialExpressionWorldPosition::StaticClass();
        CanonicalToken = TEXT("WorldPosition");
    }
    else if (OpLower == TEXT("objectposition") || OpLower == TEXT("objectpos")
        || OpLower == TEXT("objectpositionws"))
    {
        NodeClass = UMaterialExpressionObjectPositionWS::StaticClass();
        CanonicalToken = TEXT("ObjectPosition");
    }
    else if (OpLower == TEXT("cameraposition") || OpLower == TEXT("camerapos")
        || OpLower == TEXT("camerapositionws"))
    {
        NodeClass = UMaterialExpressionCameraPositionWS::StaticClass();
        CanonicalToken = TEXT("CameraPosition");
    }
    else if (OpLower == TEXT("screenposition") || OpLower == TEXT("screenpos"))
    {
        NodeClass = UMaterialExpressionScreenPosition::StaticClass();
        CanonicalToken = TEXT("ScreenPosition");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown 'op' token '%s'. Use TextureCoordinate / Panner / Rotator / WorldPosition / ObjectPosition / CameraPosition / ScreenPosition"),
                *OpToken));
    }

    // Position cascade reuses the same DeriveDefaultPosition helper
    // that add_expression / add_constant / add_math share.
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
        Material, NodeClass, PosX, PosY);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateMaterialExpression failed for class '%s'"), *NodeClass->GetName()));
    }

    // Optional `name` renames the spawned node so follow-up wiring
    // calls can address it by FName.
    FString NameOverride;
    if (Params->TryGetStringField(TEXT("name"), NameOverride) && !NameOverride.IsEmpty())
    {
        NewExpr->Rename(*NameOverride, NewExpr->GetOuter(), REN_DontCreateRedirectors);
    }

    // Optional flat properties dict (CoordinateIndex / UTiling /
    // SpeedX / SpeedY / WorldPositionShaderOffset / OriginType etc.)
    // applies through ImportText_InContainer.
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
    // expression's named input pin. Same shape as add_expression /
    // add_math.
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_uv_node"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("uv_op"), CanonicalToken);
    ResultObj->SetStringField(TEXT("expression_class"), NodeClass->GetName());
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
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
    // Spawns a UMaterialExpressionDynamicParameter on a target
    // material's graph. Niagara renderers (and other runtime systems)
    // can drive the four-channel output per particle / per instance
    // without shipping a Material Instance for every variation. The
    // engine surfaces the four channels through `ParamNames` (the
    // editor-side per-channel name list) plus a `ParameterIndex`
    // selector that lets a material host up to four dynamic parameter
    // nodes (each one binds a different per-particle slot in the
    // Niagara renderer). The expression also carries a `DefaultValue`
    // FLinearColor used as the preview / fallback when no renderer is
    // bound.
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

    // The per-material slot index: each material can carry up to four
    // dynamic parameter expressions, each one binding a different
    // per-particle slot in the Niagara renderer's
    // DynamicMaterialParameters array. The engine clamps internally
    // but we surface a clear error rather than relying on the implicit
    // mod-4 to keep the shape obvious.
    int32 ParameterIndex = 0;
    if (!Params->TryGetNumberField(TEXT("parameter_index"), ParameterIndex)
        && !Params->TryGetNumberField(TEXT("index"), ParameterIndex)
        && !Params->TryGetNumberField(TEXT("slot"), ParameterIndex))
    {
        ParameterIndex = 0;
    }
    if (ParameterIndex < 0 || ParameterIndex > 3)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'parameter_index' must be in [0, 3], got %d"), ParameterIndex));
    }

    // ParamNames: 4-entry channel-named list. Accept either an array
    // of strings (mapped to R / G / B / A in order) or an object
    // {r, g, b, a} (case-insensitive keys). Missing channels stay at
    // the engine-default `NAME_None`.
    TArray<FName> ChannelNames;
    ChannelNames.Init(NAME_None, 4);
    const TCHAR* ChannelKeys[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };

    TSharedPtr<FJsonValue> NamesValue = Params->TryGetField(TEXT("param_names"));
    if (!NamesValue.IsValid())
    {
        NamesValue = Params->TryGetField(TEXT("parameter_names"));
    }
    if (!NamesValue.IsValid())
    {
        NamesValue = Params->TryGetField(TEXT("channel_names"));
    }
    if (NamesValue.IsValid())
    {
        if (NamesValue->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = NamesValue->AsArray();
            for (int32 I = 0; I < FMath::Min(4, Arr.Num()); ++I)
            {
                if (Arr[I].IsValid() && Arr[I]->Type == EJson::String)
                {
                    const FString Name = Arr[I]->AsString();
                    if (!Name.IsEmpty())
                    {
                        ChannelNames[I] = FName(*Name);
                    }
                }
            }
        }
        else if (NamesValue->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = NamesValue->AsObject();
            for (int32 I = 0; I < 4; ++I)
            {
                FString Name;
                if (Obj->TryGetStringField(ChannelKeys[I], Name) && !Name.IsEmpty())
                {
                    ChannelNames[I] = FName(*Name);
                }
            }
        }
    }

    // Position cascade reuses the same DeriveDefaultPosition helper
    // that add_expression / add_constant / add_math / add_uv_node
    // share.
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

    UMaterialExpression* NewExprBase = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionDynamicParameter::StaticClass(), PosX, PosY);
    UMaterialExpressionDynamicParameter* NewExpr = Cast<UMaterialExpressionDynamicParameter>(NewExprBase);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression failed for class 'MaterialExpressionDynamicParameter'"));
    }

    // Land the per-channel name list. The engine sizes ParamNames at
    // 4 entries on construction; we overwrite each slot rather than
    // SetNum so we leave any pre-set defaults the constructor put down
    // in place when the caller passes a partial channel map.
    if (NewExpr->ParamNames.Num() < 4)
    {
        NewExpr->ParamNames.SetNum(4);
    }
    for (int32 I = 0; I < 4; ++I)
    {
        // ParamNames is TArray<FString>; ChannelNames is TArray<FName>.
        NewExpr->ParamNames[I] = ChannelNames[I].ToString();
    }

    NewExpr->ParameterIndex = ParameterIndex;

    // Optional editor-side default value (the FLinearColor preview /
    // fallback the compiler uses when no Niagara driver is bound).
    // Accept a 4-element array `[r, g, b, a]` or an object form
    // `{r, g, b, a}` mirroring `param_names`. A scalar number falls
    // back to (n, n, n, n) for the "drive every channel from a slider"
    // shape.
    TSharedPtr<FJsonValue> DefaultsValue = Params->TryGetField(TEXT("default_values"));
    if (!DefaultsValue.IsValid())
    {
        DefaultsValue = Params->TryGetField(TEXT("defaults"));
    }
    if (!DefaultsValue.IsValid())
    {
        DefaultsValue = Params->TryGetField(TEXT("default_value"));
    }
    bool bWroteDefaultValue = false;
    if (DefaultsValue.IsValid())
    {
        FLinearColor NewDefault = NewExpr->DefaultValue;
        if (DefaultsValue->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = DefaultsValue->AsArray();
            if (Arr.Num() >= 1) NewDefault.R = static_cast<float>(Arr[0]->AsNumber());
            if (Arr.Num() >= 2) NewDefault.G = static_cast<float>(Arr[1]->AsNumber());
            if (Arr.Num() >= 3) NewDefault.B = static_cast<float>(Arr[2]->AsNumber());
            if (Arr.Num() >= 4) NewDefault.A = static_cast<float>(Arr[3]->AsNumber());
            bWroteDefaultValue = true;
        }
        else if (DefaultsValue->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = DefaultsValue->AsObject();
            double Tmp = 0.0;
            if (Obj->TryGetNumberField(TEXT("r"), Tmp)) NewDefault.R = static_cast<float>(Tmp);
            if (Obj->TryGetNumberField(TEXT("g"), Tmp)) NewDefault.G = static_cast<float>(Tmp);
            if (Obj->TryGetNumberField(TEXT("b"), Tmp)) NewDefault.B = static_cast<float>(Tmp);
            if (Obj->TryGetNumberField(TEXT("a"), Tmp)) NewDefault.A = static_cast<float>(Tmp);
            bWroteDefaultValue = true;
        }
        else if (DefaultsValue->Type == EJson::Number)
        {
            const float N = static_cast<float>(DefaultsValue->AsNumber());
            NewDefault = FLinearColor(N, N, N, N);
            bWroteDefaultValue = true;
        }
        NewExpr->DefaultValue = NewDefault;
    }

    // Optional `name` renames the spawned node for follow-up wiring.
    FString NameOverride;
    if (Params->TryGetStringField(TEXT("name"), NameOverride) && !NameOverride.IsEmpty())
    {
        NewExpr->Rename(*NameOverride, NewExpr->GetOuter(), REN_DontCreateRedirectors);
    }

    // Optional flat `properties` dict for any additional UPROPERTY
    // overrides (rare; the dedicated parameter_index / param_names /
    // default_values knobs above cover the supported surface). Same
    // shape as the other add_* ops.
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

    // Optional one-shot downstream wiring (same shape as add_uv_node /
    // add_constant): wire one of the new node's outputs to either a
    // material attribute (`property`) or another named expression's
    // input pin (`connect_to` + `connect_input`).
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

    // Surface the resolved per-channel name list so a follow-up call
    // can verify the bytes landed without re-reading the expression.
    TArray<TSharedPtr<FJsonValue>> NamesOut;
    for (int32 I = 0; I < 4; ++I)
    {
        // ParamNames entries are already FString.
        NamesOut.Add(MakeShared<FJsonValueString>(NewExpr->ParamNames[I]));
    }

    TSharedPtr<FJsonObject> DefaultOut = MakeShared<FJsonObject>();
    DefaultOut->SetNumberField(TEXT("r"), NewExpr->DefaultValue.R);
    DefaultOut->SetNumberField(TEXT("g"), NewExpr->DefaultValue.G);
    DefaultOut->SetNumberField(TEXT("b"), NewExpr->DefaultValue.B);
    DefaultOut->SetNumberField(TEXT("a"), NewExpr->DefaultValue.A);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_dynamic_parameter"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetNumberField(TEXT("parameter_index"), ParameterIndex);
    ResultObj->SetArrayField(TEXT("param_names"), NamesOut);
    ResultObj->SetObjectField(TEXT("default_value"), DefaultOut);
    ResultObj->SetBoolField(TEXT("wrote_default_value"), bWroteDefaultValue);
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
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::AddFresnel(const TSharedPtr<FJsonObject>& Params)
{
    // Spawns a UMaterialExpressionFresnel on a target material's
    // graph. The Fresnel expression generates the view-angle falloff
    // most commonly wired into a material's EmissiveColor (rim light)
    // or Opacity (edge fade) input. The engine surfaces three
    // editor-side knobs on the expression: `Exponent` (float; the
    // falloff sharpness), `BaseReflectFraction` (float; the floor
    // value at view angle 0, the Schlick F0 term) and the `Normal` /
    // `CameraVector` input pins (both default to the engine's
    // pixel-shader-side world-space inputs when left empty).
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

    // Position cascade reuses the same DeriveDefaultPosition helper
    // that add_expression / add_constant / add_math / add_uv_node /
    // add_dynamic_parameter share.
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

    UMaterialExpression* NewExprBase = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionFresnel::StaticClass(), PosX, PosY);
    UMaterialExpressionFresnel* NewExpr = Cast<UMaterialExpressionFresnel>(NewExprBase);
    if (!NewExpr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("CreateMaterialExpression failed for class 'MaterialExpressionFresnel'"));
    }

    // Capture the engine-default values so the response carries the
    // before / after pair on a single round trip.
    const float DefaultExponent = NewExpr->Exponent;
    const float DefaultBaseReflectFraction = NewExpr->BaseReflectFraction;

    // Optional Exponent (falloff sharpness; UE default 5.0). The
    // engine clamps the runtime side, but a sane caller still wants
    // to land 1.0 / 2.0 / 5.0 for the classic preset shapes.
    bool bWroteExponent = false;
    double ExponentValue = 0.0;
    if (Params->TryGetNumberField(TEXT("Exponent"), ExponentValue)
        || Params->TryGetNumberField(TEXT("exponent"), ExponentValue)
        || Params->TryGetNumberField(TEXT("falloff"), ExponentValue)
        || Params->TryGetNumberField(TEXT("power"), ExponentValue))
    {
        NewExpr->Exponent = static_cast<float>(ExponentValue);
        bWroteExponent = true;
    }

    // Optional BaseReflectFraction (the Schlick F0 floor at view
    // angle 0). UE default is 0.04 (the canonical dielectric F0).
    bool bWroteBaseReflectFraction = false;
    double BaseReflectFractionValue = 0.0;
    if (Params->TryGetNumberField(TEXT("BaseReflectFraction"), BaseReflectFractionValue)
        || Params->TryGetNumberField(TEXT("base_reflect_fraction"), BaseReflectFractionValue)
        || Params->TryGetNumberField(TEXT("base_reflect"), BaseReflectFractionValue)
        || Params->TryGetNumberField(TEXT("f0"), BaseReflectFractionValue)
        || Params->TryGetNumberField(TEXT("F0"), BaseReflectFractionValue))
    {
        NewExpr->BaseReflectFraction = static_cast<float>(BaseReflectFractionValue);
        bWroteBaseReflectFraction = true;
    }

    // Optional `name` renames the spawned node for follow-up wiring.
    FString NameOverride;
    if (Params->TryGetStringField(TEXT("name"), NameOverride) && !NameOverride.IsEmpty())
    {
        NewExpr->Rename(*NameOverride, NewExpr->GetOuter(), REN_DontCreateRedirectors);
    }

    // Optional flat properties dict mirrors the other add_* ops for
    // anything the dedicated knobs above do not cover.
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

    // Optional input wiring. The Fresnel expression carries two
    // input pins: `Normal` and `CameraVector`. Both default to the
    // engine's pixel-shader-side world-space inputs when left empty,
    // which covers the common rim-light case. A caller who needs a
    // tangent-space normal, a custom view vector, or a Niagara
    // sprite alignment vector can wire a named sibling expression
    // into either pin through ConnectMaterialExpressions.
    bool bWroteNormalInput = false;
    bool bWroteCameraVectorInput = false;
    FString NormalSource, NormalOutput;
    if (Params->TryGetStringField(TEXT("Normal"), NormalSource)
        || Params->TryGetStringField(TEXT("normal"), NormalSource)
        || Params->TryGetStringField(TEXT("normal_input"), NormalSource))
    {
        Params->TryGetStringField(TEXT("normal_output"), NormalOutput);
        UMaterialExpression* SourceExpr = FindExpressionByName(Material, NormalSource);
        if (!SourceExpr)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("'normal' source '%s' not found on material"), *NormalSource));
        }
        if (UMaterialEditingLibrary::ConnectMaterialExpressions(SourceExpr, NormalOutput, NewExpr, TEXT("Normal")))
        {
            bWroteNormalInput = true;
        }
    }
    FString CameraVectorSource, CameraVectorOutput;
    if (Params->TryGetStringField(TEXT("CameraVector"), CameraVectorSource)
        || Params->TryGetStringField(TEXT("camera_vector"), CameraVectorSource)
        || Params->TryGetStringField(TEXT("camera_vector_input"), CameraVectorSource)
        || Params->TryGetStringField(TEXT("view"), CameraVectorSource)
        || Params->TryGetStringField(TEXT("view_vector"), CameraVectorSource))
    {
        Params->TryGetStringField(TEXT("camera_vector_output"), CameraVectorOutput);
        UMaterialExpression* SourceExpr = FindExpressionByName(Material, CameraVectorSource);
        if (!SourceExpr)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("'camera_vector' source '%s' not found on material"), *CameraVectorSource));
        }
        if (UMaterialEditingLibrary::ConnectMaterialExpressions(SourceExpr, CameraVectorOutput, NewExpr, TEXT("CameraVector")))
        {
            bWroteCameraVectorInput = true;
        }
    }

    // Optional one-shot downstream wiring (same shape as the other
    // add_* ops): wire the new node's output to either a material
    // attribute (`property`) or another named expression's input pin
    // (`connect_to` + `connect_input`).
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_fresnel"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetObjectField(TEXT("expression"), ExpressionToSummary(NewExpr));
    ResultObj->SetNumberField(TEXT("exponent"), NewExpr->Exponent);
    ResultObj->SetNumberField(TEXT("base_reflect_fraction"), NewExpr->BaseReflectFraction);
    ResultObj->SetNumberField(TEXT("default_exponent"), DefaultExponent);
    ResultObj->SetNumberField(TEXT("default_base_reflect_fraction"), DefaultBaseReflectFraction);
    ResultObj->SetBoolField(TEXT("wrote_exponent"), bWroteExponent);
    ResultObj->SetBoolField(TEXT("wrote_base_reflect_fraction"), bWroteBaseReflectFraction);
    ResultObj->SetBoolField(TEXT("wrote_normal_input"), bWroteNormalInput);
    ResultObj->SetBoolField(TEXT("wrote_camera_vector_input"), bWroteCameraVectorInput);
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

namespace
{
    /** Resolve a user-supplied blend-mode token to the EBlendMode enum
     *  the master UMaterial stores. Case-insensitive; accepts both the
     *  bare token (`Opaque`, `Masked`, etc.) and the engine's
     *  fully-qualified UPROPERTY meta form (`BLEND_Opaque` etc.). The
     *  set mirrors the engine's EBlendMode declarations through
     *  UE 5.7 (the alpha-composite / alpha-holdout pair landed in 5.0
     *  and the rest are pre-4.0). */
    bool ResolveBlendModeToken(const FString& InToken, EBlendMode& OutMode, FString& OutCanonical)
    {
        FString T = InToken.TrimStartAndEnd().ToLower();
        if (T.StartsWith(TEXT("blend_")))
        {
            T = T.RightChop(6);
        }
        T = T.Replace(TEXT("_"), TEXT(""));
        T = T.Replace(TEXT(" "), TEXT(""));
        T = T.Replace(TEXT("-"), TEXT(""));

        if (T == TEXT("opaque"))
        {
            OutMode = BLEND_Opaque;
            OutCanonical = TEXT("Opaque");
            return true;
        }
        if (T == TEXT("masked") || T == TEXT("mask"))
        {
            OutMode = BLEND_Masked;
            OutCanonical = TEXT("Masked");
            return true;
        }
        if (T == TEXT("translucent"))
        {
            OutMode = BLEND_Translucent;
            OutCanonical = TEXT("Translucent");
            return true;
        }
        if (T == TEXT("additive"))
        {
            OutMode = BLEND_Additive;
            OutCanonical = TEXT("Additive");
            return true;
        }
        if (T == TEXT("modulate"))
        {
            OutMode = BLEND_Modulate;
            OutCanonical = TEXT("Modulate");
            return true;
        }
        if (T == TEXT("alphacomposite") || T == TEXT("premultiplied"))
        {
            OutMode = BLEND_AlphaComposite;
            OutCanonical = TEXT("AlphaComposite");
            return true;
        }
        if (T == TEXT("alphaholdout") || T == TEXT("holdout"))
        {
            OutMode = BLEND_AlphaHoldout;
            OutCanonical = TEXT("AlphaHoldout");
            return true;
        }
        return false;
    }

    FString BlendModeToCanonicalToken(EBlendMode Mode)
    {
        switch (Mode)
        {
        case BLEND_Opaque: return TEXT("Opaque");
        case BLEND_Masked: return TEXT("Masked");
        case BLEND_Translucent: return TEXT("Translucent");
        case BLEND_Additive: return TEXT("Additive");
        case BLEND_Modulate: return TEXT("Modulate");
        case BLEND_AlphaComposite: return TEXT("AlphaComposite");
        case BLEND_AlphaHoldout: return TEXT("AlphaHoldout");
        default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(Mode));
        }
    }
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetBlendMode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(Asset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial (Material Instances route through set_attribute_blendable)"), *MaterialPath));
    }

    FString BlendToken;
    if (!Params->TryGetStringField(TEXT("blend_mode"), BlendToken)
        && !Params->TryGetStringField(TEXT("blendmode"), BlendToken)
        && !Params->TryGetStringField(TEXT("mode"), BlendToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blend_mode' parameter (Opaque / Masked / Translucent / Additive / Modulate / AlphaComposite / AlphaHoldout)"));
    }

    EBlendMode NewMode;
    FString CanonicalToken;
    if (!ResolveBlendModeToken(BlendToken, NewMode, CanonicalToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown blend mode '%s'. Supported: Opaque, Masked, Translucent, Additive, Modulate, AlphaComposite, AlphaHoldout"), *BlendToken));
    }

    // Capture the previous values so the response carries the
    // before / after pair on a single round trip.
    const EBlendMode PreviousMode = Material->BlendMode;
    const float PreviousClipValue = Material->OpacityMaskClipValue;
    const FString PreviousToken = BlendModeToCanonicalToken(PreviousMode);

    // Resolve the BlendMode UPROPERTY so PreEditChange / PostEditChange
    // route the static-permutation refresh through the right slot. The
    // engine recompiles the static permutation shaders when the blend
    // mode flips since the translucency pass / depth pass selection
    // changes per blend mode.
    FProperty* BlendModeProperty = Material->GetClass()->FindPropertyByName(TEXT("BlendMode"));
    if (!BlendModeProperty)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Reflection database does not expose 'BlendMode' on UMaterial"));
    }

    // Optional `opacity_mask_clip_value` (alias `opacity_clip` / `clip`).
    // The engine consults this only on Masked, but a caller authoring
    // a Masked + clip-value pair in one round trip wants both knobs on
    // the same op.
    bool bClipProvided = false;
    double ClipValue = 0.0;
    if (Params->TryGetNumberField(TEXT("opacity_mask_clip_value"), ClipValue)
        || Params->TryGetNumberField(TEXT("opacity_clip_value"), ClipValue)
        || Params->TryGetNumberField(TEXT("opacity_clip"), ClipValue)
        || Params->TryGetNumberField(TEXT("clip"), ClipValue)
        || Params->TryGetNumberField(TEXT("OpacityMaskClipValue"), ClipValue))
    {
        bClipProvided = true;
    }

    Material->PreEditChange(BlendModeProperty);
    Material->BlendMode = NewMode;
    if (bClipProvided)
    {
        Material->OpacityMaskClipValue = static_cast<float>(ClipValue);
    }

    // PostEditChangeProperty on BlendMode rebuilds the cached shader
    // permutation map, which is what we need to land here.
    FPropertyChangedEvent ChangeEvent(BlendModeProperty, EPropertyChangeType::ValueSet);
    Material->PostEditChangeProperty(ChangeEvent);

    // If the caller supplied an opacity-mask clip and we did not bundle
    // it into the BlendMode PostEditChange call (the engine groups them
    // in the editor UI), still fire a second PostEditChange against the
    // clip-value UPROPERTY so the displayed value refreshes too.
    if (bClipProvided)
    {
        if (FProperty* ClipProperty = Material->GetClass()->FindPropertyByName(TEXT("OpacityMaskClipValue")))
        {
            FPropertyChangedEvent ClipChange(ClipProperty, EPropertyChangeType::ValueSet);
            Material->PostEditChangeProperty(ClipChange);
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_blend_mode"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("blend_mode"), CanonicalToken);
    ResultObj->SetStringField(TEXT("previous_blend_mode"), PreviousToken);
    ResultObj->SetNumberField(TEXT("opacity_mask_clip_value"), Material->OpacityMaskClipValue);
    ResultObj->SetNumberField(TEXT("previous_opacity_mask_clip_value"), PreviousClipValue);
    ResultObj->SetBoolField(TEXT("clip_value_written"), bClipProvided);
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

namespace
{
    /** Parse a JSON value as a bool. Accepts true / false, numeric 0 / 1,
     *  and the common string tokens. Returns false (with bOk=false) when
     *  the value is not a recognisable bool shape. */
    bool MaterialFlags_ParseBool(const TSharedPtr<FJsonValue>& Value, bool& OutBool)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Boolean)
        {
            OutBool = Value->AsBool();
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            OutBool = (Value->AsNumber() != 0.0);
            return true;
        }
        if (Value->Type == EJson::String)
        {
            FString T = Value->AsString().TrimStartAndEnd().ToLower();
            if (T == TEXT("true") || T == TEXT("1") || T == TEXT("on")
                || T == TEXT("yes") || T == TEXT("enable") || T == TEXT("enabled"))
            {
                OutBool = true;
                return true;
            }
            if (T == TEXT("false") || T == TEXT("0") || T == TEXT("off")
                || T == TEXT("no") || T == TEXT("disable") || T == TEXT("disabled"))
            {
                OutBool = false;
                return true;
            }
            return false;
        }
        return false;
    }

    /** A small allow-list of canonical UMaterial bool UPROPERTY names plus
     *  their casual aliases. The reflection lookup is case-sensitive, so
     *  we keep the canonical names from the engine header (e.g. UMaterial
     *  in `Engine/Classes/Materials/Material.h`) and map common shortcuts
     *  to them. Entries that resolve to a missing UPROPERTY (the field
     *  got renamed in a future engine version) land on the response's
     *  `skipped` array rather than aborting the whole call. */
    bool MaterialFlags_ResolveFieldName(const FString& InName, FString& OutCanonical)
    {
        const FString T = InName.TrimStartAndEnd();

        struct FFieldAlias
        {
            const TCHAR* Alias;
            const TCHAR* Canonical;
        };
        static const FFieldAlias Aliases[] =
        {
            { TEXT("TwoSided"),                       TEXT("TwoSided") },
            { TEXT("two_sided"),                      TEXT("TwoSided") },
            { TEXT("twosided"),                       TEXT("TwoSided") },

            { TEXT("DitheredLODTransition"),          TEXT("DitheredLODTransition") },
            { TEXT("dithered_lod_transition"),        TEXT("DitheredLODTransition") },
            { TEXT("dithered_lod"),                   TEXT("DitheredLODTransition") },
            { TEXT("dither_lod"),                     TEXT("DitheredLODTransition") },

            { TEXT("bUseMaterialAttributes"),         TEXT("bUseMaterialAttributes") },
            { TEXT("use_material_attributes"),        TEXT("bUseMaterialAttributes") },
            { TEXT("UseMaterialAttributes"),          TEXT("bUseMaterialAttributes") },

            { TEXT("bCastDynamicShadowAsMasked"),     TEXT("bCastDynamicShadowAsMasked") },
            { TEXT("cast_dynamic_shadow_as_masked"),  TEXT("bCastDynamicShadowAsMasked") },
            { TEXT("CastDynamicShadowAsMasked"),      TEXT("bCastDynamicShadowAsMasked") },

            { TEXT("bOutputTranslucentVelocity"),     TEXT("bOutputTranslucentVelocity") },
            { TEXT("output_translucent_velocity"),    TEXT("bOutputTranslucentVelocity") },
            { TEXT("OutputTranslucentVelocity"),      TEXT("bOutputTranslucentVelocity") },

            { TEXT("bUsedWithStaticLighting"),        TEXT("bUsedWithStaticLighting") },
            { TEXT("used_with_static_lighting"),      TEXT("bUsedWithStaticLighting") },
            { TEXT("UsedWithStaticLighting"),         TEXT("bUsedWithStaticLighting") },

            { TEXT("bUsedWithSkeletalMesh"),          TEXT("bUsedWithSkeletalMesh") },
            { TEXT("used_with_skeletal_mesh"),        TEXT("bUsedWithSkeletalMesh") },
            { TEXT("UsedWithSkeletalMesh"),           TEXT("bUsedWithSkeletalMesh") },
        };

        for (const FFieldAlias& Entry : Aliases)
        {
            if (T.Equals(Entry.Alias, ESearchCase::IgnoreCase))
            {
                OutCanonical = Entry.Canonical;
                return true;
            }
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetMaterialFlags(const TSharedPtr<FJsonObject>& Params)
{
    // Reflection-driven UMaterial bool flag writer. Mirrors the rest of
    // the hosted material_edit family by exposing a single round-trip for
    // the long tail of bool knobs on UMaterial (TwoSided, DitheredLOD,
    // bUseMaterialAttributes, etc.). Each entry routes through
    // FindPropertyByName + FBoolProperty::SetPropertyValue_InContainer
    // so the op stays compatible with the visibility tightening UE has
    // done across recent versions. Failures land on the response's
    // `skipped` array rather than aborting the whole call. Runs
    // PostEditChangeProperty against each touched UPROPERTY so the
    // static permutation recompiles when the engine cares about it
    // (the TwoSided / bUseMaterialAttributes / bCastDynamicShadowAsMasked
    // flags all invalidate the shader permutation map).
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(Asset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial (Material Instances do not expose these flags directly; route through set_attribute_blendable)"), *MaterialPath));
    }

    // The flat dict lives under any of these aliases so callers can mix
    // the documented `flags` shape with the shorter `bools` or generic
    // `properties` shape we use elsewhere.
    const TSharedPtr<FJsonObject>* FlagsObjPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("flags"), FlagsObjPtr)
        && !Params->TryGetObjectField(TEXT("bools"), FlagsObjPtr)
        && !Params->TryGetObjectField(TEXT("properties"), FlagsObjPtr)
        && !Params->TryGetObjectField(TEXT("values"), FlagsObjPtr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_material_flags: missing 'flags' object (map of bool UPROPERTY names to true/false). Supported: TwoSided, DitheredLODTransition, bUseMaterialAttributes, bCastDynamicShadowAsMasked, bOutputTranslucentVelocity, bUsedWithStaticLighting, bUsedWithSkeletalMesh"));
    }
    const TSharedPtr<FJsonObject>& FlagsObj = *FlagsObjPtr;
    if (!FlagsObj.IsValid() || FlagsObj->Values.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_material_flags: 'flags' object is empty"));
    }

    TArray<TSharedPtr<FJsonValue>> AppliedArr;
    TArray<TSharedPtr<FJsonValue>> SkippedArr;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : FlagsObj->Values)
    {
        const FString& InName = Entry.Key;

        FString Canonical;
        if (!MaterialFlags_ResolveFieldName(InName, Canonical))
        {
            TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
            SkipEntry->SetStringField(TEXT("name"), InName);
            SkipEntry->SetStringField(TEXT("reason"), TEXT("unknown_field"));
            SkipEntry->SetStringField(TEXT("hint"), TEXT("Supported: TwoSided, DitheredLODTransition, bUseMaterialAttributes, bCastDynamicShadowAsMasked, bOutputTranslucentVelocity, bUsedWithStaticLighting, bUsedWithSkeletalMesh"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(SkipEntry));
            continue;
        }

        bool NewBool = false;
        if (!MaterialFlags_ParseBool(Entry.Value, NewBool))
        {
            TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
            SkipEntry->SetStringField(TEXT("name"), InName);
            SkipEntry->SetStringField(TEXT("canonical"), Canonical);
            SkipEntry->SetStringField(TEXT("reason"), TEXT("not_a_bool"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(SkipEntry));
            continue;
        }

        FProperty* Prop = Material->GetClass()->FindPropertyByName(FName(*Canonical));
        if (!Prop)
        {
            TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
            SkipEntry->SetStringField(TEXT("name"), InName);
            SkipEntry->SetStringField(TEXT("canonical"), Canonical);
            SkipEntry->SetStringField(TEXT("reason"), TEXT("uproperty_not_found"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(SkipEntry));
            continue;
        }
        FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop);
        if (!BoolProp)
        {
            TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
            SkipEntry->SetStringField(TEXT("name"), InName);
            SkipEntry->SetStringField(TEXT("canonical"), Canonical);
            SkipEntry->SetStringField(TEXT("reason"), TEXT("not_a_boolproperty"));
            SkipEntry->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());
            SkippedArr.Add(MakeShared<FJsonValueObject>(SkipEntry));
            continue;
        }

        const bool PreviousBool = BoolProp->GetPropertyValue_InContainer(Material);

        // Route the write through PreEditChange / SetPropertyValue /
        // PostEditChangeProperty so the engine treats the change like the
        // editor would. The flags in this list all sit on the shader
        // permutation key, so the PostEditChange call invalidates the
        // cached permutation map and queues a recompile.
        Material->PreEditChange(BoolProp);
        BoolProp->SetPropertyValue_InContainer(Material, NewBool);
        FPropertyChangedEvent ChangeEvent(BoolProp, EPropertyChangeType::ValueSet);
        Material->PostEditChangeProperty(ChangeEvent);

        TSharedPtr<FJsonObject> AppliedEntry = MakeShared<FJsonObject>();
        AppliedEntry->SetStringField(TEXT("name"), InName);
        AppliedEntry->SetStringField(TEXT("canonical"), Canonical);
        AppliedEntry->SetBoolField(TEXT("previous"), PreviousBool);
        AppliedEntry->SetBoolField(TEXT("value"), NewBool);
        AppliedArr.Add(MakeShared<FJsonValueObject>(AppliedEntry));
    }

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);
    // Only recompile when we actually wrote something, otherwise we burn
    // shader compile time on a no-op.
    if (bRecompile && AppliedArr.Num() > 0)
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_material_flags"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedArr);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedArr);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedArr.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile && AppliedArr.Num() > 0);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

namespace
{
    /** Resolve a user-supplied shading-model token to the
     *  EMaterialShadingModel enum value the engine stores on
     *  UMaterial::ShadingModel. The token set mirrors the engine's
     *  EMaterialShadingModel declarations through UE 5.7
     *  (`Engine/Source/Runtime/Engine/Classes/Engine/EngineTypes.h`).
     *  Accepts the bare token (`Unlit`, `DefaultLit`, etc.) and the
     *  fully-qualified UPROPERTY meta form (`MSM_Unlit` etc.); the
     *  match runs case-insensitively after stripping underscores /
     *  spaces / dashes so casual snake_case (`default_lit`,
     *  `single_layer_water`) lands on the same enum value as the
     *  engine spelling. */
    bool ResolveShadingModelToken(const FString& InToken, EMaterialShadingModel& OutModel, FString& OutCanonical)
    {
        FString T = InToken.TrimStartAndEnd().ToLower();
        if (T.StartsWith(TEXT("msm_")))
        {
            T = T.RightChop(4);
        }
        T = T.Replace(TEXT("_"), TEXT(""));
        T = T.Replace(TEXT(" "), TEXT(""));
        T = T.Replace(TEXT("-"), TEXT(""));

        if (T == TEXT("unlit"))
        {
            OutModel = MSM_Unlit;
            OutCanonical = TEXT("Unlit");
            return true;
        }
        if (T == TEXT("defaultlit") || T == TEXT("lit") || T == TEXT("default"))
        {
            OutModel = MSM_DefaultLit;
            OutCanonical = TEXT("DefaultLit");
            return true;
        }
        if (T == TEXT("subsurface"))
        {
            OutModel = MSM_Subsurface;
            OutCanonical = TEXT("Subsurface");
            return true;
        }
        if (T == TEXT("preintegratedskin") || T == TEXT("preintegrated") || T == TEXT("skin"))
        {
            OutModel = MSM_PreintegratedSkin;
            OutCanonical = TEXT("PreintegratedSkin");
            return true;
        }
        if (T == TEXT("clearcoat") || T == TEXT("coat"))
        {
            OutModel = MSM_ClearCoat;
            OutCanonical = TEXT("ClearCoat");
            return true;
        }
        if (T == TEXT("subsurfaceprofile") || T == TEXT("subsurfaceprofileshading"))
        {
            OutModel = MSM_SubsurfaceProfile;
            OutCanonical = TEXT("SubsurfaceProfile");
            return true;
        }
        if (T == TEXT("twosidedfoliage") || T == TEXT("foliage"))
        {
            OutModel = MSM_TwoSidedFoliage;
            OutCanonical = TEXT("TwoSidedFoliage");
            return true;
        }
        if (T == TEXT("hair"))
        {
            OutModel = MSM_Hair;
            OutCanonical = TEXT("Hair");
            return true;
        }
        if (T == TEXT("cloth"))
        {
            OutModel = MSM_Cloth;
            OutCanonical = TEXT("Cloth");
            return true;
        }
        if (T == TEXT("eye"))
        {
            OutModel = MSM_Eye;
            OutCanonical = TEXT("Eye");
            return true;
        }
        if (T == TEXT("singlelayerwater") || T == TEXT("water"))
        {
            OutModel = MSM_SingleLayerWater;
            OutCanonical = TEXT("SingleLayerWater");
            return true;
        }
        if (T == TEXT("thintranslucent") || T == TEXT("thintranslucency"))
        {
            OutModel = MSM_ThinTranslucent;
            OutCanonical = TEXT("ThinTranslucent");
            return true;
        }
        return false;
    }

    FString ShadingModelToCanonicalToken(EMaterialShadingModel Model)
    {
        switch (Model)
        {
        case MSM_Unlit:              return TEXT("Unlit");
        case MSM_DefaultLit:         return TEXT("DefaultLit");
        case MSM_Subsurface:         return TEXT("Subsurface");
        case MSM_PreintegratedSkin:  return TEXT("PreintegratedSkin");
        case MSM_ClearCoat:          return TEXT("ClearCoat");
        case MSM_SubsurfaceProfile:  return TEXT("SubsurfaceProfile");
        case MSM_TwoSidedFoliage:    return TEXT("TwoSidedFoliage");
        case MSM_Hair:               return TEXT("Hair");
        case MSM_Cloth:              return TEXT("Cloth");
        case MSM_Eye:                return TEXT("Eye");
        case MSM_SingleLayerWater:   return TEXT("SingleLayerWater");
        case MSM_ThinTranslucent:    return TEXT("ThinTranslucent");
        default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(Model));
        }
    }
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetShadingModel(const TSharedPtr<FJsonObject>& Params)
{
    // Writer for UMaterial::ShadingModel. The engine carries a paired
    // ShadingModels bitset (FMaterialShadingModelField; the multi-model
    // surface From Material attributes lit), but the legacy single
    // ShadingModel enum is what the static permutation key consults on
    // the BasePass shader compile when bUseMaterialAttributes is off;
    // we route through the enum slot since that's the documented
    // single-shading-model authoring path. Translation to the bitset is
    // engine-internal (UMaterial::RebuildShadingModelField runs on
    // PostEditChangeProperty so it stays consistent). Refuses Material
    // Instances since the override surface lives on
    // FMaterialInstanceBasePropertyOverrides::ShadingModel and routes
    // through set_attribute_blendable.
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(Asset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial (Material Instances route through set_attribute_blendable)"), *MaterialPath));
    }

    FString ShadingToken;
    if (!Params->TryGetStringField(TEXT("shading_model"), ShadingToken)
        && !Params->TryGetStringField(TEXT("shadingmodel"), ShadingToken)
        && !Params->TryGetStringField(TEXT("model"), ShadingToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'shading_model' parameter (Unlit / DefaultLit / Subsurface / PreintegratedSkin / ClearCoat / SubsurfaceProfile / TwoSidedFoliage / Hair / Cloth / Eye / SingleLayerWater / ThinTranslucent)"));
    }

    EMaterialShadingModel NewModel;
    FString CanonicalToken;
    if (!ResolveShadingModelToken(ShadingToken, NewModel, CanonicalToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown shading model '%s'. Supported: Unlit, DefaultLit, Subsurface, PreintegratedSkin, ClearCoat, SubsurfaceProfile, TwoSidedFoliage, Hair, Cloth, Eye, SingleLayerWater, ThinTranslucent"), *ShadingToken));
    }

    // Capture the previous value for the diff payload.
    const EMaterialShadingModel PreviousModel = Material->GetShadingModels().GetFirstShadingModel();
    const FString PreviousToken = ShadingModelToCanonicalToken(PreviousModel);

    // Resolve the ShadingModel UPROPERTY so PreEditChange / PostEditChange
    // route the static-permutation refresh through the right slot. The
    // engine's PostEditChangeProperty on ShadingModel runs
    // RebuildShadingModelField, which keeps the paired ShadingModels
    // bitset in sync, plus invalidates the shader permutation map since
    // the basepass shaders fork per shading model.
    FProperty* ShadingModelProperty = Material->GetClass()->FindPropertyByName(TEXT("ShadingModel"));
    if (!ShadingModelProperty)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Reflection database does not expose 'ShadingModel' on UMaterial"));
    }

    Material->PreEditChange(ShadingModelProperty);
    // UMaterial::ShadingModel is private; SetShadingModel lands the value
    // and keeps the paired ShadingModels bitset in sync.
    Material->SetShadingModel(NewModel);

    // PostEditChangeProperty on ShadingModel rebuilds the cached shader
    // permutation map plus the ShadingModels bitset, which is what we
    // need to land here.
    FPropertyChangedEvent ChangeEvent(ShadingModelProperty, EPropertyChangeType::ValueSet);
    Material->PostEditChangeProperty(ChangeEvent);

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
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_shading_model"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetStringField(TEXT("shading_model"), CanonicalToken);
    ResultObj->SetStringField(TEXT("previous_shading_model"), PreviousToken);
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}

namespace
{
    /** Normalise a user-supplied translucency knob name onto the canonical
     *  UMaterial UPROPERTY spelling. The reflection lookup is case-sensitive
     *  so we keep the engine spelling (from
     *  `Engine/Source/Runtime/Engine/Classes/Materials/Material.h`) and map
     *  common shortcuts to it. Entries that resolve to a missing UPROPERTY
     *  (the field got renamed in a future engine version) land on the
     *  response's `skipped` array rather than aborting the whole call. */
    bool TranslucencySettings_ResolveFieldName(const FString& InName, FString& OutCanonical)
    {
        const FString T = InName.TrimStartAndEnd();
        struct FFieldAlias
        {
            const TCHAR* Alias;
            const TCHAR* Canonical;
        };
        static const FFieldAlias Aliases[] =
        {
            { TEXT("TranslucencyLightingMode"),               TEXT("TranslucencyLightingMode") },
            { TEXT("translucency_lighting_mode"),             TEXT("TranslucencyLightingMode") },
            { TEXT("lighting_mode"),                          TEXT("TranslucencyLightingMode") },

            { TEXT("TranslucentShadowDensityScale"),          TEXT("TranslucentShadowDensityScale") },
            { TEXT("translucent_shadow_density_scale"),       TEXT("TranslucentShadowDensityScale") },
            { TEXT("shadow_density_scale"),                   TEXT("TranslucentShadowDensityScale") },

            { TEXT("TranslucentSelfShadowDensityScale"),      TEXT("TranslucentSelfShadowDensityScale") },
            { TEXT("translucent_self_shadow_density_scale"),  TEXT("TranslucentSelfShadowDensityScale") },
            { TEXT("self_shadow_density_scale"),              TEXT("TranslucentSelfShadowDensityScale") },

            { TEXT("TranslucentBackscatteringExponent"),      TEXT("TranslucentBackscatteringExponent") },
            { TEXT("translucent_backscattering_exponent"),    TEXT("TranslucentBackscatteringExponent") },
            { TEXT("backscattering_exponent"),                TEXT("TranslucentBackscatteringExponent") },

            { TEXT("bScreenSpaceReflections"),                TEXT("bScreenSpaceReflections") },
            { TEXT("ScreenSpaceReflections"),                 TEXT("bScreenSpaceReflections") },
            { TEXT("screen_space_reflections"),               TEXT("bScreenSpaceReflections") },
            { TEXT("ssr"),                                    TEXT("bScreenSpaceReflections") },

            { TEXT("bUseTranslucencyVertexFog"),              TEXT("bUseTranslucencyVertexFog") },
            { TEXT("UseTranslucencyVertexFog"),               TEXT("bUseTranslucencyVertexFog") },
            { TEXT("use_translucency_vertex_fog"),            TEXT("bUseTranslucencyVertexFog") },
            { TEXT("vertex_fog"),                             TEXT("bUseTranslucencyVertexFog") },
        };
        for (const FFieldAlias& Entry : Aliases)
        {
            if (T.Equals(Entry.Alias, ESearchCase::IgnoreCase))
            {
                OutCanonical = Entry.Canonical;
                return true;
            }
        }
        return false;
    }

    /** Resolve a user-supplied translucency-lighting-mode token onto the
     *  ETranslucencyLightingMode enum value the engine stores in
     *  TEnumAsByte<ETranslucencyLightingMode> on UMaterial. The token set
     *  mirrors the engine's ETranslucencyLightingMode declarations through
     *  UE 5.7 (`Engine/Source/Runtime/Engine/Classes/Engine/EngineTypes.h`).
     *  Accepts the bare token plus the `TLM_` prefix variant; the match
     *  runs case-insensitively after stripping underscores / spaces /
     *  dashes so casual snake_case lands on the same enum value. */
    bool ResolveTranslucencyLightingModeToken(const FString& InToken, uint8& OutByteValue, FString& OutCanonical)
    {
        FString T = InToken.TrimStartAndEnd().ToLower();
        if (T.StartsWith(TEXT("tlm_")))
        {
            T = T.RightChop(4);
        }
        T = T.Replace(TEXT("_"), TEXT(""));
        T = T.Replace(TEXT(" "), TEXT(""));
        T = T.Replace(TEXT("-"), TEXT(""));

        if (T == TEXT("volumetricnondirectional"))
        {
            OutByteValue = static_cast<uint8>(TLM_VolumetricNonDirectional);
            OutCanonical = TEXT("VolumetricNonDirectional");
            return true;
        }
        if (T == TEXT("volumetricdirectional"))
        {
            OutByteValue = static_cast<uint8>(TLM_VolumetricDirectional);
            OutCanonical = TEXT("VolumetricDirectional");
            return true;
        }
        if (T == TEXT("volumetricpervertexnondirectional"))
        {
            OutByteValue = static_cast<uint8>(TLM_VolumetricPerVertexNonDirectional);
            OutCanonical = TEXT("VolumetricPerVertexNonDirectional");
            return true;
        }
        if (T == TEXT("volumetricpervertexdirectional"))
        {
            OutByteValue = static_cast<uint8>(TLM_VolumetricPerVertexDirectional);
            OutCanonical = TEXT("VolumetricPerVertexDirectional");
            return true;
        }
        if (T == TEXT("surface") || T == TEXT("surfacenondirectional"))
        {
            OutByteValue = static_cast<uint8>(TLM_Surface);
            OutCanonical = TEXT("Surface");
            return true;
        }
        if (T == TEXT("surfaceperpixellighting"))
        {
            OutByteValue = static_cast<uint8>(TLM_SurfacePerPixelLighting);
            OutCanonical = TEXT("SurfacePerPixelLighting");
            return true;
        }
        if (T == TEXT("surfaceforwardshading") || T == TEXT("forward") || T == TEXT("forwardshading"))
        {
            // UE 5.7 folded the old TLM_SurfaceForwardShading into
            // TLM_SurfacePerPixelLighting (its DisplayName is still
            // "Surface ForwardShading"); the forward-shading aliases map
            // onto that value.
            OutByteValue = static_cast<uint8>(TLM_SurfacePerPixelLighting);
            OutCanonical = TEXT("SurfacePerPixelLighting");
            return true;
        }
        return false;
    }

    FString TranslucencyLightingModeToCanonicalToken(uint8 InByte)
    {
        const ETranslucencyLightingMode Mode = static_cast<ETranslucencyLightingMode>(InByte);
        switch (Mode)
        {
        case TLM_VolumetricNonDirectional:           return TEXT("VolumetricNonDirectional");
        case TLM_VolumetricDirectional:              return TEXT("VolumetricDirectional");
        case TLM_VolumetricPerVertexNonDirectional:  return TEXT("VolumetricPerVertexNonDirectional");
        case TLM_VolumetricPerVertexDirectional:     return TEXT("VolumetricPerVertexDirectional");
        case TLM_Surface:                            return TEXT("Surface");
        case TLM_SurfacePerPixelLighting:            return TEXT("SurfacePerPixelLighting");
        default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InByte));
        }
    }
}

TSharedPtr<FJsonObject> FSproftMaterialEditCommands::SetTranslucencySettings(const TSharedPtr<FJsonObject>& Params)
{
    // Reflection-driven writer for the translucency block on UMaterial.
    // The engine carries a long set of translucency knobs in scattered
    // UPROPERTYs across UMaterial; the typical authoring workflow opens
    // the material editor's Translucency category and lands a small
    // subset by hand. This op covers the high-traffic subset documented
    // in the README addition. Each entry routes through FindPropertyByName
    // + the matching FBoolProperty / FByteProperty (the
    // TEnumAsByte<ETranslucencyLightingMode>) / FFloatProperty setter so
    // the op stays compatible with the visibility tightening UE has done
    // across recent versions. Failures land on the response's `skipped`
    // array rather than aborting the whole call. PostEditChangeProperty
    // fires per-touched UPROPERTY so the static permutation invalidates
    // when the engine cares about it (TranslucencyLightingMode is one of
    // the permutation-key drivers on the basepass shader compile for
    // translucent passes).
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material' parameter (path to a UMaterial)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    UMaterial* Material = Cast<UMaterial>(Asset);
    if (!Material)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMaterial (Material Instances do not expose a paired translucency override block; the FMaterialInstanceBasePropertyOverrides struct only covers blend_mode / shading_model / opacity_mask_clip_value / two_sided etc.)"), *MaterialPath));
    }

    const TSharedPtr<FJsonObject>* SettingsObjPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("settings"), SettingsObjPtr)
        && !Params->TryGetObjectField(TEXT("translucency"), SettingsObjPtr)
        && !Params->TryGetObjectField(TEXT("properties"), SettingsObjPtr)
        && !Params->TryGetObjectField(TEXT("values"), SettingsObjPtr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_translucency_settings: missing 'settings' object. Supported keys: TranslucencyLightingMode, TranslucentShadowDensityScale, TranslucentSelfShadowDensityScale, TranslucentBackscatteringExponent, bScreenSpaceReflections, bUseTranslucencyVertexFog"));
    }
    const TSharedPtr<FJsonObject>& SettingsObj = *SettingsObjPtr;
    if (!SettingsObj.IsValid() || SettingsObj->Values.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_translucency_settings: 'settings' object is empty"));
    }

    TArray<TSharedPtr<FJsonValue>> AppliedArr;
    TArray<TSharedPtr<FJsonValue>> SkippedArr;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : SettingsObj->Values)
    {
        const FString& InName = Entry.Key;

        FString Canonical;
        if (!TranslucencySettings_ResolveFieldName(InName, Canonical))
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), InName);
            Skip->SetStringField(TEXT("reason"), TEXT("unknown_field"));
            Skip->SetStringField(TEXT("hint"), TEXT("Supported: TranslucencyLightingMode, TranslucentShadowDensityScale, TranslucentSelfShadowDensityScale, TranslucentBackscatteringExponent, bScreenSpaceReflections, bUseTranslucencyVertexFog"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        FProperty* Prop = Material->GetClass()->FindPropertyByName(FName(*Canonical));
        if (!Prop)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), InName);
            Skip->SetStringField(TEXT("canonical"), Canonical);
            Skip->SetStringField(TEXT("reason"), TEXT("uproperty_not_found"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        // Route by the concrete property type so the byte-enum and bool
        // bitfield cases both land on the right setter.
        if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
        {
            bool NewBool = false;
            if (!MaterialFlags_ParseBool(Entry.Value, NewBool))
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), InName);
                Skip->SetStringField(TEXT("canonical"), Canonical);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_bool"));
                SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const bool PreviousBool = BoolProp->GetPropertyValue_InContainer(Material);

            Material->PreEditChange(BoolProp);
            BoolProp->SetPropertyValue_InContainer(Material, NewBool);
            FPropertyChangedEvent ChangeEvent(BoolProp, EPropertyChangeType::ValueSet);
            Material->PostEditChangeProperty(ChangeEvent);

            TSharedPtr<FJsonObject> AppliedEntry = MakeShared<FJsonObject>();
            AppliedEntry->SetStringField(TEXT("name"), InName);
            AppliedEntry->SetStringField(TEXT("canonical"), Canonical);
            AppliedEntry->SetStringField(TEXT("kind"), TEXT("bool"));
            AppliedEntry->SetBoolField(TEXT("previous"), PreviousBool);
            AppliedEntry->SetBoolField(TEXT("value"), NewBool);
            AppliedArr.Add(MakeShared<FJsonValueObject>(AppliedEntry));
            continue;
        }

        if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
        {
            // The only byte / enum UPROPERTY in this set is
            // TranslucencyLightingMode, which the engine declares as
            // TEnumAsByte<ETranslucencyLightingMode>. Resolve through the
            // token table; numbers also accepted as a fallback so callers
            // that already have the int value can pass it.
            uint8 NewByte = 0;
            FString CanonicalToken;
            bool bResolved = false;
            if (Entry.Value.IsValid() && Entry.Value->Type == EJson::String)
            {
                bResolved = ResolveTranslucencyLightingModeToken(Entry.Value->AsString(), NewByte, CanonicalToken);
            }
            else if (Entry.Value.IsValid() && Entry.Value->Type == EJson::Number)
            {
                NewByte = static_cast<uint8>(Entry.Value->AsNumber());
                CanonicalToken = TranslucencyLightingModeToCanonicalToken(NewByte);
                bResolved = !CanonicalToken.StartsWith(TEXT("Unknown("));
            }

            if (!bResolved)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), InName);
                Skip->SetStringField(TEXT("canonical"), Canonical);
                Skip->SetStringField(TEXT("reason"), TEXT("unknown_translucency_lighting_mode"));
                Skip->SetStringField(TEXT("hint"), TEXT("Supported: VolumetricNonDirectional, VolumetricDirectional, VolumetricPerVertexNonDirectional, VolumetricPerVertexDirectional, Surface, SurfacePerPixelLighting, SurfaceForwardShading"));
                SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }

            const uint8 PreviousByte = ByteProp->GetPropertyValue_InContainer(Material);

            Material->PreEditChange(ByteProp);
            ByteProp->SetPropertyValue_InContainer(Material, NewByte);
            FPropertyChangedEvent ChangeEvent(ByteProp, EPropertyChangeType::ValueSet);
            Material->PostEditChangeProperty(ChangeEvent);

            TSharedPtr<FJsonObject> AppliedEntry = MakeShared<FJsonObject>();
            AppliedEntry->SetStringField(TEXT("name"), InName);
            AppliedEntry->SetStringField(TEXT("canonical"), Canonical);
            AppliedEntry->SetStringField(TEXT("kind"), TEXT("enum"));
            AppliedEntry->SetStringField(TEXT("previous"), TranslucencyLightingModeToCanonicalToken(PreviousByte));
            AppliedEntry->SetStringField(TEXT("value"), CanonicalToken);
            AppliedArr.Add(MakeShared<FJsonValueObject>(AppliedEntry));
            continue;
        }

        if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
        {
            if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Number)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), InName);
                Skip->SetStringField(TEXT("canonical"), Canonical);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_number"));
                SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            const float PreviousFloat = FloatProp->GetPropertyValue_InContainer(Material);
            const float NewFloat = static_cast<float>(Entry.Value->AsNumber());

            Material->PreEditChange(FloatProp);
            FloatProp->SetPropertyValue_InContainer(Material, NewFloat);
            FPropertyChangedEvent ChangeEvent(FloatProp, EPropertyChangeType::ValueSet);
            Material->PostEditChangeProperty(ChangeEvent);

            TSharedPtr<FJsonObject> AppliedEntry = MakeShared<FJsonObject>();
            AppliedEntry->SetStringField(TEXT("name"), InName);
            AppliedEntry->SetStringField(TEXT("canonical"), Canonical);
            AppliedEntry->SetStringField(TEXT("kind"), TEXT("float"));
            AppliedEntry->SetNumberField(TEXT("previous"), PreviousFloat);
            AppliedEntry->SetNumberField(TEXT("value"), NewFloat);
            AppliedArr.Add(MakeShared<FJsonValueObject>(AppliedEntry));
            continue;
        }

        // Anything else (a renamed field that turned into a struct, etc.)
        // surfaces under skipped with the cpp type so the caller can see
        // what the field resolved to.
        TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
        Skip->SetStringField(TEXT("name"), InName);
        Skip->SetStringField(TEXT("canonical"), Canonical);
        Skip->SetStringField(TEXT("reason"), TEXT("unsupported_property_type"));
        Skip->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());
        SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
    }

    bool bRecompile = true;
    Params->TryGetBoolField(TEXT("recompile"), bRecompile);
    if (bRecompile && AppliedArr.Num() > 0)
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
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_translucency_settings"));
    ResultObj->SetStringField(TEXT("material"), Material->GetPathName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedArr);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedArr);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedArr.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
    ResultObj->SetBoolField(TEXT("recompiled"), bRecompile && AppliedArr.Num() > 0);
    ResultObj->SetBoolField(TEXT("saved"), bSave);
    return ResultObj;
}
