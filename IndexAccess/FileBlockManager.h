#ifndef FILEBLOCKMANAGER_H__
#define FILEBLOCKMANAGER_H__

#include "../Utils/FileAccess.h"
#include <cstdint>
#include <memory>

/*
* FileBlockManager - Manages block-based file I/O operations
* Uses the FileAccess class for actual file operations
* 
* This class provides an interface for reading/writing fixed-size blocks
* from/to index files, supporting efficient random access patterns
* required for search index operations.
*/

class FileBlockManager
{
public:
    FileBlockManager() : m_BlockSize(4096) {}
    explicit FileBlockManager(size_t block_size) : m_BlockSize(block_size) {}
    ~FileBlockManager() = default;
    
    // Initialize with a file
    bool open(const char* filename) {
        m_FileAccess = std::make_unique<FileAccess>(filename);
        return m_FileAccess && m_FileAccess->Init();
    }
    
    void close() {
        m_FileAccess.reset();
    }
    
    // Read a block by sequence number
    bool read(uint32_t block_seq, void* buffer) {
        if (!m_FileAccess) {
            return false;
        }
        return m_FileAccess->ReadBlock(block_seq, buffer, m_BlockSize);
    }
    
    // Write a block by sequence number  
    bool write(uint32_t block_seq, const void* buffer) {
        // TODO: Implement write functionality in FileAccess
        // For now, return false to indicate not implemented
        return false;
    }
    
    // Get block size
    size_t getBlockSize() const { return m_BlockSize; }
    
    // Set block size (should be done before opening file)
    void setBlockSize(size_t block_size) { m_BlockSize = block_size; }

private:
    std::unique_ptr<FileAccess> m_FileAccess;
    size_t m_BlockSize;
};

#endif