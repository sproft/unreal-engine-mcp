#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: material_edit
 *
 * A growing cut of the hosted Flop "material_edit" tool. Operations:
 *   - "create_material": create a UMaterial asset with a single Constant3Vector
 *      base-colour input wired to the material's BaseColor property.
 *   - "create_material_instance_constant": create a UMaterialInstanceConstant
 *      pointing at a parent UMaterialInterface (UMaterial or another MIC).
 *   - "set_instance_parameter": override scalar / vector / texture parameters
 *      on a UMaterialInstanceConstant. The parameter type is auto-detected
 *      from the supplied value (float / [r,g,b,a] / texture path string).
 *   - "add_expression": append a UMaterialExpression* to a UMaterial.
 *      Resolves the expression class from a short name (constant,
 *      constant3vector, scalar_parameter, vector_parameter,
 *      texture_sample_parameter_2d, multiply, add, subtract, divide,
 *      lerp / linear_interpolate, time, panner, texture_coordinate,
 *      one_minus, saturate, clamp, fresnel, power, sine, cosine,
 *      component_mask, if, make_material_attributes), a full
 *      `/Script/Engine.UMaterialExpressionFoo` path, or a bare
 *      `MaterialExpressionFoo` class name. Optional position; the
 *      default position cascades down so callers do not have to lay
 *      out the graph manually. Optionally connects the new expression
 *      to a material attribute (`property` field, e.g. "base_color")
 *      or to another expression's named input (`connect_to` +
 *      `connect_input` fields). Returns the new expression's FName,
 *      class, position, and pin list so a follow-up call can address
 *      it.
 *   - "connect_expressions": connect a source expression's output pin
 *      to a destination expression's named input pin through
 *      `UMaterialEditingLibrary::ConnectMaterialExpressions`. Source
 *      output defaults to "" (the primary output).
 *   - "set_expression_property": apply a flat property dict to a
 *      named expression through `FProperty::ImportText`. Used to set
 *      ConstA / ConstB on a Multiply, R on a Constant, parameter
 *      name + default on a Scalar / Vector / Texture parameter, etc.
 *
 * Material Functions and Material Parameter Collections remain on the
 * backlog; the per-expression authoring above closes the largest gap.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UMaterialFactoryNew / UMaterialInstanceConstantFactoryNew
 *   - UMaterialEditingLibrary::CreateMaterialExpression +
 *     ConnectMaterialExpressions + ConnectMaterialProperty +
 *     RecompileMaterial + UpdateMaterialInstance
 *   - UMaterialExpressionConstant / Constant3Vector / Multiply /
 *     LinearInterpolate / TextureSampleParameter2D / ScalarParameter /
 *     VectorParameter / Time / Panner / TextureCoordinate / OneMinus /
 *     Saturate / Clamp / Fresnel / Power / Sine / Cosine /
 *     ComponentMask / If / MakeMaterialAttributes
 *   - FProperty::ImportText for the property writes
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

    TSharedPtr<FJsonObject> AddExpression(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ConnectExpressions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetExpressionProperty(const TSharedPtr<FJsonObject>& Params);
};
