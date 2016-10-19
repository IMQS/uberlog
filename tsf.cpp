#include "tsf.h"
#include <assert.h>

namespace tsf {

static const size_t argbuf_arraysize = 64;

class StackBuffer
{
public:
	char*		Buffer;		// The buffer
	size_t		Pos;		// The number of bytes appended
	size_t		Capacity;	// Capacity of 'Buffer'
	bool		OwnBuffer;	// True if we have allocated the buffer

	StackBuffer(char* staticbuf, size_t staticbuf_size)
	{
		OwnBuffer = false;
		Pos = 0;
		Buffer = staticbuf;
		Capacity = staticbuf_size;
	}

	void Reserve(size_t bytes)
	{
		if (Pos + bytes > Capacity)
		{
			size_t ncap = Capacity * 2;
			if (ncap < Pos + bytes) ncap = Pos + bytes;
			char* nbuf = new char[ncap];
			memcpy(nbuf, Buffer, Pos);
			Capacity = ncap;
			if (OwnBuffer) delete[] Buffer;
			OwnBuffer = true;
			Buffer = nbuf;
		}
	}

	void MoveCurrentPos(size_t bytes)	{ Pos += bytes; assert(Pos <= Capacity); }

	char* AddUninitialized(size_t bytes)
	{
		Reserve(bytes);
		char* p = Buffer + Pos;
		Pos += bytes;
		return p;
	}

	void Add(char c)
	{
		char* p = AddUninitialized(1);
		*p = c;
	}

};

static inline void fmt_settype(char argbuf[argbuf_arraysize], size_t pos, const char* width, char type)
{
	if (width != NULL)
	{
		// set the type and the width specifier
		switch (argbuf[pos - 1])
		{
		case 'l':
		case 'h':
		case 'w':
			pos--;
			break;
		}

		for (; *width; width++, pos++)
			argbuf[pos] = *width;

		argbuf[pos++] = type;
		argbuf[pos++] = 0;
	}
	else
	{
		// only set the type, not the width specifier
		argbuf[pos++] = type;
		argbuf[pos++] = 0;
	}
}

static inline int fmt_output_with_snprintf(char* outbuf, char fmt_type, char argbuf[argbuf_arraysize], size_t argbufsize, size_t outputSize, const fmtarg* arg)
{
#define				SETTYPE1(type)			fmt_settype( argbuf, argbufsize, NULL, type )
#define				SETTYPE2(width, type)	fmt_settype( argbuf, argbufsize, width, type )

#ifdef _WIN32
	const char* i64Prefix = "I64";
	const char* wcharPrefix = "";
	const char wcharType = 'S';
#else
	const char* i64Prefix = "ll";
	const char* wcharPrefix = "l";
	const char wcharType = 's';
#endif

	bool tokenint = false;
	bool tokenreal = false;

	switch (fmt_type)
	{
	case 'd':
	case 'i':
	case 'o':
	case 'u':
	case 'x':
	case 'X':
		tokenint = true;
	}

	switch (fmt_type)
	{
	case 'e':
	case 'E':
	case 'f':
	case 'g':
	case 'G':
	case 'a':
	case 'A':
		tokenreal = true;
	}

	switch (arg->Type)
	{
	case fmtarg::TNull:
		return 0;
	case fmtarg::TCStr:
		SETTYPE2("", 's');
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->CStr);
	case fmtarg::TWStr:
		SETTYPE2(wcharPrefix, wcharType);
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->WStr);
	case fmtarg::TI32:
		if (tokenint)	{ SETTYPE2("", fmt_type); }
		else			{ SETTYPE2("", 'd'); }
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->I32);
	case fmtarg::TU32:
		if (tokenint)	{ SETTYPE2("", fmt_type); }
		else			{ SETTYPE2("", 'u'); }
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->UI32);
	case fmtarg::TI64:
		if (tokenint)	{ SETTYPE2(i64Prefix, fmt_type); }
		else			{ SETTYPE2(i64Prefix, 'd'); }
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->I64);
	case fmtarg::TU64:
		if (tokenint)	{ SETTYPE2(i64Prefix, fmt_type); }
		else			{ SETTYPE2(i64Prefix, 'u'); }
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->UI64);
	case fmtarg::TDbl:
		if (tokenreal)	{ SETTYPE1(fmt_type); }
		else			{ SETTYPE1('g'); }
		return fmt_snprintf(outbuf, outputSize, argbuf, arg->Dbl);
		break;
	}

#undef SETTYPE1
#undef SETTYPE2

	return 0;
}

std::string fmt_core(const char* fmt, ssize_t nargs, const fmtarg* args)
{
	static const size_t bufsize = 256;
	char staticbuf[bufsize];
	CharLenPair res = fmt_core(fmt, nargs, args, staticbuf, bufsize);
	std::string str(res.Str, res.Len);
	if (res.Str != staticbuf)
		delete[] res.Str;
	return str;
}

CharLenPair fmt_core(const char* fmt, ssize_t nargs, const fmtarg* args, char* staticbuf, size_t staticbuf_size)
{
	ssize_t tokenstart = -1;	// true if we have passed a %, and are looking for the end of the token
	ssize_t iarg = 0;
	bool no_args_remaining;
	bool spec_too_long;
	bool disallowed;
	const ssize_t MaxOutputSize = 1 * 1024 * 1024;

	size_t initial_sprintf_guessed_size = staticbuf_size >> 2; // must be less than staticbuf_size
	StackBuffer output(staticbuf, staticbuf_size);

	const size_t argbuf_arraysize = 64;
	char argbuf[argbuf_arraysize];

	// we can always safely look one ahead, because 'fmt' is by definition zero terminated
	for (ssize_t i = 0; fmt[i]; i++)
	{
		if (tokenstart != -1)
		{
			bool tokenint = false;
			bool tokenreal = false;

			switch (fmt[i])
			{
			case 'a':
			case 'A':
			case 'c':
			case 'C':
			case 'd':
			case 'i':
			case 'e':
			case 'E':
			case 'f':
			case 'g':
			case 'G':
			case 'H':
			case 'o':
			case 's':
			case 'S':
			case 'u':
			case 'x':
			case 'X':
			case 'p':
			case 'n':
			case 'v':
				no_args_remaining	= iarg >= nargs;								// more tokens than arguments
				spec_too_long		= i - tokenstart >= argbuf_arraysize - 1;		// %_____too much data____v
				disallowed			= fmt[i] == 'n';

				if (no_args_remaining || spec_too_long || disallowed)
				{
					for (ssize_t j = tokenstart; j <= i; j++)
						output.Add(fmt[j]);
				}
				else
				{
					// prepare the single formatting token that we will send to snprintf
					ssize_t argbufsize = 0;
					for (ssize_t j = tokenstart; j < i; j++)
					{
						if (fmt[j] == '*') continue;	// ignore
						argbuf[argbufsize++] = fmt[j];
					}

					// grow output buffer size until we don't overflow
					const fmtarg* arg = &args[iarg];
					iarg++;
					ssize_t outputSize = initial_sprintf_guessed_size;
					while (true)
					{
						char* outbuf = (char*) output.AddUninitialized(outputSize);
						bool done = false;
						ssize_t written = fmt_output_with_snprintf(outbuf, fmt[i], argbuf, argbufsize, outputSize, arg);

						if (written >= 0 && written < outputSize)
						{
							output.MoveCurrentPos(written - outputSize);
							break;
						}
						else if (outputSize >= MaxOutputSize)
						{
							// give up. I first saw this on the Microsoft CRT when trying to write the "mu" symbol to an ascii string.
							break;
						}
						// discard and try again with a larger buffer
						output.MoveCurrentPos(-outputSize);
						outputSize = outputSize * 2;
					}
				}
				tokenstart = -1;
				break;
			case '%':
				output.Add('%');
				tokenstart = -1;
				break;
			default:
				break;
			}
		}
		else
		{
			switch (fmt[i])
			{
			case '%':
				tokenstart = i;
				break;
			default:
				output.Add(fmt[i]);
				break;
			}
		}
	}
	output.Add('\0');
	return {output.Buffer, output.Pos - 1};
}

static inline int fmt_translate_snprintf_return_value(int r, size_t count)
{
	if (r < 0 || (size_t) r >= count)
		return -1;
	else
		return r;
}

int fmt_snprintf(char* destination, size_t count, const char* format_str, ...)
{
	va_list va;
	va_start(va, format_str);
	int r = vsnprintf(destination, count, format_str, va);
	va_end(va);
	return fmt_translate_snprintf_return_value(r, count);
}

// On Windows, wide version has different behaviour to narrow, requiring that we set Count+1 instead of Count characters.
// On linux, both versions require Count+1 characters.
int fmt_swprintf(wchar_t* destination, size_t count, const wchar_t* format_str, ...)
{
	va_list va;
	va_start(va, format_str);
	int r = vswprintf(destination, count, format_str, va);
	va_end(va);
	return fmt_translate_snprintf_return_value(r, count);
}

} // namespace tsf
