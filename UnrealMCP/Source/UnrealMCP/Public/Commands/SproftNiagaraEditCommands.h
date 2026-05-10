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
};
