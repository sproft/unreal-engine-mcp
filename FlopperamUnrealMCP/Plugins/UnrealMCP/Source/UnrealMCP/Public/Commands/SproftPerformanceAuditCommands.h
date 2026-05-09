#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: performance_audit (small, read-only)
 *
 * Frame-time / thread-time snapshot for the active editor viewport.
 *
 * Inputs (all optional):
 *   - frames:       window size for the running averages, default 60,
 *                   capped at the engine's `FStatUnitData::NumberOfSamples`
 *                   ring (200 samples in non-shipping builds).
 *   - metrics:      optional list of metric tokens to filter the report
 *                   to a subset (`frame`, `game`, `render`, `rhi`,
 *                   `gpu`, `globals`, `samples`).
 *   - include_samples: opt-in raw per-frame array dump for each kept
 *                   metric. Default false.
 *
 * Returns:
 *   - frame_count, sample_count, max_samples (the engine's ring size).
 *   - per-metric `{ avg_ms, peak_ms, last_ms }` blocks for frame /
 *     game-thread / render-thread / RHI / GPU.
 *   - `globals`: the live cycle-counter globals (GAverageMS / GAverageFPS
 *     plus the cycle-converted GGameThreadTime / GRenderThreadTime /
 *     GRHIThreadTime / GGPUFrameTime).
 *   - optional `samples` array (only when include_samples=true).
 *   - `viewport`: name + path of the editor viewport client used to
 *     read the FStatUnitData (or `null` when no live viewport client
 *     is reachable, in which case we fall back to the cycle-counter
 *     globals only).
 *
 * Skips the deep-dive captures (`stat startfile` / `stat stopfile`,
 * Insights traces, FPSChart). Those stay on the backlog.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UEditorEngine::GetActiveViewport()` for the active editor
 *     viewport, then `FViewportClient::GetStatUnitData()`.
 *   - `FStatUnitData` ring buffers (RenderThreadTimes / GameThreadTimes
 *     / GPUFrameTimes / FrameTimes / RHITTimes).
 *   - `extern ENGINE_API float GAverageMS / GAverageFPS`.
 *   - `extern RENDERCORE_API uint32 GGameThreadTime / GRenderThreadTime
 *     / GRHIThreadTime / GGameThreadWaitTime`.
 *   - `extern RHI_API uint32 GGPUFrameTime`.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftPerformanceAuditCommands
{
public:
    FSproftPerformanceAuditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandlePerformanceAudit(const TSharedPtr<FJsonObject>& Params);
};
