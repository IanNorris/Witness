@echo off
setlocal

if "%~1"=="" (
    echo Usage: Deploy.bat ^<target_folder^>
    echo Example: Deploy.bat \\SERVER\Share\Witness
    exit /b 1
)

set TARGET=%~1
set SOURCE=X:\Programming\Witness\build\bin

echo Deploying from %SOURCE% to %TARGET%

REM Copy executables, DLLs, PDBs (exclude the massive CUDA provider if not needed)
robocopy "%SOURCE%" "%TARGET%" *.exe *.dll *.pdb *.cnf /XF onnxruntime_providers_cuda.dll /R:1 /W:1 /NJH /NJS /NDL /NP

REM Copy Web folder (real files, recursive)
robocopy "%SOURCE%\Web" "%TARGET%\Web" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP

REM Copy models
robocopy "%SOURCE%\models" "%TARGET%\models" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP

REM Copy notification sounds
robocopy "%SOURCE%\NotificationSounds" "%TARGET%\NotificationSounds" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP

REM Copy CUDA provider separately (huge file, skip if target already has it and same size)
robocopy "%SOURCE%" "%TARGET%" onnxruntime_providers_cuda.dll /R:1 /W:1 /NJH /NJS /NDL /NP /XO

echo.
echo Deploy complete.
