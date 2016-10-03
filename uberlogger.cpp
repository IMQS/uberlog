/*
This is the child process that is spawned by a log writer.
Here we consume log messages from the ring buffer, and write them
into the log file.
*/
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
static const char PLATFORM_SLASH = '\\';
#endif

#ifdef __linux__
#include <sys/types.h>
#include <unistd.h>
#include <glob.h>
static const char PLATFORM_SLASH = '/';
#endif

#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <stdio.h>
#include <stdint.h>
#include "uberlog.h"

struct Globals
{
	bool        IsParentDead = false;
	std::string Filename;
	uint32_t    ParentPID = 0;
	std::thread WatcherThread;
};

std::thread WatchForParentProcessDeath(Globals* glob)
{
#ifdef _WIN32
	// Assume that if we can't open the process, then it is already dead
	HANDLE hproc = OpenProcess(SYNCHRONIZE, false, (DWORD) glob->ParentPID);
	if (hproc == NULL)
	{
		glob->IsParentDead = true;
		return std::thread();
	}

	return std::thread([glob, hproc]() {
		WaitForSingleObject(hproc, INFINITE);
		glob->IsParentDead = true;
	});
#else
	return std::thread();
#endif
}

void PollForParentProcessDeath(Globals* glob)
{
#ifdef __linux__
	// On linux, if our parent process dies, then our parent process becomes a process with PID equal to 0 or 1.
	// via http://stackoverflow.com/a/2035683/90614
	auto ppid = getppid();
	if (ppid == 0 || ppid == 1)
	{
		glob->IsParentDead = true;
	}
#endif
}

void SleepMS(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	int64_t  nanoseconds = ms * 1000000;
	timespec t;
	t.tv_nsec = nanoseconds % 1000000000;
	t.tv_sec  = (nanoseconds - t.tv_nsec) / 1000000000;
	nanosleep(&t, nullptr);
#endif
}

// Manage the log file, and the log rotation.
// Assume we are the only process writing to this log file.
class LogFile
{
public:
	LogFile(std::string filename, int64_t maxFileSize, int32_t numArchiveFiles) : Filename(filename), MaxFileSize(maxFileSize), NumArchiveFiles(numArchiveFiles)
	{
	}

	void Write(const void* buf, size_t len)
	{
		if (!Open())
			return;

		if (FileSize + (int64_t) len > MaxFileSize)
		{
			if (!RollOver())
				return;
			if (!Open())
				return;
		}

		auto res = write(FD, buf, len);
		if (res == -1)
		{
			Close();
			if (!Open())
				return;
			res = write(FD, buf, len);
		}

		if (res != -1)
			FileSize += res;
	}

	bool Open()
	{
		if (FD == -1)
		{
#ifdef _WIN32
			FD       = _open(Filename.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT, _S_IREAD | _S_IWRITE);
			FileSize = (int64_t) _lseeki64(FD, 0, SEEK_END);
#else
			FD       = open(Filename.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
			FileSize = (int64_t) lseek64(FD, 0, SEEK_END);
#endif
			if (FD != -1)
			{
				if (FileSize == -1)
				{
					close(FD);
					FD = -1;
				}
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
	int64_t     FileSize        = 0;
	int64_t     MaxFileSize     = 0;
	int32_t     NumArchiveFiles = 0;
	int         FD              = -1;

	std::string FilenameExtension() const
	{
		// figure out the log file extension
		auto        dot        = Filename.rfind('.');
		auto        lastFSlash = Filename.rfind('/');
		auto        lastBSlash = Filename.rfind('\\');
		std::string ext;
		if (dot != -1 && (lastFSlash == -1 || lastFSlash < dot) && (lastBSlash == -1 || lastBSlash < dot))
			return Filename.substr(Filename.length() - ext.length());
		return "";
	}

	std::string ArchiveFilename() const
	{
		// build time representation (UTC)
		char timeBuf[100];
		tm   time;
#ifdef _WIN32
		__time64_t t;
		_time64(&t);
		_gmtime64_s(&time, &t);
#else
		time_t t = time(nullptr);
		gmtime_r(t, &time)
#endif
		strftime(timeBuf, sizeof(timeBuf), "-%Y-%m-%dT%H:%M:%SZ", &time);

		// build the archive filename
		std::string ext     = FilenameExtension();
		std::string archive = Filename.substr(0, Filename.length() - ext.length());
		archive += timeBuf;
		archive += ext;
		return archive;
	}

	std::string LogDir() const
	{
		auto lastSlash = Filename.rfind(PLATFORM_SLASH);
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
		HANDLE          fh = FindFirstFileA(Filename.c_str(), &fd);
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
		std::sort(archives.begin(), archives.end());
		return archives;
	}

	bool RollOver()
	{
		Close();

		// rename current log file
		std::string archive = ArchiveFilename();
		if (rename(Filename.c_str(), archive.c_str()) != 0)
			return false;

		// delete old archives, but ignore failure
		auto archives = FindArchiveFiles();
		if (archives.size() > NumArchiveFiles)
		{
			for (size_t i = 0; i < archives.size() - NumArchiveFiles; i++)
				remove(archives[i].c_str());
		}
		return true;
	}
};

void ShowHelp()
{
	auto help = R"(uberlogger is a child process that is spawned by an application that performs logging.
Normally, you do not launch uberlogger manually. It is launched automatically by the uberlog library.
uberlogger <logfilename> <parentpid>)";
	printf("%s\n", help);
}

void Run(Globals& glob)
{
	int     fd = -1;
	LogFile log(glob.Filename, 30 * 1024 * 1024, 3);

	// Try to open file immediately, for consistency & predictability sake
	log.Open();

	while (!glob.IsParentDead)
	{
		PollForParentProcessDeath(&glob);
		SleepMS(1000);
	}
	if (fd != -1)
		close(fd);
	printf("Parent is dead\n");
	SleepMS(200);
}

int main(int argc, char** argv)
{
	Globals glob;
	bool    showHelp = true;

	if (argc == 3)
	{
		showHelp           = false;
		glob.Filename      = argv[1];
		glob.ParentPID     = (uint32_t) strtoul(argv[2], nullptr, 10);
		glob.WatcherThread = WatchForParentProcessDeath(&glob);
		Run(glob);
		if (glob.WatcherThread.joinable())
			glob.WatcherThread.join();
	}
	if (showHelp)
	{
		ShowHelp();
		return 1;
	}

	return 0;
}
