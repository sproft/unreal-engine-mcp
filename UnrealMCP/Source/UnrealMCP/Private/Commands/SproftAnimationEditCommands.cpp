#include "Commands/SproftAnimationEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
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
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported animation_edit op '%s'; expected one of 'set_rate_scale', 'set_additive', 'add_notify', 'add_curve', 'add_metadata_curve', 'add_sync_marker', 'add_blendspace_sample', 'replace_blendspace_sample', 'delete_blendspace_sample'"), *Op));
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
