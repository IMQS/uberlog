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
		auto test1 = tsf::printfmt("hello %v %v %v\n", x, 1, y);
		tsf::printfmt("hello %v %v %v %v\n", x, 1, 2, 3);
		tsf::printfmt("one %v three\n", 2);
	}
	{
		uberlog::Logger log;
		log.Open("c:\\temp\\test.log");
		printf("log opened\n");
		log.LogFmt("hello %v %v\r\n", 5, "five");
		log.LogFmt("yellow\r\n");
		log.LogDefaultFormat(uberlog::Level::Info, "the quick %v %v", "brown", "fox");
		log.LogDefaultFormat(uberlog::Level::Info, "the quick %v %v       a longer longer longer string that doesn't fit into the static buffer!", "brown", "fox fox fox rabbit rabbit rabbit");
		log.LogDefaultFormat(uberlog::Level::Warn, "moooar!");
	}

	printf("parent exiting\n");
	//Sleep(3000);

	return 0;
}
