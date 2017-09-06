/*
This is the child process that is spawned by a log writer.
Here we consume log messages from the ring buffer, and write them
into the log file.
*/
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/timeb.h>
#include <sys/types.h>
#define write _write
#define open _open
#define close _close
#endif

#ifdef __linux__
#include <sys/types.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <glob.h>
#include <time.h>
#endif

#include <string.h>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <stdio.h>
#include <stdint.h>
#include "uberlog.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 6031) // /analyze is worried about us ignoring snprintf return value, but we're statically sized everywhere.
#endif

namespace uberlog {
namespace internal {

// Manage the log file, and the log rotation.
// Assume we are the only process writing to this log file.
class LogFile
{
public:
	LogFile()
	{
	}

	~LogFile()
	{
		Close();
	}

	void Init(std::string filename, int64_t maxFileSize, int32_t maxNumArchiveFiles)
	{
		Filename           = filename;
		MaxFileSize        = maxFileSize;
		MaxNumArchiveFiles = maxNumArchiveFiles;
	}

	bool Write(const void* buf, size_t len)
	{
		if (!Open())
			return false;

		if (FileSize + (int64_t) len > MaxFileSize)
		{
			if (!RollOver())
				return false;
			if (!Open())
				return false;
		}

		if (len == 0)
			return true;

		// ignore the possibility that write() is allowed to write less than 'len' bytes.
		auto res = write(FD, buf, (int) len);
		if (res == -1)
		{
			// Perhaps something has happened on the file system, such as a network share being lost and then restored, etc.
			// Closing and opening again is the best thing we can try in this scenario.
			Close();
			if (!Open())
				return false;
			res = write(FD, buf, (int) len);
		}

		if (res != -1)
			FileSize += res;
		return res == len;
	}

	bool Open()
	{
		if (FD == -1)
		{
#ifdef _WIN32
			FD = _open(Filename.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT, _S_IREAD | _S_IWRITE);
			if (FD == -1)
				return false;
			FileSize = (int64_t) _lseeki64(FD, 0, SEEK_END);
#else
			FD = open(Filename.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
			if (FD == -1)
				return false;
			FileSize = (int64_t) lseek64(FD, 0, SEEK_END);
#endif
			if (FileSize == -1)
			{
				close(FD);
				FD = -1;
			}
		}
		return FD != -1;
	}

	void Close()
	{
		if (FD == -1)
			return;
		close(FD);
		FD       = -1;
		FileSize = 0;
	}

private:
	std::string Filename;
	int64_t     FileSize           = 0;
	int64_t     MaxFileSize        = 0;
	int32_t     MaxNumArchiveFiles = 0;
	int         FD                 = -1;

	std::string FilenameExtension() const
	{
		// figure out the log file extension
		auto        dot        = Filename.rfind('.');
		auto        lastFSlash = Filename.rfind('/');
		auto        lastBSlash = Filename.rfind('\\');
		std::string ext;
		if (dot != -1 && (lastFSlash == -1 || lastFSlash < dot) && (lastBSlash == -1 || lastBSlash < dot))
			return Filename.substr(dot);
		return "";
	}

	std::string ArchiveFilename() const
	{
		// build time representation (UTC)
		char timeBuf[100];
		tm   timev;
#ifdef _WIN32
		struct timeb t;
		ftime(&t);
		_gmtime64_s(&timev, &t.time);
		uint32_t millis = (uint32_t) t.millitm;
#else
		struct timespec tp;
		clock_gettime(CLOCK_REALTIME, &tp);
		time_t t = tp.tv_sec;
		uint32_t millis = tp.tv_nsec / 1000000;
		gmtime_r(&t, &timev);
#endif
		strftime(timeBuf, sizeof(timeBuf), "-%Y-%m-%dT%H-%M-%S-", &timev);
		char milliBuf[20];
		snprintf(milliBuf, 20, "%03u-Z", millis);
		strcat(timeBuf, milliBuf);

		// build the archive filename
		std::string ext     = FilenameExtension();
		std::string archive = Filename.substr(0, Filename.length() - ext.length());
		archive += timeBuf;
		archive += ext;
		return archive;
	}

	std::string LogDir() const
	{
		auto lastSlash = Filename.rfind(PATH_SLASH);
		if (lastSlash == -1)
			return "";
		return Filename.substr(0, lastSlash + 1);
	}

	std::vector<std::string> FindArchiveFiles() const
	{
		auto                     dir      = LogDir();
		auto                     ext      = FilenameExtension();
		auto                     wildcard = Filename.substr(0, Filename.length() - ext.length()) + "-*";
		std::vector<std::string> archives;
#ifdef _WIN32
		WIN32_FIND_DATA fd;
		HANDLE          fh = FindFirstFileA(wildcard.c_str(), &fd);
		if (fh != INVALID_HANDLE_VALUE)
		{
			do
			{
				archives.push_back(dir + fd.cFileName);
			} while (!!FindNextFile(fh, &fd));
			FindClose(fh);
		}
#else
		glob_t pglob;
		if (glob(wildcard.c_str(), 0, nullptr, &pglob) == 0)
		{
			for (size_t i = 0; i < pglob.gl_pathc; i++)
				archives.push_back(dir + pglob.gl_pathv[i]);
			globfree(&pglob);
		}
#endif
		// rely on our lexicographic archive naming convention, so that files are sorted oldest to newest
		std::sort(archives.begin(), archives.end());
		return archives;
	}

	bool RollOver()
	{
		Close();

		// rename current log file
		std::string archive = ArchiveFilename();
		if (rename(Filename.c_str(), archive.c_str()) != 0)
		{
			OutOfBandWarning("Rollover failed trying to rename '%s' to '%s'\n", Filename.c_str(), archive.c_str());
			return false;
		}

		// delete old archives, but ignore failure
		auto archives = FindArchiveFiles();
		if (archives.size() > (size_t) MaxNumArchiveFiles)
		{
			for (size_t i = 0; i < archives.size() - MaxNumArchiveFiles; i++)
				remove(archives[i].c_str());
		}
		return true;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////

// This implements the logic of the logger slave process
class LoggerSlave
{
public:
	static const size_t WriteBufSize        = LoggerSlaveWriteBufferSize;
	bool                EnableDebugMessages = false;
	uint32_t            ParentPID           = 0;
	uint32_t            RingSize            = 0;
	std::atomic<bool>   IsParentDead;
	RingBuffer          Ring;
	shm_handle_t        ShmHandle = internal::NullShmHandle;
	std::thread         WatcherThread;
	std::string         Filename;
	int64_t             MaxLogSize         = 30 * 1024 * 1024;
	int32_t             MaxNumArchives     = 3;
	uint32_t            MaxSleepMS         = 1024;
	uint32_t            WaitForOpenSleepMS = 1; // Our sleep periods when we're waiting for the ring buffer to be opened
	LogFile             Log;
	char*               WriteBuf = nullptr;

#ifdef _WIN32
	HANDLE CloseMessageEvent = NULL;
#else
	bool CloseMessageFlag = false;
#endif

	LoggerSlave()
	{
		IsParentDead = false;
	}

	void Run()
	{
		DebugMsg("uberlog writer [%v, %v MB max size, %v archives] is starting\n", Filename, MaxLogSize / 1024 / 1024, MaxNumArchives);

		WriteBuf = new char[WriteBufSize];

#ifdef _WIN32
		CloseMessageEvent = CreateEvent(NULL, true, false, NULL);
#endif

		std::thread watcherThread = WatchForParentProcessDeath(); // Windows-only

		Log.Init(Filename, MaxLogSize, MaxNumArchives);

		// Try to open file immediately, for consistency & predictability sake
		Log.Open();

		uint32_t sleepMS      = 0;
		uint64_t totalSleepMS = 0;

		while (!IsParentDead && !HasReceivedCloseMessage())
		{
			bool idle = false;
			if (!Ring.Buf)
				OpenRingBuffer();
			if (Ring.Buf)
			{
				uint64_t nmessages = ReadMessages();
				if (nmessages == 0)
					idle = true;
			}

			if (idle)
				sleepMS = std::min(std::max(sleepMS, 1u) * 2, MaxSleepMS);
			else if (Ring.Buf)
				sleepMS = 0;
			else
				sleepMS = WaitForOpenSleepMS;

			PollForParentProcessDeath(); // Not used on Windows
			totalSleepMS += sleepMS;
			internal::SleepMS(sleepMS);
		}

		// Drain the buffer
		if (IsParentDead && Ring.Buf)
			ReadMessages();

		//uberlog_tsf::print("Logger slave slept for a total of %v MS\n", totalSleepMS);

		CloseRingBuffer();
		Log.Close();

		if (HasReceivedCloseMessage())
			DebugMsg("uberlog is stopping: received Close instruction\n");

		if (IsParentDead)
			DebugMsg("uberlog is stopping: parent is dead\n");

		//SleepMS(2000); // DEBUG

		if (watcherThread.joinable())
			watcherThread.join();

#ifdef _WIN32
		if (CloseMessageEvent)
			CloseHandle(CloseMessageEvent);
		CloseMessageEvent = NULL;
#endif
		delete[] WriteBuf;
	}

private:
	std::thread WatchForParentProcessDeath()
	{
#ifdef _WIN32
		// Assume that if we can't open the process, then it is already dead
		HANDLE hproc = OpenProcess(SYNCHRONIZE, false, (DWORD) ParentPID);
		if (hproc == NULL)
		{
			IsParentDead = true;
			return std::thread();
		}

		return std::thread([this, hproc]() {
			HANDLE h[]   = {hproc, CloseMessageEvent};
			DWORD  fired = WaitForMultipleObjects(2, h, false, INFINITE) - WAIT_OBJECT_0;
			if (fired == 0)
				this->IsParentDead = true;
			CloseHandle(hproc);
		});
#else
		// The linux implementation polls, via PollForParentProcessDeath
		return std::thread();
#endif
	}

	void PollForParentProcessDeath()
	{
#ifdef __linux__
		// On linux, if our parent process dies, then our parent process becomes a process with PID equal to 0 or 1.
		// via http://stackoverflow.com/a/2035683/90614
		auto ppid = getppid();
		if (ppid == 0 || ppid == 1)
		{
			IsParentDead = true;
		}
#endif
		// On Windows, we don't need to implement this path, because we can wait on process death
	}

	bool OpenRingBuffer()
	{
		shm_handle_t shm = NullShmHandle;
		void*        buf = nullptr;
		if (!SetupSharedMemory(ParentPID, Filename.c_str(), SharedMemSizeFromRingSize(RingSize), false, shm, buf))
			return false;
		ShmHandle = shm;
		Ring.Init(buf, RingSize, false);
		return true;
	}

	void CloseRingBuffer()
	{
		if (Ring.Buf)
			CloseSharedMemory(ShmHandle, Ring.Buf, SharedMemSizeFromRingSize(Ring.Size));
		Ring.Buf  = nullptr;
		Ring.Size = 0;
		ShmHandle = NullShmHandle;
	}

	// Returns number of log messages consumed
	uint64_t ReadMessages()
	{
		// Buffer up messages, so that we don't issue an OS write for every message
		size_t   bufpos    = 0;
		uint64_t nmessages = 0;

		while (true)
		{
			MessageHead head;
			size_t      avail = Ring.AvailableForRead();
			if (avail < sizeof(head))
				break;

			if (Ring.Read(&head, sizeof(head)) != sizeof(head))
				Panic("ring.Read(head) failed");
			avail -= sizeof(head);

			switch (head.Cmd)
			{
			case Command::Close:
				SetReceivedCloseMessage();
				break;
			case Command::LogMsg:
			{
				nmessages++;
				if (avail < head.PayloadLen)
					Panic("ring.Read: message payload not available in ring buffer");

				if (head.PayloadLen > WriteBufSize - bufpos)
				{
					if (!Log.Write(WriteBuf, bufpos))
						OutOfBandWarning("Failed to write to log file");
					bufpos = 0;
				}

				if (head.PayloadLen <= WriteBufSize - bufpos)
				{
					// store message in buffer
					size_t nread = Ring.Read(WriteBuf + bufpos, head.PayloadLen);
					if (nread != head.PayloadLen)
						Panic("ring.Read: unable to read all of payload\n");
					bufpos += head.PayloadLen;
				}
				else
				{
					// log message is too large to buffer. write it directly
					if (bufpos != 0)
						Panic("ring.Read: should have flushed log"); // the Log.Write call 10 lines above should have done this already
					void*  ptr1  = nullptr;
					void*  ptr2  = nullptr;
					size_t size1 = 0;
					size_t size2 = 0;
					Ring.ReadNoCopy(head.PayloadLen, ptr1, size1, ptr2, size2);
					bool ok = Log.Write(ptr1, size1);
					if (ok && size2 != 0)
						ok = Log.Write(ptr2, size2);
					if (!ok)
						OutOfBandWarning("Failed to write to log file");
					Ring.Read(nullptr, head.PayloadLen);
				}
				break;
			}
			default:
				Panic("Unexpected command");
			}
		}

		// flush write buffer
		if (bufpos != 0)
		{
			if (!Log.Write(WriteBuf, bufpos))
				OutOfBandWarning("Failed to write to log file");
		}

		return nmessages;
	}

	void SetReceivedCloseMessage()
	{
#ifdef _WIN32
		SetEvent(CloseMessageEvent);
#else
		CloseMessageFlag = true;
#endif
	}

	bool HasReceivedCloseMessage()
	{
#ifdef _WIN32
		return WaitForSingleObject(CloseMessageEvent, 0) == WAIT_OBJECT_0;
#else
		return CloseMessageFlag;
#endif
	}

	template <typename... Args>
	void DebugMsg(const char* msg, const Args&... args)
	{
		if (!EnableDebugMessages)
			return;
		fputs(uberlog_tsf::fmt(msg, args...).c_str(), stdout);
	}
};

void ShowHelp()
{
	auto help = R"(uberlogger is a child process that is spawned by an application that performs logging.
Normally, you do not launch uberlogger manually. It is launched automatically by the uberlog library.
uberlogger <parentpid> <ringsize> <logfilename> <maxlogsize> <maxarchives>)";
	printf("%s\n", help);
}
} // namespace internal
} // namespace uberlog

int main(int argc, char** argv)
{
	bool showHelp = true;

	if (argc == 6)
	{
		showHelp = false;
		uberlog::internal::LoggerSlave slave;
		slave.ParentPID      = (uint32_t) strtoul(argv[1], nullptr, 10);
		slave.RingSize       = (uint32_t) strtoul(argv[2], nullptr, 10);
		slave.Filename       = argv[3];
		slave.MaxLogSize     = (int64_t) strtoull(argv[4], nullptr, 10);
		slave.MaxNumArchives = (int32_t) strtol(argv[5], nullptr, 10);
		slave.Run();
	}
	if (showHelp)
	{
		uberlog::internal::ShowHelp();
		return 1;
	}

	return 0;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
