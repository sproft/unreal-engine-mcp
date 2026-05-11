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
 *   - "add_expressions": bulk variant of `add_expression`. Takes a list
 *      of expression specs (each with class + optional name + position
 *      + property dict) plus an optional list of edge specs (each
 *      `{source, source_output?, dest, dest_input?}` between expressions
 *      or `{source, property}` to a material attribute). Cuts the
 *      round-trip count for typical "build me a panner-driven UV
 *      chain" asks. Each expression's resolved FName comes back in the
 *      response so a follow-up call can address it. Recompiles + saves
 *      once after the whole batch unless `recompile=false` /
 *      `save=false` is passed.
 *   - "create_parameter_collection": create a UMaterialParameterCollection
 *      asset at a `/Game/...` path through
 *      `UMaterialParameterCollectionFactoryNew`. Empty by default; a
 *      follow-up `add_collection_parameter` populates the scalar / vector
 *      arrays.
 *   - "add_collection_parameter": append a typed entry to a
 *      UMaterialParameterCollection's `ScalarParameters` / `VectorParameters`
 *      array. The type token is `scalar` / `float` for scalar, `vector` /
 *      `color` for vector. Default values come in as a JSON number
 *      (scalar) or `[r, g, b, a]` array / `{r,g,b,a}` object (vector).
 *      The op runs PreEditChange + PostEditChangeProperty under a
 *      synthesized FPropertyChangedEvent so the asset's StateId
 *      regenerates and any UMaterial referencing the collection picks
 *      the new entry up on the next compile.
 *   - "create_material_function": create a UMaterialFunction asset at a
 *      `/Game/...` path through `UMaterialFunctionFactoryNew`. Optional
 *      `expressions` list mirrors the `add_expressions` shape (each
 *      spec carries `class` plus optional `name` alias / `position` /
 *      `properties` dict) and routes through
 *      `UMaterialEditingLibrary::CreateMaterialExpressionInFunction`,
 *      then `UpdateMaterialFunction` recompiles any referencing
 *      materials in one pass. The expected workflow is to seed the
 *      function with a `function_input` + `function_output` pair and
 *      a small expression chain in one call.
 *   - "add_function_call": adds a UMaterialExpressionMaterialFunctionCall
 *      to a target UMaterial's graph and binds it to a chosen
 *      UMaterialFunctionInterface. Resolves the material function from
 *      a `/Game/...` path (the canonical MF location). After the spawn
 *      the op calls `SetMaterialFunction(NewFunction)` so the call
 *      expression's `FunctionInputs` / `FunctionOutputs` arrays
 *      regenerate from the bound function's declared input / output
 *      pins; without that step the call node renders without pins.
 *      Optional `position` cascades the same way as `add_expression`;
 *      optional `name` aliases the spawned expression so a follow-up
 *      `connect_expressions` call can address the node by FName.
 *      Recompiles + saves on success unless `recompile=false` /
 *      `save=false` is passed.
 *   - "add_texture_sample": adds a `UMaterialExpressionTextureSample`
 *      to a target UMaterial's graph and binds the new node's
 *      `Texture` property to a chosen UTexture asset in one call.
 *      Common enough that going through the generic `add_expression`
 *      + manual `Texture` property set is awkward. The texture
 *      resolves from a `/Game/...` path or a unique short name
 *      probed against the asset registry's UTexture index. After
 *      the spawn the op writes `TextureSample->Texture` directly
 *      and lets the engine derive the sampler type from the texture
 *      through `AutoSetSampleType()` (the editor's right-click
 *      "Refresh Sampler Type" path). An optional `coordinates`
 *      named expression on the same material wires its first output
 *      pin into the texture sample's `Coordinates` input through
 *      `UMaterialEditingLibrary::ConnectMaterialExpressions`.
 *      Optional `connect_to` / `connect_input` and `property`
 *      knobs mirror `add_expression` so the call can both create
 *      the sample and drop its `RGB` output into `BaseColor` (or
 *      another material attribute) in one step. Optional `name`
 *      renames the new expression so a follow-up
 *      `connect_expressions` call can address it by FName.
 *      Position cascades the same way as `add_expression`.
 *      Recompiles + saves on success unless `recompile=false` /
 *      `save=false` is passed.
 *   - "set_attribute_blendable": flips a per-attribute override toggle
 *      on a UMaterialInstanceConstant's
 *      `FMaterialInstanceBasePropertyOverrides` struct. The
 *      `attribute` token resolves to a matching `bOverride_X` slot
 *      (blend_mode / shading_model / opacity_mask_clip_value /
 *      dithered_lod_transition / cast_dynamic_shadow_as_masked /
 *      two_sided / is_thin_surface / output_translucent_velocity /
 *      has_pixel_animation / enable_tessellation /
 *      displacement_scaling / enable_displacement_fade /
 *      displacement_fade_range /
 *      max_world_position_offset_displacement /
 *      compatible_with_lumen_card_sharing). The `enabled` boolean
 *      lands on the `bOverride_X` flag and the optional `value`
 *      lands on the matching payload field through
 *      `FProperty::ImportText_Direct`. PostEditChangeProperty fires
 *      on the MIC so `UpdateStaticPermutation` rebuilds the static
 *      permutation shaders. Saves on success unless `save=false`.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UMaterialFactoryNew / UMaterialInstanceConstantFactoryNew /
 *     UMaterialParameterCollectionFactoryNew / UMaterialFunctionFactoryNew
 *   - UMaterialEditingLibrary::CreateMaterialExpression +
 *     CreateMaterialExpressionInFunction +
 *     ConnectMaterialExpressions + ConnectMaterialProperty +
 *     RecompileMaterial + UpdateMaterialInstance + UpdateMaterialFunction
 *   - UMaterialExpressionConstant / Constant3Vector / Multiply /
 *     LinearInterpolate / TextureSampleParameter2D / ScalarParameter /
 *     VectorParameter / Time / Panner / TextureCoordinate / OneMinus /
 *     Saturate / Clamp / Fresnel / Power / Sine / Cosine /
 *     ComponentMask / If / MakeMaterialAttributes /
 *     FunctionInput / FunctionOutput
 *   - UMaterialParameterCollection::ScalarParameters /
 *     VectorParameters arrays + PostEditChangeProperty broadcast
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
    TSharedPtr<FJsonObject> AddExpressionsBulk(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateParameterCollection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddCollectionParameter(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateMaterialFunction(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddFunctionCall(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> SetAttributeBlendable(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> AddTextureSample(const TSharedPtr<FJsonObject>& Params);
};
