#include "Commands/SproftFoliageInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "FoliageType.h"
#include "FoliageType_Actor.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Math/RandomStream.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> Vec3ToJson(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    }

    /** Friendly level name; mirrors the SproftLevelInspect / SproftLandscapeInspect
     *  level-naming convention so cross-tool callers see one identifier. */
    FString LevelLabel(ULevel* Level)
    {
        if (!Level)
        {
            return FString();
        }
        if (UWorld* OwningWorld = Cast<UWorld>(Level->GetOuter()))
        {
            return OwningWorld->GetName();
        }
        return Level->GetName();
    }

    /** Map a UFoliageType's source pointer to a {kind, path} tuple. The two
     *  shipping subclasses are UFoliageType_InstancedStaticMesh (-> UStaticMesh)
     *  and UFoliageType_Actor (-> UClass). Anything else falls through to
     *  "unknown" with the GetSource() pointer's path so a caller still has
     *  something to look up. */
    void DescribeSource(const UFoliageType* Type, FString& OutKind, FString& OutPath)
    {
        OutKind = TEXT("unknown");
        OutPath = FString();
        if (!Type)
        {
            return;
        }

        if (const UFoliageType_InstancedStaticMesh* AsISM =
                Cast<UFoliageType_InstancedStaticMesh>(Type))
        {
            OutKind = TEXT("static_mesh");
            if (UStaticMesh* Mesh = AsISM->GetStaticMesh())
            {
                OutPath = Mesh->GetPathName();
            }
            return;
        }

        if (const UFoliageType_Actor* AsActor = Cast<UFoliageType_Actor>(Type))
        {
            OutKind = TEXT("actor");
            if (UClass* ActorClass = AsActor->ActorClass)
            {
                OutPath = ActorClass->GetPathName();
            }
            return;
        }

        // Fallback: ask the type for its source object and stringify it.
        if (UObject* Source = Type->GetSource())
        {
            OutPath = Source->GetPathName();
        }
    }

    /** Pick `Count` distinct instance indices into [0, Total) using a seeded
     *  RNG. We do not need cryptographic shuffling, just deterministic
     *  per-call output. The reservoir is a simple Fisher-Yates draw on a
     *  fresh index list. */
    TArray<int32> SampleIndices(int32 Total, int32 Count, int32 Seed)
    {
        TArray<int32> Indices;
        if (Total <= 0 || Count <= 0)
        {
            return Indices;
        }

        const int32 PickCount = FMath::Min(Count, Total);
        Indices.Reserve(Total);
        for (int32 I = 0; I < Total; ++I)
        {
            Indices.Add(I);
        }

        FRandomStream RNG(Seed);
        for (int32 I = 0; I < PickCount; ++I)
        {
            // RandRange is inclusive on both ends.
            const int32 J = RNG.RandRange(I, Total - 1);
            Indices.Swap(I, J);
        }
        Indices.SetNum(PickCount);
        Indices.Sort();
        return Indices;
    }
}

FSproftFoliageInspectCommands::FSproftFoliageInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftFoliageInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("foliage_inspect"))
    {
        return HandleFoliageInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown foliage_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftFoliageInspectCommands::HandleFoliageInspect(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString NamePattern;
    FString LevelFilter;
    int32   SampleLocations = 0;
    int32   SampleSeed      = 0;

    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("name_pattern"), NamePattern);
        Params->TryGetStringField(TEXT("level_filter"), LevelFilter);
        if (Params->HasField(TEXT("sample_locations")))
        {
            SampleLocations = static_cast<int32>(Params->GetNumberField(TEXT("sample_locations")));
            if (SampleLocations < 0)
            {
                SampleLocations = 0;
            }
            // Cap to keep responses bounded; a level with 100 foliage types
            // and 1000 samples each would balloon into 100k locations.
            if (SampleLocations > 1024)
            {
                SampleLocations = 1024;
            }
        }
        if (Params->HasField(TEXT("sample_seed")))
        {
            SampleSeed = static_cast<int32>(Params->GetNumberField(TEXT("sample_seed")));
        }
    }
    const FString NamePatternLower = NamePattern.ToLower();
    const FString LevelFilterLower = LevelFilter.ToLower();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("level_name"), World->GetMapName());
    if (UPackage* WorldPackage = World->GetOutermost())
    {
        Result->SetStringField(TEXT("level_path"), WorldPackage->GetName());
    }

    TArray<TSharedPtr<FJsonValue>> ActorArr;
    int32 ScannedActors = 0;
    int32 MatchedActors = 0;
    int32 GlobalInstanceCount = 0;

    for (ULevel* Level : World->GetLevels())
    {
        if (!Level)
        {
            continue;
        }
        const FString LevelName = LevelLabel(Level);
        if (!LevelFilter.IsEmpty() && !LevelName.ToLower().Contains(LevelFilterLower))
        {
            continue;
        }

        for (AActor* Actor : Level->Actors)
        {
            if (!Actor)
            {
                continue;
            }
            AInstancedFoliageActor* IFA = Cast<AInstancedFoliageActor>(Actor);
            if (!IFA)
            {
                continue;
            }
            ++ScannedActors;

            if (!NamePattern.IsEmpty())
            {
                const bool bNameHit  = IFA->GetName().ToLower().Contains(NamePatternLower);
                const bool bLabelHit = IFA->GetActorLabel().ToLower().Contains(NamePatternLower);
                if (!bNameHit && !bLabelHit)
                {
                    continue;
                }
            }
            ++MatchedActors;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), IFA->GetName());
            Entry->SetStringField(TEXT("label"), IFA->GetActorLabel());
            Entry->SetStringField(TEXT("class"), IFA->GetClass()->GetName());
            Entry->SetStringField(TEXT("class_path"), IFA->GetClass()->GetPathName());
            Entry->SetStringField(TEXT("level"), LevelName);
            Entry->SetArrayField(TEXT("location"), Vec3ToJson(IFA->GetActorLocation()));

            // GetFoliageInfos returns const TMap<UFoliageType*, TUniqueObj<FFoliageInfo>>&.
            const TMap<UFoliageType*, TUniqueObj<FFoliageInfo>>& Infos = IFA->GetFoliageInfos();

            TArray<TSharedPtr<FJsonValue>> TypeArr;
            int32 IFAInstanceCount = 0;

            for (const TPair<UFoliageType*, TUniqueObj<FFoliageInfo>>& Pair : Infos)
            {
                UFoliageType* Type = Pair.Key;
                if (!Type)
                {
                    continue;
                }
                const FFoliageInfo& Info = Pair.Value.Get();

                TSharedPtr<FJsonObject> TypeEntry = MakeShared<FJsonObject>();
                TypeEntry->SetStringField(TEXT("foliage_type_name"), Type->GetName());
                TypeEntry->SetStringField(TEXT("foliage_type_path"), Type->GetPathName());
                TypeEntry->SetStringField(TEXT("foliage_type_class"), Type->GetClass()->GetName());

                FString SourceKind, SourcePath;
                DescribeSource(Type, SourceKind, SourcePath);
                TypeEntry->SetStringField(TEXT("source_kind"), SourceKind);
                if (!SourcePath.IsEmpty())
                {
                    TypeEntry->SetStringField(TEXT("source_path"), SourcePath);
                }

                TypeEntry->SetNumberField(TEXT("density"), Type->Density);
                TypeEntry->SetNumberField(TEXT("density_adjustment_factor"),
                    Type->DensityAdjustmentFactor);
                TypeEntry->SetNumberField(TEXT("radius"), Type->Radius);

                TypeEntry->SetNumberField(TEXT("scale_x_min"), Type->ScaleX.Min);
                TypeEntry->SetNumberField(TEXT("scale_x_max"), Type->ScaleX.Max);
                TypeEntry->SetNumberField(TEXT("scale_y_min"), Type->ScaleY.Min);
                TypeEntry->SetNumberField(TEXT("scale_y_max"), Type->ScaleY.Max);
                TypeEntry->SetNumberField(TEXT("scale_z_min"), Type->ScaleZ.Min);
                TypeEntry->SetNumberField(TEXT("scale_z_max"), Type->ScaleZ.Max);

                int32 InstanceCount = 0;
                int32 PlacedCount = 0;
#if WITH_EDITORONLY_DATA
                InstanceCount = Info.Instances.Num();
#endif
#if WITH_EDITOR
                PlacedCount = Info.GetPlacedInstanceCount();
#endif
                TypeEntry->SetNumberField(TEXT("instance_count"), InstanceCount);
                TypeEntry->SetNumberField(TEXT("placed_instance_count"), PlacedCount);
                IFAInstanceCount += InstanceCount;

#if WITH_EDITOR
                const FBox Bounds = Info.GetApproximatedInstanceBounds();
                if (Bounds.IsValid)
                {
                    TypeEntry->SetArrayField(TEXT("approximated_bounds_min"),
                        Vec3ToJson(Bounds.Min));
                    TypeEntry->SetArrayField(TEXT("approximated_bounds_max"),
                        Vec3ToJson(Bounds.Max));
                    TypeEntry->SetArrayField(TEXT("approximated_bounds_size"),
                        Vec3ToJson(Bounds.GetSize()));
                }
#endif

#if WITH_EDITORONLY_DATA
                if (SampleLocations > 0 && InstanceCount > 0)
                {
                    const TArray<int32> Picked =
                        SampleIndices(InstanceCount, SampleLocations, SampleSeed);
                    TArray<TSharedPtr<FJsonValue>> Samples;
                    for (int32 Idx : Picked)
                    {
                        TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
                        Sample->SetNumberField(TEXT("index"), Idx);
                        Sample->SetArrayField(TEXT("location"),
                            Vec3ToJson(Info.Instances[Idx].Location));
                        Samples.Add(MakeShared<FJsonValueObject>(Sample));
                    }
                    TypeEntry->SetArrayField(TEXT("sample_locations"), Samples);
                }
#endif

                TypeArr.Add(MakeShared<FJsonValueObject>(TypeEntry));
            }

            Entry->SetArrayField(TEXT("foliage_types"), TypeArr);
            Entry->SetNumberField(TEXT("foliage_type_count"), TypeArr.Num());
            Entry->SetNumberField(TEXT("total_instance_count"), IFAInstanceCount);
            GlobalInstanceCount += IFAInstanceCount;

            ActorArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    Result->SetArrayField(TEXT("foliage_actors"), ActorArr);
    Result->SetNumberField(TEXT("foliage_actor_count"), ActorArr.Num());
    Result->SetNumberField(TEXT("scanned_actors"), ScannedActors);
    Result->SetNumberField(TEXT("matched_actors"), MatchedActors);
    Result->SetNumberField(TEXT("global_instance_count"), GlobalInstanceCount);
    return Result;
}
