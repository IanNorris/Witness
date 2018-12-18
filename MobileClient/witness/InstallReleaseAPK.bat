call %~dp0\BuildRelease.bat
"%ANDROID_HOME%\platform-tools\adb.exe" install "%~dp0\build\app\outputs\apk\release\app-release.apk"
pause