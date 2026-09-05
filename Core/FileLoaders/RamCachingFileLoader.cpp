// Copyright (c) 2015- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>

#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Common/Log.h"
#include "Core/FileLoaders/RamCachingFileLoader.h"

// Takes ownership of backend.
RamCachingFileLoader::RamCachingFileLoader(FileLoader *backend)
	: ProxiedFileLoader(backend) {
	filesize_ = backend->FileSize();
	if (filesize_ > 0) {
		InitCache();
	}
}

RamCachingFileLoader::~RamCachingFileLoader() {
	if (filesize_ > 0) {
		ShutdownCache();
	}
}

bool RamCachingFileLoader::Exists() {
	if (exists_ == -1) {
		exists_ = ProxiedFileLoader::Exists() ? 1 : 0;
	}
	return exists_ == 1;
}

bool RamCachingFileLoader::ExistsFast() {
	if (exists_ == -1) {
		return ProxiedFileLoader::ExistsFast();
	}
	return exists_ == 1;
}

bool RamCachingFileLoader::IsDirectory() {
	if (isDirectory_ == -1) {
		isDirectory_ = ProxiedFileLoader::IsDirectory() ? 1 : 0;
	}
	return isDirectory_ == 1;
}

s64 RamCachingFileLoader::FileSize() {
	return filesize_;
}

size_t RamCachingFileLoader::ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags) {
	if (filesize_ <= 0 || absolutePos < 0 || static_cast<u64>(absolutePos) >= static_cast<u64>(filesize_) || bytes == 0 || data == nullptr) {
		return 0;
	}
	bytes = static_cast<size_t>(std::min<u64>(bytes, static_cast<u64>(filesize_) - static_cast<u64>(absolutePos)));

	size_t readSize = 0;
	if (cache_ == nullptr || (flags & Flags::HINT_UNCACHED) != 0) {
		readSize = backend_->ReadAt(absolutePos, bytes, data, flags);
	} else {
		readSize = ReadFromCache(absolutePos, bytes, data);
		// While in case the cache size is too small for the entire read.
		while (readSize < bytes) {
			SaveIntoCache(absolutePos + readSize, bytes - readSize, flags);
			size_t bytesFromCache = ReadFromCache(absolutePos + readSize, bytes - readSize, (u8 *)data + readSize);
			readSize += bytesFromCache;
			if (bytesFromCache == 0) {
				// We can't read any more.
				break;
			}
		}

		StartReadAhead(absolutePos + readSize);
	}
	return readSize;
}

void RamCachingFileLoader::InitCache() {
	std::lock_guard<std::mutex> guard(blocksMutex_);
	const u64 blockCount64 = (static_cast<u64>(filesize_) + BLOCK_SIZE - 1) >> BLOCK_SHIFT;
	if (blockCount64 > std::numeric_limits<u32>::max() || blockCount64 > std::numeric_limits<size_t>::max() / BLOCK_SIZE) {
		ERROR_LOG(Log::IO, "ISO is too large for Cache full ISO in RAM. Will fall back to regular reads.");
		return;
	}
	const u32 blockCount = static_cast<u32>(blockCount64);
	// Overallocate for the last block.
	cache_ = (u8 *)malloc((size_t)blockCount << BLOCK_SHIFT);
	if (cache_ == nullptr) {
		ERROR_LOG(Log::IO, "Failed to allocate cache for Cache full ISO in RAM! Will fall back to regular reads.");
		return;
	}
	aheadRemaining_ = blockCount;
	blocks_.resize(blockCount);
}

void RamCachingFileLoader::ShutdownCache() {
	Cancel();

	// We can't delete while the thread is running, so have to wait.
	// This should only happen from the menu.
	{
		std::lock_guard<std::mutex> threadGuard(threadMutex_);
		if (aheadThread_.joinable())
			aheadThread_.join();
	}

	std::lock_guard<std::mutex> guard(blocksMutex_);
	_dbg_assert_(!aheadThreadRunning_);
	blocks_.clear();
	if (cache_ != nullptr) {
		free(cache_);
		cache_ = nullptr;
	}
}

void RamCachingFileLoader::Cancel() {
	{
		std::lock_guard<std::mutex> guard(blocksMutex_);
		aheadCancel_ = true;
	}

	ProxiedFileLoader::Cancel();
}

size_t RamCachingFileLoader::ReadFromCache(s64 pos, size_t bytes, void *data) {
	if (filesize_ <= 0 || pos < 0 || static_cast<u64>(pos) >= static_cast<u64>(filesize_) || bytes == 0 || data == nullptr) {
		return 0;
	}
	bytes = static_cast<size_t>(std::min<u64>(bytes, static_cast<u64>(filesize_) - static_cast<u64>(pos)));

	const size_t cacheStartBlock = static_cast<size_t>(pos) >> BLOCK_SHIFT;
	const size_t cacheEndBlock = (static_cast<size_t>(pos) + bytes - 1) >> BLOCK_SHIFT;
	size_t readSize = 0;
	size_t offset = static_cast<size_t>(pos) & (BLOCK_SIZE - 1);
	u8 *p = static_cast<u8 *>(data);

	std::lock_guard<std::mutex> guard(blocksMutex_);
	if (cacheEndBlock >= blocks_.size()) {
		return 0;
	}
	for (size_t i = cacheStartBlock; i <= cacheEndBlock; ++i) {
		if (blocks_[i] == 0) {
			return readSize;
		}

		size_t toRead = std::min(bytes - readSize, (size_t)BLOCK_SIZE - offset);
		const size_t cachePos = (i << BLOCK_SHIFT) + offset;
		memcpy(p + readSize, &cache_[cachePos], toRead);
		readSize += toRead;

		// Don't need an offset after the first read.
		offset = 0;
	}
	return readSize;
}

bool RamCachingFileLoader::SaveIntoCache(s64 pos, size_t bytes, Flags flags) {
	if (filesize_ <= 0 || pos < 0 || static_cast<u64>(pos) >= static_cast<u64>(filesize_) || bytes == 0) {
		return false;
	}
	bytes = static_cast<size_t>(std::min<u64>(bytes, static_cast<u64>(filesize_) - static_cast<u64>(pos)));
	std::lock_guard<std::mutex> fillGuard(fillMutex_);

	size_t cacheStartBlock = static_cast<size_t>(pos) >> BLOCK_SHIFT;
	const size_t cacheEndBlock = (static_cast<size_t>(pos) + bytes - 1) >> BLOCK_SHIFT;
	size_t blocksToRead = 0;
	{
		std::lock_guard<std::mutex> guard(blocksMutex_);
		if (cacheEndBlock >= blocks_.size()) {
			return false;
		}
		while (cacheStartBlock <= cacheEndBlock && blocks_[cacheStartBlock] != 0) {
			++cacheStartBlock;
		}
		for (size_t i = cacheStartBlock; i <= cacheEndBlock && blocks_[i] == 0; ++i) {
			++blocksToRead;
			if (blocksToRead >= MAX_BLOCKS_PER_READ) {
				break;
			}
		}
	}
	if (blocksToRead == 0) {
		return true;
	}

	const s64 cacheFilePos = static_cast<s64>(cacheStartBlock << BLOCK_SHIFT);
	const size_t readRequest = static_cast<size_t>(std::min<u64>(
		blocksToRead << BLOCK_SHIFT, static_cast<u64>(filesize_) - static_cast<u64>(cacheFilePos)));
	const size_t bytesRead = std::min(backend_->ReadAt(cacheFilePos, readRequest, &cache_[cacheFilePos], flags), readRequest);

	// In case there was an error, let's not mark blocks that failed to read as read.
	// A partial block is valid only when it is the real final block of the file.
	u32 blocksActuallyRead = static_cast<u32>(bytesRead >> BLOCK_SHIFT);
	if ((bytesRead & (BLOCK_SIZE - 1)) != 0 && cacheFilePos + static_cast<s64>(bytesRead) == filesize_) {
		++blocksActuallyRead;
	}
	{
		std::lock_guard<std::mutex> guard(blocksMutex_);
		// In case they were simultaneously read.
		u32 blocksRead = 0;
		for (size_t i = 0; i < blocksActuallyRead; ++i) {
			if (blocks_[cacheStartBlock + i] == 0) {
				blocks_[cacheStartBlock + i] = 1;
				++blocksRead;
			}
		}

		aheadRemaining_ -= std::min(aheadRemaining_, blocksRead);
	}
	return blocksActuallyRead != 0;
}

void RamCachingFileLoader::StartReadAhead(s64 pos) {
	std::lock_guard<std::mutex> threadGuard(threadMutex_);
	{
		std::lock_guard<std::mutex> guard(blocksMutex_);
		if (cache_ == nullptr) {
			return;
		}
		aheadPos_ = pos;
		if (aheadThreadRunning_) {
			// Already going.
			return;
		}
		aheadThreadRunning_ = true;
		aheadCancel_ = false;
	}
	if (aheadThread_.joinable())
		aheadThread_.join();
	aheadThread_ = std::thread([this] {
		SetCurrentThreadName("FileLoaderReadAhead");

		AndroidJNIThreadContext jniContext;

		while (true) {
			{
				std::lock_guard<std::mutex> guard(blocksMutex_);
				if (aheadRemaining_ == 0 || aheadCancel_) {
					break;
				}
			}
			// Where should we look?
			const u32 cacheStartPos = NextAheadBlock();
			if (cacheStartPos == 0xFFFFFFFF) {
				// Must be full.
				break;
			}
			if (!SaveIntoCache(static_cast<s64>(cacheStartPos) << BLOCK_SHIFT, BLOCK_SIZE * BLOCK_READAHEAD, Flags::NONE)) {
				break;
			}
		}

		std::lock_guard<std::mutex> guard(blocksMutex_);
		aheadThreadRunning_ = false;
	});
}

u32 RamCachingFileLoader::NextAheadBlock() {
	std::lock_guard<std::mutex> guard(blocksMutex_);

	// If we had an aheadPos_ set, start reading from there and go forward.
	u32 startFrom = (u32)(aheadPos_ >> BLOCK_SHIFT);
	// But next time, start from the beginning again.
	aheadPos_ = 0;

	for (u32 i = startFrom; i < blocks_.size(); ++i) {
		if (blocks_[i] == 0) {
			return i;
		}
	}

	return 0xFFFFFFFF;
}
