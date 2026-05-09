#include "Commands/SproftActorInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Resolve an actor by FName first, then by case-insensitive Outliner label. */
    AActor* ActorInspect_ResolveActor(UWorld* World, const FString& InQuery)
    {
        if (!World || InQuery.IsEmpty())
        {
            return nullptr;
        }

        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);

        // Exact internal name match first.
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == InQuery)
            {
                return Actor;
            }
        }

        // Then exact (case-insensitive) Outliner label match.
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

    /** Build a [x, y, z] number array. */
    TArray<TSharedPtr<FJsonValue>> ActorInspect_Vec3ToJson(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    }

    TArray<TSharedPtr<FJsonValue>> ActorInspect_RotToJson(const FRotator& R)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        return Arr;
    }

    /** Mobility enum to short label. */
    FString ActorInspect_MobilityToString(EComponentMobility::Type In)
    {
        switch (In)
        {
            case EComponentMobility::Static:     return TEXT("Static");
            case EComponentMobility::Stationary: return TEXT("Stationary");
            case EComponentMobility::Movable:    return TEXT("Movable");
        }
        return TEXT("Unknown");
    }

    /** Skip the heaviest / most cluttered uproperty types when dumping defaults
     *  so the response remains useful without growing into a wall of text. */
    bool ShouldSkipPropertyForDump(FProperty* Prop)
    {
        if (!Prop)
        {
            return true;
        }
        // Hide editor-only and transient / replicated-state plumbing.
        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
        {
            return true;
        }
        // Heavy struct types we do not want to splat into the JSON wholesale.
        const FString Cpp = Prop->GetCPPType();
        if (Cpp.Contains(TEXT("FBodyInstance")) || Cpp.Contains(TEXT("FCollisionResponse")))
        {
            return true;
        }
        return false;
    }

    /** Export a small set of editable properties from the object as ExportText
     *  strings. Returns up to MaxProperties entries to avoid runaway output. */
    TSharedPtr<FJsonObject> DumpPropertiesShort(UObject* Object, int32 MaxProperties)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Object)
        {
            return Out;
        }

        UClass* Class = Object->GetClass();
        int32 Emitted = 0;
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            if (Emitted >= MaxProperties)
            {
                break;
            }
            FProperty* Prop = *It;
            if (!Prop)
            {
                continue;
            }
            // Only surface Edit / Visible properties to keep the noise down.
            if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                continue;
            }
            if (ShouldSkipPropertyForDump(Prop))
            {
                continue;
            }

            FString TextValue;
            // ExportText_InContainer renders the per-instance value, falling back
            // to defaults when the instance has not been mutated.
            Prop->ExportText_InContainer(0, TextValue, Object, Object, Object, PPF_None);

            // Trim out very long blob values (textures, large arrays, etc.).
            if (TextValue.Len() > 256)
            {
                TextValue = TextValue.Left(253) + TEXT("...");
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("type"), Prop->GetCPPType());
            Entry->SetStringField(TEXT("value"), TextValue);
            Out->SetObjectField(Prop->GetName(), Entry);
            ++Emitted;
        }
        return Out;
    }

    /** Build a JSON record for one component on the actor. */
    TSharedPtr<FJsonObject> ActorInspect_ComponentRecord(UActorComponent* Component, bool bIncludeProperties, int32 MaxPropertiesPerComponent)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Component)
        {
            return Out;
        }

        Out->SetStringField(TEXT("name"), Component->GetName());
        Out->SetStringField(TEXT("class"), Component->GetClass()->GetName());
        Out->SetStringField(TEXT("class_path"), Component->GetClass()->GetPathName());
        Out->SetBoolField(TEXT("is_active"), Component->IsActive());
        Out->SetBoolField(TEXT("auto_activate"), Component->bAutoActivate);

        if (USceneComponent* AsScene = Cast<USceneComponent>(Component))
        {
            Out->SetBoolField(TEXT("is_scene_component"), true);
            Out->SetStringField(TEXT("mobility"), ActorInspect_MobilityToString(AsScene->Mobility));
            Out->SetArrayField(TEXT("relative_location"), ActorInspect_Vec3ToJson(AsScene->GetRelativeLocation()));
            Out->SetArrayField(TEXT("relative_rotation"), ActorInspect_RotToJson(AsScene->GetRelativeRotation()));
            Out->SetArrayField(TEXT("relative_scale"),    ActorInspect_Vec3ToJson(AsScene->GetRelativeScale3D()));

            if (USceneComponent* Parent = AsScene->GetAttachParent())
            {
                Out->SetStringField(TEXT("attach_parent"), Parent->GetName());
                if (AsScene->GetAttachSocketName() != NAME_None)
                {
                    Out->SetStringField(TEXT("attach_socket"), AsScene->GetAttachSocketName().ToString());
                }
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

        if (bIncludeProperties && MaxPropertiesPerComponent > 0)
        {
            Out->SetObjectField(TEXT("properties"), DumpPropertiesShort(Component, MaxPropertiesPerComponent));
        }

        return Out;
    }
}

FSproftActorInspectCommands::FSproftActorInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftActorInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("actor_inspect"))
    {
        return HandleActorInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown actor_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftActorInspectCommands::HandleActorInspect(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString ActorQuery;
    if (!Params->TryGetStringField(TEXT("actor"), ActorQuery)
        && !Params->TryGetStringField(TEXT("name"), ActorQuery)
        && !Params->TryGetStringField(TEXT("label"), ActorQuery))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor' parameter (an actor name or Outliner label)"));
    }

    bool bIncludeComponents = true;
    Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);

    bool bIncludeComponentProperties = false;
    Params->TryGetBoolField(TEXT("include_component_properties"), bIncludeComponentProperties);

    bool bIncludeActorProperties = false;
    Params->TryGetBoolField(TEXT("include_actor_properties"), bIncludeActorProperties);

    int32 MaxPropertiesPerObject = 24;
    if (Params->HasField(TEXT("max_properties_per_object")))
    {
        MaxPropertiesPerObject = static_cast<int32>(Params->GetNumberField(TEXT("max_properties_per_object")));
        if (MaxPropertiesPerObject < 0)
        {
            MaxPropertiesPerObject = 0;
        }
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    AActor* Actor = ActorInspect_ResolveActor(World, ActorQuery);
    if (!Actor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Actor not found: '%s' (matched against GetName() and GetActorLabel())"), *ActorQuery));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("name"), Actor->GetName());
    Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    Result->SetStringField(TEXT("class_path"), Actor->GetClass()->GetPathName());

    // Folder path (null if at root).
    const FName Folder = Actor->GetFolderPath();
    if (Folder != NAME_None)
    {
        Result->SetStringField(TEXT("folder_path"), Folder.ToString());
    }

    Result->SetArrayField(TEXT("location"), ActorInspect_Vec3ToJson(Actor->GetActorLocation()));
    Result->SetArrayField(TEXT("rotation"), ActorInspect_RotToJson(Actor->GetActorRotation()));
    Result->SetArrayField(TEXT("scale"),    ActorInspect_Vec3ToJson(Actor->GetActorScale3D()));

    TArray<TSharedPtr<FJsonValue>> TagArr;
    for (const FName& Tag : Actor->Tags)
    {
        TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
    }
    Result->SetArrayField(TEXT("tags"), TagArr);

    Result->SetBoolField(TEXT("hidden_in_game"), Actor->IsHidden());
    Result->SetBoolField(TEXT("hidden_in_editor"), Actor->IsTemporarilyHiddenInEditor());

    // Replication snapshot. Cheap to read and useful for verifying multiplayer
    // intent without spawning a separate inspect call.
    Result->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());
    Result->SetBoolField(TEXT("replicates_movement"), Actor->IsReplicatingMovement());
    Result->SetNumberField(TEXT("net_priority"), Actor->NetPriority);
    Result->SetNumberField(TEXT("net_update_frequency"), Actor->NetUpdateFrequency);

    if (USceneComponent* Root = Actor->GetRootComponent())
    {
        Result->SetStringField(TEXT("root_component"), Root->GetName());
        Result->SetStringField(TEXT("root_component_class"), Root->GetClass()->GetName());
        Result->SetStringField(TEXT("mobility"), ActorInspect_MobilityToString(Root->Mobility));
    }

    // Optional full actor property dump.
    if (bIncludeActorProperties && MaxPropertiesPerObject > 0)
    {
        Result->SetObjectField(TEXT("properties"), DumpPropertiesShort(Actor, MaxPropertiesPerObject));
    }

    if (bIncludeComponents)
    {
        TArray<UActorComponent*> AllComponents;
        Actor->GetComponents(AllComponents);

        TArray<TSharedPtr<FJsonValue>> CompArr;
        for (UActorComponent* Comp : AllComponents)
        {
            CompArr.Add(MakeShared<FJsonValueObject>(
                ActorInspect_ComponentRecord(Comp, bIncludeComponentProperties, MaxPropertiesPerObject)));
        }
        Result->SetArrayField(TEXT("components"), CompArr);
        Result->SetNumberField(TEXT("component_count"), AllComponents.Num());
    }

    return Result;
}
