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
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sound/SoundBase.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "UObject/Class.h"
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
    TSharedPtr<FJsonObject> SequencerEdit_FrameRateRecord(const FFrameRate& Rate)
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
    void SequencerEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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
    AActor* SequencerEdit_ResolveActorByName(UWorld* World, const FString& Target)
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

    /** Resolve a UMovieSceneTrack subclass by short name or full path.
     *  Short tokens cover the canonical track set the agent reaches for
     *  in 90% of cases. */
    UClass* ResolveTrackClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Token.ToLower();
        // Short-name lookups go through LoadClass to keep the includes
        // narrow; the engine resolves the canonical /Script/MovieSceneTracks
        // path for the well-known UMovieScene...Track types.
        struct FShortTokenMap
        {
            const TCHAR* Token;
            const TCHAR* Path;
        };
        static const FShortTokenMap Map[] = {
            { TEXT("transform"),          TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack") },
            { TEXT("3d_transform"),       TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack") },
            { TEXT("camera_cut"),         TEXT("/Script/MovieSceneTracks.MovieSceneCameraCutTrack") },
            { TEXT("cameracut"),          TEXT("/Script/MovieSceneTracks.MovieSceneCameraCutTrack") },
            { TEXT("skeletal_animation"), TEXT("/Script/MovieSceneTracks.MovieSceneSkeletalAnimationTrack") },
            { TEXT("skeletalanimation"),  TEXT("/Script/MovieSceneTracks.MovieSceneSkeletalAnimationTrack") },
            { TEXT("audio"),              TEXT("/Script/MovieSceneTracks.MovieSceneAudioTrack") },
            { TEXT("event"),              TEXT("/Script/MovieSceneTracks.MovieSceneEventTrack") },
            { TEXT("float"),              TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack") },
            { TEXT("subscene"),           TEXT("/Script/MovieSceneTracks.MovieSceneSubTrack") },
            { TEXT("bool"),               TEXT("/Script/MovieSceneTracks.MovieSceneBoolTrack") },
            { TEXT("byte"),               TEXT("/Script/MovieSceneTracks.MovieSceneByteTrack") },
        };
        for (const FShortTokenMap& Entry : Map)
        {
            if (Lower == Entry.Token)
            {
                if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, Entry.Path))
                {
                    return Loaded;
                }
            }
        }

        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UMovieSceneTrack::StaticClass()))
            {
                return Found;
            }
        }
        const FString TracksPath = FString::Printf(TEXT("/Script/MovieSceneTracks.%s"), *Token);
        if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, *TracksPath))
        {
            return Loaded;
        }
        return nullptr;
    }

    /** Find a binding GUID by short name. Walks possessables first
     *  (the common case for actor-bound tracks) and falls through to
     *  spawnables. Empty Target returns FGuid() to signal "no binding". */
    FGuid FindBindingByName(UMovieScene* MovieScene, const FString& Target)
    {
        if (!MovieScene || Target.IsEmpty())
        {
            return FGuid();
        }
        const int32 PossCount = MovieScene->GetPossessableCount();
        for (int32 I = 0; I < PossCount; ++I)
        {
            const FMovieScenePossessable& Poss = MovieScene->GetPossessable(I);
            if (Poss.GetName() == Target || Poss.GetName().Contains(Target))
            {
                return Poss.GetGuid();
            }
        }
        const int32 SpawnCount = MovieScene->GetSpawnableCount();
        for (int32 I = 0; I < SpawnCount; ++I)
        {
            FMovieSceneSpawnable& Spawn = MovieScene->GetSpawnable(I);
            if (Spawn.GetName() == Target || Spawn.GetName().Contains(Target))
            {
                return Spawn.GetGuid();
            }
        }
        return FGuid();
    }

    /** Find a track on a movie scene by FName / display name substring.
     *  When BindingGuid is set, we search the per-binding track list.
     *  Otherwise we walk the master track list. */
    UMovieSceneTrack* FindTrackByName(UMovieScene* MovieScene, const FString& Target, const FGuid& BindingGuid)
    {
        if (!MovieScene || Target.IsEmpty())
        {
            return nullptr;
        }
        auto Match = [&Target](UMovieSceneTrack* Track) -> bool
        {
            if (!Track)
            {
                return false;
            }
            if (Track->GetFName().ToString().Contains(Target))
            {
                return true;
            }
#if WITH_EDITORONLY_DATA
            if (Track->GetDisplayName().ToString().Contains(Target))
            {
                return true;
            }
#endif
            if (Track->GetClass()->GetName().Contains(Target))
            {
                return true;
            }
            return false;
        };

        if (BindingGuid.IsValid())
        {
            // Per-binding tracks live on the FMovieSceneBinding entry.
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                if (Binding.GetObjectGuid() != BindingGuid)
                {
                    continue;
                }
                for (UMovieSceneTrack* Track : Binding.GetTracks())
                {
                    if (Match(Track))
                    {
                        return Track;
                    }
                }
            }
            return nullptr;
        }
        for (UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            if (Match(Track))
            {
                return Track;
            }
        }
        // Camera cut track is special-cased on UMovieScene.
        if (UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack())
        {
            if (Match(CameraCut))
            {
                return CameraCut;
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
    if (Op == TEXT("add_track"))
    {
        return HandleAddTrack(Params);
    }
    if (Op == TEXT("add_section"))
    {
        return HandleAddSection(Params);
    }
    if (Op == TEXT("move_section"))
    {
        return HandleMoveSection(Params);
    }
    if (Op == TEXT("add_audio_track") || Op == TEXT("add_audio")
        || Op == TEXT("audio_track"))
    {
        return HandleAddAudioTrack(Params);
    }
    if (Op == TEXT("add_transform_section_keys")
        || Op == TEXT("add_transform_keys")
        || Op == TEXT("write_transform_keys"))
    {
        return HandleAddTransformSectionKeys(Params);
    }
    if (Op == TEXT("set_transform_channel_mask")
        || Op == TEXT("set_transform_mask")
        || Op == TEXT("set_channel_mask"))
    {
        return HandleSetTransformChannelMask(Params);
    }
    if (Op == TEXT("add_visibility_track")
        || Op == TEXT("add_visibility")
        || Op == TEXT("visibility_track"))
    {
        return HandleAddVisibilityTrack(Params);
    }
    if (Op == TEXT("add_audio_fade") || Op == TEXT("audio_fade")
        || Op == TEXT("add_fade") || Op == TEXT("set_audio_fade"))
    {
        return HandleAddAudioFade(Params);
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
        SequencerEdit_FrameRateRecord(MovieScene->GetTickResolution()));
    Result->SetObjectField(TEXT("display_rate"),
        SequencerEdit_FrameRateRecord(MovieScene->GetDisplayRate()));

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
    SequencerEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
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
            SequencerEdit_FrameRateRecord(MovieScene->GetTickResolution()));
        Result->SetObjectField(TEXT("display_rate"),
            SequencerEdit_FrameRateRecord(MovieScene->GetDisplayRate()));
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
    AActor* Actor = SequencerEdit_ResolveActorByName(World, ActorTarget);
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

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    FString TrackClassToken;
    if (!Params->TryGetStringField(TEXT("track_class"), TrackClassToken)
        && !Params->TryGetStringField(TEXT("class"), TrackClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'track_class' parameter"));
    }
    UClass* TrackClass = ResolveTrackClass(TrackClassToken);
    if (!TrackClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UMovieSceneTrack class '%s'"), *TrackClassToken));
    }
    if (TrackClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract"), *TrackClass->GetPathName()));
    }

    // Resolve optional binding GUID through the named lookup so callers
    // do not need to round-trip through `inspect` for the GUID.
    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }
    if (!BindingGuid.IsValid())
    {
        FString PossessableName;
        if (Params->TryGetStringField(TEXT("possessable"), PossessableName)
            || Params->TryGetStringField(TEXT("actor"), PossessableName))
        {
            BindingGuid = FindBindingByName(MovieScene, PossessableName);
            if (!BindingGuid.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("No binding matching '%s' on sequence '%s'"),
                        *PossessableName, *Sequence->GetName()));
            }
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UMovieSceneTrack* NewTrack = nullptr;
    if (BindingGuid.IsValid())
    {
        NewTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
    }
    else
    {
        // Camera cut tracks live on UMovieScene's dedicated CameraCutTrack
        // slot. Other master tracks attach via the no-binding AddTrack
        // overload (5.4+ unified the "Master" / "non-binding" surfaces).
        if (TrackClass->GetName().Contains(TEXT("CameraCut")))
        {
            if (UMovieSceneTrack* Existing = MovieScene->GetCameraCutTrack())
            {
                NewTrack = Existing;
            }
            else
            {
                // The camera cut track lives on a single dedicated slot
                // on UMovieScene. NewObject's the track, then registers
                // it through SetCameraCutTrack so Sequencer's runtime
                // picks it up the same way the editor's "Add Camera Cut
                // Track" button does.
                NewTrack = NewObject<UMovieSceneTrack>(MovieScene, TrackClass, NAME_None, RF_Transactional);
                if (NewTrack)
                {
                    MovieScene->SetCameraCutTrack(NewTrack);
                }
            }
        }
        else
        {
            NewTrack = MovieScene->AddTrack(TrackClass);
        }
    }
    if (!NewTrack)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to add track of class '%s'"), *TrackClass->GetPathName()));
    }

    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_track"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("track_name"), NewTrack->GetFName().ToString());
#if WITH_EDITORONLY_DATA
    Result->SetStringField(TEXT("display_name"), NewTrack->GetDisplayName().ToString());
#endif
    Result->SetStringField(TEXT("class"), TrackClass->GetName());
    Result->SetStringField(TEXT("class_path"), TrackClass->GetPathName());
    if (BindingGuid.IsValid())
    {
        Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddSection(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    FString TrackName;
    if (!Params->TryGetStringField(TEXT("track"), TrackName)
        && !Params->TryGetStringField(TEXT("track_name"), TrackName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'track' parameter (FName / display name / class substring)"));
    }

    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }

    UMovieSceneTrack* Track = FindTrackByName(MovieScene, TrackName, BindingGuid);
    if (!Track)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No track matching '%s' on sequence '%s'%s"),
                *TrackName, *Sequence->GetName(),
                BindingGuid.IsValid() ? *FString::Printf(TEXT(" (binding %s)"), *BindingGuid.ToString())
                                       : TEXT("")));
    }

    int32 StartFrame = 0;
    int32 DurationFrames = 0;
    if (!Params->TryGetNumberField(TEXT("start_frame"), StartFrame))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'start_frame' (integer, tick-resolution frame)"));
    }
    if (!Params->TryGetNumberField(TEXT("duration_frames"), DurationFrames))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'duration_frames' (integer, tick-resolution frames)"));
    }
    if (DurationFrames <= 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("'duration_frames' must be positive"));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UMovieSceneSection* NewSection = Track->CreateNewSection();
    if (!NewSection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Track '%s' (%s) returned null from CreateNewSection"),
                *TrackName, *Track->GetClass()->GetName()));
    }
    const TRange<FFrameNumber> Range = TRange<FFrameNumber>(
        TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartFrame)),
        TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(StartFrame + DurationFrames)));
    NewSection->SetRange(Range);
    Track->AddSection(*NewSection);

    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_section"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("track_name"), Track->GetFName().ToString());
    Result->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class"), NewSection->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class_path"), NewSection->GetClass()->GetPathName());
    Result->SetNumberField(TEXT("start_frame"), StartFrame);
    Result->SetNumberField(TEXT("end_frame_exclusive"), StartFrame + DurationFrames);
    Result->SetNumberField(TEXT("duration_frames"), DurationFrames);
    if (BindingGuid.IsValid())
    {
        Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleMoveSection(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    FString TrackName;
    if (!Params->TryGetStringField(TEXT("track"), TrackName)
        && !Params->TryGetStringField(TEXT("track_name"), TrackName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'track' parameter (FName / display name / class substring)"));
    }

    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }

    UMovieSceneTrack* Track = FindTrackByName(MovieScene, TrackName, BindingGuid);
    if (!Track)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No track matching '%s' on sequence '%s'%s"),
                *TrackName, *Sequence->GetName(),
                BindingGuid.IsValid() ? *FString::Printf(TEXT(" (binding %s)"), *BindingGuid.ToString())
                                       : TEXT("")));
    }

    int32 SectionIndex = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("section_index"), SectionIndex))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'section_index' (integer index into the track's sections array)"));
    }

    int32 StartFrame = 0;
    int32 DurationFrames = 0;
    if (!Params->TryGetNumberField(TEXT("start_frame"), StartFrame))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'start_frame' (integer, tick-resolution frame)"));
    }
    if (!Params->TryGetNumberField(TEXT("duration_frames"), DurationFrames))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'duration_frames' (integer, tick-resolution frames)"));
    }
    if (DurationFrames <= 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("'duration_frames' must be positive"));
    }

    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    if (SectionIndex < 0 || SectionIndex >= Sections.Num())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'section_index' %d out of range [0, %d) for track '%s'"),
                SectionIndex, Sections.Num(), *TrackName));
    }
    UMovieSceneSection* Section = Sections[SectionIndex];
    if (!Section)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Section at index %d on track '%s' is null"),
                SectionIndex, *TrackName));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Capture the previous range so the response can mirror it back.
    const TRange<FFrameNumber> PreviousRange = Section->GetRange();
    int32 PreviousStart = 0;
    int32 PreviousDuration = 0;
    const bool bHadStart = PreviousRange.GetLowerBound().IsClosed();
    const bool bHadEnd = PreviousRange.GetUpperBound().IsClosed();
    if (bHadStart)
    {
        PreviousStart = PreviousRange.GetLowerBoundValue().Value;
    }
    if (bHadStart && bHadEnd)
    {
        PreviousDuration = PreviousRange.GetUpperBoundValue().Value - PreviousRange.GetLowerBoundValue().Value;
    }

    const TRange<FFrameNumber> NewRange = TRange<FFrameNumber>(
        TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(StartFrame)),
        TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(StartFrame + DurationFrames)));
    // SetRange already calls TryModify(); we still dirty the package
    // so the next save lands. The base class has no separate
    // MarkAsChanged in 5.7, so this is the documented pattern.
    Section->SetRange(NewRange);
    Section->MarkPackageDirty();

    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("move_section"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("track_name"), Track->GetFName().ToString());
    Result->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class_path"), Section->GetClass()->GetPathName());
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    Result->SetNumberField(TEXT("start_frame"), StartFrame);
    Result->SetNumberField(TEXT("end_frame_exclusive"), StartFrame + DurationFrames);
    Result->SetNumberField(TEXT("duration_frames"), DurationFrames);
    Result->SetBoolField(TEXT("had_previous_start_frame"), bHadStart);
    Result->SetBoolField(TEXT("had_previous_end_frame"), bHadEnd);
    if (bHadStart)
    {
        Result->SetNumberField(TEXT("previous_start_frame"), PreviousStart);
    }
    if (bHadStart && bHadEnd)
    {
        Result->SetNumberField(TEXT("previous_duration_frames"), PreviousDuration);
    }
    if (BindingGuid.IsValid())
    {
        Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddAudioTrack(const TSharedPtr<FJsonObject>& Params)
{
    // Declarative one-call wrapper that lays a UMovieSceneAudioTrack
    // plus a UMovieSceneAudioSection down in a single pass.
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    // Resolve the sound asset. Accepts a /Game/... path or a short
    // name; the asset registry fallback handles "BGM_Loop" -> the
    // first USoundBase match.
    FString SoundPath;
    if (!Params->TryGetStringField(TEXT("sound"), SoundPath)
        && !Params->TryGetStringField(TEXT("sound_path"), SoundPath)
        && !Params->TryGetStringField(TEXT("sound_asset"), SoundPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sound' parameter (USoundBase path or short name)"));
    }
    USoundBase* Sound = nullptr;
    if (SoundPath.StartsWith(TEXT("/")))
    {
        Sound = Cast<USoundBase>(UEditorAssetLibrary::LoadAsset(SoundPath));
    }
    else
    {
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(USoundBase::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(SoundPath, ESearchCase::IgnoreCase))
            {
                Sound = Cast<USoundBase>(Data.GetAsset());
                if (Sound) break;
            }
        }
    }
    if (!Sound)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve USoundBase '%s' (pass a /Game/... path or a unique short name)"), *SoundPath));
    }

    // Optional binding (binding-scoped audio track sits under an
    // FMovieScenePossessable / FMovieSceneSpawnable). Empty binding
    // means a master audio track.
    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }
    if (!BindingGuid.IsValid())
    {
        FString PossessableName;
        if (Params->TryGetStringField(TEXT("possessable"), PossessableName)
            || Params->TryGetStringField(TEXT("actor"), PossessableName))
        {
            BindingGuid = FindBindingByName(MovieScene, PossessableName);
            if (!BindingGuid.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("No binding matching '%s' on sequence '%s'"),
                        *PossessableName, *Sequence->GetName()));
            }
        }
    }

    // Find an existing audio track on the matching scope. The
    // master-track lookup walks UMovieScene::GetTracks(); the
    // binding scope walks UMovieScene::FindTrack on the binding.
    UMovieSceneAudioTrack* AudioTrack = nullptr;
    bool bReusedExisting = false;
    if (BindingGuid.IsValid())
    {
        if (UMovieSceneTrack* Existing = MovieScene->FindTrack(UMovieSceneAudioTrack::StaticClass(), BindingGuid))
        {
            AudioTrack = Cast<UMovieSceneAudioTrack>(Existing);
            bReusedExisting = (AudioTrack != nullptr);
        }
    }
    else
    {
        for (UMovieSceneTrack* T : MovieScene->GetTracks())
        {
            if (UMovieSceneAudioTrack* Cand = Cast<UMovieSceneAudioTrack>(T))
            {
                AudioTrack = Cand;
                bReusedExisting = true;
                break;
            }
        }
    }

    // Optional `force_new_track` knob bypasses the reuse path.
    bool bForceNewTrack = false;
    Params->TryGetBoolField(TEXT("force_new_track"), bForceNewTrack);
    if (bForceNewTrack)
    {
        AudioTrack = nullptr;
        bReusedExisting = false;
    }

    if (!AudioTrack)
    {
        UMovieSceneTrack* NewTrack = nullptr;
        if (BindingGuid.IsValid())
        {
            NewTrack = MovieScene->AddTrack(UMovieSceneAudioTrack::StaticClass(), BindingGuid);
        }
        else
        {
            NewTrack = MovieScene->AddTrack(UMovieSceneAudioTrack::StaticClass());
        }
        AudioTrack = Cast<UMovieSceneAudioTrack>(NewTrack);
        if (!AudioTrack)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Failed to add a UMovieSceneAudioTrack"));
        }
    }

    // Start frame defaults to the MovieScene's playback range start
    // when omitted. The asset's intrinsic duration drives the
    // section length when `duration_frames` is missing.
    int32 StartFrameInt = 0;
    if (!Params->TryGetNumberField(TEXT("start_frame"), StartFrameInt))
    {
        // Default to the playback range's start when callers omit
        // the field. Falls back to 0 when the MovieScene has no
        // closed start bound.
        const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
        if (Playback.GetLowerBound().IsClosed())
        {
            StartFrameInt = Playback.GetLowerBoundValue().Value;
        }
    }
    const FFrameNumber StartFrame(StartFrameInt);

    // The track's AddNewSound picks the canonical AudioSection
    // subclass and seeds the section ranges. The returned section
    // is what we range-update from `duration_frames` (or the
    // sound's intrinsic length when missing).
    UMovieSceneSection* NewSection = AudioTrack->AddNewSound(Sound, StartFrame);
    if (!NewSection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMovieSceneAudioTrack::AddNewSound returned null for sound '%s'"),
                *Sound->GetPathName()));
    }
    UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(NewSection);

    int32 DurationFramesInt = 0;
    bool bDurationFromCaller = Params->TryGetNumberField(TEXT("duration_frames"), DurationFramesInt);
    if (!bDurationFromCaller)
    {
        Params->TryGetNumberField(TEXT("duration"), DurationFramesInt);
        if (DurationFramesInt != 0) bDurationFromCaller = true;
    }
    if (!bDurationFromCaller)
    {
        // Pull the sound's intrinsic length and convert through the
        // MovieScene's tick resolution. USoundBase::GetDuration
        // returns INDEFINITELY_LOOPING_DURATION (1e6f) for looping
        // cues; clamp to a single second so the section still has a
        // sensible default range when the caller forgot to set one.
        const float DurationSeconds = Sound->GetDuration();
        const FFrameRate TickRate = MovieScene->GetTickResolution();
        const float SafeDurationSeconds = (DurationSeconds > 0.0f && DurationSeconds < 1e5f)
            ? DurationSeconds
            : 1.0f;
        DurationFramesInt = static_cast<int32>(SafeDurationSeconds * TickRate.AsDecimal());
        if (DurationFramesInt <= 0) DurationFramesInt = 1;
    }

    const FFrameNumber EndFrame(StartFrame.Value + DurationFramesInt);
    const TRange<FFrameNumber> NewRange = TRange<FFrameNumber>(
        TRangeBound<FFrameNumber>::Inclusive(StartFrame),
        TRangeBound<FFrameNumber>::Exclusive(EndFrame));
    NewSection->SetRange(NewRange);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_audio_track"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("sound"), Sound->GetPathName());
    Result->SetStringField(TEXT("sound_class"), Sound->GetClass()->GetName());
    Result->SetStringField(TEXT("track_name"), AudioTrack->GetFName().ToString());
    Result->SetStringField(TEXT("track_class"), AudioTrack->GetClass()->GetName());
    Result->SetStringField(TEXT("track_class_path"), AudioTrack->GetClass()->GetPathName());
    Result->SetBoolField(TEXT("reused_existing_track"), bReusedExisting);
    if (BindingGuid.IsValid())
    {
        Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    }
    Result->SetStringField(TEXT("section_class"), NewSection->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class_path"), NewSection->GetClass()->GetPathName());
    Result->SetBoolField(TEXT("section_is_audio_section"), AudioSection != nullptr);
    Result->SetNumberField(TEXT("section_index"),
        AudioTrack->GetAllSections().IndexOfByKey(NewSection));
    Result->SetNumberField(TEXT("start_frame"), StartFrame.Value);
    Result->SetNumberField(TEXT("end_frame"), EndFrame.Value);
    Result->SetNumberField(TEXT("duration_frames"), DurationFramesInt);
    Result->SetBoolField(TEXT("duration_from_caller"), bDurationFromCaller);
    {
        // Also surface the intrinsic length (in seconds) for caller
        // diagnostics. Sound->GetDuration returns 1e6f for looping
        // cues; the bool flag tells the caller whether the length
        // is meaningful.
        const float Seconds = Sound->GetDuration();
        Result->SetNumberField(TEXT("sound_duration_seconds"), Seconds);
        Result->SetBoolField(TEXT("sound_duration_finite"), (Seconds > 0.0f && Seconds < 1e5f));
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

namespace
{
    /** Pull a 3-element number array out of a JSON value. Accepts an
     *  `[x, y, z]` array or an `{x, y, z}` object. Returns false on
     *  shape mismatch. */
    bool TransformKey_TryParseVec3(const TSharedPtr<FJsonValue>& Value, FVector& Out)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() < 3) return false;
            Out = FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
            double X = 0.0, Y = 0.0, Z = 0.0;
            if (Obj->TryGetNumberField(TEXT("x"), X)
                && Obj->TryGetNumberField(TEXT("y"), Y)
                && Obj->TryGetNumberField(TEXT("z"), Z))
            {
                Out = FVector(X, Y, Z);
                return true;
            }
            if (Obj->TryGetNumberField(TEXT("X"), X)
                && Obj->TryGetNumberField(TEXT("Y"), Y)
                && Obj->TryGetNumberField(TEXT("Z"), Z))
            {
                Out = FVector(X, Y, Z);
                return true;
            }
        }
        return false;
    }

    /** Apply a key shape to a FMovieSceneDoubleChannel. */
    void TransformKey_WriteDoubleKey(FMovieSceneDoubleChannel* Channel,
                                     FFrameNumber Frame, double Value,
                                     const FString& Interpolation)
    {
        if (!Channel) return;
        if (Interpolation == TEXT("linear"))
        {
            Channel->AddLinearKey(Frame, Value);
        }
        else if (Interpolation == TEXT("constant") || Interpolation == TEXT("step"))
        {
            Channel->AddConstantKey(Frame, Value);
        }
        else
        {
            Channel->AddCubicKey(Frame, Value);
        }
    }

    /** Apply a key shape to a FMovieSceneFloatChannel; older transform
     *  section variants and per-property float tracks use the float
     *  channel shape. */
    void TransformKey_WriteFloatKey(FMovieSceneFloatChannel* Channel,
                                    FFrameNumber Frame, float Value,
                                    const FString& Interpolation)
    {
        if (!Channel) return;
        if (Interpolation == TEXT("linear"))
        {
            Channel->AddLinearKey(Frame, Value);
        }
        else if (Interpolation == TEXT("constant") || Interpolation == TEXT("step"))
        {
            Channel->AddConstantKey(Frame, Value);
        }
        else
        {
            Channel->AddCubicKey(Frame, Value);
        }
    }

    /** Write one scalar component to either the double channel at
     *  ChannelIndex on the section's channel proxy or the float channel
     *  at the same slot when the section's storage shape predates the
     *  5.4 double-channel migration. Returns true when a write landed. */
    bool TransformKey_WriteChannel(FMovieSceneChannelProxy& Proxy, int32 ChannelIndex,
                                   FFrameNumber Frame, double Value,
                                   const FString& Interpolation,
                                   FString& OutKind)
    {
        if (FMovieSceneDoubleChannel* DoubleChannel = Proxy.GetChannel<FMovieSceneDoubleChannel>(ChannelIndex))
        {
            TransformKey_WriteDoubleKey(DoubleChannel, Frame, Value, Interpolation);
            OutKind = TEXT("double");
            return true;
        }
        if (FMovieSceneFloatChannel* FloatChannel = Proxy.GetChannel<FMovieSceneFloatChannel>(ChannelIndex))
        {
            TransformKey_WriteFloatKey(FloatChannel, Frame, static_cast<float>(Value), Interpolation);
            OutKind = TEXT("float");
            return true;
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddTransformSectionKeys(const TSharedPtr<FJsonObject>& Params)
{
    // Resolve the target sequence + MovieScene the same way the other
    // sequencer_edit ops do.
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    // Resolve the target binding. Transform tracks are always binding-scoped:
    // the engine refuses to add a master 3D transform track because the
    // track has nothing to drive without a binding.
    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }
    if (!BindingGuid.IsValid())
    {
        FString PossessableName;
        if (Params->TryGetStringField(TEXT("possessable"), PossessableName)
            || Params->TryGetStringField(TEXT("actor"), PossessableName))
        {
            BindingGuid = FindBindingByName(MovieScene, PossessableName);
            if (!BindingGuid.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("No binding matching '%s' on sequence '%s'"),
                        *PossessableName, *Sequence->GetName()));
            }
        }
    }
    if (!BindingGuid.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing target binding ('binding' GUID or 'actor' / 'possessable' name)"));
    }

    // Find or create a 3D transform track on the binding. We do not pass
    // through `add_track` so callers can write keys in one call without
    // a follow-on.
    UMovieScene3DTransformTrack* TransformTrack = nullptr;
    bool bTrackCreated = false;
    if (UMovieSceneTrack* Existing = MovieScene->FindTrack(UMovieScene3DTransformTrack::StaticClass(), BindingGuid))
    {
        TransformTrack = Cast<UMovieScene3DTransformTrack>(Existing);
    }
    if (!TransformTrack)
    {
        UMovieSceneTrack* NewTrack = MovieScene->AddTrack(UMovieScene3DTransformTrack::StaticClass(), BindingGuid);
        TransformTrack = Cast<UMovieScene3DTransformTrack>(NewTrack);
        if (!TransformTrack)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to add UMovieScene3DTransformTrack to binding %s"), *BindingGuid.ToString()));
        }
        bTrackCreated = true;
    }

    // Parse the keyframes array. Each entry is
    // `{time_frames, location?, rotation?, scale?}`. The optional
    // start_frame falls back to either the smallest key time or the
    // playback start; we use the smallest key time after parsing.
    const TArray<TSharedPtr<FJsonValue>>* KeyframesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("keyframes"), KeyframesArr)
        || !KeyframesArr || KeyframesArr->Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'keyframes' array (at least one keyframe required)"));
    }

    FString Interpolation = TEXT("cubic");
    {
        FString Token;
        if (Params->TryGetStringField(TEXT("interpolation"), Token)
            || Params->TryGetStringField(TEXT("key_shape"), Token))
        {
            Interpolation = Token.ToLower();
        }
    }

    // Iterate keyframes, gather min/max frames so the section's range
    // covers every key. We could expand per key through `ExpandToFrame`,
    // but a single SetRange at the end keeps the section's TryModify
    // count down.
    struct FParsedKey
    {
        FFrameNumber Frame;
        FVector Location;
        bool bHasLocation = false;
        FRotator Rotation;
        bool bHasRotation = false;
        FVector Scale;
        bool bHasScale = false;
    };
    TArray<FParsedKey> ParsedKeys;
    int32 MinFrame = TNumericLimits<int32>::Max();
    int32 MaxFrame = TNumericLimits<int32>::Min();
    for (int32 RowIdx = 0; RowIdx < KeyframesArr->Num(); ++RowIdx)
    {
        const TSharedPtr<FJsonValue>& RowVal = (*KeyframesArr)[RowIdx];
        if (!RowVal.IsValid() || RowVal->Type != EJson::Object)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("keyframes[%d] is not an object"), RowIdx));
        }
        const TSharedPtr<FJsonObject>& Row = RowVal->AsObject();
        double TimeFramesValue = 0.0;
        if (!Row->TryGetNumberField(TEXT("time_frames"), TimeFramesValue)
            && !Row->TryGetNumberField(TEXT("frame"), TimeFramesValue)
            && !Row->TryGetNumberField(TEXT("time"), TimeFramesValue))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("keyframes[%d] missing 'time_frames' (integer, tick-resolution frame)"), RowIdx));
        }
        FParsedKey Parsed;
        Parsed.Frame = FFrameNumber(static_cast<int32>(TimeFramesValue));
        if (Parsed.Frame.Value < MinFrame) MinFrame = Parsed.Frame.Value;
        if (Parsed.Frame.Value > MaxFrame) MaxFrame = Parsed.Frame.Value;

        TSharedPtr<FJsonValue> LocVal = Row->TryGetField(TEXT("location"));
        if (!LocVal.IsValid()) LocVal = Row->TryGetField(TEXT("translation"));
        if (LocVal.IsValid())
        {
            if (!TransformKey_TryParseVec3(LocVal, Parsed.Location))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("keyframes[%d].location: expected [x, y, z] or {x, y, z}"), RowIdx));
            }
            Parsed.bHasLocation = true;
        }
        TSharedPtr<FJsonValue> RotVal = Row->TryGetField(TEXT("rotation"));
        if (RotVal.IsValid())
        {
            FVector R;
            if (!TransformKey_TryParseVec3(RotVal, R))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("keyframes[%d].rotation: expected [roll, pitch, yaw] or {x, y, z}"), RowIdx));
            }
            // The transform section's rotation channels at proxy slots
            // 3 / 4 / 5 are Roll / Pitch / Yaw (the canonical FRotator
            // channel order on UMovieScene3DTransformSection's
            // CacheChannelProxy). Input is `[roll, pitch, yaw]` in
            // degrees, matching the Sequencer transform track's per-key
            // surface.
            Parsed.Rotation = FRotator();
            Parsed.Rotation.Roll  = R.X;
            Parsed.Rotation.Pitch = R.Y;
            Parsed.Rotation.Yaw   = R.Z;
            Parsed.bHasRotation = true;
        }
        TSharedPtr<FJsonValue> ScaleVal = Row->TryGetField(TEXT("scale"));
        if (ScaleVal.IsValid())
        {
            if (!TransformKey_TryParseVec3(ScaleVal, Parsed.Scale))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("keyframes[%d].scale: expected [sx, sy, sz] or {x, y, z}"), RowIdx));
            }
            Parsed.bHasScale = true;
        }
        if (!Parsed.bHasLocation && !Parsed.bHasRotation && !Parsed.bHasScale)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("keyframes[%d] needs at least one of 'location' / 'rotation' / 'scale'"), RowIdx));
        }
        ParsedKeys.Add(Parsed);
    }

    // Find or create the transform section. The transform track's
    // CreateNewSection picks UMovieScene3DTransformSection as the native
    // subclass; we attach + range-set the new section so subsequent
    // SetRange calls expand cleanly.
    UMovieScene3DTransformSection* Section = nullptr;
    bool bSectionCreated = false;
    {
        // start_frame defaults to either the optional caller param or
        // the smallest key time we just parsed.
        int32 CallerStartFrame = MinFrame;
        Params->TryGetNumberField(TEXT("start_frame"), CallerStartFrame);

        const TArray<UMovieSceneSection*>& Existing = TransformTrack->GetAllSections();
        for (UMovieSceneSection* S : Existing)
        {
            if (UMovieScene3DTransformSection* TS = Cast<UMovieScene3DTransformSection>(S))
            {
                Section = TS;
                break;
            }
        }
        if (!Section)
        {
            UMovieSceneSection* NewSection = TransformTrack->CreateNewSection();
            Section = Cast<UMovieScene3DTransformSection>(NewSection);
            if (!Section)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("UMovieScene3DTransformTrack::CreateNewSection returned the wrong section subclass"));
            }
            // Seed the range to the key time bounds; ExpandToFrame
            // below stretches as needed.
            Section->SetRange(TRange<FFrameNumber>(
                TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(CallerStartFrame)),
                TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(CallerStartFrame + 1))));
            TransformTrack->AddSection(*Section);
            bSectionCreated = true;
        }
        // Stretch the section to cover every keyframe time. ExpandToFrame
        // is the documented section-range-extend path for keyframe
        // authoring and is what the Sequencer key-add UI uses.
        Section->ExpandToFrame(FFrameNumber(MinFrame));
        Section->ExpandToFrame(FFrameNumber(MaxFrame));
    }

    // Walk channel proxy slots in the canonical 3D transform section
    // order: Translation X/Y/Z, Rotation X(Roll)/Y(Pitch)/Z(Yaw),
    // Scale X/Y/Z. The section's private FMovieSceneDoubleChannel[3]
    // arrays show up through the channel proxy at slots 0..8 (plus the
    // ManualWeight float channel at slot 9, which we leave alone).
    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    int32 KeysWritten = 0;
    int32 KeysSkipped = 0;
    int32 KeyfailureCount = 0;
    TArray<FString> ChannelKindLog;
    for (const FParsedKey& Key : ParsedKeys)
    {
        struct FChannelWrite
        {
            int32 Index;
            double Value;
            bool bActive;
        };
        const FChannelWrite Writes[9] = {
            { 0, Key.Location.X, Key.bHasLocation },
            { 1, Key.Location.Y, Key.bHasLocation },
            { 2, Key.Location.Z, Key.bHasLocation },
            { 3, Key.Rotation.Roll, Key.bHasRotation },
            { 4, Key.Rotation.Pitch, Key.bHasRotation },
            { 5, Key.Rotation.Yaw, Key.bHasRotation },
            { 6, Key.Scale.X, Key.bHasScale },
            { 7, Key.Scale.Y, Key.bHasScale },
            { 8, Key.Scale.Z, Key.bHasScale },
        };
        for (const FChannelWrite& W : Writes)
        {
            if (!W.bActive) { ++KeysSkipped; continue; }
            FString Kind;
            if (TransformKey_WriteChannel(Proxy, W.Index, Key.Frame, W.Value, Interpolation, Kind))
            {
                ++KeysWritten;
                if (!ChannelKindLog.Contains(Kind))
                {
                    ChannelKindLog.Add(Kind);
                }
            }
            else
            {
                ++KeyfailureCount;
            }
        }
    }

    if (KeysWritten == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("No keys landed; the transform section has no addressable float / double channels in the 0..8 range"));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Section->MarkPackageDirty();
    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_transform_section_keys"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    Result->SetStringField(TEXT("track_name"), TransformTrack->GetFName().ToString());
    Result->SetStringField(TEXT("track_class"), TransformTrack->GetClass()->GetName());
    Result->SetBoolField(TEXT("track_created"), bTrackCreated);
    Result->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class_path"), Section->GetClass()->GetPathName());
    Result->SetBoolField(TEXT("section_created"), bSectionCreated);
    Result->SetNumberField(TEXT("keyframe_count"), ParsedKeys.Num());
    Result->SetNumberField(TEXT("keys_written"), KeysWritten);
    Result->SetNumberField(TEXT("keys_skipped"), KeysSkipped);
    Result->SetNumberField(TEXT("key_failures"), KeyfailureCount);
    Result->SetStringField(TEXT("interpolation"), Interpolation);
    Result->SetNumberField(TEXT("min_frame"), MinFrame);
    Result->SetNumberField(TEXT("max_frame"), MaxFrame);
    {
        TArray<TSharedPtr<FJsonValue>> KindArr;
        for (const FString& Kind : ChannelKindLog)
        {
            KindArr.Add(MakeShared<FJsonValueString>(Kind));
        }
        Result->SetArrayField(TEXT("channel_kinds"), KindArr);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

namespace
{
    /** Resolve a token like `translation_x` / `rotation` / `scale_z` /
     *  `all_transform` to the matching `EMovieSceneTransformChannel`
     *  bit. Returns false on an unknown token. Normalises `_` / `.`
     *  out so callers can spell the token either way. */
    bool TransformMask_TryParseChannelToken(const FString& InToken, EMovieSceneTransformChannel& OutChannel)
    {
        FString T = InToken.ToLower();
        T.ReplaceInline(TEXT("_"), TEXT(""), ESearchCase::CaseSensitive);
        T.ReplaceInline(TEXT("."), TEXT(""), ESearchCase::CaseSensitive);
        T.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
        if (T == TEXT("none") || T == TEXT("0"))
        {
            OutChannel = EMovieSceneTransformChannel::None;
            return true;
        }
        if (T == TEXT("translationx") || T == TEXT("locationx") || T == TEXT("tx"))
        {
            OutChannel = EMovieSceneTransformChannel::TranslationX;
            return true;
        }
        if (T == TEXT("translationy") || T == TEXT("locationy") || T == TEXT("ty"))
        {
            OutChannel = EMovieSceneTransformChannel::TranslationY;
            return true;
        }
        if (T == TEXT("translationz") || T == TEXT("locationz") || T == TEXT("tz"))
        {
            OutChannel = EMovieSceneTransformChannel::TranslationZ;
            return true;
        }
        if (T == TEXT("translation") || T == TEXT("location") || T == TEXT("position"))
        {
            OutChannel = EMovieSceneTransformChannel::Translation;
            return true;
        }
        if (T == TEXT("rotationx") || T == TEXT("rotx") || T == TEXT("roll"))
        {
            OutChannel = EMovieSceneTransformChannel::RotationX;
            return true;
        }
        if (T == TEXT("rotationy") || T == TEXT("roty") || T == TEXT("pitch"))
        {
            OutChannel = EMovieSceneTransformChannel::RotationY;
            return true;
        }
        if (T == TEXT("rotationz") || T == TEXT("rotz") || T == TEXT("yaw"))
        {
            OutChannel = EMovieSceneTransformChannel::RotationZ;
            return true;
        }
        if (T == TEXT("rotation"))
        {
            OutChannel = EMovieSceneTransformChannel::Rotation;
            return true;
        }
        if (T == TEXT("scalex") || T == TEXT("sx"))
        {
            OutChannel = EMovieSceneTransformChannel::ScaleX;
            return true;
        }
        if (T == TEXT("scaley") || T == TEXT("sy"))
        {
            OutChannel = EMovieSceneTransformChannel::ScaleY;
            return true;
        }
        if (T == TEXT("scalez") || T == TEXT("sz"))
        {
            OutChannel = EMovieSceneTransformChannel::ScaleZ;
            return true;
        }
        if (T == TEXT("scale"))
        {
            OutChannel = EMovieSceneTransformChannel::Scale;
            return true;
        }
        if (T == TEXT("alltransform") || T == TEXT("transform"))
        {
            OutChannel = EMovieSceneTransformChannel::AllTransform;
            return true;
        }
        if (T == TEXT("weight"))
        {
            OutChannel = EMovieSceneTransformChannel::Weight;
            return true;
        }
        if (T == TEXT("all"))
        {
            OutChannel = EMovieSceneTransformChannel::All;
            return true;
        }
        return false;
    }

    /** Decode the mask bitfield back into an FString array of the
     *  canonical token spellings so the response echoes a stable
     *  picture of what the section now drives. We emit individual
     *  axis bits, not the group rollups, so a caller sees exactly
     *  which channels animate. */
    TArray<FString> TransformMask_TokenizeMask(EMovieSceneTransformChannel Mask)
    {
        TArray<FString> Out;
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::TranslationX)) Out.Add(TEXT("TranslationX"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::TranslationY)) Out.Add(TEXT("TranslationY"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::TranslationZ)) Out.Add(TEXT("TranslationZ"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::RotationX))    Out.Add(TEXT("RotationX"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::RotationY))    Out.Add(TEXT("RotationY"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::RotationZ))    Out.Add(TEXT("RotationZ"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::ScaleX))       Out.Add(TEXT("ScaleX"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::ScaleY))       Out.Add(TEXT("ScaleY"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::ScaleZ))       Out.Add(TEXT("ScaleZ"));
        if (EnumHasAllFlags(Mask, EMovieSceneTransformChannel::Weight))       Out.Add(TEXT("Weight"));
        return Out;
    }
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleSetTransformChannelMask(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    // Resolve the binding. Transform tracks always live under a
    // binding; the engine refuses a master 3D transform track.
    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }
    if (!BindingGuid.IsValid())
    {
        FString PossessableName;
        if (Params->TryGetStringField(TEXT("possessable"), PossessableName)
            || Params->TryGetStringField(TEXT("actor"), PossessableName))
        {
            BindingGuid = FindBindingByName(MovieScene, PossessableName);
            if (!BindingGuid.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("No binding matching '%s' on sequence '%s'"),
                        *PossessableName, *Sequence->GetName()));
            }
        }
    }
    if (!BindingGuid.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing target binding ('binding' GUID or 'actor' / 'possessable' name)"));
    }

    UMovieSceneTrack* Existing = MovieScene->FindTrack(UMovieScene3DTransformTrack::StaticClass(), BindingGuid);
    UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Existing);
    if (!TransformTrack)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No UMovieScene3DTransformTrack on binding %s"), *BindingGuid.ToString()));
    }

    // section_index defaults to 0 so the typical "set the mask on the
    // only section" path is a single arg. We index into GetAllSections()
    // the same way `move_section` does.
    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);
    const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
    if (SectionIndex < 0 || SectionIndex >= Sections.Num())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'section_index' %d out of range [0, %d) for the transform track on binding %s"),
                SectionIndex, Sections.Num(), *BindingGuid.ToString()));
    }
    UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Sections[SectionIndex]);
    if (!Section)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Section at index %d on the transform track is not a UMovieScene3DTransformSection"),
                SectionIndex));
    }

    // Parse the mask. Either pass a raw integer `mask` (EMovieSceneTransformChannel
    // bit layout) or a flat `channels` list of token names. The token
    // path resolves the canonical "Translation / Rotation / Scale" rollups
    // plus the per-axis bits.
    uint32 MaskBits = 0;
    bool bMaskSourced = false;
    TArray<FString> UnknownTokens;
    if (Params->HasField(TEXT("mask")))
    {
        double Raw = 0.0;
        if (Params->TryGetNumberField(TEXT("mask"), Raw))
        {
            // The bit layout maxes out at 0x3FF (All); larger inputs
            // silently lose the extra bits when SetMask runs. We do not
            // gate that here so a future engine addition stays forwards-
            // compatible.
            MaskBits = static_cast<uint32>(Raw);
            bMaskSourced = true;
        }
    }
    if (!bMaskSourced)
    {
        const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
        if (Params->TryGetArrayField(TEXT("channels"), ChannelsArr)
            && ChannelsArr)
        {
            for (const TSharedPtr<FJsonValue>& V : *ChannelsArr)
            {
                FString Token;
                if (!V.IsValid() || !V->TryGetString(Token))
                {
                    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                        TEXT("'channels' entry is not a string token"));
                }
                EMovieSceneTransformChannel Channel = EMovieSceneTransformChannel::None;
                if (!TransformMask_TryParseChannelToken(Token, Channel))
                {
                    UnknownTokens.Add(Token);
                    continue;
                }
                MaskBits |= static_cast<uint32>(Channel);
            }
            bMaskSourced = true;
        }
    }
    if (!bMaskSourced)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing mask source: pass either 'mask' (integer) or 'channels' (array of channel tokens)"));
    }
    if (UnknownTokens.Num() > 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown channel token(s) in 'channels': %s"),
                *FString::Join(UnknownTokens, TEXT(", "))));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Capture the previous mask so the response can mirror the
    // before / after state for the caller.
    const FMovieSceneTransformMask PreviousMask = Section->GetMask();
    const uint32 PreviousBits = static_cast<uint32>(PreviousMask.GetChannels());

    // SetMask rebuilds the channel proxy the next time it is asked for
    // (ChannelProxy = nullptr inside the engine implementation), so the
    // editor's channel list refreshes on the next inspect.
    const FMovieSceneTransformMask NewMask(static_cast<EMovieSceneTransformChannel>(MaskBits));
    Section->Modify();
    Section->SetMask(NewMask);
    Section->MarkPackageDirty();
    Sequence->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_transform_channel_mask"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    Result->SetStringField(TEXT("track_name"), TransformTrack->GetFName().ToString());
    Result->SetStringField(TEXT("track_class"), TransformTrack->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class_path"), Section->GetClass()->GetPathName());
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    Result->SetNumberField(TEXT("mask"), MaskBits);
    Result->SetNumberField(TEXT("previous_mask"), PreviousBits);
    {
        const TArray<FString> Tokens = TransformMask_TokenizeMask(NewMask.GetChannels());
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& T : Tokens) { Arr.Add(MakeShared<FJsonValueString>(T)); }
        Result->SetArrayField(TEXT("channels"), Arr);
    }
    {
        const TArray<FString> Tokens = TransformMask_TokenizeMask(PreviousMask.GetChannels());
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& T : Tokens) { Arr.Add(MakeShared<FJsonValueString>(T)); }
        Result->SetArrayField(TEXT("previous_channels"), Arr);
    }
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddVisibilityTrack(const TSharedPtr<FJsonObject>& Params)
{
    // Declarative one-call wrapper for the "show / hide an actor over
    // a frame range" pattern. Visibility is a per-binding concept:
    // - For an FMovieScenePossessable we attach a
    //   `UMovieSceneVisibilityTrack` (a bool property track driving
    //   `AActor::SetActorHiddenInGame`).
    // - For an FMovieSceneSpawnable we attach a `UMovieSceneSpawnTrack`
    //   (the bool track that gates the spawnable's lifetime); the
    //   spawnable has no "hidden" property in the same way a possessed
    //   actor does, so SpawnTrack is the canonical surface.
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    // Visibility is binding-scoped; a master visibility track has no
    // meaning. Require either an explicit GUID or a name to resolve.
    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }
    if (!BindingGuid.IsValid())
    {
        FString PossessableName;
        if (Params->TryGetStringField(TEXT("possessable"), PossessableName)
            || Params->TryGetStringField(TEXT("actor"), PossessableName))
        {
            BindingGuid = FindBindingByName(MovieScene, PossessableName);
            if (!BindingGuid.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("No binding matching '%s' on sequence '%s'"),
                        *PossessableName, *Sequence->GetName()));
            }
        }
    }
    if (!BindingGuid.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Visibility is per-binding: pass 'binding' (GUID) or 'actor' / 'possessable' (name)"));
    }

    // Pick the right track subclass based on the binding kind. The
    // FMovieSceneSpawnable lookup is keyed by GUID; FindSpawnable
    // returns nullptr for possessables, so the test below is one cast.
    const bool bIsSpawnable = (MovieScene->FindSpawnable(BindingGuid) != nullptr);
    UClass* TrackClass = bIsSpawnable
        ? static_cast<UClass*>(UMovieSceneSpawnTrack::StaticClass())
        : static_cast<UClass*>(UMovieSceneVisibilityTrack::StaticClass());

    // Find an existing track on the binding so the op is idempotent
    // for follow-up adds; pass `force_new_track=true` to bypass.
    bool bForceNewTrack = false;
    Params->TryGetBoolField(TEXT("force_new_track"), bForceNewTrack);

    UMovieSceneTrack* TargetTrack = nullptr;
    bool bReusedExisting = false;
    if (!bForceNewTrack)
    {
        if (UMovieSceneTrack* Existing = MovieScene->FindTrack(TrackClass, BindingGuid))
        {
            TargetTrack = Existing;
            bReusedExisting = true;
        }
    }
    if (!TargetTrack)
    {
        TargetTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
    }
    if (!TargetTrack)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to add %s under binding %s"),
                *TrackClass->GetName(), *BindingGuid.ToString()));
    }

    // Start frame defaults to the MovieScene's playback range start.
    // Duration defaults to the playback range length; explicit
    // `end_frame` wins over `duration_frames`.
    int32 StartFrameInt = 0;
    bool bStartFromCaller = Params->TryGetNumberField(TEXT("start_frame"), StartFrameInt);
    if (!bStartFromCaller)
    {
        const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
        if (Playback.GetLowerBound().IsClosed())
        {
            StartFrameInt = Playback.GetLowerBoundValue().Value;
        }
    }
    int32 EndFrameInt = 0;
    bool bEndFromCaller = Params->TryGetNumberField(TEXT("end_frame"), EndFrameInt);
    int32 DurationFramesInt = 0;
    const bool bDurationFromCaller = Params->TryGetNumberField(TEXT("duration_frames"), DurationFramesInt)
                                  || Params->TryGetNumberField(TEXT("duration"), DurationFramesInt);
    if (!bEndFromCaller)
    {
        if (bDurationFromCaller)
        {
            EndFrameInt = StartFrameInt + DurationFramesInt;
        }
        else
        {
            const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
            if (Playback.GetUpperBound().IsClosed())
            {
                EndFrameInt = Playback.GetUpperBoundValue().Value;
            }
            else
            {
                // No closed upper bound: default to one display-rate
                // second so the section has some range.
                const FFrameRate Tick = MovieScene->GetTickResolution();
                EndFrameInt = StartFrameInt + static_cast<int32>(Tick.AsDecimal());
            }
        }
    }
    if (EndFrameInt <= StartFrameInt)
    {
        EndFrameInt = StartFrameInt + 1;
    }
    DurationFramesInt = EndFrameInt - StartFrameInt;

    // The track decides its native section subclass. For
    // UMovieSceneVisibilityTrack that is UMovieSceneVisibilitySection
    // (a UMovieSceneBoolSection subclass with the per-evaluation
    // visibility entity provider). For UMovieSceneSpawnTrack that is
    // UMovieSceneSpawnSection (also a UMovieSceneBoolSection
    // subclass). The shared base lets us write the channel default
    // through one path.
    UMovieSceneSection* NewSection = TargetTrack->CreateNewSection();
    if (!NewSection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Track '%s' CreateNewSection returned null"),
                *TargetTrack->GetClass()->GetName()));
    }

    const FFrameNumber StartFrame(StartFrameInt);
    const FFrameNumber EndFrame(EndFrameInt);
    const TRange<FFrameNumber> NewRange = TRange<FFrameNumber>(
        TRangeBound<FFrameNumber>::Inclusive(StartFrame),
        TRangeBound<FFrameNumber>::Exclusive(EndFrame));
    NewSection->SetRange(NewRange);

    // Default value flips between "visible / alive" (true, default)
    // and "hidden / dead" (false). The bool section base exposes the
    // channel through `GetChannel()`; both Visibility and Spawn
    // sections inherit from UMovieSceneBoolSection.
    bool bVisible = true;
    Params->TryGetBoolField(TEXT("visible"), bVisible);
    if (UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(NewSection))
    {
        BoolSection->GetChannel().SetDefault(bVisible);
    }

    TargetTrack->AddSection(*NewSection);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    Sequence->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_visibility_track"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    Result->SetStringField(TEXT("binding_kind"), bIsSpawnable ? TEXT("spawnable") : TEXT("possessable"));
    Result->SetStringField(TEXT("track_class"), TrackClass->GetName());
    Result->SetStringField(TEXT("track_class_path"), TrackClass->GetPathName());
    Result->SetStringField(TEXT("track_name"), TargetTrack->GetFName().ToString());
    Result->SetBoolField(TEXT("reused_existing_track"), bReusedExisting);
    Result->SetStringField(TEXT("section_class"), NewSection->GetClass()->GetName());
    Result->SetStringField(TEXT("section_class_path"), NewSection->GetClass()->GetPathName());
    Result->SetNumberField(TEXT("section_index"),
        TargetTrack->GetAllSections().IndexOfByKey(NewSection));
    Result->SetNumberField(TEXT("start_frame"), StartFrameInt);
    Result->SetNumberField(TEXT("end_frame"), EndFrameInt);
    Result->SetNumberField(TEXT("duration_frames"), DurationFramesInt);
    Result->SetBoolField(TEXT("visible"), bVisible);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSequencerEditCommands::HandleAddAudioFade(const TSharedPtr<FJsonObject>& Params)
{
    // Writes a fade-in / fade-out volume ramp on an existing
    // UMovieSceneAudioSection. The engine stores per-section audio
    // volume on the section's `SoundVolume` FMovieSceneFloatChannel,
    // which the sequencer editor's "Volume" curve drives. Recent UE
    // versions (5.4+) moved the channel proxy under
    // `CacheChannelProxy` and registered SoundVolume at channel index
    // 0 with the identifier "Volume", so we resolve the channel
    // through `Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0)`.
    //
    // The fade lands as a 4-key envelope (interior bounds linear):
    //   [start_frame, 0.0]
    //   [start_frame + fade_in_frames, 1.0]
    //   [end_frame - fade_out_frames, 1.0]
    //   [end_frame, 0.0]
    //
    // A zero fade duration drops the matching pair of keys so the
    // envelope still lands clean. The op clears the channel's
    // existing keys first so a second call replaces the prior fade
    // rather than stacking keys on top.
    FString SequencePath;
    if (!Params->TryGetStringField(TEXT("sequence"), SequencePath)
        && !Params->TryGetStringField(TEXT("path"), SequencePath))
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

    // Optional binding scope: when the audio track sits under a
    // possessable / spawnable, the caller can pass the binding GUID
    // or a possessable name. Empty binding targets the master audio
    // track.
    FGuid BindingGuid;
    FString BindingGuidString;
    if (Params->TryGetStringField(TEXT("binding"), BindingGuidString)
        || Params->TryGetStringField(TEXT("binding_guid"), BindingGuidString))
    {
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid binding GUID '%s'"), *BindingGuidString));
        }
    }
    if (!BindingGuid.IsValid())
    {
        FString PossessableName;
        if (Params->TryGetStringField(TEXT("possessable"), PossessableName)
            || Params->TryGetStringField(TEXT("actor"), PossessableName))
        {
            BindingGuid = FindBindingByName(MovieScene, PossessableName);
            if (!BindingGuid.IsValid())
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("No binding matching '%s' on sequence '%s'"),
                        *PossessableName, *Sequence->GetName()));
            }
        }
    }

    // Resolve the audio track on the matching scope. Master scope
    // walks UMovieScene::GetTracks(); binding scope walks FindTrack
    // on the binding GUID, matching the lookup pattern that
    // HandleAddAudioTrack uses.
    UMovieSceneAudioTrack* AudioTrack = nullptr;
    if (BindingGuid.IsValid())
    {
        if (UMovieSceneTrack* Existing = MovieScene->FindTrack(UMovieSceneAudioTrack::StaticClass(), BindingGuid))
        {
            AudioTrack = Cast<UMovieSceneAudioTrack>(Existing);
        }
    }
    else
    {
        for (UMovieSceneTrack* T : MovieScene->GetTracks())
        {
            if (UMovieSceneAudioTrack* Cand = Cast<UMovieSceneAudioTrack>(T))
            {
                AudioTrack = Cand;
                break;
            }
        }
    }
    if (!AudioTrack)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No UMovieSceneAudioTrack found on sequence '%s' (scope: %s)"),
                *Sequence->GetName(),
                BindingGuid.IsValid() ? *BindingGuid.ToString() : TEXT("master")));
    }

    // Index into the track's sections array. Default 0; out-of-range
    // surfaces as a clean error rather than crashing the call.
    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);
    const TArray<UMovieSceneSection*>& Sections = AudioTrack->GetAllSections();
    if (SectionIndex < 0 || SectionIndex >= Sections.Num())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("section_index %d is out of range (track has %d sections)"),
                SectionIndex, Sections.Num()));
    }
    UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(Sections[SectionIndex]);
    if (!AudioSection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Section %d on the resolved audio track is not a UMovieSceneAudioSection"), SectionIndex));
    }

    // Fade durations: caller supplies seconds; we convert to the
    // MovieScene's tick resolution to land on frame boundaries that
    // match the rest of the sequencer storage shape.
    double FadeInSeconds = 0.0;
    Params->TryGetNumberField(TEXT("fade_in_seconds"), FadeInSeconds)
        || Params->TryGetNumberField(TEXT("fade_in"), FadeInSeconds)
        || Params->TryGetNumberField(TEXT("fadein"), FadeInSeconds);
    double FadeOutSeconds = 0.0;
    Params->TryGetNumberField(TEXT("fade_out_seconds"), FadeOutSeconds)
        || Params->TryGetNumberField(TEXT("fade_out"), FadeOutSeconds)
        || Params->TryGetNumberField(TEXT("fadeout"), FadeOutSeconds);
    if (FadeInSeconds < 0.0) FadeInSeconds = 0.0;
    if (FadeOutSeconds < 0.0) FadeOutSeconds = 0.0;
    if (FadeInSeconds <= 0.0 && FadeOutSeconds <= 0.0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("At least one of 'fade_in_seconds' / 'fade_out_seconds' must be positive"));
    }

    // Read the section's bounded range. Falls back to the MovieScene
    // playback range when the section happens to be unbounded, but
    // audio sections always carry a closed range from
    // AddNewSound / AddSection so this is belt-and-braces.
    const TRange<FFrameNumber> SectionRange = AudioSection->GetRange();
    if (!SectionRange.GetLowerBound().IsClosed() || !SectionRange.GetUpperBound().IsClosed())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Resolved audio section has an unbounded range; the fade envelope needs closed bounds"));
    }
    const FFrameNumber StartFrame = SectionRange.GetLowerBoundValue();
    const FFrameNumber EndFrame = SectionRange.GetUpperBoundValue();
    const int32 RangeFrames = EndFrame.Value - StartFrame.Value;
    if (RangeFrames <= 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Resolved audio section has zero / negative duration; cannot land a fade"));
    }

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    int32 FadeInFrames = static_cast<int32>(FadeInSeconds * TickResolution.AsDecimal());
    int32 FadeOutFrames = static_cast<int32>(FadeOutSeconds * TickResolution.AsDecimal());
    // Cap each fade at half the section so they cannot cross over.
    const int32 MaxFadeFrames = RangeFrames / 2;
    if (FadeInFrames > MaxFadeFrames) FadeInFrames = MaxFadeFrames;
    if (FadeOutFrames > MaxFadeFrames) FadeOutFrames = MaxFadeFrames;

    // Find the SoundVolume float channel on the section. The audio
    // section registers SoundVolume at channel index 0 in both editor
    // and runtime builds (see UMovieSceneAudioSection::CacheChannelProxy).
    FMovieSceneChannelProxy& Proxy = AudioSection->GetChannelProxy();
    FMovieSceneFloatChannel* VolumeChannel = Proxy.GetChannel<FMovieSceneFloatChannel>(0);
    if (!VolumeChannel)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Audio section does not expose a SoundVolume float channel; engine layout may have shifted"));
    }

    // Clear the channel's keys so a second call replaces the prior
    // envelope rather than stacking new keys on top. `Reset` keeps
    // the default value (1.0 on a fresh audio section) intact; the
    // four keys we write below override it on the relevant frames.
    int32 PreviousKeyCount = 0;
    {
        TMovieSceneChannelData<FMovieSceneFloatValue> ChannelData = VolumeChannel->GetData();
        PreviousKeyCount = ChannelData.GetTimes().Num();
        VolumeChannel->Reset();
    }
    VolumeChannel->SetDefault(1.0f);

    AudioSection->TryModify();

    int32 KeysWritten = 0;
    if (FadeInFrames > 0)
    {
        VolumeChannel->AddLinearKey(StartFrame, 0.0f);
        VolumeChannel->AddLinearKey(StartFrame + FFrameNumber(FadeInFrames), 1.0f);
        KeysWritten += 2;
    }
    if (FadeOutFrames > 0)
    {
        VolumeChannel->AddLinearKey(EndFrame - FFrameNumber(FadeOutFrames), 1.0f);
        VolumeChannel->AddLinearKey(EndFrame, 0.0f);
        KeysWritten += 2;
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (UPackage* Package = Sequence->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_audio_fade"));
    Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    Result->SetStringField(TEXT("track_class"), AudioTrack->GetClass()->GetName());
    Result->SetStringField(TEXT("track_name"), AudioTrack->GetFName().ToString());
    Result->SetNumberField(TEXT("section_index"), SectionIndex);
    Result->SetStringField(TEXT("section_class"), AudioSection->GetClass()->GetName());
    Result->SetNumberField(TEXT("start_frame"), StartFrame.Value);
    Result->SetNumberField(TEXT("end_frame"), EndFrame.Value);
    Result->SetNumberField(TEXT("range_frames"), RangeFrames);
    Result->SetNumberField(TEXT("fade_in_seconds"), FadeInSeconds);
    Result->SetNumberField(TEXT("fade_out_seconds"), FadeOutSeconds);
    Result->SetNumberField(TEXT("fade_in_frames"), FadeInFrames);
    Result->SetNumberField(TEXT("fade_out_frames"), FadeOutFrames);
    Result->SetNumberField(TEXT("keys_written"), KeysWritten);
    Result->SetNumberField(TEXT("previous_key_count"), PreviousKeyCount);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}
