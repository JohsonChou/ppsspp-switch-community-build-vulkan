// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(SWITCH_PERF_DIAGNOSTICS)

namespace PerfDiagnostics {

enum class Metric : uint8_t {
	HOST_READ,
	READ_AHEAD_HIT,
	ISO_READ,
	CHD_HUNK_READ,
	SHADER_MODULE,
	SHADER_WAIT,
	PIPELINE_CREATE,
	PIPELINE_WAIT,
	RENDER_SUBMIT_WAIT,
	FRAME_FENCE_WAIT,
	SWAPCHAIN_ACQUIRE,
	SWAPCHAIN_PRESENT,
	RENDER_DESCRIPTOR_FLUSH,
	RENDER_STEPS,
	RENDER_QUEUE_SUBMIT,
	DRAW_BEGIN,
	EMULATION_FRAME,
	GPU_BEGIN_HOST_FRAME,
	CPU_RUN_LOOP,
	GPU_PREPARE_DISPLAY,
	GPU_END_HOST_FRAME,
	SCREEN_RENDER,
	DRAW_END,
	POST_SUBMIT,
	PRESENT,
	COUNT,
};

struct MetricSnapshot {
	uint64_t totalUs = 0;
	uint64_t maxUs = 0;
	uint64_t work = 0;
	uint32_t count = 0;
};

using FrameSnapshot = std::array<MetricSnapshot, static_cast<size_t>(Metric::COUNT)>;

void BeginFrame();
void Record(Metric metric, double elapsedSeconds, uint64_t work = 0);
void RecordWork(Metric metric, uint64_t work);
FrameSnapshot SnapshotFrame();

inline const MetricSnapshot &Get(const FrameSnapshot &snapshot, Metric metric) {
	return snapshot[static_cast<size_t>(metric)];
}

}  // namespace PerfDiagnostics

#endif
