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
 *   - "set_binding_conversion": rewrite the per-direction conversion
 *      slot on an existing `FMVVMBlueprintViewBinding`. Resolves the
 *      target binding by FGuid binding-id string or by integer
 *      index, then either clears
 *      `Conversion.SourceToDestinationConversion` /
 *      `Conversion.DestinationToSourceConversion` (when
 *      `conversion_function` is empty / `none` / `clear=true`) or
 *      NewObject's a fresh `UMVVMBlueprintViewConversionFunction`
 *      outered to the WBP, runs
 *      `Initialize(WBP, CreateWrapperName(Binding, bSourceToDestination),
 *      FMVVMBlueprintFunctionReference(WBP, UFunction*))` against
 *      the resolved conversion UFunction. The direction defaults to
 *      `source_to_destination`; the `direction` token accepts
 *      `source_to_destination` / `destination_to_source` /
 *      `forward` / `backward` plus the canonical
 *      `SourceToDestination` / `DestinationToSource` spellings.
 *      Replacing an existing conversion runs
 *      `RemoveWrapperGraph` on the old slot so the wrapper graph
 *      garbage collects.
 *   - "add_event_binding": spawn (or focus) a
 *      `UK2Node_ComponentBoundEvent` in the WBP's event graph for a
 *      named child widget's multicast delegate property
 *      (`OnClicked` / `OnHovered` / `OnTextCommitted` /
 *      `OnValueChanged`, etc.). Resolves the FObjectProperty for
 *      the child widget on the WBP's SkeletonGeneratedClass and
 *      the FMulticastDelegateProperty on that widget's UClass,
 *      then routes through
 *      `FKismetEditorUtilities::CreateNewBoundEventForClass` to
 *      land the bound-event node in the last edited ubergraph.
 *      Existing matching nodes (same component + delegate) get
 *      reused so the op is idempotent. An optional
 *      `handler_function` runs `FBlueprintEditorUtils::RenameNode`
 *      on the spawned event so the K2Node's CustomFunctionName
 *      lands on the caller's chosen handler.
 *   - "set_widget_style": apply a flat property dict to a target
 *      child widget's style struct field through reflection.
 *      Defaults to the `WidgetStyle` UPROPERTY (UButton /
 *      UProgressBar / UScrollBar / UScrollBox / USlider / UCheckBox
 *      / UEditableText / UEditableTextBox / UComboBox / etc.); an
 *      optional `style_field` knob lets the caller target other
 *      style slots (`WidgetBarStyle` on a UScrollBox, etc.). Each
 *      entry in `style` writes through `FProperty::ImportText_Direct`
 *      against the field on the resolved style struct so designers
 *      can rebrand a button's `Normal` brush, a progress bar's
 *      `FillImage`, or a scrollbar's `Thumb`, without spelling out
 *      the struct surface by hand. Failed entries surface under
 *      the response's `skipped` array with a reason. After the
 *      writes `PostEditChangeProperty` fires on the widget so the
 *      UMG editor's preview refreshes and the variable's compiled
 *      default propagates.
 *   - "set_widget_brush": write an FSlateBrush field on a target
 *      child widget. `brush_field` picks the FSlateBrush UPROPERTY
 *      (e.g. `Brush` on UImage, `Background` on UBorder,
 *      `NormalThumbnail` on USlider, `FillImage` on UProgressBar);
 *      defaults to the canonical brush field for the widget's
 *      class (`Brush` for UImage, `Background` for UBorder).
 *      Supports both designer-sugar keys (`texture` /
 *      `material` / `tint` / `tint_color` / `size` / `image_size`
 *      / `margin` / `tiling` / `draw_as` / `mirroring`) and the
 *      raw FSlateBrush field set (any FSlateBrush UPROPERTY
 *      surfaced through reflection). Texture / material paths
 *      resolve through the asset registry and land on
 *      `ResourceObject`; tint accepts a `[r,g,b,a]` array, an
 *      `{R,G,B,A}` object, or a colour token; size accepts
 *      `[x,y]` / `{X,Y}`; margin accepts `[l,t,r,b]` or
 *      `{Left,Top,Right,Bottom}`. After the writes
 *      `PostEditChangeProperty` fires on the widget so the UMG
 *      editor's preview refreshes.
 *   - "set_overlay_slot": single-call sugar over `set_slot_property`
 *      for the UOverlaySlot surface. Takes the widget blueprint plus
 *      a target child widget FName plus the `horizontal_alignment`
 *      token (`Fill` / `Left` / `Center` / `Right`) and / or the
 *      `vertical_alignment` token (`Fill` / `Top` / `Center` /
 *      `Bottom`) plus an optional `padding` `[left, top, right,
 *      bottom]` margin. The UOverlaySlot exposes
 *      `SetHorizontalAlignment` / `SetVerticalAlignment` /
 *      `SetPadding` as the canonical mutators; we route through those
 *      so the parent UOverlay's cached slate widget refreshes
 *      (`SBox`'s slot picker rebuilds on next tick). Refuses children
 *      whose parent is not a UOverlay since the slot class on a
 *      canvas / vertical box child does not carry these fields.
 *      Complements `set_canvas_slot` for the overlay-anchored UMG
 *      layout case.
 *   - "set_grid_slot": single-call sugar over `set_slot_property` for
 *      the UGridSlot surface. Takes the widget blueprint plus a target
 *      child widget FName plus any of the canonical grid slot knobs:
 *      `row` / `column` (int cell coordinates the parent UGridPanel
 *      reads when it lays out the child), `row_span` / `column_span`
 *      (int, >= 1; how many cells the child spans), `padding` (the
 *      `[L, T, R, B]` / `[H, V]` / uniform / object margin shape we use
 *      across the slot ops), and `horizontal_alignment` /
 *      `vertical_alignment` tokens (`Fill` / `Left` / `Center` /
 *      `Right` and `Fill` / `Top` / `Center` / `Bottom` respectively).
 *      Refuses children whose parent is not a UGridPanel since the
 *      grid-only fields (Row / Column / RowSpan / ColumnSpan) do not
 *      live on UCanvasPanelSlot or UOverlaySlot. Routes through the
 *      concrete `UGridSlot::SetRow` / `SetColumn` / `SetRowSpan` /
 *      `SetColumnSpan` / `SetHorizontalAlignment` / `SetVerticalAlignment`
 *      / `SetPadding` setters so the parent UGridPanel's cached slate
 *      widget invalidates. Complements `set_box_slot` for the
 *      two-dimensional grid layout case.
 *   - "set_uniform_grid_slot": single-call sugar over
 *      `set_slot_property` for the UUniformGridSlot surface. Mirrors
 *      `set_grid_slot` but targets the uniform grid where every cell
 *      shares the same size. Takes the widget blueprint plus a target
 *      child widget FName plus any of the uniform grid slot knobs:
 *      `row` / `column` (int cell coordinates the parent
 *      UUniformGridPanel reads when it lays out the child) plus the
 *      familiar `horizontal_alignment` / `vertical_alignment` tokens
 *      (`Fill` / `Left` / `Center` / `Right` and `Fill` / `Top` /
 *      `Center` / `Bottom`). UUniformGridSlot does not carry span or
 *      padding fields since every cell shares the same size on a
 *      uniform grid; the op refuses those knobs at parse time so the
 *      caller can route through `set_grid_slot` if a regular grid is
 *      wanted. Refuses children whose parent is not a
 *      UUniformGridPanel. Routes through the concrete
 *      `UUniformGridSlot::SetRow` / `SetColumn` /
 *      `SetHorizontalAlignment` / `SetVerticalAlignment` setters so
 *      the parent UUniformGridPanel's cached slate widget
 *      invalidates.
 *   - "set_wrap_box_slot": single-call sugar over
 *      `set_slot_property` for the UWrapBoxSlot surface. Takes the
 *      widget blueprint plus a target child widget FName plus any
 *      of the wrap-box slot knobs: `padding` (the `[L, T, R, B]` /
 *      `[H, V]` / uniform / object margin shape we use across the
 *      slot ops), `fill_empty_space` (bool, drives the wrap-box's
 *      "fill leftover space along the wrap axis" behaviour),
 *      `fill_span` (float, the per-slot weight the wrap box reads
 *      when balancing remaining space), and the
 *      `horizontal_alignment` / `vertical_alignment` tokens (`Fill`
 *      / `Left` / `Center` / `Right` and `Fill` / `Top` /
 *      `Center` / `Bottom`). Refuses children whose parent is not
 *      a UWrapBox. Routes through the concrete
 *      `UWrapBoxSlot::SetPadding` / `SetFillEmptySpace` /
 *      `SetFillSpan` / `SetHorizontalAlignment` /
 *      `SetVerticalAlignment` setters so the parent UWrapBox's
 *      cached slate widget invalidates.
 *   - "set_canvas_slot": single-call sugar over `set_slot_property`
 *      for the UCanvasPanelSlot surface. Takes the widget blueprint
 *      plus a target child widget FName plus any of the canonical
 *      canvas slot knobs: `anchors_min` `[x, y]` / `anchors_max`
 *      `[x, y]` (the anchor box; equal min == max collapses the
 *      anchor onto a point), `offsets` `[left, top, right, bottom]`
 *      (the FAnchorData::Offsets margin: with auto-size off it
 *      doubles as width / height when min == max, otherwise it stays
 *      a margin only), `alignment` `[x, y]` (the per-axis pivot the
 *      offsets resolve against), `z_order` (int draw-order under the
 *      panel), and `auto_size` (bool, drives
 *      `UCanvasPanelSlot::bAutoSize` so the slot sizes to the child's
 *      preferred size). Refuses children whose parent is not a
 *      UCanvasPanel since the slot class on a vertical box / overlay
 *      child does not carry these fields. After the writes
 *      `PostEditChange` fires on the slot and the parent panel
 *      reflows so an open UMG editor refresh picks the slot change
 *      up. Useful for canvas-anchored UMG layouts without manual
 *      UPROPERTY-by-UPROPERTY tweaks.
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
    TSharedPtr<FJsonObject> SetBindingConversion(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> AddEventBinding(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetWidgetStyle(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetWidgetBrush(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetWidgetNavigation(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetCanvasSlot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetOverlaySlot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetBoxSlot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetGridSlot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetUniformGridSlot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetWrapBoxSlot(const TSharedPtr<FJsonObject>& Params);
};
