#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: niagara_script_edit (read-only first slice)
 *
 * Inspect a `UNiagaraScript` asset. Pairs with `niagara_inspect`
 * (system / emitter side) and `material_inspect` (renderer side).
 * Walks the asset's public API plus the cached VM compile data and
 * reports usage, asset version GUID, the input / output / attribute
 * parameter sets, the compile status, the cached byte-code length,
 * the GPU shader parameter count, and aggregate counts.
 *
 * Operation: single op (`inspect`, default). Edit-side ops (build a
 * new module from inputs, mutate per-script settings) stay on the
 * BACKLOG.
 *
 * Required input:
 *   - `script`: short asset name or `/Game/...` UNiagaraScript path.
 *
 * Optional inputs:
 *   - `include_inputs`:        default true. When false the per-input
 *                              parameter dump is omitted; the count
 *                              still rides along.
 *   - `include_outputs`:       default true. Same shape for the
 *                              attributes-written / output side.
 *   - `include_attributes`:    default true. Toggle for the runtime
 *                              attribute list (`Attributes` array on
 *                              the cached VM data; populated for
 *                              particle scripts).
 *   - `include_data_interfaces`: default true. Toggle for the
 *                              data-interface compile-info list.
 *   - `include_compile_data`:  default true. Toggle for the
 *                              `compile_data` block (last compile
 *                              status, byte-code length, num temp
 *                              registers, num user pointers,
 *                              parameter / internal-parameter
 *                              counts).
 *   - `max_inputs` / `max_outputs` / `max_attributes` / `max_data_interfaces`:
 *                              caps for the per-list walks. Default
 *                              512 each.
 *
 * Returns a structured payload:
 *   - `name`, `path`, `class`.
 *   - `usage` (function / module / dynamic_input / particle_spawn /
 *     particle_update / particle_event / particle_simulation_stage /
 *     particle_gpu_compute / emitter_spawn / emitter_update /
 *     system_spawn / system_update / unknown).
 *   - `usage_id`: the FGuid string for sub-versioned scripts.
 *   - `inputs`:  array of `{name, type, type_path, kind}` rows for
 *     the cached `Parameters` set (the script's input parameters).
 *   - `outputs`: array of `{name, type, type_path, kind}` rows for
 *     the script's `AttributesWritten` set (the explicitly written
 *     attributes), plus a count.
 *   - `attributes`: array of `{name, type, type_path, kind}` rows
 *     for the runtime `Attributes` set (attribute layout for the
 *     particle data).
 *   - `data_interfaces`: array of `{name, type_path, registered_function_count,
 *     user_ptr_idx}` for the per-script data interface compile info.
 *   - `compile_data`: dict with `last_compile_status` token,
 *     `byte_code_length`, `num_temp_registers`, `num_user_ptrs`,
 *     `parameter_count`, `internal_parameter_count`,
 *     `gpu_shader_parameter_count`, `has_byte_code` flag.
 *   - Aggregate counts for every truncated list.
 *
 * Read-only. We do not mutate the asset.
 *
 * Clean-room implementation derived from the public UE5 Niagara API:
 *   - `UNiagaraScript::GetUsage` / `GetUsageId` / `GetVMExecutableData`.
 *   - `FNiagaraVMExecutableData::Parameters` / `InternalParameters` /
 *     `Attributes` / `AttributesWritten` / `DataInterfaceInfo` /
 *     `LastCompileStatus` / `ByteCode` / `NumTempRegisters` /
 *     `NumUserPtrs` / `ShaderScriptParametersMetadata`.
 *   - `FNiagaraParameterStore::ReadParameterVariables` for
 *     name + type enumeration on parameters.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftNiagaraScriptEditCommands
{
public:
    FSproftNiagaraScriptEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleNiagaraScriptInspect(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetModuleUsage(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddInputParameter(const TSharedPtr<FJsonObject>& Params);
};
