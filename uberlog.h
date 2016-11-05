#pragma once

#include <atomic>
#include <string>
#include <time.h>
#include "tsf.h"

namespace uberlog {

namespace internal {

// The size of the write buffer in the logger slave. This exists so that the
// logger slave doesn't issue a write() call for every log message.
// If we make it too large, we waste memory bandwidth and pollute the cache.
// If we make it too small, we issue too many kernel calls.
// On Windows, it doesn't seem to make much of a difference above 1024.
static const size_t LoggerSlaveWriteBufferSize = 1024;

#ifdef _WIN32
static const char         PATH_SLASH = '\\';
typedef HANDLE            proc_handle_t;
typedef HANDLE            shm_handle_t;
typedef DWORD             proc_id_t;
static const shm_handle_t NullShmHandle = NULL;
static const bool         UseCRLF       = true;
#else
static const char         PATH_SLASH = '/';
typedef void*             proc_handle_t;
typedef int               shm_handle_t;
typedef pid_t             proc_id_t;
static const shm_handle_t NullShmHandle = -1;
static const bool         UseCRLF       = false;
#define _In_z_
#define _Printf_format_string_
#endif

class TestHelper;

bool        ProcessCreate(const char* cmd, const char** argv, proc_handle_t& handle, proc_id_t& pid);
bool        WaitForProcessToDie(proc_handle_t handle, proc_id_t pid, uint32_t milliseconds);
proc_id_t   GetMyPID();
proc_id_t   GetMyTID();
std::string GetMyExePath();
void        SleepMS(uint32_t ms);
void        SharedMemObjectName(proc_id_t parentID, const char* logFilename, char shmName[100]);
bool        SetupSharedMemory(proc_id_t parentID, const char* logFilename, size_t size, bool create, shm_handle_t& shmHandle, void*& shmBuf);
void        CloseSharedMemory(shm_handle_t shmHandle, void* buf, size_t size);
size_t      SharedMemSizeFromRingSize(size_t ringBufferSize);
void        OutOfBandWarning(_In_z_ _Printf_format_string_ const char* msg, ...);
void        Panic(const char* msg);
std::string FullPath(const char* relpath);
uint64_t    siphash24(const void* src, size_t src_sz, const char key[16]);

/* Memory mapped ring buffer.
To write in two (or more) phases, use WriteNoCommit, each time increasing the
offset. When you're done, use Write, but make data null. In the final call
to Write, make len equal to the total length of all your writes.

Write and WriteNoCommit will panic if you try to call them with
a value of len that is greater than AvailableForWrite().
*/
class RingBuffer
{
public:
	// Size of read & write pointers
	static const size_t HeadSize = sizeof(size_t) * 2;

	uint8_t* Buf  = nullptr;
	size_t   Size = 0; // The size of the pure ring buffer (ie this number excludes the extra space used by the Read and Write pointers)

	// You must call Init() with a size that is a power of 2. However, the actual buffer
	// must be at least size + HeadSize large.
	// If reset is true, then the read and write pointers are set to 0.
	void   Init(void* buf, size_t size, bool reset);
	void   Write(const void* data, size_t len);
	void   WriteNoCommit(size_t offset, const void* data, size_t len);
	size_t Read(void* data, size_t max_len);
	void   ReadNoCopy(size_t len, void*& ptr1, size_t& ptr1_size, void*& ptr2, size_t& ptr2_size);

	std::atomic<size_t>* ReadPtr();
	std::atomic<size_t>* WritePtr();

	size_t AvailableForRead();
	size_t AvailableForWrite();
	size_t MaxAvailableForWrite() const { return Size - 1; } // The amount of data you can transmit atomically, when the buffer is empty
};

// A command sent over the ring buffer
enum class Command : uint32_t
{
	Null   = 0,
	Close  = 1,
	LogMsg = 2,
};

// Header of a message sent over the ring buffer
struct MessageHead
{
	Command  Cmd        = Command::Null;
	uint32_t Padding    = 0; // ensure PayloadLen starts at byte 8, and entire structure is defined.
	size_t   PayloadLen = 0;
};
} // namespace internal

// Logging levels
enum class Level
{
	Debug,
	Info,
	Warn,
	Error,
	Fatal, // Calls Panic() after sending the message
};

static char LevelChar(Level lev)
{
	switch (lev)
	{
	case Level::Debug: return 'D';
	case Level::Info: return 'I';
	case Level::Warn: return 'W';
	case Level::Error: return 'E';
	case Level::Fatal: return 'F';
	}
	return 'N';
}

/* A logger
Use this from your application to write logs. This class will launch the child process
and setup a memory mapped buffer which is used to communicate with the child process.

If you want to customize the format of the log messages, then you must wrap this class
inside your own logger, and redirect calls down to LogRaw or LogFmt. Creating a new
class that inherits from this is also a reasonable approach.
*/
class Logger
{
public:
	friend uberlog::internal::TestHelper;

	Logger();
	~Logger();

	void Open(const char* filename);
	void Close();

	// Set the ring buffer size, which is used to communicate between
	// the main process and the log writer process. This must be called
	// before Open(). This has no effect if called after Open() has been called.
	// Whatever value you specify is rounded up to the next power of 2.
	// The maximum size of a log message is limited by the size of the ring buffer.
	void SetRingBufferSize(size_t ringBufferSize);

	// Set the log archive settings. This must be called before Open().
	void SetArchiveSettings(int64_t maxFileSize, int32_t maxNumArchives);

	// Set the log level.
	void SetLevel(uberlog::Level level);

	// Low level "write bytes to log file"
	void LogRaw(const void* data, size_t len);

	// Write a log message in the default uberlog format, which is "Date [Level] ThreadID Message"
	template <typename... Args>
	void Log(Level level, const char* format_str, const Args&... args)
	{
		if (level < Level)
			return;

		// [------------- 42 characters ------------]
		// [------ 28 characters -----]
		// 2015-07-15T14:53:51.979+0200 [I] 00001fdc The log message here
		const size_t statbufsize = 200;
		char         statbuf[statbufsize];

		tsf::StrLenPair msg = tsf::fmt_buf(statbuf + 42, statbufsize - 42 - EolLen, format_str, args...);

		// Split this into two phases, to reduce the amount of code in the header
		LogDefaultFormat_Phase2(level, msg, msg.Str == statbuf + 42);
	}

	template <typename... Args>
	void Debug(const char* format_str, const Args&... args)
	{
		Log(uberlog::Level::Debug, format_str, args...);
	}

	template <typename... Args>
	void Info(const char* format_str, const Args&... args)
	{
		Log(uberlog::Level::Info, format_str, args...);
	}

	template <typename... Args>
	void Warn(const char* format_str, const Args&... args)
	{
		Log(uberlog::Level::Warn, format_str, args...);
	}

	template <typename... Args>
	void Error(const char* format_str, const Args&... args)
	{
		Log(uberlog::Level::Error, format_str, args...);
	}

	template <typename... Args>
	void Fatal(const char* format_str, const Args&... args)
	{
		Log(uberlog::Level::Fatal, format_str, args...);
	}

private:
	std::string                 Filename;
	size_t                      RingBufferSize            = 1 * 1024 * 1024;
	int64_t                     MaxFileSize               = 30 * 1048576;
	int32_t                     MaxNumArchives            = 3;
	int64_t                     NumLogMessagesSent        = 0;
	uint32_t                    TimeoutChildProcessInitMS = 10000; // Time we wait for our child process to come alive
	const int                   EolLen                    = uberlog::internal::UseCRLF ? 2 : 1;
	bool                        IsOpen                    = false;
	std::atomic<uberlog::Level> Level;
	internal::RingBuffer        Ring;
	internal::proc_handle_t     HChildProcess = nullptr; // not used on linux
	internal::proc_id_t         ChildPID      = -1;
	internal::shm_handle_t      ShmHandle     = internal::NullShmHandle;

	//	Special state for tests
	char _Test_OverridePrefix[42] = {0};

	bool Open();
	void SendMessage(internal::Command cmd, const void* payload, size_t payload_len);
	bool CreateRingBuffer();
	void CloseRingBuffer();
	bool WaitForRingToBeEmpty(uint32_t milliseconds); // Returns true if the ring is empty
	void LogDefaultFormat_Phase2(uberlog::Level level, tsf::StrLenPair msg, bool buf_is_static);
};
}
