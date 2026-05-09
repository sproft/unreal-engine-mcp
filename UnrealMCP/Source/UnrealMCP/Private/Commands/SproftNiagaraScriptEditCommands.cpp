#include "Commands/SproftNiagaraScriptEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Modules/ModuleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "UObject/Class.h"

namespace
{
    UNiagaraScript* ResolveNiagaraScript(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UNiagaraScript>(UEditorAssetLibrary::LoadAsset(Input));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UNiagaraScript::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UNiagaraScript>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    /** Same usage map as the niagara_inspect implementation; kept private
     *  per file so the two readers do not have to share a translation
     *  helper. */
    const TCHAR* UsageToString(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::Function:                          return TEXT("function");
        case ENiagaraScriptUsage::Module:                            return TEXT("module");
        case ENiagaraScriptUsage::DynamicInput:                      return TEXT("dynamic_input");
        case ENiagaraScriptUsage::ParticleSpawnScript:               return TEXT("particle_spawn");
        case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated:   return TEXT("particle_spawn_interpolated");
        case ENiagaraScriptUsage::ParticleUpdateScript:              return TEXT("particle_update");
        case ENiagaraScriptUsage::ParticleEventScript:               return TEXT("particle_event");
        case ENiagaraScriptUsage::ParticleSimulationStageScript:     return TEXT("particle_simulation_stage");
        case ENiagaraScriptUsage::ParticleGPUComputeScript:          return TEXT("particle_gpu_compute");
        case ENiagaraScriptUsage::EmitterSpawnScript:                return TEXT("emitter_spawn");
        case ENiagaraScriptUsage::EmitterUpdateScript:               return TEXT("emitter_update");
        case ENiagaraScriptUsage::SystemSpawnScript:                 return TEXT("system_spawn");
        case ENiagaraScriptUsage::SystemUpdateScript:                return TEXT("system_update");
        default:                                                     return TEXT("unknown");
        }
    }

    const TCHAR* CompileStatusToString(ENiagaraScriptCompileStatus Status)
    {
        switch (Status)
        {
        case ENiagaraScriptCompileStatus::NCS_Unknown:                       return TEXT("unknown");
        case ENiagaraScriptCompileStatus::NCS_Dirty:                         return TEXT("dirty");
        case ENiagaraScriptCompileStatus::NCS_Error:                         return TEXT("error");
        case ENiagaraScriptCompileStatus::NCS_UpToDate:                      return TEXT("up_to_date");
        case ENiagaraScriptCompileStatus::NCS_BeingCreated:                  return TEXT("being_created");
        case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:          return TEXT("up_to_date_with_warnings");
        case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:   return TEXT("compute_up_to_date_with_warnings");
        default:                                                             return TEXT("unknown");
        }
    }

    /** Categorise a Niagara variable as primitive / data_interface / object
     *  so a downstream consumer does not need to peek at the type def. */
    const TCHAR* VariableKind(const FNiagaraVariableBase& Var)
    {
        if (Var.IsDataInterface()) { return TEXT("data_interface"); }
        if (Var.IsUObject())       { return TEXT("object"); }
        return TEXT("primitive");
    }

    /** Render an FNiagaraVariableBase as `{name, type, type_path, kind}`. */
    TSharedPtr<FJsonObject> VariableRow(const FNiagaraVariableBase& Var)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("name"), Var.GetName().ToString());
        const FNiagaraTypeDefinition& TypeDef = Var.GetType();
        Out->SetStringField(TEXT("type"), TypeDef.GetName());
        if (TypeDef.GetClass())
        {
            Out->SetStringField(TEXT("type_path"), TypeDef.GetClass()->GetPathName());
        }
        else if (TypeDef.GetScriptStruct())
        {
            Out->SetStringField(TEXT("type_path"), TypeDef.GetScriptStruct()->GetPathName());
        }
        else if (TypeDef.GetEnum())
        {
            Out->SetStringField(TEXT("type_path"), TypeDef.GetEnum()->GetPathName());
        }
        Out->SetStringField(TEXT("kind"), VariableKind(Var));
        return Out;
    }
}

FSproftNiagaraScriptEditCommands::FSproftNiagaraScriptEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftNiagaraScriptEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("niagara_script_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown niagara_script_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }
    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op.Equals(TEXT("inspect"), ESearchCase::IgnoreCase))
    {
        return HandleNiagaraScriptInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("niagara_script_edit: unsupported op '%s'. Only 'inspect' is shipped on this slice"), *Op));
}

TSharedPtr<FJsonObject> FSproftNiagaraScriptEditCommands::HandleNiagaraScriptInspect(const TSharedPtr<FJsonObject>& Params)
{
    FString ScriptParam;
    if (!Params->TryGetStringField(TEXT("script"), ScriptParam)
        && !Params->TryGetStringField(TEXT("path"), ScriptParam)
        && !Params->TryGetStringField(TEXT("asset"), ScriptParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'script' parameter"));
    }
    UNiagaraScript* Script = ResolveNiagaraScript(ScriptParam);
    if (!Script)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UNiagaraScript '%s'"), *ScriptParam));
    }

    bool bIncludeInputs = true;
    bool bIncludeOutputs = true;
    bool bIncludeAttributes = true;
    bool bIncludeDataInterfaces = true;
    bool bIncludeCompileData = true;
    int32 MaxInputs = 512;
    int32 MaxOutputs = 512;
    int32 MaxAttributes = 512;
    int32 MaxDataInterfaces = 512;
    Params->TryGetBoolField(TEXT("include_inputs"), bIncludeInputs);
    Params->TryGetBoolField(TEXT("include_outputs"), bIncludeOutputs);
    Params->TryGetBoolField(TEXT("include_attributes"), bIncludeAttributes);
    Params->TryGetBoolField(TEXT("include_data_interfaces"), bIncludeDataInterfaces);
    Params->TryGetBoolField(TEXT("include_compile_data"), bIncludeCompileData);
    Params->TryGetNumberField(TEXT("max_inputs"), MaxInputs);
    Params->TryGetNumberField(TEXT("max_outputs"), MaxOutputs);
    Params->TryGetNumberField(TEXT("max_attributes"), MaxAttributes);
    Params->TryGetNumberField(TEXT("max_data_interfaces"), MaxDataInterfaces);

    auto ClampMin = [](int32& Out) { if (Out < 1) Out = 1; };
    ClampMin(MaxInputs); ClampMin(MaxOutputs); ClampMin(MaxAttributes); ClampMin(MaxDataInterfaces);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("name"), Script->GetName());
    Result->SetStringField(TEXT("path"), Script->GetPathName());
    Result->SetStringField(TEXT("class"), Script->GetClass()->GetName());
    Result->SetStringField(TEXT("usage"), UsageToString(Script->GetUsage()));
    Result->SetStringField(TEXT("usage_id"), Script->GetUsageId().ToString());

    const FNiagaraVMExecutableData& VMData = Script->GetVMExecutableData();

    // Input parameters. WITH_EDITORONLY_DATA on the underlying field;
    // the editor-only walls are fine since the bridge is editor-only.
#if WITH_EDITORONLY_DATA
    if (bIncludeInputs)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        const int32 Total = VMData.Parameters.Parameters.Num();
        const int32 Emit  = FMath::Min(Total, MaxInputs);
        Arr.Reserve(Emit);
        for (int32 I = 0; I < Emit; ++I)
        {
            Arr.Add(MakeShared<FJsonValueObject>(VariableRow(VMData.Parameters.Parameters[I])));
        }
        Result->SetArrayField(TEXT("inputs"), Arr);
        Result->SetNumberField(TEXT("input_count"), Emit);
        Result->SetNumberField(TEXT("input_count_total"), Total);
        Result->SetBoolField(TEXT("inputs_truncated"), Total > Emit);
    }
    else
    {
        Result->SetNumberField(TEXT("input_count_total"), VMData.Parameters.Parameters.Num());
    }

    // Output / attributes-written set. Editor-only.
    if (bIncludeOutputs)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        const int32 Total = VMData.AttributesWritten.Num();
        const int32 Emit  = FMath::Min(Total, MaxOutputs);
        Arr.Reserve(Emit);
        for (int32 I = 0; I < Emit; ++I)
        {
            Arr.Add(MakeShared<FJsonValueObject>(VariableRow(VMData.AttributesWritten[I])));
        }
        Result->SetArrayField(TEXT("outputs"), Arr);
        Result->SetNumberField(TEXT("output_count"), Emit);
        Result->SetNumberField(TEXT("output_count_total"), Total);
        Result->SetBoolField(TEXT("outputs_truncated"), Total > Emit);
    }
    else
    {
        Result->SetNumberField(TEXT("output_count_total"), VMData.AttributesWritten.Num());
    }
#else
    Result->SetBoolField(TEXT("inputs_available"), false);
    Result->SetBoolField(TEXT("outputs_available"), false);
#endif

    // Runtime attribute layout. This array is non-editor-only and
    // populated for particle scripts with the attribute table the VM
    // writes against.
    if (bIncludeAttributes)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        const int32 Total = VMData.Attributes.Num();
        const int32 Emit  = FMath::Min(Total, MaxAttributes);
        Arr.Reserve(Emit);
        for (int32 I = 0; I < Emit; ++I)
        {
            Arr.Add(MakeShared<FJsonValueObject>(VariableRow(VMData.Attributes[I])));
        }
        Result->SetArrayField(TEXT("attributes"), Arr);
        Result->SetNumberField(TEXT("attribute_count"), Emit);
        Result->SetNumberField(TEXT("attribute_count_total"), Total);
        Result->SetBoolField(TEXT("attributes_truncated"), Total > Emit);
    }
    else
    {
        Result->SetNumberField(TEXT("attribute_count_total"), VMData.Attributes.Num());
    }

    // Data interface compile info. The per-row registered-function
    // count is editor-only; we report 0 in cooked builds.
    if (bIncludeDataInterfaces)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        const int32 Total = VMData.DataInterfaceInfo.Num();
        const int32 Emit  = FMath::Min(Total, MaxDataInterfaces);
        Arr.Reserve(Emit);
        for (int32 I = 0; I < Emit; ++I)
        {
            const FNiagaraScriptDataInterfaceCompileInfo& DI = VMData.DataInterfaceInfo[I];
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), DI.Name.ToString());
            Row->SetStringField(TEXT("type"), DI.Type.GetName());
            if (UClass* DIClass = DI.Type.GetClass())
            {
                Row->SetStringField(TEXT("type_path"), DIClass->GetPathName());
            }
            Row->SetNumberField(TEXT("user_ptr_idx"), DI.UserPtrIdx);
            Row->SetBoolField(TEXT("is_placeholder"), DI.bIsPlaceholder);
            if (!DI.SourceEmitterName.IsEmpty())
            {
                Row->SetStringField(TEXT("source_emitter_name"), DI.SourceEmitterName);
            }
            if (!DI.RegisteredParameterMapRead.IsNone())
            {
                Row->SetStringField(TEXT("registered_parameter_map_read"), DI.RegisteredParameterMapRead.ToString());
            }
            if (!DI.RegisteredParameterMapWrite.IsNone())
            {
                Row->SetStringField(TEXT("registered_parameter_map_write"), DI.RegisteredParameterMapWrite.ToString());
            }
#if WITH_EDITORONLY_DATA
            Row->SetNumberField(TEXT("registered_function_count"), DI.RegisteredFunctions.Num());
#endif
            Arr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("data_interfaces"), Arr);
        Result->SetNumberField(TEXT("data_interface_count"), Emit);
        Result->SetNumberField(TEXT("data_interface_count_total"), Total);
        Result->SetBoolField(TEXT("data_interfaces_truncated"), Total > Emit);
    }
    else
    {
        Result->SetNumberField(TEXT("data_interface_count_total"), VMData.DataInterfaceInfo.Num());
    }

    if (bIncludeCompileData)
    {
        TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
        Compile->SetStringField(TEXT("last_compile_status"), CompileStatusToString(VMData.LastCompileStatus));
        Compile->SetNumberField(TEXT("byte_code_length"), VMData.ByteCode.GetLength());
        Compile->SetBoolField(TEXT("has_byte_code"), VMData.HasByteCode());
        Compile->SetNumberField(TEXT("num_temp_registers"), VMData.NumTempRegisters);
        Compile->SetNumberField(TEXT("num_user_ptrs"), VMData.NumUserPtrs);
#if WITH_EDITORONLY_DATA
        Compile->SetNumberField(TEXT("parameter_count"),          VMData.Parameters.Parameters.Num());
        Compile->SetNumberField(TEXT("internal_parameter_count"), VMData.InternalParameters.Parameters.Num());
        Compile->SetNumberField(TEXT("baked_rapid_iteration_count"), VMData.BakedRapidIterationParameters.Num());
        Compile->SetBoolField(TEXT("reads_attribute_data"), VMData.bReadsAttributeData);
#endif
        Compile->SetNumberField(TEXT("attribute_count"), VMData.Attributes.Num());
        Compile->SetNumberField(TEXT("compile_tag_count"), VMData.CompileTags.Num());
        Compile->SetNumberField(TEXT("simulation_stage_meta_count"), VMData.SimulationStageMetaData.Num());
        Compile->SetNumberField(TEXT("script_literal_byte_count"), VMData.ScriptLiterals.Num());
        Compile->SetNumberField(TEXT("read_data_set_count"),  VMData.ReadDataSets.Num());
        Compile->SetNumberField(TEXT("write_data_set_count"), VMData.WriteDataSets.Num());

        // GPU shader parameters metadata sits next to the VM block; we
        // surface the array sizes so callers can answer "does this
        // script use GPU compute" without rerunning the compile.
        const FNiagaraShaderScriptParametersMetadata& ShaderMeta = VMData.ShaderScriptParametersMetadata;
        Compile->SetNumberField(TEXT("gpu_data_interface_param_count"), ShaderMeta.DataInterfaceParamInfo.Num());
        Compile->SetNumberField(TEXT("gpu_loose_metadata_count"), ShaderMeta.LooseMetadataNames.Num());
        Compile->SetBoolField(TEXT("gpu_external_constants_interpolated"), ShaderMeta.bExternalConstantsInterpolated);
        Compile->SetNumberField(TEXT("gpu_external_constant_count"), ShaderMeta.ExternalConstants.Num());

        Result->SetObjectField(TEXT("compile_data"), Compile);
    }

    return Result;
}
