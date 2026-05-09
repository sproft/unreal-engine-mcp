#include "Commands/SproftIkRigEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigSolverBase.h"
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

    TSharedPtr<FJsonObject> TransformToJson(const FTransform& Xf)
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
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("ik_rig_edit: unsupported op '%s'. Supported: inspect"), *Op));
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
        GoalObj->SetObjectField(TEXT("current_transform"), TransformToJson(Goal->CurrentTransform));
        GoalObj->SetObjectField(TEXT("initial_transform"), TransformToJson(Goal->InitialTransform));
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
