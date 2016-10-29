#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include "uberlog.h"

void Die(const char* file, int line, const char* msg)
{
	printf("Assertion Failed\n%s:%d %s\n", file, line, msg);
	__debugbreak();
	exit(1);
}

#ifdef _WIN32
#define open _open
#define close _close
#define read _read
#define lseek _lseek
#else
#define O_BINARY 0
#endif

#undef ASSERT
#define ASSERT(f) (void) ((f) || (Die(__FILE__,__LINE__,#f), 0) )

const char* TestLog = "utest.log";
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
	char* buf = (char*) malloc(len + 1);
	int nread = read(f, buf, (int) len);
	buf[len] = 0;
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
	LogOpenCloser()
	{
		DeleteLogFile();
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
	std::string expect;
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
	
	static_assert(LoggerSlaveWriteBufferSize == 4096, "Alter ring sizes for test");
	size_t ringSizes[2] = {512, 8192};

	DeleteLogFile();

	for (int iRing = 0; iRing < 2; iRing++)
	{
		// important that we have at least one write size (5297) that is greater than LoggerSlaveWriteBufferSize 
		const int nsizes = 8;
		size_t sizes[nsizes] = {1, 2, 3, 59, 113, 307, 709, 5297}; 
		uberlog::Logger log;
		log.SetRingBufferSize(ringSizes[iRing]);
		log.Open(TestLog);
		std::string expect;
		int isize = 0;
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

void TestAll()
{
	TestProcessLifecycle();
	TestFormattedWrite();
	TestRingBuffer();
}

#ifdef _WIN32
int AllocHook(int allocType, void *userData, size_t size, int blockType, long requestNumber, const unsigned char *filename, int lineNumber)
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
