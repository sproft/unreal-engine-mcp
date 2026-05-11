#include "Commands/SproftAnimationEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Animation/AnimBoneCompressionCodec.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCompressionTypes.h"
#include "Animation/AnimCurveCompressionCodec.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationSettings.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AnimationBlueprintLibrary.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Math/Transform.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /** Resolve an `/Game/...` Blueprint class path or `/Script/Module.Class`
     *  path or short name to a UClass derived from a chosen base. We try:
     *
     *    1. The literal path through FindObject / LoadClass.
     *    2. A `/Game/...` Blueprint path with `_C` suffix when missing.
     *    3. A short-name probe over the loaded class set.
     *    4. Common engine namespaces (`/Script/Engine.<Name>`).
     *
     *  Used here for the notify_class resolver. The `BaseClass` filter
     *  lets the caller restrict to UAnimNotify or UAnimNotifyState; we
     *  return null when the resolved class fails the IsChildOf check. */
    UClass* ResolveNotifyClass(const FString& InPath, UClass* BaseClass)
    {
        if (InPath.IsEmpty())
        {
            return nullptr;
        }

        auto AcceptClass = [BaseClass](UClass* Candidate) -> UClass*
        {
            if (Candidate && (BaseClass == nullptr || Candidate->IsChildOf(BaseClass)))
            {
                return Candidate;
            }
            return nullptr;
        };

        // 1. Literal path.
        if (UClass* Found = FindObject<UClass>(nullptr, *InPath))
        {
            if (UClass* OK = AcceptClass(Found)) return OK;
        }
        if (UClass* Loaded = LoadClass<UObject>(nullptr, *InPath))
        {
            if (UClass* OK = AcceptClass(Loaded)) return OK;
        }

        // 2. /Game/... Blueprint class path with _C suffix.
        if (InPath.StartsWith(TEXT("/Game/")) && !InPath.EndsWith(TEXT("_C")))
        {
            const FString Suffixed = InPath + TEXT("_C");
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Suffixed))
            {
                if (UClass* OK = AcceptClass(Loaded)) return OK;
            }
        }

        // 3. Short-name probe with U / A prefix variants.
        TArray<FString> Variants;
        Variants.Add(InPath);
        if (!InPath.StartsWith(TEXT("U")))
        {
            Variants.Add(TEXT("U") + InPath);
        }
        for (const FString& Variant : Variants)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Variant))
            {
                if (UClass* OK = AcceptClass(Found)) return OK;
            }
        }

        // 4. Engine namespace fallback. UE 5.7 tightened FString::Printf
        // to reject runtime format strings; concatenate the prefix + name
        // manually so the lookup avoids the format-string check.
        static const TCHAR* const Namespaces[] = {
            TEXT("/Script/Engine."),
            TEXT("/Script/AnimGraph."),
            TEXT("/Script/AnimGraphRuntime."),
        };
        for (const FString& Variant : Variants)
        {
            for (const TCHAR* Namespace : Namespaces)
            {
                const FString Path = FString(Namespace) + Variant;
                if (UClass* Loaded = LoadClass<UObject>(nullptr, *Path))
                {
                    if (UClass* OK = AcceptClass(Loaded)) return OK;
                }
            }
        }
        return nullptr;
    }

    /** Map an `additive_type` token to EAdditiveAnimationType. */
    bool ParseAdditiveAnimType(const FString& Token, EAdditiveAnimationType& Out)
    {
        const FString Norm = Token.ToLower();
        if (Norm == TEXT("none"))                            { Out = AAT_None;                     return true; }
        if (Norm == TEXT("local_space")
            || Norm == TEXT("localspace"))                   { Out = AAT_LocalSpaceBase;           return true; }
        if (Norm == TEXT("rotation_offset_mesh_space")
            || Norm == TEXT("rotationoffsetmeshspace"))      { Out = AAT_RotationOffsetMeshSpace;  return true; }
        return false;
    }

    /** Render an EAdditiveAnimationType back to a single-word token. */
    FString AdditiveAnimTypeToken(EAdditiveAnimationType Type)
    {
        switch (Type)
        {
            case AAT_None:                    return TEXT("none");
            case AAT_LocalSpaceBase:          return TEXT("local_space");
            case AAT_RotationOffsetMeshSpace: return TEXT("rotation_offset_mesh_space");
            default:                          return TEXT("unknown");
        }
    }

    /** Map a `ref_pose_type` token to EAdditiveBasePoseType. */
    bool ParseRefPoseType(const FString& Token, EAdditiveBasePoseType& Out)
    {
        const FString Norm = Token.ToLower();
        if (Norm == TEXT("none"))                       { Out = ABPT_None;       return true; }
        if (Norm == TEXT("ref_pose")
            || Norm == TEXT("refpose"))                 { Out = ABPT_RefPose;    return true; }
        if (Norm == TEXT("anim_scaled")
            || Norm == TEXT("animscaled"))              { Out = ABPT_AnimScaled; return true; }
        if (Norm == TEXT("anim_frame")
            || Norm == TEXT("animframe"))               { Out = ABPT_AnimFrame;  return true; }
        return false;
    }

    /** Render an EAdditiveBasePoseType back to a single-word token. */
    FString RefPoseTypeToken(EAdditiveBasePoseType Type)
    {
        switch (Type)
        {
            case ABPT_None:       return TEXT("none");
            case ABPT_RefPose:    return TEXT("ref_pose");
            case ABPT_AnimScaled: return TEXT("anim_scaled");
            case ABPT_AnimFrame:  return TEXT("anim_frame");
            default:              return TEXT("unknown");
        }
    }

    /** Parse a `curve_type` token to ERawCurveTrackTypes. */
    bool ParseRawCurveTrackType(const FString& Token, ERawCurveTrackTypes& Out)
    {
        const FString Norm = Token.ToLower().Replace(TEXT("_"), TEXT(""));
        if (Norm == TEXT("float") || Norm == TEXT("scalar"))
        {
            Out = ERawCurveTrackTypes::RCT_Float;
            return true;
        }
        if (Norm == TEXT("vector") || Norm == TEXT("vec3"))
        {
            Out = ERawCurveTrackTypes::RCT_Vector;
            return true;
        }
        if (Norm == TEXT("transform") || Norm == TEXT("transformation"))
        {
            Out = ERawCurveTrackTypes::RCT_Transform;
            return true;
        }
        return false;
    }

    /** Render ERawCurveTrackTypes back to its short token. */
    FString RawCurveTrackTypeToken(ERawCurveTrackTypes Type)
    {
        switch (Type)
        {
            case ERawCurveTrackTypes::RCT_Float:     return TEXT("float");
            case ERawCurveTrackTypes::RCT_Vector:    return TEXT("vector");
            case ERawCurveTrackTypes::RCT_Transform: return TEXT("transform");
            default:                                 return TEXT("unknown");
        }
    }

    /** Pull an FVector out of a JSON array of three numbers. */
    bool TryParseVector3(const TSharedPtr<FJsonValue>& Value, FVector& Out)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
        if (Arr.Num() < 3)
        {
            return false;
        }
        Out = FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
        return true;
    }
}

FSproftAnimationEditCommands::FSproftAnimationEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("animation_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown animation_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    Op = Op.ToLower();

    if (Op == TEXT("set_rate_scale"))
    {
        return HandleSetRateScale(Params);
    }
    if (Op == TEXT("set_additive"))
    {
        return HandleSetAdditive(Params);
    }
    if (Op == TEXT("add_notify"))
    {
        return HandleAddNotify(Params);
    }
    if (Op == TEXT("add_curve"))
    {
        return HandleAddCurve(Params);
    }
    if (Op == TEXT("add_sync_marker") || Op == TEXT("add_marker"))
    {
        return HandleAddSyncMarker(Params);
    }
    if (Op == TEXT("add_blendspace_sample")
        || Op == TEXT("add_blend_space_sample")
        || Op == TEXT("add_sample"))
    {
        return HandleAddBlendSpaceSample(Params);
    }
    if (Op == TEXT("replace_blendspace_sample")
        || Op == TEXT("replace_blend_space_sample")
        || Op == TEXT("replace_sample"))
    {
        return HandleReplaceBlendSpaceSample(Params);
    }
    if (Op == TEXT("add_metadata_curve") || Op == TEXT("add_meta_curve")
        || Op == TEXT("add_typed_metadata_curve"))
    {
        return HandleAddMetadataCurve(Params);
    }
    if (Op == TEXT("delete_blendspace_sample")
        || Op == TEXT("delete_blend_space_sample")
        || Op == TEXT("delete_sample")
        || Op == TEXT("remove_blendspace_sample")
        || Op == TEXT("remove_sample"))
    {
        return HandleDeleteBlendSpaceSample(Params);
    }
    if (Op == TEXT("set_root_motion") || Op == TEXT("set_rootmotion")
        || Op == TEXT("root_motion") || Op == TEXT("enable_root_motion"))
    {
        return HandleSetRootMotion(Params);
    }
    if (Op == TEXT("add_notify_state") || Op == TEXT("add_state_notify")
        || Op == TEXT("add_notifystate"))
    {
        return HandleAddNotifyState(Params);
    }
    if (Op == TEXT("set_compression_scheme") || Op == TEXT("set_compression")
        || Op == TEXT("set_compression_codec") || Op == TEXT("set_compression_settings"))
    {
        return HandleSetCompressionScheme(Params);
    }
    if (Op == TEXT("set_curve_compression") || Op == TEXT("set_curve_compression_scheme")
        || Op == TEXT("set_curve_compression_codec") || Op == TEXT("set_curve_compression_settings"))
    {
        return HandleSetCurveCompression(Params);
    }
    if (Op == TEXT("set_loop_flags") || Op == TEXT("set_loop") || Op == TEXT("set_looping")
        || Op == TEXT("set_loop_settings"))
    {
        return HandleSetLoopFlags(Params);
    }
    if (Op == TEXT("set_blend_times") || Op == TEXT("set_blendtimes")
        || Op == TEXT("set_blend") || Op == TEXT("blend_times"))
    {
        return HandleSetBlendTimes(Params);
    }
    if (Op == TEXT("add_montage_section") || Op == TEXT("add_section")
        || Op == TEXT("add_composite_section") || Op == TEXT("montage_add_section"))
    {
        return HandleAddMontageSection(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported animation_edit op '%s'; expected one of 'set_rate_scale', 'set_additive', 'add_notify', 'add_notify_state', 'add_curve', 'add_metadata_curve', 'add_sync_marker', 'add_blendspace_sample', 'replace_blendspace_sample', 'delete_blendspace_sample', 'set_root_motion', 'set_compression_scheme', 'set_curve_compression', 'set_loop_flags', 'set_blend_times', 'add_montage_section'"), *Op));
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetRateScale(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_rate_scale: missing 'asset'"));
    }
    double NewRate = 1.0;
    if (!Params->TryGetNumberField(TEXT("rate_scale"), NewRate))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_rate_scale: missing 'rate_scale' float"));
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(Asset);
    if (!SeqBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_rate_scale: '%s' is not a UAnimSequenceBase"), *AssetParam));
    }

    const float PreviousRate = SeqBase->RateScale;
    SeqBase->RateScale = static_cast<float>(NewRate);
    SeqBase->MarkPackageDirty();

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(SeqBase->GetPathName(), /*bOnlyIfIsDirty*/ false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_rate_scale"));
    Result->SetStringField(TEXT("asset"), SeqBase->GetName());
    Result->SetStringField(TEXT("path"), SeqBase->GetPathName());
    Result->SetStringField(TEXT("class"), SeqBase->GetClass()->GetName());
    Result->SetNumberField(TEXT("previous_rate_scale"), PreviousRate);
    Result->SetNumberField(TEXT("rate_scale"), SeqBase->RateScale);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetAdditive(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_additive: missing 'asset'"));
    }
    FString AdditiveToken;
    if (!Params->TryGetStringField(TEXT("additive_type"), AdditiveToken) || AdditiveToken.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_additive: missing 'additive_type'"));
    }
    EAdditiveAnimationType Additive;
    if (!ParseAdditiveAnimType(AdditiveToken, Additive))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_additive: unknown additive_type '%s'; expected none / local_space / rotation_offset_mesh_space"), *AdditiveToken));
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Seq)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_additive: '%s' is not a UAnimSequence (the additive shape lives on UAnimSequence)"), *AssetParam));
    }

    const EAdditiveAnimationType PrevAdditive = Seq->AdditiveAnimType;
    Seq->AdditiveAnimType = Additive;

    // Optional ref_pose_type / ref_pose_seq / ref_frame_index.
    bool bRefPoseTypeChanged = false;
    EAdditiveBasePoseType PrevRefPoseType = Seq->RefPoseType;
    EAdditiveBasePoseType NewRefPoseType = PrevRefPoseType;
    FString RefPoseTypeInput;
    if (Params->TryGetStringField(TEXT("ref_pose_type"), RefPoseTypeInput) && !RefPoseTypeInput.IsEmpty())
    {
        if (!ParseRefPoseType(RefPoseTypeInput, NewRefPoseType))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_additive: unknown ref_pose_type '%s'; expected none / ref_pose / anim_scaled / anim_frame"), *RefPoseTypeInput));
        }
        Seq->RefPoseType = NewRefPoseType;
        bRefPoseTypeChanged = true;
    }

    bool bRefPoseSeqChanged = false;
    UAnimSequence* PrevRefSeq = Seq->RefPoseSeq;
    FString RefPoseSeqPath;
    if (Params->TryGetStringField(TEXT("ref_pose_seq"), RefPoseSeqPath) && !RefPoseSeqPath.IsEmpty())
    {
        UObject* RefAsset = UEditorAssetLibrary::LoadAsset(RefPoseSeqPath);
        UAnimSequence* RefSeq = Cast<UAnimSequence>(RefAsset);
        if (!RefSeq)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_additive: ref_pose_seq '%s' is not a UAnimSequence"), *RefPoseSeqPath));
        }
        Seq->RefPoseSeq = RefSeq;
        bRefPoseSeqChanged = true;
    }

    bool bRefFrameChanged = false;
    int32 PrevRefFrame = Seq->RefFrameIndex;
    double RefFrameValue = 0.0;
    if (Params->TryGetNumberField(TEXT("ref_frame_index"), RefFrameValue))
    {
        Seq->RefFrameIndex = FMath::Max(0, static_cast<int32>(RefFrameValue));
        bRefFrameChanged = true;
    }

    Seq->MarkPackageDirty();

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(Seq, /*bOnlyIfIsDirty*/ false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_additive"));
    Result->SetStringField(TEXT("asset"), Seq->GetName());
    Result->SetStringField(TEXT("path"), Seq->GetPathName());
    Result->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    Result->SetStringField(TEXT("previous_additive_type"), AdditiveAnimTypeToken(PrevAdditive));
    Result->SetStringField(TEXT("additive_type"), AdditiveAnimTypeToken(Additive));
    if (bRefPoseTypeChanged)
    {
        Result->SetStringField(TEXT("previous_ref_pose_type"), RefPoseTypeToken(PrevRefPoseType));
        Result->SetStringField(TEXT("ref_pose_type"), RefPoseTypeToken(NewRefPoseType));
    }
    else
    {
        Result->SetStringField(TEXT("ref_pose_type"), RefPoseTypeToken(NewRefPoseType));
    }
    if (bRefPoseSeqChanged)
    {
        Result->SetStringField(TEXT("ref_pose_seq"), Seq->RefPoseSeq ? Seq->RefPoseSeq->GetPathName() : FString());
        Result->SetStringField(TEXT("previous_ref_pose_seq"), PrevRefSeq ? PrevRefSeq->GetPathName() : FString());
    }
    if (bRefFrameChanged)
    {
        Result->SetNumberField(TEXT("ref_frame_index"), Seq->RefFrameIndex);
        Result->SetNumberField(TEXT("previous_ref_frame_index"), PrevRefFrame);
    }
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddNotify(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_notify: missing 'asset'"));
    }
    FString TrackParam;
    if (!Params->TryGetStringField(TEXT("track"), TrackParam) || TrackParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_notify: missing 'track' name"));
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(Asset);
    if (!SeqBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_notify: '%s' is not a UAnimSequenceBase"), *AssetParam));
    }

    // Resolve the time. `frame` (integer) wins if present; otherwise we
    // use `time` (float seconds).
    double FrameValue = 0.0;
    bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameValue);
    double TimeValue = 0.0;
    bool bHasTime = Params->TryGetNumberField(TEXT("time"), TimeValue);
    if (!bHasFrame && !bHasTime)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_notify: one of 'frame' (int) or 'time' (float seconds) is required"));
    }

    float StartTime = 0.0f;
    if (bHasFrame)
    {
        // Convert frames to seconds through the asset's sampling frame
        // rate. UAnimSequence exposes GetSamplingFrameRate(); the
        // UAnimMontage / UAnimComposite branches do not, so we fall
        // back to the play length / num frames ratio for those.
        if (UAnimSequence* Seq = Cast<UAnimSequence>(SeqBase))
        {
            const FFrameRate Rate = Seq->GetSamplingFrameRate();
            if (Rate.Numerator > 0)
            {
                const FFrameTime FrameTime(FFrameNumber(static_cast<int32>(FrameValue)));
                StartTime = static_cast<float>(Rate.AsSeconds(FrameTime));
            }
            else
            {
                StartTime = static_cast<float>(FrameValue / 30.0); // sensible default
            }
        }
        else
        {
            StartTime = static_cast<float>(FrameValue / 30.0);
        }
    }
    else
    {
        StartTime = static_cast<float>(TimeValue);
    }

    // Resolve the notify class. Optional; when omitted we treat the entry
    // as a custom-event notify (no UObject backing, just NotifyName).
    FString NotifyClassParam;
    Params->TryGetStringField(TEXT("notify_class"), NotifyClassParam);

    UClass* NotifyClass = nullptr;
    UClass* StateClass = nullptr;
    FString NotifyKind;
    if (!NotifyClassParam.IsEmpty())
    {
        NotifyClass = ResolveNotifyClass(NotifyClassParam, UAnimNotify::StaticClass());
        if (NotifyClass)
        {
            NotifyKind = TEXT("instant");
        }
        else
        {
            StateClass = ResolveNotifyClass(NotifyClassParam, UAnimNotifyState::StaticClass());
            if (StateClass)
            {
                NotifyKind = TEXT("state");
            }
        }
        if (!NotifyClass && !StateClass)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("add_notify: failed to resolve notify_class '%s' as either UAnimNotify or UAnimNotifyState"), *NotifyClassParam));
        }
    }
    else
    {
        NotifyKind = TEXT("event");
    }

    // Auto-create the notify track when it does not exist yet.
    const FName TrackFName(*TrackParam);
    if (!UAnimationBlueprintLibrary::IsValidAnimNotifyTrackName(SeqBase, TrackFName))
    {
        UAnimationBlueprintLibrary::AddAnimationNotifyTrack(SeqBase, TrackFName, FLinearColor::White);
    }

    FString EventName;
    Params->TryGetStringField(TEXT("event_name"), EventName);

    UObject* CreatedNotify = nullptr;
    if (NotifyClass)
    {
        CreatedNotify = UAnimationBlueprintLibrary::AddAnimationNotifyEvent(SeqBase, TrackFName, StartTime, NotifyClass);
    }
    else if (StateClass)
    {
        double DurationValue = 0.0;
        if (!Params->TryGetNumberField(TEXT("duration"), DurationValue))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("add_notify: 'duration' (float seconds) is required for UAnimNotifyState subclasses"));
        }
        CreatedNotify = UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent(SeqBase, TrackFName, StartTime, static_cast<float>(DurationValue), StateClass);
    }
    else
    {
        // Custom-event notify: write directly into the Notifies array
        // since the BP library has no helper for the event-only shape.
        FAnimNotifyEvent& NewEvent = SeqBase->Notifies.AddZeroed_GetRef();
        NewEvent.NotifyName = EventName.IsEmpty() ? FName(*TrackParam) : FName(*EventName);
        // FAnimLinkableElement::Link unifies the old LinkSequence /
        // LinkMontage path into a single call; the slot index defaults
        // to 0 which is what we want for sequences and the primary slot
        // on montages.
        NewEvent.Link(SeqBase, StartTime, 0);
        NewEvent.TriggerTimeOffset = 0.0f;
        NewEvent.TrackIndex = 0;
        // Find the new track index after AddAnimationNotifyTrack.
        for (int32 i = 0; i < SeqBase->AnimNotifyTracks.Num(); ++i)
        {
            if (SeqBase->AnimNotifyTracks[i].TrackName == TrackFName)
            {
                NewEvent.TrackIndex = i;
                break;
            }
        }
        SeqBase->SortNotifies();
    }

    SeqBase->MarkPackageDirty();
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(SeqBase->GetPathName(), /*bOnlyIfIsDirty*/ false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_notify"));
    Result->SetStringField(TEXT("asset"), SeqBase->GetName());
    Result->SetStringField(TEXT("path"), SeqBase->GetPathName());
    Result->SetStringField(TEXT("class"), SeqBase->GetClass()->GetName());
    Result->SetStringField(TEXT("track_name"), TrackFName.ToString());
    Result->SetNumberField(TEXT("time"), StartTime);
    Result->SetStringField(TEXT("notify_kind"), NotifyKind);
    if (NotifyClass)
    {
        Result->SetStringField(TEXT("notify_class"), NotifyClass->GetName());
        Result->SetStringField(TEXT("notify_class_path"), NotifyClass->GetPathName());
    }
    else if (StateClass)
    {
        Result->SetStringField(TEXT("notify_class"), StateClass->GetName());
        Result->SetStringField(TEXT("notify_class_path"), StateClass->GetPathName());
        double DurationValue = 0.0;
        Params->TryGetNumberField(TEXT("duration"), DurationValue);
        Result->SetNumberField(TEXT("duration"), DurationValue);
    }
    else
    {
        Result->SetStringField(TEXT("event_name"), EventName.IsEmpty() ? TrackParam : EventName);
    }
    if (CreatedNotify)
    {
        Result->SetStringField(TEXT("notify_object"), CreatedNotify->GetName());
    }
    Result->SetNumberField(TEXT("notify_count"), SeqBase->Notifies.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_curve: missing 'asset'"));
    }
    FString CurveNameParam;
    if (!Params->TryGetStringField(TEXT("curve_name"), CurveNameParam)
        && !Params->TryGetStringField(TEXT("name"), CurveNameParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_curve: missing 'curve_name'"));
    }
    if (CurveNameParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_curve: 'curve_name' must be non-empty"));
    }

    FString CurveTypeToken;
    if (!Params->TryGetStringField(TEXT("curve_type"), CurveTypeToken)
        && !Params->TryGetStringField(TEXT("type"), CurveTypeToken))
    {
        // Default to float curves; the editor's "Add Curve" button does
        // the same thing.
        CurveTypeToken = TEXT("float");
    }
    ERawCurveTrackTypes CurveType;
    if (!ParseRawCurveTrackType(CurveTypeToken, CurveType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_curve: unknown curve_type '%s'; expected Float / Vector / Transform"), *CurveTypeToken));
    }

    bool bMetaDataCurve = false;
    Params->TryGetBoolField(TEXT("metadata"), bMetaDataCurve);
    Params->TryGetBoolField(TEXT("metadata_curve"), bMetaDataCurve);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(Asset);
    if (!SeqBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_curve: '%s' is not a UAnimSequenceBase"), *AssetParam));
    }

    const FName CurveFName(*CurveNameParam);

    // AddCurve registers the curve on the asset's USkeleton + the
    // sequence's per-curve table; the BP library handles the
    // FRawCurveTracks::AddCurveData path plus IAnimationDataController
    // when the asset uses the new model.
    UAnimationBlueprintLibrary::AddCurve(SeqBase, CurveFName, CurveType, bMetaDataCurve);

    // Optional initial keyframe list. Each row is `[time, value]`.
    // For Float curves `value` is a number; Vector curves take a
    // 3-vector; Transform curves take three 3-vectors (location,
    // rotation, scale). The BP library has a parallel-array
    // `AddFloatCurveKeys` / `AddVectorCurveKeys` /
    // `AddTransformationCurveKeys` overload, so we collect the per-row
    // values into parallel arrays before the bulk write.
    int32 KeyframeCount = 0;
    int32 KeyframeFailures = 0;
    TArray<TSharedPtr<FJsonValue>> KeyframeErrors;

    const TArray<TSharedPtr<FJsonValue>>* KeyframesArr = nullptr;
    if (Params->TryGetArrayField(TEXT("keyframes"), KeyframesArr) && KeyframesArr)
    {
        TArray<float> Times;
        TArray<float> FloatValues;
        TArray<FVector> VectorValues;
        TArray<FTransform> TransformValues;

        for (int32 RowIdx = 0; RowIdx < KeyframesArr->Num(); ++RowIdx)
        {
            const TSharedPtr<FJsonValue>& RowVal = (*KeyframesArr)[RowIdx];
            if (!RowVal.IsValid() || RowVal->Type != EJson::Array)
            {
                KeyframeErrors.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("row %d is not an array"), RowIdx)));
                ++KeyframeFailures;
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>& Row = RowVal->AsArray();
            if (Row.Num() < 2)
            {
                KeyframeErrors.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("row %d expected [time, value]"), RowIdx)));
                ++KeyframeFailures;
                continue;
            }
            const float Time = static_cast<float>(Row[0]->AsNumber());

            if (CurveType == ERawCurveTrackTypes::RCT_Float)
            {
                if (Row[1]->Type != EJson::Number)
                {
                    KeyframeErrors.Add(MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("row %d expected float value"), RowIdx)));
                    ++KeyframeFailures;
                    continue;
                }
                Times.Add(Time);
                FloatValues.Add(static_cast<float>(Row[1]->AsNumber()));
                ++KeyframeCount;
            }
            else if (CurveType == ERawCurveTrackTypes::RCT_Vector)
            {
                FVector Vec;
                if (!TryParseVector3(Row[1], Vec))
                {
                    KeyframeErrors.Add(MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("row %d expected [x, y, z] vector"), RowIdx)));
                    ++KeyframeFailures;
                    continue;
                }
                Times.Add(Time);
                VectorValues.Add(Vec);
                ++KeyframeCount;
            }
            else if (CurveType == ERawCurveTrackTypes::RCT_Transform)
            {
                // Transform rows expect [time, location, rotation, scale]
                // where each component is a 3-vector. The rotation
                // 3-vector is interpreted as Euler degrees, matching
                // the editor's "Curve" panel.
                if (Row.Num() < 4)
                {
                    KeyframeErrors.Add(MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("row %d expected [time, location, rotation, scale]"), RowIdx)));
                    ++KeyframeFailures;
                    continue;
                }
                FVector Location, Rotation, Scale;
                if (!TryParseVector3(Row[1], Location)
                    || !TryParseVector3(Row[2], Rotation)
                    || !TryParseVector3(Row[3], Scale))
                {
                    KeyframeErrors.Add(MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("row %d expected three [x, y, z] components"), RowIdx)));
                    ++KeyframeFailures;
                    continue;
                }
                Times.Add(Time);
                TransformValues.Add(FTransform(
                    FRotator::MakeFromEuler(Rotation),
                    Location,
                    Scale));
                ++KeyframeCount;
            }
        }

        if (Times.Num() > 0)
        {
            if (CurveType == ERawCurveTrackTypes::RCT_Float)
            {
                UAnimationBlueprintLibrary::AddFloatCurveKeys(SeqBase, CurveFName, Times, FloatValues);
            }
            else if (CurveType == ERawCurveTrackTypes::RCT_Vector)
            {
                UAnimationBlueprintLibrary::AddVectorCurveKeys(SeqBase, CurveFName, Times, VectorValues);
            }
            else if (CurveType == ERawCurveTrackTypes::RCT_Transform)
            {
                UAnimationBlueprintLibrary::AddTransformationCurveKeys(SeqBase, CurveFName, Times, TransformValues);
            }
        }
    }

    SeqBase->MarkPackageDirty();
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(SeqBase->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_curve"));
    Result->SetStringField(TEXT("asset"), SeqBase->GetName());
    Result->SetStringField(TEXT("path"), SeqBase->GetPathName());
    Result->SetStringField(TEXT("class"), SeqBase->GetClass()->GetName());
    Result->SetStringField(TEXT("curve_name"), CurveNameParam);
    Result->SetStringField(TEXT("curve_type"), RawCurveTrackTypeToken(CurveType));
    Result->SetBoolField(TEXT("metadata_curve"), bMetaDataCurve);
    Result->SetNumberField(TEXT("keyframes_added"), KeyframeCount);
    Result->SetNumberField(TEXT("keyframe_failures"), KeyframeFailures);
    if (KeyframeErrors.Num() > 0)
    {
        Result->SetArrayField(TEXT("keyframe_errors"), KeyframeErrors);
    }
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_sync_marker: missing 'asset'"));
    }

    FString TrackParam;
    if (!Params->TryGetStringField(TEXT("track"), TrackParam)
        && !Params->TryGetStringField(TEXT("track_name"), TrackParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_sync_marker: missing 'track' name"));
    }
    if (TrackParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_sync_marker: 'track' must be non-empty"));
    }

    FString MarkerParam;
    if (!Params->TryGetStringField(TEXT("marker_name"), MarkerParam)
        && !Params->TryGetStringField(TEXT("marker"), MarkerParam)
        && !Params->TryGetStringField(TEXT("name"), MarkerParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_sync_marker: missing 'marker_name'"));
    }
    if (MarkerParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_sync_marker: 'marker_name' must be non-empty"));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Seq)
    {
        // Sync markers live on UAnimSequence directly; UAnimMontage and
        // UAnimComposite do not carry an `AuthoredSyncMarkers` array.
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_sync_marker: '%s' is not a UAnimSequence (sync markers live on UAnimSequence)"), *AssetParam));
    }

    // Resolve the marker time. `frame` (integer) wins when both are
    // present, mirroring `add_notify`'s frame-vs-time precedence.
    double FrameValue = 0.0;
    bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameValue);
    double TimeValue = 0.0;
    bool bHasTime = Params->TryGetNumberField(TEXT("time"), TimeValue);
    if (!bHasFrame && !bHasTime)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_sync_marker: one of 'frame' (int) or 'time' (float seconds) is required"));
    }

    float StartTime = 0.0f;
    if (bHasFrame)
    {
        const FFrameRate Rate = Seq->GetSamplingFrameRate();
        if (Rate.Numerator > 0)
        {
            const FFrameTime FrameTime(FFrameNumber(static_cast<int32>(FrameValue)));
            StartTime = static_cast<float>(Rate.AsSeconds(FrameTime));
        }
        else
        {
            StartTime = static_cast<float>(FrameValue / 30.0);
        }
    }
    else
    {
        StartTime = static_cast<float>(TimeValue);
    }

    const FName TrackFName(*TrackParam);
    const FName MarkerFName(*MarkerParam);

    // Auto-create the notify track when missing. Sync markers anchor to
    // the same notify track surface as notifies (the editor's "Sync
    // Markers" panel and "Notifies" panel both write to AnimNotifyTracks).
    if (!UAnimationBlueprintLibrary::IsValidAnimNotifyTrackName(Seq, TrackFName))
    {
        UAnimationBlueprintLibrary::AddAnimationNotifyTrack(Seq, TrackFName, FLinearColor::White);
    }

    UAnimationBlueprintLibrary::AddAnimationSyncMarker(Seq, MarkerFName, StartTime, TrackFName);

    Seq->MarkPackageDirty();
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Seq->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    const int32 SyncMarkerCount = Seq->AuthoredSyncMarkers.Num();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_sync_marker"));
    Result->SetStringField(TEXT("asset"), Seq->GetName());
    Result->SetStringField(TEXT("path"), Seq->GetPathName());
    Result->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    Result->SetStringField(TEXT("track_name"), TrackParam);
    Result->SetStringField(TEXT("marker_name"), MarkerParam);
    Result->SetNumberField(TEXT("time"), StartTime);
    Result->SetNumberField(TEXT("sync_marker_count"), SyncMarkerCount);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
    // BlendSpace path resolves through either `blendspace` (preferred) or
    // `asset`; the latter keeps the surface consistent with the other
    // animation_edit ops that key off `asset`.
    FString BlendSpaceParam;
    if (!Params->TryGetStringField(TEXT("blendspace"), BlendSpaceParam)
        && !Params->TryGetStringField(TEXT("blend_space"), BlendSpaceParam)
        && !Params->TryGetStringField(TEXT("asset"), BlendSpaceParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: missing 'blendspace' (asset path)"));
    }
    if (BlendSpaceParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: 'blendspace' must be non-empty"));
    }

    FString AnimParam;
    if (!Params->TryGetStringField(TEXT("animation"), AnimParam)
        && !Params->TryGetStringField(TEXT("anim_sequence"), AnimParam)
        && !Params->TryGetStringField(TEXT("sequence"), AnimParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: missing 'animation' (UAnimSequence path)"));
    }
    if (AnimParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: 'animation' must be non-empty"));
    }

    // sample_value is `[x]` (1D) or `[x, y]` (2D). Z stays at zero; the
    // engine's FBlendParameter[3] storage carries an unused third slot
    // for both BlendSpace and BlendSpace1D.
    const TArray<TSharedPtr<FJsonValue>>* SampleArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("sample_value"), SampleArr)
        && !Params->TryGetArrayField(TEXT("value"), SampleArr)
        && !Params->TryGetArrayField(TEXT("position"), SampleArr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: missing 'sample_value' array ([x] or [x, y])"));
    }
    if (!SampleArr || SampleArr->Num() == 0 || SampleArr->Num() > 3)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: 'sample_value' must be an array of 1, 2, or 3 numbers"));
    }
    FVector SampleValue = FVector::ZeroVector;
    for (int32 i = 0; i < SampleArr->Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& Comp = (*SampleArr)[i];
        if (!Comp.IsValid() || Comp->Type != EJson::Number)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("add_blendspace_sample: 'sample_value[%d]' is not a number"), i));
        }
        SampleValue.Component(i) = Comp->AsNumber();
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Resolve the BlendSpace asset.
    UObject* BlendSpaceAsset = UEditorAssetLibrary::LoadAsset(BlendSpaceParam);
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(BlendSpaceAsset);
    if (!BlendSpace)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_blendspace_sample: '%s' is not a UBlendSpace / UBlendSpace1D"), *BlendSpaceParam));
    }

    // Resolve the sequence and refuse a sequence whose skeleton or
    // additive type does not match the blendspace's existing samples.
    UObject* AnimAsset = UEditorAssetLibrary::LoadAsset(AnimParam);
    UAnimSequence* AnimSequence = Cast<UAnimSequence>(AnimAsset);
    if (!AnimSequence)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_blendspace_sample: 'animation' '%s' is not a UAnimSequence"), *AnimParam));
    }

    // The engine's IsAnimationCompatibleWithSkeleton tests both the
    // skeleton compatibility (USkeleton::IsCompatible) and the additive
    // chain match. UBlendSpace::ValidateAnimationSequence wraps both
    // and the additive-only fork; we surface either failure as a clear
    // error so the caller does not silently end up with a sample the
    // engine then rejects on play.
    if (!BlendSpace->IsAnimationCompatibleWithSkeleton(AnimSequence))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_blendspace_sample: animation skeleton does not match blendspace target skeleton")));
    }
    if (!BlendSpace->IsAnimationCompatible(AnimSequence))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_blendspace_sample: animation additive type does not match existing samples")));
    }

    // Range check. Each FBlendParameter carries Min / Max for its axis.
    // ValidateSampleValue runs the same range check + close-to-existing
    // probe; we run it before AddSample so callers get a specific error
    // instead of a silent -1 return.
    if (!BlendSpace->ValidateSampleValue(SampleValue))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_blendspace_sample: sample value [%f, %f, %f] is out of range or too close to an existing sample"),
                SampleValue.X, SampleValue.Y, SampleValue.Z));
    }

    const int32 PreviousSampleCount = BlendSpace->GetBlendSamples().Num();
    const int32 NewSampleIndex = BlendSpace->AddSample(AnimSequence, SampleValue);
    if (NewSampleIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_blendspace_sample: UBlendSpace::AddSample refused the sample"));
    }
    BlendSpace->MarkPackageDirty();

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(BlendSpace->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
    const int32 AxisCount = bIs1D ? 1 : 2;
    const int32 NewSampleCount = BlendSpace->GetBlendSamples().Num();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_blendspace_sample"));
    Result->SetStringField(TEXT("asset"), BlendSpace->GetName());
    Result->SetStringField(TEXT("path"), BlendSpace->GetPathName());
    Result->SetStringField(TEXT("class"), BlendSpace->GetClass()->GetName());
    Result->SetStringField(TEXT("animation_path"), AnimSequence->GetPathName());
    Result->SetStringField(TEXT("animation_name"), AnimSequence->GetName());
    Result->SetNumberField(TEXT("axis_count"), AxisCount);

    TArray<TSharedPtr<FJsonValue>> SampleJson;
    SampleJson.Add(MakeShared<FJsonValueNumber>(SampleValue.X));
    if (AxisCount >= 2) SampleJson.Add(MakeShared<FJsonValueNumber>(SampleValue.Y));
    if (AxisCount >= 3) SampleJson.Add(MakeShared<FJsonValueNumber>(SampleValue.Z));
    Result->SetArrayField(TEXT("sample_value"), SampleJson);

    Result->SetNumberField(TEXT("sample_index"), NewSampleIndex);
    Result->SetNumberField(TEXT("previous_sample_count"), PreviousSampleCount);
    Result->SetNumberField(TEXT("sample_count"), NewSampleCount);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleReplaceBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
    // Same resolver shape as add_blendspace_sample for the asset side.
    FString BlendSpaceParam;
    if (!Params->TryGetStringField(TEXT("blendspace"), BlendSpaceParam)
        && !Params->TryGetStringField(TEXT("blend_space"), BlendSpaceParam)
        && !Params->TryGetStringField(TEXT("asset"), BlendSpaceParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("replace_blendspace_sample: missing 'blendspace' (asset path)"));
    }
    if (BlendSpaceParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("replace_blendspace_sample: 'blendspace' must be non-empty"));
    }

    // sample_index is required; the API takes an int32 BlendSampleIndex.
    int32 SampleIndex = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("sample_index"), SampleIndex)
        && !Params->TryGetNumberField(TEXT("index"), SampleIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("replace_blendspace_sample: missing 'sample_index' (int)"));
    }

    // Resolve the blendspace asset before the sample-index bounds check
    // so the response can also surface the asset path on error.
    UObject* BlendSpaceAsset = UEditorAssetLibrary::LoadAsset(BlendSpaceParam);
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(BlendSpaceAsset);
    if (!BlendSpace)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("replace_blendspace_sample: '%s' is not a UBlendSpace / UBlendSpace1D"), *BlendSpaceParam));
    }

    const int32 SampleCount = BlendSpace->GetBlendSamples().Num();
    if (!BlendSpace->IsValidBlendSampleIndex(SampleIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("replace_blendspace_sample: 'sample_index' %d is out of range [0, %d)"), SampleIndex, SampleCount));
    }

    // Read previous sequence so the response can report what we replaced.
    const FBlendSample& PreviousSample = BlendSpace->GetBlendSample(SampleIndex);
    UAnimSequence* PreviousAnimation = PreviousSample.Animation;
    const FString PreviousPath = PreviousAnimation ? PreviousAnimation->GetPathName() : FString();
    const FString PreviousName = PreviousAnimation ? PreviousAnimation->GetName() : FString();

    // Animation token resolution: an empty / "none" string (or
    // `clear=true`) unbinds the sample's UAnimSequence; otherwise we
    // resolve the new sequence and run the same skeleton + additive
    // compatibility checks AddSample uses so the asset never lands in
    // a state the engine refuses to play.
    bool bClear = false;
    Params->TryGetBoolField(TEXT("clear"), bClear);

    FString AnimParam;
    Params->TryGetStringField(TEXT("animation"), AnimParam);
    if (AnimParam.IsEmpty())
    {
        Params->TryGetStringField(TEXT("anim_sequence"), AnimParam);
    }
    if (AnimParam.IsEmpty())
    {
        Params->TryGetStringField(TEXT("sequence"), AnimParam);
    }

    UAnimSequence* AnimSequence = nullptr;
    const bool bRequestedClear = bClear
        || AnimParam.IsEmpty()
        || AnimParam.Equals(TEXT("none"), ESearchCase::IgnoreCase);
    if (!bRequestedClear)
    {
        UObject* AnimAsset = UEditorAssetLibrary::LoadAsset(AnimParam);
        AnimSequence = Cast<UAnimSequence>(AnimAsset);
        if (!AnimSequence)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("replace_blendspace_sample: 'animation' '%s' is not a UAnimSequence"), *AnimParam));
        }
        if (!BlendSpace->IsAnimationCompatibleWithSkeleton(AnimSequence))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("replace_blendspace_sample: animation skeleton does not match blendspace target skeleton")));
        }
        if (!BlendSpace->IsAnimationCompatible(AnimSequence))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("replace_blendspace_sample: animation additive type does not match existing samples")));
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // ReplaceSampleAnimation accepts a null pointer to unbind the
    // sample's sequence; that matches the editor's "Clear" picker.
    const bool bReplaced = BlendSpace->ReplaceSampleAnimation(SampleIndex, AnimSequence);
    if (!bReplaced)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("replace_blendspace_sample: UBlendSpace::ReplaceSampleAnimation refused the swap at index %d"), SampleIndex));
    }
    BlendSpace->MarkPackageDirty();

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(BlendSpace->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("replace_blendspace_sample"));
    Result->SetStringField(TEXT("asset"), BlendSpace->GetName());
    Result->SetStringField(TEXT("path"), BlendSpace->GetPathName());
    Result->SetStringField(TEXT("class"), BlendSpace->GetClass()->GetName());
    Result->SetNumberField(TEXT("sample_index"), SampleIndex);
    Result->SetBoolField(TEXT("cleared"), bRequestedClear);
    if (AnimSequence)
    {
        Result->SetStringField(TEXT("animation_path"), AnimSequence->GetPathName());
        Result->SetStringField(TEXT("animation_name"), AnimSequence->GetName());
    }
    if (!PreviousPath.IsEmpty())
    {
        Result->SetStringField(TEXT("previous_animation_path"), PreviousPath);
        Result->SetStringField(TEXT("previous_animation_name"), PreviousName);
    }
    Result->SetNumberField(TEXT("sample_count"), SampleCount);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleDeleteBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpaceParam;
    if (!Params->TryGetStringField(TEXT("blendspace"), BlendSpaceParam)
        && !Params->TryGetStringField(TEXT("blend_space"), BlendSpaceParam)
        && !Params->TryGetStringField(TEXT("asset"), BlendSpaceParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("delete_blendspace_sample: missing 'blendspace' (asset path)"));
    }
    if (BlendSpaceParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("delete_blendspace_sample: 'blendspace' must be non-empty"));
    }

    int32 SampleIndex = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("sample_index"), SampleIndex)
        && !Params->TryGetNumberField(TEXT("index"), SampleIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("delete_blendspace_sample: missing 'sample_index' (int)"));
    }

    UObject* BlendSpaceAsset = UEditorAssetLibrary::LoadAsset(BlendSpaceParam);
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(BlendSpaceAsset);
    if (!BlendSpace)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("delete_blendspace_sample: '%s' is not a UBlendSpace / UBlendSpace1D"), *BlendSpaceParam));
    }

    const int32 PreviousSampleCount = BlendSpace->GetBlendSamples().Num();
    if (!BlendSpace->IsValidBlendSampleIndex(SampleIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("delete_blendspace_sample: 'sample_index' %d is out of range [0, %d)"), SampleIndex, PreviousSampleCount));
    }

    // Capture the about-to-go sample so the response can echo what we
    // removed (helpful for callers reading the surface back).
    const FBlendSample& Removed = BlendSpace->GetBlendSample(SampleIndex);
    UAnimSequence* RemovedAnimation = Removed.Animation;
    const FString RemovedPath = RemovedAnimation ? RemovedAnimation->GetPathName() : FString();
    const FString RemovedName = RemovedAnimation ? RemovedAnimation->GetName() : FString();
    const FVector RemovedValue = Removed.SampleValue;

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    const bool bDeleted = BlendSpace->DeleteSample(SampleIndex);
    if (!bDeleted)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("delete_blendspace_sample: UBlendSpace::DeleteSample refused the delete at index %d"), SampleIndex));
    }
    BlendSpace->MarkPackageDirty();

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(BlendSpace->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
    const int32 AxisCount = bIs1D ? 1 : 2;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("delete_blendspace_sample"));
    Result->SetStringField(TEXT("asset"), BlendSpace->GetName());
    Result->SetStringField(TEXT("path"), BlendSpace->GetPathName());
    Result->SetStringField(TEXT("class"), BlendSpace->GetClass()->GetName());
    Result->SetNumberField(TEXT("sample_index"), SampleIndex);
    Result->SetNumberField(TEXT("axis_count"), AxisCount);
    if (!RemovedPath.IsEmpty())
    {
        Result->SetStringField(TEXT("removed_animation_path"), RemovedPath);
        Result->SetStringField(TEXT("removed_animation_name"), RemovedName);
    }
    {
        TArray<TSharedPtr<FJsonValue>> SampleJson;
        SampleJson.Add(MakeShared<FJsonValueNumber>(RemovedValue.X));
        if (AxisCount >= 2) SampleJson.Add(MakeShared<FJsonValueNumber>(RemovedValue.Y));
        if (AxisCount >= 3) SampleJson.Add(MakeShared<FJsonValueNumber>(RemovedValue.Z));
        Result->SetArrayField(TEXT("removed_sample_value"), SampleJson);
    }
    Result->SetNumberField(TEXT("previous_sample_count"), PreviousSampleCount);
    Result->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddMetadataCurve(const TSharedPtr<FJsonObject>& Params)
{
    // Adds a typed metadata curve to a UAnimSequenceBase. The earlier
    // `add_curve` op covered the canonical Float / Vector / Transform
    // shapes that drive timeline values. This op covers the typed
    // metadata curves AnimBPs use as runtime triggers:
    //
    //   - Material:  per-skeleton FCurveMetaData::Type.bMaterial    set.
    //   - Morph:     per-skeleton FCurveMetaData::Type.bMorphtarget set.
    //   - Attribute: a plain metadata curve, neither bit set (the
    //                "Attribute / Misc" bucket for game-side flags).
    //
    // In all three cases the asset-side curve is a Float curve flagged
    // with AACF_Metadata so the timeline stores a sparse boolean
    // trigger marker rather than a smooth scalar; the typing lives on
    // the USkeleton's per-curve FCurveMetaData (the legacy
    // AACF_DriveMaterial / AACF_DriveMorphTarget flags moved here in
    // 5.x and are marked Hidden on the asset side).
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_metadata_curve: missing 'asset'"));
    }
    FString CurveNameParam;
    if (!Params->TryGetStringField(TEXT("curve_name"), CurveNameParam)
        && !Params->TryGetStringField(TEXT("name"), CurveNameParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_metadata_curve: missing 'curve_name'"));
    }
    if (CurveNameParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_metadata_curve: 'curve_name' must be non-empty"));
    }

    FString TypeToken;
    if (!Params->TryGetStringField(TEXT("type"), TypeToken)
        && !Params->TryGetStringField(TEXT("metadata_type"), TypeToken)
        && !Params->TryGetStringField(TEXT("curve_type"), TypeToken)
        && !Params->TryGetStringField(TEXT("kind"), TypeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_metadata_curve: missing 'type' (one of Material / Morph / Attribute)"));
    }

    enum class EMetadataKind { Material, Morph, Attribute };
    EMetadataKind Kind = EMetadataKind::Attribute;
    FString CanonicalKindToken;
    const FString TypeLower = TypeToken.ToLower();
    if (TypeLower == TEXT("material") || TypeLower == TEXT("mat"))
    {
        Kind = EMetadataKind::Material;
        CanonicalKindToken = TEXT("Material");
    }
    else if (TypeLower == TEXT("morph") || TypeLower == TEXT("morphtarget")
        || TypeLower == TEXT("morph_target"))
    {
        Kind = EMetadataKind::Morph;
        CanonicalKindToken = TEXT("Morph");
    }
    else if (TypeLower == TEXT("attribute") || TypeLower == TEXT("attr")
        || TypeLower == TEXT("misc") || TypeLower == TEXT("metadata"))
    {
        Kind = EMetadataKind::Attribute;
        CanonicalKindToken = TEXT("Attribute");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_metadata_curve: unknown 'type' '%s'; expected Material / Morph / Attribute"),
                *TypeToken));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(Asset);
    if (!SeqBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_metadata_curve: '%s' is not a UAnimSequenceBase"), *AssetParam));
    }
    USkeleton* Skeleton = SeqBase->GetSkeleton();
    if (!Skeleton)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_metadata_curve: '%s' has no Skeleton; cannot register typed metadata"),
                *SeqBase->GetName()));
    }

    const FName CurveFName(*CurveNameParam);

    // Per-asset side: register the curve through the documented BP
    // library entry point with `bMetaDataCurve=true`. The Float type
    // is the only one that supports the metadata flag; the engine
    // treats a metadata curve as a Float curve whose AACF_Metadata
    // bit is set on the asset.
    UAnimationBlueprintLibrary::AddCurve(SeqBase, CurveFName, ERawCurveTrackTypes::RCT_Float, /*bMetaDataCurve=*/true);

    // Optional keyframe list. Each row is `[time, value]` where value
    // is a float; metadata curves are typically driven 0/1, but the
    // editor lets designers tune the curve so we forward whatever the
    // caller passes through the same AddFloatCurveKeys writer
    // `add_curve` uses for Float curves.
    int32 KeyframeCount = 0;
    int32 KeyframeFailures = 0;
    TArray<TSharedPtr<FJsonValue>> KeyframeErrors;

    const TArray<TSharedPtr<FJsonValue>>* KeyframesArr = nullptr;
    if (Params->TryGetArrayField(TEXT("keyframes"), KeyframesArr) && KeyframesArr)
    {
        TArray<float> Times;
        TArray<float> Values;
        for (int32 RowIdx = 0; RowIdx < KeyframesArr->Num(); ++RowIdx)
        {
            const TSharedPtr<FJsonValue>& RowVal = (*KeyframesArr)[RowIdx];
            if (!RowVal.IsValid() || RowVal->Type != EJson::Array)
            {
                KeyframeErrors.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("row %d is not an array"), RowIdx)));
                ++KeyframeFailures;
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>& Row = RowVal->AsArray();
            if (Row.Num() < 2 || Row[1]->Type != EJson::Number)
            {
                KeyframeErrors.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("row %d expected [time, value]"), RowIdx)));
                ++KeyframeFailures;
                continue;
            }
            Times.Add(static_cast<float>(Row[0]->AsNumber()));
            Values.Add(static_cast<float>(Row[1]->AsNumber()));
            ++KeyframeCount;
        }
        if (Times.Num() > 0)
        {
            UAnimationBlueprintLibrary::AddFloatCurveKeys(SeqBase, CurveFName, Times, Values);
        }
    }

    // Per-skeleton side: register the curve's metadata entry on the
    // USkeleton (idempotent; returns false when the entry already
    // exists) and flip the Material / MorphTarget bits to match the
    // requested kind. The Attribute case clears both bits so the
    // curve flows through the engine's "no typed driver" path.
    bool bMetadataEntryAdded = false;
    bool bMaterialFlag = false;
    bool bMorphFlag = false;

#if WITH_EDITOR
    bMetadataEntryAdded = Skeleton->AddCurveMetaData(CurveFName, /*bTransact=*/true);
    switch (Kind)
    {
        case EMetadataKind::Material:
            Skeleton->SetCurveMetaDataMaterial(CurveFName, true);
            Skeleton->SetCurveMetaDataMorphTarget(CurveFName, false);
            bMaterialFlag = true;
            break;
        case EMetadataKind::Morph:
            Skeleton->SetCurveMetaDataMaterial(CurveFName, false);
            Skeleton->SetCurveMetaDataMorphTarget(CurveFName, true);
            bMorphFlag = true;
            break;
        case EMetadataKind::Attribute:
            Skeleton->SetCurveMetaDataMaterial(CurveFName, false);
            Skeleton->SetCurveMetaDataMorphTarget(CurveFName, false);
            break;
    }
#else
    // Non-editor builds expose the public AccumulateCurveMetaData hot
    // path. We fall through it so the runtime stays consistent.
    Skeleton->AccumulateCurveMetaData(CurveFName,
        /*bMaterialSet=*/ Kind == EMetadataKind::Material,
        /*bMorphtargetSet=*/ Kind == EMetadataKind::Morph);
    bMaterialFlag = (Kind == EMetadataKind::Material);
    bMorphFlag = (Kind == EMetadataKind::Morph);
#endif

    SeqBase->MarkPackageDirty();
    Skeleton->MarkPackageDirty();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bSavedSeq = false;
    bool bSavedSkel = false;
    if (bSave)
    {
        bSavedSeq = UEditorAssetLibrary::SaveAsset(SeqBase->GetPathName(), /*bOnlyIfIsDirty=*/false);
        bSavedSkel = UEditorAssetLibrary::SaveAsset(Skeleton->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_metadata_curve"));
    Result->SetStringField(TEXT("asset"), SeqBase->GetName());
    Result->SetStringField(TEXT("path"), SeqBase->GetPathName());
    Result->SetStringField(TEXT("class"), SeqBase->GetClass()->GetName());
    Result->SetStringField(TEXT("curve_name"), CurveNameParam);
    Result->SetStringField(TEXT("curve_type"), TEXT("float"));
    Result->SetStringField(TEXT("metadata_type"), CanonicalKindToken);
    Result->SetBoolField(TEXT("metadata_curve"), true);
    Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    Result->SetBoolField(TEXT("skeleton_entry_added"), bMetadataEntryAdded);
    Result->SetBoolField(TEXT("material_flag"), bMaterialFlag);
    Result->SetBoolField(TEXT("morph_flag"), bMorphFlag);
    Result->SetNumberField(TEXT("keyframes_added"), KeyframeCount);
    Result->SetNumberField(TEXT("keyframe_failures"), KeyframeFailures);
    if (KeyframeErrors.Num() > 0)
    {
        Result->SetArrayField(TEXT("keyframe_errors"), KeyframeErrors);
    }
    Result->SetBoolField(TEXT("saved"), bSavedSeq);
    Result->SetBoolField(TEXT("saved_skeleton"), bSavedSkel);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetRootMotion(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the four canonical root-motion UPROPERTYs on a UAnimSequence:
    //   - bEnableRootMotion (bool, required)
    //   - RootMotionRootLock (ERootMotionRootLock token: RefPose /
    //     AnimFirstFrame / Zero; optional)
    //   - bForceRootLock (optional)
    //   - bUseNormalizedRootMotionScale (optional)
    // The four fields are public UPROPERTYs on UAnimSequence under
    // Category=RootMotion (see Engine/Classes/Animation/AnimSequence.h).
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_root_motion: missing 'asset'"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Seq)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_root_motion: '%s' is not a UAnimSequence (root-motion fields live on UAnimSequence)"), *AssetParam));
    }

    // Capture before-state so the response carries a diffable record.
    const bool bPrevEnable = Seq->bEnableRootMotion;
    const TEnumAsByte<ERootMotionRootLock::Type> PrevLock = Seq->RootMotionRootLock;
    const bool bPrevForce = Seq->bForceRootLock;
    const bool bPrevNorm = Seq->bUseNormalizedRootMotionScale;

    auto LockTokenFor = [](TEnumAsByte<ERootMotionRootLock::Type> V) -> FString
    {
        switch (V.GetValue())
        {
            case ERootMotionRootLock::RefPose: return TEXT("RefPose");
            case ERootMotionRootLock::AnimFirstFrame: return TEXT("AnimFirstFrame");
            case ERootMotionRootLock::Zero: return TEXT("Zero");
            default: return TEXT("Unknown");
        }
    };

    // Read the required enable flag. Accept booleans, numeric 0/1, and
    // string tokens (true / false / on / off / yes / no) so callers
    // can land the flag without preserialising into the JSON bool form.
    bool bNewEnable = bPrevEnable;
    bool bEnableProvided = false;
    {
        const TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("enable"));
        TSharedPtr<FJsonValue> Pick = ValueJson;
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("enable_root_motion"));
        }
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("b_enable_root_motion"));
        }
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("bEnableRootMotion"));
        }
        if (Pick.IsValid())
        {
            bEnableProvided = true;
            if (Pick->Type == EJson::Boolean)
            {
                bNewEnable = Pick->AsBool();
            }
            else if (Pick->Type == EJson::Number)
            {
                bNewEnable = (Pick->AsNumber() != 0.0);
            }
            else if (Pick->Type == EJson::String)
            {
                const FString SLower = Pick->AsString().ToLower();
                bNewEnable = (SLower == TEXT("true") || SLower == TEXT("1")
                    || SLower == TEXT("on") || SLower == TEXT("yes")
                    || SLower == TEXT("enable"));
            }
            else
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_root_motion: 'enable' must be a bool, number, or string"));
            }
        }
    }
    if (!bEnableProvided)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_root_motion: missing 'enable' boolean (required)"));
    }

    // Optional RootMotionRootLock token.
    bool bLockProvided = false;
    TEnumAsByte<ERootMotionRootLock::Type> NewLock = PrevLock;
    FString LockToken;
    if (Params->TryGetStringField(TEXT("root_motion_root_lock"), LockToken)
        || Params->TryGetStringField(TEXT("root_lock"), LockToken)
        || Params->TryGetStringField(TEXT("rootmotion_root_lock"), LockToken)
        || Params->TryGetStringField(TEXT("RootMotionRootLock"), LockToken)
        || Params->TryGetStringField(TEXT("lock"), LockToken))
    {
        const FString L = LockToken.ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
        if (L == TEXT("refpose") || L == TEXT("reference_pose") || L == TEXT("referencepose"))
        {
            NewLock = ERootMotionRootLock::RefPose;
            bLockProvided = true;
        }
        else if (L == TEXT("animfirstframe") || L == TEXT("anim_first_frame")
            || L == TEXT("firstframe") || L == TEXT("first_frame"))
        {
            NewLock = ERootMotionRootLock::AnimFirstFrame;
            bLockProvided = true;
        }
        else if (L == TEXT("zero") || L == TEXT("identity"))
        {
            NewLock = ERootMotionRootLock::Zero;
            bLockProvided = true;
        }
        else
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_root_motion: unknown root_motion_root_lock '%s'; expected RefPose / AnimFirstFrame / Zero"), *LockToken));
        }
    }

    // Optional bForceRootLock.
    bool bForceProvided = false;
    bool bNewForce = bPrevForce;
    {
        const TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("force_root_lock"));
        TSharedPtr<FJsonValue> Pick = ValueJson;
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("b_force_root_lock"));
        }
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("bForceRootLock"));
        }
        if (Pick.IsValid())
        {
            bForceProvided = true;
            if (Pick->Type == EJson::Boolean) bNewForce = Pick->AsBool();
            else if (Pick->Type == EJson::Number) bNewForce = (Pick->AsNumber() != 0.0);
            else if (Pick->Type == EJson::String)
            {
                const FString SLower = Pick->AsString().ToLower();
                bNewForce = (SLower == TEXT("true") || SLower == TEXT("1")
                    || SLower == TEXT("on") || SLower == TEXT("yes"));
            }
            else
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_root_motion: 'force_root_lock' must be a bool, number, or string"));
            }
        }
    }

    // Optional bUseNormalizedRootMotionScale.
    bool bNormProvided = false;
    bool bNewNorm = bPrevNorm;
    {
        const TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("use_normalized_root_motion_scale"));
        TSharedPtr<FJsonValue> Pick = ValueJson;
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("normalized_root_motion_scale"));
        }
        if (!Pick.IsValid())
        {
            Pick = Params->TryGetField(TEXT("bUseNormalizedRootMotionScale"));
        }
        if (Pick.IsValid())
        {
            bNormProvided = true;
            if (Pick->Type == EJson::Boolean) bNewNorm = Pick->AsBool();
            else if (Pick->Type == EJson::Number) bNewNorm = (Pick->AsNumber() != 0.0);
            else if (Pick->Type == EJson::String)
            {
                const FString SLower = Pick->AsString().ToLower();
                bNewNorm = (SLower == TEXT("true") || SLower == TEXT("1")
                    || SLower == TEXT("on") || SLower == TEXT("yes"));
            }
            else
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_root_motion: 'use_normalized_root_motion_scale' must be a bool, number, or string"));
            }
        }
    }

    // Apply the writes. Each field is a public UPROPERTY so a direct
    // member write is enough; PostEditChangeProperty fires after the
    // writes so any open editor refreshes.
    Seq->bEnableRootMotion = bNewEnable;
    if (bLockProvided)
    {
        Seq->RootMotionRootLock = NewLock;
    }
    if (bForceProvided)
    {
        Seq->bForceRootLock = bNewForce;
    }
    if (bNormProvided)
    {
        Seq->bUseNormalizedRootMotionScale = bNewNorm;
    }

#if WITH_EDITOR
    Seq->PostEditChange();
#endif
    Seq->MarkPackageDirty();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Seq->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_root_motion"));
    Result->SetStringField(TEXT("asset"), AssetParam);
    Result->SetStringField(TEXT("path"), Seq->GetPathName());
    Result->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    Result->SetBoolField(TEXT("enable_root_motion"), bNewEnable);
    Result->SetBoolField(TEXT("previous_enable_root_motion"), bPrevEnable);
    Result->SetStringField(TEXT("root_motion_root_lock"), LockTokenFor(Seq->RootMotionRootLock));
    Result->SetStringField(TEXT("previous_root_motion_root_lock"), LockTokenFor(PrevLock));
    Result->SetBoolField(TEXT("force_root_lock"), Seq->bForceRootLock);
    Result->SetBoolField(TEXT("previous_force_root_lock"), bPrevForce);
    Result->SetBoolField(TEXT("use_normalized_root_motion_scale"), Seq->bUseNormalizedRootMotionScale);
    Result->SetBoolField(TEXT("previous_use_normalized_root_motion_scale"), bPrevNorm);
    Result->SetBoolField(TEXT("lock_provided"), bLockProvided);
    Result->SetBoolField(TEXT("force_root_lock_provided"), bForceProvided);
    Result->SetBoolField(TEXT("use_normalized_root_motion_scale_provided"), bNormProvided);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddNotifyState(const TSharedPtr<FJsonObject>& Params)
{
    // Pairs with the existing `add_notify` op. `add_notify` covers both
    // UAnimNotify and UAnimNotifyState shapes through a single seconds-
    // based `time` + `duration` pair, which is convenient for one-shot
    // event notifies but awkward for state notifies that the designer
    // thinks of in frame counts. This op accepts the canonical
    // `start_frame` + `duration_frames` pair, refuses non-state classes,
    // and routes through the same public AnimationBlueprintLibrary
    // entry point.
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_notify_state: missing 'asset'"));
    }
    FString TrackParam;
    if (!Params->TryGetStringField(TEXT("track"), TrackParam)
        && !Params->TryGetStringField(TEXT("track_name"), TrackParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_notify_state: missing 'track' name"));
    }
    if (TrackParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_notify_state: 'track' must be non-empty"));
    }

    FString NotifyClassParam;
    if (!Params->TryGetStringField(TEXT("notify_state_class"), NotifyClassParam)
        && !Params->TryGetStringField(TEXT("notify_class"), NotifyClassParam)
        && !Params->TryGetStringField(TEXT("class"), NotifyClassParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_notify_state: missing 'notify_state_class' (UAnimNotifyState subclass path or short name)"));
    }
    if (NotifyClassParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_notify_state: 'notify_state_class' must be non-empty"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(Asset);
    if (!SeqBase)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_notify_state: '%s' is not a UAnimSequenceBase"), *AssetParam));
    }

    // Resolve the class against UAnimNotifyState. The existing
    // `ResolveNotifyClass` helper takes a base-class filter and rejects
    // anything that fails the IsChildOf check, so a UAnimNotify (the
    // point-notify shape) lands as a clear error rather than silently
    // misrouting to the AddAnimationNotifyEvent path.
    UClass* StateClass = ResolveNotifyClass(NotifyClassParam, UAnimNotifyState::StaticClass());
    if (!StateClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_notify_state: failed to resolve '%s' as a UAnimNotifyState subclass (point-notify shapes go through 'add_notify')"), *NotifyClassParam));
    }
    if (StateClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_notify_state: '%s' resolves to an abstract class; pick a concrete UAnimNotifyState subclass"), *NotifyClassParam));
    }

    // Resolve start + duration in seconds. Frame counts win; seconds
    // shapes (`start_time` + `duration`) are accepted for symmetry with
    // `add_notify`'s seconds-friendly precedence.
    double StartFrameValue = 0.0;
    const bool bHasStartFrame = Params->TryGetNumberField(TEXT("start_frame"), StartFrameValue)
        || Params->TryGetNumberField(TEXT("frame"), StartFrameValue);
    double StartTimeValue = 0.0;
    const bool bHasStartTime = Params->TryGetNumberField(TEXT("start_time"), StartTimeValue)
        || Params->TryGetNumberField(TEXT("time"), StartTimeValue);
    if (!bHasStartFrame && !bHasStartTime)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_notify_state: one of 'start_frame' (int) or 'start_time' (float seconds) is required"));
    }

    double DurationFramesValue = 0.0;
    const bool bHasDurationFrames = Params->TryGetNumberField(TEXT("duration_frames"), DurationFramesValue);
    double DurationSecondsValue = 0.0;
    const bool bHasDurationSeconds = Params->TryGetNumberField(TEXT("duration"), DurationSecondsValue)
        || Params->TryGetNumberField(TEXT("duration_seconds"), DurationSecondsValue);
    if (!bHasDurationFrames && !bHasDurationSeconds)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_notify_state: one of 'duration_frames' (int) or 'duration' (float seconds) is required"));
    }

    // The seconds-conversion path matches `add_notify`: the public
    // UAnimSequence::GetSamplingFrameRate is the canonical frames-to-
    // seconds ratio. UAnimMontage and UAnimComposite branches do not
    // expose the per-asset frame rate so we fall back to 30 fps.
    FFrameRate Rate(30, 1);
    if (UAnimSequence* Seq = Cast<UAnimSequence>(SeqBase))
    {
        const FFrameRate AssetRate = Seq->GetSamplingFrameRate();
        if (AssetRate.Numerator > 0)
        {
            Rate = AssetRate;
        }
    }

    float StartTime = 0.0f;
    if (bHasStartFrame)
    {
        const FFrameTime FrameTime(FFrameNumber(static_cast<int32>(StartFrameValue)));
        StartTime = static_cast<float>(Rate.AsSeconds(FrameTime));
    }
    else
    {
        StartTime = static_cast<float>(StartTimeValue);
    }
    if (StartTime < 0.0f)
    {
        StartTime = 0.0f;
    }

    float Duration = 0.0f;
    if (bHasDurationFrames)
    {
        const FFrameTime FrameTime(FFrameNumber(static_cast<int32>(DurationFramesValue)));
        Duration = static_cast<float>(Rate.AsSeconds(FrameTime));
    }
    else
    {
        Duration = static_cast<float>(DurationSecondsValue);
    }
    if (Duration <= 0.0f)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_notify_state: duration resolved to %f seconds; the engine treats non-positive state notifies as zero-length and ignores them"), Duration));
    }

    // Auto-create the notify track when missing, matching `add_notify`.
    const FName TrackFName(*TrackParam);
    if (!UAnimationBlueprintLibrary::IsValidAnimNotifyTrackName(SeqBase, TrackFName))
    {
        UAnimationBlueprintLibrary::AddAnimationNotifyTrack(SeqBase, TrackFName, FLinearColor::White);
    }

    UAnimNotifyState* CreatedNotify = UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent(
        SeqBase, TrackFName, StartTime, Duration, StateClass);
    if (!CreatedNotify)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_notify_state: AddAnimationNotifyStateEvent returned null for class '%s' on track '%s'"), *StateClass->GetName(), *TrackParam));
    }

    SeqBase->MarkPackageDirty();
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(SeqBase->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_notify_state"));
    Result->SetStringField(TEXT("asset"), SeqBase->GetName());
    Result->SetStringField(TEXT("path"), SeqBase->GetPathName());
    Result->SetStringField(TEXT("class"), SeqBase->GetClass()->GetName());
    Result->SetStringField(TEXT("track_name"), TrackFName.ToString());
    Result->SetStringField(TEXT("notify_class"), StateClass->GetName());
    Result->SetStringField(TEXT("notify_class_path"), StateClass->GetPathName());
    Result->SetStringField(TEXT("notify_kind"), TEXT("state"));
    Result->SetNumberField(TEXT("start_time"), StartTime);
    Result->SetNumberField(TEXT("duration"), Duration);
    Result->SetNumberField(TEXT("end_time"), StartTime + Duration);
    Result->SetNumberField(TEXT("frame_rate_numerator"), Rate.Numerator);
    Result->SetNumberField(TEXT("frame_rate_denominator"), Rate.Denominator);
    if (bHasStartFrame)
    {
        Result->SetNumberField(TEXT("start_frame"), StartFrameValue);
    }
    if (bHasDurationFrames)
    {
        Result->SetNumberField(TEXT("duration_frames"), DurationFramesValue);
    }
    Result->SetStringField(TEXT("notify_object"), CreatedNotify->GetName());
    Result->SetNumberField(TEXT("notify_count"), SeqBase->Notifies.Num());
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetCompressionScheme(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the per-sequence compression slot. UE5.x routes compression
    // through `UAnimSequence::BoneCompressionSettings` (a
    // UAnimBoneCompressionSettings DataAsset). The settings asset holds a
    // `Codecs` array of UAnimBoneCompressionCodec subclasses; the engine
    // runs the codecs in turn and picks the best one for each clip per
    // its quality / error metrics.
    //
    // The legacy UAnimCompress_* (UAnimCompress_BitwiseCompressOnly,
    // UAnimCompress_RemoveLinearKeys, UAnimCompress_RemoveTrivialKeys,
    // etc.) survived the 5.x refactor as UAnimBoneCompressionCodec
    // subclasses so callers can still pass these tokens. Resolves
    // either: (1) an existing UAnimBoneCompressionSettings DataAsset
    // path on the project, written into the BoneCompressionSettings
    // slot directly, or (2) a UAnimBoneCompressionCodec subclass that
    // we wrap into a per-sequence settings subobject before assigning.
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        if (!Params->TryGetStringField(TEXT("sequence"), AssetParam)
            && !Params->TryGetStringField(TEXT("path"), AssetParam))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_compression_scheme: missing 'asset' parameter"));
        }
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Seq)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_compression_scheme: '%s' is not a UAnimSequence (compression settings live on UAnimSequence)"), *AssetParam));
    }

    // Capture the previous slot for the diff field on the response.
    UAnimBoneCompressionSettings* PreviousSettings = Seq->BoneCompressionSettings;
    FString PreviousSettingsPath;
    if (PreviousSettings)
    {
        PreviousSettingsPath = PreviousSettings->GetPathName();
    }

    UAnimBoneCompressionSettings* NewSettings = nullptr;
    FString ResolvedCodecClassPath;
    FString ResolvedCodecClassName;
    bool bCodecAuthored = false;

    // Path 1: caller supplied a `/Game/...` UAnimBoneCompressionSettings
    // DataAsset path directly. Resolve through UEditorAssetLibrary and
    // refuse anything that is not the expected class.
    FString SettingsPath;
    if (Params->TryGetStringField(TEXT("compression_settings"), SettingsPath)
        || Params->TryGetStringField(TEXT("settings"), SettingsPath)
        || Params->TryGetStringField(TEXT("settings_path"), SettingsPath))
    {
        if (!SettingsPath.IsEmpty())
        {
            UObject* SettingsAsset = UEditorAssetLibrary::LoadAsset(SettingsPath);
            NewSettings = Cast<UAnimBoneCompressionSettings>(SettingsAsset);
            if (!NewSettings)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_compression_scheme: 'compression_settings' path '%s' is not a UAnimBoneCompressionSettings DataAsset"), *SettingsPath));
            }
        }
    }

    // Path 2: caller supplied a codec class name / path. We NewObject
    // a per-sequence UAnimBoneCompressionSettings, outered to the
    // sequence so the new subobject saves alongside the sequence
    // package, and assign one fresh codec subobject of the requested
    // class into the Codecs array. Per-sequence settings isolate the
    // choice from any other sequence on the project, mirroring the
    // editor's "Convert to Custom" right-click on the asset.
    FString CodecToken;
    if (!NewSettings)
    {
        if (Params->TryGetStringField(TEXT("compression_codec"), CodecToken)
            || Params->TryGetStringField(TEXT("compression_scheme"), CodecToken)
            || Params->TryGetStringField(TEXT("compression_class"), CodecToken)
            || Params->TryGetStringField(TEXT("scheme"), CodecToken)
            || Params->TryGetStringField(TEXT("codec"), CodecToken))
        {
            if (CodecToken.IsEmpty())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_compression_scheme: 'compression_codec' / 'scheme' must not be empty"));
            }
            UClass* CodecClass = ResolveNotifyClass(CodecToken, UAnimBoneCompressionCodec::StaticClass());
            if (!CodecClass)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_compression_scheme: could not resolve UAnimBoneCompressionCodec subclass from '%s' (try UAnimCompress_BitwiseCompressOnly / UAnimCompress_RemoveLinearKeys / UAnimCompress_RemoveTrivialKeys)"), *CodecToken));
            }
            if (CodecClass->HasAnyClassFlags(CLASS_Abstract))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_compression_scheme: codec class '%s' is abstract"), *CodecClass->GetName()));
            }

            // Outer the new per-sequence settings to the sequence's
            // package so the subobject lands in the sequence file
            // rather than the transient package. The editor uses the
            // same shape for "custom" per-sequence settings.
            UAnimBoneCompressionSettings* Authored = NewObject<UAnimBoneCompressionSettings>(
                Seq->GetOutermost(), MakeUniqueObjectName(Seq->GetOutermost(), UAnimBoneCompressionSettings::StaticClass(), TEXT("BoneCompressionSettings_Custom")),
                RF_Public | RF_Standalone | RF_Transactional);
            if (!Authored)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_compression_scheme: NewObject<UAnimBoneCompressionSettings> failed"));
            }
            UAnimBoneCompressionCodec* CodecInstance = NewObject<UAnimBoneCompressionCodec>(
                Authored, CodecClass, NAME_None,
                RF_Public | RF_Transactional);
            if (!CodecInstance)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_compression_scheme: NewObject<%s> failed"), *CodecClass->GetName()));
            }
            Authored->Codecs.Add(CodecInstance);
            NewSettings = Authored;
            ResolvedCodecClassPath = CodecClass->GetPathName();
            ResolvedCodecClassName = CodecClass->GetName();
            bCodecAuthored = true;
        }
    }

    if (!NewSettings)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_compression_scheme: pass either 'compression_settings' (DataAsset path) or 'compression_codec' (codec class)"));
    }

    // Modify before the write so any open editor undo records the
    // change, then assign the slot. PostEditChangeProperty fires on the
    // sequence so a Persona-side viewmodel listener picks the swap up.
    Seq->Modify();
    Seq->BoneCompressionSettings = NewSettings;
#if WITH_EDITOR
    if (FProperty* BCSProp = FindFProperty<FProperty>(UAnimSequence::StaticClass(), TEXT("BoneCompressionSettings")))
    {
        FPropertyChangedEvent Event(BCSProp, EPropertyChangeType::ValueSet);
        Seq->PostEditChangeProperty(Event);
    }
#endif

    // Optional sync compression refresh. The public NIAGARA-style
    // engine call is UAnimSequence::RequestAnimCompression with a
    // FRequestAnimCompressionParams instance. The default-constructed
    // params run synchronously; we forward through the public NOOP
    // overload that takes the params struct, so the DDC bake reruns
    // with the new codec before we save.
    bool bRequestCompile = false;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);
    if (!bRequestCompile)
    {
        Params->TryGetBoolField(TEXT("recompile"), bRequestCompile);
    }
    if (!bRequestCompile)
    {
        Params->TryGetBoolField(TEXT("request_compression"), bRequestCompile);
    }
    bool bRequestedCompile = false;
#if WITH_EDITOR
    if (bRequestCompile)
    {
        // FRequestAnimCompressionParams takes a UAnimSequence pointer in
        // the documented public ctor; the default-constructed value is
        // safe and uses the engine's current platform settings.
        FRequestAnimCompressionParams CompressionParams(Seq);
        Seq->RequestAnimCompression(CompressionParams);
        bRequestedCompile = true;
    }
#endif

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    Seq->MarkPackageDirty();
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Seq->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_compression_scheme"));
    Result->SetStringField(TEXT("asset"), AssetParam);
    Result->SetStringField(TEXT("path"), Seq->GetPathName());
    Result->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    Result->SetStringField(TEXT("settings_path"), NewSettings->GetPathName());
    Result->SetStringField(TEXT("settings_class"), NewSettings->GetClass()->GetName());
    Result->SetBoolField(TEXT("settings_authored"), bCodecAuthored);
    if (!ResolvedCodecClassPath.IsEmpty())
    {
        Result->SetStringField(TEXT("codec_class"), ResolvedCodecClassName);
        Result->SetStringField(TEXT("codec_class_path"), ResolvedCodecClassPath);
        Result->SetNumberField(TEXT("codec_count"), NewSettings->Codecs.Num());
    }
    if (!PreviousSettingsPath.IsEmpty())
    {
        Result->SetStringField(TEXT("previous_settings_path"), PreviousSettingsPath);
    }
    Result->SetBoolField(TEXT("requested_compile"), bRequestedCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetCurveCompression(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the per-sequence curve compression slot. Complement to
    // HandleSetCompressionScheme which covers the bone-track side.
    // UE5 routes float-curve compression through
    // `UAnimSequence::CurveCompressionSettings` (a
    // UAnimCurveCompressionSettings DataAsset whose `Codec` slot holds
    // a UAnimCurveCompressionCodec subclass instance).
    //
    // The engine ships several curve codec subclasses
    // (UAnimCurveCompressionCodec_CompressedRichCurve,
    // UAnimCurveCompressionCodec_UniformIndexable,
    // UAnimCurveCompressionCodec_UniformlySampled). Callers pass one
    // of two shapes: (1) a `/Game/...` UAnimCurveCompressionSettings
    // DataAsset path written into the slot directly, or (2) a codec
    // class which we wrap into a per-sequence settings subobject
    // before assigning. Per-sequence isolation mirrors the bone-side
    // op's "Convert to Custom" pattern.
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        if (!Params->TryGetStringField(TEXT("sequence"), AssetParam)
            && !Params->TryGetStringField(TEXT("path"), AssetParam))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_curve_compression: missing 'asset' parameter"));
        }
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Seq)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_curve_compression: '%s' is not a UAnimSequence (curve compression settings live on UAnimSequence)"), *AssetParam));
    }

    // Capture the previous slot for the diff field on the response.
    UAnimCurveCompressionSettings* PreviousSettings = Seq->CurveCompressionSettings;
    FString PreviousSettingsPath;
    if (PreviousSettings)
    {
        PreviousSettingsPath = PreviousSettings->GetPathName();
    }

    UAnimCurveCompressionSettings* NewSettings = nullptr;
    FString ResolvedCodecClassPath;
    FString ResolvedCodecClassName;
    bool bCodecAuthored = false;

    // Path 1: caller supplied a `/Game/...`
    // UAnimCurveCompressionSettings DataAsset path directly. Resolve
    // through UEditorAssetLibrary and refuse anything that is not the
    // expected class.
    FString SettingsPath;
    if (Params->TryGetStringField(TEXT("compression_settings"), SettingsPath)
        || Params->TryGetStringField(TEXT("curve_compression_settings"), SettingsPath)
        || Params->TryGetStringField(TEXT("settings"), SettingsPath)
        || Params->TryGetStringField(TEXT("settings_path"), SettingsPath))
    {
        if (!SettingsPath.IsEmpty())
        {
            UObject* SettingsAsset = UEditorAssetLibrary::LoadAsset(SettingsPath);
            NewSettings = Cast<UAnimCurveCompressionSettings>(SettingsAsset);
            if (!NewSettings)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_curve_compression: 'compression_settings' path '%s' is not a UAnimCurveCompressionSettings DataAsset"), *SettingsPath));
            }
        }
    }

    // Path 2: caller supplied a codec class name / path. NewObject a
    // per-sequence UAnimCurveCompressionSettings, outered to the
    // sequence so the new subobject saves alongside the sequence
    // package, and assign one fresh codec subobject of the requested
    // class into the Codec slot. Per-sequence settings isolate the
    // choice from any other sequence on the project, mirroring
    // HandleSetCompressionScheme's bone-side "Convert to Custom"
    // shape.
    FString CodecToken;
    if (!NewSettings)
    {
        if (Params->TryGetStringField(TEXT("compression_codec"), CodecToken)
            || Params->TryGetStringField(TEXT("curve_compression_codec"), CodecToken)
            || Params->TryGetStringField(TEXT("curve_compression_scheme"), CodecToken)
            || Params->TryGetStringField(TEXT("compression_scheme"), CodecToken)
            || Params->TryGetStringField(TEXT("compression_class"), CodecToken)
            || Params->TryGetStringField(TEXT("scheme"), CodecToken)
            || Params->TryGetStringField(TEXT("codec"), CodecToken))
        {
            if (CodecToken.IsEmpty())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_curve_compression: 'compression_codec' / 'scheme' must not be empty"));
            }
            UClass* CodecClass = ResolveNotifyClass(CodecToken, UAnimCurveCompressionCodec::StaticClass());
            if (!CodecClass)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_curve_compression: could not resolve UAnimCurveCompressionCodec subclass from '%s' (try UAnimCurveCompressionCodec_CompressedRichCurve / UAnimCurveCompressionCodec_UniformIndexable / UAnimCurveCompressionCodec_UniformlySampled)"), *CodecToken));
            }
            if (CodecClass->HasAnyClassFlags(CLASS_Abstract))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_curve_compression: codec class '%s' is abstract"), *CodecClass->GetName()));
            }

            UAnimCurveCompressionSettings* Authored = NewObject<UAnimCurveCompressionSettings>(
                Seq->GetOutermost(), MakeUniqueObjectName(Seq->GetOutermost(), UAnimCurveCompressionSettings::StaticClass(), TEXT("CurveCompressionSettings_Custom")),
                RF_Public | RF_Standalone | RF_Transactional);
            if (!Authored)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_curve_compression: NewObject<UAnimCurveCompressionSettings> failed"));
            }
            UAnimCurveCompressionCodec* CodecInstance = NewObject<UAnimCurveCompressionCodec>(
                Authored, CodecClass, NAME_None,
                RF_Public | RF_Transactional);
            if (!CodecInstance)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_curve_compression: NewObject<%s> failed"), *CodecClass->GetName()));
            }
            Authored->Codec = CodecInstance;
            NewSettings = Authored;
            ResolvedCodecClassPath = CodecClass->GetPathName();
            ResolvedCodecClassName = CodecClass->GetName();
            bCodecAuthored = true;
        }
    }

    if (!NewSettings)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_curve_compression: pass either 'compression_settings' (DataAsset path) or 'compression_codec' (codec class)"));
    }

    // Modify before the write so any open editor undo records the
    // change, then assign the slot. PostEditChangeProperty fires on
    // the sequence so a Persona-side viewmodel listener picks the
    // swap up.
    Seq->Modify();
    Seq->CurveCompressionSettings = NewSettings;
#if WITH_EDITOR
    if (FProperty* CCSProp = FindFProperty<FProperty>(UAnimSequence::StaticClass(), TEXT("CurveCompressionSettings")))
    {
        FPropertyChangedEvent Event(CCSProp, EPropertyChangeType::ValueSet);
        Seq->PostEditChangeProperty(Event);
    }
#endif

    // Optional sync compression refresh. RequestAnimCompression on
    // UAnimSequence reruns both the bone and curve compression
    // pipelines through the DDC so the saved asset reflects the new
    // codec.
    bool bRequestCompile = false;
    Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);
    if (!bRequestCompile)
    {
        Params->TryGetBoolField(TEXT("recompile"), bRequestCompile);
    }
    if (!bRequestCompile)
    {
        Params->TryGetBoolField(TEXT("request_compression"), bRequestCompile);
    }
    bool bRequestedCompile = false;
#if WITH_EDITOR
    if (bRequestCompile)
    {
        FRequestAnimCompressionParams CompressionParams(Seq);
        Seq->RequestAnimCompression(CompressionParams);
        bRequestedCompile = true;
    }
#endif

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    Seq->MarkPackageDirty();
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Seq->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_curve_compression"));
    Result->SetStringField(TEXT("asset"), AssetParam);
    Result->SetStringField(TEXT("path"), Seq->GetPathName());
    Result->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    Result->SetStringField(TEXT("settings_path"), NewSettings->GetPathName());
    Result->SetStringField(TEXT("settings_class"), NewSettings->GetClass()->GetName());
    Result->SetBoolField(TEXT("settings_authored"), bCodecAuthored);
    if (!ResolvedCodecClassPath.IsEmpty())
    {
        Result->SetStringField(TEXT("codec_class"), ResolvedCodecClassName);
        Result->SetStringField(TEXT("codec_class_path"), ResolvedCodecClassPath);
        if (NewSettings->Codec)
        {
            Result->SetStringField(TEXT("codec_instance_class"), NewSettings->Codec->GetClass()->GetName());
        }
    }
    if (!PreviousSettingsPath.IsEmpty())
    {
        Result->SetStringField(TEXT("previous_settings_path"), PreviousSettingsPath);
    }
    Result->SetBoolField(TEXT("requested_compile"), bRequestedCompile);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

namespace
{
    /** Parse a JSON value that should resolve to a bool. Accepts the
     *  native bool plus the canonical loose tokens callers reach for
     *  when wiring this op through Python. Returns true when the parse
     *  resolved; OutValue is left untouched otherwise. */
    bool LoopFlags_TryReadBool(const TSharedPtr<FJsonValue>& Value, bool& OutValue, FString& OutErr)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Boolean)
        {
            OutValue = Value->AsBool();
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            OutValue = (Value->AsNumber() != 0.0);
            return true;
        }
        if (Value->Type == EJson::String)
        {
            const FString SLower = Value->AsString().ToLower();
            if (SLower == TEXT("true") || SLower == TEXT("1") || SLower == TEXT("on")
                || SLower == TEXT("yes") || SLower == TEXT("enable"))
            {
                OutValue = true;
                return true;
            }
            if (SLower == TEXT("false") || SLower == TEXT("0") || SLower == TEXT("off")
                || SLower == TEXT("no") || SLower == TEXT("disable"))
            {
                OutValue = false;
                return true;
            }
            OutErr = FString::Printf(TEXT("unrecognised bool string '%s'"), *Value->AsString());
            return false;
        }
        OutErr = TEXT("value must be a bool, number, or string");
        return false;
    }

    /** Pull any of a set of aliased field names off the params dict and
     *  parse them as a bool. Returns true if any alias was set; false
     *  if none was provided. On parse failure raises OutErr.
     */
    bool LoopFlags_ReadAliasedBool(const TSharedPtr<FJsonObject>& Params,
        const TArray<FString>& Aliases, bool& OutValue, FString& OutErr)
    {
        for (const FString& Key : Aliases)
        {
            const TSharedPtr<FJsonValue> Val = Params->TryGetField(Key);
            if (Val.IsValid())
            {
                if (!LoopFlags_TryReadBool(Val, OutValue, OutErr))
                {
                    return false;
                }
                return true;
            }
        }
        OutErr.Reset();
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetLoopFlags(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the three loop knobs on UAnimSequence:
    //   - bLoop (required)
    //   - bLoopingInterpolation (optional)
    //   - bEnableRootMotionOnAllowed (optional)
    // All three resolve through reflection so the op stays compatible
    // with the property visibility tightening UE has done since 5.0.
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_loop_flags: missing 'asset'"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Seq)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_loop_flags: '%s' is not a UAnimSequence (the three loop knobs do not live on UAnimSequenceBase shared with UAnimMontage)"), *AssetParam));
    }

    UClass* AssetClass = Seq->GetClass();

    // Required: bLoop. The op refuses if absent so the caller is forced
    // to declare the loop intent rather than picking up whatever the
    // sequence was previously set to.
    bool bNewLoop = false;
    FString LoopErr;
    const bool bLoopProvided = LoopFlags_ReadAliasedBool(Params,
        {
            TEXT("loop"), TEXT("b_loop"), TEXT("bLoop"),
            TEXT("looping"), TEXT("is_looping")
        }, bNewLoop, LoopErr);
    if (!LoopErr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_loop_flags: 'loop' %s"), *LoopErr));
    }
    if (!bLoopProvided)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_loop_flags: missing 'loop' boolean (required)"));
    }

    // Optional: bLoopingInterpolation.
    bool bNewLoopingInterp = false;
    FString LoopInterpErr;
    const bool bLoopingInterpProvided = LoopFlags_ReadAliasedBool(Params,
        {
            TEXT("looping_interpolation"), TEXT("loop_interpolation"),
            TEXT("b_looping_interpolation"), TEXT("bLoopingInterpolation")
        }, bNewLoopingInterp, LoopInterpErr);
    if (!LoopInterpErr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_loop_flags: 'looping_interpolation' %s"), *LoopInterpErr));
    }

    // Optional: bEnableRootMotionOnAllowed.
    bool bNewRMAllowed = false;
    FString RMAllowedErr;
    const bool bRMAllowedProvided = LoopFlags_ReadAliasedBool(Params,
        {
            TEXT("enable_root_motion_on_allowed"),
            TEXT("b_enable_root_motion_on_allowed"),
            TEXT("bEnableRootMotionOnAllowed"),
            TEXT("root_motion_on_allowed")
        }, bNewRMAllowed, RMAllowedErr);
    if (!RMAllowedErr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_loop_flags: 'enable_root_motion_on_allowed' %s"), *RMAllowedErr));
    }

    auto ApplyBoolField = [&](const FName& FieldName, bool bNewValue, bool& bOutPrev, bool& bOutWrote) -> FString
    {
        bOutWrote = false;
        FBoolProperty* BoolProp = CastField<FBoolProperty>(AssetClass->FindPropertyByName(FieldName));
        if (!BoolProp)
        {
            return FString::Printf(TEXT("UAnimSequence has no FBoolProperty named '%s'"), *FieldName.ToString());
        }
        void* Container = static_cast<void*>(Seq);
        bOutPrev = BoolProp->GetPropertyValue_InContainer(Container);
        BoolProp->SetPropertyValue_InContainer(Container, bNewValue);
        bOutWrote = true;
        return FString();
    };

    bool bPrevLoop = false;
    bool bWroteLoop = false;
    {
        const FString Err = ApplyBoolField(TEXT("bLoop"), bNewLoop, bPrevLoop, bWroteLoop);
        if (!Err.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_loop_flags: %s"), *Err));
        }
    }

    bool bPrevLoopingInterp = false;
    bool bWroteLoopingInterp = false;
    if (bLoopingInterpProvided)
    {
        const FString Err = ApplyBoolField(TEXT("bLoopingInterpolation"),
            bNewLoopingInterp, bPrevLoopingInterp, bWroteLoopingInterp);
        if (!Err.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_loop_flags: %s"), *Err));
        }
    }

    bool bPrevRMAllowed = false;
    bool bWroteRMAllowed = false;
    if (bRMAllowedProvided)
    {
        const FString Err = ApplyBoolField(TEXT("bEnableRootMotionOnAllowed"),
            bNewRMAllowed, bPrevRMAllowed, bWroteRMAllowed);
        if (!Err.IsEmpty())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_loop_flags: %s"), *Err));
        }
    }

#if WITH_EDITOR
    Seq->PostEditChange();
#endif
    Seq->MarkPackageDirty();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Seq->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_loop_flags"));
    Result->SetStringField(TEXT("asset"), AssetParam);
    Result->SetStringField(TEXT("path"), Seq->GetPathName());
    Result->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    Result->SetBoolField(TEXT("loop"), bNewLoop);
    Result->SetBoolField(TEXT("previous_loop"), bPrevLoop);
    Result->SetBoolField(TEXT("loop_written"), bWroteLoop);
    Result->SetBoolField(TEXT("looping_interpolation"), bWroteLoopingInterp ? bNewLoopingInterp : bPrevLoopingInterp);
    Result->SetBoolField(TEXT("previous_looping_interpolation"), bPrevLoopingInterp);
    Result->SetBoolField(TEXT("looping_interpolation_provided"), bLoopingInterpProvided);
    Result->SetBoolField(TEXT("enable_root_motion_on_allowed"), bWroteRMAllowed ? bNewRMAllowed : bPrevRMAllowed);
    Result->SetBoolField(TEXT("previous_enable_root_motion_on_allowed"), bPrevRMAllowed);
    Result->SetBoolField(TEXT("enable_root_motion_on_allowed_provided"), bRMAllowedProvided);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

namespace
{
    /** Try to read an optional float from a set of aliased keys. Returns
     *  true on success; writes a diagnostic into OutErr only when the
     *  caller supplied the key but the value was not a number. */
    bool BlendTimes_ReadAliasedFloat(const TSharedPtr<FJsonObject>& Params,
        const TArray<FString>& Aliases, double& OutValue, FString& OutErr)
    {
        OutErr.Reset();
        for (const FString& Key : Aliases)
        {
            const TSharedPtr<FJsonValue> Val = Params->TryGetField(Key);
            if (!Val.IsValid())
            {
                continue;
            }
            if (Val->Type == EJson::Number)
            {
                OutValue = Val->AsNumber();
                return true;
            }
            if (Val->Type == EJson::String)
            {
                const FString S = Val->AsString().TrimStartAndEnd();
                if (!S.IsNumeric())
                {
                    OutErr = FString::Printf(TEXT("value for '%s' must be a number, got string '%s'"), *Key, *S);
                    return false;
                }
                OutValue = FCString::Atod(*S);
                return true;
            }
            OutErr = FString::Printf(TEXT("value for '%s' must be a number"), *Key);
            return false;
        }
        return false;
    }

    /** Resolve the inner `BlendTime` float UPROPERTY on an FAlphaBlend (or
     *  FAlphaBlendArgs) struct that lives at the named outer UPROPERTY on
     *  a UAnimMontage. The engine has shipped both FAlphaBlend (legacy)
     *  and FAlphaBlendArgs (modern) as the struct type for the
     *  Montage->BlendIn / Montage->BlendOut slots since 5.0; we walk the
     *  outer FStructProperty's Struct to find the inner BlendTime, so
     *  the op stays compatible with either shape without us spelling out
     *  the engine version. Returns the address of the inner float in
     *  OutFloatAddr. */
    bool BlendTimes_ResolveMontageInnerFloat(UAnimMontage* Montage, const FName& OuterFieldName,
        FFloatProperty*& OutFloatProp, void*& OutFloatAddr, FString& OutErr)
    {
        OutFloatProp = nullptr;
        OutFloatAddr = nullptr;

        if (!Montage)
        {
            OutErr = TEXT("montage is null");
            return false;
        }

        FProperty* OuterProp = Montage->GetClass()->FindPropertyByName(OuterFieldName);
        if (!OuterProp)
        {
            OutErr = FString::Printf(TEXT("UAnimMontage has no UPROPERTY named '%s'"), *OuterFieldName.ToString());
            return false;
        }
        FStructProperty* OuterStructProp = CastField<FStructProperty>(OuterProp);
        if (!OuterStructProp)
        {
            OutErr = FString::Printf(TEXT("UAnimMontage::%s is not an FStructProperty"), *OuterFieldName.ToString());
            return false;
        }

        FProperty* InnerProp = OuterStructProp->Struct->FindPropertyByName(TEXT("BlendTime"));
        if (!InnerProp)
        {
            OutErr = FString::Printf(TEXT("struct '%s' on UAnimMontage::%s has no inner 'BlendTime' field"),
                *OuterStructProp->Struct->GetName(), *OuterFieldName.ToString());
            return false;
        }
        FFloatProperty* FloatProp = CastField<FFloatProperty>(InnerProp);
        if (!FloatProp)
        {
            OutErr = FString::Printf(TEXT("inner 'BlendTime' on struct '%s' is not a float UPROPERTY"),
                *OuterStructProp->Struct->GetName());
            return false;
        }
        // Get the address of the outer struct's value, then offset to the
        // inner BlendTime float. ContainerPtrToValuePtr lands on the
        // FAlphaBlend(Args) struct body; InnerProp's ContainerPtrToValuePtr
        // resolves the float address inside that struct body.
        void* OuterStructAddr = OuterStructProp->ContainerPtrToValuePtr<void>(Montage);
        OutFloatAddr = FloatProp->ContainerPtrToValuePtr<void>(OuterStructAddr);
        OutFloatProp = FloatProp;
        return true;
    }
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleSetBlendTimes(const TSharedPtr<FJsonObject>& Params)
{
    // Writes the blend-time knobs the AnimGraph reads when a montage
    // crossfades in / out, plus the optional bEnableRootMotionTranslation
    // toggle on UAnimSequence so callers can pair a montage tuning pass
    // with the per-sequence root-motion translation gate in a single
    // round trip.
    //
    // The BlendIn / BlendOut slots on UAnimMontage are FAlphaBlend(Args)
    // structs whose inner `BlendTime` float is the seconds-long crossfade
    // duration. The engine has shipped FAlphaBlend in the past and
    // FAlphaBlendArgs more recently; we resolve through reflection on
    // the outer FStructProperty's Struct so the op stays compatible with
    // either struct shape without us hard-coding it.
    //
    // For non-Montage UAnimSequence inputs we still expose the
    // bEnableRootMotionTranslation toggle (the field lives on
    // UAnimSequence itself, not on UAnimMontage), so a caller flipping a
    // looping sequence's root-motion translation gate does not need to
    // open Persona.
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_blend_times: missing 'asset'"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_blend_times: failed to load asset '%s'"), *AssetParam));
    }
    UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
    UAnimSequence* Seq = Cast<UAnimSequence>(Asset);
    if (!Montage && !Seq)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_blend_times: asset '%s' is not a UAnimMontage or UAnimSequence"), *AssetParam));
    }

    // Optional: blend_in_time (seconds). Only applicable to montages
    // since UAnimSequence does not carry FAlphaBlend slots.
    double NewBlendInSeconds = 0.0;
    FString BlendInErr;
    const bool bBlendInProvided = BlendTimes_ReadAliasedFloat(Params,
        {
            TEXT("blend_in_time"), TEXT("blend_in"), TEXT("BlendInTime"),
            TEXT("blend_in_seconds")
        }, NewBlendInSeconds, BlendInErr);
    if (!BlendInErr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_blend_times: %s"), *BlendInErr));
    }

    // Optional: blend_out_time (seconds). Same Montage caveat.
    double NewBlendOutSeconds = 0.0;
    FString BlendOutErr;
    const bool bBlendOutProvided = BlendTimes_ReadAliasedFloat(Params,
        {
            TEXT("blend_out_time"), TEXT("blend_out"), TEXT("BlendOutTime"),
            TEXT("blend_out_seconds")
        }, NewBlendOutSeconds, BlendOutErr);
    if (!BlendOutErr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_blend_times: %s"), *BlendOutErr));
    }

    // Optional: bEnableRootMotionTranslation toggle. The field lives on
    // UAnimSequence, so we route through reflection only when the asset
    // is a UAnimSequence (UAnimMontage inherits from UAnimCompositeBase
    // -> UAnimSequenceBase, not UAnimSequence).
    bool bNewRMTranslation = false;
    FString RMTransErr;
    const bool bRMTransProvided = LoopFlags_ReadAliasedBool(Params,
        {
            TEXT("enable_root_motion_translation"),
            TEXT("b_enable_root_motion_translation"),
            TEXT("bEnableRootMotionTranslation"),
            TEXT("root_motion_translation")
        }, bNewRMTranslation, RMTransErr);
    if (!RMTransErr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_blend_times: 'enable_root_motion_translation' %s"), *RMTransErr));
    }

    if (!bBlendInProvided && !bBlendOutProvided && !bRMTransProvided)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_blend_times: pass at least one of 'blend_in_time' / 'blend_out_time' / 'enable_root_motion_translation'"));
    }

    // Reject blend_in_time / blend_out_time on non-Montage assets up front
    // so the caller gets a clear error rather than a silent skip.
    if ((bBlendInProvided || bBlendOutProvided) && !Montage)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_blend_times: 'blend_in_time' / 'blend_out_time' require a UAnimMontage; '%s' is %s"),
                *AssetParam, *Asset->GetClass()->GetName()));
    }

    // Snapshot previous values for the diff payload.
    double PrevBlendInSeconds = 0.0;
    double PrevBlendOutSeconds = 0.0;
    bool bWroteBlendIn = false;
    bool bWroteBlendOut = false;
    if (Montage)
    {
        // BlendIn
        if (bBlendInProvided)
        {
            FFloatProperty* FloatProp = nullptr;
            void* FloatAddr = nullptr;
            FString Err;
            if (!BlendTimes_ResolveMontageInnerFloat(Montage, TEXT("BlendIn"), FloatProp, FloatAddr, Err))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_blend_times: %s"), *Err));
            }
            PrevBlendInSeconds = FloatProp->GetPropertyValue(FloatAddr);
            FloatProp->SetPropertyValue(FloatAddr, static_cast<float>(NewBlendInSeconds));
            bWroteBlendIn = true;
        }
        else
        {
            // Read the current value for the response, no write.
            FFloatProperty* FloatProp = nullptr;
            void* FloatAddr = nullptr;
            FString Err;
            if (BlendTimes_ResolveMontageInnerFloat(Montage, TEXT("BlendIn"), FloatProp, FloatAddr, Err))
            {
                PrevBlendInSeconds = FloatProp->GetPropertyValue(FloatAddr);
            }
        }

        // BlendOut
        if (bBlendOutProvided)
        {
            FFloatProperty* FloatProp = nullptr;
            void* FloatAddr = nullptr;
            FString Err;
            if (!BlendTimes_ResolveMontageInnerFloat(Montage, TEXT("BlendOut"), FloatProp, FloatAddr, Err))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_blend_times: %s"), *Err));
            }
            PrevBlendOutSeconds = FloatProp->GetPropertyValue(FloatAddr);
            FloatProp->SetPropertyValue(FloatAddr, static_cast<float>(NewBlendOutSeconds));
            bWroteBlendOut = true;
        }
        else
        {
            FFloatProperty* FloatProp = nullptr;
            void* FloatAddr = nullptr;
            FString Err;
            if (BlendTimes_ResolveMontageInnerFloat(Montage, TEXT("BlendOut"), FloatProp, FloatAddr, Err))
            {
                PrevBlendOutSeconds = FloatProp->GetPropertyValue(FloatAddr);
            }
        }
    }

    // bEnableRootMotionTranslation on UAnimSequence (Montages do not
    // expose this field; the engine only reads it on UAnimSequence).
    bool bPrevRMTranslation = false;
    bool bWroteRMTranslation = false;
    if (bRMTransProvided)
    {
        if (!Seq)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_blend_times: 'enable_root_motion_translation' requires a UAnimSequence; '%s' is %s"),
                    *AssetParam, *Asset->GetClass()->GetName()));
        }
        FBoolProperty* BoolProp = CastField<FBoolProperty>(
            Seq->GetClass()->FindPropertyByName(TEXT("bEnableRootMotionTranslation")));
        if (!BoolProp)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("set_blend_times: UAnimSequence does not expose 'bEnableRootMotionTranslation' on the reflection database"));
        }
        bPrevRMTranslation = BoolProp->GetPropertyValue_InContainer(Seq);
        BoolProp->SetPropertyValue_InContainer(Seq, bNewRMTranslation);
        bWroteRMTranslation = true;
    }

#if WITH_EDITOR
    Asset->PostEditChange();
#endif
    Asset->MarkPackageDirty();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_blend_times"));
    Result->SetStringField(TEXT("asset"), AssetParam);
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());

    Result->SetBoolField(TEXT("blend_in_provided"), bBlendInProvided);
    Result->SetBoolField(TEXT("blend_in_written"), bWroteBlendIn);
    Result->SetNumberField(TEXT("blend_in_time"), bWroteBlendIn ? NewBlendInSeconds : PrevBlendInSeconds);
    Result->SetNumberField(TEXT("previous_blend_in_time"), PrevBlendInSeconds);

    Result->SetBoolField(TEXT("blend_out_provided"), bBlendOutProvided);
    Result->SetBoolField(TEXT("blend_out_written"), bWroteBlendOut);
    Result->SetNumberField(TEXT("blend_out_time"), bWroteBlendOut ? NewBlendOutSeconds : PrevBlendOutSeconds);
    Result->SetNumberField(TEXT("previous_blend_out_time"), PrevBlendOutSeconds);

    Result->SetBoolField(TEXT("enable_root_motion_translation_provided"), bRMTransProvided);
    Result->SetBoolField(TEXT("enable_root_motion_translation_written"), bWroteRMTranslation);
    Result->SetBoolField(TEXT("enable_root_motion_translation"), bWroteRMTranslation ? bNewRMTranslation : bPrevRMTranslation);
    Result->SetBoolField(TEXT("previous_enable_root_motion_translation"), bPrevRMTranslation);

    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftAnimationEditCommands::HandleAddMontageSection(const TSharedPtr<FJsonObject>& Params)
{
    // Wraps UAnimMontage::AddAnimCompositeSection. The engine surfaces
    // this as the canonical way to land a new FCompositeSection on a
    // montage's CompositeSections array (the inner section list the
    // AnimGraph reads when it jumps a montage between named labels).
    // Useful for scripting montage section authoring without opening
    // the montage editor; mirrors the editor's right-click "+ Add
    // Section" action.
    //
    // `section_name` is the FName label for the new section.
    // `start_frame` (alias `start_time` seconds) places the new
    // section on the timeline; when omitted we default to the end of
    // the last existing section (or 0 when none exist) so the new
    // section appends after everything that came before. Optional
    // `is_loop=true` self-links the new section's NextSectionName so
    // playback loops on this section.
    FString AssetParam;
    if (!Params->TryGetStringField(TEXT("asset"), AssetParam) || AssetParam.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_montage_section: missing 'asset' (path to a UAnimMontage)"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetParam);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_montage_section: failed to load asset '%s'"), *AssetParam));
    }
    UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
    if (!Montage)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_montage_section: asset '%s' is not a UAnimMontage"), *AssetParam));
    }

    FString SectionNameStr;
    if (!Params->TryGetStringField(TEXT("section_name"), SectionNameStr)
        && !Params->TryGetStringField(TEXT("name"), SectionNameStr)
        && !Params->TryGetStringField(TEXT("section"), SectionNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_montage_section: missing 'section_name' (FName label for the new composite section)"));
    }
    SectionNameStr.TrimStartAndEndInline();
    if (SectionNameStr.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("add_montage_section: 'section_name' must be non-empty"));
    }
    const FName NewSectionName(*SectionNameStr);

    // Reject duplicate names up front so the caller gets a clear error
    // rather than the engine's INDEX_NONE return. The same check runs
    // inside AddAnimCompositeSection but we surface it here for the
    // structured response shape.
    if (Montage->GetSectionIndex(NewSectionName) != INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_montage_section: section '%s' already exists on montage '%s'"),
                *SectionNameStr, *AssetParam));
    }

    // Resolve the start position. `start_frame` (int) wins over
    // `start_time` (float seconds); both fall back to the end of the
    // last existing section (or 0 when the montage has none yet).
    double FrameValue = 0.0;
    const bool bHasFrame = Params->TryGetNumberField(TEXT("start_frame"), FrameValue)
        || Params->TryGetNumberField(TEXT("frame"), FrameValue);
    double TimeValue = 0.0;
    const bool bHasTime = Params->TryGetNumberField(TEXT("start_time"), TimeValue)
        || Params->TryGetNumberField(TEXT("time"), TimeValue);

    float StartPos = 0.0f;
    bool bUsedDefault = false;
    FString StartSource;
    if (bHasFrame)
    {
        // UAnimMontage does not expose GetSamplingFrameRate(), so we use
        // the 30 fps default the other animation_edit ops fall back to
        // when the asset itself does not carry a per-asset frame rate.
        // The caller can pass `start_time` directly for full precision.
        const FFrameRate Rate(30, 1);
        const FFrameTime FrameTime(FFrameNumber(static_cast<int32>(FrameValue)));
        StartPos = static_cast<float>(Rate.AsSeconds(FrameTime));
        StartSource = TEXT("start_frame");
    }
    else if (bHasTime)
    {
        StartPos = static_cast<float>(TimeValue);
        StartSource = TEXT("start_time");
    }
    else
    {
        // Default: end of last existing section (or 0 when none exist).
        // The engine sorts CompositeSections by GetTime() on save so the
        // last entry in the array is the latest in time; we walk the
        // array to find the max so we stay correct even before the sort
        // step has run.
        bUsedDefault = true;
        float MaxTime = 0.0f;
        for (const FCompositeSection& Sect : Montage->CompositeSections)
        {
            const float T = Sect.GetTime(EAnimLinkMethod::Absolute);
            if (T > MaxTime)
            {
                MaxTime = T;
            }
        }
        StartPos = MaxTime;
        StartSource = TEXT("default_end_of_last_section");
    }
    if (StartPos < 0.0f)
    {
        StartPos = 0.0f;
    }
    // Clamp to the montage's play length so an off-the-end section
    // does not lose its anchor when the engine resolves the linkable
    // element back into a time. The engine itself does not clamp
    // inside AddAnimCompositeSection so we do it here for safety.
    const float PlayLength = Montage->GetPlayLength();
    if (PlayLength > 0.0f && StartPos > PlayLength)
    {
        StartPos = PlayLength;
    }

    // Optional `is_loop`: a self-link on NextSectionName makes the
    // section loop. The engine resolves the next-section chain at
    // playback so writing it on the new section is enough.
    bool bIsLoop = false;
    Params->TryGetBoolField(TEXT("is_loop"), bIsLoop)
        || Params->TryGetBoolField(TEXT("loop"), bIsLoop)
        || Params->TryGetBoolField(TEXT("looping"), bIsLoop);

    // Capture the previous section count for the diff payload.
    const int32 PreviousCount = Montage->CompositeSections.Num();

#if WITH_EDITOR
    const int32 NewIndex = Montage->AddAnimCompositeSection(NewSectionName, StartPos);
#else
    const int32 NewIndex = INDEX_NONE;
#endif
    if (NewIndex == INDEX_NONE)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_montage_section: AddAnimCompositeSection returned INDEX_NONE for section '%s' on '%s' (duplicate name, or build does not have WITH_EDITOR)"),
                *SectionNameStr, *AssetParam));
    }

    // Self-link the NextSectionName for the loop case. We resolve the
    // index again since AddAnimCompositeSection returns the position
    // before the engine's sort pass runs (the engine's
    // SortAnimCompositeSectionByPos call sits on the editor's "save
    // montage" path).
    if (bIsLoop)
    {
        if (Montage->CompositeSections.IsValidIndex(NewIndex))
        {
            Montage->CompositeSections[NewIndex].NextSectionName = NewSectionName;
        }
    }

#if WITH_EDITOR
    Montage->PostEditChange();
#endif
    Montage->MarkPackageDirty();

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Montage->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_montage_section"));
    Result->SetStringField(TEXT("asset"), AssetParam);
    Result->SetStringField(TEXT("path"), Montage->GetPathName());
    Result->SetStringField(TEXT("section_name"), SectionNameStr);
    Result->SetNumberField(TEXT("section_index"), NewIndex);
    Result->SetNumberField(TEXT("start_time"), StartPos);
    Result->SetStringField(TEXT("start_source"), StartSource);
    Result->SetBoolField(TEXT("used_default_start"), bUsedDefault);
    Result->SetBoolField(TEXT("is_loop"), bIsLoop);
    Result->SetNumberField(TEXT("previous_section_count"), PreviousCount);
    Result->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
    Result->SetNumberField(TEXT("play_length"), PlayLength);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}
