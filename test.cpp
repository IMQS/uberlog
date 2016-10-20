#include <windows.h>
#include <stdio.h>
#include "uberlog.h"

int AllocHook(int allocType, void *userData, size_t size, int blockType, long requestNumber, const unsigned char *filename, int lineNumber)
{
	return TRUE;
}

int main(int argc, char** argv)
{
	_CrtSetAllocHook(AllocHook);
	{
		std::string x = "x";
		std::string y = "yy";
		auto test1 = tsf::fmt_print("hello %v %v %v\n", x, 1, y);
		tsf::fmt_print("hello %v %v %v %v\n", x, 1, 2, 3);
		tsf::fmt_print("one %v three\n", 2);
	}
	{
		uberlog::Logger log;
		log.Open("c:\\temp\\test.log");
		printf("log opened\n");
		log.LogFmt("hello %v %v\r\n", 5, "five");
		log.LogFmt("yellow\r\n");
	}

	printf("parent exiting\n");
	//Sleep(3000);

	return 0;
}
