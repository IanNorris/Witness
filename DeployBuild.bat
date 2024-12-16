REM @echo off

set Root=%1
set Output=%2
set SubModules=%Root%\ThirdParty\SubModules
set CppRest=%SubModules%\cpprestsdk\build.x64v141\Binaries\RelWithDebInfo
set CppRestD=%SubModules%\cpprestsdk\build.x64v141\Binaries\Debug

mkdir %Output%\Web
robocopy /S %Root%\WitnessServer\Web\ %Output%\Web *.*

robocopy /njh /njs /np /nfl /ndl %SubModules%\libsodium\Build\ReleaseDLL\x64 %Output% libsodium.*
robocopy /njh /njs /np /nfl /ndl %SubModules%\opencv\bin\install\x64\vc15\bin %Output% opencv_world400*.*

