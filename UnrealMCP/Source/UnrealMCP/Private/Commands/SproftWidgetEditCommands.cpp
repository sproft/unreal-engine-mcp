#include "Commands/SproftWidgetEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/PanelSlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/Widget.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "EditorAssetLibrary.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/OutputDeviceNull.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/Texture.h"
#include "Layout/Margin.h"
#include "Materials/MaterialInterface.h"
#include "MovieScene.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintExtension.h"
#include "INotifyFieldValueChanged.h"
#include "Bindings/MVVMConversionFunctionHelper.h"
#include "MVVMBlueprintFunctionReference.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMPropertyPath.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMFieldVariant.h"

namespace
{
    /** Split "/Game/Foo/Bar" into ("/Game/Foo/", "Bar"). */
    void WidgetEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
    {
        FString Trim = InPath;
        Trim.TrimEndInline();
        Trim.RemoveFromEnd(TEXT("/"));

        int32 LastSlash = INDEX_NONE;
        if (Trim.FindLastChar('/', LastSlash))
        {
            OutPackageDir = Trim.Left(LastSlash + 1);
            OutAssetName = Trim.Mid(LastSlash + 1);
        }
        else
        {
            OutPackageDir = TEXT("/Game/");
            OutAssetName = Trim;
        }

        // Strip a trailing ".AssetName" suffix if the caller passed an object path.
        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Resolve a UUserWidget subclass for the parent_class param. Defaults to UUserWidget. */
    UClass* ResolveUserWidgetParent(const FString& ParentClassPath)
    {
        if (ParentClassPath.IsEmpty())
        {
            return UUserWidget::StaticClass();
        }

        // Full object path?
        if (ParentClassPath.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UUserWidget>(nullptr, *ParentClassPath))
            {
                return Loaded;
            }
            // Caller may have passed a Blueprint asset path without a "_C" suffix.
            const FString WithSuffix = ParentClassPath + TEXT("_C");
            if (UClass* LoadedSuffix = LoadClass<UUserWidget>(nullptr, *WithSuffix))
            {
                return LoadedSuffix;
            }
            return nullptr;
        }

        // Short name lookup, e.g. "UUserWidget".
        if (UClass* Found = FindObject<UClass>(nullptr, *ParentClassPath))
        {
            if (Found->IsChildOf(UUserWidget::StaticClass()))
            {
                return Found;
            }
        }
        const FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *ParentClassPath);
        if (UClass* UMG = LoadClass<UUserWidget>(nullptr, *UMGPath))
        {
            return UMG;
        }
        return nullptr;
    }

    /** Resolve a UPanelWidget subclass for an optional root panel. Defaults to UCanvasPanel. */
    UClass* ResolveRootPanelClass(const FString& RootPanelClass)
    {
        if (RootPanelClass.IsEmpty())
        {
            return UCanvasPanel::StaticClass();
        }

        if (RootPanelClass.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UPanelWidget>(nullptr, *RootPanelClass))
            {
                return Loaded;
            }
            return nullptr;
        }

        if (UClass* Found = FindObject<UClass>(nullptr, *RootPanelClass))
        {
            if (Found->IsChildOf(UPanelWidget::StaticClass()))
            {
                return Found;
            }
        }
        const FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *RootPanelClass);
        if (UClass* UMG = LoadClass<UPanelWidget>(nullptr, *UMGPath))
        {
            return UMG;
        }
        return nullptr;
    }

    /** Render a JSON value as ImportText input. Mirrors the helper used
     *  in bp_component / scene_compose so the slot-property surface
     *  accepts the same dict shape as the rest of the property tools. */
    FString WidgetEdit_JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
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

    /** Map a short widget-type string to its UClass. Returns nullptr if unknown. */
    UClass* ResolveChildWidgetClass(const FString& WidgetType)
    {
        const FString Type = WidgetType.ToLower();

        if (Type == TEXT("vertical_box") || Type == TEXT("verticalbox"))
        {
            return UVerticalBox::StaticClass();
        }
        if (Type == TEXT("horizontal_box") || Type == TEXT("horizontalbox"))
        {
            return UHorizontalBox::StaticClass();
        }
        if (Type == TEXT("canvas_panel") || Type == TEXT("canvaspanel"))
        {
            return UCanvasPanel::StaticClass();
        }
        if (Type == TEXT("overlay"))
        {
            return UOverlay::StaticClass();
        }
        if (Type == TEXT("scroll_box") || Type == TEXT("scrollbox"))
        {
            return UScrollBox::StaticClass();
        }
        if (Type == TEXT("border"))
        {
            return UBorder::StaticClass();
        }
        if (Type == TEXT("size_box") || Type == TEXT("sizebox"))
        {
            return USizeBox::StaticClass();
        }
        if (Type == TEXT("spacer"))
        {
            return USpacer::StaticClass();
        }
        if (Type == TEXT("progress_bar") || Type == TEXT("progressbar"))
        {
            return UProgressBar::StaticClass();
        }
        if (Type == TEXT("text_block") || Type == TEXT("textblock") || Type == TEXT("text"))
        {
            return UTextBlock::StaticClass();
        }
        if (Type == TEXT("button"))
        {
            return UButton::StaticClass();
        }
        if (Type == TEXT("image"))
        {
            return UImage::StaticClass();
        }

        // Fall back to a class lookup. Accept fully qualified names like
        // "/Script/UMG.RichTextBlock" or short class names.
        if (WidgetType.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UWidget>(nullptr, *WidgetType))
            {
                return Loaded;
            }
            return nullptr;
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *WidgetType))
        {
            if (Found->IsChildOf(UWidget::StaticClass()))
            {
                return Found;
            }
        }
        const FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *WidgetType);
        if (UClass* UMG = LoadClass<UWidget>(nullptr, *UMGPath))
        {
            return UMG;
        }
        return nullptr;
    }
}

FSproftWidgetEditCommands::FSproftWidgetEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("widget_edit"))
    {
        return HandleWidgetEdit(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown widget edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::HandleWidgetEdit(const TSharedPtr<FJsonObject>& Params)
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

    if (Operation == TEXT("create_widget_blueprint") || Operation == TEXT("create"))
    {
        return CreateWidgetBlueprint(Params);
    }
    if (Operation == TEXT("add_child_widget") || Operation == TEXT("add_child"))
    {
        return AddChildWidget(Params);
    }
    if (Operation == TEXT("set_slot_property") || Operation == TEXT("set_slot")
        || Operation == TEXT("slot_set") || Operation == TEXT("set_slot_properties"))
    {
        return SetSlotProperty(Params);
    }
    if (Operation == TEXT("add_animation") || Operation == TEXT("create_animation"))
    {
        return AddAnimation(Params);
    }
    if (Operation == TEXT("add_animation_track") || Operation == TEXT("add_track")
        || Operation == TEXT("animation_add_track"))
    {
        return AddAnimationTrack(Params);
    }
    if (Operation == TEXT("add_keyframe") || Operation == TEXT("add_animation_keyframe")
        || Operation == TEXT("set_keyframe") || Operation == TEXT("animation_add_keyframe"))
    {
        return AddAnimationKeyframe(Params);
    }
    if (Operation == TEXT("set_viewmodel") || Operation == TEXT("add_viewmodel")
        || Operation == TEXT("bind_viewmodel"))
    {
        return SetViewModel(Params);
    }
    if (Operation == TEXT("add_property_binding") || Operation == TEXT("add_binding")
        || Operation == TEXT("bind_property"))
    {
        return AddPropertyBinding(Params);
    }
    if (Operation == TEXT("set_binding_conversion") || Operation == TEXT("set_conversion_function")
        || Operation == TEXT("set_conversion") || Operation == TEXT("bind_conversion"))
    {
        return SetBindingConversion(Params);
    }
    if (Operation == TEXT("add_event_binding") || Operation == TEXT("bind_event")
        || Operation == TEXT("add_event") || Operation == TEXT("add_widget_event"))
    {
        return AddEventBinding(Params);
    }
    if (Operation == TEXT("set_widget_style") || Operation == TEXT("set_style")
        || Operation == TEXT("apply_widget_style") || Operation == TEXT("apply_style"))
    {
        return SetWidgetStyle(Params);
    }
    if (Operation == TEXT("set_widget_brush") || Operation == TEXT("set_brush")
        || Operation == TEXT("apply_widget_brush") || Operation == TEXT("apply_brush"))
    {
        return SetWidgetBrush(Params);
    }
    if (Operation == TEXT("set_widget_navigation") || Operation == TEXT("set_navigation")
        || Operation == TEXT("widget_navigation") || Operation == TEXT("set_nav"))
    {
        return SetWidgetNavigation(Params);
    }
    if (Operation == TEXT("set_canvas_slot") || Operation == TEXT("set_canvas_panel_slot")
        || Operation == TEXT("canvas_slot") || Operation == TEXT("set_anchored_slot"))
    {
        return SetCanvasSlot(Params);
    }
    if (Operation == TEXT("set_overlay_slot") || Operation == TEXT("overlay_slot")
        || Operation == TEXT("set_overlay") || Operation == TEXT("set_overlay_alignment"))
    {
        return SetOverlaySlot(Params);
    }
    if (Operation == TEXT("set_box_slot") || Operation == TEXT("box_slot")
        || Operation == TEXT("set_vertical_box_slot") || Operation == TEXT("set_horizontal_box_slot")
        || Operation == TEXT("vertical_box_slot") || Operation == TEXT("horizontal_box_slot"))
    {
        return SetBoxSlot(Params);
    }
    if (Operation == TEXT("set_grid_slot") || Operation == TEXT("grid_slot")
        || Operation == TEXT("set_grid_panel_slot") || Operation == TEXT("grid_panel_slot"))
    {
        return SetGridSlot(Params);
    }
    if (Operation == TEXT("set_uniform_grid_slot") || Operation == TEXT("uniform_grid_slot")
        || Operation == TEXT("set_uniform_grid_panel_slot") || Operation == TEXT("uniform_grid_panel_slot"))
    {
        return SetUniformGridSlot(Params);
    }
    if (Operation == TEXT("set_wrap_box_slot") || Operation == TEXT("wrap_box_slot")
        || Operation == TEXT("set_wrapbox_slot") || Operation == TEXT("wrapbox_slot"))
    {
        return SetWrapBoxSlot(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported widget_edit operation '%s'. Supported: create_widget_blueprint, add_child_widget, set_slot_property, add_animation, add_animation_track, add_keyframe, set_viewmodel, add_property_binding, set_binding_conversion, add_event_binding, set_widget_style, set_widget_brush, set_widget_navigation, set_canvas_slot, set_overlay_slot, set_box_slot, set_grid_slot, set_uniform_grid_slot, set_wrap_box_slot"), *Operation));
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path, got '%s'"), *PackagePath));
    }

    FString ParentClassPath;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);

    FString RootPanelClass;
    Params->TryGetStringField(TEXT("root_panel_class"), RootPanelClass);

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    FString PackageDir;
    FString AssetName;
    WidgetEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }

    const FString AssetObjectPath = PackageDir + AssetName;
    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    UClass* ParentClass = ResolveUserWidgetParent(ParentClassPath);
    if (!ParentClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent_class '%s'. Pass a UUserWidget subclass path or short name."), *ParentClassPath));
    }

    UClass* RootPanel = ResolveRootPanelClass(RootPanelClass);
    if (!RootPanel)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve root_panel_class '%s'. Pass a UPanelWidget subclass path or short name."), *RootPanelClass));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UWidgetBlueprint* NewWBP = CastChecked<UWidgetBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(
            ParentClass,
            Package,
            *AssetName,
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(),
            FName("Sproft.widget_edit")));

    if (!NewWBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UWidgetBlueprint"));
    }

    if (NewWBP->WidgetTree && NewWBP->WidgetTree->RootWidget == nullptr)
    {
        UWidget* Root = NewWBP->WidgetTree->ConstructWidget<UWidget>(RootPanel);
        if (Root)
        {
            NewWBP->WidgetTree->RootWidget = Root;
            NewWBP->OnVariableAdded(Root->GetFName());
        }
    }

    FAssetRegistryModule::AssetCreated(NewWBP);
    FBlueprintEditorUtils::MarkBlueprintAsModified(NewWBP);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_widget_blueprint"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    ResultObj->SetStringField(TEXT("root_panel_class"), RootPanel->GetPathName());
    if (NewWBP->WidgetTree && NewWBP->WidgetTree->RootWidget)
    {
        ResultObj->SetStringField(TEXT("root_widget_name"), NewWBP->WidgetTree->RootWidget->GetName());
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddChildWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString WidgetType;
    if (!Params->TryGetStringField(TEXT("widget_type"), WidgetType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_type' parameter"));
    }

    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }

    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    bool bExposeAsVariable = true;
    Params->TryGetBoolField(TEXT("expose_as_variable"), bExposeAsVariable);

    FString InitialText;
    Params->TryGetStringField(TEXT("text"), InitialText);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("WidgetBlueprint has no WidgetTree"));
    }

    UClass* ChildClass = ResolveChildWidgetClass(WidgetType);
    if (!ChildClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported widget_type '%s'. Pass one of: vertical_box, horizontal_box, canvas_panel, overlay, scroll_box, border, size_box, spacer, progress_bar, text_block, button, image, or a fully qualified UWidget class path."), *WidgetType));
    }

    // Resolve the parent panel.
    UPanelWidget* ParentPanel = nullptr;
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
        if (!ParentPanel)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Root widget is not a UPanelWidget. Pass 'parent_name' or rebuild the asset with a panel root."));
        }
    }
    else
    {
        UWidget* Found = WBP->WidgetTree->FindWidget(FName(*ParentName));
        if (!Found)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not find parent widget '%s' in %s"), *ParentName, *WidgetBlueprintPath));
        }
        ParentPanel = Cast<UPanelWidget>(Found);
        if (!ParentPanel)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent widget '%s' is not a UPanelWidget"), *ParentName));
        }
    }

    // Refuse a duplicate name in the tree, since UWidgetTree requires unique
    // FNames per widget asset.
    if (UWidget* Existing = WBP->WidgetTree->FindWidget(FName(*WidgetName)))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("A widget named '%s' already exists in %s"), *WidgetName, *WidgetBlueprintPath));
    }

    UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(ChildClass, FName(*WidgetName));
    if (!NewWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to construct widget of type '%s'"), *WidgetType));
    }

    NewWidget->bIsVariable = bExposeAsVariable;

    UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
    if (!Slot)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent panel '%s' refused to attach the new widget"), *ParentPanel->GetName()));
    }

    // Apply a small set of optional initial properties.
    if (!InitialText.IsEmpty())
    {
        if (UTextBlock* AsText = Cast<UTextBlock>(NewWidget))
        {
            AsText->SetText(FText::FromString(InitialText));
        }
    }

    if (bExposeAsVariable)
    {
        WBP->OnVariableAdded(NewWidget->GetFName());
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_child_widget"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
    ResultObj->SetStringField(TEXT("widget_type"), WidgetType);
    ResultObj->SetStringField(TEXT("widget_class"), ChildClass->GetPathName());
    ResultObj->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    ResultObj->SetBoolField(TEXT("expose_as_variable"), bExposeAsVariable);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetSlotProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName)
        && !Params->TryGetStringField(TEXT("target"), WidgetName)
        && !Params->TryGetStringField(TEXT("name"), WidgetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !(*PropsObj).IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'properties' object"));
    }
    if ((*PropsObj)->Values.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'properties' object is empty"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("WidgetBlueprint has no WidgetTree"));
    }

    UWidget* Found = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Found)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find widget '%s' in %s"), *WidgetName, *WidgetBlueprintPath));
    }

    UPanelSlot* Slot = Found->Slot;
    if (!Slot)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget '%s' has no Slot. The root widget has no parent panel; only children of a panel widget carry a slot."), *WidgetName));
    }

    UClass* SlotClass = Slot->GetClass();

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    FOutputDeviceNull NullDevice;
    for (const auto& Pair : (*PropsObj)->Values)
    {
        const FString& PropName = Pair.Key;
        const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

        FProperty* Prop = FindFProperty<FProperty>(SlotClass, *PropName);
        if (!Prop)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), PropName);
            Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
            SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        const FString TextValue = WidgetEdit_JsonValueToImportText(JsonVal);
        const TCHAR* TextPtr = *TextValue;
        const TCHAR* Result = Prop->ImportText_InContainer(
            TextPtr, Slot, Slot, PPF_None, &NullDevice);
        if (Result == nullptr)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), PropName);
            Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
            Skip->SetStringField(TEXT("attempted_value"), TextValue);
            SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
        Applied->SetStringField(TEXT("name"), PropName);
        Applied->SetStringField(TEXT("type"), Prop->GetCPPType());
        AppliedJson.Add(MakeShared<FJsonValueObject>(Applied));
    }

    // Push the slot's edits back through the runtime hook so a re-layout
    // tick picks them up. Most slot classes implement SynchronizeProperties()
    // to copy serialised state onto the live SObjectWidget; the base
    // UPanelSlot calls it from PostEditChangeProperty, so we replicate that
    // here for the reflective dict path.
    Slot->SynchronizeProperties();

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_slot_property"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
    ResultObj->SetStringField(TEXT("slot_class"), SlotClass->GetName());
    ResultObj->SetStringField(TEXT("slot_class_path"), SlotClass->GetPathName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedJson.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Resolve a UMovieSceneTrack subclass by short token, full
     *  `/Script/Module.ClassName` path, or bare class name. Mirrors the
     *  resolver `sequencer_edit add_track` uses, narrowed to the tracks
     *  designers actually drop on a Widget animation (transform-style
     *  tracks plus the property tracks UMG materializes most often). */
    UClass* WidgetEdit_ResolveTrackClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Token.ToLower();
        struct FShortTokenMap
        {
            const TCHAR* Token;
            const TCHAR* Path;
        };
        static const FShortTokenMap Map[] = {
            { TEXT("float"),     TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack") },
            { TEXT("color"),     TEXT("/Script/MovieSceneTracks.MovieSceneColorTrack") },
            { TEXT("vector"),    TEXT("/Script/MovieSceneTracks.MovieSceneVectorTrack") },
            { TEXT("vector2d"),  TEXT("/Script/MovieSceneTracks.MovieSceneVectorTrack") },
            { TEXT("transform"), TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack") },
            { TEXT("visibility"),TEXT("/Script/MovieSceneTracks.MovieSceneVisibilityTrack") },
            { TEXT("bool"),      TEXT("/Script/MovieSceneTracks.MovieSceneBoolTrack") },
            { TEXT("byte"),      TEXT("/Script/MovieSceneTracks.MovieSceneByteTrack") },
            { TEXT("event"),     TEXT("/Script/MovieSceneTracks.MovieSceneEventTrack") },
            { TEXT("audio"),     TEXT("/Script/MovieSceneTracks.MovieSceneAudioTrack") },
            { TEXT("material"),  TEXT("/Script/MovieSceneTracks.MovieSceneComponentMaterialTrack") },
        };
        for (const FShortTokenMap& Entry : Map)
        {
            if (Lower == Entry.Token)
            {
                if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, Entry.Path))
                {
                    return Loaded;
                }
            }
        }

        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UMovieSceneTrack::StaticClass()))
            {
                return Found;
            }
        }
        const FString TracksPath = FString::Printf(TEXT("/Script/MovieSceneTracks.%s"), *Token);
        if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, *TracksPath))
        {
            return Loaded;
        }
        return nullptr;
    }

    /** Locate a UWidgetAnimation by FName on a target UWidgetBlueprint.
     *  Matches against the sub-object name (which is what the editor
     *  surfaces in the Animations panel) and falls back to the
     *  asset's display label. */
    UWidgetAnimation* WidgetEdit_FindAnimation(UWidgetBlueprint* WBP, const FString& AnimationName)
    {
        if (!WBP || AnimationName.IsEmpty())
        {
            return nullptr;
        }
        const FName TargetName(*AnimationName);
        for (TObjectPtr<UWidgetAnimation>& Anim : WBP->Animations)
        {
            UWidgetAnimation* Cur = Anim.Get();
            if (!Cur)
            {
                continue;
            }
            if (Cur->GetFName() == TargetName)
            {
                return Cur;
            }
            if (Cur->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                return Cur;
            }
#if WITH_EDITOR
            if (Cur->GetDisplayLabel().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                return Cur;
            }
#endif
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString AnimationName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName)
        && !Params->TryGetStringField(TEXT("name"), AnimationName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'animation_name' parameter"));
    }

    double Duration = 1.0;
    if (Params->HasField(TEXT("duration")))
    {
        Duration = Params->GetNumberField(TEXT("duration"));
    }
    else if (Params->HasField(TEXT("duration_seconds")))
    {
        Duration = Params->GetNumberField(TEXT("duration_seconds"));
    }
    if (Duration <= 0.0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'duration' must be greater than zero, got %f"), Duration));
    }

    double DisplayRateNumerator = 20.0;
    double DisplayRateDenominator = 1.0;
    if (Params->HasField(TEXT("display_rate")))
    {
        DisplayRateNumerator = Params->GetNumberField(TEXT("display_rate"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }

    // Refuse a duplicate name to keep the variable surface stable; the editor
    // does the same when the user types a duplicate label.
    const FName TargetName(*AnimationName);
    for (const TObjectPtr<UWidgetAnimation>& Existing : WBP->Animations)
    {
        if (UWidgetAnimation* Cur = Existing.Get())
        {
            if (Cur->GetFName() == TargetName
                || Cur->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("An animation named '%s' already exists on %s"), *AnimationName, *WidgetBlueprintPath));
            }
        }
    }

    UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WBP, TargetName, RF_Transactional);
    if (!NewAnim)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UWidgetAnimation"));
    }

    UMovieScene* NewScene = NewObject<UMovieScene>(NewAnim, TargetName, RF_Transactional);
    if (!NewScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UMovieScene for animation"));
    }
    NewAnim->MovieScene = NewScene;
#if WITH_EDITOR
    NewAnim->SetDisplayLabel(AnimationName);
#endif

    // Match the AnimationTabSummoner default: 20 fps display rate. The
    // tick resolution stays on the engine default which is what the
    // Sequencer editor uses for new widget animations.
    NewScene->SetDisplayRate(FFrameRate(
        FMath::Max<int32>(1, static_cast<int32>(DisplayRateNumerator)),
        FMath::Max<int32>(1, static_cast<int32>(DisplayRateDenominator))));

    const FFrameTime EndFrame = Duration * NewScene->GetTickResolution();
    // The MovieScene uses an inclusive start / exclusive end bound, so we
    // bump the end by one tick to mirror the AnimationTabSummoner pattern.
    NewScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame.FrameNumber + 1));
#if WITH_EDITORONLY_DATA
    NewScene->GetEditorData().WorkStart = 0.0;
    NewScene->GetEditorData().WorkEnd = static_cast<float>(Duration);
#endif

    WBP->Animations.Add(NewAnim);
    WBP->OnVariableAdded(TargetName);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_animation"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
    ResultObj->SetNumberField(TEXT("duration"), Duration);
    ResultObj->SetNumberField(TEXT("display_rate_numerator"), DisplayRateNumerator);
    ResultObj->SetNumberField(TEXT("display_rate_denominator"), DisplayRateDenominator);
    ResultObj->SetNumberField(TEXT("animation_count"), WBP->Animations.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddAnimationTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString AnimationName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName)
        && !Params->TryGetStringField(TEXT("animation"), AnimationName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'animation_name' parameter"));
    }

    FString TargetWidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), TargetWidgetName)
        && !Params->TryGetStringField(TEXT("target_widget"), TargetWidgetName)
        && !Params->TryGetStringField(TEXT("target"), TargetWidgetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter (target widget on the WBP for the binding)"));
    }

    FString TrackToken;
    if (!Params->TryGetStringField(TEXT("track_class"), TrackToken)
        && !Params->TryGetStringField(TEXT("class"), TrackToken)
        && !Params->TryGetStringField(TEXT("track"), TrackToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'track_class' parameter"));
    }

    FString PropertyPath;
    Params->TryGetStringField(TEXT("property_path"), PropertyPath);
    if (PropertyPath.IsEmpty())
    {
        Params->TryGetStringField(TEXT("property"), PropertyPath);
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }

    UWidgetAnimation* Animation = WidgetEdit_FindAnimation(WBP, AnimationName);
    if (!Animation)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find animation '%s' on %s. Run widget_edit add_animation first."), *AnimationName, *WidgetBlueprintPath));
    }
    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Animation '%s' has no UMovieScene; widget_edit add_animation will repair this asset."), *AnimationName));
    }

    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("WidgetBlueprint has no WidgetTree"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*TargetWidgetName));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find target widget '%s' in %s"), *TargetWidgetName, *WidgetBlueprintPath));
    }

    UClass* TrackClass = WidgetEdit_ResolveTrackClass(TrackToken);
    if (!TrackClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve track class '%s'. Pass a short token (float / color / vector / transform / visibility / event / material / audio) or a UMovieSceneTrack subclass path."), *TrackToken));
    }

    // Reuse an existing FWidgetAnimationBinding for the same widget, or
    // create a fresh possessable + binding pair. This mirrors how the
    // editor wires a new track on an existing widget row in the Sequencer
    // panel: one MovieScene possessable + one FWidgetAnimationBinding the
    // runtime maps back through.
    FGuid BindingGuid;
    bool bReusedBinding = false;
    for (const FWidgetAnimationBinding& ExistingBind : Animation->GetBindings())
    {
        if (ExistingBind.WidgetName == TargetWidget->GetFName())
        {
            BindingGuid = ExistingBind.AnimationGuid;
            bReusedBinding = true;
            break;
        }
    }

    if (!BindingGuid.IsValid())
    {
        BindingGuid = MovieScene->AddPossessable(TargetWidget->GetName(), TargetWidget->GetClass());
        if (!BindingGuid.IsValid())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("UMovieScene::AddPossessable failed for widget '%s'"), *TargetWidgetName));
        }

        // Wire the runtime binding so playback resolves the GUID back to
        // this UWidget once the preview / live UUserWidget instances spin up.
        // UWidgetAnimation::BindPossessableObject expects a UUserWidget
        // context that does not exist at asset-author time, so we write the
        // FWidgetAnimationBinding row directly. UMG's runtime resolution
        // path goes through FWidgetAnimationBinding::FindRuntimeObject,
        // which takes a WidgetTree + UUserWidget pair, so the binding row
        // is enough to round-trip the GUID at play time.
        FWidgetAnimationBinding NewBinding;
        NewBinding.WidgetName = TargetWidget->GetFName();
        NewBinding.AnimationGuid = BindingGuid;
        NewBinding.bIsRootWidget = (WBP->WidgetTree->RootWidget == TargetWidget);
        Animation->AnimationBindings.Add(NewBinding);
    }

    UMovieSceneTrack* NewTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
    if (!NewTrack)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMovieScene::AddTrack returned null for class '%s' on binding for widget '%s'"), *TrackClass->GetName(), *TargetWidgetName));
    }

    // Property tracks expect a property path ImportText'd onto the
    // reflected `PropertyPath` member; rather than tying us to per-class
    // includes for every property-track subclass, we route through the
    // reflection database. Older property-track classes use FName
    // PropertyName + FString PropertyPath; newer ones consolidate on a
    // PropertyPath name. We write whichever one the resolved class
    // declares. Designers calling without a property_path (transform /
    // visibility / event / material on a component slot) leave this
    // alone; the next Sequencer panel open populates the row.
    bool bAppliedPropertyPath = false;
    if (!PropertyPath.IsEmpty())
    {
        FOutputDeviceNull NullDevice;
        for (const TCHAR* PropName : { TEXT("PropertyPath"), TEXT("PropertyName") })
        {
            if (FProperty* Prop = FindFProperty<FProperty>(TrackClass, FName(PropName)))
            {
                if (Prop->ImportText_InContainer(*PropertyPath, NewTrack, NewTrack, PPF_None, &NullDevice) != nullptr)
                {
                    bAppliedPropertyPath = true;
                    break;
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_animation_track"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
    ResultObj->SetStringField(TEXT("widget_name"), TargetWidgetName);
    ResultObj->SetStringField(TEXT("track_class"), TrackClass->GetName());
    ResultObj->SetStringField(TEXT("track_class_path"), TrackClass->GetPathName());
    ResultObj->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
    ResultObj->SetBoolField(TEXT("reused_binding"), bReusedBinding);
    if (!PropertyPath.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("property_path"), PropertyPath);
        ResultObj->SetBoolField(TEXT("property_path_applied"), bAppliedPropertyPath);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Walk a UMovieScene's master + binding-scoped track arrays in a
     *  stable order and return the entry at the global `Index`. Master
     *  tracks come first, then binding tracks in `GetBindings()` order.
     *  When the index resolves to a binding-scoped track we surface the
     *  owning binding GUID so the caller can echo it. */
    UMovieSceneTrack* WidgetEdit_ResolveTrackByIndex(UMovieScene* MovieScene, int32 Index, FGuid& OutOwningGuid)
    {
        if (!MovieScene || Index < 0)
        {
            return nullptr;
        }
        const TArray<UMovieSceneTrack*>& MasterTracks = MovieScene->GetTracks();
        if (Index < MasterTracks.Num())
        {
            return MasterTracks[Index];
        }
        int32 Cursor = MasterTracks.Num();
        for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
        {
            const TArray<UMovieSceneTrack*>& BindingTracks = Binding.GetTracks();
            if (Index < Cursor + BindingTracks.Num())
            {
                OutOwningGuid = Binding.GetObjectGuid();
                return BindingTracks[Index - Cursor];
            }
            Cursor += BindingTracks.Num();
        }
        return nullptr;
    }

    /** Pull `[x, y, z, w?]` channels from a JSON value. Number returns
     *  one channel; arrays return two to four. Returns the channel
     *  count, zero on failure. */
    int32 WidgetEdit_ReadKeyValueChannels(const TSharedPtr<FJsonValue>& Value, double Out[4])
    {
        Out[0] = Out[1] = Out[2] = Out[3] = 0.0;
        if (!Value.IsValid())
        {
            return 0;
        }
        if (Value->Type == EJson::Number)
        {
            Out[0] = Value->AsNumber();
            return 1;
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            const int32 N = FMath::Min(Arr.Num(), 4);
            for (int32 I = 0; I < N; ++I)
            {
                Out[I] = Arr[I]->AsNumber();
            }
            return N;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            if (!Obj.IsValid())
            {
                return 0;
            }
            int32 Read = 0;
            auto TryAxis = [&](const TCHAR* PrimaryKey, const TCHAR* AltKey)
            {
                double V = 0.0;
                if (Obj->TryGetNumberField(PrimaryKey, V) || Obj->TryGetNumberField(AltKey, V))
                {
                    Out[Read++] = V;
                    return true;
                }
                return false;
            };
            if (Obj->HasField(TEXT("x")) || Obj->HasField(TEXT("X")))
            {
                TryAxis(TEXT("x"), TEXT("X"));
                TryAxis(TEXT("y"), TEXT("Y"));
                TryAxis(TEXT("z"), TEXT("Z"));
                TryAxis(TEXT("w"), TEXT("W"));
            }
            else
            {
                TryAxis(TEXT("r"), TEXT("R"));
                TryAxis(TEXT("g"), TEXT("G"));
                TryAxis(TEXT("b"), TEXT("B"));
                TryAxis(TEXT("a"), TEXT("A"));
            }
            return Read;
        }
        return 0;
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddAnimationKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString AnimationName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName)
        && !Params->TryGetStringField(TEXT("animation"), AnimationName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'animation_name' parameter"));
    }

    int32 TrackIndex = -1;
    {
        double TrackIndexValue = -1.0;
        if (!Params->TryGetNumberField(TEXT("track_index"), TrackIndexValue)
            && !Params->TryGetNumberField(TEXT("track"), TrackIndexValue))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'track_index' parameter"));
        }
        TrackIndex = static_cast<int32>(TrackIndexValue);
    }

    // Time placement. `frame` (integer FFrameNumber on tick resolution)
    // wins over `time` (float seconds), mirroring animation_edit
    // add_notify and add_sync_marker. We translate seconds through the
    // MovieScene's tick resolution since FFrameNumber storage is on the
    // tick scale.
    double FrameValue = 0.0;
    bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameValue);
    double TimeValue = 0.0;
    bool bHasTime = Params->TryGetNumberField(TEXT("time"), TimeValue);
    if (!bHasFrame && !bHasTime)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("add_keyframe: one of 'frame' (int, in tick resolution) or 'time' (float seconds) is required"));
    }

    if (!Params->HasField(TEXT("value")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }
    const TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));

    FString InterpolationToken;
    Params->TryGetStringField(TEXT("interpolation"), InterpolationToken);
    InterpolationToken = InterpolationToken.ToLower();
    enum class EKeyShape { Cubic, Linear, Constant };
    EKeyShape KeyShape = EKeyShape::Cubic;
    if (InterpolationToken == TEXT("linear")) { KeyShape = EKeyShape::Linear; }
    else if (InterpolationToken == TEXT("constant") || InterpolationToken == TEXT("step")) { KeyShape = EKeyShape::Constant; }

    int32 ChannelOffset = 0;
    {
        double ChannelOffsetValue = 0.0;
        if (Params->TryGetNumberField(TEXT("channel_offset"), ChannelOffsetValue)
            || Params->TryGetNumberField(TEXT("channel_index"), ChannelOffsetValue))
        {
            ChannelOffset = static_cast<int32>(ChannelOffsetValue);
        }
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }

    UWidgetAnimation* Animation = WidgetEdit_FindAnimation(WBP, AnimationName);
    if (!Animation)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find animation '%s' on %s. Run widget_edit add_animation first."), *AnimationName, *WidgetBlueprintPath));
    }
    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Animation '%s' has no UMovieScene."), *AnimationName));
    }

    FGuid OwningBindingGuid;
    UMovieSceneTrack* Track = WidgetEdit_ResolveTrackByIndex(MovieScene, TrackIndex, OwningBindingGuid);
    if (!Track)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("track_index %d is out of range (master + binding tracks combined)"), TrackIndex));
    }

    // Convert seconds to a FFrameNumber on the MovieScene's tick
    // resolution. Caller-supplied `frame` is treated as an explicit
    // FFrameNumber on the tick scale so a downstream Sequencer panel
    // sees the key at the same tick the inspect path reports.
    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    FFrameNumber KeyFrame;
    if (bHasFrame)
    {
        KeyFrame = FFrameNumber(static_cast<int32>(FrameValue));
    }
    else
    {
        KeyFrame = (TimeValue * TickResolution).RoundToFrame();
    }

    // Find or spawn a section. The tracks UMG drops on a widget
    // animation declare their native section type through
    // `CreateNewSection`; we use the first existing section if there
    // is one, otherwise spawn + add a fresh one and stretch its range
    // to cover the key time on creation.
    UMovieSceneSection* Section = nullptr;
    bool bSectionCreated = false;
    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    if (Sections.Num() > 0)
    {
        Section = Sections[0];
    }
    else
    {
        Section = Track->CreateNewSection();
        if (!Section)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("UMovieSceneTrack::CreateNewSection returned null for track '%s'"), *Track->GetName()));
        }
        Track->AddSection(*Section);
        Section->SetRange(TRange<FFrameNumber>::Inclusive(KeyFrame, KeyFrame));
        bSectionCreated = true;
    }
    Section->ExpandToFrame(KeyFrame);

    double Channels[4] = { 0.0, 0.0, 0.0, 0.0 };
    const int32 ChannelCount = WidgetEdit_ReadKeyValueChannels(ValueJson, Channels);
    if (ChannelCount <= 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not parse 'value' (expected number, [x, y, z, w?] array, or {x,y,z,w} object)"));
    }

    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    int32 KeysWritten = 0;
    int32 KeyFailures = 0;
    TArray<FString> ChannelKindLog;

    for (int32 I = 0; I < ChannelCount; ++I)
    {
        const int32 ChannelIndex = ChannelOffset + I;
        // Try float channel first (the common UMG case), then double
        // channel (transform / vector tracks switched to FDoubleChannel
        // in 5.4). We surface the channel kind in the response so the
        // caller can confirm the section's native channel type.
        if (FMovieSceneFloatChannel* FloatChannel = Proxy.GetChannel<FMovieSceneFloatChannel>(ChannelIndex))
        {
            const float V = static_cast<float>(Channels[I]);
            if (KeyShape == EKeyShape::Cubic)
            {
                FloatChannel->AddCubicKey(KeyFrame, V);
            }
            else if (KeyShape == EKeyShape::Linear)
            {
                FloatChannel->AddLinearKey(KeyFrame, V);
            }
            else
            {
                FloatChannel->AddConstantKey(KeyFrame, V);
            }
            ++KeysWritten;
            ChannelKindLog.Add(TEXT("float"));
        }
        else if (FMovieSceneDoubleChannel* DoubleChannel = Proxy.GetChannel<FMovieSceneDoubleChannel>(ChannelIndex))
        {
            const double V = Channels[I];
            if (KeyShape == EKeyShape::Cubic)
            {
                DoubleChannel->AddCubicKey(KeyFrame, V);
            }
            else if (KeyShape == EKeyShape::Linear)
            {
                DoubleChannel->AddLinearKey(KeyFrame, V);
            }
            else
            {
                DoubleChannel->AddConstantKey(KeyFrame, V);
            }
            ++KeysWritten;
            ChannelKindLog.Add(TEXT("double"));
        }
        else
        {
            ++KeyFailures;
            ChannelKindLog.Add(TEXT("missing"));
        }
    }

    if (KeysWritten == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Track '%s' section has no FMovieSceneFloatChannel or FMovieSceneDoubleChannel at index %d"),
                *Track->GetName(), ChannelOffset));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_keyframe"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
    ResultObj->SetNumberField(TEXT("track_index"), TrackIndex);
    ResultObj->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    if (OwningBindingGuid.IsValid())
    {
        ResultObj->SetStringField(TEXT("owning_binding_guid"), OwningBindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
    }
    ResultObj->SetNumberField(TEXT("frame"), KeyFrame.Value);
    if (TickResolution.Numerator > 0)
    {
        ResultObj->SetNumberField(TEXT("time"), TickResolution.AsSeconds(KeyFrame));
    }
    ResultObj->SetStringField(TEXT("interpolation"),
        KeyShape == EKeyShape::Cubic ? TEXT("cubic")
        : (KeyShape == EKeyShape::Linear ? TEXT("linear") : TEXT("constant")));
    ResultObj->SetNumberField(TEXT("channel_offset"), ChannelOffset);
    ResultObj->SetNumberField(TEXT("keys_written"), KeysWritten);
    ResultObj->SetNumberField(TEXT("key_failures"), KeyFailures);
    {
        TArray<TSharedPtr<FJsonValue>> KindArr;
        for (const FString& Kind : ChannelKindLog)
        {
            KindArr.Add(MakeShared<FJsonValueString>(Kind));
        }
        ResultObj->SetArrayField(TEXT("channel_kinds"), KindArr);
    }
    ResultObj->SetBoolField(TEXT("section_created"), bSectionCreated);
    ResultObj->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Resolve a UClass for the viewmodel argument. Accepts a full
     *  `/Script/Module.ClassName` path, a `/Game/...` Blueprint class
     *  path (auto-suffixed with `_C` if missing), or a short class name
     *  that we probe against the loaded class set with a `U` prefix
     *  fallback. The returned class must implement
     *  `INotifyFieldValueChanged`; UMVVMViewModelBase is the canonical
     *  parent that does so. */
    UClass* WidgetEdit_ResolveViewModelClass(const FString& Token, FString& OutReason)
    {
        OutReason.Reset();
        if (Token.IsEmpty())
        {
            OutReason = TEXT("empty token");
            return nullptr;
        }

        UClass* Resolved = nullptr;
        if (Token.StartsWith(TEXT("/Script/")))
        {
            Resolved = LoadClass<UObject>(nullptr, *Token);
        }
        else if (Token.StartsWith(TEXT("/Game/")))
        {
            // Blueprint generated classes live at `/Game/Path/AssetName.AssetName_C`.
            FString WithSuffix = Token;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                int32 DotIdx = INDEX_NONE;
                if (!WithSuffix.FindChar('.', DotIdx))
                {
                    int32 Slash = INDEX_NONE;
                    if (WithSuffix.FindLastChar('/', Slash))
                    {
                        const FString AssetName = WithSuffix.Mid(Slash + 1);
                        WithSuffix = WithSuffix + TEXT(".") + AssetName + TEXT("_C");
                    }
                }
                else
                {
                    WithSuffix = WithSuffix + TEXT("_C");
                }
            }
            Resolved = LoadClass<UObject>(nullptr, *WithSuffix);
            if (!Resolved)
            {
                Resolved = LoadClass<UObject>(nullptr, *Token);
            }
        }
        else
        {
            // Short name. Probe loaded classes with a couple of prefix
            // variants. UClass names omit the `U` prefix in their FName.
            Resolved = FindObject<UClass>(nullptr, *Token);
            if (!Resolved && !Token.StartsWith(TEXT("U")))
            {
                Resolved = FindObject<UClass>(nullptr, *(TEXT("U") + Token));
            }
            if (!Resolved)
            {
                const FString Stripped = Token.StartsWith(TEXT("U")) ? Token.Mid(1) : Token;
                Resolved = FindObject<UClass>(nullptr, *Stripped);
            }
        }

        if (!Resolved)
        {
            OutReason = FString::Printf(TEXT("Could not resolve viewmodel class '%s'"), *Token);
            return nullptr;
        }

        if (!Resolved->ImplementsInterface(UNotifyFieldValueChanged::StaticClass()))
        {
            OutReason = FString::Printf(TEXT("Resolved class '%s' does not implement INotifyFieldValueChanged. The MVVM extension requires the viewmodel class to implement the FieldNotification interface (UMVVMViewModelBase is the canonical parent)."),
                *Resolved->GetPathName());
            return nullptr;
        }

        if (Resolved->IsChildOf(UWidget::StaticClass()))
        {
            OutReason = FString::Printf(TEXT("Resolved class '%s' derives from UWidget; the MVVM `AllowedClasses` schema disallows widget viewmodels."),
                *Resolved->GetPathName());
            return nullptr;
        }

        return Resolved;
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetViewModel(const TSharedPtr<FJsonObject>& Params)
{
    // The minimum-cut MVVM op. Routes through
    //   UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>(WBP)
    // to get-or-create the editor-only MVVM extension on the WBP, then
    //   UMVVMBlueprintView::AddViewModel(FMVVMBlueprintViewModelContext(Class, Name))
    // appends the typed viewmodel slot. An optional binding_name also
    // runs UMVVMBlueprintView::AddDefaultBinding so the asset surfaces a
    // seeded binding row ready for downstream property-path edits. The
    // full MVVM surface (conversion functions, two-way bindings,
    // bindings to widget properties beyond root) stays on the BACKLOG.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString ViewModelToken;
    if (!Params->TryGetStringField(TEXT("viewmodel_class"), ViewModelToken)
        && !Params->TryGetStringField(TEXT("viewmodel"), ViewModelToken)
        && !Params->TryGetStringField(TEXT("class"), ViewModelToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'viewmodel_class' parameter (path or short name of a UMVVMViewModelBase subclass)"));
    }

    FString ViewModelName;
    Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelName);
    if (ViewModelName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("name"), ViewModelName);
    }

    FString BindingName;
    Params->TryGetStringField(TEXT("binding_name"), BindingName);
    if (BindingName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("binding"), BindingName);
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    FString ResolveError;
    UClass* ViewModelClass = WidgetEdit_ResolveViewModelClass(ViewModelToken, ResolveError);
    if (!ViewModelClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    // Default the viewmodel slot name to the class's display name minus
    // the `U` prefix when the caller did not pass one.
    if (ViewModelName.IsEmpty())
    {
        FString Default = ViewModelClass->GetName();
        if (Default.StartsWith(TEXT("U")))
        {
            Default = Default.Mid(1);
        }
        // Strip a trailing `_C` for Blueprint generated classes.
        Default.RemoveFromEnd(TEXT("_C"));
        ViewModelName = Default;
    }

    // Get-or-create the MVVM extension on the WBP. The templated
    // RequestExtension overload calls the base class's untyped version
    // and CastChecked's the result to UMVVMWidgetBlueprintExtension_View.
    UMVVMWidgetBlueprintExtension_View* MVVMExt =
        UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>(WBP);
    if (!MVVMExt)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not get-or-create UMVVMWidgetBlueprintExtension_View on '%s'"), *WBPPath));
    }

    UMVVMBlueprintView* BlueprintView = MVVMExt->GetBlueprintView();
    bool bViewCreated = false;
    if (!BlueprintView)
    {
        MVVMExt->CreateBlueprintViewInstance();
        BlueprintView = MVVMExt->GetBlueprintView();
        bViewCreated = true;
    }
    if (!BlueprintView)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("UMVVMWidgetBlueprintExtension_View::CreateBlueprintViewInstance left BlueprintView null"));
    }

    // Guard against a duplicate viewmodel slot with the same FName.
    // FindViewModel returns non-null when there's already an entry with
    // that name; we surface it as an error rather than silently shadow.
    const FName ViewModelFName(*ViewModelName);
    if (BlueprintView->FindViewModel(ViewModelFName) != nullptr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Viewmodel '%s' already exists on '%s'. Use a different 'viewmodel_name'."),
                *ViewModelName, *WBPPath));
    }

    // Construct and add the new viewmodel context. The single-arg
    // (Class, Name) constructor sets a fresh ViewModelContextId GUID,
    // bCreateGetterFunction=true, bCreateSetterFunction=false, and the
    // default CreationType=CreateInstance which spawns a fresh
    // viewmodel instance on widget construction. Callers that want a
    // manual / global / property-path setup can run the eventual
    // dedicated edit op once shipped.
    FMVVMBlueprintViewModelContext NewContext(ViewModelClass, ViewModelFName);
    BlueprintView->AddViewModel(NewContext);

    // Optional default binding row. AddDefaultBinding constructs an
    // empty FMVVMBlueprintViewBinding with a fresh BindingId and
    // appends to the BlueprintView's Bindings array. The row's
    // SourcePath / DestinationPath stay default-empty; downstream
    // edit ops on the MVVM surface will set both.
    FGuid BindingId;
    bool bDefaultBindingAdded = false;
    if (!BindingName.IsEmpty())
    {
        FMVVMBlueprintViewBinding& NewBinding = BlueprintView->AddDefaultBinding();
        BindingId = NewBinding.BindingId;
        bDefaultBindingAdded = true;
    }

    // Mark the WBP as structurally modified so the next compile picks
    // up the new viewmodel slot. Save the asset by default.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_viewmodel"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("viewmodel_class"), ViewModelClass->GetPathName());
    ResultObj->SetStringField(TEXT("viewmodel_name"), ViewModelName);
    ResultObj->SetBoolField(TEXT("view_created"), bViewCreated);
    ResultObj->SetNumberField(TEXT("viewmodel_count"), BlueprintView->GetViewModels().Num());
    if (bDefaultBindingAdded)
    {
        ResultObj->SetBoolField(TEXT("binding_added"), true);
        ResultObj->SetStringField(TEXT("binding_name"), BindingName);
        ResultObj->SetStringField(TEXT("binding_id"), BindingId.ToString(EGuidFormats::DigitsWithHyphens));
    }
    else
    {
        ResultObj->SetBoolField(TEXT("binding_added"), false);
    }
    ResultObj->SetNumberField(TEXT("binding_count"), BlueprintView->GetNumBindings());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
    // Full MVVM binding-row authoring. Adds a fresh
    // FMVVMBlueprintViewBinding through UMVVMBlueprintView::AddDefaultBinding
    // and configures its SourcePath / DestinationPath / BindingType through
    // the public FMVVMBlueprintPropertyPath setters. The runtime MVVM
    // compiler picks the row up on the next WBP compile via the existing
    // UMVVMWidgetBlueprintExtension_View path the set_viewmodel op already
    // wires.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    UMVVMWidgetBlueprintExtension_View* MVVMExt =
        UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>(WBP);
    if (!MVVMExt)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not get-or-create UMVVMWidgetBlueprintExtension_View on '%s'"), *WBPPath));
    }
    UMVVMBlueprintView* BlueprintView = MVVMExt->GetBlueprintView();
    if (!BlueprintView)
    {
        // Auto-create the view so the caller can land both a viewmodel and
        // a binding through one set_viewmodel + add_property_binding pair
        // without manual coordination.
        MVVMExt->CreateBlueprintViewInstance();
        BlueprintView = MVVMExt->GetBlueprintView();
    }
    if (!BlueprintView)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMVVMWidgetBlueprintExtension_View on '%s' has no UMVVMBlueprintView; run set_viewmodel first"), *WBPPath));
    }

    // Resolve the source viewmodel. Caller may pass a viewmodel FName
    // (set_viewmodel-side label) or the FGuid context id string.
    FString ViewModelToken;
    if (!Params->TryGetStringField(TEXT("viewmodel"), ViewModelToken)
        && !Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelToken)
        && !Params->TryGetStringField(TEXT("source_viewmodel"), ViewModelToken)
        && !Params->TryGetStringField(TEXT("viewmodel_id"), ViewModelToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'viewmodel' parameter (viewmodel slot name or context id GUID)"));
    }
    const FMVVMBlueprintViewModelContext* ContextPtr = nullptr;
    FGuid ParsedGuid;
    if (FGuid::Parse(ViewModelToken, ParsedGuid))
    {
        ContextPtr = BlueprintView->FindViewModel(ParsedGuid);
    }
    if (!ContextPtr)
    {
        ContextPtr = BlueprintView->FindViewModel(FName(*ViewModelToken));
    }
    if (!ContextPtr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve viewmodel '%s' on '%s'. Run set_viewmodel first."),
                *ViewModelToken, *WBPPath));
    }
    UClass* ViewModelClass = ContextPtr->GetViewModelClass();
    if (!ViewModelClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Viewmodel '%s' on '%s' has no resolved class"),
                *ViewModelToken, *WBPPath));
    }

    // Resolve the source field on the viewmodel's class. Accepts a
    // UFunction name (BlueprintCallable getters / BlueprintPure
    // accessors) plus an FProperty name; the field-variant path stores
    // the right kind.
    FString SourceFieldToken;
    if (!Params->TryGetStringField(TEXT("source_field"), SourceFieldToken)
        && !Params->TryGetStringField(TEXT("source_property"), SourceFieldToken)
        && !Params->TryGetStringField(TEXT("source"), SourceFieldToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_field' parameter (FProperty / UFunction name on the source viewmodel's class)"));
    }
    UE::MVVM::FMVVMConstFieldVariant SourceField;
    if (const FProperty* SrcProp = FindFProperty<FProperty>(ViewModelClass, *SourceFieldToken))
    {
        SourceField = UE::MVVM::FMVVMConstFieldVariant(SrcProp);
    }
    else if (const UFunction* SrcFunc = ViewModelClass->FindFunctionByName(FName(*SourceFieldToken)))
    {
        SourceField = UE::MVVM::FMVVMConstFieldVariant(SrcFunc);
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No FProperty / UFunction named '%s' on viewmodel class '%s'"),
                *SourceFieldToken, *ViewModelClass->GetPathName()));
    }

    // Resolve the destination widget. The MVVM compiler resolves the
    // widget at runtime via the widget's FName on the WBP's
    // WidgetTree; we look it up at author time so we can fail closed
    // when the widget does not exist.
    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("destination_widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target_widget"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget' parameter (target widget FName)"));
    }
    UWidget* TargetWidget = nullptr;
    if (UWidgetTree* Tree = WBP->WidgetTree)
    {
        Tree->ForEachWidget([&](UWidget* W)
        {
            if (TargetWidget) return;
            if (W && W->GetFName() == FName(*WidgetNameStr))
            {
                TargetWidget = W;
            }
        });
    }
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    // Resolve the destination field on the widget's class. Same
    // function / property fallback as the source side.
    FString DestFieldToken;
    if (!Params->TryGetStringField(TEXT("destination_field"), DestFieldToken)
        && !Params->TryGetStringField(TEXT("destination_property"), DestFieldToken)
        && !Params->TryGetStringField(TEXT("destination"), DestFieldToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'destination_field' parameter (FProperty / UFunction name on the destination widget's class)"));
    }
    UClass* WidgetClass = TargetWidget->GetClass();
    UE::MVVM::FMVVMConstFieldVariant DestField;
    if (const FProperty* DestProp = FindFProperty<FProperty>(WidgetClass, *DestFieldToken))
    {
        DestField = UE::MVVM::FMVVMConstFieldVariant(DestProp);
    }
    else if (const UFunction* DestFunc = WidgetClass->FindFunctionByName(FName(*DestFieldToken)))
    {
        DestField = UE::MVVM::FMVVMConstFieldVariant(DestFunc);
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No FProperty / UFunction named '%s' on widget class '%s'"),
                *DestFieldToken, *WidgetClass->GetPathName()));
    }

    // Resolve the binding mode. The MVVM editor surfaces three
    // designer-facing modes; the engine enum has more (e.g.
    // OneWayToSource / OneTimeToSource) but those stay marked Hidden
    // and are not part of the documented contract.
    FString ModeToken;
    Params->TryGetStringField(TEXT("binding_mode"), ModeToken);
    if (ModeToken.IsEmpty())
    {
        Params->TryGetStringField(TEXT("mode"), ModeToken);
    }
    const FString ModeLower = ModeToken.ToLower();
    EMVVMBindingMode Mode = EMVVMBindingMode::OneWayToDestination;
    FString ModeCanonical = TEXT("OneWayToDestination");
    if (ModeLower.IsEmpty() || ModeLower == TEXT("one_way") || ModeLower == TEXT("oneway")
        || ModeLower == TEXT("one_way_to_destination") || ModeLower == TEXT("onewaytodestination"))
    {
        Mode = EMVVMBindingMode::OneWayToDestination;
        ModeCanonical = TEXT("OneWayToDestination");
    }
    else if (ModeLower == TEXT("two_way") || ModeLower == TEXT("twoway"))
    {
        Mode = EMVVMBindingMode::TwoWay;
        ModeCanonical = TEXT("TwoWay");
    }
    else if (ModeLower == TEXT("one_time") || ModeLower == TEXT("onetime")
        || ModeLower == TEXT("one_time_to_destination") || ModeLower == TEXT("onetimetodestination"))
    {
        Mode = EMVVMBindingMode::OneTimeToDestination;
        ModeCanonical = TEXT("OneTimeToDestination");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported 'binding_mode' '%s' (one_way / two_way / one_time)"), *ModeToken));
    }

    bool bEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);
    bool bCompileBinding = true;
    Params->TryGetBoolField(TEXT("compile_binding"), bCompileBinding);
    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    // Spawn the binding row. AddDefaultBinding emits a fresh
    // FMVVMBlueprintViewBinding with a fresh BindingId and appends it
    // to the BlueprintView's Bindings array. The returned mutable
    // reference is the one we configure in place.
    FMVVMBlueprintViewBinding& NewBinding = BlueprintView->AddDefaultBinding();
    NewBinding.SourcePath.SetViewModelId(ContextPtr->GetViewModelId());
    NewBinding.SourcePath.SetPropertyPath(WBP, SourceField);

    NewBinding.DestinationPath.SetWidgetName(TargetWidget->GetFName());
    NewBinding.DestinationPath.SetPropertyPath(WBP, DestField);

    NewBinding.BindingType = Mode;
    NewBinding.bEnabled = bEnabled;
    NewBinding.bCompile = bCompileBinding;

    const FGuid BindingId = NewBinding.BindingId;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_property_binding"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("viewmodel_name"), ContextPtr->GetViewModelName().ToString());
    ResultObj->SetStringField(TEXT("viewmodel_id"), ContextPtr->GetViewModelId().ToString(EGuidFormats::DigitsWithHyphens));
    ResultObj->SetStringField(TEXT("viewmodel_class"), ViewModelClass->GetPathName());
    ResultObj->SetStringField(TEXT("source_field"), SourceFieldToken);
    ResultObj->SetStringField(TEXT("source_kind"), SourceField.IsFunction() ? TEXT("function") : TEXT("property"));
    ResultObj->SetStringField(TEXT("widget"), TargetWidget->GetFName().ToString());
    ResultObj->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
    ResultObj->SetStringField(TEXT("destination_field"), DestFieldToken);
    ResultObj->SetStringField(TEXT("destination_kind"), DestField.IsFunction() ? TEXT("function") : TEXT("property"));
    ResultObj->SetStringField(TEXT("binding_mode"), ModeCanonical);
    ResultObj->SetStringField(TEXT("binding_id"), BindingId.ToString(EGuidFormats::DigitsWithHyphens));
    ResultObj->SetNumberField(TEXT("binding_count"), BlueprintView->GetNumBindings());
    ResultObj->SetBoolField(TEXT("enabled"), bEnabled);
    ResultObj->SetBoolField(TEXT("compile"), bCompileBinding);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetBindingConversion(const TSharedPtr<FJsonObject>& Params)
{
    // Rewrite the per-direction conversion slot on an existing
    // FMVVMBlueprintViewBinding. The canonical authoring entry the
    // MVVMEditorSubsystem uses (SetSourceToDestinationConversionFunction)
    // routes a UFunction through:
    //   1) NewObject<UMVVMBlueprintViewConversionFunction>(WBP)
    //   2) name the wrapper via CreateWrapperName(Binding, bSourceToDestination)
    //   3) Initialize(WBP, GraphName, FMVVMBlueprintFunctionReference(WBP, Function))
    // We follow the same path, plus an explicit clear branch the
    // editor surface exposes through the "<None>" picker entry.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    UMVVMWidgetBlueprintExtension_View* MVVMExt =
        UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>(WBP);
    UMVVMBlueprintView* BlueprintView = MVVMExt ? MVVMExt->GetBlueprintView() : nullptr;
    if (!BlueprintView)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'%s' has no UMVVMBlueprintView; run widget_edit set_viewmodel + add_property_binding first"),
                *WBPPath));
    }

    // Resolve the target binding. Accepts an FGuid binding-id string
    // (preferred; AddPropertyBinding returns this) or an integer
    // index into the Bindings array (a designer-friendly fallback for
    // hand-driven calls).
    FMVVMBlueprintViewBinding* Binding = nullptr;
    int32 ResolvedIndex = INDEX_NONE;
    FString BindingToken;
    if (Params->TryGetStringField(TEXT("binding_id"), BindingToken)
        || Params->TryGetStringField(TEXT("binding"), BindingToken)
        || Params->TryGetStringField(TEXT("id"), BindingToken))
    {
        FGuid ParsedGuid;
        if (FGuid::Parse(BindingToken, ParsedGuid))
        {
            Binding = BlueprintView->GetBinding(ParsedGuid);
        }
        if (!Binding)
        {
            // Fall back to "integer-as-string" so callers can ship
            // either form through the same field.
            int32 Parsed = 0;
            if (LexTryParseString(Parsed, *BindingToken)
                && Parsed >= 0 && Parsed < BlueprintView->GetNumBindings())
            {
                Binding = BlueprintView->GetBindingAt(Parsed);
                ResolvedIndex = Parsed;
            }
        }
    }
    if (!Binding)
    {
        int32 BindingIdx = INDEX_NONE;
        double TempIdx = 0.0;
        if (Params->TryGetNumberField(TEXT("binding_index"), TempIdx)
            || Params->TryGetNumberField(TEXT("index"), TempIdx))
        {
            BindingIdx = static_cast<int32>(TempIdx);
            if (BindingIdx >= 0 && BindingIdx < BlueprintView->GetNumBindings())
            {
                Binding = BlueprintView->GetBindingAt(BindingIdx);
                ResolvedIndex = BindingIdx;
            }
        }
    }
    if (!Binding)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve binding on '%s' (pass 'binding_id' GUID or 'binding_index' int)"),
                *WBPPath));
    }
    if (ResolvedIndex == INDEX_NONE)
    {
        const TArrayView<const FMVVMBlueprintViewBinding> AllBindings = BlueprintView->GetBindings();
        for (int32 Idx = 0; Idx < AllBindings.Num(); ++Idx)
        {
            if (AllBindings[Idx].BindingId == Binding->BindingId)
            {
                ResolvedIndex = Idx;
                break;
            }
        }
    }

    // Resolve the direction. The MVVM editor exposes both directions
    // through the "Forward" / "Backward" picker on the binding row.
    FString DirectionToken;
    Params->TryGetStringField(TEXT("direction"), DirectionToken);
    const FString DirectionLower = DirectionToken.ToLower();
    bool bSourceToDestination = true;
    FString DirectionCanonical = TEXT("SourceToDestination");
    if (DirectionLower.IsEmpty() || DirectionLower == TEXT("source_to_destination")
        || DirectionLower == TEXT("sourcetodestination") || DirectionLower == TEXT("forward")
        || DirectionLower == TEXT("s2d") || DirectionLower == TEXT("src_to_dst"))
    {
        bSourceToDestination = true;
        DirectionCanonical = TEXT("SourceToDestination");
    }
    else if (DirectionLower == TEXT("destination_to_source") || DirectionLower == TEXT("destinationtosource")
        || DirectionLower == TEXT("backward") || DirectionLower == TEXT("d2s")
        || DirectionLower == TEXT("dst_to_src"))
    {
        bSourceToDestination = false;
        DirectionCanonical = TEXT("DestinationToSource");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported 'direction' '%s' (source_to_destination / destination_to_source)"),
                *DirectionToken));
    }

    // Decide between clear and rebind. Treat empty / `none` /
    // explicit `clear=true` as the clear branch; everything else is
    // a UFunction path that must resolve to a UFunction at author
    // time.
    bool bClear = false;
    Params->TryGetBoolField(TEXT("clear"), bClear);
    FString FunctionToken;
    if (!bClear)
    {
        Params->TryGetStringField(TEXT("conversion_function"), FunctionToken);
        if (FunctionToken.IsEmpty()) Params->TryGetStringField(TEXT("function"), FunctionToken);
        if (FunctionToken.IsEmpty()) Params->TryGetStringField(TEXT("conversion"), FunctionToken);
        if (FunctionToken.IsEmpty()) Params->TryGetStringField(TEXT("function_path"), FunctionToken);
        const FString FunctionLower = FunctionToken.ToLower();
        if (FunctionToken.IsEmpty() || FunctionLower == TEXT("none")
            || FunctionLower == TEXT("null") || FunctionLower == TEXT("clear"))
        {
            bClear = true;
        }
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    // Snapshot the previous slot so the response can report what was
    // replaced and the editor's MVVMEditorSubsystem flow runs.
    TObjectPtr<UMVVMBlueprintViewConversionFunction>& Slot = bSourceToDestination
        ? Binding->Conversion.SourceToDestinationConversion
        : Binding->Conversion.DestinationToSourceConversion;
    FString PreviousFunctionPath;
    if (Slot)
    {
        FMVVMBlueprintFunctionReference PrevRef = Slot->GetConversionFunction();
        const UFunction* PrevFunc = PrevRef.GetFunction(WBP);
        if (PrevFunc)
        {
            PreviousFunctionPath = PrevFunc->GetPathName();
        }
    }

    if (bClear)
    {
        // Mirror MVVMEditorSubsystem's clear branch: remove the
        // wrapper graph before dropping the reference so the
        // generated graph garbage collects.
        if (Slot)
        {
            Slot->RemoveWrapperGraph(WBP);
            Slot = nullptr;
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        if (bSaveAfterEdit)
        {
            UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("operation"), TEXT("set_binding_conversion"));
        ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
        ResultObj->SetStringField(TEXT("binding_id"), Binding->BindingId.ToString(EGuidFormats::DigitsWithHyphens));
        ResultObj->SetNumberField(TEXT("binding_index"), ResolvedIndex);
        ResultObj->SetStringField(TEXT("direction"), DirectionCanonical);
        ResultObj->SetBoolField(TEXT("cleared"), true);
        if (!PreviousFunctionPath.IsEmpty())
        {
            ResultObj->SetStringField(TEXT("previous_function"), PreviousFunctionPath);
        }
        ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
        return ResultObj;
    }

    // Rebind branch. Resolve the conversion UFunction by
    // `/Script/Module.Class:Function`, `/Script/Module.Class.Function`,
    // a `/Game/...` BP class path with `:Function`, or a bare token.
    // The MVVMEditorSubsystem keeps this path UFunction-only; the
    // K2Node-class branch (async conversion nodes) is intentionally
    // out of scope for this slice.
    const UFunction* ConversionFunction = nullptr;
    FString ResolvedFunctionPath;
    {
        FString ClassPart;
        FString FuncPart;
        bool bSplit = FunctionToken.Split(TEXT(":"), &ClassPart, &FuncPart);
        if (!bSplit)
        {
            // Tolerate the dot-separated form too; LoadObject<UFunction>
            // accepts /Script/Module.Class.Function directly.
            int32 DotIdx = INDEX_NONE;
            if (FunctionToken.FindLastChar('.', DotIdx) && DotIdx > 0)
            {
                ClassPart = FunctionToken.Left(DotIdx);
                FuncPart = FunctionToken.Mid(DotIdx + 1);
                bSplit = !ClassPart.IsEmpty() && !FuncPart.IsEmpty();
            }
        }
        if (bSplit)
        {
            UClass* OwnerClass = nullptr;
            if (ClassPart.StartsWith(TEXT("/Script/")))
            {
                OwnerClass = LoadClass<UObject>(nullptr, *ClassPart);
            }
            else if (ClassPart.StartsWith(TEXT("/Game/")))
            {
                FString WithSuffix = ClassPart;
                if (!WithSuffix.EndsWith(TEXT("_C")))
                {
                    WithSuffix += TEXT("_C");
                }
                OwnerClass = LoadClass<UObject>(nullptr, *WithSuffix);
            }
            if (!OwnerClass)
            {
                OwnerClass = FindObject<UClass>(nullptr, *ClassPart);
            }
            if (OwnerClass)
            {
                if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*FuncPart)))
                {
                    ConversionFunction = Found;
                    ResolvedFunctionPath = Found->GetPathName();
                }
            }
        }
        if (!ConversionFunction)
        {
            // Fallback: maybe the caller passed the full path as a
            // UFunction object path directly.
            if (UFunction* Loaded = FindObject<UFunction>(nullptr, *FunctionToken))
            {
                ConversionFunction = Loaded;
                ResolvedFunctionPath = Loaded->GetPathName();
            }
        }
    }
    if (!ConversionFunction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve conversion UFunction '%s'. Expected '/Script/Module.Class:Function' or '/Game/.../BP_C:Function'."),
                *FunctionToken));
    }

    // Mirror MVVMEditorSubsystem::SetSourceToDestinationConversionFunction's
    // hot path (the only public authoring entry that touches the
    // Conversion slot): drop the existing wrapper graph, NewObject a
    // fresh ConversionFunction outered to the WBP, run
    // SetDestinationPath + Initialize.
    if (Slot)
    {
        Slot->RemoveWrapperGraph(WBP);
        Slot = nullptr;
    }
    UMVVMBlueprintViewConversionFunction* NewConv = NewObject<UMVVMBlueprintViewConversionFunction>(WBP);
    if (!NewConv)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("NewObject<UMVVMBlueprintViewConversionFunction> returned null"));
    }
    Slot = NewConv;
    // Pass the binding's destination path so async conversion
    // nodes (which handle the destination write internally) see the
    // right target. Synchronous UFunctions ignore the field.
    NewConv->SetDestinationPath(Binding->DestinationPath);
    const FName GraphName = UE::MVVM::ConversionFunctionHelper::CreateWrapperName(*Binding, bSourceToDestination);
    FMVVMBlueprintFunctionReference FuncRef(WBP, ConversionFunction);
    NewConv->Initialize(WBP, GraphName, FuncRef);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_binding_conversion"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("binding_id"), Binding->BindingId.ToString(EGuidFormats::DigitsWithHyphens));
    ResultObj->SetNumberField(TEXT("binding_index"), ResolvedIndex);
    ResultObj->SetStringField(TEXT("direction"), DirectionCanonical);
    ResultObj->SetBoolField(TEXT("cleared"), false);
    ResultObj->SetStringField(TEXT("conversion_function"), ResolvedFunctionPath);
    ResultObj->SetStringField(TEXT("wrapper_graph_name"), GraphName.ToString());
    if (!PreviousFunctionPath.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("previous_function"), PreviousFunctionPath);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddEventBinding(const TSharedPtr<FJsonObject>& Params)
{
    // Spawn (or focus) a UK2Node_ComponentBoundEvent in the WBP's
    // event graph for a child widget's multicast delegate. Mirrors
    // the editor's "+ event" picker that
    // FBlueprintWidgetCustomization::HandleAddOrViewEventForVariable
    // hooks up: find the FObjectProperty for the child widget on
    // the SkeletonGeneratedClass, find the FMulticastDelegateProperty
    // by name on that widget's UClass, then route the pair through
    // FKismetEditorUtilities::CreateNewBoundEventForClass. The
    // existing-node guard runs FindBoundEventForComponent so a
    // second call with the same (widget, event) pair stays
    // idempotent.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    // Resolve the target child widget. Caller passes its FName as
    // it appears in the WidgetTree (which is also the variable
    // name on the WBP's generated class).
    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target_widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("component"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget' parameter (target child widget FName)"));
    }

    // Walk the WidgetTree so we can fail closed when the child
    // does not exist, and so we have its UClass for the delegate
    // lookup. The variable property on the BP class is the runtime
    // surface CreateNewBoundEventForClass needs.
    UWidget* TargetWidget = nullptr;
    if (UWidgetTree* Tree = WBP->WidgetTree)
    {
        Tree->ForEachWidget([&](UWidget* W)
        {
            if (TargetWidget) return;
            if (W && W->GetFName() == FName(*WidgetNameStr))
            {
                TargetWidget = W;
            }
        });
    }
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    UClass* WidgetClass = TargetWidget->GetClass();
    const FName WidgetFName = TargetWidget->GetFName();

    // Resolve the delegate property by name on the widget's
    // UClass. UMG buttons expose OnClicked / OnHovered etc. as
    // FMulticastDelegateProperty fields on UButton; text input
    // boxes expose OnTextCommitted; sliders expose OnValueChanged.
    FString DelegateNameStr;
    if (!Params->TryGetStringField(TEXT("event"), DelegateNameStr)
        && !Params->TryGetStringField(TEXT("event_name"), DelegateNameStr)
        && !Params->TryGetStringField(TEXT("delegate"), DelegateNameStr)
        && !Params->TryGetStringField(TEXT("delegate_name"), DelegateNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event' parameter (multicast delegate property name, e.g. OnClicked)"));
    }
    const FName DelegateFName(*DelegateNameStr);
    FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(WidgetClass, DelegateFName);
    if (!DelegateProperty)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("No FMulticastDelegateProperty named '%s' on widget class '%s' (looking for an OnClicked / OnHovered / OnTextCommitted / OnValueChanged style multicast event)"),
                *DelegateNameStr, *WidgetClass->GetPathName()));
    }

    // The variable property is the runtime FObjectProperty on the
    // WBP's SkeletonGeneratedClass: that is what
    // CreateNewBoundEventForClass binds the event node to. The
    // SkeletonGeneratedClass is what the UMG editor inspects on
    // the +event button path (see
    // FBlueprintWidgetCustomization::HandleAddOrViewEventForVariable),
    // since it stays in sync with the variable list even when the
    // BP has not yet recompiled after a new child widget add.
    UClass* SkeletonClass = WBP->SkeletonGeneratedClass;
    if (!SkeletonClass)
    {
        SkeletonClass = WBP->GeneratedClass;
    }
    FObjectProperty* VariableProperty = nullptr;
    if (SkeletonClass)
    {
        VariableProperty = FindFProperty<FObjectProperty>(SkeletonClass, WidgetFName);
    }
    if (!VariableProperty)
    {
        // The widget may not be marked as a Blueprint variable yet
        // (the UMG editor exposes the "Is Variable" checkbox per
        // child). Without that flag the BP's class has no
        // FObjectProperty by that name, which means the bound-event
        // surface cannot wire the runtime delegate. Flip
        // bIsVariable + recompile and try again so the op stays
        // declarative.
        const bool bIsVariable = TargetWidget->bIsVariable;
        if (!bIsVariable)
        {
            TargetWidget->bIsVariable = true;
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
            FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
            SkeletonClass = WBP->SkeletonGeneratedClass;
            if (SkeletonClass)
            {
                VariableProperty = FindFProperty<FObjectProperty>(SkeletonClass, WidgetFName);
            }
        }
        if (!VariableProperty)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Widget '%s' is not exposed as a Blueprint variable on '%s' (set 'expose_as_variable=true' on add_child_widget); event binding needs an FObjectProperty on the generated class"),
                    *WidgetNameStr, *WBPPath));
        }
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    // Optional handler-function rename. The K2Node spawned by
    // CreateNewBoundEventForClass picks its CustomFunctionName from
    // the delegate property name plus the variable name (the
    // ubergraph compiler turns that into a stable BP entry point).
    // Callers can override the handler name through this knob so
    // the resulting function shows up under their chosen label;
    // matches the editor's "rename event" flow.
    FString HandlerFunctionName;
    Params->TryGetStringField(TEXT("handler_function"), HandlerFunctionName);
    if (HandlerFunctionName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("handler"), HandlerFunctionName);
    }
    if (HandlerFunctionName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("function_name"), HandlerFunctionName);
    }

    // Idempotency. The editor uses FindBoundEventForComponent
    // before spawning a fresh node; we mirror that so a second
    // call returns the existing node's coordinates without
    // doubling up the graph.
    const UK2Node_ComponentBoundEvent* Existing =
        FKismetEditorUtilities::FindBoundEventForComponent(WBP, DelegateFName, VariableProperty->GetFName());
    UK2Node_ComponentBoundEvent* EventNode = nullptr;
    bool bReused = false;
    if (Existing)
    {
        EventNode = const_cast<UK2Node_ComponentBoundEvent*>(Existing);
        bReused = true;
    }
    else
    {
        FKismetEditorUtilities::CreateNewBoundEventForClass(WidgetClass, DelegateFName, WBP, VariableProperty);
        Existing = FKismetEditorUtilities::FindBoundEventForComponent(WBP, DelegateFName, VariableProperty->GetFName());
        EventNode = const_cast<UK2Node_ComponentBoundEvent*>(Existing);
    }
    if (!EventNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("CreateNewBoundEventForClass returned no node for delegate '%s' on widget '%s' (the WBP may have no event graph)"),
                *DelegateNameStr, *WidgetNameStr));
    }

    // Rename the spawned event's CustomFunctionName so the
    // resulting BP entry point lands on the caller's handler
    // label. We avoid renaming when the requested name collides
    // with another node so the existing graph stays intact.
    FString ResolvedHandlerName;
    if (!HandlerFunctionName.IsEmpty())
    {
        const FName NewHandlerFName(*HandlerFunctionName);
        bool bNameInUse = false;
        if (UEdGraph* OwningGraph = EventNode->GetGraph())
        {
            for (const UEdGraphNode* Other : OwningGraph->Nodes)
            {
                if (!Other || Other == EventNode) continue;
                if (const UK2Node_Event* AsEvent = Cast<UK2Node_Event>(Other))
                {
                    if (AsEvent->CustomFunctionName == NewHandlerFName)
                    {
                        bNameInUse = true;
                        break;
                    }
                }
            }
        }
        if (!bNameInUse)
        {
            EventNode->CustomFunctionName = NewHandlerFName;
            EventNode->ReconstructNode();
        }
        ResolvedHandlerName = EventNode->CustomFunctionName.ToString();
    }
    else
    {
        ResolvedHandlerName = EventNode->CustomFunctionName.ToString();
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    if (bCompile && !bReused)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_event_binding"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
    ResultObj->SetStringField(TEXT("event"), DelegateNameStr);
    ResultObj->SetStringField(TEXT("delegate_property"), DelegateProperty->GetFName().ToString());
    ResultObj->SetStringField(TEXT("variable_property"), VariableProperty->GetFName().ToString());
    ResultObj->SetStringField(TEXT("handler_function"), ResolvedHandlerName);
    ResultObj->SetStringField(TEXT("node_name"), EventNode->GetFName().ToString());
    if (UEdGraph* OwningGraph = EventNode->GetGraph())
    {
        ResultObj->SetStringField(TEXT("graph"), OwningGraph->GetFName().ToString());
    }
    ResultObj->SetNumberField(TEXT("node_position_x"), EventNode->NodePosX);
    ResultObj->SetNumberField(TEXT("node_position_y"), EventNode->NodePosY);
    ResultObj->SetBoolField(TEXT("reused_existing"), bReused);
    ResultObj->SetBoolField(TEXT("compiled"), bCompile && !bReused);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetWidgetStyle(const TSharedPtr<FJsonObject>& Params)
{
    // Write a flat property dict against a child widget's style
    // struct field. Defaults to the WidgetStyle UPROPERTY (UButton
    // / UProgressBar / UScrollBar / UScrollBox / USlider / UCheckBox
    // / UEditableText etc. all expose a single FXyzStyle field
    // named WidgetStyle), with an optional style_field knob for the
    // secondary slots (WidgetBarStyle on a UScrollBox, etc.).
    // Reflection-driven so the surface picks up any future UMG
    // widget that adds a new style struct without us spelling out
    // its fields.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target_widget"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = nullptr;
    if (UWidgetTree* Tree = WBP->WidgetTree)
    {
        Tree->ForEachWidget([&](UWidget* W)
        {
            if (TargetWidget) return;
            if (W && W->GetFName() == FName(*WidgetNameStr))
            {
                TargetWidget = W;
            }
        });
    }
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    // Pick the style field on the widget's class. The default
    // (WidgetStyle) covers the canonical UMG widgets; the
    // style_field knob lets a caller target WidgetBarStyle on a
    // UScrollBox, or any future field of FXyzStyle shape that the
    // engine grows.
    FString StyleFieldName;
    Params->TryGetStringField(TEXT("style_field"), StyleFieldName);
    if (StyleFieldName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("field"), StyleFieldName);
    }
    if (StyleFieldName.IsEmpty())
    {
        StyleFieldName = TEXT("WidgetStyle");
    }
    UClass* WidgetClass = TargetWidget->GetClass();
    FStructProperty* StyleProp = FindFProperty<FStructProperty>(WidgetClass, *StyleFieldName);
    if (!StyleProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget '%s' (%s) has no FStructProperty named '%s' (pass 'style_field' to target a non-default style slot)"),
                *WidgetNameStr, *WidgetClass->GetPathName(), *StyleFieldName));
    }
    void* StyleContainer = StyleProp->ContainerPtrToValuePtr<void>(TargetWidget);
    UScriptStruct* StyleStruct = StyleProp->Struct;
    if (!StyleContainer || !StyleStruct)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve style struct on '%s' / '%s'"),
                *WidgetNameStr, *StyleFieldName));
    }

    // Pull the property dict. Accepts both `style` (designer-side
    // shape) and `properties` (the dict shape every other Sproft
    // edit op accepts) so the caller can reuse one keyword across
    // tools without remembering which slot wants which name.
    const TSharedPtr<FJsonObject>* StyleObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("style"), StyleObj)
        && !Params->TryGetObjectField(TEXT("properties"), StyleObj)
        && !Params->TryGetObjectField(TEXT("style_properties"), StyleObj)
        && !Params->TryGetObjectField(TEXT("values"), StyleObj))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'style' parameter (flat property dict to apply against the style struct)"));
    }

    TArray<TSharedPtr<FJsonValue>> AppliedRows;
    TArray<TSharedPtr<FJsonValue>> SkippedRows;
    int32 AppliedCount = 0;
    int32 SkippedCount = 0;

    if (StyleObj && (*StyleObj).IsValid())
    {
        for (const auto& KV : (*StyleObj)->Values)
        {
            const FString& Key = KV.Key;
            const TSharedPtr<FJsonValue>& Val = KV.Value;
            FProperty* FieldProp = StyleStruct->FindPropertyByName(*Key);
            if (!FieldProp)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), Key);
                Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
                SkippedRows.Add(MakeShared<FJsonValueObject>(Skip));
                ++SkippedCount;
                continue;
            }
            const FString ImportTextValue = WidgetEdit_JsonValueToImportText(Val);
            void* FieldAddr = FieldProp->ContainerPtrToValuePtr<void>(StyleContainer);
            FOutputDeviceNull NullDevice;
            const TCHAR* ImportPtr = *ImportTextValue;
            const bool bImported = FieldProp->ImportText_Direct(ImportPtr, FieldAddr, TargetWidget,
                PPF_None, &NullDevice) != nullptr;
            if (!bImported)
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("name"), Key);
                Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
                Skip->SetStringField(TEXT("input"), ImportTextValue);
                Skip->SetStringField(TEXT("property_class"), FieldProp->GetClass()->GetName());
                SkippedRows.Add(MakeShared<FJsonValueObject>(Skip));
                ++SkippedCount;
                continue;
            }
            TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
            Applied->SetStringField(TEXT("name"), Key);
            Applied->SetStringField(TEXT("cpp_type"), FieldProp->GetCPPType());
            AppliedRows.Add(MakeShared<FJsonValueObject>(Applied));
            ++AppliedCount;
        }
    }

    // PostEditChangeProperty on the widget so the UMG editor's
    // preview tree refreshes (button style swap, etc.) and any
    // bound widget animations / MVVM compiled defaults pick up the
    // new style on the next compile. Synthesise the event against
    // the StyleProp itself: the UMG details panel takes the same
    // path on its slate brush picker.
    FPropertyChangedEvent PropChanged(StyleProp, EPropertyChangeType::ValueSet);
    TargetWidget->PostEditChangeProperty(PropChanged);

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_widget_style"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
    ResultObj->SetStringField(TEXT("style_field"), StyleFieldName);
    ResultObj->SetStringField(TEXT("style_struct"), StyleStruct->GetPathName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedRows);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedRows);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedCount);
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedCount);
    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Heuristic for the canonical FSlateBrush UPROPERTY on a UMG
     *  widget class. UImage stores its brush on `Brush`; UBorder on
     *  `Background`. For other classes we fall back to whatever the
     *  caller passed in `brush_field`, or scan for the first
     *  FStructProperty whose Struct is FSlateBrush. */
    FStructProperty* WidgetEdit_FindBrushProperty(UClass* WidgetClass, const FString& Hint, FString& OutPickedName)
    {
        auto IsBrushStruct = [](const FStructProperty* Prop) -> bool
        {
            return Prop && Prop->Struct && Prop->Struct->GetFName() == TEXT("SlateBrush");
        };

        if (!Hint.IsEmpty())
        {
            if (FStructProperty* Direct = FindFProperty<FStructProperty>(WidgetClass, *Hint))
            {
                if (IsBrushStruct(Direct))
                {
                    OutPickedName = Hint;
                    return Direct;
                }
            }
            return nullptr;
        }

        // No hint; try the canonical UMG names in order.
        static const TCHAR* CandidateNames[] = {
            TEXT("Brush"),         // UImage
            TEXT("Background"),    // UBorder
            TEXT("WidgetStyle"),   // some styles embed a brush slot
        };
        for (const TCHAR* Name : CandidateNames)
        {
            if (FStructProperty* Prop = FindFProperty<FStructProperty>(WidgetClass, FName(Name)))
            {
                if (IsBrushStruct(Prop))
                {
                    OutPickedName = Name;
                    return Prop;
                }
            }
        }

        // Last-ditch: walk the class looking for any FSlateBrush field.
        for (TFieldIterator<FStructProperty> It(WidgetClass); It; ++It)
        {
            FStructProperty* SP = *It;
            if (IsBrushStruct(SP))
            {
                OutPickedName = SP->GetName();
                return SP;
            }
        }
        return nullptr;
    }

    /** Pull a vector pair (x, y) out of a JSON value. Accepts an
     *  ordered array `[x, y]` or an object with `X`/`Y` / `x`/`y` keys. */
    bool WidgetEdit_ReadVec2(const TSharedPtr<FJsonValue>& V, FVector2D& OutVec)
    {
        if (!V.IsValid()) return false;
        if (V->Type == EJson::Array)
        {
            const auto& Arr = V->AsArray();
            if (Arr.Num() < 2) return false;
            OutVec.X = Arr[0]->AsNumber();
            OutVec.Y = Arr[1]->AsNumber();
            return true;
        }
        if (V->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = V->AsObject();
            double Tmp = 0.0;
            auto GetNum = [&Obj, &Tmp](const TCHAR* K) -> bool
            {
                return Obj->TryGetNumberField(K, Tmp);
            };
            double X = 0.0, Y = 0.0;
            if (GetNum(TEXT("X"))) { X = Tmp; }
            else if (GetNum(TEXT("x"))) { X = Tmp; }
            else { return false; }
            if (GetNum(TEXT("Y"))) { Y = Tmp; }
            else if (GetNum(TEXT("y"))) { Y = Tmp; }
            else { return false; }
            OutVec.X = X;
            OutVec.Y = Y;
            return true;
        }
        return false;
    }

    /** Pull an FMargin (left, top, right, bottom) from a JSON value.
     *  Accepts a 4-array `[L, T, R, B]`, a 2-array `[H, V]` (horizontal
     *  / vertical pairs), a 1-array `[U]` (uniform), or an object
     *  with `Left`/`Top`/`Right`/`Bottom` keys. */
    bool WidgetEdit_ReadMargin(const TSharedPtr<FJsonValue>& V, FMargin& OutMargin)
    {
        if (!V.IsValid()) return false;
        if (V->Type == EJson::Number)
        {
            const float U = (float)V->AsNumber();
            OutMargin = FMargin(U);
            return true;
        }
        if (V->Type == EJson::Array)
        {
            const auto& Arr = V->AsArray();
            if (Arr.Num() == 4)
            {
                OutMargin.Left = (float)Arr[0]->AsNumber();
                OutMargin.Top = (float)Arr[1]->AsNumber();
                OutMargin.Right = (float)Arr[2]->AsNumber();
                OutMargin.Bottom = (float)Arr[3]->AsNumber();
                return true;
            }
            if (Arr.Num() == 2)
            {
                const float H = (float)Arr[0]->AsNumber();
                const float Vv = (float)Arr[1]->AsNumber();
                OutMargin = FMargin(H, Vv);
                return true;
            }
            if (Arr.Num() == 1)
            {
                OutMargin = FMargin((float)Arr[0]->AsNumber());
                return true;
            }
            return false;
        }
        if (V->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = V->AsObject();
            double Tmp = 0.0;
            if (Obj->TryGetNumberField(TEXT("Left"), Tmp)) OutMargin.Left = (float)Tmp;
            if (Obj->TryGetNumberField(TEXT("Top"), Tmp)) OutMargin.Top = (float)Tmp;
            if (Obj->TryGetNumberField(TEXT("Right"), Tmp)) OutMargin.Right = (float)Tmp;
            if (Obj->TryGetNumberField(TEXT("Bottom"), Tmp)) OutMargin.Bottom = (float)Tmp;
            return true;
        }
        return false;
    }

    /** Pull an FLinearColor from a JSON value. Accepts a 3/4-array
     *  `[r, g, b, a?]`, an object with `R`/`G`/`B`/`A` (or lower-case)
     *  keys, or a string that flows through ImportText. */
    bool WidgetEdit_ReadLinearColor(const TSharedPtr<FJsonValue>& V, FLinearColor& OutColor)
    {
        if (!V.IsValid()) return false;
        if (V->Type == EJson::Array)
        {
            const auto& Arr = V->AsArray();
            if (Arr.Num() < 3) return false;
            OutColor.R = (float)Arr[0]->AsNumber();
            OutColor.G = (float)Arr[1]->AsNumber();
            OutColor.B = (float)Arr[2]->AsNumber();
            OutColor.A = Arr.Num() >= 4 ? (float)Arr[3]->AsNumber() : 1.0f;
            return true;
        }
        if (V->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = V->AsObject();
            double Tmp = 0.0;
            auto Read = [&Obj, &Tmp](const TCHAR* Upper, const TCHAR* Lower) -> bool
            {
                return Obj->TryGetNumberField(Upper, Tmp) || Obj->TryGetNumberField(Lower, Tmp);
            };
            if (!Read(TEXT("R"), TEXT("r"))) return false;
            OutColor.R = (float)Tmp;
            if (!Read(TEXT("G"), TEXT("g"))) return false;
            OutColor.G = (float)Tmp;
            if (!Read(TEXT("B"), TEXT("b"))) return false;
            OutColor.B = (float)Tmp;
            OutColor.A = Read(TEXT("A"), TEXT("a")) ? (float)Tmp : 1.0f;
            return true;
        }
        if (V->Type == EJson::String)
        {
            // Allow `(R=1,G=0,B=0,A=1)` style ImportText through.
            // FLinearColor is a non-USTRUCT POD in UE5.7's engine snapshot,
            // so the generated ::StaticStruct() accessor is not in scope.
            // TBaseStructure<FLinearColor>::Get() returns the canonical
            // UScriptStruct registered for that built-in type at boot.
            FString Str = V->AsString();
            FOutputDeviceNull NullDevice;
            FLinearColor Imported;
            const TCHAR* Ptr = *Str;
            UScriptStruct* LinearColorStruct = TBaseStructure<FLinearColor>::Get();
            if (LinearColorStruct
                && LinearColorStruct->ImportText(Ptr, &Imported, nullptr, PPF_None, &NullDevice, LinearColorStruct->GetName()) != nullptr)
            {
                OutColor = Imported;
                return true;
            }
        }
        return false;
    }

    /** Resolve a content path / short-name to a UObject. Used for
     *  texture / material references on FSlateBrush::ResourceObject. */
    UObject* WidgetEdit_ResolveResourceObject(const FString& Path)
    {
        if (Path.IsEmpty() || Path.Equals(TEXT("none"), ESearchCase::IgnoreCase)) return nullptr;
        if (UObject* Direct = StaticLoadObject(UObject::StaticClass(), nullptr, *Path))
        {
            return Direct;
        }
        // Fall back to the editor asset library path (deals with
        // package-only paths like "/Game/Foo/Bar" without the trailing
        // ".Bar" object suffix).
        if (UObject* ViaLibrary = UEditorAssetLibrary::LoadAsset(Path))
        {
            return ViaLibrary;
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetWidgetBrush(const TSharedPtr<FJsonObject>& Params)
{
    // Write an FSlateBrush field on a target child widget. The op
    // doubles as both designer sugar (texture / tint / size / margin
    // shorthands) and a full FSlateBrush reflective dict write.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target_widget"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = nullptr;
    if (UWidgetTree* Tree = WBP->WidgetTree)
    {
        Tree->ForEachWidget([&](UWidget* W)
        {
            if (TargetWidget) return;
            if (W && W->GetFName() == FName(*WidgetNameStr))
            {
                TargetWidget = W;
            }
        });
    }
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    FString BrushFieldHint;
    Params->TryGetStringField(TEXT("brush_field"), BrushFieldHint);
    if (BrushFieldHint.IsEmpty())
    {
        Params->TryGetStringField(TEXT("field"), BrushFieldHint);
    }

    UClass* WidgetClass = TargetWidget->GetClass();
    FString PickedFieldName;
    FStructProperty* BrushProp = WidgetEdit_FindBrushProperty(WidgetClass, BrushFieldHint, PickedFieldName);
    if (!BrushProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget '%s' (%s) has no FSlateBrush field named '%s' (pass 'brush_field' to target a non-default brush slot)"),
                *WidgetNameStr, *WidgetClass->GetPathName(),
                BrushFieldHint.IsEmpty() ? TEXT("<default>") : *BrushFieldHint));
    }

    FSlateBrush* BrushPtr = BrushProp->ContainerPtrToValuePtr<FSlateBrush>(TargetWidget);
    if (!BrushPtr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve FSlateBrush pointer on '%s' / '%s'"),
                *WidgetNameStr, *PickedFieldName));
    }

    // Pull the payload dict. Accepts both `brush` (designer-side
    // shape) and `properties` (the dict shape every other Sproft
    // edit op accepts).
    const TSharedPtr<FJsonObject>* BrushObj = nullptr;
    Params->TryGetObjectField(TEXT("brush"), BrushObj);
    if (!BrushObj)
    {
        Params->TryGetObjectField(TEXT("properties"), BrushObj);
    }
    if (!BrushObj)
    {
        Params->TryGetObjectField(TEXT("brush_properties"), BrushObj);
    }
    if (!BrushObj)
    {
        Params->TryGetObjectField(TEXT("values"), BrushObj);
    }
    if (!BrushObj)
    {
        // The op also accepts top-level sugar shorthands without a
        // wrapping dict so a caller can pass `texture` / `tint` /
        // `size` / `margin` directly on the params object.
        BrushObj = nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> AppliedRows;
    TArray<TSharedPtr<FJsonValue>> SkippedRows;
    int32 AppliedCount = 0;
    int32 SkippedCount = 0;

    auto NoteApplied = [&AppliedRows, &AppliedCount](const FString& Key, const FString& Detail)
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("name"), Key);
        if (!Detail.IsEmpty())
        {
            R->SetStringField(TEXT("detail"), Detail);
        }
        AppliedRows.Add(MakeShared<FJsonValueObject>(R));
        ++AppliedCount;
    };
    auto NoteSkipped = [&SkippedRows, &SkippedCount](const FString& Key, const FString& Reason, const FString& Input = FString())
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("name"), Key);
        R->SetStringField(TEXT("reason"), Reason);
        if (!Input.IsEmpty())
        {
            R->SetStringField(TEXT("input"), Input);
        }
        SkippedRows.Add(MakeShared<FJsonValueObject>(R));
        ++SkippedCount;
    };

    // Walk a flat dict and apply each entry. Sugar keys
    // (texture / material / resource_object / tint / size /
    // image_size / margin / tiling / draw_as / mirroring) win first;
    // everything else falls through to a reflective FSlateBrush
    // field write via ImportText_Direct.
    auto ApplyEntry = [&](const FString& Key, const TSharedPtr<FJsonValue>& Val) -> void
    {
        const FString Lower = Key.ToLower();

        // Texture / material / resource_object sugar: resolve the
        // path through the asset registry and land it on
        // FSlateBrush::ResourceObject. UMG's draw code accepts any
        // UTexture or UMaterialInterface as a brush resource (see
        // FSlateBrush::AllowedClasses meta on the UPROPERTY).
        if (Lower == TEXT("texture") || Lower == TEXT("material")
            || Lower == TEXT("resource_object") || Lower == TEXT("resourceobject")
            || Lower == TEXT("image"))
        {
            if (!Val.IsValid() || Val->Type == EJson::Null)
            {
                BrushPtr->SetResourceObject(nullptr);
                NoteApplied(Key, TEXT("resource_object_cleared"));
                return;
            }
            const FString Path = Val->AsString();
            if (Path.IsEmpty() || Path.Equals(TEXT("none"), ESearchCase::IgnoreCase))
            {
                BrushPtr->SetResourceObject(nullptr);
                NoteApplied(Key, TEXT("resource_object_cleared"));
                return;
            }
            UObject* Resolved = WidgetEdit_ResolveResourceObject(Path);
            if (!Resolved)
            {
                NoteSkipped(Key, TEXT("resource_object_unresolved"), Path);
                return;
            }
            const bool bIsTexture = Resolved->IsA(UTexture::StaticClass());
            const bool bIsMaterial = Resolved->IsA(UMaterialInterface::StaticClass());
            if (!bIsTexture && !bIsMaterial)
            {
                NoteSkipped(Key, TEXT("resource_object_unsupported_class"), Resolved->GetClass()->GetPathName());
                return;
            }
            BrushPtr->SetResourceObject(Resolved);
            NoteApplied(Key, Resolved->GetPathName());
            return;
        }

        if (Lower == TEXT("tint") || Lower == TEXT("tint_color") || Lower == TEXT("tintcolor"))
        {
            FLinearColor Col;
            if (!WidgetEdit_ReadLinearColor(Val, Col))
            {
                NoteSkipped(Key, TEXT("tint_unreadable"));
                return;
            }
            BrushPtr->TintColor = FSlateColor(Col);
            NoteApplied(Key, Col.ToString());
            return;
        }

        if (Lower == TEXT("size") || Lower == TEXT("image_size") || Lower == TEXT("imagesize"))
        {
            FVector2D Vec(FVector2D::ZeroVector);
            if (!WidgetEdit_ReadVec2(Val, Vec))
            {
                NoteSkipped(Key, TEXT("size_unreadable"));
                return;
            }
            BrushPtr->SetImageSize(FVector2f((float)Vec.X, (float)Vec.Y));
            NoteApplied(Key, FString::Printf(TEXT("(%g, %g)"), Vec.X, Vec.Y));
            return;
        }

        if (Lower == TEXT("margin"))
        {
            FMargin M;
            if (!WidgetEdit_ReadMargin(Val, M))
            {
                NoteSkipped(Key, TEXT("margin_unreadable"));
                return;
            }
            BrushPtr->Margin = M;
            NoteApplied(Key, FString::Printf(TEXT("(L=%g, T=%g, R=%g, B=%g)"), M.Left, M.Top, M.Right, M.Bottom));
            return;
        }

        if (Lower == TEXT("tiling") || Lower == TEXT("draw_as") || Lower == TEXT("drawas")
            || Lower == TEXT("mirroring"))
        {
            // These are TEnumAsByte<EXxx::Type> fields on FSlateBrush;
            // hand them to ImportText_Direct so canonical tokens like
            // "Image" / "Box" / "Border" / "RoundedBox" /
            // "NoTile" / "Horizontal" / "Vertical" / "Both" /
            // "NoMirror" / "Horizontal" / "Vertical" / "Both" land
            // through the enum-token machinery.
            FName FieldName;
            if (Lower == TEXT("tiling")) FieldName = TEXT("Tiling");
            else if (Lower == TEXT("mirroring")) FieldName = TEXT("Mirroring");
            else FieldName = TEXT("DrawAs");

            FProperty* FieldProp = FSlateBrush::StaticStruct()->FindPropertyByName(FieldName);
            if (!FieldProp)
            {
                NoteSkipped(Key, TEXT("enum_field_missing"));
                return;
            }
            const FString InputText = WidgetEdit_JsonValueToImportText(Val);
            void* FieldAddr = FieldProp->ContainerPtrToValuePtr<void>(BrushPtr);
            FOutputDeviceNull NullDevice;
            const TCHAR* ImportPtr = *InputText;
            const bool bImported = FieldProp->ImportText_Direct(ImportPtr, FieldAddr, TargetWidget,
                PPF_None, &NullDevice) != nullptr;
            if (!bImported)
            {
                NoteSkipped(Key, TEXT("enum_import_failed"), InputText);
                return;
            }
            NoteApplied(Key, InputText);
            return;
        }

        // Generic FSlateBrush reflective write. Lets the caller land
        // any FSlateBrush UPROPERTY by name (`OutlineSettings`,
        // `bIsDynamicallyLoaded`, etc.) without us spelling out the
        // surface.
        FProperty* FieldProp = FSlateBrush::StaticStruct()->FindPropertyByName(*Key);
        if (!FieldProp)
        {
            NoteSkipped(Key, TEXT("not_a_uproperty"));
            return;
        }
        const FString InputText = WidgetEdit_JsonValueToImportText(Val);
        void* FieldAddr = FieldProp->ContainerPtrToValuePtr<void>(BrushPtr);
        FOutputDeviceNull NullDevice;
        const TCHAR* ImportPtr = *InputText;
        const bool bImported = FieldProp->ImportText_Direct(ImportPtr, FieldAddr, TargetWidget,
            PPF_None, &NullDevice) != nullptr;
        if (!bImported)
        {
            NoteSkipped(Key, TEXT("import_text_failed"), InputText);
            return;
        }
        NoteApplied(Key, InputText);
    };

    if (BrushObj && (*BrushObj).IsValid())
    {
        for (const auto& KV : (*BrushObj)->Values)
        {
            ApplyEntry(KV.Key, KV.Value);
        }
    }

    // Top-level sugar: a caller may pass `texture` / `tint` / `size`
    // / `margin` / `tiling` / `draw_as` directly on the params
    // object (in addition to or in place of `brush`).
    static const TCHAR* TopLevelSugar[] = {
        TEXT("texture"), TEXT("material"), TEXT("resource_object"),
        TEXT("tint"), TEXT("tint_color"),
        TEXT("size"), TEXT("image_size"),
        TEXT("margin"),
        TEXT("tiling"), TEXT("draw_as"), TEXT("mirroring")
    };
    for (const TCHAR* SugarKey : TopLevelSugar)
    {
        if (Params->HasField(SugarKey))
        {
            ApplyEntry(FString(SugarKey), Params->TryGetField(SugarKey));
        }
    }

    // PostEditChangeProperty on the widget so the UMG editor's
    // preview refreshes (texture swap, etc.) and any compiled
    // defaults pick up the new brush on the next compile.
    FPropertyChangedEvent PropChanged(BrushProp, EPropertyChangeType::ValueSet);
    TargetWidget->PostEditChangeProperty(PropChanged);

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_widget_brush"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
    ResultObj->SetStringField(TEXT("brush_field"), PickedFieldName);
    if (UObject* Res = BrushPtr->GetResourceObject())
    {
        ResultObj->SetStringField(TEXT("resource_object"), Res->GetPathName());
        ResultObj->SetStringField(TEXT("resource_class"), Res->GetClass()->GetPathName());
    }
    else
    {
        ResultObj->SetStringField(TEXT("resource_object"), FString());
    }
    {
        const FLinearColor Specified = BrushPtr->TintColor.GetSpecifiedColor();
        TArray<TSharedPtr<FJsonValue>> TintArr;
        TintArr.Add(MakeShared<FJsonValueNumber>(Specified.R));
        TintArr.Add(MakeShared<FJsonValueNumber>(Specified.G));
        TintArr.Add(MakeShared<FJsonValueNumber>(Specified.B));
        TintArr.Add(MakeShared<FJsonValueNumber>(Specified.A));
        ResultObj->SetArrayField(TEXT("tint"), TintArr);
    }
    {
        TArray<TSharedPtr<FJsonValue>> SizeArr;
        const FVector2D ImageSize(BrushPtr->GetImageSize());
        SizeArr.Add(MakeShared<FJsonValueNumber>(ImageSize.X));
        SizeArr.Add(MakeShared<FJsonValueNumber>(ImageSize.Y));
        ResultObj->SetArrayField(TEXT("image_size"), SizeArr);
    }
    {
        const FMargin& Margin = BrushPtr->Margin;
        TArray<TSharedPtr<FJsonValue>> MarginArr;
        MarginArr.Add(MakeShared<FJsonValueNumber>(Margin.Left));
        MarginArr.Add(MakeShared<FJsonValueNumber>(Margin.Top));
        MarginArr.Add(MakeShared<FJsonValueNumber>(Margin.Right));
        MarginArr.Add(MakeShared<FJsonValueNumber>(Margin.Bottom));
        ResultObj->SetArrayField(TEXT("margin"), MarginArr);
    }
    ResultObj->SetArrayField(TEXT("applied"), AppliedRows);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedRows);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedCount);
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedCount);
    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Map a per-direction token (Up / Down / Left / Right / Next /
     *  Previous, case-insensitive) onto the matching FWidgetNavigationData
     *  field on UWidgetNavigation. Returns null when the token is
     *  unrecognised. The mapping mirrors `UWidgetNavigation::GetNavigationData`
     *  but is hand-rolled so we can sidestep the editor-only accessor. */
    FWidgetNavigationData* WidgetNav_FindDirectionField(UWidgetNavigation* Nav, const FString& DirectionToken)
    {
        if (!Nav)
        {
            return nullptr;
        }
        const FString Lower = DirectionToken.ToLower();
        if (Lower == TEXT("up"))       { return &Nav->Up; }
        if (Lower == TEXT("down"))     { return &Nav->Down; }
        if (Lower == TEXT("left"))     { return &Nav->Left; }
        if (Lower == TEXT("right"))    { return &Nav->Right; }
        if (Lower == TEXT("next") || Lower == TEXT("tab"))       { return &Nav->Next; }
        if (Lower == TEXT("previous") || Lower == TEXT("prev") || Lower == TEXT("shift_tab"))
        {
            return &Nav->Previous;
        }
        return nullptr;
    }

    /** Map a rule token (Escape / Stop / Wrap / Explicit / Custom /
     *  CustomBoundary, case-insensitive with `_` normalised out) onto
     *  EUINavigationRule. Returns false when the token is unrecognised
     *  so the caller can surface a clear error. */
    bool WidgetNav_ParseRule(const FString& Token, EUINavigationRule& OutRule)
    {
        const FString Norm = Token.ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
        if (Norm == TEXT("escape"))         { OutRule = EUINavigationRule::Escape; return true; }
        if (Norm == TEXT("stop"))           { OutRule = EUINavigationRule::Stop; return true; }
        if (Norm == TEXT("wrap"))           { OutRule = EUINavigationRule::Wrap; return true; }
        if (Norm == TEXT("explicit"))       { OutRule = EUINavigationRule::Explicit; return true; }
        if (Norm == TEXT("custom"))         { OutRule = EUINavigationRule::Custom; return true; }
        if (Norm == TEXT("customboundary")) { OutRule = EUINavigationRule::CustomBoundary; return true; }
        return false;
    }

    /** Render an EUINavigationRule back to its short token. */
    FString WidgetNav_RuleTokenFor(EUINavigationRule Rule)
    {
        switch (Rule)
        {
            case EUINavigationRule::Escape:         return TEXT("Escape");
            case EUINavigationRule::Stop:           return TEXT("Stop");
            case EUINavigationRule::Wrap:           return TEXT("Wrap");
            case EUINavigationRule::Explicit:       return TEXT("Explicit");
            case EUINavigationRule::Custom:         return TEXT("Custom");
            case EUINavigationRule::CustomBoundary: return TEXT("CustomBoundary");
            case EUINavigationRule::Invalid:        return TEXT("Invalid");
            default:                                return TEXT("Unknown");
        }
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetWidgetNavigation(const TSharedPtr<FJsonObject>& Params)
{
    // Writes a per-direction navigation rule onto a child widget's
    // UWidgetNavigation instance. The UWidget::Navigation slot is an
    // Instanced UPROPERTY: a widget without a customised ruleset
    // leaves it null and the runtime falls back to the engine's default
    // navigation; the moment any direction gets a non-Escape rule the
    // editor NewObject's an instance through the Instanced tag. We
    // mirror that flow: spawn the instance on demand, lay the chosen
    // direction's FWidgetNavigationData down, then route through
    // PostEditChangeProperty so the editor's preview and any open
    // UMG editor tree refresh.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_widget_navigation: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_widget_navigation: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target_widget"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_widget_navigation: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = nullptr;
    if (UWidgetTree* Tree = WBP->WidgetTree)
    {
        Tree->ForEachWidget([&](UWidget* W)
        {
            if (TargetWidget) return;
            if (W && W->GetFName() == FName(*WidgetNameStr))
            {
                TargetWidget = W;
            }
        });
    }
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_widget_navigation: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    FString DirectionToken;
    if (!Params->TryGetStringField(TEXT("direction"), DirectionToken)
        && !Params->TryGetStringField(TEXT("nav_direction"), DirectionToken)
        && !Params->TryGetStringField(TEXT("navigation_direction"), DirectionToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_widget_navigation: missing 'direction' parameter (one of Up / Down / Left / Right / Next / Previous)"));
    }

    FString RuleToken;
    if (!Params->TryGetStringField(TEXT("rule"), RuleToken)
        && !Params->TryGetStringField(TEXT("nav_rule"), RuleToken)
        && !Params->TryGetStringField(TEXT("navigation_rule"), RuleToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_widget_navigation: missing 'rule' parameter (one of Escape / Stop / Wrap / Explicit / Custom / CustomBoundary)"));
    }
    EUINavigationRule Rule = EUINavigationRule::Escape;
    if (!WidgetNav_ParseRule(RuleToken, Rule))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_widget_navigation: unknown rule '%s'; expected Escape / Stop / Wrap / Explicit / Custom / CustomBoundary"), *RuleToken));
    }

    // The optional explicit target. Required for the Explicit rule;
    // ignored otherwise (the engine's FWidgetNavigationData::WidgetToFocus
    // slot doubles as a function name for the Custom rule, so we accept
    // a `target_widget` / `target_function` shape for both cases).
    FString TargetToken;
    bool bHasTarget = Params->TryGetStringField(TEXT("target"), TargetToken)
        || Params->TryGetStringField(TEXT("target_widget"), TargetToken)
        || Params->TryGetStringField(TEXT("target_function"), TargetToken)
        || Params->TryGetStringField(TEXT("widget_to_focus"), TargetToken);
    if (Rule == EUINavigationRule::Explicit && (!bHasTarget || TargetToken.IsEmpty()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_widget_navigation: rule 'Explicit' requires 'target' (FName of the widget to focus)"));
    }
    if (Rule == EUINavigationRule::Explicit)
    {
        UWidget* ExplicitTarget = nullptr;
        if (UWidgetTree* Tree = WBP->WidgetTree)
        {
            Tree->ForEachWidget([&](UWidget* W)
            {
                if (ExplicitTarget) return;
                if (W && W->GetFName() == FName(*TargetToken))
                {
                    ExplicitTarget = W;
                }
            });
        }
        if (!ExplicitTarget)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_widget_navigation: 'target' '%s' did not match any child widget on WBP '%s' (Explicit rule needs the widget to exist; rename target first)"),
                    *TargetToken, *WBPPath));
        }
    }

    // Get or spawn the UWidgetNavigation instance on the target. The
    // Instanced UPROPERTY contract on UWidget::Navigation expects the
    // instance to outer to the widget itself; NewObject with that outer
    // matches the editor's navigation-panel "+ rule" path.
    UWidgetNavigation* Nav = TargetWidget->Navigation;
    bool bSpawnedNavigation = false;
    if (!Nav)
    {
        Nav = NewObject<UWidgetNavigation>(TargetWidget, NAME_None, RF_Transactional);
        TargetWidget->Navigation = Nav;
        bSpawnedNavigation = true;
    }

    FWidgetNavigationData* DirField = WidgetNav_FindDirectionField(Nav, DirectionToken);
    if (!DirField)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_widget_navigation: unknown direction '%s'; expected Up / Down / Left / Right / Next / Previous"), *DirectionToken));
    }

    const EUINavigationRule PrevRule = DirField->Rule;
    const FName PrevWidgetToFocus = DirField->WidgetToFocus;

    DirField->Rule = Rule;
    // Reset the target slot when leaving Explicit / Custom; both rules
    // use FWidgetNavigationData::WidgetToFocus (the second carries a
    // function name there per the comment on the field), so the slot
    // gets cleared unless the caller passed a token.
    if (Rule == EUINavigationRule::Explicit || Rule == EUINavigationRule::Custom
        || Rule == EUINavigationRule::CustomBoundary)
    {
        if (bHasTarget)
        {
            DirField->WidgetToFocus = FName(*TargetToken);
        }
    }
    else
    {
        DirField->WidgetToFocus = NAME_None;
    }
    // Drop any cached weak widget pointer; the runtime fixes this up
    // through UWidgetNavigation::ResolveRules at construction time.
    DirField->Widget.Reset();

    // PostEditChangeProperty on the widget so the UMG editor's
    // navigation panel picks up the change. The Navigation UPROPERTY
    // on UWidget is Instanced + EditAnywhere so the path mirrors what
    // the editor's per-direction picker takes.
    if (FProperty* NavProp = FindFProperty<FProperty>(TargetWidget->GetClass(), TEXT("Navigation")))
    {
        FPropertyChangedEvent PropChanged(NavProp, EPropertyChangeType::ValueSet);
        TargetWidget->PostEditChangeProperty(PropChanged);
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_widget_navigation"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("direction"), DirectionToken);
    ResultObj->SetStringField(TEXT("rule"), WidgetNav_RuleTokenFor(Rule));
    ResultObj->SetStringField(TEXT("previous_rule"), WidgetNav_RuleTokenFor(PrevRule));
    if (DirField->WidgetToFocus != NAME_None)
    {
        ResultObj->SetStringField(TEXT("target"), DirField->WidgetToFocus.ToString());
    }
    if (PrevWidgetToFocus != NAME_None)
    {
        ResultObj->SetStringField(TEXT("previous_target"), PrevWidgetToFocus.ToString());
    }
    ResultObj->SetBoolField(TEXT("spawned_navigation_instance"), bSpawnedNavigation);
    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Parse a JSON [x, y] / {X, Y} value into FVector2D. Returns true
     *  on success. Leaves OutVec untouched on failure so the caller can
     *  hold the prior value when the field is absent. */
    bool CanvasSlot_ParseVec2(const TSharedPtr<FJsonValue>& Value, FVector2D& OutVec)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() >= 2 && Arr[0].IsValid() && Arr[1].IsValid())
            {
                OutVec.X = Arr[0]->AsNumber();
                OutVec.Y = Arr[1]->AsNumber();
                return true;
            }
            return false;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
            double X = 0.0, Y = 0.0;
            if (Obj.IsValid()
                && (Obj->TryGetNumberField(TEXT("x"), X) || Obj->TryGetNumberField(TEXT("X"), X))
                && (Obj->TryGetNumberField(TEXT("y"), Y) || Obj->TryGetNumberField(TEXT("Y"), Y)))
            {
                OutVec.X = X;
                OutVec.Y = Y;
                return true;
            }
            return false;
        }
        if (Value->Type == EJson::Number)
        {
            // A scalar broadcasts onto both axes. The "1.0" pivot case
            // for `alignment` is the typical caller.
            OutVec.X = Value->AsNumber();
            OutVec.Y = Value->AsNumber();
            return true;
        }
        return false;
    }

    /** Parse a JSON [left, top, right, bottom] / {Left, Top, Right,
     *  Bottom} (case-insensitive) value into FMargin. Returns true on
     *  success. */
    bool CanvasSlot_ParseMargin(const TSharedPtr<FJsonValue>& Value, FMargin& OutMargin)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() == 4)
            {
                OutMargin.Left   = Arr[0]->AsNumber();
                OutMargin.Top    = Arr[1]->AsNumber();
                OutMargin.Right  = Arr[2]->AsNumber();
                OutMargin.Bottom = Arr[3]->AsNumber();
                return true;
            }
            if (Arr.Num() == 2)
            {
                // Horizontal / vertical broadcast.
                OutMargin.Left = OutMargin.Right = Arr[0]->AsNumber();
                OutMargin.Top = OutMargin.Bottom = Arr[1]->AsNumber();
                return true;
            }
            return false;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
            double L = OutMargin.Left, T = OutMargin.Top, R = OutMargin.Right, B = OutMargin.Bottom;
            Obj->TryGetNumberField(TEXT("left"), L);   Obj->TryGetNumberField(TEXT("Left"), L);
            Obj->TryGetNumberField(TEXT("top"), T);    Obj->TryGetNumberField(TEXT("Top"), T);
            Obj->TryGetNumberField(TEXT("right"), R);  Obj->TryGetNumberField(TEXT("Right"), R);
            Obj->TryGetNumberField(TEXT("bottom"), B); Obj->TryGetNumberField(TEXT("Bottom"), B);
            OutMargin.Left = L; OutMargin.Top = T; OutMargin.Right = R; OutMargin.Bottom = B;
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            const double N = Value->AsNumber();
            OutMargin.Left = OutMargin.Top = OutMargin.Right = OutMargin.Bottom = N;
            return true;
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetCanvasSlot(const TSharedPtr<FJsonObject>& Params)
{
    // Sugar over set_slot_property for the UCanvasPanelSlot surface.
    // The canvas slot stores its layout under an FAnchorData field
    // (`LayoutData.Anchors` for the anchor box, `LayoutData.Offsets`
    // for the offset margin, `LayoutData.Alignment` for the per-axis
    // pivot) plus `ZOrder` (int draw order) and `bAutoSize` (sizes the
    // slot to the child's preferred size when set). The engine exposes
    // setters on UCanvasPanelSlot for each field; we route through
    // those so any open editor and the cached SBox slot picker stay
    // in sync.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_canvas_slot: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_canvas_slot: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_canvas_slot: WidgetBlueprint has no WidgetTree"));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_canvas_slot: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_canvas_slot: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(TargetWidget->Slot);
    if (!Slot)
    {
        // The slot class is decided by the parent panel when the child
        // attaches. If the child's parent is not a UCanvasPanel, the
        // slot class is something else (UVerticalBoxSlot,
        // UOverlaySlot, etc.) and the canvas-specific knobs do not
        // apply. Surface a clear error so the caller knows to either
        // reparent the child or use the generic `set_slot_property`.
        UClass* SlotClass = TargetWidget->Slot ? TargetWidget->Slot->GetClass() : nullptr;
        const FString SlotClassName = SlotClass ? SlotClass->GetName() : FString(TEXT("<null>"));
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_canvas_slot: widget '%s' is not parented to a UCanvasPanel (slot class is '%s'). Reparent the child to a canvas panel or use 'set_slot_property' for non-canvas slots."),
                *WidgetNameStr, *SlotClassName));
    }

    // Capture the previous values for the diff payload. We use the
    // raw LayoutData snapshot so the caller can read every changed
    // field on a single round trip.
    const FAnchorData PrevLayout = Slot->GetLayout();
    const int32 PrevZOrder = Slot->GetZOrder();
    const bool bPrevAutoSize = Slot->GetAutoSize();

    // The new values start from the previous so a partial update
    // preserves untouched fields. We walk each optional knob, parse
    // any present value, and route through the setter so the engine
    // signals layout invalidation correctly.
    FVector2D AnchorsMin = PrevLayout.Anchors.Minimum;
    FVector2D AnchorsMax = PrevLayout.Anchors.Maximum;
    FMargin Offsets = PrevLayout.Offsets;
    FVector2D Alignment = PrevLayout.Alignment;
    int32 ZOrder = PrevZOrder;
    bool bAutoSize = bPrevAutoSize;

    TArray<FString> Applied;

    bool bWroteAnchors = false;
    {
        const TSharedPtr<FJsonValue> MinVal = Params->TryGetField(TEXT("anchors_min"));
        const TSharedPtr<FJsonValue> MaxVal = Params->TryGetField(TEXT("anchors_max"));
        if (MinVal.IsValid())
        {
            if (!CanvasSlot_ParseVec2(MinVal, AnchorsMin))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_canvas_slot: 'anchors_min' must be [x, y] or {x, y}"));
            }
            bWroteAnchors = true;
            Applied.Add(TEXT("anchors_min"));
        }
        if (MaxVal.IsValid())
        {
            if (!CanvasSlot_ParseVec2(MaxVal, AnchorsMax))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_canvas_slot: 'anchors_max' must be [x, y] or {x, y}"));
            }
            bWroteAnchors = true;
            Applied.Add(TEXT("anchors_max"));
        }
        // `anchors` accepts an object form {min:[x,y], max:[x,y]} for
        // callers who want to set the whole box in one shot, or an
        // array `[minx, miny, maxx, maxy]`. Both spellings broadcast
        // onto the AnchorsMin / AnchorsMax pair.
        const TSharedPtr<FJsonValue> AnchorsVal = Params->TryGetField(TEXT("anchors"));
        if (AnchorsVal.IsValid())
        {
            if (AnchorsVal->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = AnchorsVal->AsArray();
                if (Arr.Num() == 4)
                {
                    AnchorsMin.X = Arr[0]->AsNumber();
                    AnchorsMin.Y = Arr[1]->AsNumber();
                    AnchorsMax.X = Arr[2]->AsNumber();
                    AnchorsMax.Y = Arr[3]->AsNumber();
                    bWroteAnchors = true;
                    Applied.Add(TEXT("anchors"));
                }
                else if (Arr.Num() == 2)
                {
                    AnchorsMin.X = AnchorsMax.X = Arr[0]->AsNumber();
                    AnchorsMin.Y = AnchorsMax.Y = Arr[1]->AsNumber();
                    bWroteAnchors = true;
                    Applied.Add(TEXT("anchors"));
                }
                else
                {
                    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                        TEXT("set_canvas_slot: 'anchors' array must be [minx, miny, maxx, maxy] or [x, y]"));
                }
            }
            else if (AnchorsVal->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>& Obj = AnchorsVal->AsObject();
                const TSharedPtr<FJsonValue> Min = Obj->TryGetField(TEXT("min"));
                const TSharedPtr<FJsonValue> Max = Obj->TryGetField(TEXT("max"));
                bool bRead = false;
                if (Min.IsValid())
                {
                    bRead = CanvasSlot_ParseVec2(Min, AnchorsMin) || bRead;
                }
                if (Max.IsValid())
                {
                    bRead = CanvasSlot_ParseVec2(Max, AnchorsMax) || bRead;
                }
                if (bRead)
                {
                    bWroteAnchors = true;
                    Applied.Add(TEXT("anchors"));
                }
            }
        }
    }

    bool bWroteOffsets = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("offsets"));
        if (Val.IsValid())
        {
            if (!CanvasSlot_ParseMargin(Val, Offsets))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_canvas_slot: 'offsets' must be [left, top, right, bottom] or {left, top, right, bottom}"));
            }
            bWroteOffsets = true;
            Applied.Add(TEXT("offsets"));
        }
        // Allow `position` + `size` shorthand for the canonical
        // "place an absolutely positioned widget" caller. Position
        // and size both write into the Offsets margin under the
        // canvas layout contract (Left / Top hold the position,
        // Right / Bottom hold the size when the anchors collapse
        // onto a point).
        const TSharedPtr<FJsonValue> PosVal = Params->TryGetField(TEXT("position"));
        if (PosVal.IsValid())
        {
            FVector2D Position(Offsets.Left, Offsets.Top);
            if (!CanvasSlot_ParseVec2(PosVal, Position))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_canvas_slot: 'position' must be [x, y] or {x, y}"));
            }
            Offsets.Left = Position.X;
            Offsets.Top = Position.Y;
            bWroteOffsets = true;
            Applied.Add(TEXT("position"));
        }
        const TSharedPtr<FJsonValue> SizeVal = Params->TryGetField(TEXT("size"));
        if (SizeVal.IsValid())
        {
            FVector2D Size(Offsets.Right, Offsets.Bottom);
            if (!CanvasSlot_ParseVec2(SizeVal, Size))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_canvas_slot: 'size' must be [x, y] or {x, y}"));
            }
            Offsets.Right = Size.X;
            Offsets.Bottom = Size.Y;
            bWroteOffsets = true;
            Applied.Add(TEXT("size"));
        }
    }

    bool bWroteAlignment = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("alignment"));
        if (!Val.IsValid())
        {
            // The editor exposes the alignment field as "Alignment"; we
            // also accept "pivot" since that is the role the field plays.
            const TSharedPtr<FJsonValue> Pivot = Params->TryGetField(TEXT("pivot"));
            if (Pivot.IsValid())
            {
                if (!CanvasSlot_ParseVec2(Pivot, Alignment))
                {
                    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                        TEXT("set_canvas_slot: 'pivot' must be [x, y] or {x, y}"));
                }
                bWroteAlignment = true;
                Applied.Add(TEXT("pivot"));
            }
        }
        else
        {
            if (!CanvasSlot_ParseVec2(Val, Alignment))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_canvas_slot: 'alignment' must be [x, y] or {x, y}"));
            }
            bWroteAlignment = true;
            Applied.Add(TEXT("alignment"));
        }
    }

    bool bWroteZOrder = false;
    {
        int32 ZRead = ZOrder;
        if (Params->TryGetNumberField(TEXT("z_order"), ZRead)
            || Params->TryGetNumberField(TEXT("zorder"), ZRead)
            || Params->TryGetNumberField(TEXT("z"), ZRead))
        {
            ZOrder = ZRead;
            bWroteZOrder = true;
            Applied.Add(TEXT("z_order"));
        }
    }

    bool bWroteAutoSize = false;
    {
        bool BRead = bAutoSize;
        if (Params->TryGetBoolField(TEXT("auto_size"), BRead)
            || Params->TryGetBoolField(TEXT("autosize"), BRead)
            || Params->TryGetBoolField(TEXT("size_to_content"), BRead))
        {
            bAutoSize = BRead;
            bWroteAutoSize = true;
            Applied.Add(TEXT("auto_size"));
        }
    }

    if (Applied.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_canvas_slot: pass at least one of 'anchors_min' / 'anchors_max' / 'anchors' / 'offsets' / 'position' / 'size' / 'alignment' / 'pivot' / 'z_order' / 'auto_size'"));
    }

    // Route writes through the UCanvasPanelSlot setters so the
    // engine's layout-invalidate path fires. The setters tickle the
    // parent UCanvasPanel's cached slate widget so a live PIE session
    // picks the change up.
    Slot->Modify();
    if (bWroteAnchors)
    {
        Slot->SetAnchors(FAnchors(AnchorsMin.X, AnchorsMin.Y, AnchorsMax.X, AnchorsMax.Y));
    }
    if (bWroteOffsets)
    {
        Slot->SetOffsets(Offsets);
    }
    if (bWroteAlignment)
    {
        Slot->SetAlignment(Alignment);
    }
    if (bWroteZOrder)
    {
        Slot->SetZOrder(ZOrder);
    }
    if (bWroteAutoSize)
    {
        Slot->SetAutoSize(bAutoSize);
    }

#if WITH_EDITOR
    Slot->PostEditChange();
    TargetWidget->PostEditChange();
#endif

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    auto Vec2ToArray = [](const FVector2D& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        return Arr;
    };
    auto MarginToArray = [](const FMargin& M)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(M.Left));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Top));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Right));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Bottom));
        return Arr;
    };

    const FAnchorData NewLayout = Slot->GetLayout();

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_canvas_slot"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());

    ResultObj->SetArrayField(TEXT("anchors_min"), Vec2ToArray(NewLayout.Anchors.Minimum));
    ResultObj->SetArrayField(TEXT("anchors_max"), Vec2ToArray(NewLayout.Anchors.Maximum));
    ResultObj->SetArrayField(TEXT("offsets"), MarginToArray(NewLayout.Offsets));
    ResultObj->SetArrayField(TEXT("alignment"), Vec2ToArray(NewLayout.Alignment));
    ResultObj->SetNumberField(TEXT("z_order"), Slot->GetZOrder());
    ResultObj->SetBoolField(TEXT("auto_size"), Slot->GetAutoSize());

    ResultObj->SetArrayField(TEXT("previous_anchors_min"), Vec2ToArray(PrevLayout.Anchors.Minimum));
    ResultObj->SetArrayField(TEXT("previous_anchors_max"), Vec2ToArray(PrevLayout.Anchors.Maximum));
    ResultObj->SetArrayField(TEXT("previous_offsets"), MarginToArray(PrevLayout.Offsets));
    ResultObj->SetArrayField(TEXT("previous_alignment"), Vec2ToArray(PrevLayout.Alignment));
    ResultObj->SetNumberField(TEXT("previous_z_order"), PrevZOrder);
    ResultObj->SetBoolField(TEXT("previous_auto_size"), bPrevAutoSize);

    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Resolve a user-supplied horizontal-alignment token to the
     *  EHorizontalAlignment enum the engine stores on
     *  UOverlaySlot::HorizontalAlignment. The token list mirrors the
     *  UMG editor's "Horizontal Alignment" dropdown. */
    bool OverlaySlot_ParseHAlign(const FString& InToken, EHorizontalAlignment& OutAlign, FString& OutCanonical)
    {
        FString T = InToken.TrimStartAndEnd().ToLower();
        if (T.StartsWith(TEXT("halign_")))
        {
            T = T.RightChop(7);
        }
        T = T.Replace(TEXT("_"), TEXT(""));
        T = T.Replace(TEXT(" "), TEXT(""));
        T = T.Replace(TEXT("-"), TEXT(""));

        if (T == TEXT("fill"))      { OutAlign = HAlign_Fill;   OutCanonical = TEXT("Fill");   return true; }
        if (T == TEXT("left"))      { OutAlign = HAlign_Left;   OutCanonical = TEXT("Left");   return true; }
        if (T == TEXT("center") || T == TEXT("centre")
                                  ) { OutAlign = HAlign_Center; OutCanonical = TEXT("Center"); return true; }
        if (T == TEXT("right"))     { OutAlign = HAlign_Right;  OutCanonical = TEXT("Right");  return true; }
        return false;
    }

    /** Resolve a user-supplied vertical-alignment token to the
     *  EVerticalAlignment enum the engine stores on
     *  UOverlaySlot::VerticalAlignment. */
    bool OverlaySlot_ParseVAlign(const FString& InToken, EVerticalAlignment& OutAlign, FString& OutCanonical)
    {
        FString T = InToken.TrimStartAndEnd().ToLower();
        if (T.StartsWith(TEXT("valign_")))
        {
            T = T.RightChop(7);
        }
        T = T.Replace(TEXT("_"), TEXT(""));
        T = T.Replace(TEXT(" "), TEXT(""));
        T = T.Replace(TEXT("-"), TEXT(""));

        if (T == TEXT("fill"))      { OutAlign = VAlign_Fill;   OutCanonical = TEXT("Fill");   return true; }
        if (T == TEXT("top"))       { OutAlign = VAlign_Top;    OutCanonical = TEXT("Top");    return true; }
        if (T == TEXT("center") || T == TEXT("centre") || T == TEXT("middle")
                                  ) { OutAlign = VAlign_Center; OutCanonical = TEXT("Center"); return true; }
        if (T == TEXT("bottom"))    { OutAlign = VAlign_Bottom; OutCanonical = TEXT("Bottom"); return true; }
        return false;
    }

    FString HAlignToToken(EHorizontalAlignment InAlign)
    {
        switch (InAlign)
        {
        case HAlign_Fill:   return TEXT("Fill");
        case HAlign_Left:   return TEXT("Left");
        case HAlign_Center: return TEXT("Center");
        case HAlign_Right:  return TEXT("Right");
        default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InAlign));
        }
    }

    FString VAlignToToken(EVerticalAlignment InAlign)
    {
        switch (InAlign)
        {
        case VAlign_Fill:   return TEXT("Fill");
        case VAlign_Top:    return TEXT("Top");
        case VAlign_Center: return TEXT("Center");
        case VAlign_Bottom: return TEXT("Bottom");
        default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InAlign));
        }
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetOverlaySlot(const TSharedPtr<FJsonObject>& Params)
{
    // Sugar over set_slot_property for the UOverlaySlot surface. The
    // overlay slot stores per-axis alignment plus a padding margin.
    // The engine exposes `SetHorizontalAlignment` / `SetVerticalAlignment`
    // / `SetPadding` as the canonical mutators; routing through those
    // tickles the parent UOverlay's cached slate widget so an open UMG
    // editor refreshes on the next tick.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_overlay_slot: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_overlay_slot: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_overlay_slot: WidgetBlueprint has no WidgetTree"));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_overlay_slot: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_overlay_slot: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    UOverlaySlot* Slot = Cast<UOverlaySlot>(TargetWidget->Slot);
    if (!Slot)
    {
        // The slot class is decided by the parent panel when the child
        // attaches. If the child's parent is not a UOverlay, the slot
        // class is something else (UCanvasPanelSlot, UVerticalBoxSlot,
        // etc.) and the overlay-specific knobs do not apply. Surface
        // a clear error so the caller knows to either reparent the
        // child or use the generic `set_slot_property`.
        UClass* SlotClass = TargetWidget->Slot ? TargetWidget->Slot->GetClass() : nullptr;
        const FString SlotClassName = SlotClass ? SlotClass->GetName() : FString(TEXT("<null>"));
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_overlay_slot: widget '%s' is not parented to a UOverlay (slot class is '%s'). Reparent the child to an overlay or use 'set_slot_property' for non-overlay slots."),
                *WidgetNameStr, *SlotClassName));
    }

    // Capture the previous values for the diff payload.
    const EHorizontalAlignment PrevHAlign = Slot->GetHorizontalAlignment();
    const EVerticalAlignment PrevVAlign = Slot->GetVerticalAlignment();
    const FMargin PrevPadding = Slot->GetPadding();

    EHorizontalAlignment NewHAlign = PrevHAlign;
    EVerticalAlignment NewVAlign = PrevVAlign;
    FMargin NewPadding = PrevPadding;

    TArray<FString> Applied;

    FString HAlignToken;
    FString HAlignCanonical = HAlignToToken(PrevHAlign);
    bool bWroteHAlign = false;
    if (Params->TryGetStringField(TEXT("horizontal_alignment"), HAlignToken)
        || Params->TryGetStringField(TEXT("h_align"), HAlignToken)
        || Params->TryGetStringField(TEXT("halign"), HAlignToken)
        || Params->TryGetStringField(TEXT("hAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("HAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("horizontal"), HAlignToken))
    {
        if (!OverlaySlot_ParseHAlign(HAlignToken, NewHAlign, HAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_overlay_slot: unknown horizontal_alignment '%s'. Supported: Fill, Left, Center, Right"), *HAlignToken));
        }
        bWroteHAlign = true;
        Applied.Add(TEXT("horizontal_alignment"));
    }

    FString VAlignToken;
    FString VAlignCanonical = VAlignToToken(PrevVAlign);
    bool bWroteVAlign = false;
    if (Params->TryGetStringField(TEXT("vertical_alignment"), VAlignToken)
        || Params->TryGetStringField(TEXT("v_align"), VAlignToken)
        || Params->TryGetStringField(TEXT("valign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("VAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vertical"), VAlignToken))
    {
        if (!OverlaySlot_ParseVAlign(VAlignToken, NewVAlign, VAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_overlay_slot: unknown vertical_alignment '%s'. Supported: Fill, Top, Center, Bottom"), *VAlignToken));
        }
        bWroteVAlign = true;
        Applied.Add(TEXT("vertical_alignment"));
    }

    bool bWrotePadding = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("padding"));
        if (Val.IsValid())
        {
            if (!CanvasSlot_ParseMargin(Val, NewPadding))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_overlay_slot: 'padding' must be [left, top, right, bottom] or {left, top, right, bottom}"));
            }
            bWrotePadding = true;
            Applied.Add(TEXT("padding"));
        }
    }

    if (Applied.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_overlay_slot: pass at least one of 'horizontal_alignment' / 'vertical_alignment' / 'padding'"));
    }

    Slot->Modify();
    if (bWroteHAlign)
    {
        Slot->SetHorizontalAlignment(NewHAlign);
    }
    if (bWroteVAlign)
    {
        Slot->SetVerticalAlignment(NewVAlign);
    }
    if (bWrotePadding)
    {
        Slot->SetPadding(NewPadding);
    }

#if WITH_EDITOR
    Slot->PostEditChange();
    TargetWidget->PostEditChange();
#endif

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    auto MarginToArray = [](const FMargin& M)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(M.Left));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Top));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Right));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Bottom));
        return Arr;
    };

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_overlay_slot"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());

    ResultObj->SetStringField(TEXT("horizontal_alignment"), HAlignToToken(Slot->GetHorizontalAlignment()));
    ResultObj->SetStringField(TEXT("vertical_alignment"), VAlignToToken(Slot->GetVerticalAlignment()));
    ResultObj->SetArrayField(TEXT("padding"), MarginToArray(Slot->GetPadding()));

    ResultObj->SetStringField(TEXT("previous_horizontal_alignment"), HAlignToToken(PrevHAlign));
    ResultObj->SetStringField(TEXT("previous_vertical_alignment"), VAlignToToken(PrevVAlign));
    ResultObj->SetArrayField(TEXT("previous_padding"), MarginToArray(PrevPadding));

    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

namespace
{
    /** Resolve a user-supplied `size` shape to an FSlateChildSize. The
     *  box slot stores the rule on `SizeRule` (ESlateSizeRule::Type
     *  Auto / Fill) and a per-rule float weight on `Value`. We accept
     *  several shapes so callers do not have to spell the engine token
     *  out:
     *    - a plain string `"Auto"` (alias `"auto_size"`)
     *    - a plain string `"Fill"` with `Value=1.0` (alias `"fill"`)
     *    - a number `N` interpreted as `{Fill, N}` (weight)
     *    - an object `{rule: "Auto" | "Fill", value: N}` (alias
     *      `size_rule` for `rule`, `weight` for `value`)
     *    - an array `["Fill", N]` or `[N]` (broadcasts to Fill)
     */
    bool BoxSlot_ParseSize(const TSharedPtr<FJsonValue>& Value, FSlateChildSize& OutSize, FString& OutCanonical)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        auto Apply = [&OutSize, &OutCanonical](ESlateSizeRule::Type Rule, float Weight)
        {
            OutSize.SizeRule = Rule;
            OutSize.Value = Weight;
            OutCanonical = (Rule == ESlateSizeRule::Fill)
                ? FString::Printf(TEXT("Fill(%.4g)"), Weight)
                : FString(TEXT("Auto"));
        };

        auto ParseRuleToken = [](const FString& InToken, ESlateSizeRule::Type& OutRule) -> bool
        {
            FString T = InToken.TrimStartAndEnd().ToLower();
            T = T.Replace(TEXT("_"), TEXT(""));
            T = T.Replace(TEXT(" "), TEXT(""));
            if (T.StartsWith(TEXT("sizerule")))
            {
                T = T.RightChop(8);
            }
            if (T.StartsWith(TEXT("eslate")))
            {
                T = T.RightChop(6);
            }
            if (T == TEXT("auto") || T == TEXT("autosize"))      { OutRule = ESlateSizeRule::Automatic; return true; }
            if (T == TEXT("fill") || T == TEXT("fillweight"))    { OutRule = ESlateSizeRule::Fill;      return true; }
            return false;
        };

        if (Value->Type == EJson::String)
        {
            ESlateSizeRule::Type Rule;
            if (!ParseRuleToken(Value->AsString(), Rule))
            {
                return false;
            }
            // The Auto rule ignores Value; the Fill rule defaults to 1.0
            // weight when the caller only passes the token.
            Apply(Rule, Rule == ESlateSizeRule::Fill ? 1.0f : 1.0f);
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            // A bare number is treated as Fill(weight). The Auto rule
            // does not carry a numeric knob so the bare-number shape
            // routes through Fill.
            Apply(ESlateSizeRule::Fill, static_cast<float>(Value->AsNumber()));
            return true;
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() == 1)
            {
                if (Arr[0]->Type == EJson::String)
                {
                    ESlateSizeRule::Type Rule;
                    if (!ParseRuleToken(Arr[0]->AsString(), Rule)) { return false; }
                    Apply(Rule, 1.0f);
                    return true;
                }
                if (Arr[0]->Type == EJson::Number)
                {
                    Apply(ESlateSizeRule::Fill, static_cast<float>(Arr[0]->AsNumber()));
                    return true;
                }
                return false;
            }
            if (Arr.Num() == 2)
            {
                ESlateSizeRule::Type Rule = ESlateSizeRule::Fill;
                if (Arr[0]->Type == EJson::String)
                {
                    if (!ParseRuleToken(Arr[0]->AsString(), Rule)) { return false; }
                }
                else
                {
                    return false;
                }
                const float Weight = (Arr[1]->Type == EJson::Number)
                    ? static_cast<float>(Arr[1]->AsNumber())
                    : 1.0f;
                Apply(Rule, Weight);
                return true;
            }
            return false;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& Obj = Value->AsObject();
            FString RuleStr;
            ESlateSizeRule::Type Rule = ESlateSizeRule::Fill;
            bool bHasRule = false;
            if (Obj->TryGetStringField(TEXT("rule"), RuleStr)
                || Obj->TryGetStringField(TEXT("Rule"), RuleStr)
                || Obj->TryGetStringField(TEXT("size_rule"), RuleStr)
                || Obj->TryGetStringField(TEXT("SizeRule"), RuleStr))
            {
                if (!ParseRuleToken(RuleStr, Rule)) { return false; }
                bHasRule = true;
            }
            double Weight = 1.0;
            const bool bHasValue =
                Obj->TryGetNumberField(TEXT("value"), Weight)
                || Obj->TryGetNumberField(TEXT("Value"), Weight)
                || Obj->TryGetNumberField(TEXT("weight"), Weight)
                || Obj->TryGetNumberField(TEXT("Weight"), Weight);
            if (!bHasRule && !bHasValue)
            {
                return false;
            }
            if (!bHasRule)
            {
                // A bare weight reads as Fill(weight).
                Rule = ESlateSizeRule::Fill;
            }
            Apply(Rule, static_cast<float>(Weight));
            return true;
        }
        return false;
    }

    FString SizeRuleToToken(ESlateSizeRule::Type InRule)
    {
        switch (InRule)
        {
        case ESlateSizeRule::Automatic: return TEXT("Auto");
        case ESlateSizeRule::Fill:      return TEXT("Fill");
        default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InRule));
        }
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetBoxSlot(const TSharedPtr<FJsonObject>& Params)
{
    // Sugar over set_slot_property for the UHorizontalBoxSlot /
    // UVerticalBoxSlot pair. Both slot classes carry the same writable
    // surface (HorizontalAlignment / VerticalAlignment / Padding /
    // Size) since they share UBoxSlotBase under the hood; the engine
    // exposes the canonical `SetPadding` / `SetHorizontalAlignment` /
    // `SetVerticalAlignment` / `SetSize` setters on each class. We
    // detect which slot class the child got parented under and route
    // through the matching setter so the parent UHorizontalBox /
    // UVerticalBox cached slate widget refreshes on the next tick.
    // Mirrors the shape of set_overlay_slot / set_canvas_slot from the
    // prior batch.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_box_slot: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_box_slot: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_box_slot: WidgetBlueprint has no WidgetTree"));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_box_slot: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_box_slot: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    // Detect which UPanelSlot subclass the child carries and refuse
    // anything that is not a UHorizontalBoxSlot / UVerticalBoxSlot.
    // The slot class is decided by the parent panel when the child
    // attaches; the box-only knobs do not live on UCanvasPanelSlot or
    // UOverlaySlot. We resolve both branches up front so the writes
    // route through the concrete setter list rather than reflection.
    UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(TargetWidget->Slot);
    UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(TargetWidget->Slot);
    if (!HBoxSlot && !VBoxSlot)
    {
        UClass* SlotClass = TargetWidget->Slot ? TargetWidget->Slot->GetClass() : nullptr;
        const FString SlotClassName = SlotClass ? SlotClass->GetName() : FString(TEXT("<null>"));
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_box_slot: widget '%s' is not parented to a UHorizontalBox / UVerticalBox (slot class is '%s'). Reparent the child to a box or use 'set_slot_property' for non-box slots."),
                *WidgetNameStr, *SlotClassName));
    }

    const bool bIsHorizontal = (HBoxSlot != nullptr);

    auto GetHAlign = [&]() -> EHorizontalAlignment
    {
        return bIsHorizontal ? HBoxSlot->GetHorizontalAlignment() : VBoxSlot->GetHorizontalAlignment();
    };
    auto GetVAlign = [&]() -> EVerticalAlignment
    {
        return bIsHorizontal ? HBoxSlot->GetVerticalAlignment() : VBoxSlot->GetVerticalAlignment();
    };
    auto GetPadding = [&]() -> FMargin
    {
        return bIsHorizontal ? HBoxSlot->GetPadding() : VBoxSlot->GetPadding();
    };
    auto GetSize = [&]() -> FSlateChildSize
    {
        return bIsHorizontal ? HBoxSlot->GetSize() : VBoxSlot->GetSize();
    };

    // Capture the previous values for the diff payload.
    const EHorizontalAlignment PrevHAlign = GetHAlign();
    const EVerticalAlignment PrevVAlign = GetVAlign();
    const FMargin PrevPadding = GetPadding();
    const FSlateChildSize PrevSize = GetSize();

    EHorizontalAlignment NewHAlign = PrevHAlign;
    EVerticalAlignment NewVAlign = PrevVAlign;
    FMargin NewPadding = PrevPadding;
    FSlateChildSize NewSize = PrevSize;

    TArray<FString> Applied;

    FString HAlignToken;
    FString HAlignCanonical = HAlignToToken(PrevHAlign);
    bool bWroteHAlign = false;
    if (Params->TryGetStringField(TEXT("horizontal_alignment"), HAlignToken)
        || Params->TryGetStringField(TEXT("h_align"), HAlignToken)
        || Params->TryGetStringField(TEXT("halign"), HAlignToken)
        || Params->TryGetStringField(TEXT("hAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("HAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("horizontal"), HAlignToken))
    {
        if (!OverlaySlot_ParseHAlign(HAlignToken, NewHAlign, HAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_box_slot: unknown horizontal_alignment '%s'. Supported: Fill, Left, Center, Right"), *HAlignToken));
        }
        bWroteHAlign = true;
        Applied.Add(TEXT("horizontal_alignment"));
    }

    FString VAlignToken;
    FString VAlignCanonical = VAlignToToken(PrevVAlign);
    bool bWroteVAlign = false;
    if (Params->TryGetStringField(TEXT("vertical_alignment"), VAlignToken)
        || Params->TryGetStringField(TEXT("v_align"), VAlignToken)
        || Params->TryGetStringField(TEXT("valign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("VAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vertical"), VAlignToken))
    {
        if (!OverlaySlot_ParseVAlign(VAlignToken, NewVAlign, VAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_box_slot: unknown vertical_alignment '%s'. Supported: Fill, Top, Center, Bottom"), *VAlignToken));
        }
        bWroteVAlign = true;
        Applied.Add(TEXT("vertical_alignment"));
    }

    bool bWrotePadding = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("padding"));
        if (Val.IsValid())
        {
            if (!CanvasSlot_ParseMargin(Val, NewPadding))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_box_slot: 'padding' must be [left, top, right, bottom] or {left, top, right, bottom}"));
            }
            bWrotePadding = true;
            Applied.Add(TEXT("padding"));
        }
    }

    FString SizeCanonical = (PrevSize.SizeRule == ESlateSizeRule::Fill)
        ? FString::Printf(TEXT("Fill(%.4g)"), PrevSize.Value)
        : FString(TEXT("Auto"));
    bool bWroteSize = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("size"));
        if (Val.IsValid())
        {
            if (!BoxSlot_ParseSize(Val, NewSize, SizeCanonical))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_box_slot: 'size' must be one of: 'Auto', 'Fill', a number weight, ['Fill', weight], or {rule: 'Fill', value: weight}"));
            }
            bWroteSize = true;
            Applied.Add(TEXT("size"));
        }
    }

    if (Applied.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_box_slot: pass at least one of 'horizontal_alignment' / 'vertical_alignment' / 'padding' / 'size'"));
    }

    // Route writes through the slot's canonical setters so the engine's
    // layout-invalidate path fires. Each setter calls Invalidate on the
    // parent panel so an open UMG designer picks the change up.
    if (bIsHorizontal)
    {
        HBoxSlot->Modify();
        if (bWroteHAlign)  { HBoxSlot->SetHorizontalAlignment(NewHAlign); }
        if (bWroteVAlign)  { HBoxSlot->SetVerticalAlignment(NewVAlign); }
        if (bWrotePadding) { HBoxSlot->SetPadding(NewPadding); }
        if (bWroteSize)    { HBoxSlot->SetSize(NewSize); }
    }
    else
    {
        VBoxSlot->Modify();
        if (bWroteHAlign)  { VBoxSlot->SetHorizontalAlignment(NewHAlign); }
        if (bWroteVAlign)  { VBoxSlot->SetVerticalAlignment(NewVAlign); }
        if (bWrotePadding) { VBoxSlot->SetPadding(NewPadding); }
        if (bWroteSize)    { VBoxSlot->SetSize(NewSize); }
    }

#if WITH_EDITOR
    if (bIsHorizontal) { HBoxSlot->PostEditChange(); }
    else               { VBoxSlot->PostEditChange(); }
    TargetWidget->PostEditChange();
#endif

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    auto MarginToArray = [](const FMargin& M)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(M.Left));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Top));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Right));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Bottom));
        return Arr;
    };
    auto SizeToObject = [](const FSlateChildSize& S)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("rule"), SizeRuleToToken(S.SizeRule));
        Obj->SetNumberField(TEXT("value"), S.Value);
        return Obj;
    };

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }

    UClass* ResolvedSlotClass = bIsHorizontal
        ? UHorizontalBoxSlot::StaticClass()
        : UVerticalBoxSlot::StaticClass();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_box_slot"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("slot_class"), ResolvedSlotClass->GetName());
    ResultObj->SetStringField(TEXT("orientation"), bIsHorizontal ? TEXT("Horizontal") : TEXT("Vertical"));
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());

    ResultObj->SetStringField(TEXT("horizontal_alignment"), HAlignToToken(GetHAlign()));
    ResultObj->SetStringField(TEXT("vertical_alignment"), VAlignToToken(GetVAlign()));
    ResultObj->SetArrayField(TEXT("padding"), MarginToArray(GetPadding()));
    ResultObj->SetObjectField(TEXT("size"), SizeToObject(GetSize()));

    ResultObj->SetStringField(TEXT("previous_horizontal_alignment"), HAlignToToken(PrevHAlign));
    ResultObj->SetStringField(TEXT("previous_vertical_alignment"), VAlignToToken(PrevVAlign));
    ResultObj->SetArrayField(TEXT("previous_padding"), MarginToArray(PrevPadding));
    ResultObj->SetObjectField(TEXT("previous_size"), SizeToObject(PrevSize));

    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetGridSlot(const TSharedPtr<FJsonObject>& Params)
{
    // Sugar over set_slot_property for the UGridSlot surface. The grid
    // slot exposes Row / Column / RowSpan / ColumnSpan as the cell
    // coordinates the parent UGridPanel reads when it lays out the
    // child, plus the familiar HorizontalAlignment / VerticalAlignment /
    // Padding triple every UPanelSlot subclass carries. The engine
    // surfaces canonical setters on UGridSlot for each field
    // (`SetRow` / `SetColumn` / `SetRowSpan` / `SetColumnSpan` /
    // `SetHorizontalAlignment` / `SetVerticalAlignment` / `SetPadding`);
    // routing through those tickles the parent UGridPanel's cached
    // slate widget so an open UMG editor refreshes on the next tick.
    // Mirrors the shape of set_box_slot / set_overlay_slot /
    // set_canvas_slot from the prior batches.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_grid_slot: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_grid_slot: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_grid_slot: WidgetBlueprint has no WidgetTree"));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_grid_slot: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_grid_slot: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    UGridSlot* Slot = Cast<UGridSlot>(TargetWidget->Slot);
    if (!Slot)
    {
        // The slot class is decided by the parent panel when the child
        // attaches. If the child's parent is not a UGridPanel, the slot
        // class is something else (UCanvasPanelSlot, UOverlaySlot, etc.)
        // and the grid-specific knobs (Row / Column / RowSpan /
        // ColumnSpan) do not apply. Surface a clear error so the caller
        // knows to either reparent the child or use the generic
        // `set_slot_property`.
        UClass* SlotClass = TargetWidget->Slot ? TargetWidget->Slot->GetClass() : nullptr;
        const FString SlotClassName = SlotClass ? SlotClass->GetName() : FString(TEXT("<null>"));
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_grid_slot: widget '%s' is not parented to a UGridPanel (slot class is '%s'). Reparent the child to a grid panel or use 'set_slot_property' for non-grid slots."),
                *WidgetNameStr, *SlotClassName));
    }

    // Capture the previous values for the diff payload.
    const int32 PrevRow = Slot->GetRow();
    const int32 PrevColumn = Slot->GetColumn();
    const int32 PrevRowSpan = Slot->GetRowSpan();
    const int32 PrevColumnSpan = Slot->GetColumnSpan();
    const EHorizontalAlignment PrevHAlign = Slot->GetHorizontalAlignment();
    const EVerticalAlignment PrevVAlign = Slot->GetVerticalAlignment();
    const FMargin PrevPadding = Slot->GetPadding();

    int32 NewRow = PrevRow;
    int32 NewColumn = PrevColumn;
    int32 NewRowSpan = PrevRowSpan;
    int32 NewColumnSpan = PrevColumnSpan;
    EHorizontalAlignment NewHAlign = PrevHAlign;
    EVerticalAlignment NewVAlign = PrevVAlign;
    FMargin NewPadding = PrevPadding;

    TArray<FString> Applied;

    // Row / Column are the headline grid knobs since they decide which
    // cell of the parent UGridPanel the child lands in. Both default
    // to the previous value so a partial update preserves untouched
    // fields.
    bool bWroteRow = false;
    {
        int32 Row = NewRow;
        if (Params->TryGetNumberField(TEXT("row"), Row))
        {
            if (Row < 0)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_grid_slot: 'row' must be >= 0 (got %d)"), Row));
            }
            NewRow = Row;
            bWroteRow = true;
            Applied.Add(TEXT("row"));
        }
    }
    bool bWroteColumn = false;
    {
        int32 Column = NewColumn;
        if (Params->TryGetNumberField(TEXT("column"), Column))
        {
            if (Column < 0)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_grid_slot: 'column' must be >= 0 (got %d)"), Column));
            }
            NewColumn = Column;
            bWroteColumn = true;
            Applied.Add(TEXT("column"));
        }
    }
    // RowSpan / ColumnSpan control how many cells the child spans. We
    // refuse < 1 so a typo lands as a clear error rather than a layout
    // glitch (the engine treats <= 0 as 1 in the layout pass anyway).
    bool bWroteRowSpan = false;
    {
        int32 RowSpan = NewRowSpan;
        if (Params->TryGetNumberField(TEXT("row_span"), RowSpan)
            || Params->TryGetNumberField(TEXT("rowspan"), RowSpan)
            || Params->TryGetNumberField(TEXT("RowSpan"), RowSpan))
        {
            if (RowSpan < 1)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_grid_slot: 'row_span' must be >= 1 (got %d)"), RowSpan));
            }
            NewRowSpan = RowSpan;
            bWroteRowSpan = true;
            Applied.Add(TEXT("row_span"));
        }
    }
    bool bWroteColumnSpan = false;
    {
        int32 ColumnSpan = NewColumnSpan;
        if (Params->TryGetNumberField(TEXT("column_span"), ColumnSpan)
            || Params->TryGetNumberField(TEXT("columnspan"), ColumnSpan)
            || Params->TryGetNumberField(TEXT("ColumnSpan"), ColumnSpan))
        {
            if (ColumnSpan < 1)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_grid_slot: 'column_span' must be >= 1 (got %d)"), ColumnSpan));
            }
            NewColumnSpan = ColumnSpan;
            bWroteColumnSpan = true;
            Applied.Add(TEXT("column_span"));
        }
    }

    // The familiar HorizontalAlignment / VerticalAlignment / Padding
    // triple. We reuse the token parsers the box / overlay slot ops
    // already use so the alignment vocabulary stays consistent across
    // the slot-sugar surface.
    FString HAlignToken;
    FString HAlignCanonical = HAlignToToken(PrevHAlign);
    bool bWroteHAlign = false;
    if (Params->TryGetStringField(TEXT("horizontal_alignment"), HAlignToken)
        || Params->TryGetStringField(TEXT("h_align"), HAlignToken)
        || Params->TryGetStringField(TEXT("halign"), HAlignToken)
        || Params->TryGetStringField(TEXT("hAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("HAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("horizontal"), HAlignToken))
    {
        if (!OverlaySlot_ParseHAlign(HAlignToken, NewHAlign, HAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_grid_slot: unknown horizontal_alignment '%s'. Supported: Fill, Left, Center, Right"), *HAlignToken));
        }
        bWroteHAlign = true;
        Applied.Add(TEXT("horizontal_alignment"));
    }

    FString VAlignToken;
    FString VAlignCanonical = VAlignToToken(PrevVAlign);
    bool bWroteVAlign = false;
    if (Params->TryGetStringField(TEXT("vertical_alignment"), VAlignToken)
        || Params->TryGetStringField(TEXT("v_align"), VAlignToken)
        || Params->TryGetStringField(TEXT("valign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("VAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vertical"), VAlignToken))
    {
        if (!OverlaySlot_ParseVAlign(VAlignToken, NewVAlign, VAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_grid_slot: unknown vertical_alignment '%s'. Supported: Fill, Top, Center, Bottom"), *VAlignToken));
        }
        bWroteVAlign = true;
        Applied.Add(TEXT("vertical_alignment"));
    }

    bool bWrotePadding = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("padding"));
        if (Val.IsValid())
        {
            if (!CanvasSlot_ParseMargin(Val, NewPadding))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_grid_slot: 'padding' must be [left, top, right, bottom] or {left, top, right, bottom}"));
            }
            bWrotePadding = true;
            Applied.Add(TEXT("padding"));
        }
    }

    if (Applied.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_grid_slot: pass at least one of 'row' / 'column' / 'row_span' / 'column_span' / 'horizontal_alignment' / 'vertical_alignment' / 'padding'"));
    }

    // Route writes through the slot's canonical setters so the engine's
    // layout-invalidate path fires. Each setter calls Invalidate on the
    // parent panel so an open UMG designer picks the change up.
    Slot->Modify();
    if (bWroteRow)        { Slot->SetRow(NewRow); }
    if (bWroteColumn)     { Slot->SetColumn(NewColumn); }
    if (bWroteRowSpan)    { Slot->SetRowSpan(NewRowSpan); }
    if (bWroteColumnSpan) { Slot->SetColumnSpan(NewColumnSpan); }
    if (bWroteHAlign)     { Slot->SetHorizontalAlignment(NewHAlign); }
    if (bWroteVAlign)     { Slot->SetVerticalAlignment(NewVAlign); }
    if (bWrotePadding)    { Slot->SetPadding(NewPadding); }

#if WITH_EDITOR
    Slot->PostEditChange();
    TargetWidget->PostEditChange();
#endif

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    auto MarginToArray = [](const FMargin& M)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(M.Left));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Top));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Right));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Bottom));
        return Arr;
    };

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_grid_slot"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("slot_class"), UGridSlot::StaticClass()->GetName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());

    ResultObj->SetNumberField(TEXT("row"), Slot->GetRow());
    ResultObj->SetNumberField(TEXT("column"), Slot->GetColumn());
    ResultObj->SetNumberField(TEXT("row_span"), Slot->GetRowSpan());
    ResultObj->SetNumberField(TEXT("column_span"), Slot->GetColumnSpan());
    ResultObj->SetStringField(TEXT("horizontal_alignment"), HAlignToToken(Slot->GetHorizontalAlignment()));
    ResultObj->SetStringField(TEXT("vertical_alignment"), VAlignToToken(Slot->GetVerticalAlignment()));
    ResultObj->SetArrayField(TEXT("padding"), MarginToArray(Slot->GetPadding()));

    ResultObj->SetNumberField(TEXT("previous_row"), PrevRow);
    ResultObj->SetNumberField(TEXT("previous_column"), PrevColumn);
    ResultObj->SetNumberField(TEXT("previous_row_span"), PrevRowSpan);
    ResultObj->SetNumberField(TEXT("previous_column_span"), PrevColumnSpan);
    ResultObj->SetStringField(TEXT("previous_horizontal_alignment"), HAlignToToken(PrevHAlign));
    ResultObj->SetStringField(TEXT("previous_vertical_alignment"), VAlignToToken(PrevVAlign));
    ResultObj->SetArrayField(TEXT("previous_padding"), MarginToArray(PrevPadding));

    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetUniformGridSlot(const TSharedPtr<FJsonObject>& Params)
{
    // Sugar over set_slot_property for the UUniformGridSlot surface.
    // The uniform grid panel paints every cell at the same size so the
    // slot exposes a narrower writable surface than UGridSlot:
    // Row / Column plus HorizontalAlignment / VerticalAlignment. There
    // is no row / column span (every entry occupies one cell) and no
    // padding (the panel reads its own SlotPadding once for the whole
    // grid, not per cell). The engine surfaces canonical setters on
    // UUniformGridSlot for each writable field (`SetRow` / `SetColumn`
    // / `SetHorizontalAlignment` / `SetVerticalAlignment`); routing
    // through those tickles the parent UUniformGridPanel's cached
    // slate widget so an open UMG editor refreshes on the next tick.
    // Mirrors the shape of set_grid_slot but stays one slot class
    // narrower since UUniformGridSlot is not a UGridSlot subclass.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_uniform_grid_slot: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_uniform_grid_slot: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_uniform_grid_slot: WidgetBlueprint has no WidgetTree"));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_uniform_grid_slot: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_uniform_grid_slot: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    UUniformGridSlot* Slot = Cast<UUniformGridSlot>(TargetWidget->Slot);
    if (!Slot)
    {
        // The slot class is decided by the parent panel when the child
        // attaches. If the parent is not a UUniformGridPanel, the slot
        // class is something else (UGridSlot / UCanvasPanelSlot / etc.)
        // and the uniform-grid-specific knobs do not apply. Surface a
        // clear error so the caller either reparents the child or routes
        // through `set_grid_slot` for the regular grid panel.
        UClass* SlotClass = TargetWidget->Slot ? TargetWidget->Slot->GetClass() : nullptr;
        const FString SlotClassName = SlotClass ? SlotClass->GetName() : FString(TEXT("<null>"));
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_uniform_grid_slot: widget '%s' is not parented to a UUniformGridPanel (slot class is '%s'). Reparent the child to a uniform grid panel or use 'set_grid_slot' for a regular grid panel."),
                *WidgetNameStr, *SlotClassName));
    }

    // Capture the previous values for the diff payload.
    const int32 PrevRow = Slot->GetRow();
    const int32 PrevColumn = Slot->GetColumn();
    const EHorizontalAlignment PrevHAlign = Slot->GetHorizontalAlignment();
    const EVerticalAlignment PrevVAlign = Slot->GetVerticalAlignment();

    int32 NewRow = PrevRow;
    int32 NewColumn = PrevColumn;
    EHorizontalAlignment NewHAlign = PrevHAlign;
    EVerticalAlignment NewVAlign = PrevVAlign;

    TArray<FString> Applied;

    // Row / Column are the only cell-coordinate knobs since each entry
    // owns exactly one cell on the uniform grid. We default to the
    // previous value so a partial update preserves untouched fields.
    bool bWroteRow = false;
    {
        int32 Row = NewRow;
        if (Params->TryGetNumberField(TEXT("row"), Row))
        {
            if (Row < 0)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_uniform_grid_slot: 'row' must be >= 0 (got %d)"), Row));
            }
            NewRow = Row;
            bWroteRow = true;
            Applied.Add(TEXT("row"));
        }
    }
    bool bWroteColumn = false;
    {
        int32 Column = NewColumn;
        if (Params->TryGetNumberField(TEXT("column"), Column))
        {
            if (Column < 0)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("set_uniform_grid_slot: 'column' must be >= 0 (got %d)"), Column));
            }
            NewColumn = Column;
            bWroteColumn = true;
            Applied.Add(TEXT("column"));
        }
    }

    // Reject span / padding up front so the caller knows to route
    // through `set_grid_slot` when those knobs are wanted; UUniformGridSlot
    // does not carry them since every cell shares the same size on a
    // uniform grid (padding lives on the parent's SlotPadding instead).
    {
        int32 IgnoredSpan = 0;
        if (Params->TryGetNumberField(TEXT("row_span"), IgnoredSpan)
            || Params->TryGetNumberField(TEXT("rowspan"), IgnoredSpan)
            || Params->TryGetNumberField(TEXT("RowSpan"), IgnoredSpan)
            || Params->TryGetNumberField(TEXT("column_span"), IgnoredSpan)
            || Params->TryGetNumberField(TEXT("columnspan"), IgnoredSpan)
            || Params->TryGetNumberField(TEXT("ColumnSpan"), IgnoredSpan))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("set_uniform_grid_slot: UUniformGridSlot does not support 'row_span' / 'column_span' (every cell is one entry). Route through 'set_grid_slot' for a UGridPanel with span support."));
        }
        const TSharedPtr<FJsonValue> Padding = Params->TryGetField(TEXT("padding"));
        if (Padding.IsValid())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("set_uniform_grid_slot: UUniformGridSlot has no per-cell padding (the UUniformGridPanel reads its own SlotPadding once for the whole grid). Route through 'set_slot_property' on the parent or use 'set_grid_slot' for per-cell padding."));
        }
    }

    // The familiar HorizontalAlignment / VerticalAlignment pair. We
    // reuse the parsers the box / overlay / grid slot ops already use
    // so the alignment vocabulary stays consistent across the slot
    // sugar surface.
    FString HAlignToken;
    FString HAlignCanonical = HAlignToToken(PrevHAlign);
    bool bWroteHAlign = false;
    if (Params->TryGetStringField(TEXT("horizontal_alignment"), HAlignToken)
        || Params->TryGetStringField(TEXT("h_align"), HAlignToken)
        || Params->TryGetStringField(TEXT("halign"), HAlignToken)
        || Params->TryGetStringField(TEXT("hAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("HAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("horizontal"), HAlignToken))
    {
        if (!OverlaySlot_ParseHAlign(HAlignToken, NewHAlign, HAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_uniform_grid_slot: unknown horizontal_alignment '%s'. Supported: Fill, Left, Center, Right"), *HAlignToken));
        }
        bWroteHAlign = true;
        Applied.Add(TEXT("horizontal_alignment"));
    }

    FString VAlignToken;
    FString VAlignCanonical = VAlignToToken(PrevVAlign);
    bool bWroteVAlign = false;
    if (Params->TryGetStringField(TEXT("vertical_alignment"), VAlignToken)
        || Params->TryGetStringField(TEXT("v_align"), VAlignToken)
        || Params->TryGetStringField(TEXT("valign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("VAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vertical"), VAlignToken))
    {
        if (!OverlaySlot_ParseVAlign(VAlignToken, NewVAlign, VAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_uniform_grid_slot: unknown vertical_alignment '%s'. Supported: Fill, Top, Center, Bottom"), *VAlignToken));
        }
        bWroteVAlign = true;
        Applied.Add(TEXT("vertical_alignment"));
    }

    if (Applied.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_uniform_grid_slot: pass at least one of 'row' / 'column' / 'horizontal_alignment' / 'vertical_alignment'"));
    }

    // Route writes through the slot's canonical setters so the engine's
    // layout-invalidate path fires. Each setter calls Invalidate on the
    // parent UUniformGridPanel so an open UMG designer picks the change
    // up.
    Slot->Modify();
    if (bWroteRow)    { Slot->SetRow(NewRow); }
    if (bWroteColumn) { Slot->SetColumn(NewColumn); }
    if (bWroteHAlign) { Slot->SetHorizontalAlignment(NewHAlign); }
    if (bWroteVAlign) { Slot->SetVerticalAlignment(NewVAlign); }

#if WITH_EDITOR
    Slot->PostEditChange();
    TargetWidget->PostEditChange();
#endif

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_uniform_grid_slot"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("slot_class"), UUniformGridSlot::StaticClass()->GetName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());

    ResultObj->SetNumberField(TEXT("row"), Slot->GetRow());
    ResultObj->SetNumberField(TEXT("column"), Slot->GetColumn());
    ResultObj->SetStringField(TEXT("horizontal_alignment"), HAlignToToken(Slot->GetHorizontalAlignment()));
    ResultObj->SetStringField(TEXT("vertical_alignment"), VAlignToToken(Slot->GetVerticalAlignment()));

    ResultObj->SetNumberField(TEXT("previous_row"), PrevRow);
    ResultObj->SetNumberField(TEXT("previous_column"), PrevColumn);
    ResultObj->SetStringField(TEXT("previous_horizontal_alignment"), HAlignToToken(PrevHAlign));
    ResultObj->SetStringField(TEXT("previous_vertical_alignment"), VAlignToToken(PrevVAlign));

    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetWrapBoxSlot(const TSharedPtr<FJsonObject>& Params)
{
    // Sugar over set_slot_property for the UWrapBoxSlot surface. The
    // wrap box stacks children along a primary axis and breaks the
    // stack onto the next row / column when the total measured
    // children exceed the panel size. Each entry's slot carries the
    // familiar HorizontalAlignment / VerticalAlignment / Padding
    // triple plus two wrap-box specific knobs: bFillEmptySpace (drives
    // the "fill leftover space along the wrap axis" behaviour) and
    // FillSpan (the per-slot weight the wrap box reads when
    // balancing remaining space). The engine surfaces canonical
    // setters on UWrapBoxSlot for each field (`SetPadding` /
    // `SetFillEmptySpace` / `SetFillSpan` / `SetHorizontalAlignment` /
    // `SetVerticalAlignment`); routing through those tickles the
    // parent UWrapBox's cached slate widget so an open UMG editor
    // refreshes on the next tick.
    FString WBPPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("widget_path"), WBPPath)
        && !Params->TryGetStringField(TEXT("blueprint"), WBPPath)
        && !Params->TryGetStringField(TEXT("path"), WBPPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_wrap_box_slot: missing 'widget_blueprint' parameter"));
    }
    UObject* WBPAsset = UEditorAssetLibrary::LoadAsset(WBPPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(WBPAsset);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_wrap_box_slot: asset is not a UWidgetBlueprint: %s"), *WBPPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_wrap_box_slot: WidgetBlueprint has no WidgetTree"));
    }

    FString WidgetNameStr;
    if (!Params->TryGetStringField(TEXT("widget"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("name"), WidgetNameStr)
        && !Params->TryGetStringField(TEXT("target"), WidgetNameStr))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_wrap_box_slot: missing 'widget' parameter (target child widget FName)"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_wrap_box_slot: could not find widget '%s' on WBP '%s'"),
                *WidgetNameStr, *WBPPath));
    }

    UWrapBoxSlot* Slot = Cast<UWrapBoxSlot>(TargetWidget->Slot);
    if (!Slot)
    {
        // The slot class is decided by the parent panel when the child
        // attaches. If the parent is not a UWrapBox, the slot class is
        // something else (UHorizontalBoxSlot / UCanvasPanelSlot / etc.)
        // and the wrap-box-only knobs do not apply.
        UClass* SlotClass = TargetWidget->Slot ? TargetWidget->Slot->GetClass() : nullptr;
        const FString SlotClassName = SlotClass ? SlotClass->GetName() : FString(TEXT("<null>"));
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("set_wrap_box_slot: widget '%s' is not parented to a UWrapBox (slot class is '%s'). Reparent the child to a wrap box or use 'set_slot_property' for non-wrap slots."),
                *WidgetNameStr, *SlotClassName));
    }

    // Capture the previous values for the diff payload.
    const FMargin PrevPadding = Slot->GetPadding();
    const bool PrevFillEmptySpace = Slot->DoesFillEmptySpace();
    const float PrevFillSpan = Slot->GetFillSpanWhenLessThan();
    const EHorizontalAlignment PrevHAlign = Slot->GetHorizontalAlignment();
    const EVerticalAlignment PrevVAlign = Slot->GetVerticalAlignment();

    FMargin NewPadding = PrevPadding;
    bool NewFillEmptySpace = PrevFillEmptySpace;
    float NewFillSpan = PrevFillSpan;
    EHorizontalAlignment NewHAlign = PrevHAlign;
    EVerticalAlignment NewVAlign = PrevVAlign;

    TArray<FString> Applied;

    bool bWrotePadding = false;
    {
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(TEXT("padding"));
        if (Val.IsValid())
        {
            if (!CanvasSlot_ParseMargin(Val, NewPadding))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("set_wrap_box_slot: 'padding' must be [left, top, right, bottom] or {left, top, right, bottom}"));
            }
            bWrotePadding = true;
            Applied.Add(TEXT("padding"));
        }
    }

    bool bWroteFillEmptySpace = false;
    {
        bool BoolVal = false;
        if (Params->TryGetBoolField(TEXT("fill_empty_space"), BoolVal)
            || Params->TryGetBoolField(TEXT("fillempty_space"), BoolVal)
            || Params->TryGetBoolField(TEXT("fill"), BoolVal)
            || Params->TryGetBoolField(TEXT("bFillEmptySpace"), BoolVal))
        {
            NewFillEmptySpace = BoolVal;
            bWroteFillEmptySpace = true;
            Applied.Add(TEXT("fill_empty_space"));
        }
    }

    bool bWroteFillSpan = false;
    {
        double FillSpanVal = 0.0;
        if (Params->TryGetNumberField(TEXT("fill_span"), FillSpanVal)
            || Params->TryGetNumberField(TEXT("fillspan"), FillSpanVal)
            || Params->TryGetNumberField(TEXT("FillSpan"), FillSpanVal)
            || Params->TryGetNumberField(TEXT("span"), FillSpanVal))
        {
            NewFillSpan = static_cast<float>(FillSpanVal);
            bWroteFillSpan = true;
            Applied.Add(TEXT("fill_span"));
        }
    }

    FString HAlignToken;
    FString HAlignCanonical = HAlignToToken(PrevHAlign);
    bool bWroteHAlign = false;
    if (Params->TryGetStringField(TEXT("horizontal_alignment"), HAlignToken)
        || Params->TryGetStringField(TEXT("h_align"), HAlignToken)
        || Params->TryGetStringField(TEXT("halign"), HAlignToken)
        || Params->TryGetStringField(TEXT("hAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("HAlign"), HAlignToken)
        || Params->TryGetStringField(TEXT("horizontal"), HAlignToken))
    {
        if (!OverlaySlot_ParseHAlign(HAlignToken, NewHAlign, HAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_wrap_box_slot: unknown horizontal_alignment '%s'. Supported: Fill, Left, Center, Right"), *HAlignToken));
        }
        bWroteHAlign = true;
        Applied.Add(TEXT("horizontal_alignment"));
    }

    FString VAlignToken;
    FString VAlignCanonical = VAlignToToken(PrevVAlign);
    bool bWroteVAlign = false;
    if (Params->TryGetStringField(TEXT("vertical_alignment"), VAlignToken)
        || Params->TryGetStringField(TEXT("v_align"), VAlignToken)
        || Params->TryGetStringField(TEXT("valign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("VAlign"), VAlignToken)
        || Params->TryGetStringField(TEXT("vertical"), VAlignToken))
    {
        if (!OverlaySlot_ParseVAlign(VAlignToken, NewVAlign, VAlignCanonical))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("set_wrap_box_slot: unknown vertical_alignment '%s'. Supported: Fill, Top, Center, Bottom"), *VAlignToken));
        }
        bWroteVAlign = true;
        Applied.Add(TEXT("vertical_alignment"));
    }

    if (Applied.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("set_wrap_box_slot: pass at least one of 'padding' / 'fill_empty_space' / 'fill_span' / 'horizontal_alignment' / 'vertical_alignment'"));
    }

    // Route writes through the slot's canonical setters so the engine's
    // layout-invalidate path fires. Each setter calls Invalidate on the
    // parent UWrapBox so an open UMG designer picks the change up.
    Slot->Modify();
    if (bWrotePadding)        { Slot->SetPadding(NewPadding); }
    if (bWroteFillEmptySpace) { Slot->SetFillEmptySpace(NewFillEmptySpace); }
    if (bWroteFillSpan)       { Slot->SetFillSpanWhenLessThan(NewFillSpan); }
    if (bWroteHAlign)         { Slot->SetHorizontalAlignment(NewHAlign); }
    if (bWroteVAlign)         { Slot->SetVerticalAlignment(NewVAlign); }

#if WITH_EDITOR
    Slot->PostEditChange();
    TargetWidget->PostEditChange();
#endif

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);
    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection);
    }
    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    auto MarginToArray = [](const FMargin& M)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(M.Left));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Top));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Right));
        Arr.Add(MakeShared<FJsonValueNumber>(M.Bottom));
        return Arr;
    };

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_wrap_box_slot"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WBP->GetPathName());
    ResultObj->SetStringField(TEXT("widget"), WidgetNameStr);
    ResultObj->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("slot_class"), UWrapBoxSlot::StaticClass()->GetName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied.Num());

    ResultObj->SetArrayField(TEXT("padding"), MarginToArray(Slot->GetPadding()));
    ResultObj->SetBoolField(TEXT("fill_empty_space"), Slot->DoesFillEmptySpace());
    ResultObj->SetNumberField(TEXT("fill_span"), Slot->GetFillSpanWhenLessThan());
    ResultObj->SetStringField(TEXT("horizontal_alignment"), HAlignToToken(Slot->GetHorizontalAlignment()));
    ResultObj->SetStringField(TEXT("vertical_alignment"), VAlignToToken(Slot->GetVerticalAlignment()));

    ResultObj->SetArrayField(TEXT("previous_padding"), MarginToArray(PrevPadding));
    ResultObj->SetBoolField(TEXT("previous_fill_empty_space"), PrevFillEmptySpace);
    ResultObj->SetNumberField(TEXT("previous_fill_span"), PrevFillSpan);
    ResultObj->SetStringField(TEXT("previous_horizontal_alignment"), HAlignToToken(PrevHAlign));
    ResultObj->SetStringField(TEXT("previous_vertical_alignment"), VAlignToToken(PrevVAlign));

    ResultObj->SetBoolField(TEXT("compiled"), bCompile);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

