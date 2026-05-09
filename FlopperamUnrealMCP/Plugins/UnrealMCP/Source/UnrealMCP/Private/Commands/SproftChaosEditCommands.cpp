#include "Commands/SproftChaosEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Chaos/ChaosSolverActor.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionSimulationTypes.h"
#include "GeometryCollection/TransformCollection.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"

namespace
{
    UGeometryCollection* ResolveCollection(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("collection"), Input)
            && !Params->TryGetStringField(TEXT("path"), Input)
            && !Params->TryGetStringField(TEXT("asset"), Input)
            && !Params->TryGetStringField(TEXT("asset_path"), Input))
        {
            return nullptr;
        }
        if (Input.IsEmpty())
        {
            return nullptr;
        }
        if (Input.StartsWith(TEXT("/")))
        {
            return Cast<UGeometryCollection>(UEditorAssetLibrary::LoadAsset(Input));
        }
        // Short-name fallback through the asset registry.
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UGeometryCollection::StaticClass()->GetClassPathName(), Found);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                return Cast<UGeometryCollection>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    const TCHAR* DamageModelToString(EDamageModelTypeEnum Model)
    {
        switch (Model)
        {
        case EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold: return TEXT("user_defined_damage_threshold");
        case EDamageModelTypeEnum::Chaos_Damage_Model_Material_Strength_And_Connectivity_DamageThreshold: return TEXT("material_strength_and_connectivity");
        default: return TEXT("unknown");
        }
    }

    const TCHAR* ConnectionTypeToString(EClusterConnectionTypeEnum Type)
    {
        switch (Type)
        {
        case EClusterConnectionTypeEnum::Chaos_PointImplicit: return TEXT("point_implicit");
        case EClusterConnectionTypeEnum::Chaos_DelaunayTriangulation: return TEXT("delaunay_triangulation");
        case EClusterConnectionTypeEnum::Chaos_MinimalSpanningSubsetDelaunayTriangulation: return TEXT("minimal_spanning_subset_delaunay");
        case EClusterConnectionTypeEnum::Chaos_PointImplicitAugmentedWithMinimalDelaunay: return TEXT("point_implicit_augmented_minimal_delaunay");
        case EClusterConnectionTypeEnum::Chaos_BoundsOverlapFilteredDelaunayTriangulation: return TEXT("bounds_overlap_filtered_delaunay");
        case EClusterConnectionTypeEnum::Chaos_None: return TEXT("none");
        default: return TEXT("unknown");
        }
    }

    TSharedPtr<FJsonObject> TransformToJson(const FTransform& Xf)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        const FVector L = Xf.GetLocation();
        const FRotator R = Xf.Rotator();
        const FVector S = Xf.GetScale3D();

        TArray<TSharedPtr<FJsonValue>> LArr;
        LArr.Add(MakeShared<FJsonValueNumber>(L.X));
        LArr.Add(MakeShared<FJsonValueNumber>(L.Y));
        LArr.Add(MakeShared<FJsonValueNumber>(L.Z));
        Out->SetArrayField(TEXT("location"), LArr);

        TArray<TSharedPtr<FJsonValue>> RArr;
        RArr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        RArr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        RArr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        Out->SetArrayField(TEXT("rotation"), RArr);

        TArray<TSharedPtr<FJsonValue>> SArr;
        SArr.Add(MakeShared<FJsonValueNumber>(S.X));
        SArr.Add(MakeShared<FJsonValueNumber>(S.Y));
        SArr.Add(MakeShared<FJsonValueNumber>(S.Z));
        Out->SetArrayField(TEXT("scale"), SArr);

        return Out;
    }
}

FSproftChaosEditCommands::FSproftChaosEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftChaosEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("chaos_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown chaos_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("inspect"))
    {
        return HandleInspect(Params);
    }
    if (Op.Equals(TEXT("set_simulation_settings"), ESearchCase::IgnoreCase))
    {
        return HandleSetSimulationSettings(Params);
    }
    if (Op.Equals(TEXT("import_static_mesh"), ESearchCase::IgnoreCase))
    {
        return HandleImportStaticMesh(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("chaos_edit: unsupported op '%s'. Supported: inspect, set_simulation_settings, import_static_mesh"), *Op));
}

TSharedPtr<FJsonObject> FSproftChaosEditCommands::HandleInspect(const TSharedPtr<FJsonObject>& Params)
{
    UGeometryCollection* Collection = ResolveCollection(Params);
    if (!Collection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UGeometryCollection (provide 'collection' or 'path' as /Game/... or short name)"));
    }

    bool bIncludeSources = true;
    Params->TryGetBoolField(TEXT("include_geometry_sources"), bIncludeSources);
    bool bIncludeHistogram = true;
    Params->TryGetBoolField(TEXT("include_per_level_histogram"), bIncludeHistogram);

    int32 MaxSources = 64;
    int32 MaxMaterials = 256;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_sources"), TempNum)) MaxSources = FMath::Max(0, static_cast<int32>(TempNum));
    if (Params->TryGetNumberField(TEXT("max_materials"), TempNum)) MaxMaterials = FMath::Max(0, static_cast<int32>(TempNum));

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("name"), Collection->GetName());
    Out->SetStringField(TEXT("path"), Collection->GetPathName());
    Out->SetStringField(TEXT("class"), Collection->GetClass()->GetName());

    Out->SetBoolField(TEXT("is_empty"), Collection->IsEmpty());
    Out->SetBoolField(TEXT("has_visible_geometry"), Collection->HasVisibleGeometry());
    Out->SetNumberField(TEXT("root_index"), Collection->GetRootIndex());

    // Geometry sources (editor-only).
#if WITH_EDITORONLY_DATA
    if (bIncludeSources)
    {
        TArray<TSharedPtr<FJsonValue>> SourceArr;
        for (int32 SrcIdx = 0; SrcIdx < Collection->GeometrySource.Num() && SourceArr.Num() < MaxSources; ++SrcIdx)
        {
            const FGeometryCollectionSource& Source = Collection->GeometrySource[SrcIdx];
            TSharedPtr<FJsonObject> SrcObj = MakeShared<FJsonObject>();
            SrcObj->SetNumberField(TEXT("index"), SrcIdx);
            SrcObj->SetStringField(TEXT("source_path"), Source.SourceGeometryObject.ToString());
            SrcObj->SetObjectField(TEXT("local_transform"), TransformToJson(Source.LocalTransform));
            SrcObj->SetBoolField(TEXT("split_components"), Source.bSplitComponents);
            SrcObj->SetBoolField(TEXT("set_internal_from_material_index"), Source.bSetInternalFromMaterialIndex);
            SrcObj->SetBoolField(TEXT("add_internal_materials"), Source.bAddInternalMaterials);

            TArray<TSharedPtr<FJsonValue>> MatArr;
            for (const TObjectPtr<UMaterialInterface>& Mat : Source.SourceMaterial)
            {
                if (Mat)
                {
                    MatArr.Add(MakeShared<FJsonValueString>(Mat->GetPathName()));
                }
                else
                {
                    MatArr.Add(MakeShared<FJsonValueString>(FString()));
                }
            }
            SrcObj->SetArrayField(TEXT("source_materials"), MatArr);

            SourceArr.Add(MakeShared<FJsonValueObject>(SrcObj));
        }
        Out->SetArrayField(TEXT("geometry_sources"), SourceArr);
        Out->SetNumberField(TEXT("source_count"), SourceArr.Num());
        Out->SetNumberField(TEXT("source_count_total"), Collection->GeometrySource.Num());
        Out->SetBoolField(TEXT("sources_truncated"), SourceArr.Num() < Collection->GeometrySource.Num());
    }
#else
    Out->SetNumberField(TEXT("source_count_total"), 0);
#endif

    // Walk the underlying FGeometryCollection for fracture levels +
    // cluster info + bone hierarchy depth.
    TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GC = Collection->GetGeometryCollection();
    int32 VertexCount = 0;
    int32 FaceCount = 0;
    int32 GeometryCount = 0;
    int32 TransformCount = 0;
    int32 ClusterCount = 0;
    int32 RigidCount = 0;
    int32 NoneSimCount = 0;
    int32 MaxLevel = 0;
    if (GC.IsValid())
    {
        VertexCount = GC->NumElements(FGeometryCollection::VerticesGroup);
        FaceCount = GC->NumElements(FGeometryCollection::FacesGroup);
        GeometryCount = GC->NumElements(FGeometryCollection::GeometryGroup);
        TransformCount = GC->NumElements(FTransformCollection::TransformGroup);

        // Per-element walks. Both arrays are populated whenever the
        // collection has any transforms.
        for (int32 Idx = 0; Idx < TransformCount; ++Idx)
        {
            if (Idx < GC->SimulationType.Num())
            {
                const int32 SimType = GC->SimulationType[Idx];
                if (SimType == FGeometryCollection::ESimulationTypes::FST_Clustered)
                {
                    ++ClusterCount;
                }
                else if (SimType == FGeometryCollection::ESimulationTypes::FST_Rigid)
                {
                    ++RigidCount;
                }
                else
                {
                    ++NoneSimCount;
                }
            }
        }

        // Per-level histogram.
        if (bIncludeHistogram)
        {
            // Level attribute lives under the Transform group with the
            // public LevelAttribute FName. It may be missing on
            // legacy assets without clustering, so we probe through
            // FindAttribute.
            const TManagedArray<int32>* LevelArr = GC->FindAttribute<int32>(
                FTransformCollection::LevelAttribute, FTransformCollection::TransformGroup);
            TArray<int32> PerLevelTotal;
            TArray<int32> PerLevelCluster;
            if (LevelArr)
            {
                for (int32 Idx = 0; Idx < LevelArr->Num(); ++Idx)
                {
                    int32 Level = (*LevelArr)[Idx];
                    if (Level < 0)
                    {
                        continue;
                    }
                    if (Level > MaxLevel) { MaxLevel = Level; }
                    while (PerLevelTotal.Num() <= Level)
                    {
                        PerLevelTotal.Add(0);
                        PerLevelCluster.Add(0);
                    }
                    ++PerLevelTotal[Level];
                    if (Idx < GC->SimulationType.Num()
                        && GC->SimulationType[Idx] == FGeometryCollection::ESimulationTypes::FST_Clustered)
                    {
                        ++PerLevelCluster[Level];
                    }
                }
            }
            TArray<TSharedPtr<FJsonValue>> LevelArrOut;
            for (int32 Total : PerLevelTotal)
            {
                LevelArrOut.Add(MakeShared<FJsonValueNumber>(Total));
            }
            Out->SetArrayField(TEXT("count_per_level"), LevelArrOut);
            TArray<TSharedPtr<FJsonValue>> ClusterArr;
            for (int32 Cnt : PerLevelCluster)
            {
                ClusterArr.Add(MakeShared<FJsonValueNumber>(Cnt));
            }
            Out->SetArrayField(TEXT("cluster_count_per_level"), ClusterArr);
        }
    }
    Out->SetNumberField(TEXT("vertex_count"), VertexCount);
    Out->SetNumberField(TEXT("face_count"), FaceCount);
    Out->SetNumberField(TEXT("geometry_count"), GeometryCount);
    Out->SetNumberField(TEXT("transform_count"), TransformCount);
    Out->SetNumberField(TEXT("cluster_count"), ClusterCount);
    Out->SetNumberField(TEXT("rigid_count"), RigidCount);
    Out->SetNumberField(TEXT("none_sim_count"), NoneSimCount);
    Out->SetNumberField(TEXT("max_level"), MaxLevel);
    Out->SetNumberField(TEXT("bone_hierarchy_depth"), MaxLevel + 1);

    // Simulation block.
    {
        TSharedPtr<FJsonObject> Sim = MakeShared<FJsonObject>();
        Sim->SetBoolField(TEXT("enable_clustering"), Collection->EnableClustering);
        Sim->SetNumberField(TEXT("cluster_group_index"), Collection->ClusterGroupIndex);
        Sim->SetNumberField(TEXT("max_cluster_level"), Collection->MaxClusterLevel);
        Sim->SetStringField(TEXT("damage_model"), DamageModelToString(Collection->DamageModel));
        Sim->SetBoolField(TEXT("use_size_specific_damage_threshold"), Collection->bUseSizeSpecificDamageThreshold);
        Sim->SetBoolField(TEXT("use_material_damage_modifiers"), Collection->bUseMaterialDamageModifiers);
        Sim->SetBoolField(TEXT("per_cluster_only_damage_threshold"), Collection->PerClusterOnlyDamageThreshold);
        Sim->SetStringField(TEXT("cluster_connection_type"), ConnectionTypeToString(Collection->ClusterConnectionType));
        Sim->SetNumberField(TEXT("connection_graph_bounds_filtering_margin"), Collection->ConnectionGraphBoundsFilteringMargin);

        TArray<TSharedPtr<FJsonValue>> ThreshArr;
        for (float T : Collection->DamageThreshold)
        {
            ThreshArr.Add(MakeShared<FJsonValueNumber>(T));
        }
        Sim->SetArrayField(TEXT("damage_threshold"), ThreshArr);

        // Mass + density toggles.
        Sim->SetBoolField(TEXT("density_from_physics_material"), Collection->bDensityFromPhysicsMaterial);
        Sim->SetBoolField(TEXT("mass_as_density"), Collection->bMassAsDensity);
        Sim->SetNumberField(TEXT("mass"), Collection->Mass);
        Sim->SetNumberField(TEXT("minimum_mass_clamp"), Collection->MinimumMassClamp);
        if (Collection->PhysicsMaterial)
        {
            Sim->SetStringField(TEXT("physics_material_path"), Collection->PhysicsMaterial->GetPathName());
        }

        // Removal.
        Sim->SetBoolField(TEXT("scale_on_removal"), Collection->bScaleOnRemoval);
        Sim->SetBoolField(TEXT("remove_on_max_sleep"), Collection->bRemoveOnMaxSleep);
        Sim->SetBoolField(TEXT("automatic_crumble_partial_clusters"), Collection->bAutomaticCrumblePartialClusters);
        Sim->SetBoolField(TEXT("slow_moving_as_sleeping"), Collection->bSlowMovingAsSleeping);
        Sim->SetNumberField(TEXT("slow_moving_velocity_threshold"), Collection->SlowMovingVelocityThreshold);
        TArray<TSharedPtr<FJsonValue>> SleepArr;
        SleepArr.Add(MakeShared<FJsonValueNumber>(Collection->MaximumSleepTime.X));
        SleepArr.Add(MakeShared<FJsonValueNumber>(Collection->MaximumSleepTime.Y));
        Sim->SetArrayField(TEXT("maximum_sleep_time"), SleepArr);
        TArray<TSharedPtr<FJsonValue>> RemovalArr;
        RemovalArr.Add(MakeShared<FJsonValueNumber>(Collection->RemovalDuration.X));
        RemovalArr.Add(MakeShared<FJsonValueNumber>(Collection->RemovalDuration.Y));
        Sim->SetArrayField(TEXT("removal_duration"), RemovalArr);

        // Misc collision toggles that surface in the asset details.
        Sim->SetBoolField(TEXT("import_collision_from_source"), Collection->bImportCollisionFromSource);
        Sim->SetBoolField(TEXT("optimize_convexes"), Collection->bOptimizeConvexes);

        Out->SetObjectField(TEXT("simulation"), Sim);
    }

    // Materials.
    {
        TArray<TSharedPtr<FJsonValue>> MatArr;
        for (int32 MatIdx = 0; MatIdx < Collection->Materials.Num() && MatArr.Num() < MaxMaterials; ++MatIdx)
        {
            UMaterialInterface* Mat = Collection->Materials[MatIdx];
            TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
            MatObj->SetNumberField(TEXT("index"), MatIdx);
            if (Mat)
            {
                MatObj->SetStringField(TEXT("path"), Mat->GetPathName());
                MatObj->SetStringField(TEXT("class"), Mat->GetClass()->GetName());
            }
            MatArr.Add(MakeShared<FJsonValueObject>(MatObj));
        }
        Out->SetArrayField(TEXT("materials"), MatArr);
        Out->SetNumberField(TEXT("material_count"), MatArr.Num());
        Out->SetNumberField(TEXT("material_count_total"), Collection->Materials.Num());
        Out->SetBoolField(TEXT("materials_truncated"), MatArr.Num() < Collection->Materials.Num());
    }

    // Nanite.
    {
        TSharedPtr<FJsonObject> Nan = MakeShared<FJsonObject>();
        Nan->SetBoolField(TEXT("enable_nanite"), Collection->EnableNanite);
        Nan->SetBoolField(TEXT("enable_nanite_fallback"), Collection->bEnableNaniteFallback);
        Nan->SetNumberField(TEXT("nanite_minimum_residency_kb"), static_cast<double>(Collection->NaniteMinimumResidencyInKB));
        Out->SetObjectField(TEXT("nanite"), Nan);
    }

    // Embedded geometry + auto instances.
    Out->SetNumberField(TEXT("embedded_geometry_count"), Collection->EmbeddedGeometryExemplar.Num());
    Out->SetNumberField(TEXT("auto_instance_mesh_count"), Collection->AutoInstanceMeshes.Num());

    // Size-specific data summary (count only; per-row dump is heavy
    // and stays on the BACKLOG).
    Out->SetNumberField(TEXT("size_specific_data_count"), Collection->SizeSpecificData.Num());

    return Out;
}

namespace
{
    /** Resolve a UStaticMesh by `/Game/...` path or short asset name.
     *  Mirrors the registry-fallback shape the existing inspect tools
     *  use. */
    UStaticMesh* ResolveStaticMesh(const FString& Token)
    {
        if (Token.IsEmpty()) return nullptr;
        if (Token.StartsWith(TEXT("/")))
        {
            return Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(Token));
        }
        FAssetRegistryModule& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        AssetRegistry.Get().GetAssetsByClass(UStaticMesh::StaticClass()->GetClassPathName(), Found);
        for (const FAssetData& Data : Found)
        {
            if (Data.AssetName.ToString().Equals(Token, ESearchCase::IgnoreCase))
            {
                return Cast<UStaticMesh>(Data.GetAsset());
            }
        }
        return nullptr;
    }

    /** Convert a JsonValue to a string token suitable for
     *  FProperty::ImportText_InContainer. Booleans get the engine
     *  spelling ("true" / "false"), numbers ride a printf-style
     *  formatter, strings pass through as-is, and arrays / objects
     *  emit their JSON serialization (which ImportText accepts for
     *  most struct types). */
    FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid()) return FString();
        switch (Value->Type)
        {
        case EJson::Boolean:
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Number:
            return FString::SanitizeFloat(Value->AsNumber());
        case EJson::String:
            return Value->AsString();
        case EJson::Array:
        case EJson::Object:
        {
            FString Out;
            TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
            FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
            return Out;
        }
        default:
            return FString();
        }
    }
}

TSharedPtr<FJsonObject> FSproftChaosEditCommands::HandleSetSimulationSettings(const TSharedPtr<FJsonObject>& Params)
{
    UGeometryCollection* Collection = ResolveCollection(Params);
    if (!Collection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UGeometryCollection (provide 'collection' or 'path' as /Game/... or short name)"));
    }

    const TSharedPtr<FJsonObject>* PropDictPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropDictPtr) || !PropDictPtr || !PropDictPtr->IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'properties' dict"));
    }
    const TSharedPtr<FJsonObject>& PropDict = *PropDictPtr;

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UClass* CollectionClass = Collection->GetClass();
    TArray<TSharedPtr<FJsonValue>> AppliedArr;
    TArray<TSharedPtr<FJsonValue>> SkippedArr;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropDict->Values)
    {
        const FString& KeyName = Pair.Key;
        FProperty* Prop = CollectionClass->FindPropertyByName(FName(*KeyName));
        if (!Prop)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), KeyName);
            Skip->SetStringField(TEXT("reason"), TEXT("property not found"));
            SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }
        const FString TextValue = JsonValueToImportText(Pair.Value);
        const TCHAR* Result = Prop->ImportText_InContainer(*TextValue, Collection, Collection, PPF_None);
        if (Result == nullptr)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), KeyName);
            Skip->SetStringField(TEXT("reason"), FString::Printf(TEXT("ImportText refused '%s'"), *TextValue));
            SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }
        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
        Applied->SetStringField(TEXT("name"), KeyName);
        Applied->SetStringField(TEXT("value"), TextValue);
        Applied->SetStringField(TEXT("property_class"), Prop->GetClass()->GetName());
        AppliedArr.Add(MakeShared<FJsonValueObject>(Applied));
    }

    // Refresh the cached simulation data so the next sim or display
    // tick picks the writes up. InvalidateCollection rebuilds the
    // managed-array cache lazily.
    Collection->InvalidateCollection();
    Collection->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Collection->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("set_simulation_settings"));
    Out->SetStringField(TEXT("collection"), Collection->GetPathName());
    Out->SetArrayField(TEXT("applied"), AppliedArr);
    Out->SetArrayField(TEXT("skipped"), SkippedArr);
    Out->SetNumberField(TEXT("applied_count"), AppliedArr.Num());
    Out->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftChaosEditCommands::HandleImportStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
    UGeometryCollection* Collection = ResolveCollection(Params);
    if (!Collection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not resolve UGeometryCollection (provide 'collection' or 'path' as /Game/... or short name)"));
    }

    FString MeshToken;
    if (!Params->TryGetStringField(TEXT("static_mesh"), MeshToken)
        && !Params->TryGetStringField(TEXT("mesh"), MeshToken)
        && !Params->TryGetStringField(TEXT("source_mesh"), MeshToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'static_mesh' parameter"));
    }
    UStaticMesh* StaticMesh = ResolveStaticMesh(MeshToken);
    if (!StaticMesh)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve UStaticMesh '%s'"), *MeshToken));
    }

    // Optional transform. Default identity. We read flat
    // location / rotation / scale arrays so the JSON shape mirrors
    // scene_compose / actor_inspect.
    FTransform Transform = FTransform::Identity;
    const TSharedPtr<FJsonObject>* XfObj = nullptr;
    if (Params->TryGetObjectField(TEXT("transform"), XfObj) && XfObj && XfObj->IsValid())
    {
        const TSharedPtr<FJsonObject>& Xf = *XfObj;
        const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
        if (Xf->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3)
        {
            Transform.SetLocation(FVector(
                (*LocArr)[0]->AsNumber(),
                (*LocArr)[1]->AsNumber(),
                (*LocArr)[2]->AsNumber()));
        }
        const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
        if (Xf->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() == 3)
        {
            const FRotator R(
                (*RotArr)[0]->AsNumber(),
                (*RotArr)[1]->AsNumber(),
                (*RotArr)[2]->AsNumber());
            Transform.SetRotation(R.Quaternion());
        }
        const TArray<TSharedPtr<FJsonValue>>* SclArr = nullptr;
        if (Xf->TryGetArrayField(TEXT("scale"), SclArr) && SclArr && SclArr->Num() == 3)
        {
            Transform.SetScale3D(FVector(
                (*SclArr)[0]->AsNumber(),
                (*SclArr)[1]->AsNumber(),
                (*SclArr)[2]->AsNumber()));
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bReindexMaterials = true;
    Params->TryGetBoolField(TEXT("reindex_materials"), bReindexMaterials);

    // Pull the static mesh's static materials so the appended geometry
    // inherits its materials. Pass a flat UMaterialInterface* array
    // because the conversion API does not accept FStaticMaterial.
    TArray<UMaterialInterface*> MaterialList;
    MaterialList.Reserve(StaticMesh->GetStaticMaterials().Num());
    for (const FStaticMaterial& Mat : StaticMesh->GetStaticMaterials())
    {
        MaterialList.Add(Mat.MaterialInterface);
    }

    FGeometryCollectionConversion::AppendStaticMesh(StaticMesh, MaterialList, Transform, Collection, bReindexMaterials);

    Collection->InvalidateCollection();
    Collection->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Collection->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), TEXT("import_static_mesh"));
    Out->SetStringField(TEXT("collection"), Collection->GetPathName());
    Out->SetStringField(TEXT("static_mesh"), StaticMesh->GetPathName());
    Out->SetObjectField(TEXT("transform"), TransformToJson(Transform));
    Out->SetBoolField(TEXT("reindex_materials"), bReindexMaterials);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}
