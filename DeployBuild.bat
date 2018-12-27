@echo off

set Root=%1
set Output=%2
set SubModules=%Root%\ThirdParty\SubModules
set CppRest=%SubModules%\cpprestsdk\build.x64v141\Binaries\RelWithDebInfo
set CppRestD=%SubModules%\cpprestsdk\build.x64v141\Binaries\Debug

robocopy /njh /np /nfl /ndl %SubModules%\libsodium\Build\ReleaseDLL\x64 %Output% libsodium.*
robocopy /njh /np /nfl /ndl %SubModules%\opencv\bin\install\x64\vc15\bin %Output% opencv_world400*.*
robocopy /njh /np /nfl /ndl %CppRest% %Output% cpprest141_2_10* 
robocopy /njh /np /nfl /ndl %CppRest% %Output% cpprest141_2_10d* 
robocopy /njh /np /nfl /ndl %CppRest% %Output% boost*
robocopy /njh /np /nfl /ndl %CppRest% %Output% SSLEAY32.*
robocopy /njh /np /nfl /ndl %CppRest% %Output% LIBEAY32.*
robocopy /njh /np /nfl /ndl %CppRest% %Output% zlib1.*

robocopy /njh /np /nfl /ndl %CppRestD% %Output% cpprest141_2_10d* 
robocopy /njh /np /nfl /ndl %CppRestD% %Output% boost*
robocopy /njh /np /nfl /ndl %CppRestD% %Output% SSLEAY32.*
robocopy /njh /np /nfl /ndl %CppRestD% %Output% LIBEAY32.*
robocopy /njh /np /nfl /ndl %CppRestD% %Output% zlibd1.*