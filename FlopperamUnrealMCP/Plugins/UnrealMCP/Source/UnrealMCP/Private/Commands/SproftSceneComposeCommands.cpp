#include "Commands/SproftSceneComposeCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Resolve a class name or path into an AActor subclass. Tries full paths
     *  first, then a short-name probe in loaded classes, then a small set of
     *  Engine-namespace fallbacks. Returns nullptr on failure. */
    UClass* ResolveActorClass(const FString& InClassPath)
    {
        if (InClassPath.IsEmpty())
        {
            return nullptr;
        }

        // Full object path of the form `/Script/Module.ClassName` or
        // `/Game/Foo/MyActor.MyActor[_C]`.
        if (InClassPath.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<AActor>(nullptr, *InClassPath))
            {
                return Loaded;
            }
            const FString WithSuffix = InClassPath + TEXT("_C");
            if (UClass* LoadedSuffix = LoadClass<AActor>(nullptr, *WithSuffix))
            {
                return LoadedSuffix;
            }
            return nullptr;
        }

        TArray<FString> Candidates;
        Candidates.Add(InClassPath);
        if (!InClassPath.StartsWith(TEXT("A")))
        {
            Candidates.Add(TEXT("A") + InClassPath);
        }

        for (const FString& Candidate : Candidates)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Candidate))
            {
                if (Found->IsChildOf(AActor::StaticClass()))
                {
                    return Found;
                }
            }
        }

        // UE 5.7 tightened FString::Printf format-string handling;
        // build the path through string concatenation instead.
        static const TCHAR* const Namespaces[] = {
            TEXT("/Script/Engine."),
            TEXT("/Script/EnhancedInput."),
        };
        for (const FString& Candidate : Candidates)
        {
            for (const TCHAR* Namespace : Namespaces)
            {
                const FString Path = FString(Namespace) + Candidate;
                if (UClass* Loaded = LoadClass<AActor>(nullptr, *Path))
                {
                    return Loaded;
                }
            }
        }

        return nullptr;
    }

    /** Resolve an actor by FName, then by case-insensitive Outliner label. */
    AActor* ResolveActor(UWorld* World, const FString& InQuery)
    {
        if (!World || InQuery.IsEmpty())
        {
            return nullptr;
        }
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);

        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == InQuery)
            {
                return Actor;
            }
        }
        const FString QueryLower = InQuery.ToLower();
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetActorLabel().ToLower() == QueryLower)
            {
                return Actor;
            }
        }
        return nullptr;
    }

    /** Convert an FJsonValue into a textual form FProperty::ImportText accepts. */
    FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        switch (Value->Type)
        {
            case EJson::String:
                return Value->AsString();
            case EJson::Number:
                return LexToString(Value->AsNumber());
            case EJson::Boolean:
                return Value->AsBool() ? TEXT("true") : TEXT("false");
            case EJson::Null:
                return TEXT("None");
            default:
            {
                FString Buffer;
                TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Buffer);
                FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
                return Buffer;
            }
        }
    }

    /** Apply a flat property dict to an actor through FProperty::ImportText.
     *  Records each successful and skipped key in OutApplied / OutSkipped. */
    void ApplyPropertyDict(
        AActor* Actor,
        const TSharedPtr<FJsonObject>& Props,
        TArray<TSharedPtr<FJsonValue>>& OutApplied,
        TArray<TSharedPtr<FJsonValue>>& OutSkipped)
    {
        if (!Actor || !Props.IsValid())
        {
            return;
        }
        FOutputDeviceNull NullDevice;
        for (const auto& Pair : Props->Values)
        {
            const FString& PropName = Pair.Key;
            const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

            FProperty* Prop = FindFProperty<FProperty>(Actor->GetClass(), *PropName);
            if (!Prop)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }

            const FString TextValue = JsonValueToImportText(JsonVal);
            const TCHAR* TextPtr = *TextValue;
            const TCHAR* Result = Prop->ImportText_InContainer(
                TextPtr, Actor, Actor, PPF_None, &NullDevice);
            if (Result == nullptr)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), PropName);
                Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
                Skip->SetStringField(TEXT("attempted_value"), TextValue);
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }

            TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
            Applied->SetStringField(TEXT("name"), PropName);
            Applied->SetStringField(TEXT("type"), Prop->GetCPPType());
            OutApplied.Add(MakeShared<FJsonValueObject>(Applied));
        }
    }

    /** Read the optional tags array as FName values. */
    void ReadTags(const TSharedPtr<FJsonObject>& Params, TArray<FName>& OutTags, bool& bOutHasTags)
    {
        bOutHasTags = false;
        const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
        if (Params->TryGetArrayField(TEXT("tags"), TagsArr) && TagsArr)
        {
            bOutHasTags = true;
            for (const TSharedPtr<FJsonValue>& V : *TagsArr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    OutTags.Add(FName(*V->AsString()));
                }
            }
        }
    }

    /** Build the small actor record returned in spawn / modify responses. */
    TSharedPtr<FJsonObject> ActorSummary(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("name"), Actor->GetName());
        Out->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Out->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        Out->SetStringField(TEXT("class_path"), Actor->GetClass()->GetPathName());

        const FVector L = Actor->GetActorLocation();
        TArray<TSharedPtr<FJsonValue>> LArr;
        LArr.Add(MakeShared<FJsonValueNumber>(L.X));
        LArr.Add(MakeShared<FJsonValueNumber>(L.Y));
        LArr.Add(MakeShared<FJsonValueNumber>(L.Z));
        Out->SetArrayField(TEXT("location"), LArr);

        const FRotator R = Actor->GetActorRotation();
        TArray<TSharedPtr<FJsonValue>> RArr;
        RArr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        RArr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        RArr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        Out->SetArrayField(TEXT("rotation"), RArr);

        const FVector S = Actor->GetActorScale3D();
        TArray<TSharedPtr<FJsonValue>> SArr;
        SArr.Add(MakeShared<FJsonValueNumber>(S.X));
        SArr.Add(MakeShared<FJsonValueNumber>(S.Y));
        SArr.Add(MakeShared<FJsonValueNumber>(S.Z));
        Out->SetArrayField(TEXT("scale"), SArr);

        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FName& Tag : Actor->Tags)
        {
            TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Out->SetArrayField(TEXT("tags"), TagArr);

        return Out;
    }
}

FSproftSceneComposeCommands::FSproftSceneComposeCommands()
{
}

TSharedPtr<FJsonObject> FSproftSceneComposeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("scene_compose"))
    {
        return HandleSceneCompose(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown scene_compose command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftSceneComposeCommands::HandleSceneCompose(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }
    Operation = Operation.ToLower();

    if (Operation == TEXT("spawn"))
    {
        return SpawnActor(Params);
    }
    if (Operation == TEXT("modify"))
    {
        return ModifyActor(Params);
    }
    if (Operation == TEXT("delete"))
    {
        return DeleteActor(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported scene_compose operation '%s'. Supported: spawn, modify, delete"), *Operation));
}

TSharedPtr<FJsonObject> FSproftSceneComposeCommands::SpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassPath;
    if (!Params->TryGetStringField(TEXT("class"), ClassPath)
        && !Params->TryGetStringField(TEXT("actor_class"), ClassPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class' parameter"));
    }

    UClass* SpawnClass = ResolveActorClass(ClassPath);
    if (!SpawnClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve actor class '%s'"), *ClassPath));
    }
    if (SpawnClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract and cannot be spawned"), *SpawnClass->GetPathName()));
    }
    if (!SpawnClass->IsChildOf(AActor::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is not an AActor subclass"), *SpawnClass->GetPathName()));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Scale(1.0f, 1.0f, 1.0f);
    if (Params->HasField(TEXT("location")))
    {
        Location = FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FEpicUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    FActorSpawnParameters SpawnParams;

    // Optional preferred FName. Reject collisions outright; SpawnActor would
    // silently rename and that hides bugs in caller code.
    FString PreferredName;
    if (Params->TryGetStringField(TEXT("name"), PreferredName) && !PreferredName.IsEmpty())
    {
        TArray<AActor*> Existing;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), Existing);
        for (AActor* Other : Existing)
        {
            if (Other && Other->GetName() == PreferredName)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Actor with name '%s' already exists"), *PreferredName));
            }
        }
        SpawnParams.Name = *PreferredName;
    }

    AActor* NewActor = World->SpawnActor<AActor>(SpawnClass, Location, Rotation, SpawnParams);
    if (!NewActor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to spawn actor of class '%s'"), *SpawnClass->GetPathName()));
    }

    // Apply scale via SetActorTransform since SpawnActor only takes location and rotation.
    FTransform Transform = NewActor->GetTransform();
    Transform.SetScale3D(Scale);
    NewActor->SetActorTransform(Transform);

    // Optional Outliner label.
    FString Label;
    if (Params->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
    {
        NewActor->SetActorLabel(Label);
    }

    // Optional tags array.
    TArray<FName> Tags;
    bool bHasTags = false;
    ReadTags(Params, Tags, bHasTags);
    if (bHasTags)
    {
        NewActor->Tags = Tags;
    }

    // Optional flat property dict.
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
    {
        ApplyPropertyDict(NewActor, *PropsObj, AppliedJson, SkippedJson);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("spawn"));
    Result->SetObjectField(TEXT("actor"), ActorSummary(NewActor));
    Result->SetArrayField(TEXT("applied"), AppliedJson);
    Result->SetArrayField(TEXT("skipped"), SkippedJson);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSceneComposeCommands::ModifyActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorQuery;
    if (!Params->TryGetStringField(TEXT("actor"), ActorQuery)
        && !Params->TryGetStringField(TEXT("name"), ActorQuery)
        && !Params->TryGetStringField(TEXT("label"), ActorQuery))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor' parameter (an actor name or Outliner label)"));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    AActor* Actor = ResolveActor(World, ActorQuery);
    if (!Actor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Actor not found: '%s'"), *ActorQuery));
    }

    // Transform patch: each axis is independent, missing axes leave the
    // existing component untouched. Different from set_actor_transform which
    // takes a full transform replacement.
    bool bChangedTransform = false;
    FTransform NewTransform = Actor->GetTransform();
    if (Params->HasField(TEXT("location")))
    {
        NewTransform.SetLocation(FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
        bChangedTransform = true;
    }
    if (Params->HasField(TEXT("rotation")))
    {
        NewTransform.SetRotation(FQuat(FEpicUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
        bChangedTransform = true;
    }
    if (Params->HasField(TEXT("scale")))
    {
        NewTransform.SetScale3D(FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
        bChangedTransform = true;
    }
    if (bChangedTransform)
    {
        Actor->SetActorTransform(NewTransform);
    }

    // Outliner label.
    FString Label;
    if (Params->TryGetStringField(TEXT("label"), Label))
    {
        Actor->SetActorLabel(Label);
    }

    // Tags. Either a full replacement set, or no change.
    TArray<FName> Tags;
    bool bHasTags = false;
    ReadTags(Params, Tags, bHasTags);
    if (bHasTags)
    {
        Actor->Tags = Tags;
    }

    // Property dict.
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
    {
        ApplyPropertyDict(Actor, *PropsObj, AppliedJson, SkippedJson);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("modify"));
    Result->SetObjectField(TEXT("actor"), ActorSummary(Actor));
    Result->SetArrayField(TEXT("applied"), AppliedJson);
    Result->SetArrayField(TEXT("skipped"), SkippedJson);
    Result->SetBoolField(TEXT("transform_changed"), bChangedTransform);
    Result->SetBoolField(TEXT("tags_changed"), bHasTags);
    return Result;
}

TSharedPtr<FJsonObject> FSproftSceneComposeCommands::DeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorQuery;
    if (!Params->TryGetStringField(TEXT("actor"), ActorQuery)
        && !Params->TryGetStringField(TEXT("name"), ActorQuery)
        && !Params->TryGetStringField(TEXT("label"), ActorQuery))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor' parameter (an actor name or Outliner label)"));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    AActor* Actor = ResolveActor(World, ActorQuery);
    if (!Actor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Actor not found: '%s'"), *ActorQuery));
    }

    const FString DeletedName = Actor->GetName();
    const FString DeletedLabel = Actor->GetActorLabel();
    const FString DeletedClass = Actor->GetClass()->GetPathName();

    Actor->Destroy();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("delete"));
    Result->SetStringField(TEXT("name"), DeletedName);
    Result->SetStringField(TEXT("label"), DeletedLabel);
    Result->SetStringField(TEXT("class_path"), DeletedClass);
    Result->SetBoolField(TEXT("destroyed"), true);
    return Result;
}
