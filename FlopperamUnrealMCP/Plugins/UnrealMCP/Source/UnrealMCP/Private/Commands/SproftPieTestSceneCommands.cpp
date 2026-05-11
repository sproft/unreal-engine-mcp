#include "Commands/SproftPieTestSceneCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"

namespace
{
    /** Actor lookup that mirrors `actor_inspect` / `scene_compose`: try
     *  GetName() first (object name; what scripts spell out) and fall
     *  back to GetActorLabel() (the Outliner label; what designers
     *  spell out). We want assertions to feel symmetric with the
     *  scene-mutation tools so a designer can spawn an actor with a
     *  preferred name and reference it by the same string here. */
    AActor* PieTestScene_ResolveActorByName(UWorld* World, const FString& Target)
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

    /** Render a JSON value as a plain string. Numbers / bools / strings
     *  flatten to their primitive text form; objects / arrays go through
     *  the standard Json writer so callers see a stable canonicalized
     *  representation when ImportText fails. */
    FString PieTestScene_JsonValueToString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        FString Out;
        switch (Value->Type)
        {
            case EJson::String:
                Value->TryGetString(Out);
                return Out;
            case EJson::Number:
            {
                double N = 0.0;
                if (Value->TryGetNumber(N))
                {
                    return FString::SanitizeFloat(N);
                }
                return TEXT("0");
            }
            case EJson::Boolean:
            {
                bool B = false;
                Value->TryGetBool(B);
                return B ? TEXT("true") : TEXT("false");
            }
            case EJson::Null:
                return TEXT("None");
            default:
                break;
        }
        // Fall through for objects / arrays: serialize compact.
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
            = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
        return Out;
    }

    /** Walk the actor's `Tags` array looking for an exact FName match.
     *  Returns true when the tag is present plus an `actual` snapshot of
     *  the tag list for the response message. */
    bool ActorHasTag(const AActor* Actor, const FName& TagName, TArray<FString>& OutAllTags)
    {
        OutAllTags.Reset();
        if (!Actor)
        {
            return false;
        }
        for (const FName& Existing : Actor->Tags)
        {
            OutAllTags.Add(Existing.ToString());
        }
        return Actor->Tags.Contains(TagName);
    }

    /** Resolve a UClass token against the live editor class set.
     *  Accepts a full `/Script/Module.ClassName` path, a `/Game/...`
     *  Blueprint class path (auto-suffixed with `_C` if missing), or a
     *  bare short class name probed through `FindObject<UClass>` with
     *  the standard A / U prefix variants and a `/Script/Engine.*`
     *  fallback. Returns nullptr on miss. */
    UClass* PieTestScene_ResolveClassByToken(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Cls = FindObject<UClass>(nullptr, *Token))
            {
                return Cls;
            }
        }
        if (Token.StartsWith(TEXT("/Game/")))
        {
            FString Path = Token;
            if (!Path.EndsWith(TEXT("_C")))
            {
                Path += TEXT("_C");
            }
            if (UClass* Cls = LoadObject<UClass>(nullptr, *Path))
            {
                return Cls;
            }
        }
        // Bare short class name: try with A / U prefixes and the
        // engine module fallback.
        const TArray<FString> Variants =
        {
            FString::Printf(TEXT("/Script/Engine.%s"), *Token),
            FString::Printf(TEXT("/Script/Engine.A%s"), *Token),
            FString::Printf(TEXT("/Script/Engine.U%s"), *Token),
        };
        for (const FString& V : Variants)
        {
            if (UClass* Cls = FindObject<UClass>(nullptr, *V))
            {
                return Cls;
            }
        }
        // Final pass: walk the loaded class set for an exact short
        // name match. This is the slowest path, gated on a miss above.
        UClass* Found = nullptr;
        ForEachObjectOfClass(UClass::StaticClass(), [&Found, &Token](UObject* Obj)
        {
            UClass* Cls = Cast<UClass>(Obj);
            if (!Cls) { return; }
            const FString N = Cls->GetName();
            if (N == Token
                || (FString(TEXT("A")) + N) == Token
                || (FString(TEXT("U")) + N) == Token
                || N == (FString(TEXT("A")) + Token)
                || N == (FString(TEXT("U")) + Token))
            {
                Found = Cls;
            }
        });
        return Found;
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
            AActor* Found = PieTestScene_ResolveActorByName(World, Target);
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

            AActor* Found = PieTestScene_ResolveActorByName(World, Target);
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

        if (Kind == TEXT("actor_overlapping_tag"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_overlapping_tag: missing 'target' actor name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            FString TagString;
            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            if (ExpectedField.IsValid())
            {
                if (!ExpectedField->TryGetString(TagString))
                {
                    TagString = PieTestScene_JsonValueToString(ExpectedField);
                }
            }
            if (TagString.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_overlapping_tag: 'expected' must be a non-empty tag string"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            AActor* Found = PieTestScene_ResolveActorByName(World, Target);
            if (!Found)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("expected"), TagString);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_overlapping_tag: no actor with name or label '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const FName ExpectedTag(*TagString);
            TArray<FString> AllTags;
            const bool bHasTag = ActorHasTag(Found, ExpectedTag, AllTags);

            TArray<TSharedPtr<FJsonValue>> TagList;
            for (const FString& T : AllTags)
            {
                TagList.Add(MakeShared<FJsonValueString>(T));
            }
            Out->SetArrayField(TEXT("actual"), TagList);
            Out->SetStringField(TEXT("expected"), TagString);
            Out->SetBoolField(TEXT("passed"), bHasTag);
            Out->SetStringField(TEXT("message"),
                bHasTag
                    ? FString::Printf(TEXT("Actor '%s' has tag '%s' (%d total tag(s))"),
                        *Found->GetName(), *TagString, AllTags.Num())
                    : FString::Printf(TEXT("Actor '%s' has %d tag(s); none match '%s'"),
                        *Found->GetName(), AllTags.Num(), *TagString));
            if (bHasTag)
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

        if (Kind == TEXT("var_equals"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("var_equals: missing 'target' actor name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            const TSharedPtr<FJsonObject>* ExpectedObj = nullptr;
            if (!ExpectedField.IsValid() || !ExpectedField->TryGetObject(ExpectedObj) || !ExpectedObj->IsValid())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("var_equals: 'expected' must be a {var, value} object"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            FString VarName;
            if (!(*ExpectedObj)->TryGetStringField(TEXT("var"), VarName) || VarName.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("var_equals: 'expected.var' must be a non-empty UPROPERTY name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const TSharedPtr<FJsonValue> ValueField = (*ExpectedObj)->TryGetField(TEXT("value"));
            if (!ValueField.IsValid())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("var_equals: 'expected.value' is required"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            AActor* Found = PieTestScene_ResolveActorByName(World, Target);
            if (!Found)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("var"), VarName);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("var_equals: no actor with name or label '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            UClass* ActorClass = Found->GetClass();
            FProperty* Prop = ActorClass ? ActorClass->FindPropertyByName(FName(*VarName)) : nullptr;
            if (!Prop)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("var"), VarName);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("var_equals: actor '%s' has no UPROPERTY named '%s'"),
                        *Found->GetName(), *VarName));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            // Read the actor's current value as ExportText so we can compare
            // string against string after canonicalizing the expected value
            // through ImportText -> ExportText on a transient buffer.
            void* PropPtr = Prop->ContainerPtrToValuePtr<void>(Found);
            FString ActualText;
            Prop->ExportText_Direct(ActualText, PropPtr, PropPtr, Found, PPF_None);

            // Canonicalize the expected JSON value through the same
            // property's ImportText into a transient stack buffer so we
            // compare apples to apples (e.g. "1" -> "1", "1.0" -> "1.0",
            // {"X":1,"Y":2,"Z":3} -> "(X=1.000000,Y=2.000000,Z=3.000000)").
            TArray<uint8> Scratch;
            Scratch.SetNumZeroed(Prop->GetSize());
            Prop->InitializeValue(Scratch.GetData());

            const FString ExpectedRaw = PieTestScene_JsonValueToString(ValueField);
            const TCHAR* ImportPtr = *ExpectedRaw;
            const TCHAR* ImportResult = Prop->ImportText_Direct(ImportPtr, Scratch.GetData(), Found, PPF_None);
            FString ExpectedCanonical;
            bool bExpectedImported = (ImportResult != nullptr);
            if (bExpectedImported)
            {
                Prop->ExportText_Direct(ExpectedCanonical, Scratch.GetData(), Scratch.GetData(), Found, PPF_None);
            }
            else
            {
                // Fall back to direct string compare against the raw input
                // when ImportText refuses the JSON literal (rare for the
                // typed properties this tool targets, but keeps the
                // assertion useful).
                ExpectedCanonical = ExpectedRaw;
            }
            Prop->DestroyValue(Scratch.GetData());

            const bool bMatch = ActualText.Equals(ExpectedCanonical, ESearchCase::CaseSensitive);

            Out->SetStringField(TEXT("var"), VarName);
            Out->SetStringField(TEXT("actual"), ActualText);
            Out->SetStringField(TEXT("expected"), ExpectedCanonical);
            Out->SetStringField(TEXT("expected_raw"), ExpectedRaw);
            Out->SetBoolField(TEXT("passed"), bMatch);
            Out->SetStringField(TEXT("property_class"), Prop->GetClass()->GetName());
            Out->SetBoolField(TEXT("expected_imported"), bExpectedImported);
            Out->SetStringField(TEXT("message"),
                bMatch
                    ? FString::Printf(TEXT("Actor '%s'.%s == %s"),
                        *Found->GetName(), *VarName, *ActualText)
                    : FString::Printf(TEXT("Actor '%s'.%s = %s; expected %s"),
                        *Found->GetName(), *VarName, *ActualText, *ExpectedCanonical));
            if (bMatch)
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

        if (Kind == TEXT("actor_has_class"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_has_class: missing 'target' actor name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            FString ExpectedClassToken;
            if (ExpectedField.IsValid())
            {
                if (!ExpectedField->TryGetString(ExpectedClassToken))
                {
                    ExpectedClassToken = PieTestScene_JsonValueToString(ExpectedField);
                }
            }
            if (ExpectedClassToken.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_has_class: 'expected' must be a non-empty class path or short class name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            UClass* ExpectedClass = PieTestScene_ResolveClassByToken(ExpectedClassToken);
            if (!ExpectedClass)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("expected"), ExpectedClassToken);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_has_class: could not resolve expected class '%s'"), *ExpectedClassToken));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            AActor* Found = PieTestScene_ResolveActorByName(World, Target);
            if (!Found)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("expected"), ExpectedClass->GetPathName());
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_has_class: no actor with name or label '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            UClass* ActualClass = Found->GetClass();
            const bool bPassed = ActualClass && ActualClass->IsChildOf(ExpectedClass);
            Out->SetStringField(TEXT("expected"), ExpectedClass->GetPathName());
            Out->SetStringField(TEXT("actual"), ActualClass ? ActualClass->GetPathName() : FString());
            Out->SetBoolField(TEXT("passed"), bPassed);
            Out->SetStringField(TEXT("message"),
                bPassed
                    ? FString::Printf(TEXT("Actor '%s' is a %s (matches %s)"),
                        *Found->GetName(),
                        *ActualClass->GetName(), *ExpectedClass->GetName())
                    : FString::Printf(TEXT("Actor '%s' is a %s; expected %s or subclass"),
                        *Found->GetName(),
                        ActualClass ? *ActualClass->GetName() : TEXT("<null>"),
                        *ExpectedClass->GetName()));
            if (bPassed) { ++Passed; } else { ++Failed; }
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }

        if (Kind == TEXT("actor_tag_count"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_tag_count: missing 'target' actor name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            double ExpectedRaw = 0.0;
            if (!ExpectedField.IsValid() || !ExpectedField->TryGetNumber(ExpectedRaw))
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_tag_count: 'expected' must be an integer"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const int32 ExpectedCount = static_cast<int32>(ExpectedRaw);

            AActor* Found = PieTestScene_ResolveActorByName(World, Target);
            if (!Found)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetNumberField(TEXT("expected"), ExpectedCount);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_tag_count: no actor with name or label '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const int32 ActualCount = Found->Tags.Num();
            const bool bPassed = (ActualCount == ExpectedCount);
            TArray<TSharedPtr<FJsonValue>> TagList;
            for (const FName& T : Found->Tags)
            {
                TagList.Add(MakeShared<FJsonValueString>(T.ToString()));
            }
            Out->SetArrayField(TEXT("tags"), TagList);
            Out->SetNumberField(TEXT("actual"), ActualCount);
            Out->SetNumberField(TEXT("expected"), ExpectedCount);
            Out->SetBoolField(TEXT("passed"), bPassed);
            Out->SetStringField(TEXT("message"),
                bPassed
                    ? FString::Printf(TEXT("Actor '%s' has %d tag(s) (matches expected)"),
                        *Found->GetName(), ActualCount)
                    : FString::Printf(TEXT("Actor '%s' has %d tag(s); expected %d"),
                        *Found->GetName(), ActualCount, ExpectedCount));
            if (bPassed) { ++Passed; } else { ++Failed; }
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }

        if (Kind == TEXT("level_actor_count"))
        {
            // For level_actor_count `target` doubles as the class
            // token: the caller is asking "how many things of class X
            // exist". We accept the same class-token shapes the other
            // class-resolving kinds accept.
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("level_actor_count: missing 'target' class path"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            double ExpectedRaw = 0.0;
            if (!ExpectedField.IsValid() || !ExpectedField->TryGetNumber(ExpectedRaw))
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("level_actor_count: 'expected' must be an integer"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            const int32 ExpectedCount = static_cast<int32>(ExpectedRaw);
            UClass* TargetClass = PieTestScene_ResolveClassByToken(Target);
            if (!TargetClass)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetNumberField(TEXT("expected"), ExpectedCount);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("level_actor_count: could not resolve class '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            // The class must be an AActor subclass for the walk to
            // make sense; everything else fails closed with a clear
            // message.
            if (!TargetClass->IsChildOf(AActor::StaticClass()))
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetNumberField(TEXT("expected"), ExpectedCount);
                Out->SetStringField(TEXT("class"), TargetClass->GetPathName());
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("level_actor_count: '%s' is not an AActor subclass"),
                        *TargetClass->GetPathName()));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            // Count actors of the class in the editor world. Mirrors
            // UGameplayStatics::GetAllActorsOfClass (which includes
            // subclasses through IsA<T>).
            int32 ActualCount = 0;
            for (TActorIterator<AActor> It(World, TargetClass); It; ++It)
            {
                if (*It)
                {
                    ++ActualCount;
                }
            }
            const bool bPassed = (ActualCount == ExpectedCount);
            Out->SetStringField(TEXT("class"), TargetClass->GetPathName());
            Out->SetNumberField(TEXT("actual"), ActualCount);
            Out->SetNumberField(TEXT("expected"), ExpectedCount);
            Out->SetBoolField(TEXT("passed"), bPassed);
            Out->SetStringField(TEXT("message"),
                bPassed
                    ? FString::Printf(TEXT("Editor world has %d actor(s) of class %s (matches expected)"),
                        ActualCount, *TargetClass->GetName())
                    : FString::Printf(TEXT("Editor world has %d actor(s) of class %s; expected %d"),
                        ActualCount, *TargetClass->GetName(), ExpectedCount));
            if (bPassed) { ++Passed; } else { ++Failed; }
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }

        if (Kind == TEXT("actor_distance"))
        {
            // Two-target distance assertion. `target` resolves the
            // first actor and `target_b` (alias `other`) resolves
            // the second; `max_distance` is the upper bound for the
            // pass case (FVector::Dist(A, B) <= max_distance +
            // tolerance). `tolerance` defaults to 0.0 so callers
            // get an exact-or-less comparison unless they opt in.
            // Both actor lookups go through the same name / label
            // resolver every other kind uses so a designer can
            // reference actors by their preferred name across
            // every assertion shape.
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_distance: missing 'target' actor name (the first actor)"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            FString TargetB;
            if (!Spec->TryGetStringField(TEXT("target_b"), TargetB)
                && !Spec->TryGetStringField(TEXT("other"), TargetB)
                && !Spec->TryGetStringField(TEXT("b"), TargetB))
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_distance: missing 'target_b' (or 'other') actor name (the second actor)"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            if (TargetB.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_distance: 'target_b' must not be empty"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            // `max_distance` is the documented pass-condition cap.
            // We also accept `expected` as an alias so the field
            // shape stays consistent with the other kinds when the
            // caller wants one flat key.
            double MaxDistance = 0.0;
            bool bHaveMax = Spec->TryGetNumberField(TEXT("max_distance"), MaxDistance);
            if (!bHaveMax)
            {
                const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
                if (ExpectedField.IsValid())
                {
                    bHaveMax = ExpectedField->TryGetNumber(MaxDistance);
                }
            }
            if (!bHaveMax)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("actor_distance: missing 'max_distance' (or 'expected') number (the upper bound, in cm)"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            if (MaxDistance < 0.0)
            {
                MaxDistance = 0.0;
            }
            double Tolerance = 0.0;
            Spec->TryGetNumberField(TEXT("tolerance"), Tolerance);
            if (Tolerance < 0.0)
            {
                Tolerance = 0.0;
            }

            AActor* FoundA = PieTestScene_ResolveActorByName(World, Target);
            if (!FoundA)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_distance: no actor with name or label '%s'"), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }
            AActor* FoundB = PieTestScene_ResolveActorByName(World, TargetB);
            if (!FoundB)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("target_b"), TargetB);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("actor_distance: no actor with name or label '%s'"), *TargetB));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            const FVector LocA = FoundA->GetActorLocation();
            const FVector LocB = FoundB->GetActorLocation();
            const double Distance = FVector::Dist(LocA, LocB);
            const double EffectiveMax = MaxDistance + Tolerance;
            const bool bPassed = Distance <= EffectiveMax;

            Out->SetStringField(TEXT("target_b"), TargetB);
            Out->SetField(TEXT("location_a"), VectorToJsonArray(LocA));
            Out->SetField(TEXT("location_b"), VectorToJsonArray(LocB));
            Out->SetNumberField(TEXT("distance"), Distance);
            Out->SetNumberField(TEXT("max_distance"), MaxDistance);
            Out->SetNumberField(TEXT("tolerance"), Tolerance);
            Out->SetBoolField(TEXT("passed"), bPassed);
            Out->SetStringField(TEXT("message"),
                bPassed
                    ? FString::Printf(TEXT("Actors '%s' and '%s' are %.4f cm apart (within max %.4f + tolerance %.4f)"),
                        *FoundA->GetName(), *FoundB->GetName(), Distance, MaxDistance, Tolerance)
                    : FString::Printf(TEXT("Actors '%s' and '%s' are %.4f cm apart; expected at most %.4f (+ tolerance %.4f)"),
                        *FoundA->GetName(), *FoundB->GetName(), Distance, MaxDistance, Tolerance));
            if (bPassed) { ++Passed; } else { ++Failed; }
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }

        Out->SetBoolField(TEXT("passed"), false);
        Out->SetStringField(TEXT("message"),
            FString::Printf(TEXT("Unsupported assertion kind '%s'; this build supports 'actor_exists', 'actor_at_location', 'actor_overlapping_tag', 'var_equals', 'actor_has_class', 'actor_tag_count', 'level_actor_count', 'actor_distance'"), *Kind));
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
