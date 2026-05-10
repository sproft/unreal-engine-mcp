#include "Commands/SproftNiagaraScriptEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Modules/ModuleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "UObject/Class.h"

#if WITH_EDITOR
#include "NiagaraScriptSourceBase.h"
#endif

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
    const TCHAR* NiagaraScriptEdit_UsageToString(ENiagaraScriptUsage Usage)
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
    const TCHAR* NiagaraScriptEdit_VariableKind(const FNiagaraVariableBase& Var)
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
        Out->SetStringField(TEXT("kind"), NiagaraScriptEdit_VariableKind(Var));
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
    if (Op.Equals(TEXT("set_module_usage"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("set_usage"), ESearchCase::IgnoreCase))
    {
        return HandleSetModuleUsage(Params);
    }
    if (Op.Equals(TEXT("add_input_parameter"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("add_parameter"), ESearchCase::IgnoreCase))
    {
        return HandleAddInputParameter(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("niagara_script_edit: unsupported op '%s'. Supported: inspect, set_module_usage, add_input_parameter"), *Op));
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
    Result->SetStringField(TEXT("usage"), NiagaraScriptEdit_UsageToString(Script->GetUsage()));
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

namespace
{
    /** Map a usage token to ENiagaraScriptUsage. The set is small on
     *  purpose; the runtime usage values gate compile shapes that
     *  `set_module_usage` is not the right tool for (e.g.
     *  ParticleSpawnScriptInterpolated). */
    bool NiagaraScriptEdit_ResolveUsage(const FString& Token, ENiagaraScriptUsage& OutUsage)
    {
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("module"))                   { OutUsage = ENiagaraScriptUsage::Module;                  return true; }
        if (Lower == TEXT("function"))                 { OutUsage = ENiagaraScriptUsage::Function;                return true; }
        if (Lower == TEXT("dynamic_input") || Lower == TEXT("dynamicinput"))
        {
            OutUsage = ENiagaraScriptUsage::DynamicInput;
            return true;
        }
        if (Lower == TEXT("emitter_spawn") || Lower == TEXT("emitterspawn"))
        {
            OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
            return true;
        }
        if (Lower == TEXT("emitter_update") || Lower == TEXT("emitterupdate"))
        {
            OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
            return true;
        }
        if (Lower == TEXT("particle_spawn") || Lower == TEXT("particlespawn"))
        {
            OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
            return true;
        }
        if (Lower == TEXT("particle_update") || Lower == TEXT("particleupdate"))
        {
            OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
            return true;
        }
        if (Lower == TEXT("system_spawn"))
        {
            OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
            return true;
        }
        if (Lower == TEXT("system_update"))
        {
            OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
            return true;
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftNiagaraScriptEditCommands::HandleSetModuleUsage(const TSharedPtr<FJsonObject>& Params)
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

    FString UsageToken;
    if (!Params->TryGetStringField(TEXT("usage"), UsageToken)
        && !Params->TryGetStringField(TEXT("module_usage"), UsageToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'usage' parameter (module / function / dynamic_input / emitter_spawn / emitter_update / particle_spawn / particle_update / system_spawn / system_update)"));
    }

    ENiagaraScriptUsage NewUsage = ENiagaraScriptUsage::Module;
    if (!NiagaraScriptEdit_ResolveUsage(UsageToken, NewUsage))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown Niagara usage token '%s'"), *UsageToken));
    }

    // The Usage member is public on UNiagaraScript (per
    // NiagaraScript.h: "cannot be private due to use of
    // GET_MEMBER_NAME_CHECKED"). Direct write is the documented
    // editor mutation path; PostEditChangeProperty broadcasts to
    // any listening compile machinery.
    const ENiagaraScriptUsage OldUsage = Script->GetUsage();
    Script->Usage = NewUsage;

#if WITH_EDITOR
    // Mark the source graph not-synchronised so the next compile
    // request reruns. The graph stays the same; only the usage
    // gate flips. UNiagaraScriptSourceBase::MarkNotSynchronized is
    // the public hook the editor calls.
    if (UNiagaraScriptSourceBase* Source = Script->GetLatestSource())
    {
        Source->MarkNotSynchronized(TEXT("Sproft niagara_script_edit set_module_usage"));
    }

    if (FProperty* UsageProp = FindFProperty<FProperty>(UNiagaraScript::StaticClass(), TEXT("Usage")))
    {
        FPropertyChangedEvent ChangeEvent(UsageProp, EPropertyChangeType::ValueSet);
        Script->PostEditChangeProperty(ChangeEvent);
    }
#endif

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    Script->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Script->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_module_usage"));
    Out->SetStringField(TEXT("script"), Script->GetPathName());
    Out->SetStringField(TEXT("usage"), NiagaraScriptEdit_UsageToString(NewUsage));
    Out->SetStringField(TEXT("previous_usage"), NiagaraScriptEdit_UsageToString(OldUsage));
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

namespace
{
    /** Per-file token resolver. Same shape as the helper inside
     *  `SproftNiagaraEditCommands.cpp` (the two files do not share a
     *  helper translation unit per the post-batch-22 unique-helper
     *  convention). */
    bool NiagaraScriptEdit_ResolveTypeToken(const FString& InToken, FNiagaraTypeDefinition& OutType, FString& OutCanonical)
    {
        const FString T = InToken.ToLower();
        if (T == TEXT("float"))
        {
            OutType = FNiagaraTypeDefinition::GetFloatDef();
            OutCanonical = TEXT("float");
            return true;
        }
        if (T == TEXT("int") || T == TEXT("int32"))
        {
            OutType = FNiagaraTypeDefinition::GetIntDef();
            OutCanonical = TEXT("int");
            return true;
        }
        if (T == TEXT("bool"))
        {
            OutType = FNiagaraTypeDefinition::GetBoolDef();
            OutCanonical = TEXT("bool");
            return true;
        }
        if (T == TEXT("vec2") || T == TEXT("vector2") || T == TEXT("fvector2f"))
        {
            OutType = FNiagaraTypeDefinition::GetVec2Def();
            OutCanonical = TEXT("vec2");
            return true;
        }
        if (T == TEXT("vec3") || T == TEXT("vector") || T == TEXT("vector3") || T == TEXT("fvector3f"))
        {
            OutType = FNiagaraTypeDefinition::GetVec3Def();
            OutCanonical = TEXT("vec3");
            return true;
        }
        if (T == TEXT("vec4") || T == TEXT("vector4") || T == TEXT("fvector4f"))
        {
            OutType = FNiagaraTypeDefinition::GetVec4Def();
            OutCanonical = TEXT("vec4");
            return true;
        }
        if (T == TEXT("color") || T == TEXT("linear_color") || T == TEXT("flinearcolor"))
        {
            OutType = FNiagaraTypeDefinition::GetColorDef();
            OutCanonical = TEXT("color");
            return true;
        }
        if (T == TEXT("quat") || T == TEXT("fquat4f"))
        {
            OutType = FNiagaraTypeDefinition::GetQuatDef();
            OutCanonical = TEXT("quat");
            return true;
        }
        return false;
    }

    /** Pull a tightly-packed default-value buffer for a Niagara type
     *  out of a JSON value. Mirrors `ReadFloatChannels` from the
     *  niagara_edit slice but lives here because the helper there is
     *  in an anonymous namespace. Unlike the niagara_edit value-write
     *  path, the optional `value` parameter on `add_input_parameter`
     *  may be missing, in which case we leave the buffer at its
     *  type-zero default. */
    bool NiagaraScriptEdit_BuildDefaultBuffer(const TSharedPtr<FJsonValue>& Value,
                                              const FString& CanonicalType,
                                              int32 TypeSize,
                                              TArray<uint8>& OutBuffer,
                                              FString& OutErr)
    {
        OutBuffer.SetNumZeroed(TypeSize);
        if (!Value.IsValid())
        {
            return true; // zero-default
        }
        if (CanonicalType == TEXT("bool"))
        {
            const bool bVal = (Value->Type == EJson::Boolean) ? Value->AsBool()
                            : (Value->Type == EJson::Number ? (Value->AsNumber() != 0.0) : false);
            const int32 EncodedBool = bVal ? -1 : 0; // FNiagaraBool::True == -1.
            FMemory::Memcpy(OutBuffer.GetData(), &EncodedBool, sizeof(int32));
            return true;
        }
        if (CanonicalType == TEXT("int"))
        {
            const int32 IntVal = (Value->Type == EJson::Number)
                ? static_cast<int32>(FMath::RoundToDouble(Value->AsNumber()))
                : 0;
            FMemory::Memcpy(OutBuffer.GetData(), &IntVal, sizeof(int32));
            return true;
        }
        // Float family. The expected channel count is implicit in the
        // type size (4 bytes per channel).
        const int32 ExpectedFloats = TypeSize / static_cast<int32>(sizeof(float));
        TArray<float> Channels;
        if (ExpectedFloats == 1)
        {
            if (Value->Type == EJson::Number)
            {
                Channels.Add(static_cast<float>(Value->AsNumber()));
            }
            else
            {
                OutErr = TEXT("Expected number for scalar 'value'");
                return false;
            }
        }
        else
        {
            if (Value->Type != EJson::Array)
            {
                OutErr = FString::Printf(TEXT("Expected array of length %d for 'value'"), ExpectedFloats);
                return false;
            }
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() < ExpectedFloats)
            {
                OutErr = FString::Printf(TEXT("Expected array of length %d for 'value', got %d"),
                    ExpectedFloats, Arr.Num());
                return false;
            }
            for (int32 I = 0; I < ExpectedFloats; ++I)
            {
                Channels.Add(static_cast<float>(Arr[I]->AsNumber()));
            }
        }
        FMemory::Memcpy(OutBuffer.GetData(), Channels.GetData(), TypeSize);
        return true;
    }
}

TSharedPtr<FJsonObject> FSproftNiagaraScriptEditCommands::HandleAddInputParameter(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
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

    FString ParameterName;
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName)
        && !Params->TryGetStringField(TEXT("parameter"), ParameterName)
        && !Params->TryGetStringField(TEXT("name"), ParameterName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }
    if (ParameterName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'parameter_name' must be non-empty"));
    }

    FString TypeToken;
    if (!Params->TryGetStringField(TEXT("parameter_type"), TypeToken)
        && !Params->TryGetStringField(TEXT("type"), TypeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'parameter_type' parameter (try float / int / bool / vec2 / vec3 / vec4 / color / quat)"));
    }
    FNiagaraTypeDefinition TypeDef;
    FString CanonicalType;
    if (!NiagaraScriptEdit_ResolveTypeToken(TypeToken, TypeDef, CanonicalType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported 'parameter_type' '%s' (try float / int / bool / vec2 / vec3 / vec4 / color / quat)"), *TypeToken));
    }
    const int32 TypeSize = TypeDef.GetSize();

    // Build the (optional) default-value buffer; missing `value`
    // leaves the parameter at its type-zero default.
    TArray<uint8> Buffer;
    FString ReadErr;
    const TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
    if (!NiagaraScriptEdit_BuildDefaultBuffer(ValueJson, CanonicalType, TypeSize, Buffer, ReadErr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ReadErr);
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // The canonical authoring path on a UNiagaraScript would walk the
    // script's source graph and call `UNiagaraGraph::AddParameter` so
    // the parameter shows up as a Module input on a recompile. The
    // three relevant `AddParameter` overloads on `UNiagaraGraph` are
    // declared without `NIAGARAEDITOR_API` in 5.7, so they are not
    // callable from a third-party module without copying engine code
    // (which we do not do per the clean-room rule). The workaround
    // below writes the parameter into the script's
    // `RapidIterationParameters` store directly through
    // `FNiagaraParameterStore::AddParameter` (NIAGARA_API, public)
    // and marks the source graph not-synchronised so the next
    // explicit recompile reruns. The parameter shows up in the asset
    // editor's stack on the script's input panel, but a graph-driven
    // recompile that walks the source graph alone will not see the
    // entry; designers who want the parameter pinned into the graph
    // should re-author it through the editor's Module Inputs panel.
    Script->Modify();
    FNiagaraParameterStore& Store = Script->RapidIterationParameters;

    FNiagaraVariable Variable(TypeDef, FName(*ParameterName));
    Variable.SetData(Buffer.GetData());

    const bool bAlreadyPresent = (Store.IndexOf(Variable) != INDEX_NONE);
    int32 ParamOffset = INDEX_NONE;
    const bool bAdded = Store.AddParameter(Variable, /*bInitialize=*/true, /*bTriggerRebind=*/true, &ParamOffset);
    if ((bAdded || bAlreadyPresent) && Buffer.Num() == TypeSize)
    {
        Store.SetParameterData(Buffer.GetData(), Variable, /*bAdd=*/false);
    }

#if WITH_EDITOR
    if (UNiagaraScriptSourceBase* Source = Script->GetLatestSource())
    {
        Source->MarkNotSynchronized(TEXT("Sproft niagara_script_edit add_input_parameter"));
    }
    if (FProperty* RIPProp = FindFProperty<FProperty>(UNiagaraScript::StaticClass(), TEXT("RapidIterationParameters")))
    {
        FPropertyChangedEvent ChangeEvent(RIPProp, EPropertyChangeType::ValueSet);
        Script->PostEditChangeProperty(ChangeEvent);
    }
#endif

    Script->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Script->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_input_parameter"));
    Out->SetStringField(TEXT("script"), Script->GetPathName());
    Out->SetStringField(TEXT("parameter_name"), ParameterName);
    Out->SetStringField(TEXT("parameter_type"), CanonicalType);
    Out->SetNumberField(TEXT("parameter_size"), TypeSize);
    Out->SetBoolField(TEXT("already_present"), bAlreadyPresent);
    Out->SetBoolField(TEXT("added"), bAdded);
    if (ParamOffset != INDEX_NONE)
    {
        Out->SetNumberField(TEXT("parameter_offset"), ParamOffset);
    }
    Out->SetStringField(TEXT("store"), TEXT("rapid_iteration_parameters"));
    Out->SetStringField(TEXT("note"),
        TEXT("Parameter written to UNiagaraScript::RapidIterationParameters; the canonical UNiagaraGraph::AddParameter is not exported by NIAGARAEDITOR_API in 5.7. A graph-driven recompile will not pin the parameter into the source graph; re-author through the editor's Module Inputs panel for graph-side persistence."));
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#else
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("niagara_script_edit add_input_parameter requires WITH_EDITORONLY_DATA"));
#endif
}
