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


#include "ppsspp_config.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#include "Common/Log.h"
#include "Common/PerfDiagnostics.h"
#include "Common/TimeUtil.h"
#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Core/Util/DarwinFileSystemServices.h"
#include "Core/FileLoaders/LocalFileLoader.h"

#if PPSSPP_PLATFORM(ANDROID)
#include "android/jni/app-android.h"
#endif

#ifdef _WIN32
#include "Common/CommonWindows.h"
#if PPSSPP_PLATFORM(UWP)
#include <fileapifromapp.h>
#endif
#else
#include <fcntl.h>
#endif

#ifdef HAVE_LIBRETRO_VFS
#include <streams/file_stream.h>
#endif

#if !defined(_WIN32) && !defined(HAVE_LIBRETRO_VFS)

void LocalFileLoader::DetectSizeFd() {
#if PPSSPP_PLATFORM(ANDROID) || (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64)
	off64_t off = lseek64(fd_, 0, SEEK_END);
	filesize_ = off;
	lseek64(fd_, 0, SEEK_SET);
#else
	off_t off = lseek(fd_, 0, SEEK_END);
	filesize_ = off;
	lseek(fd_, 0, SEEK_SET);
#endif
}
#endif

LocalFileLoader::LocalFileLoader(const Path &filename)
	: filename_(filename) {
	if (filename.empty()) {
		ERROR_LOG(Log::FileSystem, "LocalFileLoader can't load empty filenames");
		return;
	}

#if PPSSPP_PLATFORM(ANDROID) && !defined(HAVE_LIBRETRO_VFS)
	if (filename.Type() == PathType::CONTENT_URI) {
		int fd = Android_OpenContentUriFd(filename.ToString(), Android_OpenContentUriMode::READ);
		VERBOSE_LOG(Log::System, "LocalFileLoader Fd %d for content URI: '%s'", fd, filename.c_str());
		if (fd < 0) {
			ERROR_LOG(Log::FileSystem, "LocalFileLoader failed to open content URI: '%s'", filename.c_str());
			return;
		}
		fd_ = fd;
		isOpenedByFd_ = true;
		DetectSizeFd();
		return;
	}
#endif

#if defined(HAVE_LIBRETRO_VFS)
	file_ = File::OpenCFile(filename, "rb");
	if (!file_) {
		ERROR_LOG(Log::FileSystem, "LocalFileLoader: failed to open file: '%s'", filename.c_str());
		return;
	}
	filesize_ = File::GetFileSize(file_);
#elif PPSSPP_PLATFORM(IOS)
	if (!File::Exists(filename)) {
		// Try to "unlock" the path before the file loader hits it
		Path newFilename = DarwinFileSystemServices::reauthorizeBookmarkByPath(filename);
		if (!newFilename.empty()) {
			filename_ = newFilename;
		}
	}
	fd_ = open(filename_.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd_ == -1) {
		ERROR_LOG(Log::FileSystem, "LocalFileLoader: failed to open file: '%s'", filename_.c_str());
		return;
	}
	DetectSizeFd();
#elif !defined(_WIN32)
	fd_ = open(filename.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd_ == -1) {
		ERROR_LOG(Log::FileSystem, "LocalFileLoader: failed to open file: '%s'", filename.c_str());
		return;
	}

	DetectSizeFd();
#else // _WIN32
	const DWORD access = GENERIC_READ, share = FILE_SHARE_READ, mode = OPEN_EXISTING, flags = FILE_ATTRIBUTE_NORMAL;
#if PPSSPP_PLATFORM(UWP)
	handle_ = CreateFile2FromAppW(filename.ToWString().c_str(), access, share, mode, nullptr);
#else
	handle_ = CreateFile(filename.ToWString().c_str(), access, share, nullptr, mode, flags, nullptr);
#endif
	if (handle_ == INVALID_HANDLE_VALUE) {
		return;
	}
	LARGE_INTEGER end_offset;
	const LARGE_INTEGER zero{};
	if (SetFilePointerEx(handle_, zero, &end_offset, FILE_END) == 0) {
		// Couldn't seek in the file. Close it and give up? This should never happen.
		CloseHandle(handle_);
		handle_ = INVALID_HANDLE_VALUE;
		return;
	}
	filesize_ = end_offset.QuadPart;
	SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN);
#endif // _WIN32

#if PPSSPP_PLATFORM(SWITCH) && !defined(HAVE_LIBRETRO_VFS)
	const std::string extension = filename_.GetFileExtension();
	readAheadEnabled_ = extension == ".iso" || extension == ".cso" || extension == ".chd";
#endif
}

LocalFileLoader::~LocalFileLoader() {
#if defined(HAVE_LIBRETRO_VFS)
	if (file_ != nullptr) {
		fclose(file_);
	}
#elif PPSSPP_PLATFORM(IOS)
	close(fd_);
	DarwinFileSystemServices::stopAccessingPath(filename_);
#elif !defined(_WIN32)
	if (fd_ != -1) {
		close(fd_);
	}
#else
	if (handle_ != INVALID_HANDLE_VALUE) {
		CloseHandle(handle_);
	}
#endif
}

bool LocalFileLoader::Exists() {
	// If we opened it for reading, it must exist.  Done.
#if defined(HAVE_LIBRETRO_VFS)
	return file_ != nullptr;
#elif !defined(_WIN32)
	if (isOpenedByFd_) {
		// As an optimization, if we already tried and failed, quickly return.
		// This is used because Android Content URIs are so slow.
		return fd_ != -1;
	}
	if (fd_ != -1)
		return true;
#else
	if (handle_ != INVALID_HANDLE_VALUE)
		return true;
#endif

	return File::Exists(filename_);
}

bool LocalFileLoader::IsDirectory() {
	File::FileInfo info;
	if (File::GetFileInfo(filename_, &info)) {
		return info.exists && info.isDirectory;
	}
	return false;
}

s64 LocalFileLoader::FileSize() {
	return filesize_;
}

size_t LocalFileLoader::ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags) {
	if (bytes == 0)
		return 0;

	if (filesize_ == 0) {
		ERROR_LOG(Log::FileSystem, "ReadAt from 0-sized file: %s", filename_.c_str());
		return 0;
	}

#if defined(HAVE_LIBRETRO_VFS)
	std::lock_guard<std::mutex> guard(readLock_);
	File::Fseek(file_, absolutePos, SEEK_SET);
	return fread(data, bytes, count, file_);
#elif PPSSPP_PLATFORM(SWITCH)
	// Toolchain has no fancy IO API.  We must lock.
	if (absolutePos < 0 || count > std::numeric_limits<size_t>::max() / bytes) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(readLock_);
	return ReadAtSwitch(absolutePos, bytes * count, data, flags) / bytes;
#elif PPSSPP_PLATFORM(ANDROID)
	// pread64 doesn't appear to actually be 64-bit safe, though such ISOs are uncommon.  See #10862.
	if (absolutePos <= 0x7FFFFFFF) {
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64
		return pread64(fd_, data, bytes * count, absolutePos) / bytes;
#else
		return pread(fd_, data, bytes * count, absolutePos) / bytes;
#endif
	} else {
		// Since pread64 doesn't change the file offset, it should be safe to avoid the lock in the common case.
		std::lock_guard<std::mutex> guard(readLock_);
		lseek64(fd_, absolutePos, SEEK_SET);
		return read(fd_, data, bytes * count) / bytes;
	}
#elif !defined(_WIN32)
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64
	return pread64(fd_, data, bytes * count, absolutePos) / bytes;
#else
	return pread(fd_, data, bytes * count, absolutePos) / bytes;
#endif
#else
	DWORD read = -1;
	OVERLAPPED offset = { 0 };
	offset.Offset = (DWORD)(absolutePos & 0xffffffff);
	offset.OffsetHigh = (DWORD)((absolutePos & 0xffffffff00000000) >> 32);
	auto result = ReadFile(handle_, data, (DWORD)(bytes * count), &read, &offset);
	return result == TRUE ? (size_t)read / bytes : -1;
#endif
}

#if PPSSPP_PLATFORM(SWITCH) && !defined(HAVE_LIBRETRO_VFS)
size_t LocalFileLoader::ReadAtSwitchRaw(s64 absolutePos, size_t bytes, void *data) {
	if (absolutePos < 0 || static_cast<u64>(absolutePos) >= filesize_ || bytes == 0) {
		return 0;
	}

	const size_t available = static_cast<size_t>(std::min<u64>(filesize_ - static_cast<u64>(absolutePos), bytes));
#if defined(SWITCH_PERF_DIAGNOSTICS)
	const double start = time_now_d();
#endif
	if (lseek(fd_, absolutePos, SEEK_SET) < 0) {
#if defined(SWITCH_PERF_DIAGNOSTICS)
		PerfDiagnostics::Record(PerfDiagnostics::Metric::HOST_READ, time_now_d() - start);
#endif
		return 0;
	}

	size_t totalRead = 0;
	while (totalRead < available) {
		const ssize_t result = read(fd_, static_cast<u8 *>(data) + totalRead, available - totalRead);
		if (result > 0) {
			totalRead += static_cast<size_t>(result);
		} else if (result < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
#if defined(SWITCH_PERF_DIAGNOSTICS)
	PerfDiagnostics::Record(PerfDiagnostics::Metric::HOST_READ, time_now_d() - start, totalRead);
#endif
	return totalRead;
}

LocalFileLoader::ReadAheadWindow *LocalFileLoader::FindReadAheadWindow(s64 absolutePos) {
	for (ReadAheadWindow &window : readAheadWindows_) {
		if (window.start >= 0 && absolutePos >= window.start &&
			static_cast<u64>(absolutePos - window.start) < window.validBytes) {
			window.generation = ++readAheadGeneration_;
			return &window;
		}
	}
	return nullptr;
}

LocalFileLoader::ReadAheadWindow *LocalFileLoader::FillReadAheadWindow(s64 absolutePos) {
	const s64 windowStart = absolutePos / static_cast<s64>(READ_AHEAD_WINDOW_SIZE) * static_cast<s64>(READ_AHEAD_WINDOW_SIZE);
	ReadAheadWindow *target = nullptr;
	for (ReadAheadWindow &window : readAheadWindows_) {
		if (window.start < 0) {
			target = &window;
			break;
		}
		if (!target || window.generation < target->generation) {
			target = &window;
		}
	}

	if (!target->data) {
		target->data = std::make_unique<u8[]>(READ_AHEAD_WINDOW_SIZE);
	}
	target->start = -1;
	target->validBytes = ReadAtSwitchRaw(windowStart, READ_AHEAD_WINDOW_SIZE, target->data.get());
	if (target->validBytes == 0) {
		return nullptr;
	}
	target->start = windowStart;
	target->generation = ++readAheadGeneration_;
	return target;
}

size_t LocalFileLoader::ReadAtSwitch(s64 absolutePos, size_t bytes, void *data, Flags flags) {
	if (absolutePos < 0 || static_cast<u64>(absolutePos) >= filesize_ || bytes == 0) {
		return 0;
	}

	const size_t requested = static_cast<size_t>(std::min<u64>(filesize_ - static_cast<u64>(absolutePos), bytes));
	if (!readAheadEnabled_ || flags == Flags::HINT_UNCACHED || requested > READ_AHEAD_WINDOW_SIZE) {
		return ReadAtSwitchRaw(absolutePos, requested, data);
	}

	size_t totalRead = 0;
	while (totalRead < requested) {
		const s64 position = absolutePos + static_cast<s64>(totalRead);
		ReadAheadWindow *window = FindReadAheadWindow(position);
		const bool cacheHit = window != nullptr;
		if (!window) {
			window = FillReadAheadWindow(position);
		}
		if (!window) {
			break;
		}

		const size_t windowOffset = static_cast<size_t>(position - window->start);
		const size_t copyBytes = std::min(requested - totalRead, window->validBytes - windowOffset);
		if (copyBytes == 0) {
			break;
		}
		memcpy(static_cast<u8 *>(data) + totalRead, window->data.get() + windowOffset, copyBytes);
#if defined(SWITCH_PERF_DIAGNOSTICS)
		if (cacheHit) {
			PerfDiagnostics::RecordWork(PerfDiagnostics::Metric::READ_AHEAD_HIT, copyBytes);
		}
#endif
		totalRead += copyBytes;
	}
	return totalRead;
}
#endif
