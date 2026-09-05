// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/PerfDiagnostics.h"

#if defined(SWITCH_PERF_DIAGNOSTICS)

#include <atomic>

namespace PerfDiagnostics {

namespace {

struct AtomicMetric {
	std::atomic<uint64_t> totalUs{0};
	std::atomic<uint64_t> maxUs{0};
	std::atomic<uint64_t> work{0};
	std::atomic<uint32_t> count{0};
};

using AtomicFrame = std::array<AtomicMetric, static_cast<size_t>(Metric::COUNT)>;

AtomicFrame frameStats[2];
std::atomic<uint32_t> activeFrame{0};

void ResetFrame(AtomicFrame &frame) {
	for (AtomicMetric &metric : frame) {
		metric.totalUs.store(0, std::memory_order_relaxed);
		metric.maxUs.store(0, std::memory_order_relaxed);
		metric.work.store(0, std::memory_order_relaxed);
		metric.count.store(0, std::memory_order_relaxed);
	}
}

}  // namespace

void BeginFrame() {
	const uint32_t nextFrame = activeFrame.load(std::memory_order_relaxed) ^ 1;
	ResetFrame(frameStats[nextFrame]);
	activeFrame.store(nextFrame, std::memory_order_release);
}

void Record(Metric metric, double elapsedSeconds, uint64_t work) {
	if (elapsedSeconds <= 0.0) {
		return;
	}

	const uint64_t elapsedUs = static_cast<uint64_t>(elapsedSeconds * 1000000.0 + 0.5);
	AtomicMetric &stats = frameStats[activeFrame.load(std::memory_order_acquire)][static_cast<size_t>(metric)];
	stats.totalUs.fetch_add(elapsedUs, std::memory_order_relaxed);
	stats.work.fetch_add(work, std::memory_order_relaxed);
	stats.count.fetch_add(1, std::memory_order_relaxed);

	uint64_t previousMax = stats.maxUs.load(std::memory_order_relaxed);
	while (previousMax < elapsedUs &&
		!stats.maxUs.compare_exchange_weak(previousMax, elapsedUs, std::memory_order_relaxed)) {
	}
}

void RecordWork(Metric metric, uint64_t work) {
	AtomicMetric &stats = frameStats[activeFrame.load(std::memory_order_acquire)][static_cast<size_t>(metric)];
	stats.work.fetch_add(work, std::memory_order_relaxed);
	stats.count.fetch_add(1, std::memory_order_relaxed);
}

FrameSnapshot SnapshotFrame() {
	FrameSnapshot snapshot{};
	AtomicFrame &frame = frameStats[activeFrame.load(std::memory_order_acquire)];
	for (size_t i = 0; i < snapshot.size(); ++i) {
		snapshot[i].totalUs = frame[i].totalUs.load(std::memory_order_relaxed);
		snapshot[i].maxUs = frame[i].maxUs.load(std::memory_order_relaxed);
		snapshot[i].work = frame[i].work.load(std::memory_order_relaxed);
		snapshot[i].count = frame[i].count.load(std::memory_order_relaxed);
	}
	return snapshot;
}

}  // namespace PerfDiagnostics

#endif
