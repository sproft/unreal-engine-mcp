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
 *   - "add_texture_sample_cube": cube-texture sibling of
 *      `add_texture_sample`. Spawns a
 *      `UMaterialExpressionTextureSampleParameterCube` and binds its
 *      `Texture` property to a chosen `UTextureCube` asset. The
 *      cube-sample expression carries a `ParameterName` slot the
 *      caller can populate through the same `name` / `properties`
 *      knobs `add_expression` exposes. If the resolved asset is a
 *      UTexture2D instead the op falls back to the 2D variant
 *      (`UMaterialExpressionTextureSample`) so callers can pass a
 *      generic "texture" path without first probing the asset class.
 *      All the same downstream knobs apply: optional `coordinates`
 *      wires the UV / vector input from a named expression;
 *      optional `connect_to` / `connect_input` / `property` drops
 *      the RGB output into another expression or a material
 *      attribute; optional `name` renames the spawned expression.
 *      Recompiles + saves on success unless `recompile=false` /
 *      `save=false` is passed.
 *   - "add_2d_array_sample": Texture2DArray sibling of
 *      `add_texture_sample` / `add_texture_sample_cube`. Takes a
 *      material + a UTexture2DArray asset path. With no
 *      `parameter_name` set the op spawns a plain
 *      `UMaterialExpressionTextureSample` and binds the
 *      Texture2DArray asset directly on the new node so the array
 *      lands without first wiring a parameter. With a
 *      `parameter_name` set the op spawns
 *      `UMaterialExpressionTextureSampleParameter2DArray` (or
 *      falls back to `UMaterialExpressionTextureSampleParameter2D`
 *      if the resolved asset turns out to be a UTexture2D) and
 *      lands the FName on the parent
 *      `UMaterialExpressionTextureSampleParameter::ParameterName`
 *      slot so the resulting material exposes a named array slot
 *      that calling Material Instances can swap. Auto-detects the
 *      spawn class from the resolved asset's IsA<UTexture2DArray>
 *      so callers can stay flat. Same downstream knobs as
 *      `add_texture_sample`: optional `coordinates` wires the UV
 *      input; optional `connect_to` / `connect_input` / `property`
 *      drops the RGB output downstream; optional `name` renames
 *      the spawned expression; optional `properties` writes
 *      additional UPROPERTY values through `ImportText_InContainer`.
 *      Recompiles + saves on success unless `recompile=false` /
 *      `save=false` is passed.
 *   - "add_constant": single-call wrapper for the common
 *      literal-constant case. Pass `material` and `value` (a JSON
 *      number for a 1-channel `UMaterialExpressionConstant`, a
 *      2-element array for `UMaterialExpressionConstant2Vector`, a
 *      3-element array for `UMaterialExpressionConstant3Vector`,
 *      or a 4-element array for `UMaterialExpressionConstant4Vector`);
 *      the op auto-picks the matching expression class and lands
 *      the literal on the matching node fields (`R` for the 1- /
 *      2-channel variants, `Constant` (FLinearColor) for the 3- /
 *      4-channel variants). Same downstream knobs as
 *      `add_expression` (`position`, `name`, `properties`,
 *      `property` / `connect_to` / `connect_input` for one-shot
 *      downstream wiring, `recompile`, `save`).
 *   - "add_math": single-call wrapper that spawns the common math
 *      expression nodes by short token. `op` accepts one of `Add` /
 *      `Subtract` / `Multiply` / `Divide` / `Min` / `Max` / `Lerp`
 *      / `Power` / `Sin` / `Cos` / `Abs` / `Saturate` / `OneMinus`
 *      / `Normalize` / `DotProduct` / `CrossProduct` (case-
 *      insensitive). Optional `A` / `B` / `T` (alpha for Lerp) /
 *      `input` / `base` / `exponent` are names of existing
 *      expressions on the same material whose first output (or the
 *      pin named by `<slot>_output`) wires into the matching input.
 *      Two-input math nodes (Add / Subtract / Multiply / Divide /
 *      Min / Max) accept a literal `constant` / `constant_a` /
 *      `constant_b` for the `ConstA` / `ConstB` slots; `Power`
 *      accepts `constant_exponent`; `Lerp` accepts `constant_a` /
 *      `constant_b` / `constant_alpha` (alias `t`). Same downstream
 *      knobs as `add_expression` and `add_constant` (`position`,
 *      `name`, `properties`, `property` / `connect_to` /
 *      `connect_input` for one-shot downstream wiring, `recompile`,
 *      `save`).
 *   - "add_dynamic_parameter": spawns a
 *      `UMaterialExpressionDynamicParameter` on a target material's
 *      graph and lands a per-channel name list onto the expression's
 *      `ParamNames` array. Dynamic parameter nodes give Niagara
 *      renderers (and other runtime systems) four extra material
 *      inputs they can drive per particle / per instance without
 *      shipping a Material Instance for every variation. The
 *      `parameter_index` (0..3) picks the slot since each material
 *      can host up to four dynamic parameter nodes; `param_names`
 *      accepts a 4-entry list of FName strings (mapped to the R / G
 *      / B / A channels in order) or an object form
 *      `{r, g, b, a}` so callers can patch a subset without padding
 *      with empty entries. Position cascades; the new node accepts
 *      `name` to rename for follow-up wiring plus the same downstream
 *      knobs the other `add_*` ops expose (`property` /
 *      `connect_to` / `connect_input` for one-shot wiring,
 *      `recompile`, `save`). The optional `default_values` array
 *      (length 4) lands on the expression's `DefaultValue`
 *      FLinearColor slot for the editor-side preview value the
 *      compiler falls back to when no Niagara driver is bound.
 *   - "add_uv_node": single-call wrapper that spawns one of the
 *      common UV-flow expression nodes by short `op` token.
 *      Accepts `TextureCoordinate` / `Panner` / `Rotator` /
 *      `WorldPosition` / `ObjectPosition` / `CameraPosition` /
 *      `ScreenPosition` (case-insensitive plus aliases `TexCoord`
 *      / `UV` / `ObjectPositionWS` / `CameraPositionWS`). The op
 *      auto-picks the matching expression subclass and lays the
 *      new node onto the target material. Optional flat
 *      `properties` dict applies through `ImportText_InContainer`
 *      so callers can land `CoordinateIndex` / `UTiling` /
 *      `VTiling` on TextureCoordinate, `SpeedX` / `SpeedY` /
 *      `ConstCoordinate` on Panner, `CenterX` / `CenterY` /
 *      `Speed` on Rotator, the `WorldPositionShaderOffset` enum
 *      on WorldPosition, the `OriginType` enum on ObjectPosition,
 *      etc., in the same call. Same downstream knobs as
 *      `add_expression` / `add_math` (`position`, `name`,
 *      `property` / `connect_to` / `connect_input` for one-shot
 *      downstream wiring, `recompile`, `save`).
 *   - "add_fresnel": spawns a `UMaterialExpressionFresnel` on a target
 *      material's graph. The Fresnel expression generates the
 *      view-angle falloff most commonly wired into a material's
 *      EmissiveColor (rim light) or Opacity (edge fade) input. The
 *      engine surfaces three editor-side knobs on the expression:
 *      `Exponent` (float; default 5.0; the falloff sharpness) and
 *      `BaseReflectFraction` (float; default 0.04; the floor value
 *      at view angle 0, the Schlick `F0` term) plus the `Normal`
 *      and `CameraVector` input pins (both default to the engine's
 *      pixel-shader-side world-space inputs when left empty).
 *      Optional `normal` / `camera_vector` inputs wire a named
 *      sibling expression's first output into the matching pin
 *      through `ConnectMaterialExpressions`. Same downstream knobs
 *      as `add_constant` / `add_math` / `add_uv_node` (`position`,
 *      `name`, `properties`, `property` / `connect_to` /
 *      `connect_input` for one-shot downstream wiring, `recompile`,
 *      `save`). Common enough that calling `add_expression` then
 *      setting two properties via `set_expression_property` (one
 *      for `Exponent`, one for `BaseReflectFraction`) round-trips
 *      when one call should suffice.
 *   - "set_blend_mode": rebinds the master UMaterial's `BlendMode`
 *      UPROPERTY plus the paired `OpacityMaskClipValue` knob. The
 *      `blend_mode` token (alias `mode`) accepts `Opaque` / `Masked` /
 *      `Translucent` / `Additive` / `Modulate` / `AlphaComposite` /
 *      `AlphaHoldout`, case-insensitive, with or without the `BLEND_`
 *      prefix. Optional `opacity_mask_clip_value` (alias
 *      `opacity_clip` / `clip`) writes the matching float (the engine
 *      only consults this on Masked). Routes through PreEditChange
 *      / PostEditChangeProperty against the BlendMode UPROPERTY so
 *      the static permutation recompiles for the new translucency
 *      pass. The op surfaces the previous + new BlendMode tokens and
 *      the previous + new opacity-mask clip values so the caller gets
 *      a before / after pair on a single round trip. Refuses
 *      non-UMaterial assets (Material Instances flow through
 *      `set_attribute_blendable` instead since the override surface is
 *      on the FMaterialInstanceBasePropertyOverrides struct, not on
 *      the MIC's UPROPERTY directly).
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
    TSharedPtr<FJsonObject> AddTextureSampleCube(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddTexture2DArraySample(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddConstant(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddMath(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddUVNode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddDynamicParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddFresnel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetBlendMode(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetMaterialFlags(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetShadingModel(const TSharedPtr<FJsonObject>& Params);
};
