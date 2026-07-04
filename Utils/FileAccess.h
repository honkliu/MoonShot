
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef FILEACCESS_H__
#define FILEACCESS_H__

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#endif

#include <stdio.h>
#include <stdint.h>
#include <atomic>

#if defined(__linux__)
struct FileAccessIoUring;
#endif

class FileAccess {
	public:
		struct IoStats {
			uint64_t IoUringReads = 0;
			uint64_t PreadFallbackReads = 0;
			uint64_t IoUringSetupOk = 0;
			uint64_t IoUringSetupFailed = 0;
		};

		FileAccess() = default;
		FileAccess(const char * fileName);
		~FileAccess();
		static IoStats GetIoStats();
		bool Init();
		bool InitWrite(bool truncate = true);
		int GetData(void * buffer, int numBytes);
		bool PutData(const void * buffer, uint64_t numBytes);

		bool ReadBlock(uint32_t block_seq, void* buffer, size_t block_size,
		               uint64_t base_byte_offset = 0);
		bool WriteBlock(uint32_t block_seq, const void* buffer, size_t block_size);
		bool SetPosition(uint64_t position);

	private:
#ifdef _WIN32
		HANDLE m_FileHandle = INVALID_HANDLE_VALUE;
#else
		int m_FileHandle = -1;
		FileAccessIoUring* m_IoUring = nullptr;
#endif
		std::atomic<uint64_t> m_Position{0};
		char * m_FileName = nullptr;
};

#endif
