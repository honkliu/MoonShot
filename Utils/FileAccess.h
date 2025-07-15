
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef FILEACCESS_H__
#define FILEACCESS_H__

#include <windows.h>

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
		HANDLE m_FileHandle = INVALID_HANDLE_VALUE;
		char * m_FileName = nullptr;	
};
 
#endif

