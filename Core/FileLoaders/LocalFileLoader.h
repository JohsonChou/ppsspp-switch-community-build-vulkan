// Copyright (c) 2012- PPSSPP Project.

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

#pragma once

#include <array>
#include <memory>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
#include "Common/StringUtils.h"
#include "Core/Loaders.h"

#if defined(_WIN32) && !defined(HAVE_LIBRETRO_VFS)
typedef void *HANDLE;
#endif

class LocalFileLoader : public FileLoader {
public:
	LocalFileLoader(const Path &filename);
	~LocalFileLoader();

	bool Exists() override;
	bool IsDirectory() override;
	s64 FileSize() override;
	Path GetPath() const override {
		return filename_;
	}
	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags = Flags::NONE) override;

private:
#if defined(__SWITCH__) && !defined(HAVE_LIBRETRO_VFS)
	struct ReadAheadWindow {
		std::unique_ptr<u8[]> data;
		s64 start = -1;
		size_t validBytes = 0;
		u64 generation = 0;
	};

	static constexpr size_t READ_AHEAD_WINDOW_SIZE = 256 * 1024;
	static constexpr size_t READ_AHEAD_WINDOW_COUNT = 16;

	size_t ReadAtSwitch(s64 absolutePos, size_t bytes, void *data, Flags flags);
	size_t ReadAtSwitchRaw(s64 absolutePos, size_t bytes, void *data);
	ReadAheadWindow *FindReadAheadWindow(s64 absolutePos);
	ReadAheadWindow *FillReadAheadWindow(s64 absolutePos);
#endif

#ifdef HAVE_LIBRETRO_VFS
	FILE *file_ = nullptr;
#elif !defined(_WIN32)
	void DetectSizeFd();
	int fd_ = -1;
#else
	HANDLE handle_ = 0;
#endif
	u64 filesize_ = 0;
	Path filename_;
	std::mutex readLock_;
	bool isOpenedByFd_ = false;
#if defined(__SWITCH__) && !defined(HAVE_LIBRETRO_VFS)
	// Coalesce bursty PSP sector reads without adding another worker thread.
	std::array<ReadAheadWindow, READ_AHEAD_WINDOW_COUNT> readAheadWindows_;
	u64 readAheadGeneration_ = 0;
	bool readAheadEnabled_ = false;
#endif
};
