#ifndef FILEBLOCKMANAGER_H__
#define FILEBLOCKMANAGER_H__

#include "../Utils/FileAccess.h"
#include <cstdint>
#include <memory>

/*
* Block-level file I/O: random read and write of fixed-size pages.
* Each block is identified by a sequence number; byte offset = seq * block_size.
*/
class FileBlockManager
{
public:
    FileBlockManager() : m_BlockSize(4096), m_BaseOffset(0) {}
    explicit FileBlockManager(size_t block_size, uint64_t base_offset = 0)
        : m_BlockSize(block_size), m_BaseOffset(base_offset) {}
    ~FileBlockManager() = default;

    bool open(const char* filename)
    {
        m_FileAccess = std::make_unique<FileAccess>(filename);
        return m_FileAccess && m_FileAccess->Init();
    }

    bool openWrite(const char* filename)
    {
        m_FileAccess = std::make_unique<FileAccess>(filename);
        return m_FileAccess && m_FileAccess->InitWrite();
    }

    void close()
    {
        m_FileAccess.reset();
    }

    bool read(uint32_t block_seq, void* buffer)
    {
        if (!m_FileAccess || !buffer) {
            return false;
        }
        return m_FileAccess->ReadBlock(block_seq, buffer, m_BlockSize, m_BaseOffset);
    }

    bool write(uint32_t block_seq, const void* buffer)
    {
        if (!m_FileAccess || !buffer) {
            return false;
        }
        return m_FileAccess->WriteBlock(block_seq, buffer, m_BlockSize);
    }

    size_t getBlockSize() const { return m_BlockSize; }
    void   setBlockSize(size_t block_size) { m_BlockSize = block_size; }
    uint64_t getBaseOffset() const { return m_BaseOffset; }
    void     setBaseOffset(uint64_t base_offset) { m_BaseOffset = base_offset; }

private:
    std::unique_ptr<FileAccess> m_FileAccess;
    size_t   m_BlockSize;
    uint64_t m_BaseOffset;
};

#endif
