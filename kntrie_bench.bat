@echo off
setlocal

set EXE=out\build\x64-Release\bench_kntrie.exe
set OUTDIR=kntrie
REM set N=100000000
set N=2000000

echo Running u16...
REM %EXE% u16 i32 %N% y > %OUTDIR%\chart16q.html 2>%OUTDIR%\chart16q.err

echo Running u32...
REM %EXE% u32 i32 %N% y > %OUTDIR%\chart32q.html 2>%OUTDIR%\chart32q.err

echo Running u64...
%EXE% u64 i32 %N% y > %OUTDIR%\chart64q.html 2>%OUTDIR%\chart64q.err

echo Done. Results in %OUTDIR%\
dir %OUTDIR%\chart*.*