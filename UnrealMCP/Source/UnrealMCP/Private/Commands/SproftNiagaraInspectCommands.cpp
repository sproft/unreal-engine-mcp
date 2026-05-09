#include "Commands/SproftNiagaraInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Class.h"

namespace
{
    const TCHAR* SimTargetToString(ENiagaraSimTarget Target)
    {
        switch (Target)
        {
        case ENiagaraSimTarget::CPUSim: return TEXT("cpu");
        case ENiagaraSimTarget::GPUComputeSim: return TEXT("gpu");
        default: return TEXT("unknown");
        }
    }

    const TCHAR* NiagaraInspect_UsageToString(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::Function: return TEXT("function");
        case ENiagaraScriptUsage::Module: return TEXT("module");
        case ENiagaraScriptUsage::DynamicInput: return TEXT("dynamic_input");
        case ENiagaraScriptUsage::ParticleSpawnScript: return TEXT("particle_spawn");
        case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("particle_spawn_interpolated");
        case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("particle_update");
        case ENiagaraScriptUsage::ParticleEventScript: return TEXT("particle_event");
        case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("particle_simulation_stage");
        case ENiagaraScriptUsage::ParticleGPUComputeScript: return TEXT("particle_gpu_compute");
        case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("emitter_spawn");
        case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("emitter_update");
        case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("system_spawn");
        case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("system_update");
        default: return TEXT("unknown");
        }
    }

    TSharedPtr<FJsonObject> ScriptSummary(UNiagaraScript* Script, const TCHAR* StageName)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("stage"), StageName);
        if (Script)
        {
            Out->SetStringField(TEXT("script_name"), Script->GetName());
            Out->SetStringField(TEXT("script_path"), Script->GetPathName());
            Out->SetStringField(TEXT("usage"), NiagaraInspect_UsageToString(Script->GetUsage()));
        }
        else
        {
            Out->SetStringField(TEXT("script_name"), TEXT(""));
        }
        return Out;
    }

    /** Categorise a Niagara variable so a downstream consumer does not
     *  need to peek at the FNiagaraTypeDefinition. */
    const TCHAR* NiagaraInspect_VariableKind(const FNiagaraVariableBase& Var)
    {
        if (Var.IsDataInterface()) { return TEXT("data_interface"); }
        if (Var.IsUObject()) { return TEXT("object"); }
        return TEXT("primitive");
    }
}

FSproftNiagaraInspectCommands::FSproftNiagaraInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftNiagaraInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("niagara_inspect"))
    {
        return HandleNiagaraInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown niagara_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftNiagaraInspectCommands::HandleNiagaraInspect(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString SystemPath;
    if (!Params->TryGetStringField(TEXT("system"), SystemPath)
        && !Params->TryGetStringField(TEXT("system_path"), SystemPath)
        && !Params->TryGetStringField(TEXT("path"), SystemPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system' parameter"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(SystemPath);
    UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
    if (!System)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UNiagaraSystem"), *SystemPath));
    }

    bool bIncludeEventHandlers = true;
    bool bIncludeSimStages = true;
    bool bIncludeRenderers = true;
    bool bIncludeParameters = true;
    Params->TryGetBoolField(TEXT("include_event_handlers"), bIncludeEventHandlers);
    Params->TryGetBoolField(TEXT("include_simulation_stages"), bIncludeSimStages);
    Params->TryGetBoolField(TEXT("include_renderers"), bIncludeRenderers);
    Params->TryGetBoolField(TEXT("include_parameters"), bIncludeParameters);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), System->GetName());
    Result->SetStringField(TEXT("path"), System->GetPathName());
    Result->SetStringField(TEXT("class"), System->GetClass()->GetName());

    if (UNiagaraScript* Spawn = System->GetSystemSpawnScript())
    {
        Result->SetStringField(TEXT("system_spawn_script"), Spawn->GetPathName());
    }
    if (UNiagaraScript* Update = System->GetSystemUpdateScript())
    {
        Result->SetStringField(TEXT("system_update_script"), Update->GetPathName());
    }

    // Emitter list with per-stage scripts and renderer / sim-stage chains.
    TArray<TSharedPtr<FJsonValue>> EmitterArr;
    int32 EnabledEmitters = 0;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterRow = MakeShared<FJsonObject>();
        EmitterRow->SetStringField(TEXT("name"), Handle.GetName().ToString());
        EmitterRow->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
        const bool bEnabled = Handle.GetIsEnabled();
        EmitterRow->SetBoolField(TEXT("enabled"), bEnabled);
        if (bEnabled) { ++EnabledEmitters; }

        const FVersionedNiagaraEmitter Versioned = Handle.GetInstance();
        if (Versioned.Emitter)
        {
            EmitterRow->SetStringField(TEXT("emitter_path"), Versioned.Emitter->GetPathName());
        }

        FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
        if (!EmitterData)
        {
            EmitterRow->SetStringField(TEXT("error"), TEXT("emitter has no resolved data"));
            EmitterArr.Add(MakeShared<FJsonValueObject>(EmitterRow));
            continue;
        }

        EmitterRow->SetStringField(TEXT("sim_target"), SimTargetToString(EmitterData->SimTarget));
        EmitterRow->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
        EmitterRow->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
        EmitterRow->SetNumberField(TEXT("random_seed"), EmitterData->RandomSeed);

        TArray<TSharedPtr<FJsonValue>> ScriptArr;
        ScriptArr.Add(MakeShared<FJsonValueObject>(ScriptSummary(EmitterData->EmitterSpawnScriptProps.Script, TEXT("emitter_spawn"))));
        ScriptArr.Add(MakeShared<FJsonValueObject>(ScriptSummary(EmitterData->EmitterUpdateScriptProps.Script, TEXT("emitter_update"))));
        ScriptArr.Add(MakeShared<FJsonValueObject>(ScriptSummary(EmitterData->SpawnScriptProps.Script, TEXT("particle_spawn"))));
        ScriptArr.Add(MakeShared<FJsonValueObject>(ScriptSummary(EmitterData->UpdateScriptProps.Script, TEXT("particle_update"))));
        if (UNiagaraScript* GPU = EmitterData->GetGPUComputeScript())
        {
            ScriptArr.Add(MakeShared<FJsonValueObject>(ScriptSummary(GPU, TEXT("particle_gpu_compute"))));
        }
        EmitterRow->SetArrayField(TEXT("scripts"), ScriptArr);

        if (bIncludeEventHandlers)
        {
            TArray<TSharedPtr<FJsonValue>> EventArr;
            for (const FNiagaraEventScriptProperties& Event : EmitterData->GetEventHandlers())
            {
                TSharedPtr<FJsonObject> EventRow = MakeShared<FJsonObject>();
                EventRow->SetStringField(TEXT("source_event_name"), Event.SourceEventName.ToString());
                EventRow->SetNumberField(TEXT("spawn_number"), Event.SpawnNumber);
                EventRow->SetNumberField(TEXT("max_events_per_frame"), Event.MaxEventsPerFrame);
                EventRow->SetBoolField(TEXT("random_spawn_number"), Event.bRandomSpawnNumber);
                EventRow->SetNumberField(TEXT("min_spawn_number"), Event.MinSpawnNumber);
                if (Event.Script)
                {
                    EventRow->SetStringField(TEXT("script_name"), Event.Script->GetName());
                    EventRow->SetStringField(TEXT("script_path"), Event.Script->GetPathName());
                }
                EventArr.Add(MakeShared<FJsonValueObject>(EventRow));
            }
            EmitterRow->SetArrayField(TEXT("event_handlers"), EventArr);
        }

        if (bIncludeSimStages)
        {
            TArray<TSharedPtr<FJsonValue>> StageArr;
            for (UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
            {
                TSharedPtr<FJsonObject> StageRow = MakeShared<FJsonObject>();
                if (Stage)
                {
                    StageRow->SetStringField(TEXT("name"), Stage->GetName());
                    StageRow->SetStringField(TEXT("class"), Stage->GetClass()->GetName());
                }
                StageArr.Add(MakeShared<FJsonValueObject>(StageRow));
            }
            EmitterRow->SetArrayField(TEXT("simulation_stages"), StageArr);
        }

        if (bIncludeRenderers)
        {
            TArray<TSharedPtr<FJsonValue>> RendererArr;
            for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
            {
                TSharedPtr<FJsonObject> RendererRow = MakeShared<FJsonObject>();
                if (Renderer)
                {
                    RendererRow->SetStringField(TEXT("name"), Renderer->GetName());
                    RendererRow->SetStringField(TEXT("class"), Renderer->GetClass()->GetName());
                    RendererRow->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
                }
                RendererArr.Add(MakeShared<FJsonValueObject>(RendererRow));
            }
            EmitterRow->SetArrayField(TEXT("renderers"), RendererArr);
        }

        EmitterArr.Add(MakeShared<FJsonValueObject>(EmitterRow));
    }
    Result->SetArrayField(TEXT("emitters"), EmitterArr);
    Result->SetNumberField(TEXT("emitter_count"), EmitterArr.Num());
    Result->SetNumberField(TEXT("enabled_emitter_count"), EnabledEmitters);

    if (bIncludeParameters)
    {
        TArray<TSharedPtr<FJsonValue>> ParamArr;
        const FNiagaraParameterStore& Store = System->GetExposedParameters();
        for (const FNiagaraVariableWithOffset& Var : Store.ReadParameterVariables())
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Var.GetName().ToString());
            const FNiagaraTypeDefinition& TypeDef = Var.GetType();
            Row->SetStringField(TEXT("type"), TypeDef.GetName());
            if (TypeDef.GetClass())
            {
                Row->SetStringField(TEXT("type_path"), TypeDef.GetClass()->GetPathName());
            }
            else if (TypeDef.GetScriptStruct())
            {
                Row->SetStringField(TEXT("type_path"), TypeDef.GetScriptStruct()->GetPathName());
            }
            else if (TypeDef.GetEnum())
            {
                Row->SetStringField(TEXT("type_path"), TypeDef.GetEnum()->GetPathName());
            }
            Row->SetStringField(TEXT("kind"), NiagaraInspect_VariableKind(Var));
            Row->SetNumberField(TEXT("offset"), Var.Offset);
            ParamArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("parameters"), ParamArr);
        Result->SetNumberField(TEXT("parameter_count"), ParamArr.Num());
    }

    return Result;
}
