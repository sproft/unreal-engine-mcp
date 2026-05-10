#include "Commands/SproftIkRigEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "RigEditor/IKRigController.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"

namespace
{
    UIKRigDefinition* ResolveRig(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("rig"), Input)
            && !Params->TryGetStringField(TEXT("path"), Input)
            && !Params->TryGetStringField(TEXT("asset"), Input)
            && !Params->TryGetStringField(TEXT("asset_path"), Input))
        {
            return nullptr;
        }
        if (Input.IsEmpty())
        {
            return nullptr;
        }
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UIKRigDefinition>(UEditorAssetLibrary::LoadAsset(Input));
        }
        // Short-name fallback through the asset registry.
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UIKRigDefinition::StaticClass()->GetClassPathName(), Found);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UIKRigDefinition>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> IkRigEdit_TransformToJson(const FTransform& Xf)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        const FVector L = Xf.GetLocation();
        const FRotator R = Xf.Rotator();
        const FVector S = Xf.GetScale3D();

        TArray<TSharedPtr<FJsonValue>> LArr;
        LArr.Add(MakeShared<FJsonValueNumber>(L.X));
        LArr.Add(MakeShared<FJsonValueNumber>(L.Y));
        LArr.Add(MakeShared<FJsonValueNumber>(L.Z));
        Out->SetArrayField(TEXT("location"), LArr);

        TArray<TSharedPtr<FJsonValue>> RArr;
        RArr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        RArr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        RArr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        Out->SetArrayField(TEXT("rotation"), RArr);

        TArray<TSharedPtr<FJsonValue>> SArr;
        SArr.Add(MakeShared<FJsonValueNumber>(S.X));
        SArr.Add(MakeShared<FJsonValueNumber>(S.Y));
        SArr.Add(MakeShared<FJsonValueNumber>(S.Z));
        Out->SetArrayField(TEXT("scale"), SArr);

        return Out;
    }

    /** Walk every UPROPERTY on `Struct` rooted at `ContainerPtr` and emit
     *  a flat `{name: ExportText_Direct value}` dict. We skip the
     *  base-struct opt-in fields (`Goal`, `Bone`) when the caller asks
     *  for the user-visible settings only. */
    void DumpStructProperties(const UScriptStruct* Struct, const void* ContainerPtr, TSharedPtr<FJsonObject>& Out)
    {
        if (!Struct || !ContainerPtr)
        {
            return;
        }
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop)
            {
                continue;
            }
            const void* Value = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);
            FString Text;
            Prop->ExportText_Direct(Text, Value, Value, nullptr, PPF_None);
            Out->SetStringField(Prop->GetName(), Text);
        }
    }
}

FSproftIkRigEditCommands::FSproftIkRigEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("ik_rig_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown ik_rig_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("inspect"))
    {
        return HandleInspect(Params);
    }
    if (Op.Equals(TEXT("set_retarget_root"), ESearchCase::IgnoreCase))
    {
        return HandleSetRetargetRoot(Params);
    }
    if (Op.Equals(TEXT("add_retarget_chain"), ESearchCase::IgnoreCase))
    {
        return HandleAddRetargetChain(Params);
    }
    if (Op.Equals(TEXT("add_ik_goal"), ESearchCase::IgnoreCase))
    {
        return HandleAddIkGoal(Params);
    }
    if (Op.Equals(TEXT("add_solver"), ESearchCase::IgnoreCase))
    {
        return HandleAddSolver(Params);
    }
    if (Op.Equals(TEXT("remove_solver_at"), ESearchCase::IgnoreCase)
        || Op.Equals(TEXT("remove_solver"), ESearchCase::IgnoreCase))
    {
        return HandleRemoveSolverAt(Params);
    }
    if (Op.Equals(TEXT("set_solver_settings"), ESearchCase::IgnoreCase))
    {
        return HandleSetSolverSettings(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("ik_rig_edit: unsupported op '%s'. Supported: inspect, set_retarget_root, add_retarget_chain, add_ik_goal, add_solver, remove_solver_at, set_solver_settings"), *Op));
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleInspect(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }

    bool bIncludeSolverSettings = true;
    Params->TryGetBoolField(TEXT("include_solver_settings"), bIncludeSolverSettings);
    bool bIncludeBoneSettings = true;
    Params->TryGetBoolField(TEXT("include_bone_settings"), bIncludeBoneSettings);

    int32 MaxChains = 256;
    int32 MaxGoals = 256;
    int32 MaxSolvers = 64;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_chains"), TempNum)) MaxChains = FMath::Max(0, static_cast<int32>(TempNum));
    if (Params->TryGetNumberField(TEXT("max_goals"), TempNum)) MaxGoals = FMath::Max(0, static_cast<int32>(TempNum));
    if (Params->TryGetNumberField(TEXT("max_solvers"), TempNum)) MaxSolvers = FMath::Max(0, static_cast<int32>(TempNum));

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("name"), Rig->GetName());
    Out->SetStringField(TEXT("path"), Rig->GetPathName());
    Out->SetStringField(TEXT("class"), Rig->GetClass()->GetName());

    // Preview skeletal mesh (when set on the asset).
    if (USkeletalMesh* PreviewMesh = Rig->GetPreviewMesh())
    {
        Out->SetStringField(TEXT("preview_skeletal_mesh_path"), PreviewMesh->GetPathName());
    }
    else
    {
        Out->SetStringField(TEXT("preview_skeletal_mesh_path"), FString());
    }

    // Retarget root.
    Out->SetStringField(TEXT("retarget_root_bone"), Rig->GetPelvis().ToString());

    // Retarget chains.
    const TArray<FBoneChain>& Chains = Rig->GetRetargetChains();
    Out->SetNumberField(TEXT("chain_count_total"), Chains.Num());
    TArray<TSharedPtr<FJsonValue>> ChainArr;
    for (int32 ChainIdx = 0; ChainIdx < Chains.Num() && ChainArr.Num() < MaxChains; ++ChainIdx)
    {
        const FBoneChain& Chain = Chains[ChainIdx];
        TSharedPtr<FJsonObject> ChainObj = MakeShared<FJsonObject>();
        ChainObj->SetNumberField(TEXT("index"), ChainIdx);
        ChainObj->SetStringField(TEXT("chain_name"), Chain.ChainName.ToString());
        ChainObj->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
        ChainObj->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
        ChainObj->SetStringField(TEXT("ik_goal_name"), Chain.IKGoalName.ToString());
        ChainArr.Add(MakeShared<FJsonValueObject>(ChainObj));
    }
    Out->SetArrayField(TEXT("chains"), ChainArr);
    Out->SetNumberField(TEXT("chain_count"), ChainArr.Num());
    Out->SetBoolField(TEXT("chains_truncated"), ChainArr.Num() < Chains.Num());

    // IK goals.
    const TArray<UIKRigEffectorGoal*>& Goals = Rig->GetGoalArray();
    Out->SetNumberField(TEXT("goal_count_total"), Goals.Num());
    TArray<TSharedPtr<FJsonValue>> GoalArr;
    for (int32 GoalIdx = 0; GoalIdx < Goals.Num() && GoalArr.Num() < MaxGoals; ++GoalIdx)
    {
        const UIKRigEffectorGoal* Goal = Goals[GoalIdx];
        if (!Goal)
        {
            continue;
        }
        TSharedPtr<FJsonObject> GoalObj = MakeShared<FJsonObject>();
        GoalObj->SetNumberField(TEXT("index"), GoalIdx);
        GoalObj->SetStringField(TEXT("goal_name"), Goal->GoalName.ToString());
        GoalObj->SetStringField(TEXT("bone_name"), Goal->BoneName.ToString());
        GoalObj->SetNumberField(TEXT("position_alpha"), Goal->PositionAlpha);
        GoalObj->SetNumberField(TEXT("rotation_alpha"), Goal->RotationAlpha);
        GoalObj->SetObjectField(TEXT("current_transform"), IkRigEdit_TransformToJson(Goal->CurrentTransform));
        GoalObj->SetObjectField(TEXT("initial_transform"), IkRigEdit_TransformToJson(Goal->InitialTransform));
        GoalArr.Add(MakeShared<FJsonValueObject>(GoalObj));
    }
    Out->SetArrayField(TEXT("goals"), GoalArr);
    Out->SetNumberField(TEXT("goal_count"), GoalArr.Num());
    Out->SetBoolField(TEXT("goals_truncated"), GoalArr.Num() < Goals.Num());

    // Solver stack walk.
    const TArray<FInstancedStruct>& Solvers = Rig->GetSolverStructs();
    Out->SetNumberField(TEXT("solver_count_total"), Solvers.Num());
    TArray<TSharedPtr<FJsonValue>> SolverArr;
    int32 BoneSettingCount = 0;
    for (int32 SolverIdx = 0; SolverIdx < Solvers.Num() && SolverArr.Num() < MaxSolvers; ++SolverIdx)
    {
        const FInstancedStruct& SolverStruct = Solvers[SolverIdx];
        TSharedPtr<FJsonObject> SolverObj = MakeShared<FJsonObject>();
        SolverObj->SetNumberField(TEXT("index"), SolverIdx);

        const UScriptStruct* StructType = SolverStruct.GetScriptStruct();
        if (StructType)
        {
            SolverObj->SetStringField(TEXT("struct_type"), StructType->GetName());
            SolverObj->SetStringField(TEXT("struct_path"), StructType->GetPathName());
        }

        const FIKRigSolverBase* SolverPtr = SolverStruct.GetPtr<FIKRigSolverBase>();
        if (!SolverPtr)
        {
            SolverObj->SetBoolField(TEXT("solver_resolved"), false);
            SolverArr.Add(MakeShared<FJsonValueObject>(SolverObj));
            continue;
        }
        SolverObj->SetBoolField(TEXT("solver_resolved"), true);
        SolverObj->SetBoolField(TEXT("enabled"), SolverPtr->IsEnabled());

        FIKRigSolverBase* MutableSolver = const_cast<FIKRigSolverBase*>(SolverPtr);

        if (MutableSolver->UsesStartBone())
        {
            SolverObj->SetStringField(TEXT("start_bone"), MutableSolver->GetStartBone().ToString());
        }
        if (MutableSolver->UsesEndBone())
        {
            SolverObj->SetStringField(TEXT("end_bone"), MutableSolver->GetEndBone().ToString());
        }

        // Solver-level settings reflection dump. The settings struct is
        // returned by GetSolverSettings() and described by
        // GetSolverSettingsType(), so we can walk both blindly without
        // hard-coding per-solver fields.
        if (bIncludeSolverSettings)
        {
            const UScriptStruct* SettingsType = MutableSolver->GetSolverSettingsType();
            const FIKRigSolverSettingsBase* SettingsPtr = MutableSolver->GetSolverSettings();
            if (SettingsType && SettingsPtr)
            {
                SolverObj->SetStringField(TEXT("settings_type"), SettingsType->GetName());
                SolverObj->SetStringField(TEXT("settings_path"), SettingsType->GetPathName());
                TSharedPtr<FJsonObject> SettingsObj = MakeShared<FJsonObject>();
                DumpStructProperties(SettingsType, SettingsPtr, SettingsObj);
                SolverObj->SetObjectField(TEXT("settings"), SettingsObj);
            }
        }

        // Per-solver bone-setting dump. Each solver type gates the
        // call through UsesCustomBoneSettings(), and emits a per-bone
        // FIKRigBoneSettingsBase-derived row.
        if (bIncludeBoneSettings && MutableSolver->UsesCustomBoneSettings())
        {
            TSet<FName> BonesWithSettings;
            MutableSolver->GetBonesWithSettings(BonesWithSettings);
            const UScriptStruct* BoneSettingsType = MutableSolver->GetBoneSettingsType();
            TArray<TSharedPtr<FJsonValue>> BoneSettingsArr;
            for (const FName& BoneName : BonesWithSettings)
            {
                FIKRigBoneSettingsBase* PerBone = MutableSolver->GetBoneSettings(BoneName);
                if (!PerBone)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
                BoneObj->SetStringField(TEXT("bone_name"), BoneName.ToString());
                if (BoneSettingsType)
                {
                    BoneObj->SetStringField(TEXT("settings_type"), BoneSettingsType->GetName());
                    DumpStructProperties(BoneSettingsType, PerBone, BoneObj);
                }
                BoneSettingsArr.Add(MakeShared<FJsonValueObject>(BoneObj));
                ++BoneSettingCount;
            }
            SolverObj->SetArrayField(TEXT("bone_settings"), BoneSettingsArr);
            SolverObj->SetNumberField(TEXT("bone_settings_count"), BoneSettingsArr.Num());
        }

        SolverArr.Add(MakeShared<FJsonValueObject>(SolverObj));
    }
    Out->SetArrayField(TEXT("solvers"), SolverArr);
    Out->SetNumberField(TEXT("solver_count"), SolverArr.Num());
    Out->SetBoolField(TEXT("solvers_truncated"), SolverArr.Num() < Solvers.Num());
    Out->SetNumberField(TEXT("bone_setting_count"), BoneSettingCount);

    return Out;
}

namespace
{
    /** Resolve the editor controller for a UIKRigDefinition. The
     *  controller is the documented mutate path; bypassing it leaves
     *  the asset in a half-constructed state. */
    UIKRigController* GetRigController(UIKRigDefinition* Rig)
    {
        return Rig ? UIKRigController::GetController(Rig) : nullptr;
    }

    void SaveRigIfRequested(UIKRigDefinition* Rig, bool bSave)
    {
        if (!Rig) return;
        Rig->MarkPackageDirty();
        if (bSave)
        {
            UEditorAssetLibrary::SaveAsset(Rig->GetPathName(), /*bOnlyIfIsDirty=*/false);
        }
    }
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleSetRetargetRoot(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }
    UIKRigController* Controller = GetRigController(Rig);
    if (!Controller)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UIKRigController for '%s'"), *Rig->GetPathName()));
    }

    FString BoneToken;
    if (!Params->TryGetStringField(TEXT("bone"), BoneToken)
        && !Params->TryGetStringField(TEXT("bone_name"), BoneToken)
        && !Params->TryGetStringField(TEXT("retarget_root"), BoneToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'bone' parameter"));
    }
    const FName BoneName(*BoneToken);
    const bool bSetOk = Controller->SetRetargetRoot(BoneName);
    if (!bSetOk)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRigController::SetRetargetRoot refused bone '%s' on rig '%s'"),
                *BoneToken, *Rig->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveRigIfRequested(Rig, bSave);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_retarget_root"));
    Out->SetStringField(TEXT("rig"), Rig->GetPathName());
    Out->SetStringField(TEXT("retarget_root_bone"), BoneName.ToString());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleAddRetargetChain(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }
    UIKRigController* Controller = GetRigController(Rig);
    if (!Controller)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UIKRigController for '%s'"), *Rig->GetPathName()));
    }

    FString ChainNameStr;
    Params->TryGetStringField(TEXT("chain_name"), ChainNameStr);
    if (ChainNameStr.IsEmpty()) Params->TryGetStringField(TEXT("name"), ChainNameStr);
    if (ChainNameStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'chain_name' parameter"));
    }

    FString StartBoneStr;
    FString EndBoneStr;
    Params->TryGetStringField(TEXT("start_bone"), StartBoneStr);
    Params->TryGetStringField(TEXT("end_bone"), EndBoneStr);
    if (StartBoneStr.IsEmpty() || EndBoneStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Both 'start_bone' and 'end_bone' are required"));
    }
    FString GoalNameStr;
    Params->TryGetStringField(TEXT("ik_goal_name"), GoalNameStr);
    if (GoalNameStr.IsEmpty()) Params->TryGetStringField(TEXT("goal_name"), GoalNameStr);

    const FName ChainName(*ChainNameStr);
    const FName StartBone(*StartBoneStr);
    const FName EndBone(*EndBoneStr);
    const FName GoalName = GoalNameStr.IsEmpty() ? NAME_None : FName(*GoalNameStr);

    const FName ResolvedChainName = Controller->AddRetargetChain(ChainName, StartBone, EndBone, GoalName);
    if (ResolvedChainName.IsNone())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRigController::AddRetargetChain rejected chain '%s' (start=%s end=%s) on rig '%s'"),
                *ChainNameStr, *StartBoneStr, *EndBoneStr, *Rig->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveRigIfRequested(Rig, bSave);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_retarget_chain"));
    Out->SetStringField(TEXT("rig"), Rig->GetPathName());
    Out->SetStringField(TEXT("chain_name"), ResolvedChainName.ToString());
    Out->SetStringField(TEXT("start_bone"), StartBoneStr);
    Out->SetStringField(TEXT("end_bone"), EndBoneStr);
    if (!GoalName.IsNone())
    {
        Out->SetStringField(TEXT("ik_goal_name"), GoalName.ToString());
    }
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleAddIkGoal(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }
    UIKRigController* Controller = GetRigController(Rig);
    if (!Controller)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UIKRigController for '%s'"), *Rig->GetPathName()));
    }

    FString GoalNameStr;
    Params->TryGetStringField(TEXT("goal_name"), GoalNameStr);
    if (GoalNameStr.IsEmpty()) Params->TryGetStringField(TEXT("name"), GoalNameStr);
    if (GoalNameStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'goal_name' parameter"));
    }

    FString BoneStr;
    Params->TryGetStringField(TEXT("bone"), BoneStr);
    if (BoneStr.IsEmpty()) Params->TryGetStringField(TEXT("bone_name"), BoneStr);
    if (BoneStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'bone' parameter"));
    }

    const FName GoalName(*GoalNameStr);
    const FName BoneName(*BoneStr);
    const FName ResolvedGoalName = Controller->AddNewGoal(GoalName, BoneName);
    if (ResolvedGoalName.IsNone())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRigController::AddNewGoal rejected goal '%s' (bone=%s) on rig '%s'"),
                *GoalNameStr, *BoneStr, *Rig->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveRigIfRequested(Rig, bSave);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_ik_goal"));
    Out->SetStringField(TEXT("rig"), Rig->GetPathName());
    Out->SetStringField(TEXT("goal_name"), ResolvedGoalName.ToString());
    Out->SetStringField(TEXT("bone_name"), BoneStr);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

namespace
{
    /** Resolve a UIKRigSolverBase-derived UScriptStruct from a string
     *  token. Accepts a full `/Script/Module.FStructName` path, a
     *  bare struct name (probes the loaded UScriptStruct set), and
     *  short tokens for the stock solvers shipped by the IKRig
     *  plugin (full_body / fbik / limb / pole / body_mover /
     *  set_transform). */
    UScriptStruct* IkRigEdit_ResolveSolverStruct(const FString& Token)
    {
        if (Token.IsEmpty()) return nullptr;
        if (Token.StartsWith(TEXT("/")))
        {
            return Cast<UScriptStruct>(StaticLoadObject(UScriptStruct::StaticClass(), nullptr, *Token));
        }
        // Short-name table for the stock solvers; we keep the table
        // small on purpose so callers stay aware of which solver
        // they're appending.
        static const TMap<FString, FString> Aliases =
        {
            { TEXT("full_body"),     TEXT("/Script/IKRig.IKRigFBIKSolver") },
            { TEXT("fbik"),          TEXT("/Script/IKRig.IKRigFBIKSolver") },
            { TEXT("full_body_ik"),  TEXT("/Script/IKRig.IKRigFBIKSolver") },
            { TEXT("limb"),          TEXT("/Script/IKRig.IKRigLimbSolver") },
            { TEXT("pole"),          TEXT("/Script/IKRig.IKRigPoleSolver") },
            { TEXT("body_mover"),    TEXT("/Script/IKRig.IKRigBodyMoverSolver") },
            { TEXT("bodymover"),     TEXT("/Script/IKRig.IKRigBodyMoverSolver") },
            { TEXT("set_transform"), TEXT("/Script/IKRig.IKRigSetTransformSolver") },
        };
        if (const FString* Hit = Aliases.Find(Token.ToLower()))
        {
            return Cast<UScriptStruct>(StaticLoadObject(UScriptStruct::StaticClass(), nullptr, **Hit));
        }
        // Bare struct name fallback: probe the loaded UScriptStruct
        // set with optional `F` prefix variants.
        for (TObjectIterator<UScriptStruct> It; It; ++It)
        {
            UScriptStruct* Candidate = *It;
            if (!Candidate) continue;
            const FString Name = Candidate->GetName();
            if (Name.Equals(Token, ESearchCase::IgnoreCase)
                || Name.Equals(FString(TEXT("F")) + Token, ESearchCase::IgnoreCase))
            {
                if (Candidate->IsChildOf(FIKRigSolverBase::StaticStruct()))
                {
                    return Candidate;
                }
            }
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleAddSolver(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }
    UIKRigController* Controller = GetRigController(Rig);
    if (!Controller)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UIKRigController for '%s'"), *Rig->GetPathName()));
    }

    FString SolverToken;
    if (!Params->TryGetStringField(TEXT("solver_type"), SolverToken)
        && !Params->TryGetStringField(TEXT("type"), SolverToken)
        && !Params->TryGetStringField(TEXT("struct_path"), SolverToken)
        && !Params->TryGetStringField(TEXT("class"), SolverToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'solver_type' parameter (full_body / limb / pole / body_mover / set_transform / `/Script/Module.FStructName` / bare struct name)"));
    }

    UScriptStruct* SolverStruct = IkRigEdit_ResolveSolverStruct(SolverToken);
    if (!SolverStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UScriptStruct for solver token '%s'"), *SolverToken));
    }
    if (!SolverStruct->IsChildOf(FIKRigSolverBase::StaticStruct()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Resolved struct '%s' is not a FIKRigSolverBase derivative"), *SolverStruct->GetPathName()));
    }

    // The 5.6 polymorphic op-stack accepts either an FString
    // struct path (BlueprintCallable overload) or a UScriptStruct*
    // (C++ overload). Use the typed overload so we control the
    // resolved struct exactly.
    const int32 NewIndex = Controller->AddSolver(SolverStruct);
    if (NewIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRigController::AddSolver returned INDEX_NONE for '%s'"), *SolverStruct->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveRigIfRequested(Rig, bSave);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("add_solver"));
    Out->SetStringField(TEXT("rig"), Rig->GetPathName());
    Out->SetNumberField(TEXT("index"), NewIndex);
    Out->SetStringField(TEXT("struct_type"), SolverStruct->GetName());
    Out->SetStringField(TEXT("struct_path"), SolverStruct->GetPathName());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleRemoveSolverAt(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }
    UIKRigController* Controller = GetRigController(Rig);
    if (!Controller)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UIKRigController for '%s'"), *Rig->GetPathName()));
    }

    int32 Index = INDEX_NONE;
    double IndexDouble = 0.0;
    if (Params->TryGetNumberField(TEXT("index"), IndexDouble))
    {
        Index = static_cast<int32>(IndexDouble);
    }
    else if (Params->TryGetNumberField(TEXT("solver_index"), IndexDouble))
    {
        Index = static_cast<int32>(IndexDouble);
    }
    if (Index < 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing or negative 'index' parameter"));
    }

    const int32 NumBefore = Controller->GetNumSolvers();
    if (Index >= NumBefore)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Solver index %d is out of range (0..%d) on rig '%s'"),
                Index, NumBefore - 1, *Rig->GetPathName()));
    }

    // Capture the struct identity for the response before the
    // remove call invalidates the slot.
    FString RemovedStructPath;
    if (FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(Index))
    {
        if (const UScriptStruct* StructType = SolverStruct->GetScriptStruct())
        {
            RemovedStructPath = StructType->GetPathName();
        }
    }

    const bool bRemoved = Controller->RemoveSolver(Index);
    if (!bRemoved)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRigController::RemoveSolver(%d) refused on rig '%s'"),
                Index, *Rig->GetPathName()));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveRigIfRequested(Rig, bSave);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("remove_solver_at"));
    Out->SetStringField(TEXT("rig"), Rig->GetPathName());
    Out->SetNumberField(TEXT("index"), Index);
    Out->SetNumberField(TEXT("solver_count"), Controller->GetNumSolvers());
    if (!RemovedStructPath.IsEmpty())
    {
        Out->SetStringField(TEXT("removed_struct_path"), RemovedStructPath);
    }
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftIkRigEditCommands::HandleSetSolverSettings(const TSharedPtr<FJsonObject>& Params)
{
    UIKRigDefinition* Rig = ResolveRig(Params);
    if (!Rig)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRigDefinition (provide 'rig' or 'path' as /Game/... or short name)"));
    }
    UIKRigController* Controller = GetRigController(Rig);
    if (!Controller)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UIKRigController for '%s'"), *Rig->GetPathName()));
    }

    int32 Index = INDEX_NONE;
    double IndexDouble = 0.0;
    if (Params->TryGetNumberField(TEXT("index"), IndexDouble))
    {
        Index = static_cast<int32>(IndexDouble);
    }
    else if (Params->TryGetNumberField(TEXT("solver_index"), IndexDouble))
    {
        Index = static_cast<int32>(IndexDouble);
    }
    if (Index < 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing or negative 'index' parameter"));
    }

    if (Index >= Controller->GetNumSolvers())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Solver index %d is out of range (0..%d) on rig '%s'"),
                Index, Controller->GetNumSolvers() - 1, *Rig->GetPathName()));
    }

    FIKRigSolverBase* Solver = Controller->GetSolverAtIndex(Index);
    if (!Solver)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve FIKRigSolverBase at index %d"), Index));
    }
    const UScriptStruct* SettingsType = Solver->GetSolverSettingsType();
    FIKRigSolverSettingsBase* SettingsPtr = Solver->GetSolverSettings();
    if (!SettingsType || !SettingsPtr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Solver at index %d does not expose a settings struct"), Index));
    }

    const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropertiesObj) || !PropertiesObj || !PropertiesObj->IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'properties' object (flat dict of property name -> string value)"));
    }

    TArray<TSharedPtr<FJsonValue>> AppliedArr;
    TArray<TSharedPtr<FJsonValue>> SkippedArr;
    void* SettingsContainer = static_cast<void*>(SettingsPtr);

    for (const auto& Entry : (*PropertiesObj)->Values)
    {
        const FString& PropName = Entry.Key;
        const TSharedPtr<FJsonValue>& Value = Entry.Value;

        FProperty* Prop = SettingsType->FindPropertyByName(FName(*PropName));
        if (!Prop)
        {
            // Case-insensitive fallback walk; FProperty::FindPropertyByName
            // is case-sensitive on FName equality.
            for (TFieldIterator<FProperty> It(SettingsType); It; ++It)
            {
                if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
                {
                    Prop = *It;
                    break;
                }
            }
        }
        if (!Prop)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), PropName);
            Skip->SetStringField(TEXT("reason"), TEXT("unknown_property"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        // Render the JSON value back to a literal string
        // FProperty::ImportText understands. Bools / numbers / strings
        // each go through their natural rendering.
        FString Literal;
        if (!Value.IsValid())
        {
            Literal = TEXT("None");
        }
        else if (Value->Type == EJson::Boolean)
        {
            Literal = Value->AsBool() ? TEXT("True") : TEXT("False");
        }
        else if (Value->Type == EJson::Number)
        {
            Literal = LexToString(Value->AsNumber());
        }
        else if (Value->Type == EJson::String)
        {
            Literal = Value->AsString();
        }
        else
        {
            // Object / array literals: serialise raw and let
            // ImportText handle the deeper grammar (transform, vector
            // etc. callers should pass the canonical text form).
            const TSharedPtr<FJsonObject>* SubObj = nullptr;
            if (Value->TryGetObject(SubObj) && SubObj && SubObj->IsValid())
            {
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Literal);
                FJsonSerializer::Serialize(SubObj->ToSharedRef(), Writer);
            }
            else
            {
                Literal = Value->AsString();
            }
        }

        const TCHAR* ImportResult =
            Prop->ImportText_InContainer(*Literal, SettingsContainer, nullptr, PPF_None);
        if (!ImportResult)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), PropName);
            Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
            Skip->SetStringField(TEXT("attempted"), Literal);
            SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
        Applied->SetStringField(TEXT("name"), PropName);
        Applied->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());
        Applied->SetStringField(TEXT("imported"), Literal);
        AppliedArr.Add(MakeShared<FJsonValueObject>(Applied));
    }

    // Run the solver's official settings setter so any
    // derived-type custom logic (e.g. UpdateSettingsFromAsset
    // mirror copies) fires. The base implementation memcpys the
    // settings struct over the existing pointer.
    if (AppliedArr.Num() > 0)
    {
        Solver->SetSolverSettings(SettingsPtr);
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    SaveRigIfRequested(Rig, bSave);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_solver_settings"));
    Out->SetStringField(TEXT("rig"), Rig->GetPathName());
    Out->SetNumberField(TEXT("index"), Index);
    Out->SetStringField(TEXT("settings_type"), SettingsType->GetName());
    Out->SetStringField(TEXT("settings_path"), SettingsType->GetPathName());
    Out->SetArrayField(TEXT("applied"), AppliedArr);
    Out->SetNumberField(TEXT("applied_count"), AppliedArr.Num());
    Out->SetArrayField(TEXT("skipped"), SkippedArr);
    Out->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}
