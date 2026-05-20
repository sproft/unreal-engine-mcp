#include "Commands/SproftAnimationInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Animation/AnimTypes.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"

namespace
{
    /** Render an FVector as a [x, y, z] number array. */
    TArray<TSharedPtr<FJsonValue>> AnimationInspect_Vec3ToJson(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    }

    /** Render an FRotator as a [pitch, yaw, roll] number array. */
    TArray<TSharedPtr<FJsonValue>> RotatorToJson(const FRotator& R)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        return Arr;
    }

    /** Render an FFrameRate as `{numerator, denominator, approx_fps}`. */
    TSharedPtr<FJsonObject> AnimationInspect_FrameRateRecord(const FFrameRate& Rate)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("numerator"),   Rate.Numerator);
        Out->SetNumberField(TEXT("denominator"), Rate.Denominator);
        if (Rate.Denominator != 0)
        {
            Out->SetNumberField(TEXT("approx_fps"), Rate.AsDecimal());
        }
        return Out;
    }

    /** Map EAdditiveAnimationType to a single-word token. */
    FString AdditiveAnimTypeToString(EAdditiveAnimationType Type)
    {
        switch (Type)
        {
        case AAT_None:                     return TEXT("none");
        case AAT_LocalSpaceBase:           return TEXT("local_space");
        case AAT_RotationOffsetMeshSpace:  return TEXT("rotation_offset_mesh_space");
        default:                           return TEXT("unknown");
        }
    }

    /** Render one FAnimNotifyEvent. We expose name + time + duration +
     *  source class plus the linked notify / notify-state object class,
     *  which is what a designer reads at a glance. */
    TSharedPtr<FJsonObject> NotifyRecord(const FAnimNotifyEvent& Notify)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
        Out->SetNumberField(TEXT("time"), Notify.GetTime(EAnimLinkMethod::Absolute));
        Out->SetNumberField(TEXT("duration"), Notify.Duration);
        Out->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
        Out->SetNumberField(TEXT("trigger_chance"), Notify.NotifyTriggerChance);

        if (UAnimNotify* Inst = Notify.Notify)
        {
            Out->SetStringField(TEXT("notify_class"), Inst->GetClass()->GetName());
            Out->SetStringField(TEXT("notify_class_path"), Inst->GetClass()->GetPathName());
            Out->SetStringField(TEXT("notify_kind"), TEXT("instant"));
        }
        else if (UAnimNotifyState* StateInst = Notify.NotifyStateClass)
        {
            Out->SetStringField(TEXT("notify_class"), StateInst->GetClass()->GetName());
            Out->SetStringField(TEXT("notify_class_path"), StateInst->GetClass()->GetPathName());
            Out->SetStringField(TEXT("notify_kind"), TEXT("state"));
        }
        else
        {
            // Custom event notify: NotifyName is the event name, no
            // backing UObject. The runtime fires the event by name.
            Out->SetStringField(TEXT("notify_kind"), TEXT("event"));
        }
        return Out;
    }

    /** Walk a USkeletalMesh and emit the kind-specific block. */
    TSharedPtr<FJsonObject> InspectSkeletalMesh(USkeletalMesh* Mesh,
                                                bool bIncludeBones,
                                                bool bIncludeSockets,
                                                int32 MaxBones)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("kind"), TEXT("skeletal_mesh"));

        if (USkeleton* Skel = Mesh->GetSkeleton())
        {
            Out->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
        }

        Out->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());

        // Bone list. Skeleton-side reference skeleton is canonical;
        // every animation system addresses bones by name through the
        // skeleton, not the mesh. The mesh has its own ref skeleton
        // that may differ when bones are stripped from a reduced LOD,
        // so we expose both counts.
        if (USkeleton* Skel = Mesh->GetSkeleton())
        {
            const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
            const TArray<FMeshBoneInfo>& BoneInfo = RefSkel.GetRefBoneInfo();
            Out->SetNumberField(TEXT("bone_count"), BoneInfo.Num());

            if (bIncludeBones)
            {
                const int32 EmitCount = MaxBones > 0 ? FMath::Min(BoneInfo.Num(), MaxBones) : BoneInfo.Num();
                TArray<TSharedPtr<FJsonValue>> BoneArr;
                for (int32 i = 0; i < EmitCount; ++i)
                {
                    const FMeshBoneInfo& Info = BoneInfo[i];
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("name"), Info.Name.ToString());
                    Entry->SetNumberField(TEXT("parent_index"), Info.ParentIndex);
                    if (BoneInfo.IsValidIndex(Info.ParentIndex))
                    {
                        Entry->SetStringField(TEXT("parent_name"), BoneInfo[Info.ParentIndex].Name.ToString());
                    }
                    BoneArr.Add(MakeShared<FJsonValueObject>(Entry));
                }
                Out->SetArrayField(TEXT("bones"), BoneArr);
                if (BoneInfo.Num() > EmitCount)
                {
                    Out->SetBoolField(TEXT("bones_truncated"), true);
                }
            }
        }

        if (bIncludeSockets)
        {
            TArray<TSharedPtr<FJsonValue>> SocketArr;

            // Mesh-level sockets first; the mesh wins when both define
            // the same socket name, which mirrors the editor's
            // resolution order.
            for (USkeletalMeshSocket* Socket : Mesh->GetActiveSocketList())
            {
                if (!Socket)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Socket->SocketName.ToString());
                Entry->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
                Entry->SetArrayField(TEXT("relative_location"), AnimationInspect_Vec3ToJson(Socket->RelativeLocation));
                Entry->SetArrayField(TEXT("relative_rotation"), RotatorToJson(Socket->RelativeRotation));
                Entry->SetArrayField(TEXT("relative_scale"),    AnimationInspect_Vec3ToJson(Socket->RelativeScale));
                Entry->SetStringField(TEXT("source"), TEXT("skeletal_mesh"));
                SocketArr.Add(MakeShared<FJsonValueObject>(Entry));
            }

            // Skeleton-level sockets the mesh did not override.
            if (USkeleton* Skel = Mesh->GetSkeleton())
            {
                TSet<FName> MeshSocketNames;
                for (USkeletalMeshSocket* Socket : Mesh->GetActiveSocketList())
                {
                    if (Socket)
                    {
                        MeshSocketNames.Add(Socket->SocketName);
                    }
                }
                for (USkeletalMeshSocket* Socket : Skel->Sockets)
                {
                    if (!Socket || MeshSocketNames.Contains(Socket->SocketName))
                    {
                        continue;
                    }
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("name"), Socket->SocketName.ToString());
                    Entry->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
                    Entry->SetArrayField(TEXT("relative_location"), AnimationInspect_Vec3ToJson(Socket->RelativeLocation));
                    Entry->SetArrayField(TEXT("relative_rotation"), RotatorToJson(Socket->RelativeRotation));
                    Entry->SetArrayField(TEXT("relative_scale"),    AnimationInspect_Vec3ToJson(Socket->RelativeScale));
                    Entry->SetStringField(TEXT("source"), TEXT("skeleton"));
                    SocketArr.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }
            Out->SetArrayField(TEXT("sockets"), SocketArr);
        }

        return Out;
    }

    /** Walk a UAnimSequence and emit the kind-specific block. */
    TSharedPtr<FJsonObject> InspectAnimSequence(UAnimSequence* Seq,
                                                bool bIncludeNotifies,
                                                int32 MaxNotifies)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("kind"), TEXT("anim_sequence"));

        Out->SetNumberField(TEXT("play_length"), Seq->GetPlayLength());
        Out->SetNumberField(TEXT("rate_scale"),  Seq->RateScale);
        Out->SetObjectField(TEXT("sampling_frame_rate"), AnimationInspect_FrameRateRecord(Seq->GetSamplingFrameRate()));
        Out->SetNumberField(TEXT("sampled_key_count"), Seq->GetNumberOfSampledKeys());
        Out->SetStringField(TEXT("additive_anim_type"), AdditiveAnimTypeToString(Seq->GetAdditiveAnimType()));

        if (USkeleton* Skel = Seq->GetSkeleton())
        {
            Out->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
        }

        Out->SetNumberField(TEXT("notify_count"), Seq->Notifies.Num());

        if (bIncludeNotifies)
        {
            const int32 EmitCount = MaxNotifies > 0 ? FMath::Min(Seq->Notifies.Num(), MaxNotifies) : Seq->Notifies.Num();
            TArray<TSharedPtr<FJsonValue>> NotifyArr;
            for (int32 i = 0; i < EmitCount; ++i)
            {
                NotifyArr.Add(MakeShared<FJsonValueObject>(NotifyRecord(Seq->Notifies[i])));
            }
            Out->SetArrayField(TEXT("notifies"), NotifyArr);
            if (Seq->Notifies.Num() > EmitCount)
            {
                Out->SetBoolField(TEXT("notifies_truncated"), true);
            }
        }
        return Out;
    }

    /** Walk a UAnimMontage and emit the kind-specific block. */
    TSharedPtr<FJsonObject> InspectAnimMontage(UAnimMontage* Montage,
                                               bool bIncludeNotifies,
                                               bool bIncludeSections,
                                               bool bIncludeSlotTracks,
                                               int32 MaxNotifies)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("kind"), TEXT("anim_montage"));

        Out->SetNumberField(TEXT("play_length"), Montage->GetPlayLength());
        Out->SetNumberField(TEXT("rate_scale"),  Montage->RateScale);

        if (USkeleton* Skel = Montage->GetSkeleton())
        {
            Out->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
        }

        Out->SetNumberField(TEXT("composite_section_count"), Montage->CompositeSections.Num());
        Out->SetNumberField(TEXT("slot_track_count"),        Montage->SlotAnimTracks.Num());
        Out->SetNumberField(TEXT("notify_count"),            Montage->Notifies.Num());

        if (bIncludeSections)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FCompositeSection& Section : Montage->CompositeSections)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Section.SectionName.ToString());
                Entry->SetNumberField(TEXT("time"), Section.GetTime(EAnimLinkMethod::Absolute));
                if (!Section.NextSectionName.IsNone())
                {
                    Entry->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
                }
                Arr.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Out->SetArrayField(TEXT("composite_sections"), Arr);
        }

        if (bIncludeSlotTracks)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("slot_name"), Slot.SlotName.ToString());
                Entry->SetNumberField(TEXT("animation_count"), Slot.AnimTrack.AnimSegments.Num());
                Arr.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Out->SetArrayField(TEXT("slot_tracks"), Arr);
        }

        if (bIncludeNotifies)
        {
            const int32 EmitCount = MaxNotifies > 0 ? FMath::Min(Montage->Notifies.Num(), MaxNotifies) : Montage->Notifies.Num();
            TArray<TSharedPtr<FJsonValue>> NotifyArr;
            for (int32 i = 0; i < EmitCount; ++i)
            {
                NotifyArr.Add(MakeShared<FJsonValueObject>(NotifyRecord(Montage->Notifies[i])));
            }
            Out->SetArrayField(TEXT("notifies"), NotifyArr);
            if (Montage->Notifies.Num() > EmitCount)
            {
                Out->SetBoolField(TEXT("notifies_truncated"), true);
            }
        }
        return Out;
    }

    /** Render one FBlendParameter axis. */
    TSharedPtr<FJsonObject> AxisRecord(const FBlendParameter& Axis)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("display_name"), Axis.DisplayName);
        Out->SetNumberField(TEXT("min"), Axis.Min);
        Out->SetNumberField(TEXT("max"), Axis.Max);
        Out->SetNumberField(TEXT("grid_num"), Axis.GridNum);
        Out->SetBoolField(TEXT("snap_to_grid"), Axis.bSnapToGrid);
        Out->SetBoolField(TEXT("wrap_input"), Axis.bWrapInput);
        return Out;
    }

    /** Walk a UBlendSpace (or 1D) and emit the kind-specific block.
     *  We probe for UBlendSpace1D so we can report axis_count = 1; the
     *  underlying FBlendParameter array always has 3 entries even on
     *  the 1D variant, and only the first is used in 1D space. */
    TSharedPtr<FJsonObject> InspectBlendSpace(UBlendSpace* BlendSpace)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("kind"), TEXT("blend_space"));

        const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
        const int32 AxisCount = bIs1D ? 1 : 2;
        Out->SetNumberField(TEXT("axis_count"), AxisCount);
        Out->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());

        if (USkeleton* Skel = BlendSpace->GetSkeleton())
        {
            Out->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
        }

        // Read each axis through the public UBlendSpace::GetBlendParameter
        // getter; the underlying storage is a fixed-size FBlendParameter[3]
        // and the getter clamps the index for us.
        const TCHAR* AxisFieldNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
        TArray<TSharedPtr<FJsonValue>> Axes;
        for (int32 i = 0; i < AxisCount; ++i)
        {
            const FBlendParameter& Axis = BlendSpace->GetBlendParameter(i);
            TSharedPtr<FJsonObject> AxisJson = AxisRecord(Axis);
            AxisJson->SetStringField(TEXT("axis"), AxisFieldNames[i]);
            Axes.Add(MakeShared<FJsonValueObject>(AxisJson));
        }
        Out->SetArrayField(TEXT("axes"), Axes);
        return Out;
    }

    /** Walk a UAnimBlueprint and emit the kind-specific block. */
    TSharedPtr<FJsonObject> InspectAnimBlueprint(UAnimBlueprint* AnimBP,
                                                 bool bIncludeStateMachines)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("kind"), TEXT("anim_blueprint"));

        if (UClass* ParentClass = AnimBP->ParentClass)
        {
            Out->SetStringField(TEXT("parent_class"), ParentClass->GetName());
            Out->SetStringField(TEXT("parent_class_path"), ParentClass->GetPathName());
        }
        if (USkeleton* Skel = AnimBP->TargetSkeleton)
        {
            Out->SetStringField(TEXT("target_skeleton_path"), Skel->GetPathName());
        }
        Out->SetBoolField(TEXT("is_template"), AnimBP->bIsTemplate);

        // Variable count off the Blueprint's NewVariables array. This is
        // safe in non-editor builds too.
        Out->SetNumberField(TEXT("variable_count"), AnimBP->NewVariables.Num());

        if (bIncludeStateMachines)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            // Read state machines off the cached generated class so we
            // do not need the editor-only AnimGraph module. The class
            // is null until the BP has compiled at least once; we
            // surface the empty array in that case.
            if (UAnimBlueprintGeneratedClass* GenClass = AnimBP->GetAnimBlueprintGeneratedClass())
            {
                for (const FBakedAnimationStateMachine& SM : GenClass->BakedStateMachines)
                {
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("name"), SM.MachineName.ToString());
                    Entry->SetNumberField(TEXT("state_count"), SM.States.Num());
                    Entry->SetNumberField(TEXT("transition_count"), SM.Transitions.Num());
                    Entry->SetNumberField(TEXT("initial_state"), SM.InitialState);
                    if (SM.States.IsValidIndex(SM.InitialState))
                    {
                        Entry->SetStringField(TEXT("initial_state_name"),
                            SM.States[SM.InitialState].StateName.ToString());
                    }
                    Arr.Add(MakeShared<FJsonValueObject>(Entry));
                }
            }
            Out->SetArrayField(TEXT("state_machines"), Arr);
        }

        return Out;
    }
}

FSproftAnimationInspectCommands::FSproftAnimationInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftAnimationInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("animation_inspect"))
    {
        return HandleAnimationInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown animation_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftAnimationInspectCommands::HandleAnimationInspect(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'asset' parameter is required"));
    }

    bool bIncludeBones         = true;
    bool bIncludeSockets       = true;
    bool bIncludeNotifies      = true;
    bool bIncludeSections      = true;
    bool bIncludeSlotTracks    = true;
    bool bIncludeStateMachines = true;
    int32 MaxBones    = 4096;
    int32 MaxNotifies = 1024;

    bool TempBool = false;
    if (Params->TryGetBoolField(TEXT("include_bones"), TempBool))           bIncludeBones = TempBool;
    if (Params->TryGetBoolField(TEXT("include_sockets"), TempBool))         bIncludeSockets = TempBool;
    if (Params->TryGetBoolField(TEXT("include_notifies"), TempBool))        bIncludeNotifies = TempBool;
    if (Params->TryGetBoolField(TEXT("include_sections"), TempBool))        bIncludeSections = TempBool;
    if (Params->TryGetBoolField(TEXT("include_slot_tracks"), TempBool))     bIncludeSlotTracks = TempBool;
    if (Params->TryGetBoolField(TEXT("include_state_machines"), TempBool))  bIncludeStateMachines = TempBool;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_bones"), TempNum))    MaxBones = FMath::Max(0, static_cast<int32>(TempNum));
    if (Params->TryGetNumberField(TEXT("max_notifies"), TempNum)) MaxNotifies = FMath::Max(0, static_cast<int32>(TempNum));

    // Resolve. UEditorAssetLibrary::LoadAsset works for short names if
    // the asset already lives somewhere in /Game/. For full paths we
    // pass it straight through.
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load asset: %s"), *AssetParam));
    }

    TSharedPtr<FJsonObject> Result;
    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
    {
        Result = InspectSkeletalMesh(Mesh, bIncludeBones, bIncludeSockets, MaxBones);
    }
    else if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
    {
        // Order matters: UAnimMontage extends UAnimSequenceBase, which
        // extends UAnimSequence's parent; cast for the most-specific
        // shape first.
        Result = InspectAnimMontage(Montage, bIncludeNotifies, bIncludeSections,
                                    bIncludeSlotTracks, MaxNotifies);
    }
    else if (UAnimSequence* Seq = Cast<UAnimSequence>(Asset))
    {
        Result = InspectAnimSequence(Seq, bIncludeNotifies, MaxNotifies);
    }
    else if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset))
    {
        Result = InspectBlendSpace(BlendSpace);
    }
    else if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Asset))
    {
        Result = InspectAnimBlueprint(AnimBP, bIncludeStateMachines);
    }
    else
    {
        Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("kind"), TEXT("unknown"));
    }

    // Common identity fields go on every response.
    Result->SetStringField(TEXT("name"),       Asset->GetName());
    Result->SetStringField(TEXT("path"),       Asset->GetPathName());
    Result->SetStringField(TEXT("class"),      Asset->GetClass()->GetName());
    Result->SetStringField(TEXT("class_path"), Asset->GetClass()->GetPathName());
    return Result;
}
