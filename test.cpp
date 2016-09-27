#include <windows.h>
#include <stdio.h>
#include "uberlog.h"

int main(int argc, char** argv)
{
	uberlog::Log log;
	if (argc == 1)
	{
		log.Open("c:\\temp\\test.log");
		printf("log opened\n");
		Sleep(60000);
	}
	else
	{
		printf("slave\n");
		Sleep(60000);
	}

	return 0;
}
