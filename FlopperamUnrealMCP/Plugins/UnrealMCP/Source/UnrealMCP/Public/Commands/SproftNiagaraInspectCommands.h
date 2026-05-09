#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: niagara_inspect (read-only)
 *
 * Read-only structured dump of a UNiagaraSystem asset. Pairs with
 * `material_inspect` for the VFX side: lists emitters, per-emitter
 * scripts grouped by execution stage (system / emitter / particle
 * spawn / particle update / event handler / simulation stage / GPU
 * compute), the user-exposed parameter store entries, the renderer
 * properties chain, and a few sim-target / determinism flags.
 *
 * Operation: single op (`inspect`, default).
 *
 * Required input:
 *   - `system`: short asset name or full `/Game/...` Niagara System
 *     path. Resolves through `UEditorAssetLibrary::LoadAsset`.
 *
 * Optional inputs:
 *   - `include_event_handlers`: default true.
 *   - `include_simulation_stages`: default true.
 *   - `include_renderers`: default true.
 *   - `include_parameters`: default true.
 *
 * Returns a structured payload with:
 *   - `name`, `path`, `class`.
 *   - `system_spawn_script`, `system_update_script`: package paths of
 *     the per-system Niagara scripts.
 *   - `emitters`: array of per-emitter dicts. Each emitter dict
 *     reports `name`, `enabled`, `sim_target` (CPU / GPU), `local_space`,
 *     `determinism`, the per-stage script list (each entry with
 *     `usage`, `script_name`, `script_path`), the renderer list (each
 *     with class), the simulation-stage list (each with class), and
 *     event-handler entries (each with their source-event-name and
 *     execution-mode plus the wrapped script path).
 *   - `parameters`: array of `{name, type, type_path, kind}` entries
 *     pulled from `GetExposedParameters().ReadParameterVariables()`.
 *     `kind` reports primitive / data_interface / object so callers
 *     can branch without re-checking the type.
 *
 * Read-only. We do not mutate the asset and we do not save anything.
 *
 * Clean-room implementation derived from the public UE5 Niagara API:
 *   - UNiagaraSystem::GetEmitterHandles / GetSystemSpawnScript /
 *     GetSystemUpdateScript / GetExposedParameters.
 *   - FNiagaraEmitterHandle::GetName / GetIsEnabled / GetEmitterData.
 *   - FVersionedNiagaraEmitterData::SpawnScriptProps / UpdateScriptProps /
 *     EmitterSpawnScriptProps / EmitterUpdateScriptProps /
 *     EventHandlerScriptProps / SimulationStages / RendererProperties /
 *     SimTarget / bLocalSpace / bDeterminism.
 *   - FNiagaraParameterStore::ReadParameterVariables for the parameter
 *     enumeration.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftNiagaraInspectCommands
{
public:
    FSproftNiagaraInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleNiagaraInspect(const TSharedPtr<FJsonObject>& Params);
};
