#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#endif

#include "uberlog.h"

namespace uberlog
{

///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
static const char PATH_SLASH = '\\';
static bool _ProcessCreate(const char* cmd, const char* args)
{
	STARTUPINFO si;
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
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
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
static bool _ProcessCreate(const char* cmd, const char* args)
{
	return false;
}
static std::string _GetMyExePath()
{
	char buf[4096];
	buf[0] = 0;
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

void Log::Open(const char* filename)
{
	Filename = filename;
	Open();
}

bool Log::Open()
{
	if (IsOpen)
		return true;

#ifdef _WIN32
	auto pid = GetCurrentProcessId();
#else
	auto pid = getpid();
#endif

	std::string uberLoggerPath = "uberlogger";
	auto myPath = _GetMyExePath();
	auto lastSlash = myPath.rfind(PATH_SLASH);
	if (lastSlash != std::string::npos)
		uberLoggerPath = myPath.substr(0, lastSlash + 1) + "uberlogger";

	char args[4096];
	sprintf(args, "%s %u", Filename.c_str(), pid);
	if (!_ProcessCreate(uberLoggerPath.c_str(), args))
		return false;

	IsOpen = true;
	return true;
}

}
