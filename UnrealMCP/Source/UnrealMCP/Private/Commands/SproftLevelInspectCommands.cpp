#include "Commands/SproftLevelInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> LevelInspect_Vec3ToJson(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    }

    TArray<TSharedPtr<FJsonValue>> LevelInspect_RotToJson(const FRotator& R)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        return Arr;
    }

    FString LevelInspect_MobilityToString(EComponentMobility::Type In)
    {
        switch (In)
        {
            case EComponentMobility::Static:     return TEXT("Static");
            case EComponentMobility::Stationary: return TEXT("Stationary");
            case EComponentMobility::Movable:    return TEXT("Movable");
        }
        return TEXT("Unknown");
    }

    /** Friendly level name. ULevel itself is named "PersistentLevel" / "World";
     *  callers want the owning world's package. */
    FString LevelInspect_LevelLabel(ULevel* Level)
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

    /** Compact per-component record. */
    TSharedPtr<FJsonObject> LevelInspect_ComponentRecord(UActorComponent* Component)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Component)
        {
            return Out;
        }

        Out->SetStringField(TEXT("name"), Component->GetName());
        Out->SetStringField(TEXT("class"), Component->GetClass()->GetName());
        Out->SetBoolField(TEXT("is_active"), Component->IsActive());

        if (USceneComponent* AsScene = Cast<USceneComponent>(Component))
        {
            Out->SetBoolField(TEXT("is_scene_component"), true);
            Out->SetStringField(TEXT("mobility"), LevelInspect_MobilityToString(AsScene->Mobility));
            Out->SetArrayField(TEXT("relative_location"), LevelInspect_Vec3ToJson(AsScene->GetRelativeLocation()));
            if (USceneComponent* Parent = AsScene->GetAttachParent())
            {
                Out->SetStringField(TEXT("attach_parent"), Parent->GetName());
            }
        }
        else
        {
            Out->SetBoolField(TEXT("is_scene_component"), false);
        }

        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FName& Tag : Component->ComponentTags)
        {
            TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Out->SetArrayField(TEXT("tags"), TagArr);
        return Out;
    }
}

FSproftLevelInspectCommands::FSproftLevelInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftLevelInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("level_inspect"))
    {
        return HandleLevelInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown level_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftLevelInspectCommands::HandleLevelInspect(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ClassFilter;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("class"), ClassFilter);
    }
    const FString ClassFilterLower = ClassFilter.ToLower();

    bool bMatchClassSubstring = true;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("match_class_substring"), bMatchClassSubstring);
    }

    FString NamePattern;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("name_pattern"), NamePattern);
    }
    NamePattern = NamePattern.ToLower();

    FString LabelPattern;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("label_pattern"), LabelPattern);
    }
    LabelPattern = LabelPattern.ToLower();

    FString TagFilter;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("tag"), TagFilter);
    }
    const FName TagFName = TagFilter.IsEmpty() ? NAME_None : FName(*TagFilter);

    FString LevelFilter;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("level_filter"), LevelFilter);
    }
    const FString LevelFilterLower = LevelFilter.ToLower();

    bool bIncludeComponents = false;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
    }

    int32 Limit = 512;
    if (Params.IsValid() && Params->HasField(TEXT("limit")))
    {
        Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
        if (Limit <= 0)
        {
            Limit = 512;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("level_name"), World->GetMapName());
    if (UPackage* WorldPackage = World->GetOutermost())
    {
        Result->SetStringField(TEXT("level_path"), WorldPackage->GetName());
    }

    // Per-level summary array.
    TArray<TSharedPtr<FJsonValue>> LevelArr;
    int32 ActorScanned = 0;
    int32 TotalMatches = 0;
    bool bTruncated = false;

    TArray<TSharedPtr<FJsonValue>> ActorArr;

    for (ULevel* Level : World->GetLevels())
    {
        if (!Level)
        {
            continue;
        }

        const FString LevelName = LevelInspect_LevelLabel(Level);

        TSharedPtr<FJsonObject> LevelEntry = MakeShared<FJsonObject>();
        LevelEntry->SetStringField(TEXT("name"), LevelName);
        LevelEntry->SetBoolField(TEXT("is_persistent"), Level == World->PersistentLevel);
        LevelEntry->SetBoolField(TEXT("is_visible"), Level->bIsVisible);
        LevelEntry->SetNumberField(TEXT("actor_count"), Level->Actors.Num());
        LevelArr.Add(MakeShared<FJsonValueObject>(LevelEntry));

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
            ++ActorScanned;

            // Class filter.
            if (!ClassFilter.IsEmpty())
            {
                const FString ClassName = Actor->GetClass()->GetName().ToLower();
                const FString ClassPath = Actor->GetClass()->GetPathName().ToLower();
                const bool bMatches = bMatchClassSubstring
                    ? (ClassName.Contains(ClassFilterLower) || ClassPath.Contains(ClassFilterLower))
                    : (ClassName == ClassFilterLower);
                if (!bMatches)
                {
                    continue;
                }
            }

            if (!NamePattern.IsEmpty() && !Actor->GetName().ToLower().Contains(NamePattern))
            {
                continue;
            }

            if (!LabelPattern.IsEmpty() && !Actor->GetActorLabel().ToLower().Contains(LabelPattern))
            {
                continue;
            }

            if (TagFName != NAME_None && !Actor->Tags.Contains(TagFName))
            {
                continue;
            }

            ++TotalMatches;
            if (ActorArr.Num() >= Limit)
            {
                bTruncated = true;
                continue;
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Actor->GetName());
            Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
            Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
            Entry->SetStringField(TEXT("class_path"), Actor->GetClass()->GetPathName());
            Entry->SetStringField(TEXT("level"), LevelName);

            const FName Folder = Actor->GetFolderPath();
            if (Folder != NAME_None)
            {
                Entry->SetStringField(TEXT("folder_path"), Folder.ToString());
            }

            Entry->SetArrayField(TEXT("location"), LevelInspect_Vec3ToJson(Actor->GetActorLocation()));
            Entry->SetArrayField(TEXT("rotation"), LevelInspect_RotToJson(Actor->GetActorRotation()));
            Entry->SetArrayField(TEXT("scale"),    LevelInspect_Vec3ToJson(Actor->GetActorScale3D()));

            TArray<TSharedPtr<FJsonValue>> TagArr;
            for (const FName& Tag : Actor->Tags)
            {
                TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
            }
            Entry->SetArrayField(TEXT("tags"), TagArr);

            Entry->SetBoolField(TEXT("hidden_in_game"), Actor->IsHidden());
            Entry->SetBoolField(TEXT("hidden_in_editor"), Actor->IsTemporarilyHiddenInEditor());

            if (USceneComponent* Root = Actor->GetRootComponent())
            {
                Entry->SetStringField(TEXT("root_component_class"), Root->GetClass()->GetName());
                Entry->SetStringField(TEXT("mobility"), LevelInspect_MobilityToString(Root->Mobility));
            }

            if (bIncludeComponents)
            {
                TArray<UActorComponent*> Components;
                Actor->GetComponents(Components);

                TArray<TSharedPtr<FJsonValue>> CompArr;
                for (UActorComponent* Comp : Components)
                {
                    CompArr.Add(MakeShared<FJsonValueObject>(LevelInspect_ComponentRecord(Comp)));
                }
                Entry->SetArrayField(TEXT("components"), CompArr);
                Entry->SetNumberField(TEXT("component_count"), Components.Num());
            }

            ActorArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    Result->SetArrayField(TEXT("levels"), LevelArr);
    Result->SetArrayField(TEXT("actors"), ActorArr);
    Result->SetNumberField(TEXT("actors_scanned"), ActorScanned);
    Result->SetNumberField(TEXT("total_matches"), TotalMatches);
    Result->SetNumberField(TEXT("returned"), ActorArr.Num());
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetNumberField(TEXT("limit"), Limit);
    return Result;
}
