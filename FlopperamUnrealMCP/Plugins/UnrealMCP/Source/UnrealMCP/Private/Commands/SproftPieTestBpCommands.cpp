#include "Commands/SproftPieTestBpCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Render a JSON value as a plain string. Numbers / bools / strings
     *  flatten to their primitive text form; objects / arrays go through
     *  the standard Json writer so callers see a stable canonicalised
     *  representation when ImportText fails. Mirrors the helper in
     *  `pie_test_scene`. */
    FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
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
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
            = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
        return Out;
    }
}

FSproftPieTestBpCommands::FSproftPieTestBpCommands()
{
}

TSharedPtr<FJsonObject> FSproftPieTestBpCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("pie_test_bp"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown pie_test_bp command: %s"), *CommandType));
    }
    return HandlePieTestBp(Params);
}

TSharedPtr<FJsonObject> FSproftPieTestBpCommands::HandlePieTestBp(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintPath)
        && !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath)
        && !Params->TryGetStringField(TEXT("path"), BlueprintPath)
        && !Params->TryGetStringField(TEXT("asset"), BlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint' parameter"));
    }

    const TArray<TSharedPtr<FJsonValue>>* AssertionsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("assertions"), AssertionsArr) || !AssertionsArr || AssertionsArr->Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing or empty 'assertions' array"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(BlueprintPath);
    UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UBlueprint"), *BlueprintPath));
    }

    UClass* GeneratedClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
    UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/true) : nullptr;
    if (!CDO || !GeneratedClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no resolved generated class / CDO; recompile and retry"),
                *Blueprint->GetName()));
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

        const TSharedPtr<FJsonObject>* SpecObjPtr = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(SpecObjPtr) || !SpecObjPtr->IsValid())
        {
            Out->SetBoolField(TEXT("passed"), false);
            Out->SetStringField(TEXT("message"),
                TEXT("Assertion entry must be a JSON object {kind, target, expected}"));
            ++Failed;
            Results.Add(MakeShared<FJsonValueObject>(Out));
            continue;
        }
        const TSharedPtr<FJsonObject>& Spec = *SpecObjPtr;

        FString Kind;
        Spec->TryGetStringField(TEXT("kind"), Kind);
        Out->SetStringField(TEXT("kind"), Kind);

        FString Target;
        Spec->TryGetStringField(TEXT("target"), Target);
        Out->SetStringField(TEXT("target"), Target);

        if (Kind == TEXT("default_value_equals"))
        {
            if (Target.IsEmpty())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("default_value_equals: missing 'target' UPROPERTY name"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            const TSharedPtr<FJsonValue> ExpectedField = Spec->TryGetField(TEXT("expected"));
            if (!ExpectedField.IsValid())
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    TEXT("default_value_equals: 'expected' is required"));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            FProperty* Prop = GeneratedClass->FindPropertyByName(FName(*Target));
            if (!Prop)
            {
                Out->SetBoolField(TEXT("passed"), false);
                Out->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("default_value_equals: Blueprint '%s' has no UPROPERTY named '%s'"),
                        *Blueprint->GetName(), *Target));
                ++Failed;
                Results.Add(MakeShared<FJsonValueObject>(Out));
                continue;
            }

            // Read the CDO's current default through ExportText so the
            // comparison runs against the engine's canonical text form.
            void* PropPtr = Prop->ContainerPtrToValuePtr<void>(CDO);
            FString ActualText;
            Prop->ExportText_Direct(ActualText, PropPtr, PropPtr, CDO, PPF_None);

            // Canonicalise the expected JSON literal through the same
            // property's ImportText into a transient buffer; ExportText
            // it back so we compare engine-canonical text against
            // engine-canonical text. Falls back to the raw JSON-string
            // form when ImportText refuses the literal so the assertion
            // still gives a useful message.
            TArray<uint8> Scratch;
            Scratch.SetNumZeroed(Prop->GetSize());
            Prop->InitializeValue(Scratch.GetData());

            const FString ExpectedRaw = JsonValueToString(ExpectedField);
            const TCHAR* ImportPtr = *ExpectedRaw;
            const TCHAR* ImportResult =
                Prop->ImportText_Direct(ImportPtr, Scratch.GetData(), CDO, PPF_None);
            FString ExpectedCanonical;
            const bool bExpectedImported = (ImportResult != nullptr);
            if (bExpectedImported)
            {
                Prop->ExportText_Direct(ExpectedCanonical, Scratch.GetData(), Scratch.GetData(), CDO, PPF_None);
            }
            else
            {
                ExpectedCanonical = ExpectedRaw;
            }
            Prop->DestroyValue(Scratch.GetData());

            const bool bMatch = ActualText.Equals(ExpectedCanonical, ESearchCase::CaseSensitive);

            Out->SetStringField(TEXT("var"), Target);
            Out->SetStringField(TEXT("actual"), ActualText);
            Out->SetStringField(TEXT("expected"), ExpectedCanonical);
            Out->SetStringField(TEXT("expected_raw"), ExpectedRaw);
            Out->SetBoolField(TEXT("passed"), bMatch);
            Out->SetStringField(TEXT("property_class"), Prop->GetClass()->GetName());
            Out->SetBoolField(TEXT("expected_imported"), bExpectedImported);
            Out->SetStringField(TEXT("message"),
                bMatch
                    ? FString::Printf(TEXT("Blueprint '%s' CDO.%s == %s"),
                        *Blueprint->GetName(), *Target, *ActualText)
                    : FString::Printf(TEXT("Blueprint '%s' CDO.%s = %s; expected %s"),
                        *Blueprint->GetName(), *Target, *ActualText, *ExpectedCanonical));

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

        Out->SetBoolField(TEXT("passed"), false);
        Out->SetStringField(TEXT("message"),
            FString::Printf(TEXT("Unsupported assertion kind '%s'; this build supports 'default_value_equals' (function_returns and event_fired need a running PIE session and stay on the backlog)"),
                *Kind));
        ++Unsupported;
        Results.Add(MakeShared<FJsonValueObject>(Out));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    Result->SetStringField(TEXT("generated_class"), GeneratedClass->GetPathName());
    Result->SetNumberField(TEXT("total"), AssertionsArr->Num());
    Result->SetNumberField(TEXT("passed"), Passed);
    Result->SetNumberField(TEXT("failed"), Failed);
    Result->SetNumberField(TEXT("unsupported"), Unsupported);
    Result->SetBoolField(TEXT("all_passed"), Failed == 0 && Unsupported == 0);
    Result->SetArrayField(TEXT("assertions"), Results);
    return Result;
}
