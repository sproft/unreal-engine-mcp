# Sproft fork: hosted Flop tool backlog

Tracks the gap between the hosted Flop MCP tool surface (~58 tools, 50+ doc'd
in `README.md`) and what exists in the local Python server. The shipped fork
additions live next to this file; what remains is listed below with a one-line
spec lifted from the README and a difficulty estimate (small / medium / large).

All future work in this list must remain clean-room: derived from the public
UE5 API and the documented behaviour, never from the proprietary FlopAI plugin.

The most recent pass shipped four deepening additions across
existing tools: `behavior_tree` gains `set_root_decorator`
(appends a UBTDecorator to `UBehaviorTree::RootDecorators`, the
tree-level chain the BT editor exposes under "Add Decorator" on
the root composite; NewObject's the decorator outered to the
tree, runs `InitializeFromAsset`, and lands an optional flat
property dict through `FProperty::ImportText_InContainer`),
`sequencer_edit` gains `add_visibility_track` (declarative
one-call wrapper for the show / hide pattern; resolves a
binding through `binding` GUID or `actor` / `possessable` name,
picks `UMovieSceneVisibilityTrack` for possessables and
`UMovieSceneSpawnTrack` for spawnables, finds-or-adds the track,
spawns a section through `UMovieSceneTrack::CreateNewSection` +
`AddSection`, wraps `[start_frame, end_frame)` into the
section's range, and writes the bool channel default through
the shared `UMovieSceneBoolSection` base so the same writer
covers both subclasses), `niagara_edit` gains
`set_system_warmup` (writes the system-level warmup trio
(`WarmupTime` / `WarmupTickCount` / `WarmupTickDelta`) on a
`UNiagaraSystem`; routes through the public NIAGARA_API
`SetWarmupTime` / `SetWarmupTickDelta` mutators so the engine's
`ResolveWarmupTickCount` re-derives the count from the time and
delta when those are supplied, and reflection-writes
`WarmupTickCount` when the caller pins the count directly
(harmonising the time field so the editor's `WarmupTime > 0`
EditCondition holds)), and `behavior_tree` gains
`rename_blackboard_key` (renames an own key on a target
UBlackboardData and propagates the new name across every
FBlackboardKeySelector on every Hard-referencer asset that
implements IBlackboardAssetProvider; refuses duplicate names
against own + parent keys, fires the documented
PreEditChange + PostEditChangeChainProperty pair against the
`Keys` array's `EntryName` subfield so open BB pickers refresh,
walks each Hard-referencer package through
`IAssetRegistry::GetReferencers`, loads each candidate asset,
iterates every subobject under the package, rewrites
`FStructProperty` of FBlackboardKeySelector whose
`SelectedKeyName` matches the old name, and saves the BB plus
every dirty referencer package). The four deepenings together
close the tree-level RootDecorator authoring follow-on, the
"show / hide an actor without leaving the MCP layer" gap on
the Sequencer side, the system-level warmup writer gap on the
Niagara side, and the Blackboard key-rename half of the
"Blackboard rename / type-change ops" follow-on.

The pass before that shipped two maintenance fixes that close
the UE 5.7 build break (the `ENiagaraInterpolatedSpawnMode` enum
tail dropped in this engine snapshot so the `bInterpolatedSpawning`
branch on `niagara_edit` no longer compiled; the `FLinearColor`
USTRUCT entry vanished in the same pass so the `FSlateBrush`
tint reader on `widget_edit` no longer compiled) plus two
deepening additions across existing tools: `material_edit`
gains `add_2d_array_sample` (Texture2DArray sibling of
`add_texture_sample` / `add_texture_sample_cube`; spawns a
plain `UMaterialExpressionTextureSample` and binds a chosen
`UTexture2DArray` directly when no `parameter_name` is supplied,
or a `UMaterialExpressionTextureSampleParameter2DArray` when one
is set, and lands the FName on the parent
`UMaterialExpressionTextureSampleParameter::ParameterName` slot
so the resulting material exposes a named array slot calling
Material Instances can swap; auto-detects the spawn class from
`Texture->IsA<UTexture2DArray>()` with a fallback to the 2D
parameter variant when the resolved asset turns out to be a
UTexture2D; same `coordinates` / `connect_to` / `connect_input`
/ `property` / `name` / `properties` downstream knobs as the
other two texture-sample ops), and `behavior_tree` gains
`add_blackboard_decorator` (declarative one-call shortcut for
the Blackboard decorator the editor's add-decorator picker
spawns most often; spawns a `UBTDecorator_Blackboard` under a
target child slot, wires the `FBlackboardKeySelector` against a
named Blackboard key, picks the right operation family
(`Basic` / `Arithmetic` / `Text`) from the resolved key's class
(Int / Float / Bool / Enum -> Arithmetic, Name / String -> Text,
the rest stay on Basic), and reflection-writes the matching
`EBasicKeyOperation` / `EArithmeticKeyOperation` /
`ETextKeyOperation` row plus the comparison payload field
(`IntValue` / `FloatValue` / `StringValue`) through the property
database so the protected UPROPERTY surface lands without us
touching engine private headers; conditions cover `IsSet` /
`IsNotSet` (Basic family) plus `IsEqualTo` / `IsNotEqualTo`
(Arithmetic family for numeric / bool / enum keys, Text family
for FName / FString keys); `NotifyObserver` defaults to
`ResultChange` with an optional `notify_observer` override).
The Niagara fix remaps both the `ResolveInterpolatedSpawnMode`
canonical-token table and the `bInterpolatedSpawning` write path
onto `ENiagaraInterpolatedSpawnMode::Interpolation` so the
"turn interpolated spawning on" intent still resolves on a
5.7 source build; legacy caller tokens
(`run_update_script_with_interpolation` / `interpolated`) still
point at the same value so existing automation keeps working.
The `FLinearColor` fix routes the `FSlateBrush` tint reader's
ImportText through `TBaseStructure<FLinearColor>::Get()` which
returns the canonical UScriptStruct registered for the built-in
POD types at boot; FSlateBrush's own `::StaticStruct()` calls
(real USTRUCT) keep working as before.

The pass before that shipped four deepening additions across
existing tools: `animation_edit` gains `replace_blendspace_sample`
plus `delete_blendspace_sample` (per-sample-slot edits paired off
the existing `add_blendspace_sample` op; `replace` routes through
`UBlendSpace::ReplaceSampleAnimation(SampleIndex, AnimSequence)`
with skeleton + additive-type checks against the new sequence and
an optional `clear=true` path that unbinds the sample; `delete`
routes through `UBlendSpace::DeleteSample(SampleIndex)` with a
bounds-checked `IsValidBlendSampleIndex` probe), `sequencer_edit`
gains `set_transform_channel_mask` (declarative one-call writer
that lands an `FMovieSceneTransformMask` on a binding's
UMovieScene3DTransformSection through the documented
`UMovieScene3DTransformSection::SetMask`; resolves the same
`binding` GUID / `actor` / `possessable` triple
`add_transform_section_keys` uses, indexes into the track's
`GetAllSections()` at an optional `section_index` (default 0),
accepts either a raw integer `mask` or a flat `channels` list of
channel tokens (`translation_x` / `rotation` / `scale_z` /
`all_transform` / `weight` / `all` / `none`, case-insensitive)
and surfaces the previous + new mask both as a raw bitfield and
as a tokenised channel list in the response), `material_edit`
gains `add_texture_sample_cube` (cube-texture sibling of
`add_texture_sample`; spawns a
`UMaterialExpressionTextureSampleParameterCube` and binds the
new node's `Texture` property to a chosen UTextureCube asset
in one call, with an auto-fallback to
`UMaterialExpressionTextureSample` when the resolved asset is a
UTexture2D so a generic "wire this texture up" call still
lands; supports the same `coordinates` / `connect_to` /
`connect_input` / `property` / `name` / `properties`
downstream knobs), and `pie_test_scene` gains three editor-world
assertion kinds (`actor_has_class`: target actor name + expected
class path; pass when the actor's class matches the expected
class or a subclass; `actor_tag_count`: target actor name +
expected integer; pass when `Tags.Num()` equals the expected
count; `level_actor_count`: target class path + expected
integer; pass when `TActorIterator<AActor>(World, TargetClass)`
counts exactly the expected number, mirroring
`UGameplayStatics::GetAllActorsOfClass`'s subclass-inclusive
walk). The class-token resolver shared by `actor_has_class` and
`level_actor_count` accepts a full `/Script/Module.ClassName`
path, a `/Game/...` Blueprint class path auto-suffixed with
`_C`, or a bare short class name probed with A / U prefix
variants plus a `/Script/Engine.*` fallback. All four
deepenings keep the rest of each tool's surface intact; the
pair on `animation_edit` closes the per-sample-slot edit row
that the previous add-only cut left on the BACKLOG.

The pass before that shipped four deepening additions across
existing tools: `animation_edit` gains `add_blendspace_sample`
(UBlendSpace::AddSample wrapper that handles the skeleton +
additive-type + range-validity guards up front), `sequencer_edit`
gains `add_transform_section_keys` (declarative one-call writer
for per-channel transform keys on a binding's
UMovieScene3DTransformTrack; finds or creates the track + section,
writes location.x / .y / .z / rotation.roll / .pitch / .yaw /
scale.x / .y / .z through the section's channel proxy, expands
the section's range to cover every key time), `material_edit`
gains `add_texture_sample` (spawns a UMaterialExpressionTextureSample
through UMaterialEditingLibrary::CreateMaterialExpression, writes
TextureSample->Texture directly, runs AutoSetSampleType so the
sampler type derives from the texture, and optionally wires
Coordinates from a named expression plus drops the RGB output
into a material attribute), and `bp_input` gains `add_action_chord`
(declarative one-call wrapper that lays a UInputTriggerChordAction
on a mapping row and binds its `ChordAction` slot to a sibling
UInputAction asset path in one step; refuses self-chord since the
runtime never resolves it). Each tool keeps the rest of its
surface intact; the four deepenings together cover the most
common "I need to author X by hand and the small variant did not
have a one-call shortcut" gaps designers hit in the three months
since the small-variant cuts shipped.

The pass before that shipped three deepening additions across
existing tools: `widget_edit` gains `set_widget_brush` (FSlateBrush
reflective writer with designer-sugar shorthands), `chaos_edit`
gains `set_damage_threshold` (per-fracture-level + uniform writer
over the asset's `DamageThreshold` array), and `sequencer_edit`
gains `add_audio_track` (declarative one-call wrapper that lays a
UMovieSceneAudioTrack + UMovieSceneAudioSection for a chosen
USoundBase in one pass). `widget_edit set_widget_brush` resolves
a target child widget on a UWidgetBlueprint, picks the FSlateBrush
UPROPERTY through an explicit `brush_field` or a short-name probe
that defaults to `Brush` for UImage and `Background` for UBorder,
then applies a dict of writes covering designer-sugar keys
(`texture` / `material` / `resource_object` lands an asset on
`FSlateBrush::SetResourceObject`; `tint` accepts `[r,g,b,a]` /
`{R,G,B,A}` / `(R=,G=,B=,A=)` ImportText; `size` accepts `[x,y]` /
`{X,Y}`; `margin` accepts `[L,T,R,B]` / `[H,V]` / uniform float /
`{Left,Top,Right,Bottom}`; `tiling` / `draw_as` / `mirroring`
route through TEnumAsByte ImportText) plus a raw `brush` dict that
lands any other FSlateBrush UPROPERTY through
`FProperty::ImportText_Direct`. `chaos_edit set_damage_threshold`
accepts either a per-level float array (`thresholds`) or a single
uniform `threshold` / `value`. Optional `set_damage_model=true`
flips `DamageModel` over to UserDefined so the per-level threshold
actually drives the runtime strain; optional
`clear_size_specific=true` clears the size-specific override so
the per-level table applies. The Dataflow-driver path stays on the
BACKLOG (the inner FDataflowNode is not a UPROPERTY on the
UDataflowEdNode wrapper, so the reflective write needs the
editor-only DATAFLOWENGINE dep this module stays clear of).
`sequencer_edit add_audio_track` resolves a USoundBase by path /
short name, finds or creates a UMovieSceneAudioTrack scoped to a
binding or master, routes through
`UMovieSceneAudioTrack::AddNewSound` to land a
UMovieSceneAudioSection, and sets the section range from
`start_frame` + `duration_frames` or the sound's intrinsic length
through `USoundBase::GetDuration` converted via the MovieScene's
tick resolution. An `force_new_track=true` knob bypasses the
reuse-existing path so each call spawns a fresh track. Each tool
keeps the rest of its surface intact; per-tool BACKLOG follow-on
rows shrink to the dataflow side on chaos and per-section
audio-channel writes on sequencer.

The pass before that shipped four additions that close the
UMG event-binding gap on `widget_edit`, the UMG style-struct gap
on `widget_edit`, the long-standing PlanarCut gap on `chaos_edit`,
and the mystery Blueprint authoring trio (`bp_author` /
`bp_dry_run` / `bp_skills`) that the hosted README names but does
not document beyond a one-line phrase. `widget_edit` gains
`add_event_binding`: spawns (or focuses) a
`UK2Node_ComponentBoundEvent` in the WBP's event graph for a named
child widget's multicast delegate (`OnClicked` / `OnHovered` /
`OnTextCommitted` / `OnValueChanged`, etc.). Resolves the
FObjectProperty for the child widget on the WBP's
`SkeletonGeneratedClass` and the FMulticastDelegateProperty on
that widget's UClass, then routes through
`FKismetEditorUtilities::CreateNewBoundEventForClass` (UNREALED_API)
so the runtime wiring matches the UMG editor's "+ event" picker.
Auto-flips `bIsVariable` + recompiles when the child widget is not
yet exposed as a BP variable, since CreateNewBoundEventForClass
needs the FObjectProperty on the generated class.
`FindBoundEventForComponent` runs first so a second call returns
the existing node rather than doubling up. Optional
`handler_function` lands on the spawned K2Node's
`CustomFunctionName` so the resulting BP entry point picks up the
caller's chosen label; the rename skips when the name collides
with another event node. `widget_edit` gains `set_widget_style`:
writes a flat property dict against a child widget's style struct
field through reflection. Defaults to the `WidgetStyle` UPROPERTY
(covers UButton / UProgressBar / UScrollBar / UScrollBox / USlider
/ UCheckBox / UEditableText / UEditableTextBox / UComboBox etc.);
an optional `style_field` knob targets secondary slots
(`WidgetBarStyle` on a UScrollBox, etc.). Each entry in `style`
writes through `FProperty::ImportText_Direct` against the field on
the resolved FXyzStyle struct. Failed entries surface under the
response's `skipped` array with reason + attempted ImportText
input. After the writes `PostEditChangeProperty` fires on the
widget so the UMG editor's preview refreshes and the variable's
compiled default propagates. `chaos_edit` gains `fracture_box`:
runs an axis-aligned box fracture against a UGeometryCollection
through the `PlanarCut` plugin's `CutWithPlanarCells` entry point
(PLANARCUT_API on `PlanarCut.h` line 210). Required `min` / `max`
3-element arrays describe the cutting region in the collection's
local space; optional `divisions` (`[X, Y, Z]` int triple, default
`[2, 2, 2]`) drives the cell grid through the documented
`FPlanarCells(FBox, FIntVector)` constructor; optional
`transform_index` picks the target transform (defaults to the
collection's `root_index`, the unfractured root for a fresh
asset). After the cut runs `InitializeMaterials` +
`UpdateGeometryDependentProperties` + `InvalidateCollection` so
the cached simulation data and renderer-side fields rebuild on the
next access. Enables the `PlanarCut` plugin in
`UnrealMCP.uplugin` (`EnabledByDefault: false` upstream) and adds
`PlanarCut` + `GeometryCore` + `DynamicMesh` + `GeometryAlgorithms`
to the editor-only `PrivateDependencyModuleNames` (the 4-dep
explicit list stays under the 8-dep ceiling; the transitive deps
off PlanarCut's `PublicDependencyModuleNames` cover the rest).
The mystery trio ships as Python-side glue over the existing
Blueprint authoring tools; no new C++ surface. `bp_author` walks a
declarative JSON spec (`{name, parent_class, variables, components,
graphs: {events, functions}}`) and runs `bp_create` →
`bp_variable add` → `bp_component` → `bp_function_create` →
`bp_nodes` → `bp_wire` → `bp_commit` in order. Each step's
response surfaces on the ordered `steps` array. `stop_on_error`
(default true) returns at the first failing step; pass false to
continue past failures and collect every per-step result.
`bp_dry_run` accepts the same spec and runs the parent-class /
variable-type / class-token shape checkers against the live
`unreal_api` describe path without calling any write op. Returns
a `would_do` step plan plus `unresolved` rows for unknown tokens
and `warnings` for soft red flags (e.g. node class tokens outside
the bp_nodes short-name table). `bp_skills` is the `skills` tool
scoped to topic IDs that start with `blueprint-`. Seed entries:
`blueprint-events` and `blueprint-variables`. Adding a new entry
is a file-add under `Python/skills/`, not a code change. With
this pass landing the `widget_edit` UMG row narrows to advanced
styles (USlateBrush asset-driven), the `chaos_edit` BACKLOG row
loses the `fracture_box` retry, and the Blueprint authoring rows
that called out `bp_author` / `bp_dry_run` / `bp_skills` as
unshipped close.

The pass before that shipped three deepening edit slices that
close the per-direction MVVM conversion gap on `widget_edit`, the
per-execution scoped-modifier gap on `gas_edit`, and the
conditional-effect-row gap on `gas_edit`. `widget_edit` gains
`set_binding_conversion`: rewrites the per-direction
conversion-function slot on an existing
`FMVVMBlueprintViewBinding`. Resolves the target binding by
`binding_id` FGuid string (the return shape of
`add_property_binding`) or by `binding_index` int into the
BlueprintView's Bindings array; picks the source-to-destination
or destination-to-source slot through the `direction` token; either
clears the slot (`conversion_function` empty / `none` / explicit
`clear=true`) or NewObject's a fresh
`UMVVMBlueprintViewConversionFunction` outered to the WBP and runs
`Initialize(WBP, CreateWrapperName(Binding, bSourceToDestination),
FMVVMBlueprintFunctionReference(WBP, UFunction*))` against the
resolved conversion UFunction. The conversion-function token
resolves through `/Script/Module.Class:Function` or
`/Game/.../BP_C:Function` (the dot-separated
`/Script/Module.Class.Function` form is also accepted). Replacing
an existing conversion calls `RemoveWrapperGraph` first so the
prior wrapper graph garbage collects. Mirrors the canonical
`UMVVMEditorSubsystem::SetSourceToDestinationConversionFunction` /
`SetDestinationToSourceConversionFunction` hot path; the
K2Node-class branch (async conversion nodes) stays out of scope
for this slice. With this op landing the `widget_edit` MVVM
surface narrows to bindings against fields beyond UFunction
conversions plus the wrapper-graph pin-default authoring side.
`gas_edit` gains `add_calculation_modifier`: appends an
`FGameplayEffectExecutionScopedModifierInfo` to the chosen
execution's `CalculationModifiers` array on a UGameplayEffect.
`execution_index` picks the row into the GE's `Executions` list
(run `add_execution` first). The captured attribute resolves
through the same `attribute` colon-form / `attribute_set` +
`attribute_name` pair resolver `add_modifier` uses; `source`
(`Source` / `Target`, default Source) + `snapshot` (default false)
build the `FGameplayEffectAttributeCaptureDefinition` through its
public attribute-backed constructor so `AggregatorType` lands on
`CapturedAttributeBacked`. Optional `modifier_op` (default
Additive) lands on `ModifierOp`; optional `magnitude` literal
float wraps into the scalable-float variant of
`FGameplayEffectModifierMagnitude` so the scoped modifier carries
a baseline magnitude. Recompiles + saves on success.
`gas_edit` also gains `add_conditional_effect`: appends an
`FConditionalGameplayEffect` to a parent UGameplayEffect's
`ConditionalGameplayEffects` array. The child effect resolves
through `effect_class` (UGameplayEffect-derived class path,
`/Script/Module.ClassName` or `/Game/.../BP` auto-suffixed with
`_C`) via the existing `ResolveClassByToken` helper; optional
`required_source_tags` lands on `RequiredSourceTags` through
`UGameplayTagsManager::RequestGameplayTag` with
`bErrorIfNotFound=false` so unknown tags surface a warning and
skip. Recompiles + saves on success. With this pair landing the
`gas_edit` BACKLOG row that called out `CalculationModifiers` and
`ConditionalGameplayEffects` per-row authoring as the follow-on
gap closes.

The pass before that shipped three deepening edit slices that
close the per-attribute Material Instance override gap, the
system-side Niagara exposed-parameter gap, and the GameplayEffect
executions-array gap. `material_edit` gains
`set_attribute_blendable`: flips a per-attribute override toggle
on a `UMaterialInstanceConstant`'s
`FMaterialInstanceBasePropertyOverrides` struct. The attribute
token resolves to a matching `bOverride_X` slot (`blend_mode` /
`shading_model` / `two_sided` / `opacity_mask_clip_value` /
`dithered_lod_transition` / `cast_dynamic_shadow_as_masked` /
`is_thin_surface` / `output_translucent_velocity` /
`has_pixel_animation` / `enable_tessellation` /
`displacement_scaling` / `enable_displacement_fade` /
`displacement_fade_range` /
`max_world_position_offset_displacement` /
`compatible_with_lumen_card_sharing`). The `enabled` boolean
lands on the `bOverride_X` flag through
`FBoolProperty::SetPropertyValue_InContainer` so the override's
bit-packed uint8 stays correct, and the optional `value` lands on
the matching payload field through
`FProperty::ImportText_Direct` so callers can set
`BlendMode = "BLEND_Masked"` / `OpacityMaskClipValue = 0.333` /
`TwoSided = true` in the same call. After the writes the op fires
`PostEditChangeProperty` on the MIC (which is what the editor
UI calls on the override-checkbox toggle path; the engine code
routes that through `UpdateStaticPermutation`) and follows up with
`UpdateOverridableBaseProperties` so the renderer-side cached
fields stay in sync. `niagara_edit` gains
`set_system_exposed_parameter`: resolves an existing
UNiagaraSystem and writes a parameter into the system's
`ExposedParameters` store
(`FNiagaraUserRedirectionParameterStore`). Reuses the same
type-token resolver and tightly-packed byte-buffer marshaling the
per-emitter `set_emitter_local_parameter` variant uses
(`float` / `int` / `bool` / `vec2` / `vec3` / `vec4` / `color` /
`quat`), and routes through the documented NIAGARA_API
`FNiagaraParameterStore::SetParameterData(buffer, var, bAdd=true)`
overload. When the parameter is missing the base store's
`SetParameterData` calls the virtual `AddParameter`, and the
user-redirect store's override normalises the bare token into the
`User.X` namespace and updates the redirection map automatically;
callers may therefore pass either the bare form (`MyFloat`) or the
fully-qualified `User.MyFloat` form. `gas_edit` gains
`add_execution`: appends an `FGameplayEffectExecutionDefinition`
to a UGameplayEffect's `Executions` array on the GE CDO. The
`calculation_class` token resolves to a
`UGameplayEffectExecutionCalculation` subclass through a
`/Script/Module.ClassName` reflection path, a `/Game/.../BP` class
path, or a bare class name fallback that walks
`TObjectIterator<UClass>`. Abstract subclasses fail closed. Optional
`passed_in_tags` lands on the entry's `PassedInTags`
`FGameplayTagContainer`; each tag flows through
`UGameplayTagsManager::RequestGameplayTag` with
`bErrorIfNotFound=false` so unknown tags surface in the response's
`unknown_tags` array rather than abort the op. `MarkBlueprintAsModified`
+ `CompileBlueprint` follow the same compile-on-write pattern
`add_modifier` uses; recompiles + saves on success. The
`CalculationModifiers` and `ConditionalGameplayEffects` rows stay
on the BACKLOG for follow-on edit ops.

The pass before that shipped four deepening edit slices that
close the Niagara sim-stage / sim-target gap, the GameplayEffect
magnitude variant gap, the Material Function reuse gap, and the
full MVVM property-binding row authoring gap. `niagara_edit`
gains `add_sim_stage` and `set_emitter_sim_target`. `add_sim_stage`
resolves an existing system + emitter handle, NewObject's a
`UNiagaraSimulationStageBase` subobject outered to the
UNiagaraEmitter (default subclass `UNiagaraSimulationStageGeneric`;
the `generic` short token plus full `/Script/Niagara.X` paths
and bare class names resolve), and routes through
`UNiagaraEmitter::AddSimulationStage(stage, EmitterVersion)`
(NIAGARA_API). Abstract subclasses fail closed. An optional
`stage_name` lands on `SimulationStageName` before the add so
the editor's stack viewmodel surfaces a designer-readable label.
`set_emitter_sim_target` writes
`FVersionedNiagaraEmitterData::SimTarget` against the four
documented tokens (`cpu` / `gpu` / `CPUSim` / `GPUComputeSim`).
The emitter version GUID for AddSimulationStage flows through
`FNiagaraEmitterHandle::GetInstance().Version` so callers do not
have to plumb the GUID. `gas_edit` gains
`set_modifier_magnitude` covering all four documented
`FGameplayEffectModifierMagnitude` variants. The previous
`add_modifier` op only emitted the scalable-float variant; this
op extends the surface to `AttributeBased` (reuses the
`add_modifier` attribute resolver plus a `FAttributeBasedFloat`
payload with backing-attribute capture source / snapshot /
coefficient / pre + post multiply / calculation type),
`SetByCaller` (`data_name` and / or `data_tag` with a clean
warning on unknown tags), and `CustomCalculationClass`
(`UGameplayModMagnitudeCalculation` subclass path; refuses
non-subclasses). The op assigns the fresh
`FGameplayEffectModifierMagnitude` through the public
constructor of each variant so the protected member fields stay
sealed behind the engine's friend-class contract; recompiles +
saves on success. `material_edit` gains `add_function_call`:
adds a `UMaterialExpressionMaterialFunctionCall` to a target
UMaterial through
`UMaterialEditingLibrary::CreateMaterialExpression` (same path
`add_expression` uses for every other subclass), then casts the
new expression and calls the BlueprintCallable
`SetMaterialFunction(NewFunction)` (ENGINE_API on
MaterialExpressionMaterialFunctionCall.h line 157) so the call
expression's `FunctionInputs` / `FunctionOutputs` arrays
regenerate from the bound function's declared input / output
pins; without that step the call node renders without pins and
a downstream `connect_expressions` call has no input names to
target. Position cascades through the same
`DeriveDefaultPosition` helper `add_expression` uses; optional
`name` renames the spawned expression so a follow-up
`connect_expressions` call can address the node by FName.
Recompiles + saves on success. With this op landing the
`material_edit` BACKLOG row that read "Material Functions
remain on the backlog" closes — the small `material_edit`
surface now covers function creation plus the call-site
authoring needed to reuse a function on a material. `widget_edit`
gains `add_property_binding` covering the full MVVM
property-binding row beyond the seeded-empty row that
`set_viewmodel` leaves behind. The op resolves a UWidgetBlueprint
plus a viewmodel slot on its `UMVVMBlueprintView` (by FName
label or by FGuid context id), resolves the source field on the
viewmodel's class (FProperty or UFunction; the field-variant
path stores the right kind), resolves the destination widget by
FName on the WBP's WidgetTree, and resolves the destination
field on the widget's class. The new FMVVMBlueprintViewBinding
spawns through `UMVVMBlueprintView::AddDefaultBinding` and the
returned mutable reference's SourcePath / DestinationPath /
BindingType / bEnabled / bCompile land in place. Path setup
goes through the public `FMVVMBlueprintPropertyPath::SetViewModelId`
+ `SetPropertyPath(WBP, FieldVariant)` and `SetWidgetName` +
`SetPropertyPath(WBP, FieldVariant)` setters. `binding_mode`
tokens are `one_way` (default) / `two_way` / `one_time`.
Conversion functions stay on the BACKLOG.

The pass before that shipped four deepening edit slices that
close the Niagara compile / flag gap, the bp_input trigger gap,
the MVVM minimum cut on `widget_edit`, and the previously skipped
graph-side `gas_edit set_ability_cue_tag`. `niagara_edit` gains
`request_compile` (routes through the public
`UNiagaraSystem::RequestCompile(bForce)` NIAGARA_API overload; the
optional `force` flag picks the bForce argument; the bool return
surfaces as `compile_requested`) and `set_emitter_flag` (writes a
boolean flag onto `FVersionedNiagaraEmitterData` through the
reflection database; supported flags `bLocalSpace` /
`bDeterminism` / `bInterpolatedSpawning` /
`bRequiresPersistentIDs`; the deprecated `bInterpolatedSpawning`
token routes through the modern `InterpolatedSpawnMode` enum slot
so modern emitters stay consistent, and the op accepts
`no_interpolation` / `run_update_script` /
`run_update_script_with_interpolation` tokens when value is a
string). `bp_input` gains `add_action_trigger`: parallels the
existing `add_action_modifier` op but writes to the mapping row's
`Triggers` array on `FEnhancedActionKeyMapping`. The trigger class
resolves through short tokens (`pressed` / `released` / `hold` /
`hold_and_release` / `tap` / `pulse` / `chord_action` / `down` /
`repeated_tap` / `combo`) or a UInputTrigger subclass path; the
optional flat `properties` dict applies through
`FProperty::ImportText_InContainer` so callers can land
`HoldTimeThreshold` / `TapReleaseTimeThreshold` / `ChordAction` /
`Interval` / etc. in the same call. `widget_edit` gains
`set_viewmodel` (MVVM minimum cut): resolves a UWidgetBlueprint
plus a UClass implementing `INotifyFieldValueChanged`
(`UMVVMViewModelBase` subclasses are the canonical case) and
routes through
`UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>`
to get-or-create the MVVM extension on the WBP. If the extension
has no `UMVVMBlueprintView` instance yet we call
`CreateBlueprintViewInstance()`, then append a fresh
`FMVVMBlueprintViewModelContext(Class, Name)` through
`UMVVMBlueprintView::AddViewModel`. An optional `binding_name`
also runs `UMVVMBlueprintView::AddDefaultBinding` so the asset
surfaces one seeded binding row ready for downstream
property-path edits. The full MVVM surface (conversion
functions, two-way bindings, bindings to widget properties
beyond root) stays in [BACKLOG.md](BACKLOG.md). `gas_edit` gains
`set_ability_cue_tag` as a graph-side authoring slice: the
previous pass dropped the op because UGameplayAbility has no
canonical UPROPERTY storing a per-ability cue association, but
the runtime BlueprintCallable `K2_ExecuteGameplayCue` lives on
the ability class itself. This op spawns a `UK2Node_CallFunction`
wired to that UFunction and pre-fills the `GameplayCueTag`
literal pin with the canonical struct ExportText form
`(TagName="Foo.Bar")`. Reuses an existing matching call node
when present so the op is idempotent. Unknown tags surface a
clean `cue_tag_warning` rather than crash.

The pass before that shipped four deepening edit slices that
close keyframe / curve / module gaps on Animation, UMG, Niagara,
and Material assets. `material_edit` gains
`create_material_function`: NewObject's a `UMaterialFunction` at
a `/Game/...` path through `UMaterialFunctionFactoryNew` and runs
an optional initial `expressions` list (same shape as
`add_expressions`, each spec carrying `class` plus optional
`name` alias / `position` / `properties` dict) through
`UMaterialEditingLibrary::CreateMaterialExpressionInFunction`,
then runs `UpdateMaterialFunction` once after the batch so any
existing materials that already reference the function recompile
in one pass. The expression-class resolver picks up
`function_input` / `function_output` short tokens for the
canonical seed pair. With this op landing the `material_edit`
BACKLOG row that read "Material Functions remain on the
backlog" closes. `animation_edit` gains `add_curve` and
`add_sync_marker`. `add_curve` registers a typed animation curve
(`Float` / `Vector` / `Transform`) on a UAnimSequenceBase
through `UAnimationBlueprintLibrary::AddCurve`, then optionally
seeds the new curve with `[time, value]` keyframes through
`AddFloatCurveKeys` / `AddVectorCurveKeys` /
`AddTransformationCurveKeys`. Float keyframes accept a number,
Vector keyframes take an `[x, y, z]` array, Transform keyframes
take `[time, location, rotation, scale]` triples (each component
a 3-vector; rotation interpreted as Euler degrees, matching the
editor's "Curve" panel). `add_sync_marker` writes an
`FAnimSyncMarker` onto a UAnimSequence's notify track through
`UAnimationBlueprintLibrary::AddAnimationSyncMarker`; the notify
track auto-creates through `AddAnimationNotifyTrack` when missing
and `frame` (int) wins over `time` (float seconds), mirroring
`add_notify`'s precedence. `widget_edit` gains `add_keyframe`:
resolves a target track by global index across the animation's
master + binding tracks, finds or spawns a section through
`UMovieSceneTrack::CreateNewSection` + `AddSection` (extending
the section range to cover the key time via
`UMovieSceneSection::ExpandToFrame`), and writes the key through
`FMovieSceneFloatChannel::AddCubicKey` / `AddLinearKey` /
`AddConstantKey`, falling through to `FMovieSceneDoubleChannel`
for the 5.4+ vector + transform track shape that switched away
from float channels. `value` accepts a number (scalar tracks),
an `[x, y, z, w?]` array, or an `{x,y,z,w}` / `{r,g,b,a}` object;
channels write 0..N-1 starting at `channel_offset`. `niagara_edit`
gains `add_module_to_stage`: resolves an existing system + emitter
handle and an existing `UNiagaraScript` configured as a Module
(`Usage = Module`), walks the spawn / update script's source
graph for the `UNiagaraNodeOutput` whose `GetUsage()` matches
the chosen stage (we walk `Source->NodeGraph->Nodes` directly
because `UNiagaraGraph::FindOutputNode` is not exported as
`NIAGARAEDITOR_API` in 5.7), and routes through the documented
`FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript,
OutputNode, TargetIndex, SuggestedName)` overload (that one is
`NIAGARAEDITOR_API` exported). `MarkNotSynchronized` on the
script's source so the next compile re-runs.

The pass before that shipped four deepening edit slices that
fill small breadth gaps on already-shipped tools. `widget_edit`
gains `add_animation` and `add_animation_track`: the first
appends a `UWidgetAnimation` to the WBP's `Animations` array
with a fresh `UMovieScene` whose playback range covers
`[0, duration]` seconds at a 20 fps display rate (matching the
AnimationTabSummoner default the UMG editor uses for new
animations); the second resolves a UMovieSceneTrack subclass by
short token (`float` / `color` / `vector` / `transform` /
`visibility` / `event` / `material` / `audio` / `bool` / `byte`)
or `/Script/Module.ClassName` path, walks the animation for an
existing FWidgetAnimationBinding to the named target widget, and
either reuses that binding GUID or spawns a fresh possessable +
binding pair through `UMovieScene::AddPossessable` plus a manual
FWidgetAnimationBinding row before calling
`UMovieScene::AddTrack(TrackClass, BindingGuid)`. An optional
`property_path` lands on either `PropertyPath` or `PropertyName`
through reflection so the property-track surface gets a friendly
entry without us spelling out every track subclass. Per-keyframe
authoring stays on the BACKLOG. `material_edit` gains
`create_parameter_collection` and `add_collection_parameter`.
The first NewObject's a `UMaterialParameterCollection` at a
`/Game/...` path through `UMaterialParameterCollectionFactoryNew`;
the second resolves the target MPC, refuses duplicate names
across both arrays, and appends a typed entry to
`ScalarParameters` or `VectorParameters` based on a `scalar` /
`float` / `vector` / `color` token. Default values come in as a
JSON number for scalars or `[r, g, b, a]` array / `{r,g,b,a}`
object for vectors. The op runs `PreEditChange` on the target
array UPROPERTY before the mutation and a synthesised
`FPropertyChangedEvent(EPropertyChangeType::ArrayAdd)` afterward,
which walks the asset's PostEditChangeProperty path and
regenerates `StateId` plus requeues every UMaterial referencing
the collection through `FMaterialUpdateContext`, the canonical
"this MPC changed, recompile me" broadcast. With this pair
landing the `material_edit` BACKLOG row narrows to Material
Functions. `gas_edit` gains `create_cue_notify`: NewObject's a
`UBlueprint` at a `/Game/...` path with a
`UGameplayCueNotify_Static` (default) or
`AGameplayCueNotify_Actor` parent. The `parent_class` arg
accepts short tokens (`static` / `actor` / `notify_static` /
`notify_actor`) plus `UGameplayCueNotify_*` subclass paths. An
optional `cue_tag` writes the asset's `GameplayCueTag` UPROPERTY
through reflection: we resolve the tag through
`UGameplayTagsManager::Get().RequestGameplayTag` with
`bErrorIfNotFound=false` so unknown tags surface a clean
`cue_tag_warning` rather than crash, and the mirror FName
`GameplayCueName` field also picks up the typed tag string to
match the engine's PostEditChangeProperty fix-up flow.
`set_ability_cue_tag` was the fourth target on this pass but
stays on the BACKLOG: `UGameplayAbility` exposes cue invocation
through the BlueprintCallable `K2_AddGameplayCue` /
`K2_ExecuteGameplayCue` pair, but there is no canonical
UPROPERTY storing a per-ability cue tag association, so the
"bind a cue tag to an ability" surface needs a graph-side
authoring slice on the ability's event graph rather than a CDO
write; the prompt called out the same risk and authorised the
drop. `bp_input` gains `add_action_modifier`: writes a
`UInputModifier` subobject onto an existing
`FEnhancedActionKeyMapping` row's `Modifiers` array. The row is
located by walking `IMC->GetMappings()` for an
`(action, key)` match (action accepts a `/Game/...`
UInputAction path or a short name matched against the row's
`Action->GetName`); `modifier_class` resolves through short
tokens (`negate` / `scalar` / `dead_zone` / `swizzle_axis`) or
a UInputModifier subclass path. The new modifier outers under
the IMC asset (matching the editor's `Instanced` subobject
convention) and an optional flat `properties` dict applies
through `FProperty::ImportText_InContainer` so callers can land
`Order` on a swizzle, `Scalar` on a scalar, `LowerThreshold` /
`UpperThreshold` / `Type` on a dead zone, `bX` / `bY` / `bZ` on
a negate, etc., in the same call. Failed property entries
surface under `skipped`, mirroring the convention shipped by
`widget_edit set_slot_property` and
`pcg_graph_edit set_node_settings`.

The pass before that shipped three deepening edit slices that
close one persistent skip and add two new edit op surfaces.
`foliage_edit` gains `place_foliage_instances`: places N
foliage instances on the IFA for a chosen level. The op
resolves (or spawns) the IFA through
`AInstancedFoliageActor::GetInstancedFoliageActorForLevel(Level,
/*bCreateIfNone=*/true)`, ensures the type binding through
`AInstancedFoliageActor::AddFoliageType` (so the caller can
skip the explicit `add_foliage_type` step), and walks the
required `locations` array building one `FFoliageInstance` per
row through `FFoliageInfo::AddInstance(InSettings,
NewInstance)` (FOLIAGE_API). Optional parallel `rotations`
(`[pitch, yaw, roll]` triples) and `scales` (`[x, y, z]`
triples) override the per-instance rotation / scale; missing
rotations land at zero rotation, missing scales fall back to
the foliage type's per-axis ScaleX / Y / Z interval midpoint
so the placement matches the painting density tool's
defaults. After the inner loop the IFA's owning level is
dirtied + saved (the IFA is a level actor, not an asset);
`Refresh(Async=false, Force=true)` runs once at the end so
the editor viewport sees the new instances without per-add
ISMC tree rebuilds. The per-stroke painting brush surface
stays on the BACKLOG. `gas_edit` gains the cost / cooldown
rebind pair: `set_ability_cost` writes a UGameplayAbility's
`CostGameplayEffectClass` UPROPERTY and `set_ability_cooldown`
writes `CooldownGameplayEffectClass`. Both accept an `effect`
field that is either a `/Script/Module.ClassName` path, a
`/Game/...` Blueprint class path (auto-suffixed with `_C` if
missing), or `none` / empty / explicit `clear=true` to clear
the binding. The writes route through reflected
`FClassProperty` + `ContainerPtrToValuePtr<TSubclassOf<UObject>>`
on the ability's UClass; the FProperty path side-steps the
5.7 visibility tightening that demoted these fields from
public to protected (the tag-container side of the ability
already routed through reflection for the same reason). Each
op recompiles + saves on success unless overridden. With
this pair landing the `gas_edit` BACKLOG row narrows to
GameplayCue authoring. `niagara_script_edit` gains
`add_input_parameter` via the documented workaround. The
canonical authoring entry point (`UNiagaraGraph::AddParameter`
in NiagaraGraph.h lines 354 to 356, three overloads) is still
not exported by NIAGARAEDITOR_API in 5.7, so the new op
writes the parameter into the script's
`RapidIterationParameters` store directly through the public
`FNiagaraParameterStore::AddParameter` (NIAGARA_API) plus a
follow-up `SetParameterData` for the optional default value.
Type tokens are the same set niagara_edit accepts (`float` /
`int` / `bool` / `vec2` / `vec3` / `vec4` / `color` /
`quat`); missing `value` lands the parameter at type-zero
default. After the write we run `MarkNotSynchronized` +
`PostEditChangeProperty` on the script's source so the next
compile-id machinery refreshes. Limitation noted in the
response and in the README addition: a graph-driven
recompile that re-derives the parameter set from the source
graph alone will not see the addition; designers wanting
graph-side persistence should re-author through the editor's
Module Inputs panel. `pie_test_scene` PIE-needed assertion
kinds (`actor_visible_in_pie`, `var_equals_in_pie`,
`event_fired_within`) were the fourth target on this run but
stay on the BACKLOG; the synchronous bridge command callback
returns inside one editor tick, so any kind that needs the
PIE world to spin up first cannot resolve in the same
response cycle without rewiring the bridge to support
deferred / awaited responses.

The pass before that shipped three deepening tools plus one
maintenance fix. `animation_graph_edit` gains two more edit
ops on the AnimGraph state-machine surface: `add_transition`
spawns a `UAnimStateTransitionNode` between two existing states
on the same state machine through the same public schema-action
template (`FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>`)
and runs `CreateConnections(From, To)` so the transition arrow
points the right way; the rule `BoundGraph` (the Boolean
condition graph) is wired by `PostPlacedNewNode`; an optional
`priority` lands on `PriorityOrder`; the duplicate-edge guard
returns an error rather than spawning a parallel transition.
`set_state_animation` writes the per-state animation asset
reference into the state's `BoundGraph`: it resolves the right
player class for the asset shape (`UAnimSequence` ->
`UAnimGraphNode_SequencePlayer`, `UBlendSpace` ->
`UAnimGraphNode_BlendSpacePlayer`, etc.) through the engine's
`GetNodeClassForAsset(AssetClass)` helper. If a matching player
already lives in the BoundGraph the op swaps its asset; otherwise
it spawns a fresh player on the BoundGraph, runs
`SetAnimationAsset` + `CopySettingsFromAnimationAsset` (so node-
specific defaults pick up), and wires the player's `Pose`
output into the state's `Result` input pin. `ik_rig_edit`
gains the polymorphic solver mutation surface: `add_solver`
resolves a `UIKRigSolverBase`-derived `UScriptStruct` from a
short token (`full_body` / `fbik` / `limb` / `pole` /
`body_mover` / `set_transform`), a bare struct name, or a
`/Script/Module.FStructName` path, then routes through the
typed `UIKRigController::AddSolver(UScriptStruct*)` overload
(the BlueprintCallable string overload sits next to it but
the typed one keeps us honest about which struct landed);
`remove_solver_at` calls `UIKRigController::RemoveSolver(Index)`
behind a stack-bounds guard; `set_solver_settings` resolves
the solver at the given stack `index`, walks a flat `properties`
dict, and applies each entry through `FProperty::ImportText_InContainer`
against the reflected `GetSolverSettingsType()` struct rooted
at `GetSolverSettings()`; failed entries surface under the
response's `skipped` array; on success the solver's official
`SetSolverSettings` mutator runs so per-derived-type custom
logic (mirror copies, internal cache invalidation) fires.
`niagara_script_edit` gains `set_module_usage` (writes the
script's `ENiagaraScriptUsage` member; the underlying field
is public on `UNiagaraScript`, so we drop the new value in
directly, mark the source graph not-synchronised through
`UNiagaraScriptSourceBase::MarkNotSynchronized` so the next
compile request reruns, and call `PostEditChangeProperty` so
any in-process compile-id machinery refreshes). The
`add_input_parameter` op stayed dropped on this pass: the
canonical entry point (`UNiagaraGraph::AddParameter`) is not
exported by `NIAGARAEDITOR_API` in 5.7, and a clean-room
implementation cannot link it without copying from the engine.
The maintenance fix lays down a UE 5.7 portability batch.
5.7 tightened the visibility on every reflected
`FGameplayTagContainer` field on `UGameplayAbility` from public
to protected (CancelAbilitiesWithTag / BlockAbilitiesWithTag /
ActivationOwnedTags / ActivationRequiredTags /
ActivationBlockedTags / SourceRequiredTags / SourceBlockedTags
/ TargetRequiredTags / TargetBlockedTags), plus
`CostGameplayEffectClass` / `CooldownGameplayEffectClass` /
`AbilityTriggers`. The fields stay reflected, so the GAS
inspect path now goes through the reflection accessor
(FindFProperty + ContainerPtrToValuePtr) rather than the direct
member; this keeps us clean-room (we read through the public
reflection database, not through friending the class) and
keeps the code compatible with 5.5 / 5.6 builds where the
same members were public. The same pass demoted public access
to `AActor::NetUpdateFrequency` in favour of the
`GetNetUpdateFrequency()` accessor pair (works on 5.5 / 5.6
too); `UPCGSettings::PostEditChangeProperty` to protected (we
upcast the settings pointer to `UObject*` so the public base
declaration of the virtual dispatches correctly); and the
ternary-with-nullptr pattern in `performance_audit` to a
two-statement init (5.7's `TSharedPtr` deduction stopped
accepting `bIncludeSamples ? MakeShared<FJsonObject>() : nullptr`
because the conversion to `TSharedRef` no longer round-trips).
Adds `MetasoundFrontend` to PublicDependencyModuleNames so
`FMetasoundFrontendClassName::Parse` and `GetFullName` link
again, and adds the `GameplayEffectExecutionCalculation.h`
forward-include the executor row needed.

The pass before that shipped one maintenance fix and three deepening
tools. The maintenance fix walks every `Sproft*Commands.cpp` file
under `UnrealMCP/Source/UnrealMCP/Private/Commands/` and renames each
helper that is defined in two or more files with a per-file prefix
(e.g. `SceneCompose_LevelLabel`, `ActorInspect_ResolveActor`,
`AssetFactory_SplitPackagePath`). Helpers defined in only one file
stay as-is. Twenty-five helper names across forty-two cpp files
ended up touched, and the bundled
`FlopperamUnrealMCP/Plugins/UnrealMCP/Source/Commands/` tree picks
up the same renames in the same commit. The unity-build collision
that batch 22 hit (anonymous namespace contents from one cpp
clashing with a same-named helper from another cpp inside the
unified TU) goes away because each helper is now globally unique
inside the unity-grouped TU. `unreal_api` gains three new ops:
`list_classes` (substring filter across every loaded UClass with
optional `include_subclasses_of` / `include_native` /
`include_blueprint` / `include_interfaces` / `include_abstract`
filters; backed by `TObjectIterator<UClass>` plus
`GetDerivedClasses(Parent, Out, /*bRecursive=*/true)` when a parent
is given), `find_in_subclasses` (parent class plus property /
function name substring; walks each descendant's own declarations
through `EFieldIteratorFlags::ExcludeSuper` so the result tells the
caller which descendant ADDS a member rather than which inherits
it), and `class_diff` (two class paths; returns
`properties.added` / `properties.removed` / `properties.changed`
plus the parallel function trio with the canonical signature
including parameter list / return type / the relevant flag set).
Read-only and reflection-driven; no new module deps. `pcg_graph_edit`
gains the per-node settings-write hole that the earlier slice left
open: one op `set_node_settings` that pulls
`UPCGNode::GetSettings()`, applies a flat `properties` dict through
`FProperty::ImportText_InContainer`, runs
`Settings->PostEditChangeProperty` plus
`Node->OnNodeChangedDelegate.Broadcast(Node, EPCGChangeType::Settings)`
so any open PCG editor refreshes the node's pin layout, then
saves the asset. Failed entries surface under `skipped` mirroring
the convention shipped by `chaos_edit set_simulation_settings`.
`animation_graph_edit` gains the focused-minimum-cut edit slice
that the earlier pass dropped: one op `add_state` that resolves a
state machine on the AnimBP by name (walks every UEdGraph reachable
through FunctionGraphs / UbergraphPages / MacroGraphs plus per-node
GetSubGraphs / per-graph SubGraphs for any
`UAnimGraphNode_StateMachineBase` whose `GetStateMachineName()`
matches), reaches through `EditorStateMachineGraph` to the
`UAnimationStateMachineGraph`, and spawns a new `UAnimStateNode`
through the public schema-action template
`FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(Graph, NewObject<UAnimStateNode>(), Location)`.
PerformAction wires the per-state `BoundGraph` (the per-state
AnimGraph that holds the pose subtree) via PostPlacedNewNode plus
AllocateDefaultPins so the state opens cleanly in the editor. The
duplicate-name guard surfaces a clear error rather than silently
spawning; `compile=true` (default) recompiles after the edit, and
`save=true` (default) writes to disk. Adds `AnimGraph` to the
editor-only `PrivateDependencyModuleNames`. Transitions and
per-state property writes stay on this list.

The pass before that deepened edit-side coverage on two already-shipped
multi-op tools and folded one maintenance fix on top. `ik_retarget`
gains three edit ops routed through the editor-only
`UIKRetargeterController::GetController(Retargeter)` accessor
(already in IKRigEditor, which we picked up for `ik_rig_edit`):
`set_source_ik_rig` and `set_target_ik_rig` rebind the retargeter's
source / target IK Rig through `UIKRetargeterController::SetIKRig`
(pass `clear=true` to unbind the side); `set_retarget_pose` switches
the active retarget pose for either side through
`SetCurrentRetargetPose(PoseName, Side)` with the missing-pose guard
the controller already implements. `behavior_tree` gains three more
edit ops on the Blackboard side of the BT/BB pair: `set_blackboard`
rebinds the BT's BlackboardAsset slot; `add_blackboard_key` appends a
typed FBlackboardEntry to a target Blackboard's `Keys` array (short
token resolver covers `bool` / `int` / `float` / `string` / `name` /
`vector` / `rotator` / `object` / `class` / `enum` / `struct` plus
the `/Script/AIModule.UBlackboardKeyType_*` paths and short class
names; wires `BaseClass` on Object / Class keys, `EnumType` on Enum
keys, and `DefaultValue.InitializeAs(Struct)` on Struct keys when the
caller passes the corresponding inner-type hint); `remove_blackboard_key`
removes an own key by FName. The Blackboard target resolves either
through an explicit `blackboard` arg or, when only `tree` is set,
through the BT's BlackboardAsset slot. The maintenance fix re-syncs
the bundled `FlopperamUnrealMCP/Plugins/UnrealMCP/Source/` tree with
the canonical `UnrealMCP/Source/` tree, drops a `SOURCE_NOTE.md`
beside the bundled `UnrealMCP.uplugin` so future readers know where
the canonical tree lives, and brings the bundled uplugin manifest in
line with the canonical one (the bundled tree had drifted far behind
and the bundled project was failing to compile against the canonical
bridge header). The `animation_graph_edit` edit slice was the
fourth target on that pass but stayed skipped at the time: the
AnimGraph editor module surface (UAnimStateNode + UAnimStateTransitionNode +
the state machine's UEdGraph plus the `BoundGraph` per-state
subgraph authoring) was wider than the focused-minimum cut shape
we ship for small variants. The current pass collapses the cut
to a single `add_state` op so the slice still ships small.

The pass before that deepened edit-side coverage on four
already-shipped multi-op tools: `behavior_tree` adds three edit
ops (`add_child_task` appends a UBTNode child slot under a target
composite via `UBTCompositeNode::Children.AddDefaulted_GetRef()` +
`ChildComposite` / `ChildTask` assignment with `InitializeFromAsset`
wiring; `add_decorator` appends a UBTDecorator to a child slot's
`Decorators` array; `add_service` appends a UBTService to a
composite's `Services` array). The new short-name resolver covers
the stock task / decorator / service set (`wait` / `move_to` /
`blackboard` / `cooldown` / `loop` / `time_limit` / `default_focus`
etc.) plus `/Script/Module.ClassName` and `/Game/...` BP class
paths. `sequencer_edit` adds two edit ops: `add_track` resolves a
UMovieSceneTrack subclass by short token (`transform` /
`camera_cut` / `skeletal_animation` / `audio` / `event` / `float` /
`subscene` / `bool` / `byte`) plus full paths and routes
binding-scoped tracks through `UMovieScene::AddTrack(Class, Guid)`,
camera cut tracks through `UMovieScene::SetCameraCutTrack` with
`NewObject<UMovieSceneTrack>`, and master tracks through the
no-binding `UMovieScene::AddTrack(Class)` overload (5.4+ unified
the master / no-binding path); `add_section` calls
`UMovieSceneTrack::CreateNewSection` (so the track decides its
native section subclass), wraps `start_frame` + `duration_frames`
into a `TRange<FFrameNumber>` (inclusive start, exclusive end), and
attaches via `AddSection`. `gas_edit` adds three modifier ops:
`add_modifier` appends an `FGameplayModifierInfo` with attribute /
modifier op / scalable-float magnitude (modifier op covers
`Add` / `Multiply` / `Override` / `Division` plus the canonical
UE 5.x names case-insensitive); `remove_modifier_at` removes by
index; `set_attribute_default` writes a UAttributeSet's base value,
branching between legacy float storage and the
`FGameplayAttributeData` struct path. `metasound_edit` adds the
graph-authoring slice: `add_node` parses a
`Namespace.Name[.Variant]` token through
`FMetasoundFrontendClassName::Parse` and routes through
`UMetaSoundBuilderBase::AddNodeByClassName(ClassName, Result, MajorVersion=1)`;
`connect_nodes` parses two FGuid strings (from add_node return
values) and routes through the public
`ConnectNodes(SourceNode, OutputName, DestinationNode, InputName)`
overload. The builder is obtained per-asset through
`UMetaSoundBuilderSubsystem::AttachBuilderToAssetChecked`. Each
mutating op runs `MarkPackageDirty` and saves to disk by default;
recompile + save on the GAS side route through the owning
Blueprint when the asset is a UBlueprint.

This pass also lays down a small portability cleanup. UE 5.7's
stricter `FString::Printf` format-string check (the new
`TCheckedFormatStringPrivate` wrapper on argument 1) rejects passing
a runtime `FString` as the format. The four shipped Sproft commands
that used `'/Script/Module.%s'` through Printf for namespace
fallback lookups (animation_edit, bp_component, scene_compose,
scene_query) now concatenate the namespace prefix + class name
directly. Two doxygen comment blocks on
`SproftFoliageEditCommands.h` and `SproftNiagaraEditCommands.h` had
nested `/* ... */` fragments that parse cleanly under previous UE
versions but trip the 5.7 pre-processor; both un-nest. The
uplugin manifest also gains the `GameplayAbilities`,
`GameplayTagsEditor`, and `GeometryCollectionPlugin` entries so the
dependency warnings UBT raised about the modules pulled in by
gas_edit / chaos_edit go away.

The pass before that deepened edit-side coverage on four
read-only tools shipped earlier: `pcg_graph_edit` adds an edit
slice (`add_node` through `UPCGGraph::AddNodeOfType<T>`,
`connect_pins` through `UPCGGraph::AddEdge`, `remove_node`
through `UPCGGraph::RemoveNode`); `niagara_edit` adds
`add_emitter_from_asset` through the editor-only
`UNiagaraSystem::AddEmitterHandle(SourceEmitter, HandleName,
VersionGuid)` overload (defaulting the version GUID to the
source emitter's `GetExposedVersion().VersionGuid` so the
system pulls the active branch); `ik_rig_edit` adds three ops
(`set_retarget_root` / `add_retarget_chain` / `add_ik_goal`)
routed through the editor-only `UIKRigController` accessor
(`UIKRigController::GetController(Rig)` plus the documented
`SetRetargetRoot` / `AddRetargetChain(ChainName, StartBone,
EndBone, OptionalGoalName)` / `AddNewGoal(GoalName, BoneName)`
methods); `chaos_edit` adds `set_simulation_settings` (writes
a flat property dict against the asset's reflected simulation
surface (`Mass` / `MinimumMassClamp` / `bMassAsDensity` /
`EnableClustering` / `MaxClusterLevel` / `DamageModel` etc.)
through `FProperty::ImportText_InContainer`, then runs
`InvalidateCollection` so the cached simulation data rebuilds)
plus `import_static_mesh` (appends a UStaticMesh through the
editor-only `FGeometryCollectionConversion::AppendStaticMesh`).
Each mutating op runs `MarkPackageDirty` and saves to disk by
default; failed property writes on `chaos_edit set_simulation_settings`
surface under `skipped` with a reason.

Adds `IKRigEditor` and `GeometryCollectionEditor` to the
editor-only `PrivateDependencyModuleNames`. The remove-side
ops (chain remove, goal remove, solver mutation,
bone-settings writes for IK Rig; per-instance damage threshold
override and the re-cluster ops for Chaos) stay on this list,
along with the broader Niagara emitter-side authoring surface
(parameter store mutations, module / sim-stage authoring,
sim-target / determinism flag writes, request-compile) and
the PCG pin-rename / per-node settings-property mutate ops.

The pass before that shipped four new tool families that close
out the remaining persistent skips: `landscape_edit` (small
variant retry covering `set_landscape_material` and
`import_heightmap_png`, the slice we dropped on the earlier
pass because the sculpt brush surface did not collapse to one
focused minimum), `pcg_graph_edit` (read-only first slice over
`UPCGGraph` covering nodes / pins / edges plus the graph's
exposed input / output surface), `niagara_script_edit`
(read-only first slice over `UNiagaraScript` covering usage /
input / output / attribute parameter sets plus the cached VM
compile data including the GPU shader parameter metadata), and
`animation_graph_edit` (read-only first slice over
`UAnimBlueprint` going one level deeper than the existing
`animation_inspect` AnimBP path: per state machine returns the
per-state details, the per-machine transitions array with
`previous_state` / `next_state` / blend-mode tokens, and the
flat AnimGraph node-property list off
`UAnimBlueprintGeneratedClass::AnimNodeProperties`). Together
the four close `landscape_edit`, `pcg_graph_edit`,
`niagara_script_edit`, and `animation_graph_edit` on the
priority list. The earlier pass shipped four tool families that
went wider: `ik_rig_edit` (read-only first slice over
`UIKRigDefinition` paired with the existing `ik_retarget`),
`chaos_edit` (read-only first slice over `UGeometryCollection`),
`skills` (workflow-doc lookup over a curated `Python/skills/` index
with `replication` / `enhanced-input` / `gameplay-tags` /
`crafting-data-tables` pre-baked), and `niagara_edit` (ultra-minimum
cut: one op `create_niagara_system` through
`UNiagaraSystemFactoryNew::InitializeSystem(System, false)` — no
emitters, no parameter store, no module / sim-stage authoring;
breaks the persistent four-skip on the Niagara write side, the
broader authoring surface stays on this list). `ik_rig_edit` walks
the asset's public surface (preview mesh, retarget root, retarget
chains with `start_bone` / `end_bone` / `ik_goal_name`, IK goals
with current + initial transforms and position / rotation alpha)
plus the polymorphic solver stack (each solver's struct type,
enabled flag, optional `start_bone` / `end_bone`, a
reflection-driven `settings` dict reflected off `GetSolverSettings()`
plus the settings struct type, and a `bone_settings` array for
solvers that gate `UsesCustomBoneSettings()` true). Filters:
`include_solver_settings` / `include_bone_settings` / `max_chains` /
`max_goals` / `max_solvers`. `chaos_edit` walks both the
`UGeometryCollection` asset surface (geometry sources, simulation
block with clustering / damage model / mass + density / removal
surface, materials, Nanite block) and the underlying
`FGeometryCollection` managed-array data (vertex / face / geometry /
transform counts, per-fracture-level histogram with parallel
cluster counts, max level + bone hierarchy depth, per-element
SimulationType counts). Reaches through `FindAttribute<int32>(LevelAttribute, TransformGroup)`
on the managed-array collection so legacy assets without level
data degrade to "level info missing" rather than crashing. Adds
GeometryCollectionEngine and Chaos to PublicDependencyModuleNames.
`skills` indexes flat markdown files under `Python/skills/`; the
`get` op returns one body, `list` returns every entry's slug + first
H1, `search` is a case-insensitive substring filter over slug +
title. Adding a new skill entry is a file-add, not a code change.
`niagara_edit` ships the create-only branch through the editor-only
`NiagaraEditor` PrivateDependencyModuleName so we link
`UNiagaraSystemFactoryNew::InitializeSystem` without pulling
`FNiagaraStackGraphUtilities` (NiagaraEditor private) into our
public surface; an emitter-less system opens with a "no emitter"
warning on the asset banner, which is the documented behaviour for
this minimum-cut slice.

The pass before that shipped three new tool families: `unreal_api` (reflection-driven query
of the live UE5 type database), `sound_asset_edit` (small variant
covering Sound Cue creation, wave-player append, and attenuation
rebind), and `ik_retarget` (read-only first slice over the
UIKRetargeter op stack). `unreal_api` covers three ops keyed by `op`:
`describe` (parent class + direct child class list + interfaces +
every UPROPERTY field with type / flags / tooltip + every UFUNCTION
method with full signature / flags / tooltip), `find_property`
(case-insensitive substring search across one class's properties),
and `find_function` (case-insensitive substring search across one
class's functions). The walk uses `TFieldIterator<FProperty>` /
`TFieldIterator<UFunction>` plus `GetDerivedClasses` from
`UObjectHash.h` so it answers from the live in-process reflection
database without an offline reference table. `sound_asset_edit`
covers `create_sound_cue` (NewObject's a USoundCue at a `/Game/...`
path; optional `sound_wave` parameter resolves the named USoundWave
and wires a USoundNodeWavePlayer into the cue's `FirstNode` slot),
`add_sound_node_wave_player` (USoundCue::ConstructSoundNode +
SetSoundWave + optional FirstNode rebind through
`LinkGraphNodesFromSoundNodes`), and `set_attenuation` (writes the
`AttenuationSettings` UPROPERTY on the USoundBase shape; null /
empty clears the override). `ik_retarget` handles the read side:
asset path / class, source / target IK Rig paths plus has-rig flags,
current source / target retarget pose names with their
bone-rotation-offset counts and root-offset flags, the retarget op
stack (per-op `index` / `name` / `parent_name` / `struct_type` /
`enabled` / `initialized` / optional `chain_mapping`), and aggregate
counts. The walk reaches through `FInstancedStruct::GetPtr<FIKRetargetOpBase>`
plus the per-op `GetChainMapping()` accessor that the 5.6 op refactor
exposes. Adds IKRig to PublicDependencyModuleNames and the IKRig
plugin to the uplugin manifest. Edit-side ops on each tool stay on
this list.

The pass before that extended `gas_edit` with three edit ops, added
two new tools (`performance_audit`, `pie_test_bp`), and laid down
the small variant of `metasound_edit`. `gas_edit` now answers
`create_gameplay_ability` (NewObject's a UBlueprint at a `/Game/...`
path with a UGameplayAbility-derived parent class, default
`/Script/GameplayAbilities.GameplayAbility`),
`create_gameplay_effect` (same shape with a UGameplayEffect parent;
optional `duration_policy` / `duration_magnitude` write through the
CDO before the first compile), and `set_gameplay_tags` (tag-container
mutation on either asset shape; UGameplayAbility writes through the
reflected `AbilityTags` / `CancelAbilitiesWithTag` /
`BlockAbilitiesWithTag` etc. fields, UGameplayEffect routes through
`FindOrAddComponent` on the asset / target / block-ability tag
GE-component subclasses and calls each component's `SetAndApply`
mutator). `performance_audit` (small read-only) reports a per-metric
avg / peak / last triple over the active editor viewport's
FStatUnitData ring (frame / game / render / RHI / GPU) plus a live
globals snapshot (`GAverageMS` / `GAverageFPS` plus the
cycle-converted thread / GPU times through `RHIGetGPUFrameCycles`).
`pie_test_bp` (small) is the Blueprint-side counterpart to
`pie_test_scene`: one assertion kind in this slice
(`default_value_equals`), targeting a UPROPERTY FName on the
Blueprint's CDO, with the JSON literal canonicalised through the
property's `ImportText` -> `ExportText` round-trip and compared
against the CDO's `ExportText` output. `metasound_edit` (small) lays
down `create_metasound_source` (UMetaSoundSource with optional
output format / sample-rate / block-rate overrides) and
`create_metasound_patch` (UMetaSoundPatch). Both ops route through
`UMetaSoundEditorSubsystem::GetChecked()`'s public `InitAsset` +
`RegisterGraphWithFrontend`. The graph-authoring surface (add
nodes, connect pins) stays on this list.

The pass before that extended `pie_test_scene` with two new
assertion kinds and shipped two new edit-slice tools that pair with
existing read-only inspectors. `pie_test_scene` now answers
`actor_overlapping_tag` (target = actor name, expected = an FName
tag string; pass = the actor's `Tags` array contains that FName)
and `var_equals` (target = actor name, expected = a `{var, value}`
dict; pass = the actor's UPROPERTY ImportText-matches the
canonicalized representation of `value`, which lets vector /
rotator / transform / FString / gameplay tag fields all flow
through one comparison path). The earlier "PIE-only" framing on
those kinds was overcautious; both run against the editor world
fine. `animation_edit` (small) lays down `set_rate_scale` (float
write on UAnimSequenceBase), `set_additive` (UAnimSequence
AdditiveAnimType / RefPoseType / RefPoseSeq / RefFrameIndex),
and `add_notify` (FAnimNotifyEvent append to a notify track
through `UAnimationBlueprintLibrary::AddAnimationNotifyEvent` /
`AddAnimationNotifyStateEvent`, with auto-create on the named
track). `foliage_edit` (small) lays down `add_foliage_type`
(UFoliageType registration on the level's
`AInstancedFoliageActor`, spawning the IFA when missing) and
`set_foliage_density` (Density / DensityAdjustmentFactor /
Radius / per-axis ScaleX-Y-Z FFloatInterval writes on a
UFoliageType asset). The heavier branches (animation curves /
key frames / sync markers, foliage instance placement, BT child
append / decorator insertion / Blackboard key edits, Sequencer
track add / section move / spawnable creation / camera-cut
creation) stay on this list.

The pass before that shipped three small wide-domain read-only
tools that round out the orientation surface the agent reaches
for at the start of a session: `project_context` (one-shot
project summary covering identity, engine version, enabled
plugins filtered to user-installed, source modules, top-level
Content folders with asset counts, and the active map +
GameMode + default pawn through both per-level override and
project-wide default), `animation_inspect` (class-keyed dump
for USkeletalMesh / UAnimSequence / UAnimMontage / UBlendSpace
/ UAnimBlueprint covering bones / sockets / notifies / sections
/ slot tracks / blend-space axes / state machines), and
`cpp_source` (read header + cpp pair through
`FSourceCodeNavigation::FindClassHeaderPath` /
`FindClassSourcePath` for class-driven lookup, or extension-swap
inference for direct disk-path lookup, with per-file truncation
caps and existence flags).

The pass before that shipped three read-only inspectors covering
broader-domain coverage that was previously locked behind
`python_execution`: `landscape_inspect` (every `ALandscape` actor's
component-grid configuration, materials, layers, bounds, and
heightmap / weightmap texture sets), `foliage_inspect` (every
`AInstancedFoliageActor`'s foliage type list with mesh / actor source
paths, density / radius / scale intervals, instance counts, and
optional sampled world locations), and the read-only first slice of
`sequencer_edit` for `ULevelSequence` (master tracks, sections,
possessables, spawnables, plus tick / display frame rates and
playback range).

The pass before that shipped `bp_export`, `behavior_tree` (read-only
slice), the `widget_edit` slot-property surface, and `gas_edit`
(read-only slice). Together they close the canonical Blueprint
snapshot for diffable round-trips, give the agent a structured AI
asset dump that pairs with `niagara_inspect`, and make UMG slot
authoring reflective without us spelling out every UPanelSlot
subclass. `gas_edit` reads UGameplayAbility / UGameplayEffect /
UAttributeSet assets so a caller can answer "what tags drive what
ability" alongside `tag_registry_edit`.

## Shipped in this fork

- `editor_actions` (small) — save / undo / redo / focus selection / play / stop play.
- `window_capture` (small) — synchronous PNG screenshot of the active viewport.
- `asset_factory` (small) — create DataTable / Enum / Struct / DataAsset /
  Enhanced Input Bundle assets. The Enum variant takes a list of entry
  names; the Struct variant takes a list of `{name, type}` field specs
  covering the standard scalar and small-struct types plus `/Game/`-
  rooted UScriptStruct paths; the DataAsset variant accepts a target
  UDataAsset class and an optional flat property dict applied through
  `FProperty::ImportText_InContainer`; the enhanced_input_bundle variant
  takes a list of action specs (name + value_type) plus a list of
  mapping rows (action + key + optional negate / swizzle) and produces
  one UInputMappingContext plus N UInputAction assets in a single call.
  Existing assets at the target paths are reused unless `overwrite`
  is set.
- `widget_edit` (small) — create a UWidgetBlueprint (`create_widget_blueprint`)
  and add a typed child widget (`add_child_widget`, e.g. vertical_box,
  progress_bar, text_block, button, image) under a parent panel by FName.
- `widget_inspect` (small) — read-only counterpart to `widget_edit`. Walks
  the UWidgetTree, returns the nested hierarchy, a flat widget list, any
  named slots, and the asset's user-declared variables (excluding entries
  that are themselves widget tree members).
- `editor_log` (small) — tail the project's on-disk log file with optional
  category and minimum-verbosity filters; write a single line through GLog
  under a `LogSproftMCP` category.
- `bp_input` (small) — Enhanced Input data asset factory. Three operations:
  create a `UInputAction` (Boolean / Axis1D / Axis2D / Axis3D), create an
  empty `UInputMappingContext`, and append one key-to-action binding row
  through `UInputMappingContext::MapKey`.
- `bp_component` (small) — add a `UActorComponent` subclass to an existing
  Blueprint's `SimpleConstructionScript`. Accepts a short class name or full
  `/Script/Module.ClassName` path, an optional `parent_component` for
  attachment under an existing scene component, and an optional flat
  property dict applied through `FProperty::ImportText` on the template.
  Compiles and saves on success.
- `scene_query` (small) — read-only multiplexed actor query for the editor
  world. Combines class (substring or exact), `name_pattern`,
  `label_pattern`, single `tag`, and an optional spherical spatial filter
  with a result limit. Returns class / name / label / location / rotation /
  scale / tags / mobility / hidden flags per actor.
- `material_edit` (small) — six operations: `create_material` (with an
  optional `Constant3Vector` base-colour driver wired into `BaseColor`),
  `create_material_instance_constant` from a parent UMaterialInterface,
  `set_instance_parameter` for scalar / vector / texture overrides on a
  UMaterialInstanceConstant, plus the expression-graph trio
  `add_expression` (short-name resolver against the most-used
  UMaterialExpression* subclasses, optional `properties` dict applied
  through `FProperty::ImportText`, optional one-shot connection to a
  material attribute or another expression input),
  `connect_expressions` (source expression output -> destination
  expression input or material attribute through
  `UMaterialEditingLibrary::ConnectMaterialExpressions` /
  `ConnectMaterialProperty`), and `set_expression_property` (flat
  property dict applied to a named expression). Each expression-graph
  op recompiles + saves on success unless `recompile=false` or
  `save=false` is passed. Material Functions and Material Parameter
  Collections remain on the backlog.
- `actor_inspect` (small) — read-only counterpart to `scene_query` for a
  single actor. Resolves the actor by `GetName()` first and then by
  Outliner label, returns transform / tags / replication snapshot / root
  component, and (when asked) the full component tree with each
  component's class, relative transform, attach parent / socket, tags, and
  a short `FProperty::ExportText` value dump per component or per actor.
- `scene_compose` (small) — declarative single-actor scene mutation.
  Three operations on one actor per call: `spawn` (class path + optional
  transform / preferred FName / Outliner label / tags / flat property
  dict), `modify` (partial transform / label / tags / property patch on
  an actor resolved by name or label), and `delete`. Property dicts apply
  through `FProperty::ImportText` on the actor instance.
- `python_execution` (small) — run Python in the editor's interpreter
  through `IPythonScriptPlugin::ExecPythonCommandEx`. Two operations:
  `execute_string` (a string of source, multi-statement by default) and
  `execute_file` (a `.py` path on disk with optional positional args).
  Returns stdout / stderr / command_result and the structured log array.
  Requires `PythonScriptPlugin`; the uplugin manifest references it so
  consumer projects auto-enable it.
- `scene_brief` (small) — read-only one-shot orientation summary of the
  active editor world: persistent level name + path, attached streaming
  sublevels, total actor count + per-class counts, world bounds union,
  GameMode override + default pawn class, level-blueprint has-user-events
  flag, deduplicated tags in use, and a short list of notable landmark
  actors (player starts, directional lights, post-process volumes).
- `level_inspect` (small) — read-only structured per-actor record list for
  the editor world plus any loaded sublevels. Sits between `scene_brief`
  and `scene_query`: always returns a uniformly-shaped per-actor block
  plus a per-level summary, with optional `class` / `name_pattern` /
  `label_pattern` / `tag` / `level_filter` filters and an optional
  `include_components` toggle for a compact per-component list.
- `bp_input` (graph wiring extension) — `add_action_event_node` operation
  on the existing `bp_input` tool. Spawns a `UK2Node_EnhancedInputAction`
  in a target Blueprint's event graph for a given `UInputAction` asset,
  reusing an existing node for the same action. Optionally MakeLinkTo's
  the chosen trigger exec pin (default "Triggered") to a named function
  call on the same Blueprint through a `UK2Node_CallFunction` follow-on.
- `tag_registry_edit` (small) — manage Gameplay Tags through the editor
  module. Three operations: `add_tag` writes a tag (with optional dev
  comment) into a chosen `Config/Default*Tags.ini` source through
  `IGameplayTagsEditorModule::AddNewGameplayTagToINI`; `remove_tag`
  deletes a tag through `IGameplayTagsEditorModule::DeleteTagFromINI`;
  `list_tags` is a read-only substring search over
  `UGameplayTagsManager::RequestAllGameplayTags` returning each tag's
  owning source name, source ini path, and dev comment. The editor
  module handles ini rewrites, tag-tree refresh, and the broadcast that
  live tag pickers listen on.
- `bp_create` (small) — create a UBlueprint asset with a chosen parent
  class through `FKismetEditorUtilities::CreateBlueprint`. Resolves the
  parent class from a short name (Actor, Pawn, Character, ActorComponent,
  SceneComponent, GameMode, GameModeBase, PlayerController, AIController,
  UserWidget, DataAsset, BlueprintFunctionLibrary, etc.), a full
  `/Script/Module.ClassName` path, or a `/Game/...` Blueprint class
  path. Output package path is configurable under `/Game/`. An optional
  flat property dict applies through `FProperty::ImportText` on the
  generated CDO before the first compile. Compiles and saves on success.
- `bp_brief` (small) — read-only one-page orientation summary of a
  Blueprint asset. Returns name, path, parent class (short + full path),
  blueprint type, variable count, function count, macro count,
  event-graph node count, named-event list (UK2Node_Event +
  UK2Node_CustomEvent), SCS component summary, implemented Blueprint
  interfaces, and a data-only flag.
- `bp_inspect` (small) — read-only targeted query operations on a
  Blueprint asset, keyed by `op`. `list_variables`, `list_functions`,
  `list_events`, `list_components`, and `find_node` (substring against
  node short class name and / or node title across all graphs).
- `bp_variable` (small) — declarative Blueprint variable management.
  One multi-op tool covering `list` / `add` / `remove` / `set_default` /
  `set_flags` against `UBlueprint::NewVariables`. The `add` path uses a
  wider type resolver than the existing local helper: scalar tokens,
  built-in structs (vector, rotator, transform, color, linear_color),
  `/Script/Module.ClassName` object refs, `/Game/...` Blueprint class
  refs (auto-suffixed with `_C`), and `struct:/...` UScriptStruct
  paths. Container types cover `single` / `array` / `set` / `map` (with
  a `value_type` for the map case). Each mutating op compiles + saves
  on success unless `compile=false` or `save=false`.
- `bp_class` (small) — manage class-level settings on an existing
  UBlueprint. One multi-op tool covering `read` / `set_parent` /
  `set_class_settings` / `add_interface` / `remove_interface`. The
  reparent path mirrors the editor's flow: assigns the new ParentClass,
  runs `RefreshAllNodes` and `MarkBlueprintAsStructurallyModified`,
  then recompiles. The interface ops use the `FTopLevelAssetPath`
  overloads of `ImplementNewInterface` / `RemoveInterface` and accept
  short names, full `/Script/Module.IName` paths, or `/Game/...`
  Blueprint Interface paths. `set_class_settings` writes the
  BlueprintOptions property surface (description, display name,
  namespace, category, hide categories) under `WITH_EDITORONLY_DATA`.
- `bp_graph` (small) — read-only graph traversal beyond `bp_inspect`.
  One multi-op tool covering `list_graphs` / `list_nodes` / `get_node`
  / `list_connections`. `list_graphs` walks `UbergraphPages`,
  `FunctionGraphs`, `MacroGraphs`, and each
  `ImplementedInterfaces[*].Graphs`. `list_nodes` accepts substring
  filters on node class and title and caps at 256 by default.
  `get_node` returns full pin info (direction, type, default value /
  object, exec / data flag) plus each pin's connected targets.
  `list_connections` returns a flat edge list with exec / data filters
  and a 1024-edge cap.
- `bp_nodes` (small) — batched K2 node creation in a chosen graph.
  Defaults to the first event graph; pass `graph` to target a
  function / macro / interface graph. Supports the most-used K2 node
  classes (variable_get, variable_set, call_function, branch /
  if_then_else, dynamic_cast, self, format_text, execution_sequence,
  knot, make_array, custom_event, event), optional FName + position +
  pin defaults per entry. Compile is NOT automatic so a caller can
  stitch wires through `bp_wire` first and run a single
  `compile_blueprint` at the end of the batch.
- `bp_wire` (small) — declarative connect / disconnect of named pins
  between named nodes in a Blueprint graph. Validates pin direction
  and runs `UEdGraphSchema_K2::CanCreateConnection` for type checks.
  Per-entry `disconnect=true` breaks an existing wire instead of
  making a new one; `op="disconnect"` is the per-call shortcut.
- `material_inspect` (small) — read-only material / material instance
  dump. For UMaterial: expression list (each with FName, class,
  position, parameter name), parameter set (scalar / vector / texture
  / static_switch), per-attribute connected output expression for
  BaseColor / Metallic / Specular / Roughness / Anisotropy / Normal /
  Tangent / EmissiveColor / Opacity / OpacityMask /
  WorldPositionOffset / AmbientOcclusion / Refraction / Displacement,
  and the used-texture list. For UMaterialInstance: parent material
  path, the parent's parameter list, and the instance's scalar /
  vector / texture overrides.
- `search_assets` (small) — Content-Browser-style asset search backed
  by `IAssetRegistry::GetAssets(FARFilter, ...)`. Filters: class
  (single token, list, or substring pattern, plus optional
  `include_subclasses`), path (single prefix or list, recursive by
  default), name pattern, and package-tag pairs mapped onto
  `FARFilter::TagsAndValues`. Returns each row's path, name, class,
  class_path, package, and package_path. Optional `include_disk_size`
  pulls the package's on-disk byte size through
  `IAssetRegistry::TryGetAssetPackageData`. The response carries
  `count`, `matched_total`, and a `limit_hit` flag so a caller can
  paginate by tightening the filter.
- `asset_references` (small) — read-only dependency graph for one
  asset through `IAssetRegistry::GetReferencers` /
  `GetDependencies`. `direction` selects one of `hard_referencers`,
  `soft_referencers`, `hard_dependencies`, `soft_dependencies`, or
  the `all_*` variants. Walks transitively up to `depth` (default 1,
  cap 6). Each row carries name, path, class, class_path, package,
  and package_path. Optional `class_filter` drops rows whose asset
  class does not match. Returns `count`, `matched_total`,
  `limit_hit`, and `depth_reached`.
- `bp_commit` (small) — convenience wrapper that runs the standard
  end-of-edit Blueprint cycle in one call:
  `MarkBlueprintAsStructurallyModified` (or the lighter
  `MarkBlueprintAsModified` when `mark_structurally=false`) plus
  `CompileBlueprint` with a captured `FCompilerResultsLog` plus
  `SaveAsset`. Surfaces compiler errors / warnings / infos as separate
  string arrays. Skips save when the compile produced errors so a
  broken Blueprint does not get pinned to disk; `force_save=true`
  overrides for diagnostic snapshots. Becomes the canonical end-of-edit
  step for designers chaining `bp_nodes` -> `bp_wire` -> `bp_commit`.
- `bp_function_create` (small) — declarative one-call wrapper for
  laying down a new Blueprint function with its full typed signature.
  Wraps `FBlueprintEditorUtils::CreateNewGraph` +
  `AddFunctionGraph<UClass>` plus the FunctionEntry / FunctionResult
  pin authoring. Inputs accept the same wide type resolver as
  `bp_variable` (scalar tokens, built-in structs, `/Script/...`,
  `/Game/...` BP class refs auto-suffixed with `_C`, `struct:/...`
  UScriptStruct paths) plus per-entry `is_array` / `is_reference`.
  Optional `pure` flag, `category`, `keywords`, `tooltip`, and
  `call_in_editor` toggles land directly on the entry node's
  `FKismetUserDeclaredFunctionMetadata`. Compiles and saves on
  success unless `compile=false` / `save=false`.
- `material_edit` (bulk `add_expressions` op) — extends the
  expression-graph trio with a single-call form that takes a list of
  expression specs (each with `class`, optional `name` alias,
  `position`, `properties` dict) and an optional list of edge specs
  (each `{source, source_output?, dest, dest_input?}` between
  expressions or `{source, property}` to a material attribute). The
  per-spec `name` alias lets a downstream connection reference an
  expression created earlier in the same call without waiting for
  the engine's resolved FName. Recompiles + saves once after the
  whole batch unless overridden. Cuts the round-trip count for typical
  panner-driven UV chain or normal-map setup workflows.
- `niagara_inspect` (small, read-only) — structured dump of a
  UNiagaraSystem asset. Returns the system-level spawn / update
  script paths and an `emitters` array. Each emitter dict reports
  `name`, `enabled`, `sim_target` (cpu / gpu), `local_space`,
  `determinism`, the per-stage script list grouped by execution stage
  (emitter spawn / emitter update / particle spawn / particle update
  / particle gpu compute), the event-handler chain, the
  simulation-stage class list, and the renderer class list. The
  `parameters` array enumerates every entry in
  `UNiagaraSystem::GetExposedParameters()` with name + type + kind
  (primitive / data_interface / object). Edit-side ops
  (niagara_edit / niagara_script_edit) remain on the backlog. Adds
  Niagara to UnrealMCP's PublicDependencyModuleNames and the
  uplugin manifest.
- `bp_export` (small, read-only) — canonical Blueprint snapshot.
  Returns name + path + parent class + blueprint type, the
  variables array (with type, default, friendly name, category,
  flag set), the SCS components array (with relative transform +
  optional `FProperty::ExportText` defaults dump), the implemented-
  interfaces array, and a `graphs` array covering every event /
  function / macro / interface graph. Each graph carries name +
  kind + node count + a node list (class, title, position, GUID,
  optional event / custom-event signature, capped pin list with
  default value / default object / link count) plus a flat edge
  list (source / target node + pin name + is_exec). Per-node pin
  output is capped at `max_pins_per_node` (default 64) and tagged
  `pins_truncated` when the cap fires. Sits next to the read-only
  `bp_brief` / `bp_inspect` / `bp_graph` triad and is the primary
  diff-able payload for verifying that an MCP-driven authoring
  session left a Blueprint in the expected state.
- `behavior_tree` (small) — multi-op tool keyed by `op`. The
  `inspect` op (default) is the read-only structured dump shipped
  earlier: full tree (composite root + recursive children +
  per-child decorator chain + per-composite service chain), the
  tree-level RootDecorators, plus the linked Blackboard (path,
  parent path, key list with name + type token + inner BaseClass /
  EnumType / Struct path for typed Object / Class / Enum / Struct
  keys, instance-sync flag, parent-inherited flag). SimpleParallel
  composites also report their FinishMode (immediate / delayed).
  The recursive walk caps at `max_depth` (default 32) and flips
  `children_truncated` on the offending composite.
  The earlier edit ops are `create_behavior_tree` (NewObject's a
  UBehaviorTree at a `/Game/...` path with an optional linked
  UBlackboardData) and `add_root_composite` (NewObject's a
  Selector / Sequence / SimpleParallel composite under the tree
  and assigns it as `RootNode`). The next set of edit ops are
  `add_child_task` (appends a UBTNode child slot under a target
  composite via `UBTCompositeNode::Children.AddDefaulted_GetRef()`
  + `ChildComposite` / `ChildTask` assignment with
  `InitializeFromAsset` wiring; resolves the parent composite by
  `GetNodeName()` substring or the `root` sentinel; the new node
  outers under the tree asset so it travels with the package on
  save), `add_decorator` (appends a UBTDecorator to a child slot's
  Decorators array; targets the child by `GetNodeName()`
  substring), and `add_service` (appends a UBTService to a
  composite's Services array). Each new node accepts an optional
  flat property dict applied through
  `FProperty::ImportText_InContainer`; failed entries surface under
  `skipped`. Short-name resolver covers the stock task / decorator
  / service set (`wait` / `move_to` / `blackboard` / `cooldown` /
  `loop` / `time_limit` / `default_focus` etc.) plus full
  `/Script/Module.ClassName` paths and `/Game/...` BP class paths.
  The newest edit ops cover the Blackboard side of the BT/BB pair:
  `set_blackboard` rebinds the BT's BlackboardAsset slot (pass
  `clear=true` to unbind); `add_blackboard_key` appends a typed
  FBlackboardEntry to a target Blackboard's `Keys` array (short
  token resolver covers `bool` / `int` / `float` / `string` /
  `name` / `vector` / `rotator` / `object` / `class` / `enum` /
  `struct` plus `/Script/AIModule.UBlackboardKeyType_*` paths and
  short class names; wires `BaseClass` on Object / Class keys,
  `EnumType` on Enum keys, and `DefaultValue.InitializeAs(Struct)`
  on Struct keys when the caller passes the matching inner-type
  hint; the `instance_synced` flag, editor-only `description`, and
  `category` land on the new entry; `UpdateIfHasSynchronizedKeys`
  + `UpdateKeyIDs` + `PropagateKeyChangesToDerivedBlackboardAssets`
  refresh the per-asset cache and any derived Blackboards after
  the write); `remove_blackboard_key` removes an own key by FName
  with the same post-write fix-up. The Blackboard target resolves
  either through an explicit `blackboard` arg or, when only `tree`
  is set, through the BT's BlackboardAsset slot. All edit ops save
  by default. The runtime exec / memory indices stay null on
  append; the BT graph editor's RebuildExecutionOrder populates
  them when the asset is re-opened or re-compiled. Adds AIModule
  to PublicDependencyModuleNames. The deepening passes since this
  cut landed `add_blackboard_decorator`, `set_root_decorator`,
  and `rename_blackboard_key`. Remaining follow-ons: Blackboard
  key type-change op, and parent-Blackboard re-bind.
- `widget_edit` slot-property surface — a third op `set_slot_property`
  on the existing `widget_edit` tool. Takes a target widget FName plus
  a flat property dict and applies the dict to the widget's UPanelSlot
  through `FProperty::ImportText_InContainer`. Covers UCanvasPanelSlot
  anchors / offsets / size / ZOrder, UVerticalBoxSlot /
  UHorizontalBoxSlot padding / fill / alignment, UOverlaySlot /
  UGridSlot, and any other UPanelSlot-derived class without us
  spelling out each property by name. Each entry that fails to
  resolve as a UPROPERTY or refuses ImportText is reported under
  `skipped` with a reason. After applying the dict the slot's
  `SynchronizeProperties()` runs so a re-layout tick picks the change
  up.
- `landscape_inspect` (small, read-only) — structured dump of every
  `ALandscape` actor in the editor world. Per-actor record covers
  name + label + transform + owning level + landscape GUID, the
  section-grid tuple (ComponentSizeQuads / SubsectionSizeQuads /
  NumSubsections / component_count), the proxy material driver and
  any hole-material override, world-space proxy bounds (min / max /
  size), the editor-only XY component-space extent rectangle when
  ULandscapeInfo is registered, the registered layer list (each
  entry with `layer_name`, `layer_info_object_path`,
  `phys_material`, `blend_method` enum byte, `is_no_blend`,
  `is_visibility_layer`), heightmap / weightmap texture
  deduplication counts plus opt-in package-path arrays, and an
  optional per-component records array (each with `name`,
  `section_base`, `weightmap_count`, `weightmap_layer_allocation_count`,
  `heightmap_path`). Filters: `name_pattern` (substring on actor
  name + label) and `level_filter` (substring on owning ULevel
  name). Adds `Landscape` to PublicDependencyModuleNames. Pairs
  with `foliage_inspect` for terrain reasoning. Edit-side ops
  (sculpting, paint layers, heightmap import / export) remain on
  the backlog.
- `foliage_inspect` (small, read-only) — structured dump of every
  `AInstancedFoliageActor` in the editor world. Walks the public
  `AInstancedFoliageActor::GetFoliageInfos` accessor and emits a
  per-IFA actor block (name, label, transform, level, foliage type
  count, total instance count) plus a per-type `foliage_types`
  array. Each foliage_type carries the type asset path, source-mesh
  or actor-class path with a `source_kind`
  (`static_mesh` / `actor` / `unknown`) discriminator, density,
  density adjustment factor, radius, per-axis scale interval
  (ScaleX / ScaleY / ScaleZ min and max), instance counts (placed
  and total), and the editor-only approximated-bounds box of all
  its instances. Optional `sample_locations` draws a deterministic
  seeded reservoir of N world-space instance locations per type
  (capped at 1024) so the agent can probe density without us
  shipping the full instance dump. Filters: `name_pattern` and
  `level_filter`. Adds `Foliage` to PublicDependencyModuleNames.
  Edit-side ops (paint, scatter, remove instances) remain on the
  backlog.
- `cpp_source` (small, read-only) — read C++ source by class path
  or by full file path on disk. Three input modes (class +
  header_path + source_path), all routed through
  `FSourceCodeNavigation::FindClassHeaderPath` /
  `FindClassSourcePath` for the class-driven path or extension-swap
  inference for the file-driven paths. Returns header / cpp text +
  byte size + truncation flag per file plus per-file existence
  flags so a caller can tell "header-only class" from
  "Blueprint-defined class with no C++ at all". Pairs with
  `bp_brief` for the "what does this Blueprint's parent C++ class
  look like" question. The `module` + `module_dir` metadata
  through `FindClassModuleName` + `FindModulePath` lets a caller
  jump straight to the .Build.cs without a second tool call.
- `pie_test_scene` (small) — scene-state assertion harness. Runs
  against the editor world without driving Play in Editor. Four
  assertion kinds:
  - `actor_exists`: `target` is an actor name (matched against
    `GetName()` first and Outliner label second). Pass = an
    actor with that name or label is present in the current
    editor world.
  - `actor_at_location`: `target` is an actor name, `expected`
    is a `[x, y, z]` world-space location, and `tolerance`
    (default 1.0 cm) is the pass radius. Pass = the resolved
    actor's `GetActorLocation` is within `tolerance` of
    `expected`.
  - `actor_overlapping_tag`: `target` is an actor name,
    `expected` is an FName tag string. Pass = the resolved
    actor's `Tags` array contains that FName. Despite the
    historical "PIE-only" framing, `AActor::Tags` is populated
    in the editor world too, so the kind answers statically
    against the loaded level.
  - `var_equals`: `target` is an actor name, `expected` is a
    `{var, value}` dict. Pass = the resolved actor's UPROPERTY
    (looked up by FName) ImportText-matches the canonicalized
    representation of `value`. The expected JSON literal is
    routed through the property's `ImportText` into a transient
    buffer, then re-emitted through `ExportText` so the
    comparison runs against the engine's own canonical form
    (so `{"X":1,"Y":2,"Z":3}` matches `(X=1.000000,Y=2.000000,Z=3.000000)`
    on an FVector property). Works against transform fields,
    Blueprint-exposed variables, gameplay tags, FString fields,
    and any other reflected actor property in the editor world.
  Per-assertion the response carries `index`, `kind`, `target`,
  `passed` flag, optional `actual` / `expected` / `delta` /
  `tolerance` / `var` / `property_class` / `expected_imported`
  for the relevant kinds, and a human-readable `message`.
  Aggregate counts (`total`, `passed`, `failed`, `unsupported`,
  `all_passed`) sit at the top of the response. Open follow-ons:
  per-assertion timeout for kinds that need a running PIE world,
  and an over-PIE harness option for callers who want the same
  surface but inside a running PIE world.
- `animation_edit` (small) — multi-op tool for targeted
  UAnimSequence / UAnimMontage edits, keyed by `op`:
  - `set_rate_scale`: writes `RateScale` (float) on
    UAnimSequenceBase. Works on UAnimSequence and UAnimMontage.
  - `set_additive`: toggles the additive shape on UAnimSequence.
    Writes `AdditiveAnimType` (`none` / `local_space` /
    `rotation_offset_mesh_space`) plus optional `RefPoseType`
    (`none` / `ref_pose` / `anim_scaled` / `anim_frame`),
    `RefPoseSeq` (a `/Game/...` UAnimSequence path applied when
    ref_pose_type is anim_scaled / anim_frame), and
    `RefFrameIndex` (integer frame index applied when
    ref_pose_type is anim_frame).
  - `add_notify`: appends an FAnimNotifyEvent to a notify track
    on UAnimSequenceBase. Resolves the optional `notify_class`
    to either UAnimNotify or UAnimNotifyState (or treats the
    entry as a custom-event notify when no class is given).
    Auto-creates the named notify track through
    `UAnimationBlueprintLibrary::AddAnimationNotifyTrack` when
    missing, then routes through `AddAnimationNotifyEvent` /
    `AddAnimationNotifyStateEvent`. `frame` (integer) wins over
    `time` (float seconds) when both are present; the
    frame-to-seconds conversion uses the asset's
    `GetSamplingFrameRate`. `duration` is required for
    UAnimNotifyState subclasses.
  Each op saves the asset by default. Adds
  AnimationBlueprintLibrary to PrivateDependencyModuleNames.
  Pairs with `animation_inspect`. Heavier branches (curves, key
  frames, sync markers, anim composite section authoring,
  AnimBP state-machine edits, IK rig / retargeting) remain on
  this list.
- `foliage_edit` (small) — multi-op tool for foliage authoring,
  keyed by `op`:
  - `add_foliage_type`: registers a UFoliageType asset on the
    `AInstancedFoliageActor` for a chosen level. Resolves (or
    spawns) the IFA through
    `AInstancedFoliageActor::GetInstancedFoliageActorForLevel(Level, /*bCreateIfNone=*/true)`
    and binds the type through
    `AInstancedFoliageActor::AddFoliageType`. Reuses an existing
    FFoliageInfo when the type is already registered. Optional
    `level` is a substring on the owning ULevel name; defaults
    to the persistent level. `created_actor` /
    `type_already_bound` flags surface in the response.
  - `set_foliage_density`: writes `Density`,
    `DensityAdjustmentFactor`, `Radius`, and the per-axis
    `ScaleX` / `ScaleY` / `ScaleZ` FFloatInterval pairs on a
    UFoliageType asset. All fields are optional; only the ones
    present in the call are written. Each axis interval requires
    both `_min` and `_max` to be present together.
  Pairs with `foliage_inspect`. The "place N instances at
  locations" op stays on this list.
- `behavior_tree` edit slice (small) — adds two edit ops to the
  existing read-only inspector through a new `op` discriminator
  (default stays `inspect`):
  - `create_behavior_tree`: NewObject's a UBehaviorTree at a
    `/Game/...` package path with an optional `/Game/...`
    UBlackboardData linked through `BlackboardAsset`.
    `overwrite=true` is the standard escape hatch for an existing
    asset at the path.
  - `add_root_composite`: NewObject's a Selector / Sequence /
    SimpleParallel composite (case-insensitive token, with a
    UClass-path escape hatch for any UBTCompositeNode subclass)
    under the tree as outer and assigns it to `RootNode`. We
    refuse to overwrite an existing RootNode unless `replace=true`.
  Both ops save by default. Together the two ops produce a usable
  empty tree in two declarative calls. Open follow-ons: append
  child task / composite to a chosen parent, insert a decorator
  on a chosen child slot, append a service on a composite, and
  the full Blackboard key edit surface (add / remove / rename /
  type change / sync flag toggle).
- `sequencer_edit` edit slice (small) — adds two edit ops to the
  existing read-only inspector through the same `op` discriminator
  (default stays `inspect`):
  - `create_level_sequence`: NewObject's a ULevelSequence at a
    `/Game/...` package path and runs `ULevelSequence::Initialize`
    so the new asset has a fresh UMovieScene with the project's
    default tick / display rates and clock source. Without
    Initialize the sequence opens but every Sequencer panel call
    hits a null MovieScene path, so we run it unconditionally
    before AssetCreated and the optional save.
  - `add_possessable`: resolves a target sequence by short name or
    `/Game/...` path and a target actor by `GetName()` (first) /
    Outliner label (second), then runs
    `UMovieScene::AddPossessable` + `UMovieSceneSequence::BindPossessableObject`
    so Sequencer's runtime can map the binding GUID back to the
    editor-world actor. `binding_name` defaults to the actor's
    `GetActorLabel()` so the Sequencer Outliner shows a
    designer-readable row name.
  Both ops save by default. Open follow-ons: track add (a chosen
  UMovieSceneTrack subclass), section add (with an explicit frame
  range), section move, spawnable creation, and camera-cut track
  creation.
- `animation_inspect` (small, read-only) — structured dump for
  animation assets. Resolves the asset by short name or
  `/Game/...` path and branches by class:
  - USkeletalMesh: skeleton path + LOD count + bone list (each
    `{name, parent_index, parent_name}`) + socket list (mesh-level
    sockets first, then non-overridden skeleton-level sockets,
    each with `relative_location` / `relative_rotation` /
    `relative_scale` plus a `source` discriminator).
  - UAnimSequence: play length + rate scale + sampling frame rate
    (numerator / denominator / approx_fps) + sampled key count +
    additive anim type token + notify list. Each notify carries
    name + absolute time + duration + track index + trigger
    chance + linked UAnimNotify / UAnimNotifyState class path
    plus a `notify_kind` discriminator (`instant` / `state` /
    `event`).
  - UAnimMontage: play length + rate scale + composite sections
    (name + time + next-section) + slot tracks (slot_name +
    animation count) + notifies through the same shape.
  - UBlendSpace (and UBlendSpace1D): per-axis FBlendParameter
    (display_name + min + max + grid_num + snap_to_grid +
    wrap_input) plus axis_count and sample count. Reads through
    the public `UBlendSpace::GetBlendParameter(int32)` so we
    avoid the fixed-size FBlendParameter[3] reflection dance.
  - UAnimBlueprint: parent class + target skeleton path +
    template flag + variable count + state-machine list (each
    machine's name + state count + transition count + initial
    state index + initial state name). The state-machine list
    reads off the cached UAnimBlueprintGeneratedClass through
    the `BakedStateMachines` array so we do not need the
    editor-only AnimGraph module.
  Pairs with `bp_brief` for AnimBP-specific orientation. Edit-side
  ops (sequence / montage authoring, AnimBP state-machine
  authoring, IK rig / retargeting) remain on the backlog.
- `project_context` (small, read-only) — one-shot designer summary
  of the loaded project. Returns project name + uproject path +
  project dir + content dir, the .uproject metadata (description,
  category, EngineAssociation, enterprise flag), the full engine
  version strings plus per-component major / minor / patch /
  changelist / branch / licensee flag, the current editor level
  (name + path), the per-level GameMode override + default pawn
  through `AWorldSettings::DefaultGameMode`, the project-wide
  GameMapsSettings surface (`default_game_mode_class_project`,
  `default_game_map`, `transition_map`, `editor_startup_map`,
  `game_instance_class`), an `enabled_plugins` array filtered by
  default to project / external / mod / enterprise plugins (each
  entry with name + friendly_name + type + location + version +
  version_name + category + description + created_by +
  engine_version + can_contain_content + is_beta + is_experimental
  + base_dir; engine plugins opt-in through
  `include_engine_plugins=true`), a `source_modules` array from
  the .uproject (name + type + loading_phase), and a
  `content_roots` array of every immediate `/Game/*` subfolder
  with a recursive asset count through `IAssetRegistry::GetAssets`.
  Pairs with `scene_brief` for the orientation pass: one tells
  the agent what project it is in, the other tells it what level
  it is in. Adds EngineSettings to PublicDependencyModuleNames.
- `sequencer_edit` (small) — multi-op tool keyed by `op`. The
  `inspect` op (default) is the read-only structured dump shipped
  earlier: resolves a target `ULevelSequence` (or any
  UMovieSceneSequence subclass) and returns asset name + path +
  class plus the linked UMovieScene's tick / display frame rates
  (each as `{numerator, denominator, approx_fps}`), the playback
  range as a start / end / duration triple with
  `playback_has_start` / `playback_has_end` flags, the master
  tracks array (each track with name + editor-only display_name +
  class + class_path + section_count and an optional sections
  array reporting `class` + `class_path` +
  `inclusive_start_frame` / `exclusive_end_frame` /
  `duration_frames` plus the `has_start_frame` / `has_end_frame`
  bound flags), the optional camera-cut track stub when present,
  the possessables array (binding GUID + name + possessed-class +
  parent_guid), and the spawnables array (binding GUID + name +
  spawn-template class). Per-track section emission caps at
  `max_sections_per_track` (default 64), with `sections_truncated`
  set on the offending track when the cap fires.
  The new edit ops are `create_level_sequence` (NewObject's a
  ULevelSequence at a `/Game/...` path and runs
  `ULevelSequence::Initialize` so the asset has a fresh UMovieScene
  with the project's default tick / display rates and clock
  source) and `add_possessable` (resolves a target sequence and a
  named editor-world actor, runs `UMovieScene::AddPossessable` +
  `UMovieSceneSequence::BindPossessableObject` so Sequencer's
  runtime can map the binding GUID back to the actor;
  `binding_name` defaults to the actor's `GetActorLabel()`). Both
  edit ops save by default. Adds `MovieScene` and `LevelSequence`
  to PublicDependencyModuleNames.
  The newer edit ops are `add_track` and `add_section`. `add_track`
  resolves a UMovieSceneTrack subclass by short token (`transform`
  / `camera_cut` / `skeletal_animation` / `audio` / `event` /
  `float` / `subscene` / `bool` / `byte`) plus
  `/Script/MovieSceneTracks.X` paths and `/Script/Module.Class`
  shapes; binding-scoped tracks attach via
  `UMovieScene::AddTrack(TrackClass, BindingGuid)` when a binding
  GUID or possessable name resolves, master tracks attach via the
  no-binding `UMovieScene::AddTrack(TrackClass)` overload (5.4+
  unified the master / no-binding path), and camera cut tracks land
  on `UMovieScene::SetCameraCutTrack` after a NewObject of the
  track class. `add_section` calls
  `UMovieSceneTrack::CreateNewSection` (so the track decides its
  native section subclass), wraps the requested `start_frame` +
  `duration_frames` into a `TRange<FFrameNumber>` with an inclusive
  start and exclusive end bound, and attaches via `AddSection`. The
  track lookup matches a track by FName / display name / class name
  substring, scoped to a binding when a binding GUID is supplied so
  a sequence with multiple bindings of the same track type stays
  addressable. Heavier edit ops (section move, spawnable creation,
  per-row edits) remain on the backlog.
- `gas_edit` (small read + edit slice) — Gameplay Ability System
  multi-op tool keyed by `op`. The default op `inspect` covers the
  three asset shapes:
  - UGameplayAbility (or a Blueprint with a UGameplayAbility CDO):
    ability tags + cancel / block / activation owned / required /
    blocked tags + source / target required / blocked tags, cost +
    cooldown gameplay-effect class paths, AbilityTriggers.
  - UGameplayEffect (or a Blueprint with a UGameplayEffect CDO):
    DurationPolicy + DurationMagnitude / MaxDurationMagnitude when
    Has-Duration, modifier list (each with attribute name + owning
    AttributeSet class + ModifierOp + literal magnitude when
    scalable), executions list with calculation classes,
    GameplayCues with tag set + level range + magnitude attribute,
    plus the cached asset / granted / blocked-ability tag
    containers through the public accessors that the GE component
    model migrated to in 5.3+. Includes stack limit + stack
    expiration policy.
  - UAttributeSet (or a Blueprint with a UAttributeSet CDO): walks
    the CDO's FProperty list filtering on
    `FGameplayAttribute::IsSupportedProperty` and dumps each
    attribute's name, CPP type, base / current default value, and
    storage mode (legacy float vs. FGameplayAttributeData).
  Pairs with `tag_registry_edit` so a caller can answer "what tags
  drive what ability" in two read-only calls. Edit-side ops (tag
  mutation, modifier add / remove, cost / cooldown rebind, attribute
  default override) remain on the backlog. Adds GameplayAbilities to
  PublicDependencyModuleNames.
  The new edit ops are `create_gameplay_ability` (NewObject's a
  UBlueprint at a `/Game/...` path with a UGameplayAbility-derived
  parent class, default `/Script/GameplayAbilities.GameplayAbility`),
  `create_gameplay_effect` (same shape with a UGameplayEffect-derived
  parent; optional `duration_policy` (`instant` / `has_duration` /
  `infinite`) plus an optional literal `duration_magnitude` write
  through the CDO before the first compile), and `set_gameplay_tags`
  (tag-container mutation on either asset shape). For UGameplayAbility
  the writes route through reflected `AbilityTags` /
  `CancelAbilitiesWithTag` / `BlockAbilitiesWithTag` /
  `ActivationOwnedTags` / `ActivationRequiredTags` /
  `ActivationBlockedTags` / `SourceRequiredTags` / `SourceBlockedTags`
  / `TargetRequiredTags` / `TargetBlockedTags` UPROPERTY fields. For
  UGameplayEffect we route through
  `FindOrAddComponent<UAssetTagsGameplayEffectComponent>` /
  `UTargetTagsGameplayEffectComponent` /
  `UBlockAbilityTagsGameplayEffectComponent` and call each component's
  `SetAndApplyAssetTagChanges` / `SetAndApplyTargetTagChanges` /
  `SetAndApplyBlockedAbilityTagChanges` mutator so the cached
  tag-container snapshot on the GE refreshes.
  The newer modifier ops are `add_modifier` (appends an
  `FGameplayModifierInfo` with attribute / modifier op /
  scalable-float magnitude; `attribute` accepts
  `<set_path>:<attr_name>` colon shape, separate `attribute_set` +
  `attribute_name` params, or a bare attribute name that sweeps
  every loaded UAttributeSet subclass for the first matching
  FProperty filtered through
  `FGameplayAttribute::IsSupportedProperty`; `modifier_op` covers
  `Add` / `Multiply` / `Override` / `Division` plus the canonical
  UE 5.x names (`add_base` / `multiply_additive` / `divide_additive`
  / `multiply_compound` / `add_final`) case-insensitive; `magnitude`
  wraps a literal float into `FScalableFloat` and the
  modifier-magnitude variant constructor),
  `remove_modifier_at` (bounds-checked `Modifiers.RemoveAt`), and
  `set_attribute_default` (writes a UAttributeSet's base value plus
  the matching current value for `FGameplayAttributeData` storage,
  branching between the legacy float path and the
  `FGameplayAttributeData` struct path through
  `CastField<FStructProperty>::Struct->IsChildOf`). Each mutating op
  recompiles + saves on success unless overridden; recompile + save
  run through the owning Blueprint when the asset is a UBlueprint.
  Heavier ops (cost / cooldown rebind, GameplayCue authoring) remain
  on the backlog.
- `performance_audit` (small, read-only) — frame-time / thread-time
  snapshot for the active editor viewport. Reads the live
  `FStatUnitData` ring (the 200-sample circular buffer that `stat
  unit` already populates) on the editor's active viewport and
  reports a per-metric `avg_ms` / `peak_ms` / `last_ms` triple over
  the last `frames` samples (default 60, capped at the engine's ring
  size). Returns blocks for `frame` / `game` (game-thread) / `render`
  (render-thread) / `rhi` (RHI thread) / `gpu` (GPU frame), plus a
  live `globals` block carrying `GAverageMS` / `GAverageFPS` and the
  cycle-converted `GGameThreadTime` / `GRenderThreadTime` /
  `GRHIThreadTime` plus the GPU frame cycles through
  `RHIGetGPUFrameCycles(0)`. Optional `metrics` filter list trims
  the report to a subset, and `include_samples=true` opts into the
  raw per-frame ring dump. Skips the deep-dive captures (`stat
  startfile` / `stat stopfile`, Insights traces, FPSChart). Adds
  RenderCore + RHI to PublicDependencyModuleNames.
- `pie_test_bp` (small) — Blueprint-side assertion harness. Sits
  next to `pie_test_scene` (which targets actors in the active editor
  world) and lets a caller verify properties on a Blueprint asset's
  CDO without a running PIE session. Currently supports one
  assertion kind: `default_value_equals` (target = a UPROPERTY FName
  on the Blueprint's generated class; expected = a JSON literal that
  is canonicalised through the property's `ImportText` ->
  `ExportText` round-trip and compared against the CDO's
  `ExportText` output). Per-assertion the response carries `index`,
  `kind`, `target`, `passed` flag, `var` / `actual` / `expected` /
  `expected_raw` / `property_class` / `expected_imported`, plus a
  human-readable `message`. Aggregate counts (`total` / `passed` /
  `failed` / `unsupported` / `all_passed`) sit at the top. The
  kinds that need a running PIE session (`function_returns`,
  `event_fired`) stay on the backlog.
- `metasound_edit` (small) — MetaSound asset authoring. Two ops
  keyed by `op`: `create_metasound_source` (NewObject's a
  `UMetaSoundSource` at a `/Game/...` path; optional `output_format`
  token (`mono` / `stereo` / `quad` / `5_1` / `7_1`, default stereo)
  plus optional `sample_rate` / `block_rate` overrides land on the
  asset's OutputFormat / SampleRateOverride / BlockRateOverride
  before InitAsset wires the document) and `create_metasound_patch`
  (NewObject's a `UMetaSoundPatch` at a `/Game/...` path; reusable
  graph asset, no audio output). Both ops route through
  `UMetaSoundEditorSubsystem::GetChecked()`'s public `InitAsset` +
  `RegisterGraphWithFrontend` so the new asset has a fresh document
  plus an editor graph that opens cleanly in the MetaSound editor.
  Adds MetasoundEngine to PublicDependencyModuleNames,
  MetasoundEditor to the editor-only PrivateDependencyModuleNames,
  and Metasound to the uplugin's plugin list so consumer projects
  auto-enable it.
  The newer graph-authoring ops are `add_node` and `connect_nodes`.
  The builder is obtained per-asset through
  `UMetaSoundBuilderSubsystem::AttachBuilderToAssetChecked(Asset)`
  on the asset's `IMetaSoundDocumentInterface`; the subsystem
  attaches a `UMetaSoundBuilderBase` to the document so subsequent
  graph edits land on the asset and the editor graph picks them up
  the next time it opens. `add_node` parses a
  `Namespace.Name[.Variant]` token through
  `FMetasoundFrontendClassName::Parse` (with a bare-name fallback
  for empty namespace) and routes through
  `AddNodeByClassName(ClassName, Result, MajorVersion=1)`; returns
  the new node handle's GUID so a follow-up `connect_nodes` can
  address it without a round-trip through inspect. `connect_nodes`
  parses two FGuid strings and routes through
  `ConnectNodes(SourceNode, OutputName, DestinationNode, InputName,
  Result)`. Saves on success unless save=false. Member-default
  writes and the read-only `metasound_inspect` counterpart stay on
  the backlog.
- `unreal_api` (small, read-only) — reflection-driven query of the
  live UE5 type database. Where the hosted Flop tool is documented as
  a "15 K+ API lookup", this clean-room variant trades the offline
  reference table for live `UClass` / `FProperty` / `UFunction` walks
  against whichever modules have already loaded into the editor.
  Six ops keyed by `op`:
  - `describe` (default): full surface for one class. Returns parent
    class, direct child class list (plus the recursive count),
    implemented interfaces, all UPROPERTY fields with type / flags /
    tooltip / category / inherited flag, all UFUNCTION methods with
    full parameter list (each with cpp_type / container / is_const /
    is_reference / is_out / is_return) plus return value plus flags
    / tooltip / category / `is_pure` / `is_blueprint_callable` /
    `is_blueprint_event` / `is_static` / `is_net` / `is_const`, and
    the queried class's own flag set decoded into FName tokens.
  - `find_property`: case-insensitive substring search across one
    class's property list. Same record shape as `describe`'s
    `properties` array.
  - `find_function`: case-insensitive substring search across one
    class's function list. Same record shape as `describe`'s
    `functions` array.
  - `list_classes`: substring filter across every loaded `UClass`.
    Iterates `TObjectIterator<UClass>` and emits a compact class
    row per match (`class` / `class_path` / `super_class` /
    `is_native` / `is_abstract` / `is_interface` / `is_blueprint`).
    Optional `include_subclasses_of` (a class identifier) collapses
    the walk to descendants of the given parent through
    `GetDerivedClasses(Parent, Out, /*bRecursive=*/true)`.
    `include_native` / `include_blueprint` / `include_interfaces` /
    `include_abstract` filters trim the result by class flags.
    Default `max_results=256`.
  - `find_in_subclasses`: walks every descendant of `parent_class`
    and reports each subclass that declares a property / function
    whose name matches the supplied substring. `property` /
    `function` filter the per-side search; `member` is a shorthand
    that matches either side. Walks each candidate's own
    declarations through `EFieldIteratorFlags::ExcludeSuper` so the
    result tells the caller which descendant ADDS a member rather
    than which inherits it. `include_parent=true` opts the parent
    class itself in.
  - `class_diff`: compares two classes' own (or inherited when
    `include_inherited=true`) property + function surfaces.
    Returns `properties.added` / `properties.removed` /
    `properties.changed` plus the parallel function trio.
    `properties.changed` rows surface when the canonical signature
    (cpp_type plus container inner / map key / map value) differs;
    `functions.changed` rows surface when the parameter list,
    return type, or the relevant flag set (BlueprintPure /
    BlueprintCallable / BlueprintEvent / Static / NetMulticast /
    NetServer / NetClient / Const) differs.
  The class identifier accepts `/Script/Module.ClassName`, a
  `/Game/...` Blueprint class path (auto-suffixed with `_C`), or a
  short class name (probed against the loaded class set with A / U
  prefix variants and a `/Script/Engine.<Name>` fallback). Walks
  inherited members by default through `EFieldIteratorFlags::IncludeSuper`;
  `include_inherited=false` restricts to the class's own declarations.
  Per-list caps (`max_properties` / `max_functions` / `max_children`)
  with truncated flags and aggregate totals. Property records also
  emit `inner_type` for arrays / sets and `key_type` / `value_type`
  for maps so callers can answer "what does this TArray<FFoo> hold"
  without a second tool call. Backed by `TFieldIterator<FProperty>`
  / `TFieldIterator<UFunction>` plus `GetDerivedClasses` from
  `UObjectHash.h`. Pairs with `cpp_source` for the "what does this
  UClass actually look like" question without leaving the
  reflection database.
- `sound_asset_edit` (small) — Sound Cue authoring. Three ops keyed
  by `op`:
  - `create_sound_cue`: NewObject's a `USoundCue` at a `/Game/...`
    path. Optional `sound_wave` parameter resolves the named
    `USoundWave` and wires a single `USoundNodeWavePlayer` into the
    cue's `FirstNode` slot, mirroring the editor's "right-click sound
    wave -> Create Cue" shortcut. Without `sound_wave` the asset
    ships with `FirstNode = nullptr` and a blank graph.
  - `add_sound_node_wave_player`: resolves an existing
    `USoundCue` and a target `USoundWave`, calls
    `USoundCue::ConstructSoundNode<USoundNodeWavePlayer>`, binds the
    wave through `USoundNodeWavePlayer::SetSoundWave`, and (when
    `connect_to_root=true`, the default) writes the new node into
    the cue's `FirstNode` slot. `LinkGraphNodesFromSoundNodes`
    refreshes the editor graph so the SoundCue editor opens cleanly.
  - `set_attenuation`: writes `AttenuationSettings` (the
    `USoundAttenuation` ref on USoundBase) on a target `USoundCue`.
    `attenuation` accepts a `/Game/...` path or null / empty string
    to clear the override.
  Cue + wave + attenuation lookups accept `/Game/...` paths or short
  names (short-name resolution falls back to the asset registry's
  per-class index). The full SoundCue node-graph authoring surface
  (mixer, modulator, delay / loop / branch composites, attenuation
  node, distance crossfade, random / sequence composites) remains
  on the backlog. Backed entirely by classes from
  `Engine/Classes/Sound/`; no new module deps required.
- `ik_retarget` (small, read + edit slice) — inspect or mutate a
  `UIKRetargeter` asset. The 5.6 retargeter refactor moved chain
  mapping + root settings + global settings into a polymorphic op
  stack (`FInstancedStruct` of `FIKRetargetOpBase`-derived structs).
  The read-only inspect slice walks the asset's public surface plus
  the op stack and reports asset path / class, source IK Rig path +
  has_source_ik_rig flag, target IK Rig path + has_target_ik_rig
  flag, current source / target retarget pose names plus their
  bone-rotation-offset counts and root-offset flags, the retarget op
  stack (per-op `index`, `name`, `parent_name`, `struct_type`
  short-name + path, `enabled` flag, `initialized` flag, optional
  `chain_mapping` array of `{target_chain, source_chain}` pairs
  reaching through `FIKRetargetOpBase::GetChainMapping()`), and
  aggregate counts (`op_count` / `op_count_total` / `ops_truncated`
  / `chain_pair_count`). Filters: `include_op_chain_mappings`
  (default true), `max_ops` (default 64). The edit ops route
  through the editor-only `UIKRetargeterController::GetController(Retargeter)`
  accessor: `set_source_ik_rig` and `set_target_ik_rig` rebind the
  retargeter's source / target IK Rig through
  `UIKRetargeterController::SetIKRig(Source / Target, Rig)` (pass
  `clear=true` to unbind the side); `set_retarget_pose` switches
  the active retarget pose for either side through
  `SetCurrentRetargetPose(PoseName, Side)` and surfaces the
  controller's missing-pose guard as a clear error. Each mutating
  op runs `MarkPackageDirty` and saves the asset by default. Adds
  IKRig to PublicDependencyModuleNames and the IKRig plugin to the
  uplugin manifest; the IKRigEditor private dependency was already
  in the build.cs from the `ik_rig_edit` slice. Edit-side ops still
  on the backlog: append op / remove op (the polymorphic
  FInstancedStruct array on the asset), set chain mapping pair on a
  chosen op, override per-bone retarget pose offsets, profile
  management through `UIKRetargeter::GetProfileByName`.
- `ik_rig_edit` (small, read + edit slice) — inspect or mutate
  a `UIKRigDefinition` asset. Pairs with `ik_retarget` for the
  rig side of the retargeting pipeline. The 5.6 IK Rig refactor
  moved the solver list onto a polymorphic op stack
  (`FInstancedStruct` of `FIKRigSolverBase`-derived structs); the
  read-only inspect slice walks the asset's public surface plus
  the solver stack and reports asset path / class, preview
  skeletal mesh path (when set), retarget root bone (`Pelvis`),
  retarget chain list (each chain `chain_name` / `start_bone` /
  `end_bone` / `ik_goal_name`), IK goal list (each goal
  `goal_name` / `bone_name` / position alpha / rotation alpha /
  current + initial transforms), the solver stack (per-solver
  `index` / `struct_type` / `struct_path` / `enabled` plus
  optional `start_bone` / `end_bone` for solvers that use them,
  plus a reflection-driven `settings` dict reflected off
  `GetSolverSettings()` and a `bone_settings` array for solvers
  that use custom bone settings), and aggregate counts
  (`chain_count`, `goal_count`, `solver_count`,
  `bone_setting_count`). Filters: `include_solver_settings`
  (default true), `include_bone_settings` (default true),
  `max_chains` / `max_goals` / `max_solvers`. Edit ops route
  through the editor-only `UIKRigController::GetController(Rig)`
  accessor: `set_retarget_root` (`SetRetargetRoot(BoneName)`),
  `add_retarget_chain` (`AddRetargetChain(ChainName, StartBone,
  EndBone, OptionalGoalName)`; the duplicate-name guard returns
  `NAME_None` and surfaces as a clear error), and `add_ik_goal`
  (`AddNewGoal(GoalName, BoneName)`; same guard). Each mutating
  op runs `MarkPackageDirty` and saves the asset by default
  unless `save=false`. Adds `IKRigEditor` to the editor-only
  `PrivateDependencyModuleNames`. The remove-side ops
  (chain remove, goal remove, solver mutation, bone-settings
  writes) stay on this list.
- `chaos_edit` (small, read + edit slice) — inspect or mutate
  a `UGeometryCollection` asset. Default op `inspect` walks both
  the asset's public
  surface and the underlying `FGeometryCollection` managed-array
  data and reports asset path / class, `is_empty` /
  `has_visible_geometry` / `root_index` flags, the
  `geometry_sources` array (each `{source_path, local_transform,
  source_materials, split_components,
  set_internal_from_material_index, add_internal_materials}`),
  aggregate transform counts (`vertex_count` / `face_count` /
  `geometry_count` / `transform_count` / `cluster_count` /
  `rigid_count` / `none_sim_count`), the `max_level` + parallel
  `bone_hierarchy_depth`, the per-fracture-level histogram
  (`count_per_level` and parallel `cluster_count_per_level`), the
  `simulation` block (clustering toggle, cluster group index, max
  cluster level, `cluster_connection_type` token, `damage_model`
  token, damage threshold list, per-cluster-only damage threshold
  flag, minimum mass clamp, total mass, mass-as-density flag,
  density toggles, removal surface with scale-on-removal /
  remove-on-max-sleep / sleep + removal duration intervals, slow-
  moving-as-sleeping toggle), the `materials` array (each entry's
  path + class), the `nanite` block (enable + fallback + minimum
  residency), and `embedded_geometry_count` /
  `auto_instance_mesh_count` / `size_specific_data_count`
  aggregates. Reaches the per-fracture-level data through
  `FindAttribute<int32>(FTransformCollection::LevelAttribute,
  FTransformCollection::TransformGroup)` on the managed-array
  collection so legacy assets without level data degrade to "level
  info missing" rather than crashing. Filters:
  `include_geometry_sources` (default true),
  `include_per_level_histogram` (default true), `max_sources` /
  `max_materials`. Adds `GeometryCollectionEngine` + `Chaos` to
  PublicDependencyModuleNames. Edit ops: `set_simulation_settings`
  (writes a flat `properties` dict against the asset's reflected
  simulation surface (`Mass`, `MinimumMassClamp`, `bMassAsDensity`,
  `EnableClustering`, `MaxClusterLevel`, `DamageModel`, etc.)
  through `FProperty::ImportText_InContainer`; failed entries
  surface under `skipped` with a reason; `InvalidateCollection`
  runs after the writes so the cached simulation data rebuilds),
  and `import_static_mesh` (appends a UStaticMesh into the
  collection through the editor-only
  `FGeometryCollectionConversion::AppendStaticMesh(StaticMesh,
  Materials, Transform, Collection, ReindexMaterials)`; optional
  `transform` lays the mesh down at a chosen world-space
  transform; the source mesh's static materials inherit by
  default). Each mutating op runs `MarkPackageDirty` and saves
  the asset by default unless `save=false`. Adds
  `GeometryCollectionEditor` to the editor-only
  `PrivateDependencyModuleNames` for the conversion API. The
  fracture / authoring write side beyond mesh append, dataflow
  driver, per-instance damage threshold override, and the
  re-cluster ops stay on this list.
- `niagara_edit` (small, system + emitter authoring) — two ops
  keyed by `op`. Default op `create_niagara_system` NewObject's a
  `UNiagaraSystem` at a `/Game/...` path through
  `UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/false)`.
  A system with no emitters opens cleanly in the Niagara editor
  but surfaces a "no emitter" warning in the asset's status
  banner. New op `add_emitter_from_asset` resolves an existing
  system and an existing `UNiagaraEmitter` and routes through
  the editor-only `UNiagaraSystem::AddEmitterHandle(SourceEmitter,
  HandleName, VersionGuid)` overload. The emitter handle display
  name defaults to the source emitter's `GetName()`; the version
  GUID defaults to the source emitter's currently exposed version
  (`UNiagaraEmitter::GetExposedVersion().VersionGuid`). Saves the
  system to disk by default unless `save=false`. With the
  `add_sim_stage` + `set_emitter_sim_target` ops landing the
  authoring surface gap narrows to GPU compute-script wiring and
  the per-stage module attach beyond `add_module_to_stage`. Adds
  `NiagaraEditor` to the editor-only
  `PrivateDependencyModuleNames` for the `InitializeSystem`
  linkage; we deliberately bypass the
  `bCreateDefaultNodes=true` path so we never have to pull
  `FNiagaraStackGraphUtilities` (NiagaraEditor private) into our
  public surface.
- `skills` (small) — fetch on-demand workflow docs over a curated
  index of short markdown files baked into this fork
  (`Python/skills/*.md`). Each doc follows a four-section shape
  (when to use, the canonical UE5 path, our wrappers in this fork,
  gotchas) sized at 200 to 400 words. Three ops keyed by `op`:
  `get` (default; returns the markdown body of one skill, with
  topic-name normalisation handling `replication` /
  `enhanced_input` / `Enhanced Input` / `replication.md`
  interchangeably), `list` (returns every available topic with its
  slug + first H1), and `search` (case-insensitive substring
  search across topic slug + first H1). Pre-baked entries:
  `replication` (multiplayer state sync, RPCs, lifetime props),
  `enhanced-input` (Enhanced Input action / context / mapping),
  `gameplay-tags` (Gameplay Tag registry and runtime queries), and
  `crafting-data-tables` (DataTable + row struct patterns). The
  tool reads markdown at request time so adding a new entry is a
  file-add under `Python/skills/`, not a code change. Pairs with
  `unreal_api` for the runtime-class side and `cpp_source` for the
  in-engine-source side; the workflow docs sit one level higher
  for "what is the canonical pattern for X" questions.
- `animation_graph_edit` (small, read + edit slice) — inspect
  a `UAnimBlueprint`'s compiled state machines plus the flat
  AnimGraph node list, or spawn a new state on a chosen state
  machine. Pairs with `animation_inspect`, which already returns
  a state-machine summary keyed off
  `UAnimBlueprintGeneratedClass::BakedStateMachines`. The
  read-only inspect slice goes one level deeper: per state
  machine returns name + initial-state index + per-state details
  (FName, state-root-node index, notify indices, entry-rule node
  index, always-reset / conduit flags, per-state exit-transition
  table) and the per-machine transitions array (each row with
  `previous_state` / `next_state` / `previous_state_name` /
  `next_state_name` / `crossfade_duration` /
  `min_time_before_reentry` / `blend_mode` token (linear /
  cubic_in / hermite_cubic / sinusoidal / quadratic_in_out /
  cubic_in_out / quartic_in_out / quintic_in_out / circular_in /
  circular_out / circular_in_out / exp_in / exp_out / exp_in_out
  / custom) / `logic_type` token (standard_blend /
  inertialization / custom) / start / end / interrupt notify
  indices). The flat AnimGraph node-property list off
  `UAnimBlueprintGeneratedClass::AnimNodeProperties` emits
  `{index, struct_type, struct_path}` rows per node so a
  downstream consumer can answer "what AnimGraph nodes does this
  AnimBP have" without needing the editor-only AnimGraph module.
  Filters: `include_state_machines` / `include_states` /
  `include_transitions` / `include_anim_nodes` (default true)
  plus per-list caps (`max_state_machines` (64) /
  `max_states_per_machine` (256) /
  `max_transitions_per_machine` (1024) / `max_anim_nodes`
  (2048)). Uncompiled AnimBPs report `compiled=false` with empty
  arrays. The new edit op `add_state` resolves the target state
  machine on the AnimBP by name (walks every UEdGraph reachable
  through FunctionGraphs / UbergraphPages / MacroGraphs plus
  per-node GetSubGraphs / per-graph SubGraphs for any
  `UAnimGraphNode_StateMachineBase` whose `GetStateMachineName()`
  matches case-insensitive), reaches through
  `EditorStateMachineGraph` to the
  `UAnimationStateMachineGraph`, and spawns a new
  `UAnimStateNode` through the public schema-action template
  `FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(Graph, NewObject<UAnimStateNode>(), Location)`.
  PerformAction wires the per-state `BoundGraph` (the per-state
  AnimGraph that holds the pose subtree) via `PostPlacedNewNode`
  plus `AllocateDefaultPins`. Renames the node to the caller-
  supplied FName via
  `Rename(NewName, nullptr, REN_DontCreateRedirectors)` so
  `GetStateName()` returns the designer-readable label. Optional
  2D `position` (`{x, y}`) writes the FVector2f location through
  the schema action. The duplicate-name guard surfaces a clear
  error rather than silently spawning. Recompile via
  `FKismetEditorUtilities::CompileBlueprint` runs by default
  unless `compile=false`; saves to disk by default unless
  `save=false`. Adds `AnimGraph` to the editor-only
  `PrivateDependencyModuleNames`. Edit-side follow-ons
  (transition add / mutate, state-property writes, conduit /
  alias spawn, anim-graph node add / connect inside a state's
  BoundGraph, link a Linked Anim Graph by tag) stay on this
  list.
- `niagara_script_edit` (small, read-only first slice) — inspect
  a `UNiagaraScript` asset. Pairs with `niagara_inspect` (system /
  emitter side) and `material_inspect` (renderer side). Walks the
  asset's public API plus the cached VM compile data
  (`UNiagaraScript::GetVMExecutableData`) and reports usage token
  + usage GUID + `inputs` / `outputs` / `attributes` /
  `data_interfaces` parameter sets (`{name, type, type_path,
  kind}` rows mapped through `FNiagaraTypeDefinition::GetClass` /
  `GetScriptStruct` / `GetEnum`) plus a `compile_data` block
  (last-compile status, byte-code length, num temp registers,
  num user pointers, parameter / internal-parameter /
  baked-rapid-iteration counts, GPU shader parameter / loose-
  metadata / external-constant counts off
  `FNiagaraShaderScriptParametersMetadata`). Filters:
  `include_inputs` / `include_outputs` / `include_attributes` /
  `include_data_interfaces` / `include_compile_data` (default
  true) plus per-list caps (`max_inputs` / `max_outputs` /
  `max_attributes` / `max_data_interfaces`, default 512 each).
  Editor-only fields (`Parameters`, `AttributesWritten`,
  `BakedRapidIterationParameters`, `bReadsAttributeData`,
  `RegisteredFunctions`) gate behind `WITH_EDITORONLY_DATA`; the
  bridge runs editor-only so they always emit. Edit-side ops
  (build a new module from a typed input list, mutate per-script
  settings) stay on this list.
- `pcg_graph_edit` (small, read + edit slice) — inspect or
  mutate a `UPCGGraph` asset. Default op `inspect` walks
  `UPCGGraph::GetNodes()`, the per-node input / output pins,
  the graph's exposed input / output pin surface (`GetInputNode`
  / `GetOutputNode`), and a flat edge list stitched from each
  pin's `Edges` array. Edges are surfaced as `from` (upstream) /
  `to` (downstream) so callers do not have to remember PCG's
  reversed pin label convention (UPCGEdge::InputPin is the
  upstream side, UPCGEdge::OutputPin is the downstream side).
  Per-node fields cover `index`, `name`, `title` through
  `UPCGNode::GetNodeTitle(EPCGNodeTitleType::ListView)`,
  settings class + path through `UPCGNode::GetSettings()`, 2D
  editor `position` through `GetNodePosition`, and a pin
  descriptor list (`label`, `type` through
  `FPCGDataTypeIdentifier::ToString()`, `usage` (normal / loop /
  feedback / dependency_only), `status` (normal / required /
  advanced / override_or_user_param), `multiple_data`,
  `multiple_connections`, `invisible`, `edge_count`). The graph
  IO node uses sentinel indices (-1 for `GetInputNode`, -2 for
  `GetOutputNode`) so a downstream consumer can branch cleanly.
  Filters: `include_pins` (default true), `include_edges`
  (default true), `max_nodes` (default 1024), `max_edges`
  (default 4096). Edit ops: `add_node` (resolves a `UPCGSettings`
  subclass by short name or `/Script/Module.ClassName` path
  through `UPCGGraph::AddNodeOfType<T>` with optional `node_name`
  rename and 2D `position` write), `connect_pins` (creates an
  edge between two named nodes / pin labels through
  `UPCGGraph::AddEdge`; `from_node` / `to_node` accept a node
  FName, a node title substring, or the sentinel tokens `input`
  / `output` for the graph IO nodes; `from_pin` / `to_pin`
  default to the first matching output / input pin), and
  `remove_node` (`UPCGGraph::RemoveNode` plus cascading edge
  cleanup). The newer edit op `set_node_settings` resolves a
  target node by FName / title / substring (the same resolver
  `connect_pins` and `remove_node` use), pulls the node's
  `UPCGSettings` subobject through `UPCGNode::GetSettings()`,
  walks a flat `properties` dict, and applies each entry through
  `FProperty::ImportText_InContainer`. Accepts JSON booleans /
  numbers / strings plus complex shapes (objects / arrays) which
  re-serialize through the shared writer so a struct dict still
  round-trips when ImportText understands the emitted shape.
  Failed entries surface under `skipped` with a reason and the
  attempted ImportText string (mirrors `chaos_edit
  set_simulation_settings`). After the writes,
  `Settings->PostEditChangeProperty` runs and
  `Node->OnNodeChangedDelegate.Broadcast(Node, EPCGChangeType::Settings)`
  fires so any open PCG editor refreshes the node's pin layout
  / cached settings. Each mutating op runs `MarkPackageDirty`
  and saves the asset by default unless `save=false`. Adds
  `PCG` to PublicDependencyModuleNames and the PCG plugin to
  the uplugin manifest. The pin-rename op stays on this list.
- `landscape_edit` (small variant retry) — multi-op tool for
  ALandscape authoring, keyed by `op`. Two ops:
  - `set_landscape_material`: writes the proxy's master
    `LandscapeMaterial` UPROPERTY to a chosen UMaterialInterface
    (UMaterial or UMaterialInstance) and runs the same
    `PostEditChangeProperty` rebroadcast that the editor's
    BlueprintSetter uses, so component MICs rebuild on the next
    tick. The hole-material override (`LandscapeHoleMaterial`)
    stays on this list.
  - `import_heightmap_png`: decodes a 16-bit grayscale PNG file
    off disk through
    `IImageWrapperModule::CreateImageWrapper(EImageFormat::PNG)`
    plus `IImageWrapper::SetCompressed` / `GetRaw`, walks the
    resolved `ULandscapeInfo` extent through `GetLandscapeExtent`,
    and writes the height samples through
    `FLandscapeEditDataInterface::SetHeightData` so every
    component, heightmap texture, and collision mip lands in one
    pass. PNG dimensions must match the landscape's extent (the
    standard "components * CompSize + 1" inclusive grid). 8-bit
    PNGs land widened by * 257; floating-point PNGs are rejected.
  Pairs with `landscape_inspect`. The earlier batch dropped the
  whole landscape_edit family because the sculpt brush surface
  did not collapse to one focused minimum; this retry fences the
  small variant to the two operations above. The sculpt / paint
  brush surface, per-edit-layer writes, and import_layer_data
  branches stay on this list. Adds `ImageWrapper` to
  PublicDependencyModuleNames.
- `widget_edit` animation surface (small) — adds two ops to the
  existing widget_edit tool through the same `operation`
  discriminator: `add_animation` and `add_animation_track`.
  `add_animation` appends a new `UWidgetAnimation` to the WBP's
  `Animations` array, spawns a fresh `UMovieScene` outered to the
  animation, sets the display rate to 20 fps to match the
  AnimationTabSummoner default, and writes the playback range to
  `[FFrameNumber(0), end + 1]` ticks where end is
  `duration * MovieScene->GetTickResolution()` rounded to the next
  tick boundary. The asset's `DisplayLabel` picks up the supplied
  `animation_name` so the Animations panel shows the designer-
  readable label. The op refuses duplicate names against
  `WBP->Animations` and runs `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified`
  + `OnVariableAdded` so the WBP variable surface picks the new
  animation up. `add_animation_track` resolves a `UMovieSceneTrack`
  subclass by short token (`float` / `color` / `vector` /
  `transform` / `visibility` / `event` / `material` / `audio` /
  `bool` / `byte`) plus `/Script/MovieSceneTracks.X` paths and
  `/Script/Module.Class` shapes, walks the animation's
  `AnimationBindings` array for an existing binding to the named
  target widget, and either reuses that binding's GUID or spawns
  a fresh possessable through `UMovieScene::AddPossessable` plus a
  manual `FWidgetAnimationBinding` row before calling
  `UMovieScene::AddTrack(TrackClass, BindingGuid)`. We write the
  binding row directly rather than calling
  `UWidgetAnimation::BindPossessableObject` because that runtime
  helper expects a UUserWidget context that does not exist at
  asset-author time; the runtime resolution path goes through
  `FWidgetAnimationBinding::FindRuntimeObject` which the binding row
  alone can satisfy. An optional `property_path` lands on either
  the resolved track's `PropertyPath` or `PropertyName` field
  through reflection so designers calling against
  `MovieSceneFloatTrack` / `MovieSceneColorTrack` / similar
  property tracks get a friendly entry without us spelling out
  every track subclass. Per-keyframe authoring (section creation,
  Sequencer key-frame writes, transform sub-track binding) stays
  on the BACKLOG. Adds `MovieScene` includes; the module is
  already in PublicDependencyModuleNames from the sequencer_edit
  slice.
- `material_edit` parameter-collection surface (small) — adds two
  ops to the existing material_edit tool through the same
  `operation` discriminator: `create_parameter_collection` and
  `add_collection_parameter`. The first NewObject's a
  `UMaterialParameterCollection` at a `/Game/...` path through
  `UMaterialParameterCollectionFactoryNew::FactoryCreateNew`. The
  factory ships the asset with empty `ScalarParameters` /
  `VectorParameters` arrays. The second resolves the target MPC
  by path, refuses duplicate names across both arrays (matching
  the asset's `SanitizeParameters` rename guard but failing
  loud rather than silent), and appends a typed entry. The
  `parameter_type` token is `scalar` / `float` for scalar, or
  `vector` / `color` / `linear_color` for vector. Default values
  come in as a JSON number (scalars) or `[r, g, b, a]` array /
  `{r,g,b,a}` object / CSV string for vectors (flows through the
  same `TryParseLinearColor` helper material_edit's
  `set_instance_parameter` already uses). Each entry generates a
  fresh `FGuid::NewGuid()` so the asset's per-entry ID stays
  stable across renames. After the array write the op runs
  `PreEditChange` against the resolved
  `ScalarParameters` / `VectorParameters` UPROPERTY and a
  synthesised `FPropertyChangedEvent(EPropertyChangeType::ArrayAdd)`
  through `PostEditChangeProperty`, which is the asset's canonical
  "I just changed an MPC" path: regenerates `StateId`, rebuilds the
  uniform buffer layout through `CreateBufferStruct`, and walks
  `TObjectIterator<UMaterial>` requeueing every material that
  references this collection through `FMaterialUpdateContext`.
  With this pair landing the `material_edit` BACKLOG row narrows
  to Material Functions.
- `gas_edit` GameplayCue surface (small) — adds one op to the
  existing gas_edit tool through the same `op` discriminator:
  `create_cue_notify` (aliases `create_gameplay_cue` /
  `create_cue`). NewObject's a `UBlueprint` at a `/Game/...` path
  with a `UGameplayCueNotify_Static` (default) or
  `AGameplayCueNotify_Actor` parent. The `parent_class` arg
  accepts short tokens (`static` / `actor` / `notify_static` /
  `notify_actor` / `gameplay_cue_notify_static` /
  `gameplay_cue_notify_actor`) plus `UGameplayCueNotify_*` subclass
  paths. An optional `cue_tag` writes the asset's `GameplayCueTag`
  UPROPERTY through reflection: we resolve the FGameplayTag via
  `UGameplayTagsManager::Get().RequestGameplayTag` with
  `bErrorIfNotFound=false` so unknown tags surface a clean
  `cue_tag_warning` rather than crash. The mirror FName
  `GameplayCueName` field also picks up the typed tag string,
  matching the engine's PostEditChangeProperty fix-up flow that
  keeps the asset registry searchable. The op recompiles + saves
  by default. The `set_ability_cue_tag` op stays on this list:
  `UGameplayAbility` exposes cue invocation through the
  BlueprintCallable `K2_AddGameplayCue` / `K2_ExecuteGameplayCue`
  pair, but there is no canonical UPROPERTY storing a per-ability
  cue tag association, so the surface needs a graph-side authoring
  slice on the ability's event graph rather than a CDO write.
- `bp_input` modifier surface (small) — adds one op to the
  existing bp_input tool: `add_action_modifier` (aliases
  `add_modifier` / `add_mapping_modifier`). Writes a
  `UInputModifier` subobject onto an existing
  `FEnhancedActionKeyMapping` row's `Modifiers` array. The row is
  located by walking `IMC->GetMappings()` for a `(action, key)`
  match: `input_action` accepts a `/Game/...` UInputAction path
  (loaded through the asset registry) or a short name (matched
  against the row's `Action->GetName`); the FKey check
  short-circuits invalid input early. `modifier_class` resolves
  through short tokens (`negate` / `scalar` / `dead_zone` /
  `deadzone` / `swizzle_axis` / `swizzle`) or a UInputModifier
  subclass path with `/Script/EnhancedInput.X` and bare class name
  fallbacks. The new modifier outers under the IMC asset (matching
  the editor's `Instanced` subobject convention; the Modifiers
  array's `Instanced` UPROPERTY metadata wires the per-row
  serialisation). An optional flat `properties` dict applies
  through `FProperty::ImportText_InContainer` on the new modifier
  so callers can land `Order` on a swizzle, `Scalar` on a scalar,
  `LowerThreshold` / `UpperThreshold` / `Type` on a dead zone,
  `bX` / `bY` / `bZ` on a negate, etc., in the same call. Failed
  property entries surface under `skipped` with a reason and the
  attempted ImportText input, mirroring the convention shipped by
  `widget_edit set_slot_property`, `chaos_edit
  set_simulation_settings`, and `pcg_graph_edit set_node_settings`.
  Saves the IMC by default unless `save=false`. The
  per-row Triggers array (next to Modifiers on
  FEnhancedActionKeyMapping) and per-action Modifier surface on
  the UInputAction asset stay on this list.
- `bp_input add_action_chord` (small) — declarative one-call
  wrapper that lays a `UInputTriggerChordAction` down on a mapping
  row and binds its `ChordAction` slot to a sibling `UInputAction`
  in one step. The existing `add_action_trigger` op already covers
  the generic trigger-class side; this op fronts the canonical
  chord-action shape so the caller does not have to assemble a
  `chord_action` short token + `properties = {ChordAction =
  /Game/...}` dict on the way in. Resolves the host mapping row by
  `(input_action, key)` pair the same way `add_action_trigger`
  does, walks `IMC->GetMappings()` for the match, NewObject's a
  `UInputTriggerChordAction` outered to the IMC (matching the
  editor's `Instanced` UPROPERTY convention on
  `FEnhancedActionKeyMapping::Triggers`), writes the resolved
  sibling UInputAction to the trigger's public
  `ChordAction TObjectPtr<const UInputAction>` member, and appends
  the trigger to the row's `Triggers` array. The chord-action
  sibling resolves through a `/Game/...` path or a unique short
  name probed against the asset registry's UInputAction index.
  Refuses self-chord (the runtime never resolves a row chorded
  against its own action). Saves the IMC by default unless
  `save=false`. Optional flat `properties` dict lands on the new
  trigger through `FProperty::ImportText_InContainer` for any
  additional fields a future UE version drops on the
  UInputTriggerChordAction surface beyond `ChordAction`.
- `behavior_tree add_blackboard_decorator` (small) — declarative
  one-call shortcut for the Blackboard decorator the editor's
  add-decorator picker spawns most often. Spawns a
  `UBTDecorator_Blackboard` under a target child slot, wires the
  `FBlackboardKeySelector` against a named Blackboard key, picks
  the right operation family (`Basic` / `Arithmetic` / `Text`) from
  the resolved key's class (Int / Float / Bool / Enum -> Arithmetic,
  Name / String -> Text, Object / Class / Vector / Rotator /
  Struct -> Basic), and reflection-writes the matching
  `EBasicKeyOperation` / `EArithmeticKeyOperation` /
  `ETextKeyOperation` row plus the comparison payload field
  (`IntValue` / `FloatValue` / `StringValue`) through the property
  database so the protected UPROPERTY surface lands without
  touching engine private headers (`OperationType` is a protected
  uint8 on the decorator class; the operation tokens live behind
  `WITH_EDITORONLY_DATA` TEnumAsByte fields the reflection cast
  reads through). Conditions cover `IsSet` / `IsNotSet` (Basic
  family) plus `IsEqualTo` / `IsNotEqualTo` (Arithmetic family for
  numeric / bool / enum keys, Text family for FName / FString
  keys). The optional `value` field accepts a number, bool, or
  string and lands on all three payload fields so the engine's
  enum-key resync path (UBTDecorator_Blackboard::RefreshEnumBasedDecorator)
  has the StringValue mirror to read from. The `BlackboardKey`
  selector's inner `SelectedKeyName` lands through the
  FStructProperty + FNameProperty inner write; the engine's
  `InitializeFromAsset(*Tree)` resolves SelectedKeyID + SelectedKeyType
  off the tree's Blackboard before we attach the decorator to the
  child slot. `NotifyObserver` defaults to `ResultChange`
  (matches the editor's default); pass `notify_observer="value_change"`
  to switch to `EBTBlackboardRestart::ValueChange`. The response
  surfaces the resolved key + classified operation family + the
  payload triple + the decorator FName so a follow-up call can
  address the spawned decorator.
- `behavior_tree set_root_decorator` (small) — appends a
  UBTDecorator to `UBehaviorTree::RootDecorators`, the tree-level
  decorator chain the BT editor exposes under "Add Decorator" on
  the root composite. NewObject's the decorator outered to the
  tree, runs `InitializeFromAsset`, and lands an optional flat
  property dict through `FProperty::ImportText_InContainer`. The
  decorator class is resolved through the same short-name table
  `add_decorator` uses (`blackboard` / `cooldown` / `loop` /
  `time_limit` / `force_success`) plus full `/Script/...` paths
  and `/Game/...` BP class paths. Refuses abstract classes.
  Saves the BT on success unless `save=false`. Closes the
  "tree-level RootDecorator authoring" follow-on the prior
  `behavior_tree` pass tagged.
- `behavior_tree rename_blackboard_key` (small) — renames an own
  key on a target UBlackboardData and propagates the new name to
  every FBlackboardKeySelector across every asset that
  Hard-references the BB's package and implements
  IBlackboardAssetProvider. Refuses duplicate names (own + parent
  keys) plus a same-name no-op. Fires the documented
  PreEditChange / PostEditChangeChainProperty pair against the
  `Keys` array's `EntryName` subfield so open BB pickers refresh,
  then walks each Hard-referencer through
  `IAssetRegistry::GetReferencers(EDependencyCategory::Package,
  EDependencyProperty::Hard)`, loads each candidate, iterates
  every subobject under the package via `GetObjectsWithOuter`,
  and rewrites each `FStructProperty` of FBlackboardKeySelector
  whose `SelectedKeyName` matches the old name. Saves the BB
  plus every dirty referencer package. The walk re-implements
  the BT editor's UpdateExternalBlackboardKeyReferences path
  clean-room against the public AIModule + AssetRegistry API so
  we stay clear of the editor-only BehaviorTreeEditor module.
  Closes the rename half of the "Blackboard rename /
  type-change ops" follow-on.
- `sequencer_edit add_visibility_track` (small) — declarative
  one-call wrapper for the show / hide pattern. Resolves a
  binding through `binding` GUID or `actor` / `possessable` name
  (visibility is per-binding). For possessables we attach
  `UMovieSceneVisibilityTrack` (the bool property track that
  drives `SetActorHiddenInGame`); for spawnables we attach
  `UMovieSceneSpawnTrack` (the bool track that gates the
  spawnable's lifetime). The binding kind is decided by a single
  `UMovieScene::FindSpawnable` cast on the resolved GUID. The
  track is reused when an existing one of the right class lives
  under the binding so the op is idempotent for follow-up adds
  (`force_new_track=true` overrides). The section spawns via
  `UMovieSceneTrack::CreateNewSection` + `AddSection`; the range
  wraps `[start_frame, end_frame)` with the inclusive-start /
  exclusive-end convention. `start_frame` defaults to the
  MovieScene's playback range start; `end_frame` defaults to
  `start_frame + duration_frames` (when supplied) or the
  playback range end otherwise. Bool channel default lands
  through the shared `UMovieSceneBoolSection` base both subclasses
  inherit, so a single `GetChannel().SetDefault(visible)` covers
  both paths. Pass `visible=false` to key "hidden" / "dead". The
  response carries the binding kind, the track-reuse flag, and
  the resolved frame triple so the caller can verify what
  landed.
- `niagara_edit set_system_warmup` (small) — writes the
  system-level warmup trio (`WarmupTime` seconds,
  `WarmupTickCount` ticks, `WarmupTickDelta` seconds-per-tick)
  on a UNiagaraSystem. The public NIAGARA_API mutators
  `SetWarmupTime` and `SetWarmupTickDelta` call the engine's
  `ResolveWarmupTickCount` so the derived count stays consistent
  with the time / delta. When the caller wants to pin the tick
  count directly we reflection-write `WarmupTickCount` (no
  public setter; the engine derives it from the time / delta
  pair) and harmonise `WarmupTime` so the editor's
  `WarmupTime > 0` EditCondition reveals the rest of the warmup
  surface. The response carries the previous + new trio plus
  the `NeedsWarmup()` flag so the caller can verify the warmup
  is active. Saves the system on success unless `save=false`.
- `material_edit add_2d_array_sample` (small) — Texture2DArray
  sibling of `add_texture_sample` / `add_texture_sample_cube`.
  Spawns either a plain `UMaterialExpressionTextureSample` and
  binds a chosen `UTexture2DArray` directly when no
  `parameter_name` is supplied, or a
  `UMaterialExpressionTextureSampleParameter2DArray` when
  `parameter_name` is set, and lands the FName on the parent
  `UMaterialExpressionTextureSampleParameter::ParameterName`
  slot so the resulting material exposes a named array slot
  calling Material Instances can swap. The spawn class
  auto-detects from `Texture->IsA<UTexture2DArray>()` with a
  fallback to `UMaterialExpressionTextureSampleParameter2D` for
  the parameter variant when the resolved asset is actually a
  `UTexture2D`, mirroring the cube variant's fallback rule so
  a generic "wire this texture up" call still lands. The
  shared base class `UMaterialExpressionTextureBase` carries
  the `Texture` UPROPERTY plus `AutoSetSampleType()` so the
  same writer path applies regardless of which spawn class we
  picked. All the downstream knobs `add_texture_sample` exposes
  apply (`coordinates` / `coordinates_output`, `connect_to` /
  `connect_input` / `property`, `name`, `properties`).
  Recompiles + saves on success unless `recompile=false` /
  `save=false`. The response surfaces `is_texture_2d_array` /
  `fell_back_to_2d` / `as_parameter` / `parameter_name` so a
  caller can verify which spawn class actually landed. With
  this op landing the `add_texture_sample_cube` BACKLOG row's
  follow-on list narrows to the
  `TextureSampleParameterVolume` /
  `TextureSampleParameterSubUV` /
  `TextureSampleParameterCubeArray` variants, all of which the
  existing `add_expression` short-name table can still cover
  through the bare class-name fallback.
- `material_edit add_texture_sample_cube` (small) — cube-texture
  sibling of `add_texture_sample`. Spawns a
  `UMaterialExpressionTextureSampleParameterCube` and binds the new
  node's `Texture` property to a chosen `UTextureCube` asset in one
  call. The cube-parameter variant carries a `ParameterName` slot the
  caller can populate through the same `name` / `properties` knobs
  `add_expression` exposes. When the resolved asset is a `UTexture2D`
  (or anything other than a UTextureCube) the op falls back to
  spawning a `UMaterialExpressionTextureSample` so a generic "wire
  this texture up" call still lands; the response surfaces
  `is_cube_texture` and `fell_back_to_2d` flags. The shared base
  class `UMaterialExpressionTextureBase` carries the `Texture`
  UPROPERTY and the documented `AutoSetSampleType()` (ENGINE_API on
  `MaterialExpressionTextureBase.h` line 63) the editor's right-click
  "Refresh Sampler Type" entry uses, so the same writer path applies
  to both branches without spelling out the per-class case. All the
  downstream knobs `add_texture_sample` exposes apply (`coordinates`
  / `coordinates_output` wires the UV / 3-vector input from a named
  expression; `connect_to` / `connect_input` / `property` lands the
  RGB output on another expression or a material attribute; `name`
  renames the spawned expression; `properties` lands the
  `ParameterName` / `Group` / `SortPriority` UPROPERTYs through
  `FProperty::ImportText_InContainer`). Position cascades through
  the same `DeriveDefaultPosition` helper. Recompiles + saves on
  success unless `recompile=false` / `save=false`. With this op
  landing the `add_texture_sample` BACKLOG row that read "non-2D
  texture-sample variants stay on the open backlog" narrows to the
  `TextureSampleParameterVolume` / `TextureSampleParameterSubUV` /
  `TextureSampleParameter2DArray` / `TextureSampleParameterCubeArray`
  variants, which the existing `add_expression` short-name table
  can still cover through the bare class-name fallback.
- `material_edit add_texture_sample` (small) — adds a
  `UMaterialExpressionTextureSample` to a target UMaterial and binds
  the new node's `Texture` property to a chosen UTexture asset in one
  call. Spawns the expression through
  `UMaterialEditingLibrary::CreateMaterialExpression` (the same path
  `add_expression` uses for every other expression subclass), writes
  `TextureSample->Texture` directly, and runs
  `UMaterialExpressionTextureBase::AutoSetSampleType` (ENGINE_API on
  MaterialExpressionTextureBase.h line 63) so the engine derives the
  sampler type from the texture the same way the editor's right-click
  "Refresh Sampler Type" entry does. Without that step the node renders
  with a mismatched sampler and a downstream compile warning. The
  texture resolves through a `/Game/...` path or a unique short name
  probed against the asset registry's UTexture index (any UTexture
  subclass passes the IsA check). Optional `coordinates` knob names
  an existing expression on the same material whose first output (or
  the pin named by `coordinates_output`) wires into the texture
  sample's `Coordinates` input through
  `UMaterialEditingLibrary::ConnectMaterialExpressions`. Optional
  `property` / `connect_to` / `connect_input` knobs mirror
  `add_expression` so a single call can both spawn the sample and
  drop its RGB output into BaseColor (or another material attribute /
  expression). Position cascades through the same `DeriveDefaultPosition`
  helper `add_expression` uses; optional `name` renames the spawned
  expression. Recompiles + saves on success unless `recompile=false`
  / `save=false`. The non-2D texture-sample variants
  (TextureSampleParameterCube, TextureSampleParameterVolume) stay
  on the open backlog under the existing `add_expression` shape.
- `sequencer_edit set_transform_channel_mask` (small) — declarative
  one-call writer for the per-section `FMovieSceneTransformMask`.
  Resolves the target binding + transform track + section the same
  way `add_transform_section_keys` does (`binding` GUID or
  `actor` / `possessable` name; `section_index` defaults to 0 into
  the track's `GetAllSections()` array). The mask is supplied either
  as a flat `channels` list of `EMovieSceneTransformChannel` token
  names (`translation_x` / `rotation_y` / `scale_z` / `translation`
  / `rotation` / `scale` / `all_transform` / `weight` / `all` /
  `none`, case-insensitive with snake / camel and `_` / `.`
  normalisation so callers can spell either way) or a raw integer
  `mask` matching the EMovieSceneTransformChannel bit layout
  (TranslationX = 0x001 ... ScaleZ = 0x100 plus Weight = 0x200).
  Routes through the documented MOVIESCENETRACKS_API
  `UMovieScene3DTransformSection::SetMask` (which clears
  `ChannelProxy` so the proxy regenerates on the next inspect; the
  editor's channel list refreshes accordingly). Marks the section +
  sequence packages dirty and saves on success unless `save=false`.
  The response carries the previous + new mask both as a raw
  bitfield (`previous_mask`, `mask`) and as a tokenised channel
  list (`previous_channels`, `channels`) so the caller can audit the
  diff. With this op landing the
  `sequencer_edit add_transform_section_keys` per-section channel
  authoring row on the open BACKLOG narrows to the per-row weight /
  constraint channel writes.
- `sequencer_edit add_transform_section_keys` (small) — declarative
  one-call writer for per-channel transform keys on a binding's
  `UMovieScene3DTransformTrack`. Resolves the target binding through
  `binding` GUID or `actor` / `possessable` name, finds or creates
  the transform track via `UMovieScene::FindTrack` / `AddTrack`,
  finds or creates a `UMovieScene3DTransformSection` via the track's
  `CreateNewSection` + `AddSection`, then walks the supplied
  `keyframes` list and writes each `{time_frames, location?,
  rotation?, scale?}` row through the section's channel proxy.
  Channel layout follows `UMovieScene3DTransformSection::CacheChannelProxy`:
  slots 0..2 are Translation X/Y/Z, slots 3..5 are Rotation X/Y/Z
  (Roll/Pitch/Yaw on FRotator), slots 6..8 are Scale X/Y/Z. The op
  writes through `FMovieSceneDoubleChannel::AddCubicKey` /
  `AddLinearKey` / `AddConstantKey` (the 5.4+ double-channel storage
  shape) with a `FMovieSceneFloatChannel` fallback for older section
  variants. The section's range expands through `ExpandToFrame` to
  cover the min / max key times so a camera fly-through is one call.
  Optional `interpolation` is `cubic` (default) / `linear` /
  `constant`. Saves on success unless `save=false`. The per-mask
  EMovieSceneTransformChannel toggle and the weight / constraint
  channel writes stay on this list.
- `animation_edit replace_blendspace_sample` (small) — swaps the
  UAnimSequence on an existing sample at `sample_index` through the
  documented public editor surface
  `UBlendSpace::ReplaceSampleAnimation(SampleIndex, AnimSequence)`
  (`ENGINE_API` gated under `WITH_EDITOR`). Bounds-checked through
  `IsValidBlendSampleIndex` so the response surfaces a clear "out of
  range" message rather than a silent false return; an empty / `none`
  animation string or explicit `clear=true` unbinds the sample's
  sequence (mirrors the editor's "Clear" picker). The replacement
  sequence's skeleton goes through the same
  `IsAnimationCompatibleWithSkeleton` / `IsAnimationCompatible` chain
  `add_blendspace_sample` already runs so the asset never lands in a
  state the engine refuses to play. Marks the package dirty + saves
  on success unless `save=false`. With this op landing the `add_sample`
  -> `replace_sample` -> `edit_value` triplet on the BACKLOG narrows
  to the per-sample value edit (`UBlendSpace::EditSampleValue`).
- `animation_edit delete_blendspace_sample` (small) — removes the
  sample at `sample_index` through the documented public editor
  surface `UBlendSpace::DeleteSample(SampleIndex)`. Bounds-checked
  through `IsValidBlendSampleIndex`. The response carries the
  removed sample's animation path + sample value so the caller can
  reapply through `add_blendspace_sample` if they only meant to nudge
  the sample's axis position. Marks the package dirty + saves on
  success unless `save=false`.
- `animation_edit add_blendspace_sample` (small) — appends a sample
  to a UBlendSpace or UBlendSpace1D through the documented public
  editor surface `UBlendSpace::AddSample(UAnimSequence*, FVector)`
  (`ENGINE_API` gated under `WITH_EDITOR`). `blendspace` (alias
  `blend_space` / `asset`) is the target asset; `animation` (alias
  `anim_sequence` / `sequence`) is the UAnimSequence path;
  `sample_value` (alias `value` / `position`) is a 1-element
  `[x]` array for UBlendSpace1D or a 2-element `[x, y]` array for
  UBlendSpace (the engine's FBlendParameter[3] storage carries an
  unused third axis for both classes so we accept up to three
  values and pad with zero). Before AddSample runs the op refuses
  the operation when the animation's skeleton fails
  `UBlendSpace::IsAnimationCompatibleWithSkeleton`, when the
  additive type fails `IsAnimationCompatible`, or when the sample
  value fails `ValidateSampleValue` (range check plus
  too-close-to-existing-sample probe), so callers see a specific
  error rather than a silent INDEX_NONE return. The fork ships
  this op against the canonical UE5 BlendSpace authoring path so
  the asset stays consistent with the editor's "drop animation
  in the blend grid" path: AddSample handles the snap-to-grid /
  validation / SampleData mutation and any open BlendSpace editor
  refreshes through PostEditChangeProperty on the next selection
  change. Marks the package dirty and saves on success unless
  `save=false`. The follow-on edit ops (per-sample replace /
  delete / edit_value / blend-axis remap) stay on this list.

## Blueprint authoring (medium to large each)

- `bp_create` (small variant ships in this fork) — short-name parent
  class resolution, optional flat property dict on the CDO, compile +
  save on success. Open follow-ons: assign Blueprint interfaces at
  creation time, post-create `bp_class` reparenting, and post-create
  default-component dict beyond CDO properties.
- `bp_class` (small variant ships in this fork) — `read` / `set_parent`
  / `set_class_settings` / `add_interface` / `remove_interface`. Open
  follow-ons: structural fixups when the new parent removes parent
  members the BP still references (member-fixup), and per-class flag
  toggles (Const / Abstract / NotPlaceable) we don't expose yet.
- `bp_variable` (small variant ships in this fork) — `list` / `add` /
  `remove` / `set_default` / `set_flags` against `NewVariables`. Open
  follow-ons: per-variable replication-condition surface beyond the
  bool toggle, default-value parsing for nested struct literals through
  `FProperty::ImportText` (instead of the current raw-text fallback),
  and a `rename` op that fixes up downstream Get / Set nodes.
- `bp_component` (single-add variant ships in this fork) — broader hosted
  surface still pending: move / rename / remove component nodes, deeper
  per-component property control, and full reparenting under an arbitrary
  attach socket name.
- `bp_graph` (small read-only variant ships in this fork) —
  `list_graphs` / `list_nodes` / `get_node` / `list_connections`. Open
  follow-ons: a `get_or_create_event_graph` op for declarative graph
  authoring, function-graph creation with typed inputs / outputs in a
  single call, and a `compact` mode that returns a graph as a single
  edge-list dump (one payload, no per-node fan-out).
- `bp_nodes` (small variant ships in this fork) — batched K2 node
  creation in a chosen graph for the most-used node classes
  (variable_get, variable_set, call_function, branch, dynamic_cast,
  self, format_text, execution_sequence, knot, make_array,
  custom_event, event). Open follow-ons: SwitchEnum / SwitchInteger /
  SwitchString / SwitchName, MakeStruct / BreakStruct, MathExpression,
  AddComponentByClass, and a per-call `auto_wire` flag that infers
  obvious exec connections from a sequential `nodes` array.
- `bp_wire` (small variant ships in this fork) — connect / disconnect
  named pins through `MakeLinkTo` / `BreakLinkTo` with K2 schema
  compatibility checks. Open follow-ons: pin-default value writes
  during the same call, batched promoted-default literal nodes when a
  type does not match, and a `breakall` op that drops every wire on
  a named pin in one shot.
- `bp_input` (asset side + first graph-wiring slice ship in this fork) —
  the data asset side (`create_input_action`,
  `create_input_mapping_context`, `add_mapping`) and a focused
  `add_action_event_node` operation are shipped. Open follow-ons: select
  multiple trigger exec pins in one call, parameter-binding from the
  enhanced-input action value pin into the connected function, and
  pin-by-pin `set_node_property` overrides on the spawned node.
- `bp_commit` (small variant ships in this fork) — `MarkBlueprintAsStructurallyModified` + `CompileBlueprint` with captured `FCompilerResultsLog` + `SaveAsset`. Surfaces error / warning / info lines as separate string arrays and refuses to save a broken Blueprint unless `force_save=true`. Open follow-ons: include the structural diff against the previous compiled state (added / removed function signatures and variable types), and a per-call timing breakdown.
- `bp_function_create` (small variant ships in this fork) — declarative `CreateNewGraph` + `AddFunctionGraph<UClass>` + typed FunctionEntry / FunctionResult pins in one call. Open follow-ons: add a Local Variables array to the new function in the same call, add Latent flag handling + UObject return-pin glue beyond the current FUNC_BlueprintPure toggle, and a `from_interface` mode that fills the signature from a Blueprint Interface method's parameter list.
- `bp_author` (small variant ships in this fork) — Python-side
  compositor that walks a declarative JSON spec
  (`{name, parent_class, variables, components, graphs: {events,
  functions}}`) and runs `bp_create` → `bp_variable add` →
  `bp_component` → `bp_function_create` → `bp_nodes` → `bp_wire` →
  `bp_commit` in order. Each step's response surfaces on the ordered
  `steps` array. Open follow-ons: per-step rollback hooks (drop
  the asset when a mid-flight step fails on `stop_on_error=true`),
  declarative graph-edge auto-routing where the spec lists nodes
  but does not spell out every wire, and a `--diff` mode that
  emits the canonical bp_export delta the run produced.
- `bp_dry_run` (small variant ships in this fork) — Python-side
  validator that walks the same spec `bp_author` accepts, probes
  the parent class through live `unreal_api describe`, runs the
  variable-type-token / class-token / node-class shape checkers
  offline, and returns a `would_do` step plan plus `unresolved`
  rows for unknown tokens and `warnings` for soft red flags.
  Open follow-ons: live component-class probing (we currently
  skip the live probe for component classes since the bp_component
  resolver does its own lookup), pin-compatibility checks for the
  declared `bp_wire` edges, and a `cost_estimate` field that sums
  per-stage compile counts.
- `bp_skills` (small variant ships in this fork) — alias for the
  `skills` tool scoped to topic IDs starting with `blueprint-`.
  Seed entries: `blueprint-events` and `blueprint-variables`.
  Adding a new entry is a file-add under `Python/skills/`, not a
  code change.

The local repo already implements `add_node`, `connect_nodes`,
`create_variable`, etc. The hosted batched variants would sit on top of those
helpers.

## Blueprint inspection (medium)

- `bp_brief` (small variant ships in this fork) — read-only one-page
  orientation summary. Open follow-ons: per-variable replication-condition
  surface, deeper interface-method-by-method readout, and a "constructed
  archetype" comparison against the parent class CDO.
- `bp_inspect` (small variant ships in this fork) — `list_variables`,
  `list_functions`, `list_events`, `list_components`, `find_node`. Open
  follow-ons: per-pin readback for matched nodes, per-function
  parameter-list readout, and timeline / sequencer node summaries.
- `bp_export` (small variant ships in this fork) — canonical
  Blueprint-to-JSON snapshot covering every graph plus components,
  variables, defaults, interfaces, and pin-level edges. Open
  follow-ons: a deeper component default-overrides-only mode
  (compare against the parent CDO and only emit deltas), a graph
  filter param so a caller can restrict the dump to a single named
  graph, and a `compact` mode that returns the snapshot as a
  graphviz-style edge dump for one-shot rendering.

## Scene & level (medium each)

- `scene_query` (small variant ships in this fork) — broader hosted scope
  still pending: bounding-box / convex-volume spatial filters, actor
  component listings as part of each record, and multi-tag / boolean tag
  filters.
- `scene_brief` (small variant ships in this fork) — broader hosted
  scope still pending: post-process volume settings inline, world
  partition state surface where present, world settings nav-mesh /
  lighting summary.
- `scene_compose` (small variant ships in this fork) — broader hosted
  scope still pending: batched spawn / modify / delete in a single call,
  prefab / level snippet rollouts, and child-actor reparenting.
- `actor_inspect` (small variant ships in this fork) — broader hosted
  scope still pending: full component child-actor recursion, deeper
  per-component property control, and component-by-name lookups inline.
- `level_inspect` (small variant ships in this fork) — broader hosted
  scope still pending: streaming-volume listings with their bound
  ULevelStreaming entries, World Partition cell state, world settings
  fragments inline.
- `search_assets` (small variant ships in this fork) — Content
  Browser search through `IAssetRegistry`. Open follow-ons: persisted
  saved-search definitions and result paging across multiple calls
  through an opaque cursor.
- `asset_references` (small variant ships in this fork) — dependency
  graph for an asset through `IAssetRegistry::GetReferencers` /
  `GetDependencies`. Open follow-ons: SearchableName / Manage
  category support beyond the current package-only walk, return-by-
  level grouping when the seed package is a level, and a `dot_graph`
  output mode for one-shot rendering of the dependency closure.
- `project_context` (small variant ships in this fork) — one-shot
  read-only project summary covering identity, engine version,
  enabled plugins (filtered to user-installed by default), source
  modules from the .uproject, top-level Content folders with
  recursive asset counts, and the active map + GameMode +
  default pawn (per-level override and project-wide). Open
  follow-ons: per-plugin module list (descriptor's `Modules` array)
  beyond the project's modules, project-wide tag categories /
  feature packs surface, and an `additional_plugin_directories`
  array exposing the .uproject's external plugin search paths.

## Materials & shading (large)

- `material_inspect` (small variant ships in this fork) — read-only
  dump of a UMaterial or UMaterialInstance: expression list, parameter
  set across scalar / vector / texture / static_switch, per-attribute
  connected output expression for the standard GBuffer attributes,
  used texture list, and instance overrides. Open follow-ons:
  Material Function and Material Parameter Collection inspection,
  per-expression connected-output dump (which output of which child
  drives each input of this expression), and an `include_graph`
  toggle that returns the expression graph as an edge list.
- `material_edit` (small variant ships in this fork: create material
  with a Constant3Vector base colour, create material instance
  constant, set scalar / vector / texture parameters on an instance,
  the expression-graph trio `add_expression` / `connect_expressions`
  / `set_expression_property`, plus the bulk `add_expressions` op
  that lays a small graph down in one call from a list of expression
  specs and a list of edge specs). The Material Parameter
  Collection slice landed via `create_parameter_collection` +
  `add_collection_parameter`, the Material Function slice landed
  via `create_material_function` + `add_function_call` (the call
  expression spawns through `UMaterialEditingLibrary::CreateMaterialExpression`
  with `UMaterialExpressionMaterialFunctionCall::StaticClass()` and
  binds the function via the BlueprintCallable `SetMaterialFunction`
  so the call node's inputs / outputs populate). Pending:
  `set_attribute_blendable` op for the override-surface chain on
  an instance.

## VFX (large each)

- `niagara_inspect` (small read-only variant ships in this fork) — dump
  emitters, per-stage scripts, event-handler chain, simulation-stage
  class list, renderer class list, and the user-exposed parameter
  store entries. Open follow-ons: per-emitter renderer property
  readback (lit-sprite / mesh / ribbon settings), data-interface
  configuration dump beyond the type kind, and per-script binding
  list for the rapid-iteration parameter set.
- `niagara_edit` (small ultra-minimum-cut variant ships in this fork) —
  one op: `create_niagara_system` through
  `UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/false)`.
  No emitters, no parameter store, no module / sim-stage authoring;
  the asset opens in the Niagara editor with the documented "no
  emitter" warning. The follow-on emitter / parameter / module / sim-
  stage / sim-target / request-compile ops landed in later passes
  (see the `add_emitter_from_asset` / `set_emitter_local_parameter`
  / `add_module_to_stage` / `add_sim_stage` /
  `set_emitter_sim_target` / `request_compile` / `set_emitter_flag`
  edit slices summarised above). Remaining open follow-ons:
  system-side `GetExposedParameters()` parameter-store mutations,
  GPU compute-script wiring on the per-emitter compute script, and
  the per-stage module attach beyond `add_module_to_stage`.
- `niagara_script_edit` (small read-only first slice ships in this
  fork) — inspect a `UNiagaraScript` asset (cached VM compile
  data plus typed parameter sets and the GPU shader parameter
  metadata). Open follow-ons: reusable Niagara module authoring
  (build a new module asset from a typed input / output list and
  a literal HLSL body or a chosen op chain), per-script
  validation rule walk, and a deep-dive `compile_metadata` op
  that emits the byte-code disassembly through
  `FNiagaraVMExecutableData::LastAssemblyTranslation`.
- `chaos_edit` (small read + edit slice ships in this fork) —
  inspect a `UGeometryCollection` asset (geometry-source list +
  per-fracture-level histogram + cluster info + bone hierarchy
  depth + simulation block + materials + Nanite block) plus the
  edit ops `set_simulation_settings` (flat property dict over the
  reflected simulation surface), `import_static_mesh` (appends a
  UStaticMesh via `FGeometryCollectionConversion::AppendStaticMesh`),
  and `fracture_box` (axis-aligned box fracture through the
  `PlanarCut` plugin's `CutWithPlanarCells` entry point;
  `min` / `max` 3-element arrays describe the region, optional
  `divisions` `[X, Y, Z]` int triple drives the cell grid through
  `FPlanarCells(FBox, FIntVector)`, optional `transform_index`
  picks the target transform), plus `set_damage_threshold`
  (writes the asset's `DamageThreshold` `TArray<float>` /
  per-fracture-level threshold table; accepts either a `thresholds`
  per-level float array or a single uniform `threshold` /
  `value`; optional `set_damage_model=true` flips
  `DamageModel` over to UserDefined so the per-level threshold
  actually drives the runtime strain; optional
  `clear_size_specific=true` clears
  `bUseSizeSpecificDamageThreshold` so the per-level table
  applies; invalidates the collection on success). Open
  follow-ons: the dataflow driver under the asset's
  `DataflowAsset` (the inner FDataflowNode `DamageThresholds`
  is not a UPROPERTY on the UDataflowEdNode wrapper so the
  write needs the editor-only DATAFLOWENGINE dependency in this
  module), per-instance damage threshold override,
  fracture-level mutate / re-cluster ops, per-bone damage
  propagation tweaks, and the heavier fracture entry points
  (`CutMultipleWithPlanarCells` with multi-transform selection,
  `CutWithMesh` for arbitrary cutting meshes).

## Animation (large each)

- `animation_inspect` (small read-only variant ships in this fork) —
  branch by class for USkeletalMesh / UAnimSequence / UAnimMontage /
  UBlendSpace / UAnimBlueprint. Open follow-ons: per-LOD bone-name
  delta against the skeleton, virtual-bone surface (USkeleton::GetVirtualBones)
  beyond the basic bone list, AnimComposite + AimOffsetBlendSpace
  + UAnimSequence's TrackToSkeletonMapTable readback, Anim Notify
  Track readout (Tracks array on UAnimSequenceBase) so the agent
  can ask "which track is this notify on", FCompositeSection's
  `MetaData` array dump on UAnimMontage, and an `include_anim_graph_dump`
  toggle on UAnimBlueprint that walks the EventGraph + AnimGraph
  pages and emits the K2 nodes' titles + classes (matches
  `bp_inspect`'s shape).
- `animation_edit` (small variant ships in this fork) — multi-op
  tool keyed by `op` covering `set_rate_scale` (UAnimSequenceBase
  RateScale write), `set_additive` (UAnimSequence AdditiveAnimType
  / RefPoseType / RefPoseSeq / RefFrameIndex), and `add_notify`
  (FAnimNotifyEvent append through
  `UAnimationBlueprintLibrary::AddAnimationNotifyEvent` /
  `AddAnimationNotifyStateEvent`, with auto-create on the named
  notify track). Open follow-ons: per-curve key authoring through
  the IAnimationDataController surface, sync-marker authoring,
  composite section authoring on UAnimMontage (slot tracks,
  sections, transitions), and per-notify property dict on add.
- `animation_graph_edit` (small read + edit slice ships in this
  fork) — inspect a `UAnimBlueprint`'s compiled state machines
  plus the flat AnimGraph node list off
  `UAnimBlueprintGeneratedClass::BakedStateMachines` and
  `AnimNodeProperties`, plus one edit op `add_state` that spawns
  a new `UAnimStateNode` on a chosen state machine through
  `FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate`.
  Open follow-ons: state machine create / mutate (add / remove
  transition, remove state, set initial state, override per-
  transition crossfade / blend mode), per-state property writes
  (always-reset / conduit / state-entered / state-left notify),
  AnimGraph node add / connect / disconnect, link a Linked Anim
  Graph by tag, sub-state-machine / nested-state-machine
  surface, and an editor-only walker that emits the AnimGraph
  node titles + per-node UPROPERTY defaults so a caller can
  reason about an existing AnimBP without round-tripping the
  raw FStructProperty list.
- `ik_rig_edit` (small read-only first slice ships in this fork) —
  inspect a `UIKRigDefinition` asset. Walks asset public surface
  (preview mesh, retarget root, retarget chains with start / end
  bone + IK goal name, IK goals with current / initial transforms +
  position / rotation alpha) plus the polymorphic solver stack
  (per-solver struct type, enabled flag, optional `start_bone` /
  `end_bone`, reflection-driven `settings` dict reflected off
  `GetSolverSettings()` plus the settings struct type, and a
  `bone_settings` array for solvers gating `UsesCustomBoneSettings()`
  true). Open follow-ons: the edit side (rebind preview mesh
  through `IIKRigController`, append / remove solver via
  `AddSolverToStack`, append / remove chain through
  `AddRetargetChain`, rename retarget root, override goal
  transform, mutate per-solver bone settings via
  `SetBoneSettings`).
- `ik_retarget` (small read + edit slice ships in this fork) —
  inspect or mutate a UIKRetargeter asset. Walks the asset's
  public surface (source / target IK Rig paths, current source /
  target retarget pose names, retarget op stack with per-op
  `name` / `parent_name` / `struct_type` / `enabled` /
  `initialized` flags and any per-op chain mapping pairs, plus
  aggregate counts). Edit ops route through the editor-only
  `UIKRetargeterController::GetController(Retargeter)` accessor:
  `set_source_ik_rig` and `set_target_ik_rig` rebind the source /
  target IK Rig through `UIKRetargeterController::SetIKRig`;
  `set_retarget_pose` switches the active retarget pose for either
  side through `SetCurrentRetargetPose(PoseName, Side)`. Open
  follow-ons: append op / remove op (the polymorphic
  FInstancedStruct array on the asset), set chain mapping pair on
  a chosen op, per-pose bone-rotation-offset dump and override,
  retargeter evaluation against a sample pose, and profile
  management through `UIKRetargeter::GetProfileByName`.

## UMG / Widgets (medium)

- `widget_inspect` — the small variant ships in this fork. The remaining
  hosted-Flop scope (style readback, MVVM binding readback) is still
  outstanding.
- `widget_edit` — the small variant plus the slot-property surface
  ship in this fork. `set_slot_property` covers UCanvasPanelSlot /
  UVerticalBoxSlot / UHorizontalBoxSlot / UOverlaySlot etc. without
  spelling out each subclass. The animation surface ships through
  `add_animation` + `add_animation_track` + `add_keyframe`; the
  MVVM side ships through `set_viewmodel` (viewmodel slot + seeded
  empty binding) plus `add_property_binding` (full
  FMVVMBlueprintViewBinding row authoring: source viewmodel field
  + destination widget field + binding mode tokens) plus
  `set_binding_conversion` (per-direction UFunction conversion
  rewrite). Event binding ships through `add_event_binding`
  (spawns a `UK2Node_ComponentBoundEvent` for a child widget's
  multicast delegate; the canonical
  `FKismetEditorUtilities::CreateNewBoundEventForClass` hot path).
  Style authoring ships through `set_widget_style` (flat property
  dict against a child widget's style struct field through
  reflection; defaults to the `WidgetStyle` UPROPERTY and supports
  any FXyzStyle slot). Brush authoring ships through
  `set_widget_brush` (writes an FSlateBrush field on a target
  child widget; sugar keys `texture` / `material` / `tint` /
  `size` / `margin` / `tiling` / `draw_as` plus a reflective
  `brush` dict for raw FSlateBrush UPROPERTY writes; defaults the
  brush_field to `Brush` for UImage and `Background` for UBorder).
  Remaining open follow-ons: the K2Node-class conversion branch
  (async conversion nodes on `set_binding_conversion`),
  per-binding pin-default authoring inside the wrapper graph, and
  USlateBrush asset-driven style swaps (i.e. plumbing the
  `Style` slot of a `USlateWidgetStyleAsset` onto the resolved
  style struct).

## AI & abilities (large each)

- `behavior_tree` (small read + edit slice ships in this fork) —
  read-only `inspect` plus the two edit ops `create_behavior_tree`
  (asset creation with optional linked Blackboard) and
  `add_root_composite` (Selector / Sequence / SimpleParallel root
  assignment). Open follow-ons: append child task / composite to
  a chosen parent, insert a decorator on a chosen child slot,
  append a service on a composite, full Blackboard key edits
  (add / remove / rename / type change / sync flag toggle), and an
  AI Controller / EQS slice with the same read-only structure for
  the Run Behavior Tree -> Make Decision flow. AIModule is already
  pulled in.
- `gas_edit` (small read + small edit slice ships in this fork) —
  read-only `inspect` plus the full edit slice landed in later
  passes: `create_gameplay_ability`, `create_gameplay_effect`
  (with the duration-policy / duration-magnitude shortcut),
  `set_gameplay_tags`, `add_modifier`, `remove_modifier_at`,
  `set_modifier_magnitude` (full FGameplayEffectModifierMagnitude
  surface: scalable_float / attribute_based / set_by_caller /
  custom_calculation_class), `set_attribute_default`,
  `set_ability_cost`, `set_ability_cooldown`, `create_cue_notify`,
  and `set_ability_cue_tag` (graph-side authoring).
  Open follow-ons: per-tag dev-comment surface beyond the
  registry's, GE Executions array authoring beyond the modifier
  surface, and the AbilityTask side that the existing surface
  does not cover.
- `tag_registry_edit` (small variant ships in this fork) — `add_tag`,
  `remove_tag`, `list_tags` through `IGameplayTagsEditorModule`. Open
  follow-ons: rename through `RenameTagInINI`, restricted-tag source
  creation through `AddNewGameplayTagSource`, and per-tag-asset usage
  search over the AssetRegistry for "where is this tag referenced".

## Landscape & foliage (large each)

- `landscape_inspect` (small read-only variant ships in this fork) —
  per-actor dump for every `ALandscape`: section-grid configuration,
  proxy material, registered layers, world-space bounds, deduplicated
  heightmap / weightmap texture sets, optional per-component records.
  Open follow-ons: per-edit-layer dump (each ULandscapeEditLayer's
  Guid + name + visibility + paint / sculpt flags), spline component
  dump for actors implementing ILandscapeSplineInterface, and a
  height / weight sample at a chosen world location through
  `ULandscapeInfo::GetLayerWeightAtLocation`.
- `landscape_edit` (small variant retry ships in this fork) —
  multi-op tool keyed by `op` covering `set_landscape_material`
  (writes the proxy's master `LandscapeMaterial` UPROPERTY through a
  PostEditChangeProperty rebroadcast that mirrors
  `EditorSetLandscapeMaterial`) and `import_heightmap_png` (decodes
  a 16-bit grayscale PNG and writes through
  `FLandscapeEditDataInterface::SetHeightData`). Open follow-ons:
  the per-edit-layer write side (`SetHeightDataForLayer`), the
  paint-layer-by-stroke surface, weightmap import per layer
  (`SetAlphaData`), the sculpt brush primitives, and the heightmap
  export counterpart.
- `foliage_inspect` (small read-only variant ships in this fork) —
  per-IFA dump with foliage type list (mesh / actor source path,
  density, radius, per-axis scale interval, instance counts) plus an
  optional sampled world-location reservoir per type. Open follow-
  ons: per-instance base-component lookup (which static mesh /
  landscape component does each instance attach to), procedural
  foliage spawner + procedural foliage volume readout, and a
  spatial-bound query that returns "every foliage instance whose
  origin is inside this FBox / FSphere".
- `foliage_edit` (small variant ships in this fork) — multi-op
  tool keyed by `op` covering `add_foliage_type` (registers a
  UFoliageType on the level's `AInstancedFoliageActor`, spawning
  the IFA when missing through
  `AInstancedFoliageActor::GetInstancedFoliageActorForLevel(Level, /*bCreateIfNone=*/true)`)
  and `set_foliage_density` (Density / DensityAdjustmentFactor /
  Radius / per-axis ScaleX-Y-Z FFloatInterval writes on a
  UFoliageType asset). Open follow-ons: place N instances at
  caller-chosen world locations through `FFoliageInfo::AddInstance`,
  remove instances by index or spatial bound, paint instances by
  spawning under a chosen base component, and procedural foliage
  spawner authoring.

## Cinematics & audio (large each)

- `sequencer_edit` (small read + edit slice ships in this fork) —
  read-only `inspect` plus the edit ops `create_level_sequence`
  (NewObject + `ULevelSequence::Initialize` for a fresh empty
  asset), `add_possessable` (`UMovieScene::AddPossessable` +
  `BindPossessableObject` for actor-world binding), `add_track`
  (UMovieSceneTrack subclass attachment, master and binding-
  scoped both supported), `add_section` (UMovieSceneTrack::
  CreateNewSection + AddSection with an explicit frame range),
  `move_section` (UMovieSceneSection::SetRange with a captured
  previous-range snapshot), and `add_audio_track` (declarative
  one-call wrapper: resolves a USoundBase by path / short name,
  finds or creates a UMovieSceneAudioTrack scoped to a binding
  or master, routes through `UMovieSceneAudioTrack::AddNewSound`
  to land a UMovieSceneAudioSection, sets the section range
  from start_frame plus duration_frames or the sound's
  intrinsic length via `USoundBase::GetDuration` converted
  through the MovieScene's tick resolution; an optional
  `force_new_track=true` bypasses the reuse-existing path).
  Open follow-ons: per-track row dump for multi-row tracks
  (UMovieSceneNameableTrack subclasses), per-channel key dump
  for the standard transform / float / bool / enum tracks
  through `UMovieSceneSection::GetChannelProxy`, asset-bound
  resolution for possessables (which actor in the current
  editor world is bound to a possessable GUID), spawnable
  creation, camera-cut creation as a single-call wrapper, and
  per-section audio-asset editing (start offset / pitch / volume
  channel writes through the section's MovieScene float
  channels).
- `metasound_edit` (small variant ships in this fork) — two ops on
  MetaSound assets keyed by `op`: `create_metasound_source`
  (UMetaSoundSource at a `/Game/...` path; optional `output_format`
  / `sample_rate` / `block_rate` overrides land on the asset before
  InitAsset wires the document) and `create_metasound_patch`
  (UMetaSoundPatch at a `/Game/...` path). Both ops route through
  `UMetaSoundEditorSubsystem::GetChecked()`'s public `InitAsset` +
  `RegisterGraphWithFrontend`. Open follow-ons: the graph-authoring
  surface (add / connect / disconnect nodes through the
  `UMetaSoundBuilder` API), member defaults, preset support, and a
  `metasound_inspect` read-only counterpart that walks the asset's
  document and emits the node graph as a JSON edge list.
- `sound_asset_edit` (small variant ships in this fork) — three ops
  on USoundCue assets: `create_sound_cue` (with optional initial
  USoundNodeWavePlayer wired to a chosen USoundWave),
  `add_sound_node_wave_player` (USoundCue::ConstructSoundNode +
  SetSoundWave + optional FirstNode rebind), and `set_attenuation`
  (USoundCue::AttenuationSettings rebind on the USoundBase shape).
  Open follow-ons: the heavier graph-authoring branches (random /
  sequence / mixer / modulator composites, attenuation-node
  insertion with FAttenuationSettings overrides, delay / loop /
  branch / concatenator composites, distance-crossfade authoring),
  USoundClass + USoundMix asset edits, and a `sound_asset_inspect`
  read-only counterpart that walks the cue's node graph and emits
  it as a JSON tree.

## Procedural (large)

- `pcg_graph_edit` (small read + edit slice ships in this fork) —
  inspect a `UPCGGraph` asset, plus four edit ops:
  `add_node` / `connect_pins` / `remove_node` / `set_node_settings`.
  The walk covers `UPCGGraph::GetNodes` plus per-node pins (with
  type / status / usage tokens off `FPCGPinProperties` and
  `FPCGDataTypeIdentifier::ToString`), the graph's exposed input /
  output pin surface, and a flat edge list with `from` / `to` edge
  endpoints (PCG's UPCGEdge stores upstream as `InputPin` and
  downstream as `OutputPin`; we normalise to the editor convention).
  `set_node_settings` applies a flat property dict to the target
  node's `UPCGSettings` subobject through
  `FProperty::ImportText_InContainer`, runs
  `Settings->PostEditChangeProperty`, and broadcasts
  `Node->OnNodeChangedDelegate.Broadcast(Node, EPCGChangeType::Settings)`
  so any open PCG editor refreshes. Open follow-ons: pin-rename
  op, graph parameter readout (`UPCGGraph::UserParameters`
  instanced property bag), and a follow-on read of the per-node
  `bIsDisabled` / `bDebug` editor flags.

## Data assets (small to medium each, on the asset_factory umbrella)

- `asset_factory` (DataAsset) — shipped in this fork.
- `asset_factory` (Enhanced Input bundle) — shipped in this fork. One
  call accepts a list of action specs (name + value_type) and a list
  of mapping rows (action + key + optional negate / swizzle) and
  produces an IMC plus N UInputActions. The dedicated `bp_input` tool
  still covers per-asset creation and the action-event-node wiring
  side; the bundle variant is the right shortcut when the caller can
  describe the entire input layer declaratively.

## Editor & diagnostics (medium each)

- `editor_log` — the on-disk-log tail variant ships in this fork. A future
  pass could attach a buffering FOutputDevice to GLog so the tool can read
  log entries that arrived after the editor started without re-parsing the
  full file. We can also expose the in-editor SOutputLog widget filter
  helpers if a hook is added to the OutputLog module.
- `performance_audit` (small variant ships in this fork) — frame /
  thread / GPU snapshot read off the active editor viewport's
  `FStatUnitData` ring plus the cycle-counter globals. Open
  follow-ons: deep-dive captures (`stat startfile` / `stat
  stopfile`), Insights `.utrace` capture wiring, the FPSChart
  histogram surface, and per-emitter / per-actor breakdowns out of
  the `STAT GROUP` snapshot the engine already maintains for
  Engine / Memory / RHI.
- `cpp_source` (small read-only variant ships in this fork) — read
  header / cpp pair through `FSourceCodeNavigation::FindClassHeaderPath`
  / `FindClassSourcePath`, or by direct .h / .cpp disk path with
  sibling-inference. Open follow-ons: write side (mutate header /
  cpp text + run `ICompilerResultsLog` Live Coding patch) and
  per-class symbol dump (functions + properties from the
  reflection database with their declared file + line through
  `FProperty::GetMetaData("MetaSource")`).

## Runtime verification (large each)

- `pie_test_bp` (small variant ships in this fork) — Blueprint-side
  assertion harness with one assertion kind that answers statically
  against a Blueprint's CDO: `default_value_equals` (target = a
  UPROPERTY FName on the Blueprint's generated class; expected = a
  JSON literal that we canonicalise through the property's
  `ImportText` -> `ExportText` round-trip and compare against the
  CDO's `ExportText` output). Open follow-ons: `function_returns`
  (invoke a Blueprint function and assert on its return value;
  needs PIE), `event_fired` (custom-event broadcast assertion;
  needs PIE), `component_default_equals` (one level deeper to a
  named UActorComponent's UPROPERTY), and a `function_returns_pure`
  variant that runs the function on the CDO directly when the
  function is marked pure with no side effects.
- `pie_test_scene` (small variant ships in this fork) — scene-state
  assertion harness with seven assertion kinds that all answer
  statically against the editor world: `actor_exists` for actor-name
  presence, `actor_at_location` for distance-bounded location
  matches, `actor_overlapping_tag` for tag-list membership on
  `AActor::Tags`, `var_equals` for canonicalized UPROPERTY
  value matches through `FProperty::ImportText` /
  `ExportText_Direct`, `actor_has_class` for actor-class /
  subclass identity, `actor_tag_count` for exact `Tags.Num()`
  matches, and `level_actor_count` for `GetAllActorsOfClass`-style
  per-class population counts (subclass-inclusive through the
  typed `TActorIterator<AActor>` overload). The class-token
  resolver shared by `actor_has_class` and `level_actor_count`
  accepts a full `/Script/Module.ClassName` path, a `/Game/...`
  Blueprint class path auto-suffixed with `_C`, or a bare short
  class name probed with A / U prefix variants plus a
  `/Script/Engine.*` fallback. Open follow-ons: per-assertion
  timeout for kinds that need a running PIE world, an over-PIE
  harness option for callers who want the same surface but inside
  a running PIE world, and a `component_var_equals` kind that
  walks one level deeper to a named UActorComponent's UPROPERTY.

## Execution (large each)

- `python_execution` (small variant ships in this fork) — broader hosted
  scope still pending: persistent shared interpreter scope across calls,
  output streaming for long-running scripts, and richer typed result
  marshalling beyond the current stdout / stderr / repr capture.
- `unreal_api` (small variant ships in this fork) — `describe` /
  `find_property` / `find_function` / `list_classes` /
  `find_in_subclasses` / `class_diff` ops keyed off the live UE5
  reflection database. `list_classes` walks
  `TObjectIterator<UClass>` (or descendants of a chosen parent
  class via `GetDerivedClasses(Parent, Out, /*bRecursive=*/true)`)
  with substring + flag filters. `find_in_subclasses` walks each
  descendant's own declarations through
  `EFieldIteratorFlags::ExcludeSuper` so the result tells the
  caller which descendant ADDS a member rather than which
  inherits. `class_diff` compares two classes' property +
  function surfaces and surfaces `added` / `removed` /
  `changed` arrays per side. Open follow-ons: an offline JSON
  dump path that pre-computes the full reflection surface for
  the project's enabled modules, and a `find_in_class_chain`
  op that walks the parent chain (rather than the descendant
  set) for "where in the inheritance tree did this member get
  declared".
- `skills` (small variant ships in this fork) — fetch on-demand
  workflow docs over a curated `Python/skills/*.md` index. Three
  ops keyed by `op`: `get` (default; returns one skill body),
  `list` (returns every entry's slug + first H1), `search`
  (case-insensitive substring filter). Pre-baked entries:
  `replication`, `enhanced-input`, `gameplay-tags`,
  `crafting-data-tables`. Open follow-ons: more pre-baked entries
  (multiplayer-replication-graph, materials-mvvm, perf-budget),
  per-skill front-matter for difficulty / area tags, and a
  cross-skill `related` array so callers chain related docs in one
  walk.

## Suggested next-pass shortlist for a single-player game project

The latest pass shipped three deepening edit slices:
`landscape_edit` adds the `set_height_box` op (uniform-height
write across an axis-aligned bounded region of the landscape's
component grid through `FLandscapeEditDataInterface::SetHeightData`),
`niagara_edit` adds `set_emitter_local_parameter` (writes through
the per-script `RapidIterationParameters` store via the
documented `FNiagaraParameterStore::SetParameterData` byte-buffer
overload), and `sequencer_edit` adds `move_section` (resolves a
section by track + index, writes a fresh `TRange<FFrameNumber>`
through `UMovieSceneSection::SetRange`). The
`chaos_edit fracture_box` slice (PlanarCut entry point through
`CutMultipleWithPlanarCells` plus `FPlanarCells(FBox)`) was the
fourth target on this run but stayed on the BACKLOG: the
`PlanarCut` plugin is `EnabledByDefault: false` and the cut
surface needs `FInternalSurfaceMaterials` authoring plus a
PostFracture cleanup walk we did not want to land in a small
slice. The next set should pick up:

1. `landscape_edit` edit slice — per-edit-layer write
   (`SetHeightDataForLayer`), the paint-layer-by-stroke surface
   through the same FLandscapeEditDataInterface (`SetAlphaData`
   per layer), the sculpt brush primitives that `LandscapeEdMode`
   wraps, and a heightmap export counterpart
   (`GetHeightDataTempl` -> 16-bit grayscale PNG). The
   `set_height_box` op shipped in the most recent pass.
2. `niagara_script_edit` edit slice — Module / DynamicInput script
   creation with a typed `inputs` / `outputs` list off
   `FNiagaraVariable`, plus a `compile` op that drives the script's
   own `RequestCompile`.
3. `animation_graph_edit` follow-on edit ops — per-transition
   crossfade / blend mode writes, per-state property writes
   (always-reset / conduit, state-entered / state-left notify),
   AnimGraph node add / connect inside a state's `BoundGraph`
   beyond the player, and a `link_anim_graph_by_tag` op that writes
   the LinkedAnimGraph slot for a chosen tag. The `add_state` /
   `add_transition` / `set_state_animation` ops shipped in earlier
   passes.
4. `ik_rig_edit` edit slice — rebind preview mesh, append /
   remove retarget chain through `AddRetargetChain`, rename
   retarget root, override goal transform, and mutate per-solver
   bone settings via `SetBoneSettings`. Pairs with the
   already-shipped `ik_retarget` read side and the IKRig dep we
   already pulled in. The `add_solver` / `remove_solver_at` /
   `set_solver_settings` ops shipped in earlier passes.
5. `chaos_edit` edit slice — `fracture_box` through the
   `PlanarCut` module's `CutMultipleWithPlanarCells` plus
   `FPlanarCells(FBox)`, with the `PlanarCut` plugin enabled in
   the `.uplugin` manifest, plus the heavier dataflow driver
   under `DataflowInstance` and the per-cluster damage threshold
   override side that `set_simulation_settings` does not cover.
6. `niagara_edit` next-cut — emitter add through
   `FNiagaraEditorUtilities::AddEmitterToSystem`, system-side
   parameter store write via `UNiagaraSystem::GetExposedParameters()`
   (the user-redirection store on the system), per-emitter
   sim-target / determinism flag writes, module add through the
   per-emitter spawn / update script source, and a
   `request_compile` op that drives `RequestCompile` after the
   writes. The `set_emitter_local_parameter` op shipped in the
   most recent pass.
7. `ik_retarget` follow-on edit ops — append op / remove op (the
   polymorphic `FInstancedStruct` array on the asset), set chain
   mapping pair on a chosen op, per-pose bone-rotation-offset
   override, and profile management through
   `UIKRetargeter::GetProfileByName`.
8. `sound_asset_edit` heavier ops — composite-node insertion
   (random / sequence / mixer / modulator / delay / loop / branch /
   concatenator), attenuation-node insertion with FAttenuationSettings
   overrides, and distance-crossfade authoring. Plus a
   `sound_asset_inspect` read-only counterpart that walks the cue's
   node graph and emits it as a JSON tree paired with the existing
   create surface.
9. `pcg_graph_edit` follow-ons — pin-rename op, graph parameter
   readout (`UPCGGraph::UserParameters` instanced property bag),
   and a follow-on read of the per-node `bIsDisabled` / `bDebug`
   editor flags. The `add_node` / `connect_pins` / `remove_node` /
   `set_node_settings` ops shipped in the most recent pass.
10. `unreal_api` follow-ons — an offline JSON dump path that
    pre-computes the full reflection surface for the project's
    enabled modules, and a `find_in_class_chain` op that walks
    the parent chain (rather than the descendant set) for "where
    in the inheritance tree did this member get declared". The
    `list_classes` / `find_in_subclasses` / `class_diff` ops
    shipped in the most recent pass.
11. `gas_edit` remaining ops — rebind cost / cooldown classes on a
    UGameplayAbility through the CDO, and a GameplayCue authoring
    slice (cue-tag set + level range + magnitude attribute).
12. `behavior_tree` heavier edit ops — Blackboard key type-change
    op and parent-Blackboard re-bind. The `rename_blackboard_key`
    and `set_root_decorator` ops shipped in the most recent pass.
13. `sequencer_edit` remaining edit ops — spawnable creation and
    per-row edits. The `move_section` op shipped in the most
    recent pass.
14. `metasound_edit` follow-ons — member-default writes through the
    builder, and a `metasound_inspect` read-only counterpart that
    walks the asset's document and emits the node graph as a JSON
    edge list.
15. `pie_test_bp` heavier kinds — `function_returns` (invoke a
    Blueprint function in PIE and assert on its return value),
    `event_fired` (custom-event broadcast assertion in PIE), and
    `component_default_equals` (one level deeper to a named
    UActorComponent's UPROPERTY).
16. `performance_audit` deep-dive captures — `stat startfile` /
    `stat stopfile` for an offline `.ue4stats` chart, the FPSChart
    histogram surface, and an Insights `.utrace` capture wrapper.
17. `skills` index expansion — pre-bake more workflow docs
    (multiplayer-replication-graph, materials-mvvm, perf-budget,
    landscape-painting, pcg-graph-essentials), per-skill front-
    matter for difficulty / area tags, and a cross-skill
    `related` array so callers can chain related docs in one
    walk.

`python_execution` still covers any operation we have not wrapped
natively; prefer wrapping the high-frequency calls as dedicated tools
so the agent does not need to author Python every time.
