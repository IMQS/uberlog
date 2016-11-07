#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include "uberlog.h"

void Die(const char* file, int line, const char* msg)
{
	printf("Assertion Failed\n%s:%d %s\n", file, line, msg);
	//__debugbreak();
	//__builtin_trap();
	exit(1);
}

#ifdef _WIN32
#define open _open
#define close _close
#define read _read
#define lseek _lseek
#define write _write
#else
#define O_BINARY 0
#endif

#undef ASSERT
#define ASSERT(f) (void) ((f) || (Die(__FILE__, __LINE__, #f), 0))

const char* TestLog       = "utest.log";
const char* TestLogPrefix = "2015-07-15T14:53:51.979+0200 [I] 00001fdc ";
#ifdef _WIN32
const char* EOL = "\r\n";
#else
const char* EOL = "\n";
#endif

// If 'expected' is null, verify that file cannot be opened.
void LogFileEquals(const char* expected)
{
	int f = open(TestLog, O_BINARY | O_RDONLY, 0);
	if (f == -1)
		ASSERT(expected == nullptr);
	if (expected == nullptr)
		ASSERT(false);
	size_t len = (size_t) lseek(f, 0, SEEK_END);
	lseek(f, 0, SEEK_SET);
	char* buf         = (char*) malloc(len + 1);
	int   nread       = read(f, buf, (int) len);
	buf[len]          = 0;
	size_t expect_len = strlen(expected);
	if (len != expect_len || memcmp(expected, buf, len) != 0)
	{
		FILE* f2 = fopen("expected", "wb");
		fwrite(expected, 1, expect_len, f2);
		fclose(f2);

		if (len < 500 && expect_len < 500)
			printf("expected: %s\n  actual: %s\n", expected, buf);
		size_t minLen = std::min(len, expect_len);
		for (size_t i = 0; i < minLen; i++)
		{
			if (buf[i] != expected[i])
			{
				printf("First difference at byte %d\n", (int) i);
				break;
			}
		}
		ASSERT(false);
	}
	free(buf);
	close(f);
}

bool FileExists(const char* path)
{
#ifdef _WIN32
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return stat(path, &st) == 0;
#endif
}

void DeleteLogFile()
{
	if (!FileExists(TestLog))
		return;
	int rv = remove(TestLog);
	if (rv == 0)
		return;

	ASSERT(false && "Unable to delete log file");
}

std::string MakeMsg(int len, int seed = 0)
{
	std::string x;
	for (int i = 0; x.length() < (size_t) len; i++)
	{
		char buf[50];
		sprintf(buf, "%d ", seed);
		x += buf;
		seed++;
		if ((i + seed) % 20 == 0)
			x += "\n";
	}
	x += "\n";
	if (x.length() > len)
		x.erase(x.begin() + len);
	return x;
}

struct LogOpenCloser
{
	uberlog::Logger Log;
	LogOpenCloser(size_t ringSize = 0, size_t rollingSize = 0)
	{
		DeleteLogFile();
		if (ringSize != 0)
			Log.SetRingBufferSize(ringSize);
		if (rollingSize != 0)
			Log.SetArchiveSettings(rollingSize, 3);
		Log.Open(TestLog);
	}
	~LogOpenCloser()
	{
		Log.Close();
		DeleteLogFile();
	}
};

namespace uberlog {
namespace internal {
class TestHelper
{
public:
	static void SetPrefix(uberlog::Logger& log, const char* prefix)
	{
		ASSERT(strlen(prefix) == 42);
		memcpy(log._Test_OverridePrefix, prefix, 42);
	}
};
}
}
using namespace uberlog::internal;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TestProcessLifecycle()
{
	printf("Process Lifecycle\n");
	for (int i = 0; i < 10; i++)
	{
		LogOpenCloser oc;
		oc.Log.LogRaw("hello", 5);
		oc.Log.Close();
		LogFileEquals("hello");
	}
}

void TestFormattedWrite()
{
	printf("Formatted Write\n");
	LogOpenCloser oc;
	std::string   expect;
	for (int size = 0; size <= 1000; size++)
	{
		TestHelper::SetPrefix(oc.Log, TestLogPrefix);
		oc.Log.Warn("%v", MakeMsg(size, size));
		expect += TestLogPrefix + MakeMsg(size, size) + EOL;
	}
	oc.Log.Close();
	LogFileEquals(expect.c_str());
}

void TestRingBuffer()
{
	printf("Ring Buffer\n");
	// Test two sizes of ring buffer. One that's smaller than LoggerSlaveWriteBufferSize, and one that's larger.
	// We must write chunks that are larger than the buffer, so that we stress that code path.
	// Bear in mind that we don't support writing log messages that are larger than our ring buffer, so we
	// make no attempt to test that.

	static_assert(LoggerSlaveWriteBufferSize == 1024, "Alter ring sizes for test");
	static const int nringSize            = 2;
	const size_t     ringSizes[nringSize] = {512, 8192};

	DeleteLogFile();

	for (int iRing = 0; iRing < nringSize; iRing++)
	{
		// important that we have at least one write size (5297) that is greater than LoggerSlaveWriteBufferSize
		const int    nsizes        = 8;
		const size_t sizes[nsizes] = {1, 2, 3, 59, 113, 307, 709, 5297};
		ASSERT(sizes[nsizes - 1] < ringSizes[nringSize - 1]); // Our 'big' write size must be smaller than our 'big' ring buffer size.
		uberlog::Logger log;
		log.SetRingBufferSize(ringSizes[iRing]);
		log.Open(TestLog);
		std::string expect;
		int         isize = 0;
		for (int i = 0; i < 1000; i++)
		{
			auto msg = MakeMsg((int) sizes[isize], i);
			log.LogRaw(msg.c_str(), msg.length());
			expect += msg;
			isize = (isize + 1) % nsizes;
			while (sizes[isize] > ringSizes[iRing])
				isize = (isize + 1) % nsizes;
		}
		log.Close();
		LogFileEquals(expect.c_str());
		DeleteLogFile();
	}
}

void BenchThroughput()
{
	printf("RingKB MsgLen   KB/s   Msg/s\n");
	size_t msgSizes[] = {1, 10, 200, 1000};
	for (size_t ringKB = 64; ringKB <= 8192; ringKB *= 2)
	{
		int isize = 2;
		//for (int isize = 0; isize < 4; isize++)
		{
			size_t        mlen = msgSizes[isize];
			LogOpenCloser oc(ringKB * 1024, 1000 * 1024 * 1024);
			std::string   msg   = MakeMsg((int) mlen, 0);
			auto          start = std::chrono::system_clock::now();
			size_t        niter = 5 * 10 * 1000 * 1000 / mlen;
			for (size_t i = 0; i < niter; i++)
			{
				oc.Log.LogRaw(msg.c_str(), msg.length());
			}
			oc.Log.Close();
			double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start).count() / 1000.0;
			printf("%6d %6d %6.0f %7.0f\n", (int) ringKB, (int) mlen, (mlen * niter / 1024.0) / elapsed_s, niter / elapsed_s);
		}
	}
}

double AccurateTimeSeconds()
{
#ifdef _MSC_VER
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (double) c.QuadPart / (double) f.QuadPart;
#else
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (double) tp.tv_sec + (double) tp.tv_nsec / 1000000000.0;
#endif
}

void BenchLatency()
{
	for (int mode = 0; mode < 2; mode++)
	{
		// Make the ring buffer size large enough that we never stall. We want to measure minimum latency here.
		LogOpenCloser oc(32768 * 1024, 500 * 1024 * 1024);

		size_t warmup = 100;
		size_t count = 50000;

		std::string staticMsg = "This is a message of a similar length, but it is a static string, so no formatting or time";

		double start = 0;
		for (size_t i = 0; i < warmup + count; i++)
		{
			if (i == warmup)
				start = AccurateTimeSeconds();

			if (mode == 0)
				oc.Log.Info("A typical log message, of a typical length, with %v or %v arguments", "two", "three");
			else if (mode == 1)
				oc.Log.LogRaw(staticMsg.c_str(), staticMsg.length());
		}
		double end = AccurateTimeSeconds();
		const char* zmode = mode == 0 ? "formatted" : "raw";
		tsf::printfmt("ns per message (%s): %v\n", zmode, 1000000000 * (end - start) / count);
	}
}

void BenchWriteLatency()
{
#ifdef _WIN32
	HANDLE fd = CreateFileA("xyz", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
#else
	int fd = open("xyz", O_BINARY | O_TRUNC | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
#endif

	size_t warmup = 100;
	size_t count = 200000;

	double start = 0;
	for (size_t i = 0; i < warmup + count; i++)
	{
		if (i == warmup)
			start = AccurateTimeSeconds();
#ifdef _WIN32
		WriteFile(fd, "hello", 5, NULL, NULL);
#else
		write(fd, "hello", 5);
#endif
	}
	double end = AccurateTimeSeconds();
	tsf::printfmt("ns per write: %v\n", 1000000000 * (end - start) / count);

#ifdef _WIN32
	CloseHandle(fd);
#else
	close(fd);
#endif
}

void HelloWorld()
{
	uberlog::Logger l;
	l.Open("hello.log");
	l.Info("Hello!");	
}

void TestAll()
{
	BenchWriteLatency();
	//BenchLatency();
	//BenchThroughput();
	//TestProcessLifecycle();
	//TestFormattedWrite();
	//TestRingBuffer();
}

#ifdef _WIN32
int AllocHook(int allocType, void* userData, size_t size, int blockType, long requestNumber, const unsigned char* filename, int lineNumber)
{
	return TRUE;
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
	_CrtSetAllocHook(AllocHook);
#endif

	TestAll();

	printf("OK\n");

	return 0;
}
