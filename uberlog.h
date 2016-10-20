#pragma once

#include <atomic>
#include <string>
#include "tsf.h"

namespace uberlog {

namespace internal {

#ifdef _WIN32
const char         PATH_SLASH = '\\';
typedef HANDLE     proc_handle_t;
typedef HANDLE     shm_handle_t;
typedef DWORD      proc_id_t;
const shm_handle_t NullShmHandle = NULL;
#else
const char         PATH_SLASH = '/';
typedef void*      TProcessHandle;
typedef int        shm_handle_t;
typedef pid_t      TProcessID;
const shm_handle_t NullShmHandle = -1;
#endif

bool        ProcessCreate(const char* cmd, const char* args, proc_handle_t& handle, proc_id_t& pid);
bool        WaitForProcessToDie(proc_handle_t handle, proc_id_t pid, uint32_t milliseconds);
proc_id_t   GetMyPID();
std::string GetMyExePath();
void        SleepMS(uint32_t ms);
void        SharedMemObjectName(proc_id_t parentID, const char* logFilename, char shmName[100]);
bool        SetupSharedMemory(proc_id_t parentID, const char* logFileName, size_t size, bool create, shm_handle_t& shmHandle, void*& shmBuf);
void        CloseSharedMemory(shm_handle_t shmHandle, void* buf, size_t size);
size_t      SharedMemSizeFromRingSize(size_t ringBufferSize);
void        OutOfBandWarning(_In_z_ _Printf_format_string_ const char* msg, ...);
void        Panic(const char* msg);
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
	size_t   Size = 0;

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
};

enum class Command : uint32_t
{
	Null   = 0,
	Close  = 1,
	LogMsg = 2,
};

struct MessageHead
{
	Command  Cmd        = Command::Null;
	uint32_t Padding    = 0; // ensure PayloadLen starts at byte 8, and entire structure is defined.
	size_t   PayloadLen = 0;
};
}

/* A logger
Use this from your application to write logs. This class will launch the child process
and setup a memory mapped buffer which is used to communicate with the child process.
*/
class Logger
{
public:
	Logger();
	~Logger();

	void Open(const char* filename);
	void Close();

	// Set the ring buffer size, which is used to communicate between
	// the main process and the log writer process. This must be called
	// before Open(). This has no effect if called after Open() has been called.
	// Whatever value you specify is rounded up to the next power of 2.
	void SetRingBufferSize(size_t ringBufferSize);

	// Set the log archive settings.
	void SetArchiveSettings(int64_t maxFileSize, int32_t maxNumArchives);

	// Low level "write bytes to log file"
	void LogRaw(const void* data, size_t len);

	// Write printf compatible, but type safe, formatted output to log file. See tsf.h for details
	template<typename... Args>
	void LogFmt(const char* format_str, const Args&... args)
	{
		const size_t bufsize = 160;
		char staticbuf[bufsize];
		tsf::CharLenPair res = tsf::fmt_static_buf(staticbuf, bufsize, format_str, args...);
		LogRaw(res.Str, res.Len);
		if (res.Str != staticbuf)
			delete[] res.Str;
	}

private:
	std::string             Filename;
	size_t                  RingBufferSize            = 1024 * 1024;
	int64_t                 MaxFileSize               = 30 * 1048576;
	int32_t                 MaxNumArchives            = 3;
	int64_t                 NumLogMessagesSent        = 0;
	uint32_t                TimeoutChildProcessInitMS = 10000; // Time we wait for our child process to come alive
	bool                    IsOpen                    = false;
	internal::RingBuffer    Ring;
	internal::proc_handle_t HChildProcess = nullptr; // not used on linux
	internal::proc_id_t     ChildPID      = -1;
	internal::shm_handle_t  ShmHandle     = internal::NullShmHandle;
	bool                    Open();
	void                    SendMessage(internal::Command cmd, const void* payload, size_t payload_len);
	bool                    CreateRingBuffer();
	void                    CloseRingBuffer();
	bool                    WaitForRingToBeEmpty(uint32_t milliseconds); // Returns true if the ring is empty
};
}
