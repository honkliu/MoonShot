/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include "FileAccess.h"
#include <windows.h>

FileAccess::FileAccess(const char * fileName)
: m_FileName(const_cast<char*>(fileName)), m_FileHandle(INVALID_HANDLE_VALUE)
{

}

bool FileAccess::Init()
{
    m_FileHandle = CreateFileA(m_FileName, 
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            NULL,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
    return m_FileHandle != INVALID_HANDLE_VALUE;
}

int FileAccess::GetData(void * buffer, int numBytes)
{
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    
    DWORD bytesRead = 0;
    if (ReadFile(m_FileHandle, buffer, numBytes, &bytesRead, NULL)) {
        return static_cast<int>(bytesRead);
    }
    return -1;
}

bool FileAccess::ReadBlock(uint32_t block_seq, void* buffer, size_t block_size)
{
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    // Calculate file position for this block
    uint64_t position = static_cast<uint64_t>(block_seq) * block_size;
    
    // Set file pointer to the block position
    LARGE_INTEGER liPosition;
    liPosition.QuadPart = position;
    
    if (!SetFilePointerEx(m_FileHandle, liPosition, NULL, FILE_BEGIN)) {
        return false;
    }
    
    // Read the block
    DWORD bytesRead = 0;
    if (ReadFile(m_FileHandle, buffer, static_cast<DWORD>(block_size), &bytesRead, NULL)) {
        return bytesRead == block_size;
    }
    
    return false;
}

bool FileAccess::SetPosition(uint64_t position)
{
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    LARGE_INTEGER liPosition;
    liPosition.QuadPart = position;
    
    return SetFilePointerEx(m_FileHandle, liPosition, NULL, FILE_BEGIN) != 0;
}

FileAccess::~FileAccess()
{
    if (m_FileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_FileHandle);
    }
}


