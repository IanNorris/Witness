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

#include <cwchar>
#include <stdexcept>

std::string ReadFileToString(std::string Filename)
{
	std::ifstream File( Filename );
	std::stringstream Buffer;

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

std::string Trim( std::string tInput )
{
	static const char pszWhitespace[] = " \t\n\r";
	tInput = tInput.erase( tInput.find_last_not_of( pszWhitespace )+1 );
	tInput = tInput.erase( 0, tInput.find_first_not_of( pszWhitespace ) );
	return tInput;
}

std::vector< std::string > SplitString( std::string tInput, std::string tSeparator, StringTrim eTrim, StringStrip eStrip )
{
	std::vector< std::string > tResult;

	std::string::const_iterator tStart = tInput.begin();
	std::string::const_iterator tEnd;

	if( tInput.size() == 0 )
	{
			return tResult;
	}

	while( true )
	{
		tEnd = std::search< std::string::const_iterator, std::string::const_iterator >( tStart, tInput.end(), tSeparator.begin(), tSeparator.end() );

		std::string tSubString( tStart, tEnd );

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

std::string StringPrintfA(const char* Message, ...)
{
	va_list Args;
	va_start( Args, Message );

	size_t Length = _vscprintf( Message, Args );

	std::shared_ptr<char> Buffer( new char[ Length + 1 ], std::default_delete<char[]>() );

	vsprintf_s( Buffer.get(), Length + 1, Message, Args );

	va_end( Args );

	return std::string(Buffer.get());
}