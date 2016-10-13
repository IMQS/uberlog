#pragma once

#include <atomic>
#include <string>

namespace uberlog {

namespace internal {

/* Memory mapped ring buffer.
To write in two (or more) phases, use WriteNoCommit, each time increasing the
offset. When you're done, use Write, but make data null. In the final call
to Write, make len equal to the total length of all your writes.
*/
class RingBuffer
{
public:
	// Size of read & write pointers
	static const size_t HeadSize = sizeof(size_t) * 2;

	size_t   Size = 0;
	uint8_t* Buf  = nullptr;

	// You must call Init() with a size that is a power_of_2 + HeadSize
	void   Init(void* buf, size_t size);
	bool   Write(const void* data, size_t len);
	bool   WriteNoCommit(size_t offset, const void* data, size_t len);
	size_t Read(void* data, size_t max_len);

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
#ifdef _WIN32
	typedef DWORD TProcID;
#else
	typedef pid_t TProcID;
#endif
	Logger();
	~Logger();

	void Open(const char* filename);
	void Close();

	// Set the ring buffer size, which is used to communicate between
	// the main process and the log writer process. This must be called
	// before Open(). This has no effect if called after Open() has been called.
	void SetRingBufferSize(size_t ringBufferSize);

	// Set the log archive settings.
	void SetArchiveSettings(int64_t maxFileSize, int32_t maxNumArchives);

	// Lowest level, generic "write bytes to log file"
	void LogRaw(const void* data, size_t len);

private:
	std::string          Filename;
	size_t               RingBufferSize = 1048576 + internal::RingBuffer::HeadSize;
	int64_t              MaxFileSize    = 30 * 1048576;
	int32_t              MaxNumArchives = 3;
	bool                 IsOpen         = false;
	internal::RingBuffer Ring;
#ifdef _WIN32
	HANDLE HChildProcess = nullptr;
#else
	void* HChildProcess = nullptr; // not used
#endif
	TProcID ChildPID = -1;
	bool    Open();
	bool    SendMessage(internal::Command cmd, const void* payload, size_t payload_len);
	bool    SetupRingBuffer();
	void    CloseRingBuffer();
	TProcID GetMyPID();
};
}
