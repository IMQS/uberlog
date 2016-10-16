#include <windows.h>
#include <stdio.h>
#include "uberlog.h"

int main(int argc, char** argv)
{
	uberlog::Logger log;
	log.Open("c:\\temp\\test.log");
	printf("log opened\n");
	log.LogRaw("hello", 5);

	printf("parent exiting\n");
	Sleep(1000);

	return 0;
}
