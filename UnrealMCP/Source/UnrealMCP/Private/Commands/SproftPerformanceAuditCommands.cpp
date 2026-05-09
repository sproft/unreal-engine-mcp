#include "Commands/SproftPerformanceAuditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "HAL/PlatformTime.h"
#include "DynamicRHI.h"
#include "RenderTimer.h"
#include "UnrealClient.h"
#include "UnrealEdGlobals.h"
#include "ViewportClient.h"

namespace
{
    /** Convert raw uint32 cycle counts to milliseconds through the
     *  same path the engine uses for stat-unit display. */
    double CyclesToMS(uint32 Cycles)
    {
        return static_cast<double>(FPlatformTime::ToMilliseconds(Cycles));
    }

    /** Pick the most-recent N samples out of `Source`'s ring buffer.
     *  `Source` is a fixed-size ring; the engine writes to it with a
     *  rolling index, but the public surface only exposes the array
     *  and not the index, so we treat the buffer as already-ordered
     *  and take the tail N. */
    int32 EffectiveSampleCount(int32 RingSize, int32 RequestedFrames)
    {
        if (RingSize <= 0)
        {
            return 0;
        }
        const int32 Effective = RequestedFrames > 0 ? RequestedFrames : RingSize;
        return FMath::Clamp(Effective, 0, RingSize);
    }

    void SummariseRing(
        const TArray<float>& Ring,
        int32 SampleCount,
        TSharedPtr<FJsonObject>& OutSummary,
        TArray<TSharedPtr<FJsonValue>>* OptOutSamples)
    {
        const int32 Total = Ring.Num();
        const int32 Take = FMath::Clamp(SampleCount, 0, Total);

        double Sum = 0.0;
        double Peak = 0.0;
        double Last = 0.0;
        int32 Counted = 0;
        for (int32 i = Total - Take; i < Total; ++i)
        {
            const float V = Ring[i];
            // Filter zero / negative entries so a partially-warm ring
            // does not skew the average down.
            if (!FMath::IsFinite(V) || V <= 0.0f)
            {
                continue;
            }
            Sum += V;
            Peak = FMath::Max(Peak, static_cast<double>(V));
            Last = static_cast<double>(V);
            ++Counted;
            if (OptOutSamples)
            {
                OptOutSamples->Add(MakeShared<FJsonValueNumber>(static_cast<double>(V)));
            }
        }
        const double Avg = Counted > 0 ? Sum / static_cast<double>(Counted) : 0.0;
        OutSummary->SetNumberField(TEXT("avg_ms"), Avg);
        OutSummary->SetNumberField(TEXT("peak_ms"), Peak);
        OutSummary->SetNumberField(TEXT("last_ms"), Last);
        OutSummary->SetNumberField(TEXT("counted_samples"), Counted);
    }

    /** Resolve the editor's active viewport and pull its FStatUnitData
     *  pointer. Returns nullptr when no viewport is active (PIE
     *  startup, headless mode, etc.). The returned `OutViewportName`
     *  is a designer-readable identifier the caller can echo. */
    FStatUnitData* TryGetActiveStatUnitData(FString& OutViewportName, FString& OutViewportPath)
    {
        if (!GUnrealEd)
        {
            return nullptr;
        }
        FViewport* Viewport = GUnrealEd->GetActiveViewport();
        if (!Viewport)
        {
            return nullptr;
        }
        FViewportClient* Client = Viewport->GetClient();
        if (!Client)
        {
            return nullptr;
        }
        FStatUnitData* Data = Client->GetStatUnitData();
        if (!Data)
        {
            return nullptr;
        }
        OutViewportName = Client->GetWorld() ? Client->GetWorld()->GetName() : TEXT("editor_active");
        if (UWorld* World = Client->GetWorld())
        {
            OutViewportPath = World->GetPathName();
        }
        return Data;
    }
}

FSproftPerformanceAuditCommands::FSproftPerformanceAuditCommands()
{
}

TSharedPtr<FJsonObject> FSproftPerformanceAuditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("performance_audit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown performance_audit command: %s"), *CommandType));
    }
    return HandlePerformanceAudit(Params);
}

TSharedPtr<FJsonObject> FSproftPerformanceAuditCommands::HandlePerformanceAudit(const TSharedPtr<FJsonObject>& Params)
{
    int32 RequestedFrames = 60;
    if (Params.IsValid())
    {
        int32 Parsed = 0;
        if (Params->TryGetNumberField(TEXT("frames"), Parsed) && Parsed > 0)
        {
            RequestedFrames = Parsed;
        }
    }

    bool bIncludeSamples = false;
    TSet<FString> MetricFilter;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("include_samples"), bIncludeSamples);
        const TArray<TSharedPtr<FJsonValue>>* Filter = nullptr;
        if (Params->TryGetArrayField(TEXT("metrics"), Filter) && Filter)
        {
            for (const TSharedPtr<FJsonValue>& V : *Filter)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    MetricFilter.Add(V->AsString().ToLower());
                }
            }
        }
    }

    auto MetricEnabled = [&MetricFilter](const TCHAR* Token) -> bool
    {
        if (MetricFilter.Num() == 0)
        {
            return true;
        }
        return MetricFilter.Contains(FString(Token));
    };

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("requested_frames"), RequestedFrames);

    FString ViewportName;
    FString ViewportPath;
    FStatUnitData* StatData = TryGetActiveStatUnitData(ViewportName, ViewportPath);

    if (StatData)
    {
        TSharedPtr<FJsonObject> ViewportObj = MakeShared<FJsonObject>();
        ViewportObj->SetStringField(TEXT("world_name"), ViewportName);
        if (!ViewportPath.IsEmpty())
        {
            ViewportObj->SetStringField(TEXT("world_path"), ViewportPath);
        }
        Result->SetObjectField(TEXT("viewport"), ViewportObj);

#if !UE_BUILD_SHIPPING
        const int32 RingSize = StatData->FrameTimes.Num();
        const int32 EffSamples = EffectiveSampleCount(RingSize, RequestedFrames);
        Result->SetNumberField(TEXT("max_samples"), RingSize);
        Result->SetNumberField(TEXT("sample_count"), EffSamples);

        struct FMetricRow
        {
            const TCHAR* Token;
            const TArray<float>* Ring;
        };
        const TArray<float>& GPU0 = StatData->GPUFrameTimes[0];
        const FMetricRow Rows[] = {
            { TEXT("frame"),  &StatData->FrameTimes },
            { TEXT("game"),   &StatData->GameThreadTimes },
            { TEXT("render"), &StatData->RenderThreadTimes },
            { TEXT("rhi"),    &StatData->RHITTimes },
            { TEXT("gpu"),    &GPU0 },
        };
        TSharedPtr<FJsonObject> SamplesObj = bIncludeSamples ? MakeShared<FJsonObject>() : nullptr;
        for (const FMetricRow& Row : Rows)
        {
            if (!MetricEnabled(Row.Token))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> Samples;
            SummariseRing(*Row.Ring, EffSamples, Summary, bIncludeSamples ? &Samples : nullptr);
            Result->SetObjectField(Row.Token, Summary);
            if (bIncludeSamples)
            {
                SamplesObj->SetArrayField(Row.Token, Samples);
            }
        }
        if (bIncludeSamples)
        {
            Result->SetObjectField(TEXT("samples"), SamplesObj);
        }
#else
        Result->SetNumberField(TEXT("max_samples"), 0);
        Result->SetNumberField(TEXT("sample_count"), 0);
        Result->SetBoolField(TEXT("ring_unavailable_in_shipping"), true);
#endif
    }
    else
    {
        // Live FStatUnitData not reachable: we still emit the
        // cycle-counter globals below so a caller gets _some_ signal
        // out of a headless or pre-viewport editor session.
        Result->SetField(TEXT("viewport"), MakeShared<FJsonValueNull>());
        Result->SetNumberField(TEXT("max_samples"), 0);
        Result->SetNumberField(TEXT("sample_count"), 0);
    }

    if (MetricEnabled(TEXT("globals")))
    {
        // Cycle-counter globals. These tick once per frame in
        // FViewport::Draw and are the closest thing to a live "right
        // now" snapshot the engine exposes outside the stats system.
        TSharedPtr<FJsonObject> Globals = MakeShared<FJsonObject>();
        extern ENGINE_API float GAverageMS;
        extern ENGINE_API float GAverageFPS;
        Globals->SetNumberField(TEXT("average_ms"), static_cast<double>(GAverageMS));
        Globals->SetNumberField(TEXT("average_fps"), static_cast<double>(GAverageFPS));
        Globals->SetNumberField(TEXT("game_thread_ms"), CyclesToMS(GGameThreadTime));
        Globals->SetNumberField(TEXT("game_thread_wait_ms"), CyclesToMS(GGameThreadWaitTime));
        Globals->SetNumberField(TEXT("render_thread_ms"), CyclesToMS(GRenderThreadTime));
        Globals->SetNumberField(TEXT("render_thread_wait_ms"), CyclesToMS(GRenderThreadWaitTime));
        Globals->SetNumberField(TEXT("rhi_thread_ms"), CyclesToMS(GRHIThreadTime));
        Globals->SetNumberField(TEXT("gpu_frame_ms"), CyclesToMS(RHIGetGPUFrameCycles(0)));
        Result->SetObjectField(TEXT("globals"), Globals);
    }

    return Result;
}
