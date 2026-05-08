#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: material_inspect
 *
 * Read-only counterpart to `material_edit`. Useful for diagnosing a
 * material before editing it. Resolves the asset path against
 * UEditorAssetLibrary, accepts UMaterial / UMaterialInstanceConstant /
 * UMaterialInstance / UMaterialFunction in the inspect path.
 *
 * Operations (keyed by `op` plus a required `material` path):
 *   - "inspect" / "read" (default): one combined dump.
 *      For UMaterial:
 *        - asset path, name, asset class
 *        - shading model / blend mode / domain
 *        - parameter list across scalar / vector / texture / static_switch
 *          (name, default value, owning expression class)
 *        - expression list: each expression's class, FName, position, and
 *          parameter name when relevant
 *        - per-attribute connected output expression for the standard
 *          GBuffer attributes (BaseColor, Metallic, Specular, Roughness,
 *          Anisotropy, Normal, Tangent, EmissiveColor, Opacity,
 *          OpacityMask, WorldPositionOffset, AmbientOcclusion,
 *          Refraction, Displacement). Reports "(unset)" when no
 *          expression is wired.
 *      For UMaterialInstanceConstant:
 *        - parent material path and parent material's parameters
 *        - overridden parameter values on the instance
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UMaterial::GetExpressions, UMaterialEditorOnlyData::BaseColor / etc.
 *   - UMaterialEditingLibrary::Get*ParameterNames for the parameter list.
 *   - UMaterialEditingLibrary::GetMaterialPropertyInputNode +
 *     GetMaterialExpressionNodePosition.
 *   - UMaterialInstance::ScalarParameterValues / VectorParameterValues /
 *     TextureParameterValues for instance overrides.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftMaterialInspectCommands
{
public:
    FSproftMaterialInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleMaterialInspect(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> InspectMaterial(class UMaterial* Material);
    TSharedPtr<FJsonObject> InspectInstance(class UMaterialInstance* Instance);
};
