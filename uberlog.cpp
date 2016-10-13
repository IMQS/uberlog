#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <assert.h>

#include "uberlog.h"

using namespace uberlog::internal;

namespace uberlog {

///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
static const char PATH_SLASH = '\\';
static bool       _ProcessCreate(const char* cmd, const char* args, HANDLE& handle, DWORD& pid)
{
	STARTUPINFO         si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	std::string buf;
	buf += cmd;
	buf += " ";
	buf += args;
	DWORD flags = 0;
	// flags |= CREATE_SUSPENDED; // Useful for debugging
	flags |= CREATE_NEW_CONSOLE; // Useful for debugging
	if (!CreateProcess(NULL, &buf[0], NULL, NULL, false, flags, NULL, NULL, &si, &pi))
		return false;
	//ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	//CloseHandle(pi.hProcess);
	handle = pi.hProcess;
	pid    = pi.dwProcessId;
	return true;
}
static bool _WaitForProcessToDie(HANDLE handle, DWORD pid, uint32_t milliseconds)
{
	bool isDead = WaitForSingleObject(handle, (DWORD) milliseconds) == WAIT_OBJECT_0;
	CloseHandle(handle);
	return isDead;
}
static std::string _GetMyExePath()
{
	char buf[4096];
	GetModuleFileNameA(NULL, buf, (DWORD) sizeof(buf));
	buf[sizeof(buf) - 1] = 0;
	return buf;
}
#else
static const char PATH_SLASH = '/';
static bool       _ProcessCreate(const char* cmd, const char* args, void*& handle__unused, pid_t& pid)
{
	return false;
}
static bool _WaitForProcessToDie(void* handle, pid_t pid, uint32_t milliseconds)
{
	// If we don't do this, we end up with zombie child processes
	auto start = std::chrono::system_clock::now();
	for (;;)
	{
		int r = waitpid(pid, &status, WNOHANG | WUNTRACED);
		if (r == pid)
			return true;
		usleep(1000);
		std::chrono::milliseconds elapsed_ms = std::chrono::system_clock::now() - start;
		if (elapsed_ms.count() > milliseconds)
			return false;
	}
}
static std::string _GetMyExePath()
{
	char buf[4096];
	buf[0]   = 0;
	size_t r = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (r == -1)
		return buf;

	if (r < sizeof(buf))
		buf[r] = 0;
	else
		buf[sizeof(buf) - 1] = 0;
	return buf;
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
namespace internal {

static size_t RoundUpToPowerOf2(size_t v)
{
	size_t x = 1;
	while (x < v)
		x <<= 1;
	return x;
}

void RingBuffer::Init(void* buf, size_t size)
{
	size_t size_down2 = RoundUpToPowerOf2(size) >> 1;
	assert(size == size_down2 + HeadSize); // If you don't do this, then you're wasting VM
	Buf  = (uint8_t*) buf;
	Size = size_down2;
	ReadPtr()->store(0);
	WritePtr()->store(0);
}

// If data is null, then the only thing we do here is increment the write pointer.
bool RingBuffer::Write(const void* data, size_t len)
{
	if (data != nullptr)
	{
		if (!WriteNoCommit(0, data, len))
			return false;
	}
	size_t writep = WritePtr()->load();
	WritePtr()->store((writep + len) & (Size - 1));
	return true;
}

// Writes data, but does not alter WritePtr.
// Used to implement writing out a message in more than one piece,
// and atomically updating the write pointer at the end.
// Data is written into the address WritePtr + offset.
bool RingBuffer::WriteNoCommit(size_t offset, const void* data, size_t len)
{
	if (AvailableForWrite() < len + offset)
		return false;

	const uint8_t* data8 = (const uint8_t*) data;

	size_t writep = (WritePtr()->load() + offset) & (Size - 1);
	if (writep + len > Size)
	{
		// split
		auto part1 = Size - writep;
		memcpy(Buf + writep, data8, part1);
		memcpy(Buf, data8 + part1, len - part1);
	}
	else
	{
		memcpy(Buf + writep, data8, len);
	}

	return true;
}

size_t RingBuffer::Read(void* data, size_t max_len)
{
	uint8_t* data8 = (uint8_t*) data;
	size_t   copy  = std::min((size_t) max_len, AvailableForRead());
	size_t   readp = ReadPtr()->load();
	if (readp + copy > Size)
	{
		// split
		auto part1 = Size - readp;
		memcpy(data8, Buf + readp, part1);
		memcpy(data8 + part1, Buf, copy - part1);
	}
	else
	{
		memcpy(data8, Buf + readp, copy);
	}
	ReadPtr()->store((readp + copy) & (Size - 1));
	return copy;
}

std::atomic<size_t>* RingBuffer::ReadPtr()
{
	return (std::atomic<size_t>*) (Buf + Size);
}

std::atomic<size_t>* RingBuffer::WritePtr()
{
	return (std::atomic<size_t>*) (Buf + Size + sizeof(size_t));
}

size_t RingBuffer::AvailableForRead()
{
	size_t readp  = ReadPtr()->load();
	size_t writep = WritePtr()->load();
	return (writep - readp) & (Size - 1);
}

size_t RingBuffer::AvailableForWrite()
{
	return Size - 1 - AvailableForRead();
}
}
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

Logger::Logger()
{
}

Logger::~Logger()
{
	Close();
}

void Logger::Open(const char* filename)
{
	Filename = filename;
	Open();
}

void Logger::Close()
{
	if (!IsOpen)
		return;

	SendMessage(Command::Close, nullptr, 0);
	_WaitForProcessToDie(HChildProcess, ChildPID, 0);

	HChildProcess = nullptr;
	ChildPID      = -1;

	CloseRingBuffer();
	IsOpen = false;
}

void Logger::SetRingBufferSize(size_t ringBufferSize)
{
	if (IsOpen)
	{
		printf("Log.SetRingBufferSize must be called before Open\n");
		return;
	}
	RingBufferSize = RoundUpToPowerOf2(ringBufferSize) + RingBuffer::HeadSize;
}

void Logger::SetArchiveSettings(int64_t maxFileSize, int32_t maxNumArchives)
{
	if (IsOpen)
	{
		printf("Log.SetArchiveSettings must be called before Open\n");
		return;
	}
	MaxFileSize    = maxFileSize;
	MaxNumArchives = maxNumArchives;
}

void Logger::LogRaw(const void* data, size_t len)
{
	if (!IsOpen)
		return;
	SendMessage(Command::LogMsg, data, len);
}

bool Logger::Open()
{
	if (IsOpen)
		return true;

	if (!SetupRingBuffer())
		return false;

	std::string uberLoggerPath = "uberlogger";
	auto        myPath         = _GetMyExePath();
	auto        lastSlash      = myPath.rfind(PATH_SLASH);
	if (lastSlash != std::string::npos)
		uberLoggerPath = myPath.substr(0, lastSlash + 1) + "uberlogger";

	char args[4096];
	sprintf(args, "%u %u %s %lld %d", GetMyPID(), (uint32_t) RingBufferSize, Filename.c_str(), (long long) MaxFileSize, MaxNumArchives);
	if (!_ProcessCreate(uberLoggerPath.c_str(), args, HChildProcess, ChildPID))
	{
		CloseRingBuffer();
		return false;
	}

	IsOpen = true;
	return true;
}

bool Logger::SendMessage(internal::Command cmd, const void* payload, size_t payload_len)
{
	MessageHead msg;
	msg.Cmd        = cmd;
	msg.PayloadLen = payload_len;
	if (!Ring.WriteNoCommit(0, &msg, sizeof(msg)))
		return false;
	if (payload && !Ring.WriteNoCommit(sizeof(msg), payload, payload_len))
		return false;
	return Ring.Write(nullptr, sizeof(msg) + payload_len);
}

#ifdef _WIN32
bool Logger::SetupRingBuffer()
{
	char shmName[100];
	sprintf(shmName, "uberlog-shm-%u", GetMyPID());
	HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(RingBufferSize >> 32), (DWORD) RingBufferSize, shmName);
	if (mapping == NULL)
	{
		printf("uberlog: CreateFileMapping failed: %u\n", GetLastError());
		return false;
	}
	void* buf = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, RingBufferSize);
	CloseHandle(mapping);
	if (buf == NULL)
	{
		printf("uberlog: MapViewOfFile failed: %u\n", GetLastError());
		return false;
	}
	Ring.Init(buf, RingBufferSize);
	return true;
}

void Logger::CloseRingBuffer()
{
	if (Ring.Buf)
		UnmapViewOfFile(Ring.Buf);
	Ring.Buf  = nullptr;
	Ring.Size = 0;
}
#endif
#ifdef __linux__
bool Logger::SetupRingBuffer()
{
}

void Logger::CloseRingBuffer()
{
}
#endif

Logger::TProcID Logger::GetMyPID()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return getpid();
#endif
}
}
