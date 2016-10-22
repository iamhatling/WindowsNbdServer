@if not exist bin mkdir bin
cl /Ox /Febin\WindowsNbdServer.exe /I. /D_MBCS /DLITTLE_ENDIAN ws2_32.lib SelectServer.cpp Nbd.cpp NbdServer.cpp Main.cpp
@if errorlevel 1 goto BUILD_FAILED

@echo BUILD SUCCESS
@goto EXIT

:BUILD_FAILED
@echo BUILD FAILED

:EXIT