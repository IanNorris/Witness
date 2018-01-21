#include "Common.h"

#include <fstream>
#include <sstream>

string ReadFileToString(string_t Filename)
{
	std::ifstream File( Filename );
	stringstream Buffer;

	Buffer << File.rdbuf();

	return Buffer.str();
}
