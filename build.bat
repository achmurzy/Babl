@echo off

IF NOT EXIST build mkdir build
pushd build
REM Check the command in shell.bat if you want to compile in 32bits (x86) instead - see Handmade ep16 for the rabbithole on that 
REM -nologo gets rid of annnoying proprietary info banner on the compiler
REM WX generates errors for warnings
REM W4 level 4 warnings are informational, as opposed to more critical warnings at levels 1, 2, 3
REM wdXXXX blacklists a specific compiler warning
REM Oi generate compiler intrinsics (basically inlining assembly code for library functions)
REM /GR disables C++ run-time type inference (Casey would say that we always know what data we are working with)
REM -EHa disables exception handling - which generates extra stack memory that may or may not make things faster
REM -Fm win32_BABL.map

set CompilerFlags= -nologo -WX -W4 -wd4100 -wd4201 -Oi -EHa -DBABL_INTERNAL=1 -DBABL_SLOW=1 -DBABL_WIN32=1 /FC /Zi /GR /Fmwin32_babl.map
set LinkerFlags= -opt:ref user32.lib Gdi32.lib Winmm.lib

del *.pdb > NUL 2> NUL
cl  %CompilerFlags% ../babl.cpp -LD /link -incremental:no -opt:ref -PDB:babl_%random%.pdb /EXPORT:GameUpdateAndRender /EXPORT:GameGetSoundSamples
cl  %CompilerFlags% ../win32_babl.cpp /link %LinkerFlags%
popd