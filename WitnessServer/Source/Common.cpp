#include "Common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <stdarg.h>

#include <fstream>
#include <sstream>

string ReadFileToString(string_t Filename)
{
	std::ifstream File( Filename );
	stringstream Buffer;

	Buffer << File.rdbuf();

	return Buffer.str();
}

void SetStdinEcho( bool Enable )
{
#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE); 
    DWORD mode;
    GetConsoleMode(hStdin, &mode);

    if( !Enable )
        mode &= ~ENABLE_ECHO_INPUT;
    else
        mode |= ENABLE_ECHO_INPUT;

    SetConsoleMode(hStdin, mode );

#else
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    if( !Enable )
        tty.c_lflag &= ~ECHO;
    else
        tty.c_lflag |= ECHO;

    (void) tcsetattr(STDIN_FILENO, TCSANOW, &tty);
#endif
}

string_t Trim( string_t tInput )
{
	static const TCHAR pszWhitespace[] = _T(" \t\n\r");
	tInput = tInput.erase( tInput.find_last_not_of( pszWhitespace )+1 );
	tInput = tInput.erase( 0, tInput.find_first_not_of( pszWhitespace ) );
	return tInput;
}

vector< string_t > SplitString( string_t tInput, string_t tSeparator, StringTrim eTrim, StringStrip eStrip )
{
	vector< string_t > tResult;

	string_t::const_iterator tStart = tInput.begin();
	string_t::const_iterator tEnd;

	if( tInput.size() == 0 )
	{
			return tResult;
	}

	while( true )
	{
		tEnd = search< string_t::const_iterator, string_t::const_iterator >( tStart, tInput.end(), tSeparator.begin(), tSeparator.end() );

		string_t tSubString( tStart, tEnd );

		if( eTrim == StringTrim::Trim )
		{
			tSubString = Trim( tSubString );
		}

		if( !tSubString.empty() || eStrip == StringStrip::DoNotRemoveEmpty )
		{
			tResult.push_back( tSubString );
		}

		if( tEnd == tInput.end() )
		{
			break;
		}

		tStart = tEnd + tSeparator.size();
	}

	return tResult;
}

string StringPrintfA(char* Message, ...)
{
	va_list Args;
	va_start( Args, Message );

	size_t Length = _vscprintf( Message, Args );

	shared_ptr<char> Buffer( new char[ Length + 1 ], std::default_delete<char[]>() );

	vsprintf_s( Buffer.get(), Length + 1, Message, Args );

	va_end( Args );

	return string(Buffer.get());
}

wstring StringPrintfW(wchar_t* Message, ...)
{
	va_list Args;
	va_start( Args, Message );

	size_t Length = _vscwprintf( Message, Args );

	shared_ptr<wchar_t> Buffer( new wchar_t[ Length + 1 ], std::default_delete<wchar_t[]>() );

	vswprintf_s( Buffer.get(), Length + 1, Message, Args );

	va_end( Args );

	return wstring(Buffer.get());
}