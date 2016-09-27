#include <windows.h>
#include "uberlog.h"

namespace uberlog
{

///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
namespace internal
{
bool ProcessCreate(const char* cmd, const char* args, HANDLE* handle, DWORD* pid)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	char buf[4096];
	strcpy(buf, cmd);
	strcat(buf, " ");
	strcat(buf, args);
	if (!CreateProcess(NULL, buf, NULL, NULL, false, 0, NULL, NULL, &si, &pi))
		return false;
	CloseHandle(pi.hThread);
	if (handle)
		*handle = pi.hProcess;
	else
		CloseHandle(pi.hProcess);
	if (pid)
		*pid = pi.dwProcessId;
	return true;
}
}


///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

void Log::Open(const char* filename)
{
	internal::ProcessCreate("uberlog.exe", "foo bar", nullptr, nullptr);
}

}
