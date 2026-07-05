/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include "FileAccess.h"

#include <algorithm>
#include <limits>
#include <mutex>

#if defined(__linux__)
static std::atomic<uint64_t> g_IoUringReads{0};
static std::atomic<uint64_t> g_PreadFallbackReads{0};
static std::atomic<uint64_t> g_IoUringSetupOk{0};
static std::atomic<uint64_t> g_IoUringSetupFailed{0};
#endif

FileAccess::IoStats FileAccess::GetIoStats()
{
#if defined(__linux__)
    return IoStats{
        g_IoUringReads.load(std::memory_order_relaxed),
        g_PreadFallbackReads.load(std::memory_order_relaxed),
        g_IoUringSetupOk.load(std::memory_order_relaxed),
        g_IoUringSetupFailed.load(std::memory_order_relaxed),
    };
#else
    return {};
#endif
}

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

namespace {

static int IoUringSetup(unsigned entries, io_uring_params* params)
{
    return static_cast<int>(syscall(__NR_io_uring_setup, entries, params));
}

static int IoUringEnter(int ringFd, unsigned toSubmit, unsigned minComplete, unsigned flags)
{
    return static_cast<int>(syscall(__NR_io_uring_enter, ringFd, toSubmit, minComplete, flags, nullptr, 0));
}

}

struct FileAccessIoUring {
    int RingFd = -1;
    io_uring_params Params{};

    uint32_t* SqHead = nullptr;
    uint32_t* SqTail = nullptr;
    uint32_t* SqRingMask = nullptr;
    uint32_t* SqRingEntries = nullptr;
    uint32_t* SqFlags = nullptr;
    uint32_t* SqDropped = nullptr;
    uint32_t* SqArray = nullptr;
    io_uring_sqe* Sqes = nullptr;

    uint32_t* CqHead = nullptr;
    uint32_t* CqTail = nullptr;
    uint32_t* CqRingMask = nullptr;
    uint32_t* CqRingEntries = nullptr;
    uint32_t* CqOverflow = nullptr;
    io_uring_cqe* Cqes = nullptr;

    void* SqRingPtr = MAP_FAILED;
    void* CqRingPtr = MAP_FAILED;
    void* SqesPtr = MAP_FAILED;
    size_t SqRingSize = 0;
    size_t CqRingSize = 0;
    size_t SqesSize = 0;
    std::mutex Mutex;

    bool Init(unsigned entries = 64)
    {
        std::memset(&Params, 0, sizeof(Params));
        RingFd = IoUringSetup(entries, &Params);
        if (RingFd < 0)
            return false;

        SqRingSize = Params.sq_off.array + Params.sq_entries * sizeof(uint32_t);
        CqRingSize = Params.cq_off.cqes + Params.cq_entries * sizeof(io_uring_cqe);
        if (Params.features & IORING_FEAT_SINGLE_MMAP) {
            SqRingSize = std::max(SqRingSize, CqRingSize);
            CqRingSize = SqRingSize;
        }

        SqRingPtr = mmap(nullptr, SqRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, RingFd, IORING_OFF_SQ_RING);
        if (SqRingPtr == MAP_FAILED) {
            Close();
            return false;
        }

        if (Params.features & IORING_FEAT_SINGLE_MMAP) {
            CqRingPtr = SqRingPtr;
        } else {
            CqRingPtr = mmap(nullptr, CqRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, RingFd, IORING_OFF_CQ_RING);
            if (CqRingPtr == MAP_FAILED) {
                Close();
                return false;
            }
        }

        SqesSize = Params.sq_entries * sizeof(io_uring_sqe);
        SqesPtr = mmap(nullptr, SqesSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, RingFd, IORING_OFF_SQES);
        if (SqesPtr == MAP_FAILED) {
            Close();
            return false;
        }

        auto* sq = static_cast<uint8_t*>(SqRingPtr);
        SqHead = reinterpret_cast<uint32_t*>(sq + Params.sq_off.head);
        SqTail = reinterpret_cast<uint32_t*>(sq + Params.sq_off.tail);
        SqRingMask = reinterpret_cast<uint32_t*>(sq + Params.sq_off.ring_mask);
        SqRingEntries = reinterpret_cast<uint32_t*>(sq + Params.sq_off.ring_entries);
        SqFlags = reinterpret_cast<uint32_t*>(sq + Params.sq_off.flags);
        SqDropped = reinterpret_cast<uint32_t*>(sq + Params.sq_off.dropped);
        SqArray = reinterpret_cast<uint32_t*>(sq + Params.sq_off.array);
        Sqes = static_cast<io_uring_sqe*>(SqesPtr);

        auto* cq = static_cast<uint8_t*>(CqRingPtr);
        CqHead = reinterpret_cast<uint32_t*>(cq + Params.cq_off.head);
        CqTail = reinterpret_cast<uint32_t*>(cq + Params.cq_off.tail);
        CqRingMask = reinterpret_cast<uint32_t*>(cq + Params.cq_off.ring_mask);
        CqRingEntries = reinterpret_cast<uint32_t*>(cq + Params.cq_off.ring_entries);
        CqOverflow = reinterpret_cast<uint32_t*>(cq + Params.cq_off.overflow);
        Cqes = reinterpret_cast<io_uring_cqe*>(cq + Params.cq_off.cqes);
        return true;
    }

    bool Read(int fd, void* buffer, size_t bytes, uint64_t offset)
    {
        std::lock_guard<std::mutex> guard(Mutex);
        if (RingFd < 0 || !SqHead || !SqTail || !CqHead || !CqTail)
            return false;

        const uint32_t head = __atomic_load_n(SqHead, __ATOMIC_ACQUIRE);
        const uint32_t tail = __atomic_load_n(SqTail, __ATOMIC_RELAXED);
        if (tail - head >= *SqRingEntries)
            return false;

        const uint32_t index = tail & *SqRingMask;
        io_uring_sqe* sqe = &Sqes[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(buffer);
        sqe->len = static_cast<uint32_t>(bytes);
        sqe->off = offset;
        sqe->user_data = 1;
        SqArray[index] = index;
        __atomic_store_n(SqTail, tail + 1, __ATOMIC_RELEASE);

        if (IoUringEnter(RingFd, 1, 1, IORING_ENTER_GETEVENTS) < 0)
            return false;

        while (true) {
            const uint32_t cqHead = __atomic_load_n(CqHead, __ATOMIC_ACQUIRE);
            const uint32_t cqTail = __atomic_load_n(CqTail, __ATOMIC_ACQUIRE);
            if (cqHead != cqTail) {
                io_uring_cqe* cqe = &Cqes[cqHead & *CqRingMask];
                const int result = cqe->res;
                __atomic_store_n(CqHead, cqHead + 1, __ATOMIC_RELEASE);
                return result == static_cast<int>(bytes);
            }
            if (IoUringEnter(RingFd, 0, 1, IORING_ENTER_GETEVENTS) < 0)
                return false;
        }
    }

    void Close()
    {
        if (SqesPtr != MAP_FAILED) {
            munmap(SqesPtr, SqesSize);
            SqesPtr = MAP_FAILED;
        }
        if (CqRingPtr != MAP_FAILED && CqRingPtr != SqRingPtr) {
            munmap(CqRingPtr, CqRingSize);
            CqRingPtr = MAP_FAILED;
        }
        if (SqRingPtr != MAP_FAILED) {
            munmap(SqRingPtr, SqRingSize);
            SqRingPtr = MAP_FAILED;
        }
        if (RingFd >= 0) {
            close(RingFd);
            RingFd = -1;
        }
    }

    ~FileAccessIoUring()
    {
        Close();
    }
};
#endif

FileAccess::FileAccess(const char * fileName)
: m_FileName(const_cast<char*>(fileName))
{
#ifdef _WIN32
    m_FileHandle = INVALID_HANDLE_VALUE;
#else
    m_FileHandle = -1;
    m_IoUring = nullptr;
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
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            NULL);
    return m_FileHandle != INVALID_HANDLE_VALUE;
#else
    m_FileHandle = open(m_FileName, O_RDONLY);
    if (m_FileHandle == -1)
        return false;
    m_IoUring = new FileAccessIoUring();
    if (!m_IoUring->Init()) {
        ++g_IoUringSetupFailed;
        delete m_IoUring;
        m_IoUring = nullptr;
    } else {
        ++g_IoUringSetupOk;
    }
    return true;
#endif
}

bool FileAccess::InitWrite(bool truncate)
{
#ifdef _WIN32
    m_FileHandle = CreateFileA(m_FileName,
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            NULL,
                            truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
    return m_FileHandle != INVALID_HANDLE_VALUE;
#else
    m_FileHandle = open(m_FileName, O_RDWR | O_CREAT | (truncate ? O_TRUNC : 0), 0644);
    return m_FileHandle != -1;
#endif
}

int FileAccess::GetData(void * buffer, int numBytes)
{
#ifdef _WIN32
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    const uint64_t position = m_Position.load(std::memory_order_relaxed);
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(position & 0xffffffffu);
    overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);

    DWORD bytesRead = 0;
    if (ReadFile(m_FileHandle, buffer, numBytes, &bytesRead, &overlapped)) {
        m_Position.store(position + bytesRead, std::memory_order_relaxed);
        return static_cast<int>(bytesRead);
    }
    const DWORD error = GetLastError();
    if (error == ERROR_HANDLE_EOF) {
        return 0;
    }
    if (error == ERROR_IO_PENDING) {
        if (GetOverlappedResult(m_FileHandle, &overlapped, &bytesRead, TRUE)) {
            m_Position.store(position + bytesRead, std::memory_order_relaxed);
            return static_cast<int>(bytesRead);
        }
        if (GetLastError() == ERROR_HANDLE_EOF) {
            return 0;
        }
    }
    return -1;
#else
    if (m_FileHandle == -1) {
        return -1;
    }

    const uint64_t position = m_Position.load(std::memory_order_relaxed);
    ssize_t bytesRead = pread(m_FileHandle, buffer, numBytes, static_cast<off_t>(position));
    if (bytesRead > 0)
        m_Position.store(position + static_cast<uint64_t>(bytesRead), std::memory_order_relaxed);
    return static_cast<int>(bytesRead);
#endif
}

bool FileAccess::PutData(const void * buffer, uint64_t numBytes)
{
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    uint64_t writtenTotal = 0;
    while (numBytes > 0) {
        const int chunk = static_cast<int>(std::min<uint64_t>(numBytes, static_cast<uint64_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
        if (m_FileHandle == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(m_FileHandle, cursor, static_cast<DWORD>(chunk), &bytesWritten, NULL)
            || bytesWritten != static_cast<DWORD>(chunk)) {
            return false;
        }
#else
        if (m_FileHandle == -1) {
            return false;
        }

        ssize_t bytesWritten = write(m_FileHandle, cursor, chunk);
        if (bytesWritten != static_cast<ssize_t>(chunk)) {
            return false;
        }
#endif
        cursor += chunk;
        numBytes -= static_cast<uint64_t>(chunk);
        writtenTotal += static_cast<uint64_t>(chunk);
    }
    m_Position.fetch_add(writtenTotal, std::memory_order_relaxed);
    return true;
}

bool FileAccess::ReadBlock(uint32_t block_seq, void* buffer, size_t block_size,
                           uint64_t base_byte_offset)
{
#ifdef _WIN32
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint64_t position = base_byte_offset + static_cast<uint64_t>(block_seq) * block_size;

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(position & 0xffffffffu);
    overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);

    DWORD bytesRead = 0;
    if (ReadFile(m_FileHandle, buffer, static_cast<DWORD>(block_size), &bytesRead, &overlapped)) {
        return bytesRead == block_size;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        if (GetOverlappedResult(m_FileHandle, &overlapped, &bytesRead, TRUE))
            return bytesRead == block_size;
    }

    return false;
#else
    if (m_FileHandle == -1) {
        return false;
    }

    off_t position = static_cast<off_t>(base_byte_offset)
                   + static_cast<off_t>(block_seq) * static_cast<off_t>(block_size);

    if (m_IoUring && m_IoUring->Read(m_FileHandle, buffer, block_size, static_cast<uint64_t>(position))) {
        ++g_IoUringReads;
        return true;
    }

    ++g_PreadFallbackReads;
    ssize_t bytesRead = pread(m_FileHandle, buffer, block_size, position);
    return bytesRead == static_cast<ssize_t>(block_size);
#endif
}

bool FileAccess::WriteBlock(uint32_t block_seq, const void* buffer, size_t block_size)
{
#ifdef _WIN32
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint64_t position = static_cast<uint64_t>(block_seq) * block_size;

    LARGE_INTEGER liPosition;
    liPosition.QuadPart = position;

    if (!SetFilePointerEx(m_FileHandle, liPosition, NULL, FILE_BEGIN)) {
        return false;
    }

    DWORD written = 0;
    if (WriteFile(m_FileHandle, buffer, static_cast<DWORD>(block_size), &written, NULL)) {
        return written == static_cast<DWORD>(block_size);
    }

    return false;
#else
    if (m_FileHandle == -1) {
        return false;
    }

    off_t position = static_cast<off_t>(block_seq) * block_size;

    if (lseek(m_FileHandle, position, SEEK_SET) == -1) {
        return false;
    }

    ssize_t written = write(m_FileHandle, buffer, block_size);
    return written == static_cast<ssize_t>(block_size);
#endif
}

bool FileAccess::SetPosition(uint64_t position)
{
    m_Position.store(position, std::memory_order_relaxed);
#ifdef _WIN32
    if (m_FileHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER liPosition;
    liPosition.QuadPart = static_cast<LONGLONG>(position);
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
    delete m_IoUring;
    m_IoUring = nullptr;
#endif
}
