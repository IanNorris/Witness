cd /d %~dp0
mkdir Data\Cascades
xcopy /D /Y ..\ThirdParty\SubModules\mallick_cascades\lbpcascades\*face*.xml Data\Cascades
xcopy /D /Y ..\ThirdParty\SubModules\mallick_cascades\haarcascades\*face*.xml Data\Cascades
xcopy /D /Y ..\ThirdParty\SubModules\opencv\bin\install\etc\haarcascades\*body*.xml Data\Cascades
xcopy /D /Y ..\ThirdParty\SubModules\opencv\bin\install\etc\haarcascades\*improved*.xml Data\Cascades
xcopy /D /Y ..\ThirdParty\SubModules\opencv\bin\install\etc\lbpcascades\*improved*.xml Data\Cascades
pause