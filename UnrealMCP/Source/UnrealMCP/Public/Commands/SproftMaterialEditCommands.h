#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: material_edit (small variant)
 *
 * A trimmed cut of the hosted Flop "material_edit" tool. Three operations:
 *   - "create_material": create a UMaterial asset with a single Constant3Vector
 *      base-colour input wired to the material's BaseColor property.
 *   - "create_material_instance_constant": create a UMaterialInstanceConstant
 *      pointing at a parent UMaterialInterface (UMaterial or another MIC).
 *   - "set_instance_parameter": override scalar / vector / texture parameters
 *      on a UMaterialInstanceConstant. The parameter type is auto-detected
 *      from the supplied value (float / [r,g,b,a] / texture path string).
 *
 * Authoring expression graphs and Material Parameter Collections is left in
 * BACKLOG.md; the verbose UMaterialExpression* surface is a later pass.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UMaterialFactoryNew / UMaterialInstanceConstantFactoryNew
 *   - UMaterialEditingLibrary::ConnectMaterialProperty +
 *     SetMaterialInstance{Scalar,Vector,Texture}ParameterValue
 *   - UMaterialExpressionConstant3Vector for the base-colour driver
 *   - FAssetRegistryModule::AssetCreated + UEditorAssetLibrary save / load
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftMaterialEditCommands
{
public:
    FSproftMaterialEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleMaterialEdit(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> CreateMaterialInstanceConstant(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetInstanceParameter(const TSharedPtr<FJsonObject>& Params);
};
