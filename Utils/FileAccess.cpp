/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include "FileAccess.h"

FileAccess::FileAccess(const char * fileName)
: m_FileName(const_cast<char*>(fileName))
{
#ifdef _WIN32
    m_FileHandle = INVALID_HANDLE_VALUE;
#else
    m_FileHandle = -1;
#endif
}

bool FileAccess::Init()
{
#ifdef _WIN32
    m_FileHandle = CreateFileA(m_FileName, 
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            NULL,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
    return m_FileHandle != INVALID_HANDLE_VALUE;
#else
    m_FileHandle = open(m_FileName, O_RDONLY);
    return m_FileHandle != -1;
#endif
}

int FileAccess::GetData(void * buffer, int numBytes)
{
#ifdef _WIN32
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    
    DWORD bytesRead = 0;
    if (ReadFile(m_FileHandle, buffer, numBytes, &bytesRead, NULL)) {
        return static_cast<int>(bytesRead);
    }
    return -1;
#else
    if (m_FileHandle == -1) {
        return -1;
    }
    
    ssize_t bytesRead = read(m_FileHandle, buffer, numBytes);
    return static_cast<int>(bytesRead);
#endif
}

bool FileAccess::ReadBlock(uint32_t block_seq, void* buffer, size_t block_size)
{
#ifdef _WIN32
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
#else
    if (m_FileHandle == -1) {
        return false;
    }
    
    // Calculate file position for this block
    off_t position = static_cast<off_t>(block_seq) * block_size;
    
    // Seek to the block position
    if (lseek(m_FileHandle, position, SEEK_SET) == -1) {
        return false;
    }
    
    // Read the block
    ssize_t bytesRead = read(m_FileHandle, buffer, block_size);
    return bytesRead == static_cast<ssize_t>(block_size);
#endif
}

bool FileAccess::SetPosition(uint64_t position)
{
#ifdef _WIN32
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    LARGE_INTEGER liPosition;
    liPosition.QuadPart = position;
    
    return SetFilePointerEx(m_FileHandle, liPosition, NULL, FILE_BEGIN) != 0;
#else
    if (m_FileHandle == -1) {
        return false;
    }
    
    return lseek(m_FileHandle, static_cast<off_t>(position), SEEK_SET) != -1;
#endif
}

FileAccess::~FileAccess()
{
#ifdef _WIN32
    if (m_FileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_FileHandle);
    }
#else
    if (m_FileHandle != -1) {
        close(m_FileHandle);
    }
#endif
}


