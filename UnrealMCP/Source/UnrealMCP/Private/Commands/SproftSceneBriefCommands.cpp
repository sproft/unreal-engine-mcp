#include "Commands/SproftSceneBriefCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "K2Node_Event.h"
#include "Kismet/GameplayStatics.h"

namespace
{
    /** Build a [x, y, z] number array. */
    TArray<TSharedPtr<FJsonValue>> Vec3ToJson(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    }

    /** Probe whether the level blueprint declares any user-authored event nodes.
     *  We treat any K2Node_Event in any of the level blueprint's graphs as "yes". */
    bool LevelBlueprintHasEvents(UWorld* World)
    {
        if (!World)
        {
            return false;
        }
        ULevel* Persistent = World->PersistentLevel;
        if (!Persistent)
        {
            return false;
        }
        // Pass bDontCreate=true so reading the brief never mutates the level.
        ULevelScriptBlueprint* LevelBP = Persistent->GetLevelScriptBlueprint(true);
        if (!LevelBP)
        {
            return false;
        }

        TArray<UEdGraph*> AllGraphs;
        LevelBP->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->IsA<UK2Node_Event>())
                {
                    return true;
                }
            }
        }
        return false;
    }
}

FSproftSceneBriefCommands::FSproftSceneBriefCommands()
{
}

TSharedPtr<FJsonObject> FSproftSceneBriefCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("scene_brief"))
    {
        return HandleSceneBrief(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown scene_brief command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftSceneBriefCommands::HandleSceneBrief(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    // Class-count cap. The map is small in normal levels, but the user may be
    // running this on a content sandbox. Default 256 keeps responses bounded.
    int32 MaxClassCounts = 256;
    if (Params.IsValid() && Params->HasField(TEXT("max_class_counts")))
    {
        MaxClassCounts = static_cast<int32>(Params->GetNumberField(TEXT("max_class_counts")));
        if (MaxClassCounts <= 0)
        {
            MaxClassCounts = 256;
        }
    }

    int32 MaxNotableActors = 16;
    if (Params.IsValid() && Params->HasField(TEXT("max_notable_actors")))
    {
        MaxNotableActors = static_cast<int32>(Params->GetNumberField(TEXT("max_notable_actors")));
        if (MaxNotableActors < 0)
        {
            MaxNotableActors = 0;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Persistent level identity. GetMapName() returns the editor's "current map"
    // string, which is the right designer-facing handle.
    Result->SetStringField(TEXT("level_name"), World->GetMapName());
    if (UPackage* WorldPackage = World->GetOutermost())
    {
        Result->SetStringField(TEXT("level_path"), WorldPackage->GetName());
    }

    // Sublevels (only the streaming-level entries, not any unbound levels).
    TArray<TSharedPtr<FJsonValue>> SublevelArr;
    for (ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (!Streaming)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Streaming->GetName());
        Entry->SetStringField(TEXT("package"), Streaming->GetWorldAssetPackageName());
        Entry->SetStringField(TEXT("class"), Streaming->GetClass()->GetName());
        Entry->SetBoolField(TEXT("should_be_loaded"), Streaming->ShouldBeLoaded());
        Entry->SetBoolField(TEXT("should_be_visible"), Streaming->ShouldBeVisible());
        SublevelArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("sublevels"), SublevelArr);
    Result->SetNumberField(TEXT("sublevel_count"), SublevelArr.Num());

    // World walk: counts per class, tag union, world bounds, notable actors.
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);

    TMap<FString, int32> ClassCountMap;
    TSet<FString> Tags;
    TArray<TSharedPtr<FJsonValue>> NotableArr;

    bool bHasBounds = false;
    FBox WorldBounds(ForceInit);

    for (AActor* Actor : AllActors)
    {
        if (!Actor)
        {
            continue;
        }
        const FString ClassName = Actor->GetClass()->GetName();
        ClassCountMap.FindOrAdd(ClassName) += 1;

        for (const FName& Tag : Actor->Tags)
        {
            if (!Tag.IsNone())
            {
                Tags.Add(Tag.ToString());
            }
        }

        // Bounds union. GetActorBounds with bOnlyCollidingComponents=false picks
        // up visual-only actors too, which is what designers expect from a
        // "what's roughly where" summary.
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent, false);
        if (!Extent.IsNearlyZero())
        {
            const FBox ActorBox = FBox::BuildAABB(Origin, Extent);
            WorldBounds += ActorBox;
            bHasBounds = true;
        }

        // Notable actors: keep a small list of common designer landmarks.
        if (NotableArr.Num() < MaxNotableActors)
        {
            const bool bIsNotable =
                Actor->IsA(APlayerStart::StaticClass()) ||
                Actor->IsA(ADirectionalLight::StaticClass()) ||
                Actor->IsA(APostProcessVolume::StaticClass());
            if (bIsNotable)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Actor->GetName());
                Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
                Entry->SetStringField(TEXT("class"), ClassName);
                Entry->SetArrayField(TEXT("location"), Vec3ToJson(Actor->GetActorLocation()));
                NotableArr.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    Result->SetNumberField(TEXT("actor_count"), AllActors.Num());

    // class_counts: bounded, sorted by descending count for predictability.
    {
        TArray<TPair<FString, int32>> Sorted;
        Sorted.Reserve(ClassCountMap.Num());
        for (const auto& Pair : ClassCountMap)
        {
            Sorted.Add(TPair<FString, int32>(Pair.Key, Pair.Value));
        }
        Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
        {
            if (A.Value != B.Value)
            {
                return A.Value > B.Value;
            }
            return A.Key < B.Key;
        });

        TSharedPtr<FJsonObject> ClassCounts = MakeShared<FJsonObject>();
        const int32 EmitCount = FMath::Min(Sorted.Num(), MaxClassCounts);
        for (int32 i = 0; i < EmitCount; ++i)
        {
            ClassCounts->SetNumberField(Sorted[i].Key, Sorted[i].Value);
        }
        Result->SetObjectField(TEXT("class_counts"), ClassCounts);
        Result->SetBoolField(TEXT("class_counts_truncated"), Sorted.Num() > EmitCount);
        Result->SetNumberField(TEXT("unique_classes"), Sorted.Num());
    }

    // Tags in use.
    {
        TArray<FString> TagList = Tags.Array();
        TagList.Sort();
        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FString& Tag : TagList)
        {
            TagArr.Add(MakeShared<FJsonValueString>(Tag));
        }
        Result->SetArrayField(TEXT("tags_in_use"), TagArr);
    }

    // World bounds union.
    if (bHasBounds && WorldBounds.IsValid != 0)
    {
        TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
        BoundsObj->SetArrayField(TEXT("min"),    Vec3ToJson(WorldBounds.Min));
        BoundsObj->SetArrayField(TEXT("max"),    Vec3ToJson(WorldBounds.Max));
        BoundsObj->SetArrayField(TEXT("center"), Vec3ToJson(WorldBounds.GetCenter()));
        BoundsObj->SetArrayField(TEXT("extent"), Vec3ToJson(WorldBounds.GetExtent()));
        Result->SetObjectField(TEXT("bounds"), BoundsObj);
    }
    else
    {
        Result->SetField(TEXT("bounds"), MakeShared<FJsonValueNull>());
    }

    // GameMode + default pawn. Read through WorldSettings so we honour the
    // per-map override; fall back to the project's default GameMode when the
    // override is empty.
    AWorldSettings* WorldSettings = World->GetWorldSettings(false, false);
    if (WorldSettings)
    {
        UClass* GameModeClass = WorldSettings->DefaultGameMode;
        if (GameModeClass)
        {
            Result->SetStringField(TEXT("game_mode_class"), GameModeClass->GetPathName());

            if (AGameModeBase* GMCDO = GameModeClass->GetDefaultObject<AGameModeBase>())
            {
                if (UClass* Pawn = GMCDO->DefaultPawnClass)
                {
                    Result->SetStringField(TEXT("default_pawn_class"), Pawn->GetPathName());
                }
            }
        }
    }

    Result->SetBoolField(TEXT("level_blueprint_has_events"), LevelBlueprintHasEvents(World));

    Result->SetArrayField(TEXT("notable_actors"), NotableArr);

    return Result;
}
