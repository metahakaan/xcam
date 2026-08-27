@echo off
rem Moves a loaded copy of the filter out of the linker's way.
rem
rem Anything that has ever enumerated cameras keeps xcam-dsfilter.dll loaded --
rem Chrome and NVIDIA Broadcast do it merely by running -- and Windows refuses to
rem overwrite a module in use. It does allow renaming one, and the registered
rem path is what matters, so processes still holding the old file carry on with
rem the renamed copy until they unload.
rem
rem Everything here is best effort: on a first build there is nothing to move,
rem and a cast-off that is itself still loaded cannot be deleted. Neither should
rem fail the build, hence the unconditional success at the end.

setlocal
cd /d "%~1" || exit /b 0

for %%f in (xcam-dsfilter.*.old.dll) do @del /q "%%f" >nul 2>&1

if exist xcam-dsfilter.dll (
    ren xcam-dsfilter.dll "xcam-dsfilter.%RANDOM%.old.dll" >nul 2>&1
)

exit /b 0
