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

#if WITH_EDITOR
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#endif

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
    if (Op.Equals(TEXT("add_state"), ESearchCase::IgnoreCase))
    {
        return HandleAddState(Params);
    }
    if (Op.Equals(TEXT("add_transition"), ESearchCase::IgnoreCase))
    {
        return HandleAddTransition(Params);
    }
    if (Op.Equals(TEXT("set_state_animation"), ESearchCase::IgnoreCase))
    {
        return HandleSetStateAnimation(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("animation_graph_edit: unsupported op '%s'. Supported: inspect, add_state, add_transition, set_state_animation"), *Op));
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

#if WITH_EDITOR
namespace
{
    /** Walk every UEdGraph reachable from the AnimBP and return the
     *  first UAnimGraphNode_StateMachineBase whose state-machine name
     *  matches `MachineName` (case-insensitive). Walks UbergraphPages
     *  and FunctionGraphs; the AnimGraph itself sits under
     *  `FunctionGraphs` on a UAnimBlueprint. */
    UAnimGraphNode_StateMachineBase* AnimationGraphEdit_FindStateMachineNode(
        UAnimBlueprint* AnimBP, const FString& MachineName)
    {
        if (!AnimBP || MachineName.IsEmpty()) return nullptr;
        const auto Walk = [&MachineName](UEdGraph* Graph) -> UAnimGraphNode_StateMachineBase*
        {
            if (!Graph) return nullptr;
            TArray<UEdGraph*> Stack;
            Stack.Push(Graph);
            while (Stack.Num() > 0)
            {
                UEdGraph* Current = Stack.Pop();
                if (!Current) continue;
                for (UEdGraphNode* Node : Current->Nodes)
                {
                    if (UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
                    {
                        const FString Name = StateMachineNode->GetStateMachineName();
                        if (Name.Equals(MachineName, ESearchCase::IgnoreCase))
                        {
                            return StateMachineNode;
                        }
                    }
                    if (Node)
                    {
                        for (UEdGraph* Sub : Node->GetSubGraphs())
                        {
                            if (Sub) Stack.Push(Sub);
                        }
                    }
                }
                for (UEdGraph* Sub : Current->SubGraphs)
                {
                    if (Sub) Stack.Push(Sub);
                }
            }
            return nullptr;
        };

        for (UEdGraph* Graph : AnimBP->FunctionGraphs)
        {
            if (UAnimGraphNode_StateMachineBase* Found = Walk(Graph)) return Found;
        }
        for (UEdGraph* Graph : AnimBP->UbergraphPages)
        {
            if (UAnimGraphNode_StateMachineBase* Found = Walk(Graph)) return Found;
        }
        for (UEdGraph* Graph : AnimBP->MacroGraphs)
        {
            if (UAnimGraphNode_StateMachineBase* Found = Walk(Graph)) return Found;
        }
        return nullptr;
    }

    /** Walk a UAnimationStateMachineGraph for an existing UAnimStateNode
     *  whose name matches `StateName` (case-insensitive on FName +
     *  GetStateName()). Used by the duplicate-name guard. */
    UAnimStateNode* AnimationGraphEdit_FindStateNodeByName(UAnimationStateMachineGraph* Graph, const FString& StateName)
    {
        if (!Graph) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
            {
                if (StateNode->GetFName().ToString().Equals(StateName, ESearchCase::IgnoreCase)
                    || StateNode->GetStateName().Equals(StateName, ESearchCase::IgnoreCase))
                {
                    return StateNode;
                }
            }
        }
        return nullptr;
    }
}
#endif

TSharedPtr<FJsonObject> FSproftAnimationGraphEditCommands::HandleAddState(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("animation_graph_edit add_state requires WITH_EDITOR (the editor-only AnimGraph module)"));
#else
    FString AnimBpParam;
    if (!Params->TryGetStringField(TEXT("anim_bp"), AnimBpParam)
        && !Params->TryGetStringField(TEXT("blueprint"), AnimBpParam)
        && !Params->TryGetStringField(TEXT("path"), AnimBpParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'anim_bp' parameter"));
    }
    UAnimBlueprint* AnimBP = ResolveAnimBlueprint(AnimBpParam);
    if (!AnimBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UAnimBlueprint '%s'"), *AnimBpParam));
    }

    FString MachineName;
    if (!Params->TryGetStringField(TEXT("state_machine"), MachineName)
        && !Params->TryGetStringField(TEXT("machine"), MachineName)
        && !Params->TryGetStringField(TEXT("machine_name"), MachineName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'state_machine' parameter"));
    }

    FString NewStateName;
    if (!Params->TryGetStringField(TEXT("state_name"), NewStateName)
        && !Params->TryGetStringField(TEXT("name"), NewStateName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'state_name' parameter"));
    }
    if (NewStateName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'state_name' must be non-empty"));
    }

    UAnimGraphNode_StateMachineBase* StateMachineNode = AnimationGraphEdit_FindStateMachineNode(AnimBP, MachineName);
    if (!StateMachineNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find state machine '%s' on AnimBP '%s'"), *MachineName, *AnimBP->GetName()));
    }
    UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode->EditorStateMachineGraph;
    if (!StateMachineGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("State machine '%s' has no editor graph (uncompiled or stripped)"), *MachineName));
    }

    if (UAnimStateNode* Existing = AnimationGraphEdit_FindStateNodeByName(StateMachineGraph, NewStateName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("State '%s' already exists on state machine '%s'"), *NewStateName, *MachineName));
    }

    // Optional 2D editor position. The schema action handles snap-to-
    // grid + node-flag setup, so we just hand the location through.
    FVector2f Location(0.0f, 0.0f);
    const TSharedPtr<FJsonObject>* PositionObj = nullptr;
    if (Params->TryGetObjectField(TEXT("position"), PositionObj) && PositionObj && PositionObj->IsValid())
    {
        double XVal = 0.0;
        double YVal = 0.0;
        (*PositionObj)->TryGetNumberField(TEXT("x"), XVal);
        (*PositionObj)->TryGetNumberField(TEXT("y"), YVal);
        Location = FVector2f(static_cast<float>(XVal), static_cast<float>(YVal));
    }

    bool bCompile = true;
    bool bSave = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Spawn the state node on the state machine graph through the
    // public schema-action template. Outers under the state machine
    // graph; PostPlacedNewNode wires its BoundGraph (the per-state
    // AnimGraph that holds the state's pose subtree).
    UAnimStateNode* NewStateTemplate = NewObject<UAnimStateNode>();
    UAnimStateNode* NewStateNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
        StateMachineGraph, NewStateTemplate, Location, /*bSelectNewNode=*/false);
    if (!NewStateNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FEdGraphSchemaAction_NewStateNode::PerformAction returned null on state machine '%s'"), *MachineName));
    }

    // Rename to the caller-provided FName. The state node's user-
    // visible name comes off GetStateName(), which reads through the
    // node's underlying FName.
    NewStateNode->Rename(*NewStateName, /*NewOuter=*/nullptr, REN_DontCreateRedirectors);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    if (bCompile)
    {
        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::None, &Results);
    }
    AnimBP->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AnimBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_state"));
    Out->SetStringField(TEXT("anim_bp"), AnimBP->GetPathName());
    Out->SetStringField(TEXT("state_machine"), MachineName);
    Out->SetStringField(TEXT("state_name"), NewStateNode->GetFName().ToString());
    Out->SetStringField(TEXT("state_label"), NewStateNode->GetStateName());
    if (UEdGraph* BoundGraph = NewStateNode->BoundGraph)
    {
        Out->SetStringField(TEXT("bound_graph"), BoundGraph->GetName());
        Out->SetStringField(TEXT("bound_graph_path"), BoundGraph->GetPathName());
    }
    Out->SetBoolField(TEXT("compiled"), bCompile);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#endif
}

#if WITH_EDITOR
namespace
{
    /** Read a UAnimStateNode from the supplied state-machine graph by
     *  case-insensitive FName / GetStateName() match. Returns null when
     *  no state matches. */
    UAnimStateNode* AnimationGraphEdit_FindStateByName(UAnimationStateMachineGraph* Graph, const FString& StateName)
    {
        if (!Graph || StateName.IsEmpty()) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
            {
                if (StateNode->GetFName().ToString().Equals(StateName, ESearchCase::IgnoreCase)
                    || StateNode->GetStateName().Equals(StateName, ESearchCase::IgnoreCase))
                {
                    return StateNode;
                }
            }
        }
        return nullptr;
    }

    /** Walk a state machine graph for an existing transition between
     *  the two states. Used by the duplicate-edge guard so we do not
     *  silently spawn parallel transitions on every call. */
    UAnimStateTransitionNode* AnimationGraphEdit_FindTransitionBetween(
        UAnimationStateMachineGraph* Graph, UAnimStateNode* From, UAnimStateNode* To)
    {
        if (!Graph || !From || !To) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
            {
                if (Transition->GetPreviousState() == From && Transition->GetNextState() == To)
                {
                    return Transition;
                }
            }
        }
        return nullptr;
    }

    /** Resolve a UAnimationAsset from a `/Game/...` path or short name. */
    UAnimationAsset* AnimationGraphEdit_ResolveAnimAsset(const FString& Input)
    {
        if (Input.IsEmpty()) return nullptr;
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UAnimationAsset>(UEditorAssetLibrary::LoadAsset(Input));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UAnimationAsset::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UAnimationAsset>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    /** Walk every UAnimGraphNode_AssetPlayerBase already inside the
     *  state's BoundGraph. Used by set_state_animation so a second
     *  call swaps the player's asset instead of spawning a parallel
     *  player. */
    UAnimGraphNode_AssetPlayerBase* AnimationGraphEdit_FindExistingPlayer(UAnimationStateGraph* StateGraph)
    {
        if (!StateGraph) return nullptr;
        for (UEdGraphNode* Node : StateGraph->Nodes)
        {
            if (UAnimGraphNode_AssetPlayerBase* Player = Cast<UAnimGraphNode_AssetPlayerBase>(Node))
            {
                return Player;
            }
        }
        return nullptr;
    }
}
#endif

TSharedPtr<FJsonObject> FSproftAnimationGraphEditCommands::HandleAddTransition(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("animation_graph_edit add_transition requires WITH_EDITOR (the editor-only AnimGraph module)"));
#else
    FString AnimBpParam;
    if (!Params->TryGetStringField(TEXT("anim_bp"), AnimBpParam)
        && !Params->TryGetStringField(TEXT("blueprint"), AnimBpParam)
        && !Params->TryGetStringField(TEXT("path"), AnimBpParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'anim_bp' parameter"));
    }
    UAnimBlueprint* AnimBP = ResolveAnimBlueprint(AnimBpParam);
    if (!AnimBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UAnimBlueprint '%s'"), *AnimBpParam));
    }

    FString MachineName;
    if (!Params->TryGetStringField(TEXT("state_machine"), MachineName)
        && !Params->TryGetStringField(TEXT("machine"), MachineName)
        && !Params->TryGetStringField(TEXT("machine_name"), MachineName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'state_machine' parameter"));
    }

    FString FromStateName;
    if (!Params->TryGetStringField(TEXT("from_state"), FromStateName)
        && !Params->TryGetStringField(TEXT("from"), FromStateName)
        && !Params->TryGetStringField(TEXT("previous_state"), FromStateName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_state' parameter"));
    }

    FString ToStateName;
    if (!Params->TryGetStringField(TEXT("to_state"), ToStateName)
        && !Params->TryGetStringField(TEXT("to"), ToStateName)
        && !Params->TryGetStringField(TEXT("next_state"), ToStateName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_state' parameter"));
    }

    UAnimGraphNode_StateMachineBase* StateMachineNode = AnimationGraphEdit_FindStateMachineNode(AnimBP, MachineName);
    if (!StateMachineNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find state machine '%s' on AnimBP '%s'"), *MachineName, *AnimBP->GetName()));
    }
    UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode->EditorStateMachineGraph;
    if (!StateMachineGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("State machine '%s' has no editor graph (uncompiled or stripped)"), *MachineName));
    }

    UAnimStateNode* FromState = AnimationGraphEdit_FindStateByName(StateMachineGraph, FromStateName);
    if (!FromState)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find state '%s' on state machine '%s'"), *FromStateName, *MachineName));
    }
    UAnimStateNode* ToState = AnimationGraphEdit_FindStateByName(StateMachineGraph, ToStateName);
    if (!ToState)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find state '%s' on state machine '%s'"), *ToStateName, *MachineName));
    }
    if (FromState == ToState)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("'from_state' and 'to_state' resolve to the same state; transitions need two distinct states"));
    }

    if (UAnimStateTransitionNode* Existing = AnimationGraphEdit_FindTransitionBetween(StateMachineGraph, FromState, ToState))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Transition '%s' -> '%s' already exists on state machine '%s'"),
                *FromStateName, *ToStateName, *MachineName));
    }

    int32 PriorityOrder = 1;
    double PriorityDouble = 0.0;
    if (Params->TryGetNumberField(TEXT("priority"), PriorityDouble))
    {
        PriorityOrder = static_cast<int32>(PriorityDouble);
    }
    else if (Params->TryGetNumberField(TEXT("priority_order"), PriorityDouble))
    {
        PriorityOrder = static_cast<int32>(PriorityDouble);
    }

    bool bCompile = true;
    bool bSave = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Lay the transition between the two state pins. We mirror the
    // canonical engine path that
    // `UAnimationStateMachineSchema::CreateAutomaticConversionNodeAndConnections`
    // takes: spawn a UAnimStateTransitionNode through the public
    // schema-action template (PostPlacedNewNode wires the
    // BoundGraph that holds the rule sub-graph) and then call
    // CreateConnections(From, To) so the arrow points the right
    // direction.
    const FVector2f Location(
        (FromState->NodePosX + ToState->NodePosX) * 0.5f,
        (FromState->NodePosY + ToState->NodePosY) * 0.5f);
    UAnimStateTransitionNode* TransitionTemplate = NewObject<UAnimStateTransitionNode>();
    UAnimStateTransitionNode* TransitionNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
        StateMachineGraph, TransitionTemplate, Location, /*bSelectNewNode=*/false);
    if (!TransitionNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FEdGraphSchemaAction_NewStateNode::PerformAction returned null spawning a transition on state machine '%s'"),
                *MachineName));
    }

    TransitionNode->CreateConnections(FromState, ToState);
    TransitionNode->PriorityOrder = PriorityOrder;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    if (bCompile)
    {
        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::None, &Results);
    }
    AnimBP->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AnimBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_transition"));
    Out->SetStringField(TEXT("anim_bp"), AnimBP->GetPathName());
    Out->SetStringField(TEXT("state_machine"), MachineName);
    Out->SetStringField(TEXT("from_state"), FromState->GetStateName());
    Out->SetStringField(TEXT("to_state"), ToState->GetStateName());
    Out->SetStringField(TEXT("transition_node"), TransitionNode->GetFName().ToString());
    Out->SetNumberField(TEXT("priority_order"), TransitionNode->PriorityOrder);
    if (UEdGraph* RuleGraph = TransitionNode->BoundGraph)
    {
        Out->SetStringField(TEXT("bound_graph"), RuleGraph->GetName());
        Out->SetStringField(TEXT("bound_graph_path"), RuleGraph->GetPathName());
    }
    Out->SetBoolField(TEXT("compiled"), bCompile);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#endif
}

TSharedPtr<FJsonObject> FSproftAnimationGraphEditCommands::HandleSetStateAnimation(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        TEXT("animation_graph_edit set_state_animation requires WITH_EDITOR (the editor-only AnimGraph module)"));
#else
    FString AnimBpParam;
    if (!Params->TryGetStringField(TEXT("anim_bp"), AnimBpParam)
        && !Params->TryGetStringField(TEXT("blueprint"), AnimBpParam)
        && !Params->TryGetStringField(TEXT("path"), AnimBpParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'anim_bp' parameter"));
    }
    UAnimBlueprint* AnimBP = ResolveAnimBlueprint(AnimBpParam);
    if (!AnimBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UAnimBlueprint '%s'"), *AnimBpParam));
    }

    FString MachineName;
    if (!Params->TryGetStringField(TEXT("state_machine"), MachineName)
        && !Params->TryGetStringField(TEXT("machine"), MachineName)
        && !Params->TryGetStringField(TEXT("machine_name"), MachineName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'state_machine' parameter"));
    }

    FString StateName;
    if (!Params->TryGetStringField(TEXT("state_name"), StateName)
        && !Params->TryGetStringField(TEXT("state"), StateName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'state_name' parameter"));
    }

    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("animation"), AssetParam)
        && !Params->TryGetStringField(TEXT("asset"), AssetParam)
        && !Params->TryGetStringField(TEXT("anim_asset"), AssetParam)
        && !Params->TryGetStringField(TEXT("sequence"), AssetParam)
        && !Params->TryGetStringField(TEXT("blend_space"), AssetParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'animation' parameter (UAnimSequence or UBlendSpace path / short name)"));
    }
    UAnimationAsset* Asset = AnimationGraphEdit_ResolveAnimAsset(AssetParam);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UAnimationAsset '%s'"), *AssetParam));
    }

    UAnimGraphNode_StateMachineBase* StateMachineNode = AnimationGraphEdit_FindStateMachineNode(AnimBP, MachineName);
    if (!StateMachineNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find state machine '%s' on AnimBP '%s'"), *MachineName, *AnimBP->GetName()));
    }
    UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode->EditorStateMachineGraph;
    if (!StateMachineGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("State machine '%s' has no editor graph (uncompiled or stripped)"), *MachineName));
    }
    UAnimStateNode* StateNode = AnimationGraphEdit_FindStateByName(StateMachineGraph, StateName);
    if (!StateNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find state '%s' on state machine '%s'"), *StateName, *MachineName));
    }

    UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
    if (!StateGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("State '%s' has no UAnimationStateGraph BoundGraph"), *StateName));
    }
    UAnimGraphNode_StateResult* ResultNode = StateGraph->GetResultNode();
    if (!ResultNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("State '%s' BoundGraph has no MyResultNode (broken state)"), *StateName));
    }

    // Resolve the right player class for this asset shape (sequence,
    // montage, blend space, aim offset, pose asset, etc.). The
    // helper is the same one the engine calls when an asset is
    // dropped on an animation graph.
    UClass* NewNodeClass = GetNodeClassForAsset(Asset->GetClass());
    if (!NewNodeClass || !NewNodeClass->IsChildOf(UAnimGraphNode_AssetPlayerBase::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No UAnimGraphNode_AssetPlayerBase subclass registered for asset class '%s'"),
                *Asset->GetClass()->GetName()));
    }

    bool bSwappedExisting = false;
    UAnimGraphNode_AssetPlayerBase* PlayerNode = AnimationGraphEdit_FindExistingPlayer(StateGraph);
    if (PlayerNode && PlayerNode->GetClass() == NewNodeClass)
    {
        // Same kind of player already there; just swap its asset.
        PlayerNode->Modify();
        PlayerNode->SetAnimationAsset(Asset);
        PlayerNode->ReconstructNode();
        bSwappedExisting = true;
    }
    else
    {
        // No matching player on the graph (or the player class differs,
        // e.g. swap a sequence player out for a blend space player).
        // Mirror the engine's drop path: spawn the right player class,
        // set its asset, attach to the BoundGraph, and wire the
        // player's pose output to the state's Result pose input.
        const FVector2f SpawnLocation(ResultNode->NodePosX - 280.0f, ResultNode->NodePosY);
        UAnimGraphNode_AssetPlayerBase* NewPlayer =
            NewObject<UAnimGraphNode_AssetPlayerBase>(StateGraph, NewNodeClass, NAME_None, RF_Transactional);
        if (!NewPlayer)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to NewObject a player node of class '%s' on state '%s'"),
                    *NewNodeClass->GetName(), *StateName));
        }
        StateGraph->AddNode(NewPlayer, /*bFromUI=*/false, /*bSelectNewNode=*/false);
        NewPlayer->CreateNewGuid();
        NewPlayer->NodePosX = static_cast<int32>(SpawnLocation.X);
        NewPlayer->NodePosY = static_cast<int32>(SpawnLocation.Y);
        NewPlayer->SetAnimationAsset(Asset);
        NewPlayer->CopySettingsFromAnimationAsset(Asset);
        NewPlayer->AllocateDefaultPins();

        // Wire the player's pose output into the state's Result pin.
        // The pose pin on every UAnimGraphNode_AssetPlayerBase is
        // named "Pose"; use the K2 schema's MakeLinkTo so type checks
        // run.
        UEdGraphPin* PlayerPosePin = NewPlayer->FindPin(TEXT("Pose"), EGPD_Output);
        UEdGraphPin* ResultPosePin = ResultNode->FindPin(TEXT("Result"));
        if (PlayerPosePin && ResultPosePin)
        {
            PlayerPosePin->MakeLinkTo(ResultPosePin);
        }

        PlayerNode = NewPlayer;
    }

    bool bCompile = true;
    bool bSave = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    Params->TryGetBoolField(TEXT("save"), bSave);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    if (bCompile)
    {
        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::None, &Results);
    }
    AnimBP->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AnimBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_state_animation"));
    Out->SetStringField(TEXT("anim_bp"), AnimBP->GetPathName());
    Out->SetStringField(TEXT("state_machine"), MachineName);
    Out->SetStringField(TEXT("state_name"), StateName);
    Out->SetStringField(TEXT("animation_path"), Asset->GetPathName());
    Out->SetStringField(TEXT("animation_class"), Asset->GetClass()->GetName());
    Out->SetStringField(TEXT("player_class"), PlayerNode->GetClass()->GetName());
    Out->SetStringField(TEXT("player_node"), PlayerNode->GetFName().ToString());
    Out->SetBoolField(TEXT("swapped_existing_player"), bSwappedExisting);
    Out->SetBoolField(TEXT("compiled"), bCompile);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
#endif
}
