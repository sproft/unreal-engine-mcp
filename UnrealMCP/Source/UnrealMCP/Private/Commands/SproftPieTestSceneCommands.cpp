#include "Commands/SproftPieTestSceneCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
    /** Actor lookup that mirrors `actor_inspect` / `scene_compose`: try
     *  GetName() first (object name; what scripts spell out) and fall
     *  back to GetActorLabel() (the Outliner label; what designers
     *  spell out). We want assertions to feel symmetric with the
     *  scene-mutation tools so a designer can spawn an actor with a
     *  preferred name and reference it by the same string here. */
    AActor* ResolveActorByName(UWorld* World, const FString& Target)
    {
        if (!World || Target.IsEmpty())
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            if (Actor->GetName() == Target)
            {
                return Actor;
            }
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            if (Actor->GetActorLabel() == Target)
            {
                return Actor;
            }
        }
        return nullptr;
    }

    /** Pull a 3-element vector from a JSON value. Accepts arrays of
     *  three numbers and `{x, y, z}` object shapes; rejects everything
     *  else. */
    bool VectorFromJson(const TSharedPtr<FJsonValue>& Value, FVector& Out)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Value->TryGetArray(AsArray) && AsArray && AsArray->Num() == 3)
        {
            const double X = (*AsArray)[0]->AsNumber();
            const double Y = (*AsArray)[1]->AsNumber();
            const double Z = (*AsArray)[2]->AsNumber();
            Out = FVector(X, Y, Z);
            return true;
        }
        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (Value->TryGetObject(AsObject) && AsObject->IsValid())
        {
            double X = 0.0, Y = 0.0, Z = 0.0;
            if ((*AsObject)->TryGetNumberField(TEXT("x"), X)
                && (*AsObject)->TryGetNumberField(TEXT("y"), Y)
                && (*AsObject)->TryGetNumberField(TEXT("z"), Z))
            {
                Out = FVector(X, Y, Z);
                return true;
            }
        }
        return false;
    }

    /** Render an FVector as a JSON array `[x, y, z]` so the response
     *  echoes the same shape as the input expected vector. */
    TSharedPtr<FJsonValue> VectorToJsonArray(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return MakeShared<FJsonValueArray>(Arr);
    }
}

FSproftPieTestSceneCommands::FSproftPieTestSceneCommands()
{
}

TSharedPtr<FJsonObject> FSproftPieTestSceneCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("pie_test_scene"))
    {
        return HandlePieTestScene(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown pie_test_scene command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftPieTestSceneCommands::HandlePieTestScene(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    const TArray<TSharedPtr<FJsonValue>>* AssertionsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("assertions"), AssertionsArr) || !AssertionsArr || AssertionsArr->Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing or empty 'assertions' array"));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    int32 Passed = 0;
    int32 Failed = 0;
    int32 Unsupported = 0;
    TArray<TSharedPtr<FJsonValue>> Results;

    for (int32 Index = 0; Index < AssertionsArr->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Entry = (*AssertionsArr)[Index];
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("index"), Index);

        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(AsObject) || !AsObject->IsValid())
        {
            Out->SetBoolField(TEXT("passed"), false);
            Out->SetStringField(TEXT("kind"), TEXT(""));
            Out->SetStringField(TEXT("message"),
                TEXT("Assertion entry is not a JSON object"));
            ++Failed;
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }
        const TSharedPtr<FJsonObject>& Spec = *AsObject;

        FString Kind;
        Spec->TryGetStringField(TEXT("kind"), Kind);
        Out->SetStringField(TEXT("kind"), Kind);

        FString Target;
        Spec->TryGetStringField(TEXT("target"), Target);
        Out->SetStringField(TEXT("target"), Target);

        if (Kind == TEXT("actor_exists"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_exists: missing 'target' actor name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            AActor* Found = ResolveActorByName(World, Target);
            const bool bFound = (Found != nullptr);
            Out->SetBoolField(TEXT("passed"), bFound);
            if (bFound)
            {
                Out->SetStringField(TEXT("actual"), Found->GetName());
                Out->SetStringField(TEXT("actual_label"), Found->GetActorLabel());
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("Found actor '%s' (label '%s') in editor world"),
                        *Found->GetName(), *Found->GetActorLabel()));
                ++Passed;
            }
            else
            {
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("No actor with name or label '%s' in editor world"), *Target));
                ++Failed;
            }
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }

        if (Kind == TEXT("actor_at_location"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_at_location: missing 'target' actor name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            FVector Expected = FVector::ZeroVector;
            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            if (!VectorFromJson(ExpectedField, Expected))
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_at_location: 'expected' must be a [x, y, z] array or {x, y, z} object"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            double Tolerance = 1.0;
            Spec->TryGetNumberField(TEXT("tolerance"), Tolerance);
            if (Tolerance < 0.0)
            {
                Tolerance = 0.0;
            }

            AActor* Found = ResolveActorByName(World, Target);
            if (!Found)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_at_location: no actor with name or label '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const FVector Actual = Found->GetActorLocation();
            const double Delta = FVector::Dist(Actual, Expected);
            const bool bPassed = Delta <= Tolerance;

            Out->SetBoolField(TEXT("passed"), bPassed);
            Out->SetField(TEXT("actual"), VectorToJsonArray(Actual));
            Out->SetField(TEXT("expected"), VectorToJsonArray(Expected));
            Out->SetNumberField(TEXT("tolerance"), Tolerance);
            Out->SetNumberField(TEXT("delta"), Delta);
            Out->SetStringField(TEXT("message"),
                bPassed
                    ? FString::Printf(TEXT("Actor '%s' is %.4f cm from expected (tolerance %.4f)"),
                        *Found->GetName(), Delta, Tolerance)
                    : FString::Printf(TEXT("Actor '%s' is %.4f cm from expected (tolerance %.4f)"),
                        *Found->GetName(), Delta, Tolerance));
            if (bPassed)
            {
                ++Passed;
            }
            else
            {
                ++Failed;
            }
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }

        Out->SetBoolField(TEXT("passed"), false);
        Out->SetStringField(TEXT("message"),
            FString::Printf(TEXT("Unsupported assertion kind '%s'; this slice supports 'actor_exists' and 'actor_at_location'"), *Kind));
        ++Unsupported;
        Results.Add(MakeShared<FJsonValueObject>(Out));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("total"), AssertionsArr->Num());
    Result->SetNumberField(TEXT("passed"), Passed);
    Result->SetNumberField(TEXT("failed"), Failed);
    Result->SetNumberField(TEXT("unsupported"), Unsupported);
    Result->SetBoolField(TEXT("all_passed"), Failed == 0 && Unsupported == 0);
    Result->SetArrayField(TEXT("results"), Results);
    return Result;
}
