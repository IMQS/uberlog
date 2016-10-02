/*
This is the child process that is spawned by a log writer.
Here we consume log messages from the ring buffer, and write them
into the log file.
*/
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#ifdef __linux__
#include <sys/types.h>
#include <unistd.h>
#endif

#include <string>
#include <thread>
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
			RollOver();
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

	void RollOver()
	{
		Close();
	}

private:
	std::string Filename;
	int64_t     FileSize        = 0;
	int64_t     MaxFileSize     = 0;
	int32_t     NumArchiveFiles = 0;
	int         FD              = -1;
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
