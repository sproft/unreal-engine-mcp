#include "Commands/SproftMaterialInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialExpressionIO.h"
#include "SceneTypes.h"
#include "UObject/Class.h"

namespace
{
    /** A short ASCII identifier for an EMaterialProperty value, for the
     *  per-attribute connection report. */
    struct FAttributeProbe
    {
        EMaterialProperty Property;
        const TCHAR*      Label;
    };

    static const FAttributeProbe GAttributeProbes[] =
    {
        { MP_BaseColor,           TEXT("BaseColor") },
        { MP_Metallic,            TEXT("Metallic") },
        { MP_Specular,            TEXT("Specular") },
        { MP_Roughness,           TEXT("Roughness") },
        { MP_Anisotropy,          TEXT("Anisotropy") },
        { MP_Normal,              TEXT("Normal") },
        { MP_Tangent,             TEXT("Tangent") },
        { MP_EmissiveColor,       TEXT("EmissiveColor") },
        { MP_Opacity,             TEXT("Opacity") },
        { MP_OpacityMask,         TEXT("OpacityMask") },
        { MP_WorldPositionOffset, TEXT("WorldPositionOffset") },
        { MP_AmbientOcclusion,    TEXT("AmbientOcclusion") },
        { MP_Refraction,          TEXT("Refraction") },
        { MP_Displacement,        TEXT("Displacement") },
    };

    TSharedPtr<FJsonObject> ExpressionToJson(UMaterialExpression* Expr)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
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

    void AppendParameterNames(UMaterialInterface* Material, TArray<TSharedPtr<FJsonValue>>& OutScalars,
        TArray<TSharedPtr<FJsonValue>>& OutVectors, TArray<TSharedPtr<FJsonValue>>& OutTextures,
        TArray<TSharedPtr<FJsonValue>>& OutStaticSwitches)
    {
        if (!Material) { return; }
        TArray<FName> Scalars;
        UMaterialEditingLibrary::GetScalarParameterNames(Material, Scalars);
        for (const FName& Name : Scalars)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Name.ToString());
            FSoftObjectPath Source;
            UMaterialEditingLibrary::GetScalarParameterSource(Material, Name, Source);
            if (!Source.IsNull()) { Entry->SetStringField(TEXT("source"), Source.ToString()); }
            OutScalars.Add(MakeShared<FJsonValueObject>(Entry));
        }
        TArray<FName> Vectors;
        UMaterialEditingLibrary::GetVectorParameterNames(Material, Vectors);
        for (const FName& Name : Vectors)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Name.ToString());
            FSoftObjectPath Source;
            UMaterialEditingLibrary::GetVectorParameterSource(Material, Name, Source);
            if (!Source.IsNull()) { Entry->SetStringField(TEXT("source"), Source.ToString()); }
            OutVectors.Add(MakeShared<FJsonValueObject>(Entry));
        }
        TArray<FName> Textures;
        UMaterialEditingLibrary::GetTextureParameterNames(Material, Textures);
        for (const FName& Name : Textures)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Name.ToString());
            FSoftObjectPath Source;
            UMaterialEditingLibrary::GetTextureParameterSource(Material, Name, Source);
            if (!Source.IsNull()) { Entry->SetStringField(TEXT("source"), Source.ToString()); }
            OutTextures.Add(MakeShared<FJsonValueObject>(Entry));
        }
        TArray<FName> StaticSwitches;
        UMaterialEditingLibrary::GetStaticSwitchParameterNames(Material, StaticSwitches);
        for (const FName& Name : StaticSwitches)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Name.ToString());
            FSoftObjectPath Source;
            UMaterialEditingLibrary::GetStaticSwitchParameterSource(Material, Name, Source);
            if (!Source.IsNull()) { Entry->SetStringField(TEXT("source"), Source.ToString()); }
            OutStaticSwitches.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
}

FSproftMaterialInspectCommands::FSproftMaterialInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftMaterialInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("material_inspect"))
    {
        return HandleMaterialInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown material_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftMaterialInspectCommands::HandleMaterialInspect(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material"), MaterialPath)
        && !Params->TryGetStringField(TEXT("material_path"), MaterialPath)
        && !Params->TryGetStringField(TEXT("path"), MaterialPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material' parameter (path to a UMaterial / UMaterialInstance asset)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(MaterialPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Material asset not found at '%s'"), *MaterialPath));
    }

    if (UMaterial* Material = Cast<UMaterial>(Asset))
    {
        return InspectMaterial(Material);
    }
    if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset))
    {
        return InspectInstance(Instance);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Asset at '%s' is %s, not a UMaterial / UMaterialInstance"),
            *MaterialPath, *Asset->GetClass()->GetName()));
}

TSharedPtr<FJsonObject> FSproftMaterialInspectCommands::InspectMaterial(UMaterial* Material)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("kind"), TEXT("material"));
    Out->SetStringField(TEXT("name"), Material->GetName());
    Out->SetStringField(TEXT("path"), Material->GetPathName());
    Out->SetStringField(TEXT("class"), Material->GetClass()->GetName());

    // Headline material properties.
    Out->SetNumberField(TEXT("blend_mode"), static_cast<int32>(Material->GetBlendMode()));
    Out->SetStringField(TEXT("blend_mode_name"), StaticEnum<EBlendMode>() ? StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Material->GetBlendMode())) : TEXT(""));
    Out->SetBoolField(TEXT("is_two_sided"), Material->IsTwoSided());
    Out->SetBoolField(TEXT("is_translucent"), Material->IsTranslucencyWritingVelocity());

    // Expressions.
    TArray<TSharedPtr<FJsonValue>> ExpressionArr;
    int32 ExpressionCount = 0;
    for (UMaterialExpression* Expr : Material->GetExpressions())
    {
        if (!Expr) { continue; }
        ExpressionArr.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Expr)));
        ++ExpressionCount;
    }
    Out->SetNumberField(TEXT("expression_count"), ExpressionCount);
    Out->SetArrayField(TEXT("expressions"), ExpressionArr);

    // Parameters.
    TArray<TSharedPtr<FJsonValue>> Scalars, Vectors, Textures, StaticSwitches;
    AppendParameterNames(Material, Scalars, Vectors, Textures, StaticSwitches);
    TSharedPtr<FJsonObject> Parameters = MakeShared<FJsonObject>();
    Parameters->SetArrayField(TEXT("scalar"), Scalars);
    Parameters->SetArrayField(TEXT("vector"), Vectors);
    Parameters->SetArrayField(TEXT("texture"), Textures);
    Parameters->SetArrayField(TEXT("static_switch"), StaticSwitches);
    Out->SetObjectField(TEXT("parameters"), Parameters);

    // Per-attribute connected output expression.
    TArray<TSharedPtr<FJsonValue>> Attributes;
    for (const FAttributeProbe& Probe : GAttributeProbes)
    {
        TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
        AttrObj->SetStringField(TEXT("name"), Probe.Label);
        UMaterialExpression* InputExpr = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, Probe.Property);
        if (InputExpr)
        {
            AttrObj->SetStringField(TEXT("expression"), InputExpr->GetName());
            AttrObj->SetStringField(TEXT("expression_class"), InputExpr->GetClass()->GetName());
            const FString OutputName = UMaterialEditingLibrary::GetMaterialPropertyInputNodeOutputName(Material, Probe.Property);
            if (!OutputName.IsEmpty())
            {
                AttrObj->SetStringField(TEXT("output_name"), OutputName);
            }
        }
        else
        {
            AttrObj->SetBoolField(TEXT("connected"), false);
        }
        Attributes.Add(MakeShared<FJsonValueObject>(AttrObj));
    }
    Out->SetArrayField(TEXT("attributes"), Attributes);

    // Used textures (handy for verifying TextureSampleParameter wiring).
    TArray<UTexture*> UsedTextures = UMaterialEditingLibrary::GetUsedTextures(Material);
    TArray<TSharedPtr<FJsonValue>> TextureArr;
    for (UTexture* Tex : UsedTextures)
    {
        if (Tex)
        {
            TextureArr.Add(MakeShared<FJsonValueString>(Tex->GetPathName()));
        }
    }
    Out->SetArrayField(TEXT("used_textures"), TextureArr);

    return Out;
}

TSharedPtr<FJsonObject> FSproftMaterialInspectCommands::InspectInstance(UMaterialInstance* Instance)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("kind"), TEXT("material_instance"));
    Out->SetStringField(TEXT("name"), Instance->GetName());
    Out->SetStringField(TEXT("path"), Instance->GetPathName());
    Out->SetStringField(TEXT("class"), Instance->GetClass()->GetName());

    if (Instance->Parent)
    {
        Out->SetStringField(TEXT("parent_material"), Instance->Parent->GetPathName());
        Out->SetStringField(TEXT("parent_class"), Instance->Parent->GetClass()->GetName());
    }

    // Parent's parameters (full set, so the caller can see what the parent
    // exposes that the instance can override).
    TArray<TSharedPtr<FJsonValue>> ParentScalars, ParentVectors, ParentTextures, ParentStaticSwitches;
    AppendParameterNames(Instance, ParentScalars, ParentVectors, ParentTextures, ParentStaticSwitches);
    TSharedPtr<FJsonObject> ParentParameters = MakeShared<FJsonObject>();
    ParentParameters->SetArrayField(TEXT("scalar"), ParentScalars);
    ParentParameters->SetArrayField(TEXT("vector"), ParentVectors);
    ParentParameters->SetArrayField(TEXT("texture"), ParentTextures);
    ParentParameters->SetArrayField(TEXT("static_switch"), ParentStaticSwitches);
    Out->SetObjectField(TEXT("parameters"), ParentParameters);

    // Instance overrides.
    TArray<TSharedPtr<FJsonValue>> ScalarOverrides;
    for (const FScalarParameterValue& Param : Instance->ScalarParameterValues)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
        Entry->SetNumberField(TEXT("value"), Param.ParameterValue);
        ScalarOverrides.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TArray<TSharedPtr<FJsonValue>> VectorOverrides;
    for (const FVectorParameterValue& Param : Instance->VectorParameterValues)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
        TArray<TSharedPtr<FJsonValue>> Color;
        Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.R));
        Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.G));
        Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.B));
        Color.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.A));
        Entry->SetArrayField(TEXT("value"), Color);
        VectorOverrides.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TArray<TSharedPtr<FJsonValue>> TextureOverrides;
    for (const FTextureParameterValue& Param : Instance->TextureParameterValues)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
        Entry->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : FString());
        TextureOverrides.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Overrides = MakeShared<FJsonObject>();
    Overrides->SetArrayField(TEXT("scalar"), ScalarOverrides);
    Overrides->SetArrayField(TEXT("vector"), VectorOverrides);
    Overrides->SetArrayField(TEXT("texture"), TextureOverrides);
    Out->SetObjectField(TEXT("overrides"), Overrides);

    return Out;
}
