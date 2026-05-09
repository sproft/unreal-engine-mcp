#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: niagara_edit (system + emitter authoring)
 *
 * Two ops keyed by `op`:
 *   - `create_niagara_system` (default, ultra-minimum cut): NewObject's
 *     a `UNiagaraSystem` at a `/Game/...` path through
 *     `UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/false)`.
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
 * Returns asset metadata plus a `saved` flag and op-specific fields
 * (the new handle's display name + GUID for the emitter op, the
 * "no emitter" warning for the create op).
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
};
