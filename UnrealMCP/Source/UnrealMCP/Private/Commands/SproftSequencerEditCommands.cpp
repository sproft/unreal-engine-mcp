#include "Commands/SproftSequencerEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"

namespace
{
    /** Render a TRange<FFrameNumber> as a {start, end, duration} triple.
     *  Unbounded sides come through as `has_start_frame: false` /
     *  `has_end_frame: false`. The duration helper always lands so the
     *  caller does not need to redo the math. */
    void WriteFrameRange(TSharedPtr<FJsonObject> Out, const TRange<FFrameNumber>& Range,
                         const TCHAR* StartField, const TCHAR* EndField,
                         const TCHAR* DurationField,
                         const TCHAR* HasStartField, const TCHAR* HasEndField)
    {
        const bool bHasStart = Range.GetLowerBound().IsClosed();
        const bool bHasEnd   = Range.GetUpperBound().IsClosed();

        Out->SetBoolField(HasStartField, bHasStart);
        Out->SetBoolField(HasEndField,   bHasEnd);

        if (bHasStart)
        {
            Out->SetNumberField(StartField, Range.GetLowerBoundValue().Value);
        }
        if (bHasEnd)
        {
            Out->SetNumberField(EndField, Range.GetUpperBoundValue().Value);
        }
        if (bHasStart && bHasEnd)
        {
            Out->SetNumberField(DurationField,
                Range.GetUpperBoundValue().Value - Range.GetLowerBoundValue().Value);
        }
        else
        {
            Out->SetNumberField(DurationField, 0);
        }
    }

    /** Render a UMovieSceneSection as `{class, class_path, range}`. */
    TSharedPtr<FJsonObject> SectionRecord(const UMovieSceneSection* Section)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Section)
        {
            return Out;
        }
        Out->SetStringField(TEXT("class"), Section->GetClass()->GetName());
        Out->SetStringField(TEXT("class_path"), Section->GetClass()->GetPathName());

        const TRange<FFrameNumber> Range = Section->GetRange();
        WriteFrameRange(Out, Range,
            TEXT("inclusive_start_frame"),
            TEXT("exclusive_end_frame"),
            TEXT("duration_frames"),
            TEXT("has_start_frame"),
            TEXT("has_end_frame"));

        return Out;
    }

    /** Render a UMovieSceneTrack as a header dict + optional sections array. */
    TSharedPtr<FJsonObject> TrackRecord(UMovieSceneTrack* Track,
                                        bool bIncludeSections,
                                        int32 MaxSectionsPerTrack)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Track)
        {
            return Out;
        }
        Out->SetStringField(TEXT("name"), Track->GetFName().ToString());
        Out->SetStringField(TEXT("class"), Track->GetClass()->GetName());
        Out->SetStringField(TEXT("class_path"), Track->GetClass()->GetPathName());

#if WITH_EDITORONLY_DATA
        Out->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
#endif

        const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
        Out->SetNumberField(TEXT("section_count"), Sections.Num());

        if (bIncludeSections)
        {
            const int32 Cap = FMath::Max(0, MaxSectionsPerTrack);
            const int32 Emit = (Cap > 0) ? FMath::Min(Sections.Num(), Cap) : Sections.Num();
            const bool bTruncated = (Cap > 0) && (Sections.Num() > Cap);

            TArray<TSharedPtr<FJsonValue>> SectionArr;
            for (int32 I = 0; I < Emit; ++I)
            {
                SectionArr.Add(MakeShared<FJsonValueObject>(SectionRecord(Sections[I])));
            }
            Out->SetArrayField(TEXT("sections"), SectionArr);
            if (bTruncated)
            {
                Out->SetBoolField(TEXT("sections_truncated"), true);
            }
        }
        return Out;
    }

    /** Render a frame-rate as numerator / denominator with a helpful float
     *  approximation; UI surfaces prefer the float, schedulers prefer the
     *  exact ratio. */
    TSharedPtr<FJsonObject> FrameRateRecord(const FFrameRate& Rate)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("numerator"),   Rate.Numerator);
        Out->SetNumberField(TEXT("denominator"), Rate.Denominator);
        if (Rate.Denominator != 0)
        {
            Out->SetNumberField(TEXT("approx_fps"),
                static_cast<double>(Rate.Numerator) / static_cast<double>(Rate.Denominator));
        }
        return Out;
    }
}

FSproftSequencerEditCommands::FSproftSequencerEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("sequencer_edit"))
    {
        return HandleSequencerInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown sequencer_edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleSequencerInspect(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath)
        && !Params->TryGetStringField(TEXT("asset"), SequencePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sequence' parameter"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(SequencePath);
    UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(Asset);
    if (!Sequence)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UMovieSceneSequence"), *SequencePath));
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Sequence '%s' has no MovieScene"), *SequencePath));
    }

    const FString Op = Params->HasField(TEXT("op"))
        ? Params->GetStringField(TEXT("op"))
        : FString(TEXT("inspect"));
    if (Op != TEXT("inspect"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("sequencer_edit: only the 'inspect' op is supported in this slice; got '%s'"), *Op));
    }

    bool bIncludeTracks = true;
    bool bIncludeCameraCutTrack = true;
    bool bIncludeSections = true;
    bool bIncludePossessables = true;
    bool bIncludeSpawnables = true;
    int32 MaxSectionsPerTrack = 64;

    Params->TryGetBoolField(TEXT("include_tracks"), bIncludeTracks);
    Params->TryGetBoolField(TEXT("include_camera_cut_track"), bIncludeCameraCutTrack);
    Params->TryGetBoolField(TEXT("include_sections"), bIncludeSections);
    Params->TryGetBoolField(TEXT("include_possessables"), bIncludePossessables);
    Params->TryGetBoolField(TEXT("include_spawnables"), bIncludeSpawnables);
    if (Params->HasField(TEXT("max_sections_per_track")))
    {
        const int32 Parsed =
            static_cast<int32>(Params->GetNumberField(TEXT("max_sections_per_track")));
        if (Parsed > 0)
        {
            MaxSectionsPerTrack = Parsed;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("name"), Sequence->GetName());
    Result->SetStringField(TEXT("path"), Sequence->GetPathName());
    Result->SetStringField(TEXT("class"), Sequence->GetClass()->GetName());
    Result->SetStringField(TEXT("class_path"), Sequence->GetClass()->GetPathName());

    Result->SetObjectField(TEXT("tick_resolution"),
        FrameRateRecord(MovieScene->GetTickResolution()));
    Result->SetObjectField(TEXT("display_rate"),
        FrameRateRecord(MovieScene->GetDisplayRate()));

    {
        // Playback range is a bounded TRange in tick-resolution frame numbers.
        // We emit start / end / duration rather than the raw lower / upper
        // bounds so callers do not have to think about the bound-kind.
        const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
        WriteFrameRange(Result, Range,
            TEXT("playback_start_frame"),
            TEXT("playback_end_frame"),
            TEXT("playback_duration_frames"),
            TEXT("playback_has_start"),
            TEXT("playback_has_end"));
    }

    if (bIncludeTracks)
    {
        const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetTracks();
        TArray<TSharedPtr<FJsonValue>> TrackArr;
        for (UMovieSceneTrack* Track : Tracks)
        {
            TrackArr.Add(MakeShared<FJsonValueObject>(
                TrackRecord(Track, bIncludeSections, MaxSectionsPerTrack)));
        }
        Result->SetArrayField(TEXT("tracks"), TrackArr);
        Result->SetNumberField(TEXT("track_count"), TrackArr.Num());
    }

    if (bIncludeCameraCutTrack)
    {
        if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
        {
            Result->SetObjectField(TEXT("camera_cut_track"),
                TrackRecord(CameraCutTrack, bIncludeSections, MaxSectionsPerTrack));
        }
    }

    if (bIncludePossessables)
    {
        const int32 Count = MovieScene->GetPossessableCount();
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (int32 I = 0; I < Count; ++I)
        {
            const FMovieScenePossessable& Poss = MovieScene->GetPossessable(I);
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("guid"), Poss.GetGuid().ToString());
            Entry->SetStringField(TEXT("name"), Poss.GetName());
            if (const UClass* PossessedClass = Poss.GetPossessedObjectClass())
            {
                Entry->SetStringField(TEXT("class"), PossessedClass->GetName());
                Entry->SetStringField(TEXT("class_path"), PossessedClass->GetPathName());
            }
            const FGuid Parent = Poss.GetParent();
            if (Parent.IsValid())
            {
                Entry->SetStringField(TEXT("parent_guid"), Parent.ToString());
            }
            Arr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("possessables"), Arr);
        Result->SetNumberField(TEXT("possessable_count"), Arr.Num());
    }

    if (bIncludeSpawnables)
    {
        const int32 Count = MovieScene->GetSpawnableCount();
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (int32 I = 0; I < Count; ++I)
        {
            FMovieSceneSpawnable& Spawn = MovieScene->GetSpawnable(I);
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("guid"), Spawn.GetGuid().ToString());
            Entry->SetStringField(TEXT("name"), Spawn.GetName());
            if (const UObject* Template = Spawn.GetObjectTemplate())
            {
                if (const UClass* TemplateClass = Template->GetClass())
                {
                    Entry->SetStringField(TEXT("template_class"), TemplateClass->GetName());
                    Entry->SetStringField(TEXT("template_class_path"),
                        TemplateClass->GetPathName());
                }
            }
            Arr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("spawnables"), Arr);
        Result->SetNumberField(TEXT("spawnable_count"), Arr.Num());
    }

    return Result;
}
