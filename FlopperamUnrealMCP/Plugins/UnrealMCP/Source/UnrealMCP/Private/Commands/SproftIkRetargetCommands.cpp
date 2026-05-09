#include "Commands/SproftIkRetargetCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetOps.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Rig/IKRigDefinition.h"
#include "StructUtils/InstancedStruct.h"

namespace
{
    UIKRetargeter* ResolveRetargeter(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("retargeter"), Input)
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
            return Cast<UIKRetargeter>(UEditorAssetLibrary::LoadAsset(Input));
        }
        // Short-name fallback through the asset registry.
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UIKRetargeter::StaticClass()->GetClassPathName(), Found);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UIKRetargeter>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    void DumpIkRig(const UIKRetargeter* Retargeter, ERetargetSourceOrTarget Side, const TCHAR* Field, TSharedPtr<FJsonObject>& Out)
    {
        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        const UIKRigDefinition* Rig = Retargeter ? Retargeter->GetIKRig(Side) : nullptr;
        if (Rig)
        {
            Block->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
            Block->SetStringField(TEXT("ik_rig_name"), Rig->GetName());
            Block->SetBoolField(TEXT("has_ik_rig"), true);
        }
        else
        {
            Block->SetBoolField(TEXT("has_ik_rig"), false);
        }
        Block->SetStringField(TEXT("current_pose"),
            Retargeter ? Retargeter->GetCurrentRetargetPoseName(Side).ToString() : FString());
        if (Retargeter)
        {
            if (const FIKRetargetPose* Pose = Retargeter->GetCurrentRetargetPose(Side))
            {
                Block->SetNumberField(TEXT("current_pose_bone_offset_count"), Pose->GetAllDeltaRotations().Num());
                Block->SetBoolField(TEXT("current_pose_has_root_offset"),
                    !Pose->GetRootTranslationDelta().IsNearlyZero());
            }
        }
        Out->SetObjectField(Field, Block);
    }

    void DumpRetargetOp(const FInstancedStruct& OpStruct, bool bIncludeChainMappings, TSharedPtr<FJsonObject>& Out)
    {
        const UScriptStruct* StructType = OpStruct.GetScriptStruct();
        if (StructType)
        {
            Out->SetStringField(TEXT("struct_type"), StructType->GetName());
            Out->SetStringField(TEXT("struct_path"), StructType->GetPathName());
        }

        const FIKRetargetOpBase* Op = OpStruct.GetPtr<FIKRetargetOpBase>();
        if (!Op)
        {
            Out->SetBoolField(TEXT("op_resolved"), false);
            return;
        }
        Out->SetBoolField(TEXT("op_resolved"), true);
        Out->SetStringField(TEXT("name"), Op->GetName().ToString());
        Out->SetStringField(TEXT("parent_name"), Op->GetParentOpName().ToString());
        Out->SetBoolField(TEXT("enabled"), Op->IsEnabled());
        Out->SetBoolField(TEXT("initialized"), Op->IsInitialized());
        if (const UScriptStruct* ParentType = Op->GetParentOpType())
        {
            Out->SetStringField(TEXT("parent_op_type"), ParentType->GetName());
        }
        Out->SetBoolField(TEXT("can_have_child_ops"), Op->CanHaveChildOps());

        if (const UIKRigDefinition* CustomRig = Op->GetCustomTargetIKRig())
        {
            Out->SetStringField(TEXT("custom_target_ik_rig"), CustomRig->GetPathName());
        }

        if (bIncludeChainMappings)
        {
            // GetChainMapping() is allowed to return null on ops that
            // do not own a per-op chain map; skip silently in that case.
            const FRetargetChainMapping* Mapping = Op->GetChainMapping();
            if (Mapping)
            {
                TArray<TSharedPtr<FJsonValue>> ChainArr;
                for (const FRetargetChainPair& Pair : Mapping->GetChainPairs())
                {
                    TSharedPtr<FJsonObject> PairObj = MakeShared<FJsonObject>();
                    PairObj->SetStringField(TEXT("target_chain"), Pair.TargetChainName.ToString());
                    PairObj->SetStringField(TEXT("source_chain"), Pair.SourceChainName.ToString());
                    ChainArr.Add(MakeShared<FJsonValueObject>(PairObj));
                }
                Out->SetArrayField(TEXT("chain_mapping"), ChainArr);
                Out->SetNumberField(TEXT("chain_mapping_count"), ChainArr.Num());
            }
        }
    }
}

FSproftIkRetargetCommands::FSproftIkRetargetCommands()
{
}

TSharedPtr<FJsonObject> FSproftIkRetargetCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("ik_retarget"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown ik_retarget command: %s"), *CommandType));
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
        FString::Printf(TEXT("ik_retarget: unsupported op '%s'. Supported: inspect"), *Op));
}

TSharedPtr<FJsonObject> FSproftIkRetargetCommands::HandleInspect(const TSharedPtr<FJsonObject>& Params)
{
    UIKRetargeter* Retargeter = ResolveRetargeter(Params);
    if (!Retargeter)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UIKRetargeter (provide 'retargeter' or 'path' as /Game/... or short name)"));
    }

    bool bIncludeChainMappings = true;
    Params->TryGetBoolField(TEXT("include_op_chain_mappings"), bIncludeChainMappings);
    int32 MaxOps = 64;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_ops"), TempNum)) MaxOps = FMath::Max(0, static_cast<int32>(TempNum));

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("name"), Retargeter->GetName());
    Out->SetStringField(TEXT("path"), Retargeter->GetPathName());
    Out->SetStringField(TEXT("class"), Retargeter->GetClass()->GetName());
    Out->SetBoolField(TEXT("has_source_ik_rig"), Retargeter->HasSourceIKRig());
    Out->SetBoolField(TEXT("has_target_ik_rig"), Retargeter->HasTargetIKRig());

    DumpIkRig(Retargeter, ERetargetSourceOrTarget::Source, TEXT("source"), Out);
    DumpIkRig(Retargeter, ERetargetSourceOrTarget::Target, TEXT("target"), Out);

    Out->SetStringField(TEXT("default_pose_name"), UIKRetargeter::GetDefaultPoseName().ToString());

    // Op stack walk.
    const TArray<FInstancedStruct>& Ops = Retargeter->GetRetargetOps();
    Out->SetNumberField(TEXT("op_count_total"), Ops.Num());

    TArray<TSharedPtr<FJsonValue>> OpsArr;
    int32 ChainPairCount = 0;
    for (int32 OpIndex = 0; OpIndex < Ops.Num(); ++OpIndex)
    {
        if (OpsArr.Num() >= MaxOps)
        {
            break;
        }
        TSharedPtr<FJsonObject> OpObj = MakeShared<FJsonObject>();
        OpObj->SetNumberField(TEXT("index"), OpIndex);
        DumpRetargetOp(Ops[OpIndex], bIncludeChainMappings, OpObj);

        // Aggregate chain pair count by reaching into the dumped block
        // so we do not have to walk twice.
        if (bIncludeChainMappings)
        {
            int32 PerOpCount = 0;
            if (OpObj->TryGetNumberField(TEXT("chain_mapping_count"), PerOpCount))
            {
                ChainPairCount += PerOpCount;
            }
        }
        OpsArr.Add(MakeShared<FJsonValueObject>(OpObj));
    }
    Out->SetArrayField(TEXT("ops"), OpsArr);
    Out->SetNumberField(TEXT("op_count"), OpsArr.Num());
    Out->SetBoolField(TEXT("ops_truncated"), OpsArr.Num() < Ops.Num());
    Out->SetNumberField(TEXT("chain_pair_count"), ChainPairCount);

    return Out;
}
