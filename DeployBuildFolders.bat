call DeployBuild.bat %~dp0 x64\Release

set CppRestD=%SubModules%\cpprestsdk\build.x64v141\Binaries\Debug

robocopy /njh /njs /np /nfl /ndl %CppRestD%  %~dp0\x64\Debug cpprest141_2_10d* 
robocopy /njh /njs /np /nfl /ndl %CppRestD%  %~dp0\x64\Debug boost*
robocopy /njh /njs /np /nfl /ndl %CppRestD%  %~dp0\x64\Debug SSLEAY32.*
robocopy /njh /njs /np /nfl /ndl %CppRestD%  %~dp0\x64\Debug LIBEAY32.*
robocopy /njh /njs /np /nfl /ndl %CppRestD%  %~dp0\x64\Debug zlibd1.*

pause