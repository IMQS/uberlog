#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include <windows.h>
#include <sys/types.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/unistd.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <sys/timeb.h>

#include "uberlog.h"

#ifdef _WIN32
#define ftime _ftime
#define timeb _timeb
#endif

using namespace uberlog::internal;

namespace uberlog {
namespace internal {

///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
bool ProcessCreate(const char* cmd, const char** argv, proc_handle_t& handle, proc_id_t& pid)
{
	STARTUPINFO         si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	std::string buf;
	for (size_t i = 0; argv[i]; i++)
	{
		buf += "\"";
		buf += argv[i];
		buf += "\"";
		buf += " ";
	}
	DWORD flags = 0;
	//flags |= CREATE_SUSPENDED; // Useful for debugging
	//flags |= CREATE_NEW_CONSOLE; // Useful for debugging
	if (!CreateProcess(NULL, &buf[0], NULL, NULL, false, flags, NULL, NULL, &si, &pi))
		return false;
	//ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	handle = pi.hProcess;
	pid    = pi.dwProcessId;
	return true;
}
bool WaitForProcessToDie(proc_handle_t handle, proc_id_t pid, uint32_t milliseconds)
{
	bool isDead = WaitForSingleObject(handle, (DWORD) milliseconds) == WAIT_OBJECT_0;
	CloseHandle(handle);
	return isDead;
}
proc_id_t GetMyPID()
{
	return GetCurrentProcessId();
}
proc_id_t GetMyTID()
{
	return GetCurrentThreadId();
}
std::string GetMyExePath()
{
	char buf[4096];
	GetModuleFileNameA(NULL, buf, (DWORD) sizeof(buf));
	buf[sizeof(buf) - 1] = 0;
	return buf;
}
void SleepMS(uint32_t ms)
{
	Sleep((DWORD) ms);
}
bool SetupSharedMemory(proc_id_t parentID, const char* logFilename, size_t size, bool create, shm_handle_t& shmHandle, void*& shmBuf)
{
	char shmName[100];
	SharedMemObjectName(parentID, logFilename, shmName);
	if (create)
		shmHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD) size, shmName);
	else
		shmHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, shmName);

	if (shmHandle == NULL)
	{
		OutOfBandWarning("uberlog: %s failed: %u\n", create ? "CreateFileMapping" : "OpenFileMapping", GetLastError());
		return false;
	}
	shmBuf = MapViewOfFile(shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (!shmBuf)
	{
		OutOfBandWarning("uberlog: MapViewOfFile failed: %u\n", GetLastError());
		CloseHandle(shmHandle);
		shmHandle = NULL;
		return false;
	}
	return true;
}
void CloseSharedMemory(shm_handle_t shmHandle, void* buf, size_t size)
{
	UnmapViewOfFile(buf);
	CloseHandle(shmHandle);
}
void DeleteSharedMemory(proc_id_t parentID, const char* logFilename)
{
	// not necessary on Windows
}
void Panic(const char* msg)
{
	fprintf(stdout, "uberlog panic: %s\n", msg);
	//fprintf(stderr, "uberlog panic: %s\n", msg);
	*((int*) 0) = 1;
}
#else
bool ProcessCreate(const char* cmd, const char** argv, proc_handle_t& handle, proc_id_t& pid)
{
	pid_t childid = vfork();
	if (childid == -1)
		return false;
	if (childid != 0)
	{
		// parent
		pid = childid;
		return true;
	}
	else
	{
		// child
		execv(cmd, (char* const*) argv);
		// Since we're using vfork, the only thing we're allowed to do now is call  __exit
		_exit(1);
		return false; // unreachable
	}
}
bool WaitForProcessToDie(proc_handle_t handle, proc_id_t pid, uint32_t milliseconds)
{
	// If we don't do this, we end up with zombie child processes
	auto start = std::chrono::system_clock::now();
	for (;;)
	{
		int status = 0;
		int r      = waitpid(pid, &status, WNOHANG | WUNTRACED);
		if (r == pid)
			return true;
		usleep(1000);
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
		if (elapsed_ms.count() > milliseconds)
			return false;
	}
}
proc_id_t GetMyPID()
{
	return getpid();
}
proc_id_t GetMyTID()
{
	return syscall(__NR_gettid);
}
std::string GetMyExePath()
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
void SleepMS(uint32_t ms)
{
	int64_t  nanoseconds = ms * 1000000;
	timespec t;
	t.tv_nsec = nanoseconds % 1000000000;
	t.tv_sec  = (nanoseconds - t.tv_nsec) / 1000000000;
	nanosleep(&t, nullptr);
}
bool SetupSharedMemory(proc_id_t parentID, const char* logFilename, size_t size, bool create, shm_handle_t& shmHandle, void*& shmBuf)
{
	char shmName[100];
	SharedMemObjectName(parentID, logFilename, shmName);
	if (create)
		shmHandle = shm_open(shmName, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	else
		shmHandle = shm_open(shmName, O_RDWR, 0);

	if (shmHandle == -1)
	{
		OutOfBandWarning("uberlog: shm_open(%s) failed: %d\n", create ? "create" : "open", errno);
		return false;
	}
	if (create)
	{
		if (ftruncate(shmHandle, size) != 0)
		{
			OutOfBandWarning("uberlog: ftruncate on shm failed: %d\n", errno);
			close(shmHandle);
			shmHandle = -1;
			return false;
		}
	}
	shmBuf = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shmHandle, 0);
	if (shmBuf == (void*) -1)
	{
		OutOfBandWarning("uberlog: mmap failed: %d\n", errno);
		close(shmHandle);
		shmHandle = -1;
		return false;
	}
	return true;
}
void CloseSharedMemory(shm_handle_t shmHandle, void* buf, size_t size)
{
	munmap(buf, size);
	close(shmHandle);
}
void DeleteSharedMemory(proc_id_t parentID, const char* logFilename)
{
	char shmName[100];
	SharedMemObjectName(parentID, logFilename, shmName);
	shm_unlink(shmName);
}
__attribute__((noreturn)) void Panic(const char* msg)
{
	fprintf(stdout, "uberlog panic: %s\n", msg);
	//fprintf(stderr, "uberlog panic: %s\n", msg);
	__builtin_trap();
}
#endif

void SharedMemObjectName(proc_id_t parentID, const char* logFilename, char shmName[100])
{
	char key1[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	char key2[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
	memcpy(key1, &parentID, sizeof(parentID));
	uint64_t h1 = siphash24(logFilename, strlen(logFilename), key1);
	uint64_t h2 = siphash24(logFilename, strlen(logFilename), key2);
#ifdef _WIN32
	sprintf(shmName, "uberlog-shm-%u-%08x%08x%08x%08x", parentID, (uint32_t)(h1 >> 32), (uint32_t) h1, (uint32_t)(h2 >> 32), (uint32_t) h2);
#else
	sprintf(shmName, "/uberlog-shm-%u-%08x%08x%08x%08x", parentID, (uint32_t)(h1 >> 32), (uint32_t) h1, (uint32_t)(h2 >> 32), (uint32_t) h2);
#endif
}

size_t SharedMemSizeFromRingSize(size_t ringBufferSize)
{
	size_t shmSize = ringBufferSize + RingBuffer::HeadSize;
	// Round up to next 4096 (x86 page size). Anything else is just wasting those last bytes.
	// In addition, we are more likely to catch off-by-one errors if we go right up to the edge
	// of mapped memory.
	shmSize = (shmSize + 4095) & ~((size_t) 4095);
	return shmSize;
}

// Emit a warning message that is not going into the log - eg. a warning about failing to setup the log writer, etc.
void OutOfBandWarning(_In_z_ _Printf_format_string_ const char* msg, ...)
{
	va_list va;

	va_start(va, msg);
	vfprintf(stdout, msg, va);
	va_end(va);

	//va_start(va, msg);
	//vfprintf(stderr, msg, va);
	//va_end(va);
}

std::string FullPath(const char* relpath)
{
#ifdef _WIN32
	char* full = _fullpath(nullptr, relpath, 0);
#else
	char* full = realpath(relpath, nullptr);
#endif
	if (!full)
		return relpath;
	std::string copy = full;
	free(full);
	return copy;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////
// This siphash implementation is from https://github.com/majek/csiphash

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define _le64toh(x) ((uint64_t)(x))
#elif defined(_WIN32)
/* Windows is always little endian, unless you're on xbox360
   http://msdn.microsoft.com/en-us/library/b0084kay(v=vs.80).aspx */
#define _le64toh(x) ((uint64_t)(x))
#elif defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define _le64toh(x) OSSwapLittleToHostInt64(x)
#else

/* See: http://sourceforge.net/p/predef/wiki/Endianness/ */
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/endian.h>
#else
#include <endian.h>
#endif
#if defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && \
    __BYTE_ORDER == __LITTLE_ENDIAN
#define _le64toh(x) ((uint64_t)(x))
#else
#define _le64toh(x) le64toh(x)
#endif

#endif

#define ROTATE(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define HALF_ROUND(a, b, c, d, s, t) \
	a += b;                          \
	c += d;                          \
	b = ROTATE(b, s) ^ a;            \
	d = ROTATE(d, t) ^ c;            \
	a = ROTATE(a, 32);

#define DOUBLE_ROUND(v0, v1, v2, v3)    \
	HALF_ROUND(v0, v1, v2, v3, 13, 16); \
	HALF_ROUND(v2, v1, v0, v3, 17, 21); \
	HALF_ROUND(v0, v1, v2, v3, 13, 16); \
	HALF_ROUND(v2, v1, v0, v3, 17, 21);

uint64_t siphash24(const void* src, size_t src_sz, const char key[16])
{
	const uint64_t* _key = (uint64_t*) key;
	uint64_t        k0   = _le64toh(_key[0]);
	uint64_t        k1   = _le64toh(_key[1]);
	uint64_t        b    = (uint64_t) src_sz << 56;
	const uint64_t* in   = (uint64_t*) src;

	uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
	uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
	uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
	uint64_t v3 = k1 ^ 0x7465646279746573ULL;

	while (src_sz >= 8)
	{
		uint64_t mi = _le64toh(*in);
		in += 1;
		src_sz -= 8;
		v3 ^= mi;
		DOUBLE_ROUND(v0, v1, v2, v3);
		v0 ^= mi;
	}

	uint64_t t  = 0;
	uint8_t* pt = (uint8_t*) &t;
	uint8_t* m  = (uint8_t*) in;
	switch (src_sz)
	{
	case 7: pt[6]                 = m[6];
	case 6: pt[5]                 = m[5];
	case 5: pt[4]                 = m[4];
	case 4: *((uint32_t*) &pt[0]) = *((uint32_t*) &m[0]); break;
	case 3: pt[2]                 = m[2];
	case 2: pt[1]                 = m[1];
	case 1: pt[0]                 = m[0];
	}
	b |= _le64toh(t);

	v3 ^= b;
	DOUBLE_ROUND(v0, v1, v2, v3);
	v0 ^= b;
	v2 ^= 0xff;
	DOUBLE_ROUND(v0, v1, v2, v3);
	DOUBLE_ROUND(v0, v1, v2, v3);
	return (v0 ^ v1) ^ (v2 ^ v3);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

static size_t RoundUpToPowerOf2(size_t v)
{
	size_t x = 1;
	while (x < v)
		x <<= 1;
	return x;
}

void RingBuffer::Init(void* buf, size_t size, bool reset)
{
	if ((size & (size - 1)) != 0)
		Panic("Ring Buffer size must be a power of 2");
	Buf  = (uint8_t*) buf;
	Size = size;
	if (reset)
	{
		ReadPtr()->store(0);
		WritePtr()->store(0);
	}
}

// If data is null, then the only thing we do here is increment the write pointer.
void RingBuffer::Write(const void* data, size_t len)
{
	if (data != nullptr)
		WriteNoCommit(0, data, len);
	size_t writep = WritePtr()->load();
	WritePtr()->store((writep + len) & (Size - 1));
}

// Writes data, but does not alter WritePtr.
// Used to implement writing out a message in more than one piece,
// and atomically updating the write pointer at the end.
// Data is written into the address WritePtr + offset.
void RingBuffer::WriteNoCommit(size_t offset, const void* data, size_t len)
{
	if (AvailableForWrite() < len + offset)
		return Panic("attempt to write more than available bytes to ringbuffer");

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
}

size_t RingBuffer::Read(void* data, size_t max_len)
{
	uint8_t* data8 = (uint8_t*) data;
	size_t   copy  = std::min((size_t) max_len, AvailableForRead());
	size_t   readp = ReadPtr()->load();
	if (data != nullptr)
	{
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
	}
	ReadPtr()->store((readp + copy) & (Size - 1));
	return copy;
}

// Fetch one or two pointers into the ring buffer, which hold the location of the readable data.
// Does not increment the read pointer. You must call Read(null, len) once you've copied the
// data out of the ring buffer.
void RingBuffer::ReadNoCopy(size_t len, void*& ptr1, size_t& ptr1_size, void*& ptr2, size_t& ptr2_size)
{
	if (len > AvailableForRead())
		Panic("ReadPointers attempted to read more than available bytes");

	size_t pos1 = ReadPtr()->load();
	ptr1        = Buf + pos1;
	if (pos1 + len <= Size)
	{
		ptr1_size = len;
		ptr2      = nullptr;
		ptr2_size = 0;
	}
	else
	{
		ptr1_size = Size - pos1;
		ptr2      = Buf;
		ptr2_size = len - ptr1_size;
	}
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

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TimeKeeper::TimeKeeper()
{
#ifdef _WIN32
	timeb t1;
	ftime(&t1);
	TimezoneMinutes = t1.timezone;
#else
	tzset();
	TimezoneMinutes    = timezone / 60; // The global libc variable 'timezone' is set by tzset(), and is seconds west of UTC.
#endif

	// cache time zone
	uint32_t tzhour = abs(TimezoneMinutes) / 60;
	uint32_t tzmin  = abs(TimezoneMinutes) % 60;
	snprintf(TimeZoneStr, 6, "%c%02u%02u", TimezoneMinutes <= 0 ? '+' : '-', tzhour, tzmin);

	NewDay();
}

void TimeKeeper::Format(char* buf)
{
	uint64_t seconds;
	uint32_t nano;
	UnixTimeNow(seconds, nano);
	
	if (seconds - LocalDayStartSeconds >= 86400)
		NewDay();

	uint32_t dsec = (uint32_t)(seconds - LocalDayStartSeconds);
	uint32_t hour = 0;
	while (dsec >= 3600)
	{
		hour++;
		dsec -= 3600;
	}

	uint32_t minute = 0;
	while (dsec >= 60)
	{
		minute++;
		dsec -= 60;
	}

	uint32_t milli = nano / 1000000;

	memcpy(buf, DateStr, 10);
	FormatUintDecimal(2, buf + 11, hour);
	FormatUintDecimal(2, buf + 14, minute);
	FormatUintDecimal(2, buf + 17, dsec);
	FormatUintDecimal(3, buf + 20, milli);
	buf[10] = 'T';
	buf[13] = ':';
	buf[16] = ':';
	buf[19] = '.';
	memcpy(buf + 23, TimeZoneStr, 5);
}

void TimeKeeper::NewDay()
{
	uint64_t seconds;
	uint32_t nano;
	UnixTimeNow(seconds, nano);
	tm t2;
#ifdef _WIN32
	time_t tmp = seconds;
	gmtime_s(&t2, &tmp);
#else
	struct timespec tp = {0};
	tp.tv_sec          = seconds;
	tp.tv_nsec         = nano;
	gmtime_r(&tp.tv_sec, &t2);
#endif
	LocalDayStartSeconds = seconds - (t2.tm_hour * 3600 + t2.tm_min * 60 + t2.tm_sec);
	strftime(DateStr, 11, "%Y-%m-%d", &t2);
}

void TimeKeeper::UnixTimeNow(uint64_t& seconds, uint32_t& nano)
{
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	uint64_t raw             = (uint64_t) ft.dwHighDateTime << 32 | (uint64_t) ft.dwLowDateTime;
	uint64_t unix_time_100ns = raw - (370 * 365 - 276) * (uint64_t) 86400 * (uint64_t) 10000000;
	seconds                  = unix_time_100ns / 10000000;
	nano                     = (unix_time_100ns % 10000000) * 100;
#else
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	seconds = tp.tv_sec;
	nano    = tp.tv_nsec;
#endif
	seconds -= TimezoneMinutes * 60;
}

void TimeKeeper::FormatUintDecimal(uint32_t ndigit, char* buf, uint32_t v)
{
	for (uint32_t i = 0; i < ndigit; i++)
	{
		buf[ndigit - i - 1] = "0123456789"[v % 10];
		v /= 10;
	}
}

void TimeKeeper::FormatUintHex(uint32_t ndigit, char* buf, uint32_t v)
{
	for (uint32_t i = 0; i < ndigit; i++)
	{
		buf[ndigit - i - 1] = "0123456789abcdef"[v % 16];
		v /= 16;
	}
}

} // namespace internal
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

Logger::Logger()
{
	Level = uberlog::Level::Info;
}

Logger::~Logger()
{
	Close();
}

void Logger::Open(const char* filename)
{
	Filename = FullPath(filename);
	Open();
}

void Logger::Close()
{
	if (!IsOpen)
		return;

	// Always wait 10 seconds for our child to exit.
	// Waiting is nice behaviour, because the caller knows that he can manipulate the log file after Close() returns.
	uint32_t timeout = 10000;

	SendMessage(Command::Close, nullptr, 0);
	WaitForProcessToDie(HChildProcess, ChildPID, timeout);

	HChildProcess = nullptr;
	ChildPID      = -1;

	CloseRingBuffer();
	IsOpen = false;
}

void Logger::SetRingBufferSize(size_t ringBufferSize)
{
	if (IsOpen)
	{
		OutOfBandWarning("Logger.SetRingBufferSize must be called before Open\n");
		return;
	}
	RingBufferSize = RoundUpToPowerOf2(ringBufferSize);
}

void Logger::SetArchiveSettings(int64_t maxFileSize, int32_t maxNumArchives)
{
	if (IsOpen)
	{
		OutOfBandWarning("Logger.SetArchiveSettings must be called before Open\n");
		return;
	}
	MaxFileSize    = maxFileSize;
	MaxNumArchives = maxNumArchives;
}

void Logger::SetLevel(uberlog::Level level)
{
	Level = level;
}

void Logger::LogRaw(const void* data, size_t len)
{
	if (!IsOpen)
	{
		OutOfBandWarning("Logger.LogRaw called but log is not open\n");
		return;
	}

	size_t maxLen = Ring.MaxAvailableForWrite() - sizeof(MessageHead);

	if (len > maxLen)
	{
		OutOfBandWarning("Log message truncated from %lld to %lld\n", (long long) len, (long long) maxLen);
		len = maxLen;
	}

	NumLogMessagesSent++;
	SendMessage(Command::LogMsg, data, len);
	if (NumLogMessagesSent == 1)
	{
		// At process startup, it is likely that we are sending messages, and our
		// child writer process has not yet opened a handle to the shared memory.
		// If we die during that initial period, then the log messages will
		// never be delivered, because by the time the child tries to open the
		// shared memory, we will already have died and the last reference to the
		// shared memory will have been lost. One could arguably shift this
		// check to the moment after we create our child process, but we choose
		// to do it here instead, so that there is increased chance that we can
		// get useful work done while we wait for our child process to start up.
		// This is the last moment in time where we can perform this check, and
		// still live up to our claim that we won't lose a single log message,
		// even if the main process faults immediately after sending that message.
		if (!WaitForRingToBeEmpty(TimeoutChildProcessInitMS))
			OutOfBandWarning("Timed out waiting for uberlog slave to consume log messages");
	}
}

bool Logger::Open()
{
	if (IsOpen)
		return true;

	if (!CreateRingBuffer())
		return false;

	std::string uberLoggerPath = "uberlogger";
	auto        myPath         = GetMyExePath();
	auto        lastSlash      = myPath.rfind(PATH_SLASH);
	if (lastSlash != std::string::npos)
		uberLoggerPath = myPath.substr(0, lastSlash + 1) + "uberlogger";

	const int   nArgs = 6;
	std::string args[nArgs];
	const char* argv[nArgs + 1];
	args[0] = uberLoggerPath;
	args[1] = tsf::fmt("%u", GetMyPID());
	args[2] = tsf::fmt("%u", RingBufferSize);
	args[3] = Filename;
	args[4] = tsf::fmt("%d", MaxFileSize);
	args[5] = tsf::fmt("%d", MaxNumArchives);
	for (size_t i = 0; i < nArgs; i++)
		argv[i] = args[i].c_str();
	argv[nArgs] = nullptr;

	if (!ProcessCreate(uberLoggerPath.c_str(), argv, HChildProcess, ChildPID))
	{
		CloseRingBuffer();
		return false;
	}

	IsOpen             = true;
	NumLogMessagesSent = 0;
	return true;
}

void Logger::SendMessage(internal::Command cmd, const void* payload, size_t payload_len)
{
	MessageHead msg;
	msg.Cmd        = cmd;
	msg.PayloadLen = payload_len;

	if (sizeof(msg) + payload_len > Ring.MaxAvailableForWrite())
		Panic("Attempt to write too much data to the ring buffer");

	// If there's not enough space in the ring buffer, then wait for slave to consume messages
	for (int64_t i = 0; sizeof(msg) + payload_len > Ring.AvailableForWrite(); i++)
	{
		if (i < 1000)
			SleepMS(0);
		else if (i < 2000)
			SleepMS(1);
		else
			SleepMS(5);

		if (i == 2001)
			OutOfBandWarning("Waiting for log writer slave to flush queue");
	}

	Ring.WriteNoCommit(0, &msg, sizeof(msg));
	if (payload)
		Ring.WriteNoCommit(sizeof(msg), payload, payload_len);
	Ring.Write(nullptr, sizeof(msg) + payload_len);
}

bool Logger::CreateRingBuffer()
{
	shm_handle_t shm = internal::NullShmHandle;
	void*        buf = nullptr;
	if (!SetupSharedMemory(GetMyPID(), Filename.c_str(), SharedMemSizeFromRingSize(RingBufferSize), true, shm, buf))
		return false;
	ShmHandle = shm;
	Ring.Init(buf, RingBufferSize, true);
	return true;
}

void Logger::CloseRingBuffer()
{
	if (Ring.Buf)
	{
		CloseSharedMemory(ShmHandle, Ring.Buf, SharedMemSizeFromRingSize(Ring.Size));
		DeleteSharedMemory(GetMyPID(), Filename.c_str());
	}
	Ring.Buf  = nullptr;
	Ring.Size = 0;
	ShmHandle = internal::NullShmHandle;
}

bool Logger::WaitForRingToBeEmpty(uint32_t milliseconds)
{
	auto start = std::chrono::system_clock::now();
	while (Ring.AvailableForRead() != 0)
	{
		internal::SleepMS(1);
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
		if (elapsed_ms.count() >= milliseconds)
			return false;
	}
	auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
	//printf("took %d for initial drain\n", (int) elapsed_ms.count());
	return true;
}

void Logger::LogDefaultFormat_Phase2(uberlog::Level level, tsf::StrLenPair msg, bool buf_is_static)
{
	// [------------- 42 characters ------------]
	// [------ 28 characters -----]
	// 2015-07-15T14:53:51.979+0200 [I] 00001fdc The log message here

	char*  buf;
	size_t bufsize = 42 + msg.Len + EolLen + 1;
	if (buf_is_static)
	{
		buf = msg.Str - 42;
	}
	else
	{
		buf = new char[bufsize];
		memcpy(buf + 42, msg.Str, msg.Len);
		delete[] msg.Str;
	}

	// Write the first part of the log message into buf. This is the time, the log level, and the thread id
	if (_Test_OverridePrefix[0] != 0)
	{
		memcpy(buf, _Test_OverridePrefix, 42);
	}
	else
	{
		TK.Format(buf);
		buf[28] = ' ';
		buf[29] = '[';
		buf[30] = LevelChar(level);
		buf[31] = ']';
		buf[32] = ' ';
		TimeKeeper::FormatUintHex(8, buf + 33, (unsigned int) uberlog::internal::GetMyTID());
	}
	buf[41] = ' ';
	if (uberlog::internal::UseCRLF)
	{
		buf[42 + msg.Len] = '\r';
		buf[43 + msg.Len] = '\n';
		buf[44 + msg.Len] = 0;
	}
	else
	{
		buf[42 + msg.Len] = '\n';
		buf[43 + msg.Len] = 0;
	}

	LogRaw(buf, bufsize - 1);

	if (!buf_is_static)
		delete[] buf;

	if (level == Level::Fatal)
		Panic(buf);
}

} // namespace uberlog
