#pragma once

#include <string>

namespace uberlog {
class Log
{
public:
	void Open(const char *filename);

private:
	std::string Filename;
	bool        IsOpen = false;
	bool        Open();
};
}
