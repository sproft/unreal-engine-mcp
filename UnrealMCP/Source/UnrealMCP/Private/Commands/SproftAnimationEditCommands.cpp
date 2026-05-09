#include "Commands/SproftAnimationEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "AnimationBlueprintLibrary.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

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
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported animation_edit op '%s'; expected one of 'set_rate_scale', 'set_additive', 'add_notify'"), *Op));
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
