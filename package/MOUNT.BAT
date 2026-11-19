@ECHO OFF
REM MOUNT - mount the network CD.
REM   MOUNT           use the first server that answers
REM   MOUNT a.b.c.d   use the server at this address
REM This PC gets its own address with DHCP.
REM You can run MOUNT again at any time. It loads only what is missing.
REM More LEADOUT options: SET LEADOUTOPT=/H:0 (for example) before MOUNT.

C:\LEADOUT\LEADOUT /CHECK > NUL
IF ERRORLEVEL 3 GOTO DONE
IF ERRORLEVEL 2 GOTO CDX
IF ERRORLEVEL 1 GOTO IP

ECHO Loading the packet driver...
REM The packet driver for this PC. Change this line for another network card.
C:\LEADOUT\E100BPKT 0x60
C:\LEADOUT\LEADOUT /CHECK > NUL
IF NOT ERRORLEVEL 1 GOTO NOPKT

:IP
SET MTCPCFG=C:\LEADOUT\MTCP.CFG
C:\UTILS\mTCP\DHCP
IF ERRORLEVEL 1 GOTO NODHCP
IF "%1"=="" GOTO ANY
C:\LEADOUT\LEADOUT /S:%1 %LEADOUTOPT%
IF ERRORLEVEL 1 GOTO END
GOTO CDX
:ANY
C:\LEADOUT\LEADOUT %LEADOUTOPT%
IF ERRORLEVEL 1 GOTO END

:CDX
C:\DOS\SHSUCDX /D:LEADOUT1
GOTO END

:DONE
ECHO The network CD is already mounted.
GOTO END
:NOPKT
ECHO.
ECHO The packet driver did not load. Check that the network card is
ECHO enabled in the BIOS, and that it has an IRQ.
GOTO END
:NODHCP
ECHO.
ECHO DHCP did not get an IP address. Check the network cable.
ECHO Without DHCP, give this PC's address to LEADOUT yourself:
ECHO   C:\LEADOUT\LEADOUT 192.168.1.50 /S:server
:END
