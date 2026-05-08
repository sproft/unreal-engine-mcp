#include "Commands/SproftSceneQueryCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Class.h"

namespace
{
    /** Best-effort class lookup for the user-facing `class` filter. Walks every
     *  loaded UClass; cheap because the editor process keeps a manageable set
     *  of classes alive at any one moment. */
    UClass* ResolveActorClass(const FString& InClassName)
    {
        if (InClassName.IsEmpty())
        {
            return AActor::StaticClass();
        }

        // Full path first, then short-name probe.
        if (InClassName.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<AActor>(nullptr, *InClassName))
            {
                return Loaded;
            }
            const FString WithSuffix = InClassName + TEXT("_C");
            if (UClass* LoadedSuffix = LoadClass<AActor>(nullptr, *WithSuffix))
            {
                return LoadedSuffix;
            }
        }

        if (UClass* Found = FindObject<UClass>(nullptr, *InClassName))
        {
            if (Found->IsChildOf(AActor::StaticClass()))
            {
                return Found;
            }
        }

        const TArray<FString> Namespaces = {
            TEXT("/Script/Engine.%s"),
            TEXT("/Script/EnhancedInput.%s"),
        };
        for (const FString& Format : Namespaces)
        {
            const FString Path = FString::Printf(*Format, *InClassName);
            if (UClass* Loaded = LoadClass<AActor>(nullptr, *Path))
            {
                return Loaded;
            }
        }

        return nullptr;
    }

    /** Build the actor record exposed in the response. */
    TSharedPtr<FJsonObject> ActorRecord(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("name"), Actor->GetName());
        Out->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Out->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        Out->SetStringField(TEXT("class_path"), Actor->GetClass()->GetPathName());

        const FVector Location = Actor->GetActorLocation();
        TArray<TSharedPtr<FJsonValue>> LocArr;
        LocArr.Add(MakeShared<FJsonValueNumber>(Location.X));
        LocArr.Add(MakeShared<FJsonValueNumber>(Location.Y));
        LocArr.Add(MakeShared<FJsonValueNumber>(Location.Z));
        Out->SetArrayField(TEXT("location"), LocArr);

        const FRotator Rotation = Actor->GetActorRotation();
        TArray<TSharedPtr<FJsonValue>> RotArr;
        RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
        RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
        RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
        Out->SetArrayField(TEXT("rotation"), RotArr);

        const FVector Scale = Actor->GetActorScale3D();
        TArray<TSharedPtr<FJsonValue>> ScaleArr;
        ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.X));
        ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.Y));
        ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.Z));
        Out->SetArrayField(TEXT("scale"), ScaleArr);

        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FName& Tag : Actor->Tags)
        {
            TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Out->SetArrayField(TEXT("tags"), TagArr);

        Out->SetBoolField(TEXT("hidden_in_game"), Actor->IsHidden());
        Out->SetBoolField(TEXT("hidden_in_editor"), Actor->IsTemporarilyHiddenInEditor());

        if (USceneComponent* Root = Actor->GetRootComponent())
        {
            Out->SetStringField(TEXT("root_component_class"), Root->GetClass()->GetName());
            Out->SetStringField(TEXT("mobility"),
                Root->Mobility == EComponentMobility::Static    ? TEXT("Static")    :
                Root->Mobility == EComponentMobility::Stationary ? TEXT("Stationary") :
                Root->Mobility == EComponentMobility::Movable   ? TEXT("Movable")   :
                                                                  TEXT("Unknown"));
        }

        return Out;
    }
}

FSproftSceneQueryCommands::FSproftSceneQueryCommands()
{
}

TSharedPtr<FJsonObject> FSproftSceneQueryCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("scene_query"))
    {
        return HandleSceneQuery(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown scene_query command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftSceneQueryCommands::HandleSceneQuery(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString ClassFilter;
    Params->TryGetStringField(TEXT("class"), ClassFilter);

    bool bMatchClassSubstring = true;
    Params->TryGetBoolField(TEXT("match_class_substring"), bMatchClassSubstring);

    FString NamePattern;
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);
    NamePattern = NamePattern.ToLower();

    FString LabelPattern;
    Params->TryGetStringField(TEXT("label_pattern"), LabelPattern);
    LabelPattern = LabelPattern.ToLower();

    FString TagFilter;
    Params->TryGetStringField(TEXT("tag"), TagFilter);

    bool bHasSpatialFilter = false;
    FVector Center = FVector::ZeroVector;
    double Radius = 0.0;
    if (Params->HasField(TEXT("center")) && Params->HasField(TEXT("radius")))
    {
        Center = FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("center"));
        Radius = Params->GetNumberField(TEXT("radius"));
        bHasSpatialFilter = (Radius > 0.0);
    }

    int32 Limit = 256;
    if (Params->HasField(TEXT("limit")))
    {
        Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
        if (Limit <= 0)
        {
            Limit = 256;
        }
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    // Resolve class filter. If exact-class matching was requested but the
    // class did not load, the answer is unambiguously the empty set.
    UClass* SearchClass = AActor::StaticClass();
    bool bClassFilterUsesSearchClass = false;
    if (!ClassFilter.IsEmpty() && !bMatchClassSubstring)
    {
        UClass* Resolved = ResolveActorClass(ClassFilter);
        if (!Resolved)
        {
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());
            ResultObj->SetNumberField(TEXT("total_matches"), 0);
            ResultObj->SetNumberField(TEXT("returned"), 0);
            ResultObj->SetBoolField(TEXT("truncated"), false);
            ResultObj->SetStringField(TEXT("note"),
                FString::Printf(TEXT("Class '%s' could not be resolved; returning empty set."), *ClassFilter));
            return ResultObj;
        }
        SearchClass = Resolved;
        bClassFilterUsesSearchClass = true;
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, SearchClass, AllActors);

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 TotalMatches = 0;
    bool bTruncated = false;
    const FString ClassFilterLower = ClassFilter.ToLower();

    for (AActor* Actor : AllActors)
    {
        if (!Actor)
        {
            continue;
        }

        // Substring class match. Skipped when GetAllActorsOfClass already
        // narrowed the world walk to the exact class.
        if (!bClassFilterUsesSearchClass && !ClassFilter.IsEmpty())
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

        if (!TagFilter.IsEmpty())
        {
            const FName TagName(*TagFilter);
            if (!Actor->Tags.Contains(TagName))
            {
                continue;
            }
        }

        if (bHasSpatialFilter)
        {
            const double DistSq = FVector::DistSquared(Actor->GetActorLocation(), Center);
            if (DistSq > Radius * Radius)
            {
                continue;
            }
        }

        ++TotalMatches;
        if (Out.Num() < Limit)
        {
            Out.Add(MakeShared<FJsonValueObject>(ActorRecord(Actor)));
        }
        else
        {
            bTruncated = true;
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), Out);
    ResultObj->SetNumberField(TEXT("total_matches"), TotalMatches);
    ResultObj->SetNumberField(TEXT("returned"), Out.Num());
    ResultObj->SetBoolField(TEXT("truncated"), bTruncated);
    ResultObj->SetNumberField(TEXT("limit"), Limit);
    return ResultObj;
}
