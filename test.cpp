#include <windows.h>
#include <stdio.h>
#include "uberlog.h"

int main(int argc, char** argv)
{
	uberlog::Log log;
	log.Open("c:\\temp\\test.log");
	printf("log opened\n");
	Sleep(1000);

	return 0;
}
