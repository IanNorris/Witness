del x64\Release\*Avalon*
del x64\Release\*.exp
del x64\Release\*.ilk
del x64\Release\*.lib
del x64\Release\*.ipdb
del x64\Release\*.iobj
del x64\Release\*.pdb
del x64\Release\*.exp
del x64\Release\opencv_world400d.dll
del x64\Release\Installer.exe.config

rmdir x64\Release\de /S /Q
rmdir x64\Release\es /S /Q
rmdir x64\Release\fr /S /Q
rmdir x64\Release\hu /S /Q
rmdir x64\Release\it /S /Q
rmdir x64\Release\pt-BR /S /Q
rmdir x64\Release\ro /S /Q
rmdir x64\Release\ru /S /Q
rmdir x64\Release\sv /S /Q
rmdir x64\Release\zh-Hans /S /Q
rmdir x64\Release\x86 /S /Q

mkdir x64\Release\Web

robocopy /njh /njs /np /nfl /ndl /S WitnessServer\Web x64\Release\Web\
robocopy /njh /njs /np /nfl /ndl /S ThirdParty\WACS x64\Release\WACS\