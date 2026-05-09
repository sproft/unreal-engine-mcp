#include "Commands/SproftFoliageEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FoliageType.h"
#include "InstancedFoliageActor.h"

namespace
{
    /** Friendly level name used in responses. Matches the convention used
     *  by the existing SproftFoliageInspect / SproftLandscapeInspect
     *  pipeline so cross-tool consumers see one identifier. */
    FString FoliageEdit_LevelLabel(ULevel* Level)
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

    /** Pick the persistent level of the editor world unless `LevelFilter`
     *  is set, in which case we pick the first sublevel whose owning
     *  ULevel-or-UWorld name matches the substring. Returns nullptr
     *  when nothing matches. */
    ULevel* ResolveLevelByFilter(UWorld* World, const FString& LevelFilter)
    {
        if (!World)
        {
            return nullptr;
        }
        if (LevelFilter.IsEmpty())
        {
            return World->PersistentLevel;
        }
        for (ULevel* Level : World->GetLevels())
        {
            if (!Level)
            {
                continue;
            }
            if (FoliageEdit_LevelLabel(Level).Contains(LevelFilter))
            {
                return Level;
            }
        }
        return nullptr;
    }
}

FSproftFoliageEditCommands::FSproftFoliageEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftFoliageEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("foliage_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown foliage_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    Op = Op.ToLower();

    if (Op == TEXT("add_foliage_type"))
    {
        return HandleAddFoliageType(Params);
    }
    if (Op == TEXT("set_foliage_density"))
    {
        return HandleSetFoliageDensity(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported foliage_edit op '%s'; expected one of 'add_foliage_type', 'set_foliage_density'"), *Op));
}

TSharedPtr<FJsonObject> FSproftFoliageEditCommands::HandleAddFoliageType(const TSharedPtr<FJsonObject>& Params)
{
    FString TypePath;
    if (!Params->TryGetStringField(TEXT("foliage_type"), TypePath) || TypePath.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_foliage_type: missing 'foliage_type' asset path"));
    }
    FString LevelFilter;
    Params->TryGetStringField(TEXT("level"), LevelFilter);
    bool bSave = false;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_foliage_type: failed to get editor world"));
    }

    ULevel* TargetLevel = ResolveLevelByFilter(World, LevelFilter);
    if (!TargetLevel)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_foliage_type: no level matched filter '%s'"), *LevelFilter));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(TypePath);
    UFoliageType* FoliageType = Cast<UFoliageType>(Asset);
    if (!FoliageType)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_foliage_type: '%s' is not a UFoliageType"), *TypePath));
    }

    // Probe whether an IFA already exists for the chosen level so we
    // can report `created_actor` accurately. AInstancedFoliageActor::Get
    // with bCreateIfNone=false returns nullptr when no IFA exists yet.
    AInstancedFoliageActor* ExistingIFA = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(TargetLevel, /*bCreateIfNone*/ false);
    const bool bIFAExisted = (ExistingIFA != nullptr);

    AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(TargetLevel, /*bCreateIfNone*/ true);
    if (!IFA)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("add_foliage_type: failed to spawn AInstancedFoliageActor for level '%s'"), *FoliageEdit_LevelLabel(TargetLevel)));
    }

    // FindInfo before the AddFoliageType call so we can report whether
    // the binding existed already. AddFoliageType is documented to
    // return the existing entry when the type is already bound.
    const FFoliageInfo* Existing = IFA->FindInfo(FoliageType);
    const bool bTypeAlreadyBound = (Existing != nullptr);

    FFoliageInfo* OutInfo = nullptr;
    UFoliageType* ResolvedType = IFA->AddFoliageType(FoliageType, &OutInfo);

    IFA->MarkPackageDirty();
    bool bSaved = false;
    if (bSave)
    {
        // The IFA lives on the level; saving its package writes the
        // sublevel back to disk. We use SaveLoadedAsset on the level
        // package indirectly through SaveAsset on the actor's
        // outermost package path.
        if (UPackage* Pkg = IFA->GetOutermost())
        {
            bSaved = UEditorAssetLibrary::SaveAsset(Pkg->GetName(), /*bOnlyIfIsDirty*/ false);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("add_foliage_type"));
    Result->SetStringField(TEXT("foliage_type_path"), FoliageType->GetPathName());
    Result->SetStringField(TEXT("foliage_type_class"), FoliageType->GetClass()->GetName());
    Result->SetStringField(TEXT("actor_name"), IFA->GetName());
    Result->SetStringField(TEXT("actor_label"), IFA->GetActorLabel());
    Result->SetStringField(TEXT("actor_path"), IFA->GetPathName());
    Result->SetStringField(TEXT("level"), FoliageEdit_LevelLabel(TargetLevel));
    Result->SetBoolField(TEXT("created_actor"), !bIFAExisted);
    Result->SetBoolField(TEXT("type_already_bound"), bTypeAlreadyBound);
    Result->SetNumberField(TEXT("foliage_type_count"), IFA->GetFoliageInfos().Num());
    if (ResolvedType && ResolvedType != FoliageType)
    {
        // AddFoliageType can return a duplicated FoliageType under the
        // IFA's outer when the original is from a different package
        // (the engine's standard "instanced settings" path). We surface
        // both paths so a caller can spot the duplication.
        Result->SetStringField(TEXT("resolved_type_path"), ResolvedType->GetPathName());
    }
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FSproftFoliageEditCommands::HandleSetFoliageDensity(const TSharedPtr<FJsonObject>& Params)
{
    FString TypePath;
    if (!Params->TryGetStringField(TEXT("foliage_type"), TypePath) || TypePath.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("set_foliage_density: missing 'foliage_type' asset path"));
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = UEditorAssetLibrary::LoadAsset(TypePath);
    UFoliageType* FoliageType = Cast<UFoliageType>(Asset);
    if (!FoliageType)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_foliage_density: '%s' is not a UFoliageType"), *TypePath));
    }

    TSharedPtr<FJsonObject> Changes = MakeShared<FJsonObject>();
    int32 ChangedCount = 0;

    auto WriteFloatField = [&](const FString& Key, float& Field)
    {
        double V = 0.0;
        if (Params->TryGetNumberField(Key, V))
        {
            const float Prev = Field;
            Field = static_cast<float>(V);
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("previous"), Prev);
            Row->SetNumberField(TEXT("new"), Field);
            Changes->SetObjectField(Key, Row);
            ++ChangedCount;
        }
    };

    WriteFloatField(TEXT("density"),                   FoliageType->Density);
    WriteFloatField(TEXT("density_adjustment_factor"), FoliageType->DensityAdjustmentFactor);
    WriteFloatField(TEXT("radius"),                    FoliageType->Radius);

    auto WriteIntervalField = [&](const FString& MinKey, const FString& MaxKey,
                                  const FString& OutKey, FFloatInterval& Field)
    {
        double Min = 0.0;
        double Max = 0.0;
        const bool bHasMin = Params->TryGetNumberField(MinKey, Min);
        const bool bHasMax = Params->TryGetNumberField(MaxKey, Max);
        if (bHasMin && bHasMax)
        {
            const FFloatInterval Prev = Field;
            Field.Min = static_cast<float>(Min);
            Field.Max = static_cast<float>(Max);
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            TSharedPtr<FJsonObject> PrevRow = MakeShared<FJsonObject>();
            PrevRow->SetNumberField(TEXT("min"), Prev.Min);
            PrevRow->SetNumberField(TEXT("max"), Prev.Max);
            TSharedPtr<FJsonObject> NewRow = MakeShared<FJsonObject>();
            NewRow->SetNumberField(TEXT("min"), Field.Min);
            NewRow->SetNumberField(TEXT("max"), Field.Max);
            Row->SetObjectField(TEXT("previous"), PrevRow);
            Row->SetObjectField(TEXT("new"), NewRow);
            Changes->SetObjectField(OutKey, Row);
            ++ChangedCount;
        }
        else if (bHasMin || bHasMax)
        {
            // Either both or neither; partial writes are surprising on a
            // float interval. Surface a warning row instead of guessing.
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("warning"), TEXT("partial interval; both _min and _max are required to write"));
            Changes->SetObjectField(OutKey, Row);
        }
    };

    WriteIntervalField(TEXT("scale_x_min"), TEXT("scale_x_max"), TEXT("scale_x"), FoliageType->ScaleX);
    WriteIntervalField(TEXT("scale_y_min"), TEXT("scale_y_max"), TEXT("scale_y"), FoliageType->ScaleY);
    WriteIntervalField(TEXT("scale_z_min"), TEXT("scale_z_max"), TEXT("scale_z"), FoliageType->ScaleZ);

    if (ChangedCount > 0)
    {
        FoliageType->MarkPackageDirty();
    }

    bool bSaved = false;
    if (bSave && ChangedCount > 0)
    {
        bSaved = UEditorAssetLibrary::SaveAsset(FoliageType->GetPathName(), /*bOnlyIfIsDirty*/ false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("op"), TEXT("set_foliage_density"));
    Result->SetStringField(TEXT("foliage_type_path"), FoliageType->GetPathName());
    Result->SetStringField(TEXT("foliage_type_class"), FoliageType->GetClass()->GetName());
    Result->SetNumberField(TEXT("changed_count"), ChangedCount);
    Result->SetObjectField(TEXT("changes"), Changes);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}
