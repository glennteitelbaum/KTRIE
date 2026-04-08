@echo off
setlocal

set EXE=out\build\x64-Release\bench_kntrie.exe
set OUTDIR=kntrie

echo Running u16...
%EXE% u16 i32 6000000 y > %OUTDIR%\chart16.html 2>%OUTDIR%\chart16.err

echo Running u32...
%EXE% u32 i32 6000000 y > %OUTDIR%\chart32.html 2>%OUTDIR%\chart32.err

echo Running u64...
%EXE% u64 i32 6000000 y > %OUTDIR%\chart64.html 2>%OUTDIR%\chart64.err

echo Done. Results in %OUTDIR%\
dir %OUTDIR%\bench_kntrie_*.txt