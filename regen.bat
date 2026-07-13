@echo off
REM Regenerate the recompiled C from the ROM + game.toml (engine rka/ dir).
REM Needs rka.bin next to game.toml (gitignored; bring your own dump).
setlocal
set "ENGINE=%~dp0segagenesisrecomp"
if exist "%~dp0engine-local" set "ENGINE=%~dp0engine-local"
pushd "%ENGINE%\rka"
"..\recompiler\build\Release\GenesisRecomp.exe" rka.bin --game game.toml
popd
