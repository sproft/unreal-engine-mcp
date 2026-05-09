#include "Commands/SproftLandscapeInspectCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/Texture2D.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> Vec3ToJson(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    }

    TArray<TSharedPtr<FJsonValue>> RotToJson(const FRotator& R)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
        Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
        return Arr;
    }

    TArray<TSharedPtr<FJsonValue>> IntPointToJson(const FIntPoint& P)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(P.X));
        Arr.Add(MakeShared<FJsonValueNumber>(P.Y));
        return Arr;
    }

    /** Pull the owning ULevel name through the parent UWorld package, mirroring
     *  the level-naming used by SproftLevelInspectCommands. */
    FString LevelLabel(ULevel* Level)
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

    /** Best-effort path string for an asset pointer; empty when null. */
    FString PathOrEmpty(const UObject* Object)
    {
        return Object ? Object->GetPathName() : FString();
    }

    /** Per-layer record keyed off FLandscapeInfoLayerSettings. The Owner /
     *  WeightBlendMode flag and the visibility-layer comparison both need
     *  the proxy plus the optional VisibilityLayer pointer. */
    TSharedPtr<FJsonObject> LayerRecord(const FLandscapeInfoLayerSettings& Settings,
                                        const ULandscapeLayerInfoObject* VisibilityLayer)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("layer_name"), Settings.LayerName.ToString());

        const ULandscapeLayerInfoObject* LayerInfo = Settings.LayerInfoObj;
        if (LayerInfo)
        {
            Out->SetStringField(TEXT("layer_info_object_name"), LayerInfo->GetName());
            Out->SetStringField(TEXT("layer_info_object_path"), LayerInfo->GetPathName());

            if (UPhysicalMaterial* PhysMat = LayerInfo->GetPhysicalMaterial())
            {
                Out->SetStringField(TEXT("phys_material"), PhysMat->GetPathName());
            }

            // Resolved layer name straight off the layer-info object. Mostly the
            // same as Settings.LayerName but the registered settings can carry
            // a placeholder name for layers that are referenced by material
            // alone, so report both.
            Out->SetStringField(TEXT("resolved_layer_name"), LayerInfo->GetLayerName().ToString());

            // BlendMethod is the public successor to bNoWeightBlend; we expose
            // both the enum byte and the no-blend convenience flag.
            const ELandscapeTargetLayerBlendMethod BlendMethod = LayerInfo->GetBlendMethod();
            Out->SetNumberField(TEXT("blend_method"), static_cast<int32>(BlendMethod));
            Out->SetBoolField(TEXT("is_no_blend"),
                BlendMethod == ELandscapeTargetLayerBlendMethod::None);

            Out->SetBoolField(TEXT("is_visibility_layer"),
                VisibilityLayer != nullptr && LayerInfo == VisibilityLayer);
        }
        else
        {
            Out->SetBoolField(TEXT("is_no_blend"), false);
            Out->SetBoolField(TEXT("is_visibility_layer"), false);
        }

#if WITH_EDITORONLY_DATA
        if (ALandscapeProxy* Owner = Settings.Owner.Get())
        {
            Out->SetStringField(TEXT("owner_proxy"), Owner->GetName());
        }
#endif

        return Out;
    }
}

FSproftLandscapeInspectCommands::FSproftLandscapeInspectCommands()
{
}

TSharedPtr<FJsonObject> FSproftLandscapeInspectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("landscape_inspect"))
    {
        return HandleLandscapeInspect(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown landscape_inspect command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftLandscapeInspectCommands::HandleLandscapeInspect(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    bool bIncludeComponents = false;
    bool bIncludeHeightmaps = true;
    bool bIncludeWeightmaps = false;
    FString NamePattern;
    FString LevelFilter;

    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
        Params->TryGetBoolField(TEXT("include_heightmaps"), bIncludeHeightmaps);
        Params->TryGetBoolField(TEXT("include_weightmaps"), bIncludeWeightmaps);
        Params->TryGetStringField(TEXT("name_pattern"), NamePattern);
        Params->TryGetStringField(TEXT("level_filter"), LevelFilter);
    }
    const FString NamePatternLower = NamePattern.ToLower();
    const FString LevelFilterLower = LevelFilter.ToLower();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("inspect"));
    Result->SetStringField(TEXT("level_name"), World->GetMapName());
    if (UPackage* WorldPackage = World->GetOutermost())
    {
        Result->SetStringField(TEXT("level_path"), WorldPackage->GetName());
    }

    TArray<TSharedPtr<FJsonValue>> LandscapeArr;
    int32 ScannedActors = 0;
    int32 MatchedActors = 0;

    for (ULevel* Level : World->GetLevels())
    {
        if (!Level)
        {
            continue;
        }
        const FString LevelName = LevelLabel(Level);
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
            ALandscape* Landscape = Cast<ALandscape>(Actor);
            if (!Landscape)
            {
                continue;
            }
            ++ScannedActors;

            if (!NamePattern.IsEmpty())
            {
                const bool bNameHit  = Landscape->GetName().ToLower().Contains(NamePatternLower);
                const bool bLabelHit = Landscape->GetActorLabel().ToLower().Contains(NamePatternLower);
                if (!bNameHit && !bLabelHit)
                {
                    continue;
                }
            }
            ++MatchedActors;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Landscape->GetName());
            Entry->SetStringField(TEXT("label"), Landscape->GetActorLabel());
            Entry->SetStringField(TEXT("class"), Landscape->GetClass()->GetName());
            Entry->SetStringField(TEXT("class_path"), Landscape->GetClass()->GetPathName());
            Entry->SetStringField(TEXT("level"), LevelName);

            Entry->SetArrayField(TEXT("location"), Vec3ToJson(Landscape->GetActorLocation()));
            Entry->SetArrayField(TEXT("rotation"), RotToJson(Landscape->GetActorRotation()));
            Entry->SetArrayField(TEXT("scale"),    Vec3ToJson(Landscape->GetActorScale3D()));

            Entry->SetStringField(TEXT("landscape_guid"), Landscape->GetLandscapeGuid().ToString());

            // Section-grid configuration. ComponentSizeQuads * NumSubsections *
            // SubsectionSizeQuads is the canonical "what does each component
            // cover" tuple in heightmap-space.
            Entry->SetNumberField(TEXT("component_size_quads"), Landscape->ComponentSizeQuads);
            Entry->SetNumberField(TEXT("subsection_size_quads"), Landscape->SubsectionSizeQuads);
            Entry->SetNumberField(TEXT("num_subsections"), Landscape->NumSubsections);
            Entry->SetNumberField(TEXT("component_count"), Landscape->LandscapeComponents.Num());
            Entry->SetNumberField(TEXT("streaming_distance_multiplier"),
                Landscape->StreamingDistanceMultiplier);

            // Materials per layer (the proxy's primary material driver and any
            // distinct hole-material override).
            Entry->SetStringField(TEXT("landscape_material"),
                PathOrEmpty(ToRawPtr(Landscape->LandscapeMaterial)));
            if (UMaterialInterface* HoleMat = ToRawPtr(Landscape->LandscapeHoleMaterial))
            {
                Entry->SetStringField(TEXT("landscape_hole_material"),
                    HoleMat->GetPathName());
            }

            // World-space bounding region: GetProxyBounds covers the components
            // (closer to the visual landscape footprint than GetActorBounds,
            // which can include ancillary actors). We split min / max / size
            // for downstream readers that prefer one over the other.
            const FBox ProxyBounds = Landscape->GetProxyBounds();
            if (ProxyBounds.IsValid)
            {
                Entry->SetArrayField(TEXT("proxy_bounds_min"),  Vec3ToJson(ProxyBounds.Min));
                Entry->SetArrayField(TEXT("proxy_bounds_max"),  Vec3ToJson(ProxyBounds.Max));
                Entry->SetArrayField(TEXT("proxy_bounds_size"), Vec3ToJson(ProxyBounds.GetSize()));
            }

            // Editor-only landscape info: extents in component-space + the
            // registered Layers list we walk for the per-layer payload below.
            ULandscapeInfo* LandscapeInfo = nullptr;
#if WITH_EDITOR
            LandscapeInfo = Landscape->GetLandscapeInfo();
#endif
            if (LandscapeInfo)
            {
#if WITH_EDITOR
                int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
                if (LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
                {
                    Entry->SetArrayField(TEXT("xy_extent_min"),
                        IntPointToJson(FIntPoint(MinX, MinY)));
                    Entry->SetArrayField(TEXT("xy_extent_max"),
                        IntPointToJson(FIntPoint(MaxX, MaxY)));
                    Entry->SetArrayField(TEXT("xy_extent_size"),
                        IntPointToJson(FIntPoint(MaxX - MinX, MaxY - MinY)));
                }
#endif

                TArray<TSharedPtr<FJsonValue>> LayerArr;
#if WITH_EDITORONLY_DATA
                for (const FLandscapeInfoLayerSettings& Settings : LandscapeInfo->Layers)
                {
                    LayerArr.Add(MakeShared<FJsonValueObject>(
                        LayerRecord(Settings, ALandscapeProxy::VisibilityLayer)));
                }
#endif
                Entry->SetArrayField(TEXT("layers"), LayerArr);
                Entry->SetNumberField(TEXT("layer_count"), LayerArr.Num());
            }
            else
            {
                Entry->SetArrayField(TEXT("layers"), TArray<TSharedPtr<FJsonValue>>());
                Entry->SetNumberField(TEXT("layer_count"), 0);
            }

            // Heightmap / weightmap textures. We deduplicate per-component
            // pointers so a 100-component landscape that shares 8 atlases
            // still reports 8 entries.
            TSet<UTexture2D*> Heightmaps;
            TSet<UTexture2D*> Weightmaps;
            TArray<TSharedPtr<FJsonValue>> ComponentArr;

            for (ULandscapeComponent* Component : Landscape->LandscapeComponents)
            {
                if (!Component)
                {
                    continue;
                }
                // Pin the const overloads so we get the raw-pointer array form
                // and stay clear of the non-const TObjectPtr<> overload.
                const ULandscapeComponent* ConstComponent = Component;
                if (UTexture2D* Heightmap = ConstComponent->GetHeightmap())
                {
                    Heightmaps.Add(Heightmap);
                }
                const TArray<UTexture2D*>& ComponentWeightmaps =
                    ConstComponent->GetWeightmapTextures();
                for (UTexture2D* WeightmapTexture : ComponentWeightmaps)
                {
                    if (WeightmapTexture)
                    {
                        Weightmaps.Add(WeightmapTexture);
                    }
                }

                if (bIncludeComponents)
                {
                    TSharedPtr<FJsonObject> CompEntry = MakeShared<FJsonObject>();
                    CompEntry->SetStringField(TEXT("name"), Component->GetName());
                    const FIntPoint Section = Component->GetSectionBase();
                    CompEntry->SetArrayField(TEXT("section_base"), IntPointToJson(Section));
                    CompEntry->SetNumberField(TEXT("weightmap_count"), ComponentWeightmaps.Num());
                    const TArray<FWeightmapLayerAllocationInfo>& Allocations =
                        ConstComponent->GetWeightmapLayerAllocations();
                    CompEntry->SetNumberField(TEXT("weightmap_layer_allocation_count"),
                        Allocations.Num());
                    if (UTexture2D* H = ConstComponent->GetHeightmap())
                    {
                        CompEntry->SetStringField(TEXT("heightmap_path"), H->GetPathName());
                    }
                    ComponentArr.Add(MakeShared<FJsonValueObject>(CompEntry));
                }
            }

            Entry->SetNumberField(TEXT("heightmap_texture_count"), Heightmaps.Num());
            Entry->SetNumberField(TEXT("weightmap_texture_count"), Weightmaps.Num());

            if (bIncludeHeightmaps)
            {
                TArray<TSharedPtr<FJsonValue>> HeightArr;
                for (UTexture2D* H : Heightmaps)
                {
                    if (H)
                    {
                        HeightArr.Add(MakeShared<FJsonValueString>(H->GetPathName()));
                    }
                }
                Entry->SetArrayField(TEXT("heightmap_textures"), HeightArr);
            }
            if (bIncludeWeightmaps)
            {
                TArray<TSharedPtr<FJsonValue>> WeightArr;
                for (UTexture2D* W : Weightmaps)
                {
                    if (W)
                    {
                        WeightArr.Add(MakeShared<FJsonValueString>(W->GetPathName()));
                    }
                }
                Entry->SetArrayField(TEXT("weightmap_textures"), WeightArr);
            }
            if (bIncludeComponents)
            {
                Entry->SetArrayField(TEXT("components"), ComponentArr);
            }

            LandscapeArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    Result->SetArrayField(TEXT("landscapes"), LandscapeArr);
    Result->SetNumberField(TEXT("landscape_count"), LandscapeArr.Num());
    Result->SetNumberField(TEXT("scanned_actors"), ScannedActors);
    Result->SetNumberField(TEXT("matched_actors"), MatchedActors);
    return Result;
}
