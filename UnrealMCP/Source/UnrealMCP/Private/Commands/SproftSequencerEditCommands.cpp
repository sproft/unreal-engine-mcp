#include "Commands/SproftSequencerEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "UObject/Package.h"

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

    /** Split a `/Game/Subdir/AssetName` path into directory + asset
     *  name. Mirrors the helper in `SproftAssetFactoryCommands` so the
     *  edit slice does not fight the asset-creation flow. */
    void SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
    {
        FString Trim = InPath;
        Trim.TrimEndInline();
        Trim.RemoveFromEnd(TEXT("/"));

        int32 LastSlash = INDEX_NONE;
        if (Trim.FindLastChar('/', LastSlash))
        {
            OutPackageDir = Trim.Left(LastSlash + 1);
            OutAssetName = Trim.Mid(LastSlash + 1);
        }
        else
        {
            OutPackageDir = TEXT("/Game/");
            OutAssetName = Trim;
        }

        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Mirrors the actor lookup `actor_inspect` / `scene_compose` use:
     *  GetName() first, GetActorLabel() second. Sequencer always
     *  binds against the editor world, never PIE; we do the same. */
    AActor* ResolveActorByName(UWorld* World, const FString& Target)
    {
        if (!World || Target.IsEmpty())
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor && Actor->GetName() == Target)
            {
                return Actor;
            }
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor && Actor->GetActorLabel() == Target)
            {
                return Actor;
            }
        }
        return nullptr;
    }
}

FSproftSequencerEditCommands::FSproftSequencerEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("sequencer_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown sequencer_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }
    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("inspect"))
    {
        return HandleSequencerInspect(Params);
    }
    if (Op == TEXT("create_level_sequence"))
    {
        return HandleCreateLevelSequence(Params);
    }
    if (Op == TEXT("add_possessable"))
    {
        return HandleAddPossessable(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("sequencer_edit: unsupported op '%s'"), *Op));
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

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleCreateLevelSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("sequence"), PackagePath)
        && !Params->TryGetStringField(TEXT("path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sequence' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Sequence path '%s' must start with /Game/"), *PackagePath));
    }

    FString PackageDir;
    FString AssetName;
    SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }
    const FString AssetObjectPath = PackageDir + AssetName;

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"),
                *AssetObjectPath));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    ULevelSequence* Sequence = NewObject<ULevelSequence>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!Sequence)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create ULevelSequence"));
    }
    // Initialize lays down a fresh UMovieScene with the project's
    // default tick / display rates and clock source. Without this the
    // sequence opens but every Sequencer panel call hits a null
    // MovieScene path.
    Sequence->Initialize();

    FAssetRegistryModule::AssetCreated(Sequence);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_level_sequence"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), Sequence->GetClass()->GetName());
    if (UMovieScene* MovieScene = Sequence->GetMovieScene())
    {
        Result->SetObjectField(TEXT("tick_resolution"),
            FrameRateRecord(MovieScene->GetTickResolution()));
        Result->SetObjectField(TEXT("display_rate"),
            FrameRateRecord(MovieScene->GetDisplayRate()));
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddPossessable(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sequence' parameter"));
    }

    FString ActorTarget;
    if (!Params->TryGetStringField(TEXT("actor"), ActorTarget)
        && !Params->TryGetStringField(TEXT("actor_name"), ActorTarget)
        && !Params->TryGetStringField(TEXT("target"), ActorTarget))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor' parameter"));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(SequencePath);
    ULevelSequence* Sequence = Cast<ULevelSequence>(Asset);
    if (!Sequence)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a ULevelSequence"), *SequencePath));
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Sequence '%s' has no MovieScene"), *SequencePath));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }
    AActor* Actor = ResolveActorByName(World, ActorTarget);
    if (!Actor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No actor with name or label '%s' in editor world"), *ActorTarget));
    }

    FString BindingName;
    if (!Params->TryGetStringField(TEXT("binding_name"), BindingName) || BindingName.IsEmpty())
    {
        BindingName = Actor->GetActorLabel();
        if (BindingName.IsEmpty())
        {
            BindingName = Actor->GetName();
        }
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(BindingName, Actor->GetClass());
    if (!BindingGuid.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMovieScene::AddPossessable failed for actor '%s'"), *ActorTarget));
    }

    // BindPossessableObject populates the LevelSequenceBindingReferences
    // table that Sequencer's runtime uses to map a GUID back to an
    // editor-world actor. Without it, the possessable is a name + class
    // entry on the MovieScene with no live binding.
    Sequence->BindPossessableObject(BindingGuid, *Actor, World);

    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_possessable"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("guid"), BindingGuid.ToString());
    Result->SetStringField(TEXT("binding_name"), BindingName);
    Result->SetStringField(TEXT("actor"), Actor->GetName());
    Result->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    Result->SetStringField(TEXT("actor_class_path"), Actor->GetClass()->GetPathName());
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
