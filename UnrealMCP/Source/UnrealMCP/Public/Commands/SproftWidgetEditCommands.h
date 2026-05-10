#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: widget_edit
 *
 * A trimmed clone of the hosted Flop "widget_edit" surface. Operations:
 *   - "create_widget_blueprint": create a new UWidgetBlueprint with a parent
 *      class and an optional root panel widget class.
 *   - "add_child_widget": construct a named widget (Vertical Box, Horizontal
 *      Box, Progress Bar, Text Block, Button, Image) and attach it as a child
 *      of an existing widget inside a Widget Blueprint's tree.
 *   - "set_slot_property": apply a flat property dict to the UPanelSlot of
 *      an existing widget. Covers UCanvasPanelSlot anchors / offsets / size,
 *      UVerticalBoxSlot / UHorizontalBoxSlot padding / fill, and any other
 *      UPanelSlot-derived class without us spelling out each property by
 *      name. Properties go through `FProperty::ImportText_InContainer`.
 *   - "add_animation": append a new UWidgetAnimation to the
 *      `UWidgetBlueprint::Animations` array. The animation owns a fresh
 *      UMovieScene whose playback range covers `[0, duration]` seconds.
 *   - "add_animation_track": append a UMovieSceneTrack subclass against an
 *      existing animation's UMovieScene, scoped to the binding for a target
 *      widget by FName so the track resolves correctly at runtime.
 *   - "add_keyframe": write a key into a track's first section. Resolves
 *      the target track by index into the animation's MovieScene
 *      `GetTracks()` array. If the track has no section yet we spawn one
 *      through `UMovieSceneTrack::CreateNewSection` + `AddSection`; if it
 *      does, we expand the first section's range to cover the key time
 *      via `UMovieSceneSection::ExpandToFrame` and write the key through
 *      `FMovieSceneFloatChannel::AddCubicKey` (or
 *      `FMovieSceneDoubleChannel::AddCubicKey` for the 5.4+ vector track
 *      shape). Vector tracks accept an `[x, y, z, w?]` JSON array routed
 *      to channels 0..N-1; transform tracks take the same vector shape.
 *   - "set_viewmodel": MVVM minimum cut. Resolves an existing
 *      UWidgetBlueprint and a UClass implementing
 *      `INotifyFieldValueChanged` (UMVVMViewModelBase subclasses are the
 *      canonical case) and routes through
 *      `UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>(WidgetBlueprint)`
 *      to get-or-create the MVVM extension on the WBP. If the extension
 *      has no `UMVVMBlueprintView` instance yet we call
 *      `CreateBlueprintViewInstance()`, then route a new
 *      `FMVVMBlueprintViewModelContext(ClassPtr, ViewModelName)` through
 *      `UMVVMBlueprintView::AddViewModel`. An optional `binding_name`
 *      arg also runs `UMVVMBlueprintView::AddDefaultBinding()` so the
 *      caller gets one binding row seeded; the row's `SourcePath` /
 *      `DestinationPath` stay at default ready for downstream
 *      `set_binding_path` ops. Conversion functions, two-way bindings,
 *      and bindings to widget properties beyond root stay on the
 *      BACKLOG.
 *   - "add_property_binding": full MVVM binding row authoring. Resolves
 *      an existing UWidgetBlueprint plus a viewmodel slot on its
 *      UMVVMBlueprintView (by FName or by the resolved FGuid context
 *      id) plus a source field name on that viewmodel's class plus a
 *      destination widget FName on the widget tree plus a destination
 *      property name on the widget's class. Routes through
 *      `UMVVMBlueprintView::AddDefaultBinding()` to get a fresh
 *      `FMVVMBlueprintViewBinding`, then sets the row's `SourcePath`
 *      to (ViewModelContextId, viewmodel field) and `DestinationPath`
 *      to (WidgetName, widget property) through the public
 *      `FMVVMBlueprintPropertyPath::SetViewModelId` +
 *      `SetPropertyPath(WBP, FieldVariant)` and `SetWidgetName` +
 *      `SetPropertyPath(WBP, FieldVariant)` setters. The
 *      `binding_mode` token maps `one_way` / `two_way` / `one_time`
 *      onto the canonical `EMVVMBindingMode` enum values
 *      (`OneWayToDestination` / `TwoWay` / `OneTimeToDestination`).
 *      Conversion functions stay on the BACKLOG.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FKismetEditorUtilities::CreateBlueprint for asset creation
 *   - UWidgetBlueprint / UWidgetBlueprintGeneratedClass for the asset class
 *   - UWidgetTree::ConstructWidget + UPanelWidget::AddChild for the tree
 *   - UWidget::Slot for the per-widget UPanelSlot pointer
 *   - FProperty::ImportText_InContainer for the property dict surface
 *   - UWidgetBlueprint::Animations + UMovieScene::SetPlaybackRange +
 *     UMovieScene::SetDisplayRate for the animation surface
 *   - UWidgetAnimation::AnimationBindings + UMovieScene::AddPossessable +
 *     UMovieScene::AddTrack for the track surface
 *   - FBlueprintEditorUtils::MarkBlueprintAsModified for change notification
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftWidgetEditCommands
{
public:
    FSproftWidgetEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleWidgetEdit(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddChildWidget(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetSlotProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddAnimation(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddAnimationTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddAnimationKeyframe(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetViewModel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddPropertyBinding(const TSharedPtr<FJsonObject>& Params);
};
