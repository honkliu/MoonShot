
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

class FileAccess {
	public:
		FileAccess() = default;
		FileAccess(const char * fileName);
		~FileAccess();
		bool Init();
		int GetData(void * buffer, int numBytes);
		
		// Additional methods for block-based access
		bool ReadBlock(uint32_t block_seq, void* buffer, size_t block_size);
		bool SetPosition(uint64_t position);
		
	private:
#ifdef _WIN32
		HANDLE m_FileHandle = INVALID_HANDLE_VALUE;
#else
		int m_FileHandle = -1;
#endif
		char * m_FileName = nullptr;	
};
 
#endif

