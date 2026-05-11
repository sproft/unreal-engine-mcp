#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: niagara_edit (system + emitter authoring)
 *
 * Three ops keyed by `op`:
 *   - `create_niagara_system` (default, ultra-minimum cut): NewObject's
 *     a `UNiagaraSystem` at a `/Game/...` path through
 *     `UNiagaraSystemFactoryNew::InitializeSystem(System, bCreateDefaultNodes=false)`.
 *     No emitters, no parameter store, no module / sim-stage authoring.
 *     A system with no emitters opens with a "no emitter" warning in
 *     the asset's status banner; that is by design for the minimum-cut
 *     slice.
 *   - `add_emitter_from_asset`: resolves an existing system + an
 *     existing `UNiagaraEmitter` asset and routes through
 *     `UNiagaraSystem::AddEmitterHandle(SourceEmitter, EmitterName,
 *     Version)`. The version GUID defaults to the source emitter's
 *     `GetExposedVersion().VersionGuid` so the system pulls the
 *     emitter's currently active version. Optional `handle_name` lets
 *     the caller pick the system-side display name; without it we
 *     mirror the source emitter's `GetName()`.
 *   - `set_emitter_local_parameter`: resolves an existing system, walks
 *     `UNiagaraSystem::GetEmitterHandles()` for the matching emitter
 *     handle by name (case-insensitive against the handle's display
 *     name; fallback against the source emitter's `GetName()`),
 *     resolves the `Spawn` (default) or `Update` script through
 *     `FVersionedNiagaraEmitterData::SpawnScriptProps.Script` (or
 *     `UpdateScriptProps.Script`), and writes the parameter into the
 *     script's `RapidIterationParameters` store via the documented
 *     `FNiagaraParameterStore::SetParameterData(Buffer, Param,
 *     bAdd=true)` byte-buffer overload (the documented helper that
 *     supports arbitrary type sizes). The parameter is added if it
 *     does not exist on the store.
 *   - `add_module_to_stage`: resolves an existing system + emitter
 *     handle (same shape `set_emitter_local_parameter` uses) and an
 *     existing `UNiagaraScript` configured as a Module
 *     (Usage = `Module`), then walks the spawn / update script's
 *     source graph for the `UNiagaraNodeOutput` node whose `GetUsage()`
 *     matches the chosen stage and routes through
 *     `FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript,
 *     OutputNode)`. The new node is appended to the end of the stage by
 *     default; pass `target_index` to insert at a chosen index.
 *   - `request_compile`: resolves an existing `UNiagaraSystem` by
 *     `/Game/...` path or short name and routes through the public
 *     `UNiagaraSystem::RequestCompile(bForce)` (NIAGARA_API). The
 *     optional `force` flag picks the bForce argument (default
 *     false). After the call we mark the package dirty so the system
 *     saves with any post-compile fix-up state baked in.
 *   - `set_emitter_flag`: resolves an existing system + emitter
 *     handle (same shape `set_emitter_local_parameter` uses) and
 *     writes a boolean flag onto `FVersionedNiagaraEmitterData`.
 *     Supported flags: `bLocalSpace`, `bDeterminism`,
 *     `bInterpolatedSpawning`, `bRequiresPersistentIDs`. We resolve
 *     the field through the engine's reflection database against
 *     the `FVersionedNiagaraEmitterData` UScriptStruct so we do not
 *     depend on the private vs. public split that the bitfield
 *     wrappers ride on. The deprecated `bInterpolatedSpawning`
 *     field is preserved here for the documented contract; the
 *     engine's `InterpolatedSpawnMode` enum is the modern
 *     replacement, so the op also accepts the canonical enum form
 *     (`no_interpolation` / `run_update_script` /
 *     `run_update_script_with_interpolation`) and writes through
 *     `InterpolatedSpawnMode` when the flag token is
 *     `bInterpolatedSpawning` to keep modern emitters consistent.
 *   - `add_sim_stage`: resolves an existing system + emitter handle
 *     and routes through `UNiagaraEmitter::AddSimulationStage(stage,
 *     EmitterVersion)` (NIAGARA_API). The new
 *     `UNiagaraSimulationStageBase` subobject is outered to the
 *     UNiagaraEmitter; the default subclass is
 *     `UNiagaraSimulationStageGeneric`, callable through the
 *     `generic` short token plus full `/Script/Niagara.X` paths and
 *     bare class names. An optional `stage_name` lands on
 *     `SimulationStageName` before the add so the editor's stack
 *     viewmodel surfaces a designer-readable label.
 *   - `set_emitter_sim_target`: resolves an existing system +
 *     emitter handle (same shape `set_emitter_local_parameter`
 *     uses) and writes
 *     `FVersionedNiagaraEmitterData::SimTarget`. Tokens: `cpu` /
 *     `gpu` / `CPUSim` / `GPUComputeSim`. The op runs the
 *     PostEditChangeProperty fix-up against the SimTarget UPROPERTY
 *     so the cached renderer / GPU-script state refreshes.
 *   - `set_system_warmup`: writes the system-level warmup pair on
 *     a UNiagaraSystem. Inputs: optional `warmup_time` (seconds),
 *     optional `warmup_tick_count` (integer), optional
 *     `warmup_tick_delta` (seconds-per-tick override). When
 *     `warmup_time` is supplied we route through the public
 *     `UNiagaraSystem::SetWarmupTime` mutator (NIAGARA_API) so the
 *     engine's `ResolveWarmupTickCount` resolves the tick count from
 *     the current tick delta. When `warmup_tick_count` is supplied
 *     we write it directly through reflection (`WarmupTickCount`
 *     has no public setter) and harmonise `WarmupTime` so the
 *     editor's EditCondition (`WarmupTime > 0`) holds. When
 *     `warmup_tick_delta` is supplied we route through the public
 *     `UNiagaraSystem::SetWarmupTickDelta` mutator. Saves the system
 *     on success unless `save=false`.
 *   - `set_system_exposed_parameter`: resolves an existing
 *     UNiagaraSystem and writes a parameter into the system's
 *     `ExposedParameters` store
 *     (`FNiagaraUserRedirectionParameterStore`). The op reuses the
 *     same type-token resolver and byte-buffer packing the
 *     per-emitter variant uses, then routes through the documented
 *     `FNiagaraParameterStore::SetParameterData(Buffer, Param,
 *     bAdd=true)` NIAGARA_API overload. The
 *     `FNiagaraUserRedirectionParameterStore` accepts both the
 *     bare token (`MyFloat`) and the fully-qualified
 *     `User.MyFloat` form on the SetParameterData path because of
 *     the store's redirection map. Save the system on success.
 *   - `set_emitter_loop`: resolves an existing system + emitter
 *     handle (same shape `set_emitter_local_parameter` uses) and
 *     writes the per-version emitter data's `EmitterState` loop
 *     fields (`FNiagaraEmitterStateData::LoopBehavior` and
 *     `LoopCount`). `loop_mode` accepts `once` / `infinite` /
 *     `multiple` (case-insensitive); `loop_count` is required
 *     when `multiple` is selected (the editor caps at 1 when the
 *     mode is Multiple and the count was 0). The write routes
 *     through reflection against the
 *     `FVersionedNiagaraEmitterData::EmitterState` UPROPERTY plus
 *     the contained `FNiagaraEmitterStateData` struct's
 *     `LoopBehavior` / `LoopCount` fields so we never bind to a
 *     header-private alias. Save the system on success.
 *   - `set_emitter_property`: generic catch-all that lands a
 *     flat property dict on an emitter's
 *     `FVersionedNiagaraEmitterData` through reflection. Each
 *     entry that resolves to a UPROPERTY on the struct routes
 *     through `FProperty::ImportText_InContainer`; each entry
 *     that fails to resolve lands on the response's `skipped`
 *     array (with a reason and the attempted ImportText input)
 *     rather than aborting the whole write. Covers the long
 *     tail of per-emitter tunables we have not added named ops
 *     for (`bRequiresPersistentIDs`,
 *     `bUseExternalParameterStore`, `ParticleSpawnMode`,
 *     `InterpolatedSpawnMode`, etc.) without us shipping a
 *     named op for every individual field. JSON values are
 *     marshaled into the ImportText shape per-type (bool ->
 *     `true` / `false`, number -> int or float literal, string
 *     -> passthrough, array / object -> JSON re-serialisation so
 *     struct / array UPROPERTYs land too).
 *   - `set_renderer_property`: tweaks an existing renderer on an
 *     emitter's render stack rather than adding / replacing it.
 *     Resolves an existing system + emitter handle (same name
 *     resolution `set_emitter_local_parameter` uses), then walks
 *     the matched emitter's `FVersionedNiagaraEmitterData::GetRenderers()`
 *     array by `renderer_index` (default 0). Each entry of the
 *     flat `properties` dict lands through
 *     `FProperty::ImportText_InContainer` against the resolved
 *     `UNiagaraRendererProperties` subobject; failures collect on
 *     `skipped` rather than aborting so a typo in one field does
 *     not lose the rest. Distinct from `set_emitter_renderer`
 *     which adds or replaces a whole renderer subobject; this op
 *     edits the existing renderer in place. PostEditChange fires
 *     on the renderer after the writes so the cached system
 *     binding state regenerates and the editor's stack viewmodel
 *     refreshes. Save the system on success unless `save=false`.
 *   - `add_user_parameter`: declares a new user-tunable parameter
 *     on `UNiagaraSystem::GetExposedParameters()`, the `User.`
 *     namespace `FNiagaraUserRedirectionParameterStore`. Distinct
 *     from `set_system_exposed_parameter` (which writes an
 *     existing parameter's bytes through `SetParameterData`):
 *     this op routes through the documented
 *     `FNiagaraParameterStore::AddParameter` (NIAGARA_API) so a
 *     caller can declare a new system-tunable through MCP even
 *     when they have no value to land yet. `parameter_type`
 *     accepts the same scalar / vector / color / quat tokens the
 *     parameter ops already use plus `position` (LWC vector 3
 *     backed by `FNiagaraTypeDefinition::GetPositionDef`) and
 *     `transform` (the FTransform UScriptStruct wrapped as a
 *     Niagara struct type). Optional `value` (or `default`)
 *     lands a default through `SetParameterData` after the add
 *     so consumers see the canonical bytes rather than a zeroed
 *     slot; without a default the AddParameter path writes the
 *     type's zero. The `User.` namespace prefix is added
 *     automatically when missing per the redirection store's
 *     contract. Save the system on success.
 *
 * Inputs (create_niagara_system):
 *   - path: `/Game/...` package path. Required.
 *   - overwrite: replace an existing asset at the path. Default false.
 *   - save: save the new asset to disk. Default true.
 *
 * Inputs (add_emitter_from_asset):
 *   - system / system_path: required. `/Game/...` path or short name
 *     of an existing UNiagaraSystem.
 *   - emitter / emitter_path: required. `/Game/...` path or short
 *     name of an existing UNiagaraEmitter.
 *   - handle_name: optional. System-side display name for the new
 *     emitter handle. Defaults to the source emitter's `GetName()`.
 *   - save: save the system to disk. Default true.
 *
 * Inputs (set_emitter_local_parameter):
 *   - system / system_path: required. `/Game/...` path or short name
 *     of an existing UNiagaraSystem.
 *   - emitter / emitter_handle / handle_name: required. The
 *     system-side handle name (case-insensitive). Falls back to the
 *     source emitter `GetName()` when the handle has no override.
 *   - parameter_name / parameter / name: required. FName of the
 *     parameter to write (e.g. `Emitter.MyFloat` /
 *     `Module.MyValue` / a bare token).
 *   - parameter_type / type: required. Short token (`float` / `int`
 *     / `int32` / `bool` / `vec2` / `vec3` / `vec4` / `color` /
 *     `linear_color` / `quat`).
 *   - value: required. Either a JSON literal that
 *     `FProperty::ImportText` parses for the matching scalar shape
 *     (`float` / `int` / `bool`) or a JSON array for vector / color
 *     / quat shapes (length 2 / 3 / 4 to match the type).
 *   - script: optional. `spawn` (default) / `update`.
 *   - save: save the system to disk. Default true.
 *
 * Returns asset metadata plus a `saved` flag and op-specific fields
 * (the new handle's display name + GUID for the emitter op, the
 * "no emitter" warning for the create op, the resolved
 * parameter / type / value record for the set op).
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UNiagaraSystem` from
 *     `Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraSystem.h`,
 *     specifically the editor-only `AddEmitterHandle(UNiagaraEmitter&,
 *     FName, FGuid)` overload.
 *   - `UNiagaraEmitter::GetExposedVersion()` for the current
 *     version GUID.
 *   - `UNiagaraSystemFactoryNew::InitializeSystem(System, false)`
 *     for the create-only branch.
 *   - `FNiagaraEmitterHandle::GetEmitterData()` for the per-version
 *     emitter data, then `SpawnScriptProps.Script` /
 *     `UpdateScriptProps.Script` for the script handle.
 *   - `UNiagaraScript::RapidIterationParameters` for the per-script
 *     parameter store, plus `FNiagaraParameterStore::SetParameterData`
 *     and `FNiagaraTypeDefinition::Get*Def()` for the type
 *     resolution.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftNiagaraEditCommands
{
public:
    FSproftNiagaraEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleCreateSystem(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddEmitterFromAsset(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEmitterLocalParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddModuleToStage(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRequestCompile(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEmitterFlag(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSimStage(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEmitterSimTarget(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSystemExposedParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSystemWarmup(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEmitterLoop(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEmitterProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEmitterRenderer(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRendererProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddUserParameter(const TSharedPtr<FJsonObject>& Params);
};
