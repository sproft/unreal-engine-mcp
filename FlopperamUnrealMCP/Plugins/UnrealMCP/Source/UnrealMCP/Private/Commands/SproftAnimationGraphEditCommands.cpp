#include "Commands/SproftAnimationGraphEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AlphaBlend.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    UAnimBlueprint* ResolveAnimBlueprint(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UAnimBlueprint>(UEditorAssetLibrary::LoadAsset(Input));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UAnimBlueprint::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UAnimBlueprint>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    /** Map EAlphaBlendOption to a single-word token. */
    const TCHAR* AlphaBlendOptionToken(EAlphaBlendOption Option)
    {
        switch (Option)
        {
        case EAlphaBlendOption::Linear:           return TEXT("linear");
        case EAlphaBlendOption::Cubic:            return TEXT("cubic_in");
        case EAlphaBlendOption::HermiteCubic:     return TEXT("hermite_cubic");
        case EAlphaBlendOption::Sinusoidal:       return TEXT("sinusoidal");
        case EAlphaBlendOption::QuadraticInOut:   return TEXT("quadratic_in_out");
        case EAlphaBlendOption::CubicInOut:       return TEXT("cubic_in_out");
        case EAlphaBlendOption::QuarticInOut:     return TEXT("quartic_in_out");
        case EAlphaBlendOption::QuinticInOut:     return TEXT("quintic_in_out");
        case EAlphaBlendOption::CircularIn:       return TEXT("circular_in");
        case EAlphaBlendOption::CircularOut:      return TEXT("circular_out");
        case EAlphaBlendOption::CircularInOut:    return TEXT("circular_in_out");
        case EAlphaBlendOption::ExpIn:            return TEXT("exp_in");
        case EAlphaBlendOption::ExpOut:           return TEXT("exp_out");
        case EAlphaBlendOption::ExpInOut:         return TEXT("exp_in_out");
        case EAlphaBlendOption::Custom:           return TEXT("custom");
        default:                                  return TEXT("unknown");
        }
    }

    /** Map ETransitionLogicType to a single-word token. */
    const TCHAR* TransitionLogicTypeToken(ETransitionLogicType::Type Type)
    {
        switch (Type)
        {
        case ETransitionLogicType::TLT_StandardBlend: return TEXT("standard_blend");
        case ETransitionLogicType::TLT_Inertialization: return TEXT("inertialization");
        case ETransitionLogicType::TLT_Custom:        return TEXT("custom");
        default:                                      return TEXT("unknown");
        }
    }
}

FSproftAnimationGraphEditCommands::FSproftAnimationGraphEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftAnimationGraphEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("animation_graph_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown animation_graph_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }
    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op.Equals(TEXT("inspect"), ESearchCase::IgnoreCase))
    {
        return HandleAnimationGraphInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("animation_graph_edit: unsupported op '%s'. Only 'inspect' is shipped on this slice"), *Op));
}

TSharedPtr<FJsonObject> FSproftAnimationGraphEditCommands::HandleAnimationGraphInspect(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimBPParam;
    if (!Params->TryGetStringField(TEXT("anim_bp"), AnimBPParam)
        && !Params->TryGetStringField(TEXT("animation"), AnimBPParam)
        && !Params->TryGetStringField(TEXT("path"), AnimBPParam)
        && !Params->TryGetStringField(TEXT("asset"), AnimBPParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'anim_bp' parameter"));
    }
    UAnimBlueprint* AnimBP = ResolveAnimBlueprint(AnimBPParam);
    if (!AnimBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UAnimBlueprint '%s'"), *AnimBPParam));
    }

    bool bIncludeStateMachines = true;
    bool bIncludeStates = true;
    bool bIncludeTransitions = true;
    bool bIncludeAnimNodes = true;
    int32 MaxStateMachines = 64;
    int32 MaxStatesPerMachine = 256;
    int32 MaxTransitionsPerMachine = 1024;
    int32 MaxAnimNodes = 2048;
    Params->TryGetBoolField(TEXT("include_state_machines"), bIncludeStateMachines);
    Params->TryGetBoolField(TEXT("include_states"), bIncludeStates);
    Params->TryGetBoolField(TEXT("include_transitions"), bIncludeTransitions);
    Params->TryGetBoolField(TEXT("include_anim_nodes"), bIncludeAnimNodes);
    Params->TryGetNumberField(TEXT("max_state_machines"), MaxStateMachines);
    Params->TryGetNumberField(TEXT("max_states_per_machine"), MaxStatesPerMachine);
    Params->TryGetNumberField(TEXT("max_transitions_per_machine"), MaxTransitionsPerMachine);
    Params->TryGetNumberField(TEXT("max_anim_nodes"), MaxAnimNodes);
    auto ClampMin = [](int32& Out) { if (Out < 1) Out = 1; };
    ClampMin(MaxStateMachines);
    ClampMin(MaxStatesPerMachine);
    ClampMin(MaxTransitionsPerMachine);
    ClampMin(MaxAnimNodes);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("name"), AnimBP->GetName());
    Result->SetStringField(TEXT("path"), AnimBP->GetPathName());
    Result->SetStringField(TEXT("class"), AnimBP->GetClass()->GetName());
    if (UClass* ParentClass = AnimBP->ParentClass)
    {
        Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
        Result->SetStringField(TEXT("parent_class_path"), ParentClass->GetPathName());
    }
    if (USkeleton* Skel = AnimBP->TargetSkeleton)
    {
        Result->SetStringField(TEXT("target_skeleton_path"), Skel->GetPathName());
    }
    Result->SetBoolField(TEXT("is_template"), AnimBP->bIsTemplate);

    UAnimBlueprintGeneratedClass* GenClass = AnimBP->GetAnimBlueprintGeneratedClass();
    const bool bCompiled = GenClass != nullptr;
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    if (!bCompiled)
    {
        Result->SetStringField(TEXT("not_compiled"),
            TEXT("UAnimBlueprintGeneratedClass is null; AnimBP must compile at least once for the baked surface"));
        Result->SetNumberField(TEXT("state_machine_count"), 0);
        Result->SetNumberField(TEXT("anim_node_count"), 0);
        return Result;
    }

    // State machines.
    const TArray<FBakedAnimationStateMachine>& Machines = GenClass->BakedStateMachines;
    Result->SetNumberField(TEXT("state_machine_count"), Machines.Num());

    if (bIncludeStateMachines)
    {
        const int32 MachineEmit = FMath::Min(Machines.Num(), MaxStateMachines);
        TArray<TSharedPtr<FJsonValue>> MachineArr;
        MachineArr.Reserve(MachineEmit);
        for (int32 MIdx = 0; MIdx < MachineEmit; ++MIdx)
        {
            const FBakedAnimationStateMachine& SM = Machines[MIdx];
            TSharedPtr<FJsonObject> MRow = MakeShared<FJsonObject>();
            MRow->SetNumberField(TEXT("index"), MIdx);
            MRow->SetStringField(TEXT("name"), SM.MachineName.ToString());
            MRow->SetNumberField(TEXT("initial_state"), SM.InitialState);
            if (SM.States.IsValidIndex(SM.InitialState))
            {
                MRow->SetStringField(TEXT("initial_state_name"),
                    SM.States[SM.InitialState].StateName.ToString());
            }
            MRow->SetNumberField(TEXT("state_count"), SM.States.Num());
            MRow->SetNumberField(TEXT("transition_count"), SM.Transitions.Num());

            if (bIncludeStates)
            {
                const int32 StateEmit = FMath::Min(SM.States.Num(), MaxStatesPerMachine);
                TArray<TSharedPtr<FJsonValue>> StateArr;
                StateArr.Reserve(StateEmit);
                for (int32 SIdx = 0; SIdx < StateEmit; ++SIdx)
                {
                    const FBakedAnimationState& State = SM.States[SIdx];
                    TSharedPtr<FJsonObject> SRow = MakeShared<FJsonObject>();
                    SRow->SetNumberField(TEXT("index"), SIdx);
                    SRow->SetStringField(TEXT("name"), State.StateName.ToString());
                    SRow->SetNumberField(TEXT("state_root_node_index"), State.StateRootNodeIndex);
                    SRow->SetNumberField(TEXT("start_notify"), State.StartNotify);
                    SRow->SetNumberField(TEXT("end_notify"), State.EndNotify);
                    SRow->SetNumberField(TEXT("fully_blended_notify"), State.FullyBlendedNotify);
                    SRow->SetNumberField(TEXT("entry_rule_node_index"), State.EntryRuleNodeIndex);
                    SRow->SetBoolField(TEXT("always_reset_on_entry"), State.bAlwaysResetOnEntry);
                    SRow->SetBoolField(TEXT("is_a_conduit"), State.bIsAConduit);
                    SRow->SetNumberField(TEXT("player_node_count"), State.PlayerNodeIndices.Num());
                    SRow->SetNumberField(TEXT("layer_node_count"), State.LayerNodeIndices.Num());

                    // Each state's exit transitions point at the per-machine
                    // Transitions table by index. We emit the index plus
                    // a normalised return-value flag and the auto-rule
                    // info; the actual previous_state / next_state lookup
                    // happens off the machine-level Transitions array
                    // below.
                    TArray<TSharedPtr<FJsonValue>> EditArr;
                    EditArr.Reserve(State.Transitions.Num());
                    for (const FBakedStateExitTransition& Exit : State.Transitions)
                    {
                        TSharedPtr<FJsonObject> ExitRow = MakeShared<FJsonObject>();
                        ExitRow->SetNumberField(TEXT("transition_index"), Exit.TransitionIndex);
                        ExitRow->SetBoolField(TEXT("desired_transition_return_value"), Exit.bDesiredTransitionReturnValue);
                        ExitRow->SetBoolField(TEXT("automatic_remaining_time_rule"), Exit.bAutomaticRemainingTimeRule);
                        ExitRow->SetNumberField(TEXT("automatic_rule_trigger_time"), Exit.AutomaticRuleTriggerTime);
                        if (!Exit.SyncGroupNameToRequireValidMarkersRule.IsNone())
                        {
                            ExitRow->SetStringField(TEXT("sync_group_required_markers"),
                                Exit.SyncGroupNameToRequireValidMarkersRule.ToString());
                        }
                        ExitRow->SetNumberField(TEXT("can_take_delegate_index"), Exit.CanTakeDelegateIndex);
                        ExitRow->SetNumberField(TEXT("custom_result_node_index"), Exit.CustomResultNodeIndex);
                        EditArr.Add(MakeShared<FJsonValueObject>(ExitRow));
                    }
                    SRow->SetArrayField(TEXT("exit_transitions"), EditArr);
                    StateArr.Add(MakeShared<FJsonValueObject>(SRow));
                }
                MRow->SetArrayField(TEXT("states"), StateArr);
                MRow->SetBoolField(TEXT("states_truncated"), SM.States.Num() > StateEmit);
            }

            if (bIncludeTransitions)
            {
                const int32 TransEmit = FMath::Min(SM.Transitions.Num(), MaxTransitionsPerMachine);
                TArray<TSharedPtr<FJsonValue>> TransArr;
                TransArr.Reserve(TransEmit);
                for (int32 TIdx = 0; TIdx < TransEmit; ++TIdx)
                {
                    const FAnimationTransitionBetweenStates& Trans = SM.Transitions[TIdx];
                    TSharedPtr<FJsonObject> TRow = MakeShared<FJsonObject>();
                    TRow->SetNumberField(TEXT("index"), TIdx);
                    TRow->SetNumberField(TEXT("previous_state"), Trans.PreviousState);
                    TRow->SetNumberField(TEXT("next_state"), Trans.NextState);
                    if (SM.States.IsValidIndex(Trans.PreviousState))
                    {
                        TRow->SetStringField(TEXT("previous_state_name"),
                            SM.States[Trans.PreviousState].StateName.ToString());
                    }
                    if (SM.States.IsValidIndex(Trans.NextState))
                    {
                        TRow->SetStringField(TEXT("next_state_name"),
                            SM.States[Trans.NextState].StateName.ToString());
                    }
                    TRow->SetNumberField(TEXT("crossfade_duration"), Trans.CrossfadeDuration);
                    TRow->SetNumberField(TEXT("min_time_before_reentry"), Trans.MinTimeBeforeReentry);
                    TRow->SetStringField(TEXT("blend_mode"), AlphaBlendOptionToken(Trans.BlendMode));
                    TRow->SetStringField(TEXT("logic_type"), TransitionLogicTypeToken(Trans.LogicType.GetValue()));
                    TRow->SetBoolField(TEXT("allow_inertialization_for_self_transitions"),
                        Trans.bAllowInertializationForSelfTransitions != 0);
                    TRow->SetNumberField(TEXT("start_notify"), Trans.StartNotify);
                    TRow->SetNumberField(TEXT("end_notify"), Trans.EndNotify);
                    TRow->SetNumberField(TEXT("interrupt_notify"), Trans.InterruptNotify);
                    if (Trans.CustomCurve)
                    {
                        TRow->SetStringField(TEXT("custom_curve_path"), Trans.CustomCurve->GetPathName());
                    }
                    if (Trans.BlendProfile)
                    {
                        TRow->SetStringField(TEXT("blend_profile_path"), Trans.BlendProfile->GetPathName());
                    }
                    TransArr.Add(MakeShared<FJsonValueObject>(TRow));
                }
                MRow->SetArrayField(TEXT("transitions"), TransArr);
                MRow->SetBoolField(TEXT("transitions_truncated"), SM.Transitions.Num() > TransEmit);
            }

            MachineArr.Add(MakeShared<FJsonValueObject>(MRow));
        }
        Result->SetArrayField(TEXT("state_machines"), MachineArr);
        Result->SetBoolField(TEXT("state_machines_truncated"), Machines.Num() > MachineEmit);
    }

    // Flat AnimGraph node-property list. Each entry is a UScriptStruct
    // pointer that names the anim node's struct (e.g.
    // FAnimNode_StateMachine). We emit the index + struct short name +
    // struct path so a downstream consumer can answer "what AnimGraph
    // nodes does this AnimBP have" without needing the editor-only
    // AnimGraph module.
    Result->SetNumberField(TEXT("anim_node_count"), GenClass->AnimNodeProperties.Num());
    if (bIncludeAnimNodes)
    {
        const int32 NodeEmit = FMath::Min(GenClass->AnimNodeProperties.Num(), MaxAnimNodes);
        TArray<TSharedPtr<FJsonValue>> NodeArr;
        NodeArr.Reserve(NodeEmit);
        for (int32 I = 0; I < NodeEmit; ++I)
        {
            const FStructProperty* StructProp = GenClass->AnimNodeProperties[I];
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("index"), I);
            if (StructProp && StructProp->Struct)
            {
                Row->SetStringField(TEXT("struct_type"), StructProp->Struct->GetName());
                Row->SetStringField(TEXT("struct_path"), StructProp->Struct->GetPathName());
            }
            else
            {
                Row->SetStringField(TEXT("error"), TEXT("null struct property"));
            }
            NodeArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("anim_nodes"), NodeArr);
        Result->SetBoolField(TEXT("anim_nodes_truncated"), GenClass->AnimNodeProperties.Num() > NodeEmit);
    }

    return Result;
}
