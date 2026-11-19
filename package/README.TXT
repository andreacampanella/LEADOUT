LEADOUT 0.16 - network CD-ROM for DOS
=====================================

LEADOUT lets this PC use CD images (ISO, BIN/CUE, MDF/MDS) from a LEADIN
board on the network, with CD audio. LEADIN is the firmware for an ESP32
Audio Kit. You change discs in a web browser.

FILES (copy this folder to C:\LEADOUT)

  MOUNT.BAT     mounts the network CD. Use this.
  LEADOUT.COM   the network CD driver
  E100BPKT.COM  packet driver for the onboard Intel network controller
  MTCP.CFG      network settings (mTCP DHCP writes the address here)
  CDPLAY.COM    plays CD audio tracks, for a test without a game
  CDSPEED.COM   measures the read speed of the CD

  Also needed: mTCP in C:\UTILS\mTCP, and SHSUCDX in C:\DOS.

MOUNT THE NETWORK CD

  C:\LEADOUT\MOUNT

  MOUNT loads the packet driver, gets an IP address with DHCP, starts
  LEADOUT, and gives the CD a drive letter with SHSUCDX. You can run
  MOUNT again at any time: it loads only what is missing.

  With two boards on the network, or if MOUNT does not find the board,
  give the address of the one you want:
    C:\LEADOUT\MOUNT 192.168.1.40

  Without DHCP, give this PC's address to LEADOUT yourself, after the
  packet driver is loaded:
    C:\LEADOUT\LEADOUT 192.168.1.50 /S:192.168.1.40
    C:\DOS\SHSUCDX /D:LEADOUT1

  To mount the CD at every start, add this line at the end of
  C:\AUTOEXEC.BAT:
    CALL C:\LEADOUT\MOUNT

  For another network card, change the E100BPKT line in MOUNT.BAT.

THE BOARD

  The board is an Ai-Thinker ESP32 Audio Kit V2.2 with the LEADIN
  firmware. LEADIN is a separate release. Start with its FLASH.TXT. The
  board holds the images on its SD card and plays the CD audio on its
  headphone jack. Connect this PC's sound card LINE OUT to the board's
  LINE IN, and the board's EARPHONES jack to your speakers.

  The web UI is at http://leadin.local/ or at the board's address.
  Click "Insert" next to a disc. You can change the disc at any time,
  also while a game runs.

  A game can set the music volume, even to 0. The web UI then shows
  "the game set the music volume to ...". Turn on "Override" in the
  web UI to ignore the game volume and use only the slider.

TEST THE SPEED

  CDSPEED reads 2 MB of raw sectors from the CD and prints the speed:

    CDSPEED           2 MB
    CDSPEED 8         8 MB, for a steadier number

  150 KB/s is 1x. The Activity pane of the web UI shows the same figure
  from the board's side.

TEST THE MUSIC

  C:\LEADOUT\CDPLAY STATUS    track list and position
  C:\LEADOUT\CDPLAY 2         play track 2
  C:\LEADOUT\CDPLAY STOP      pause (a second STOP clears it)
  C:\LEADOUT\CDPLAY RESUME    continue

LEADOUT COMMAND LINE

  LEADOUT [a.b.c.d] [/S:a.b.c.d] [/D:name] [/B:n] [/C:n] [/H:0]
  LEADOUT /CHECK
  LEADOUT /DISC[:n]

  a.b.c.d  IP address of this PC. Without it, LEADOUT reads the IPADDR
           line from the file that MTCPCFG points to.
  /S:a.b.c.d  use only the server at this address. Without it, LEADOUT
           uses the first server that answers. MOUNT a.b.c.d does this.
  /D:name  device name (default LEADOUT1)
  /B:n     sectors per network request, 1 to 8 (default 8). Use 4 on an
           ISA card that drops packets in bursts
  /C:n     read-ahead buffer, 0 to 8 sectors (default 8, 16 KB). A small
           read fetches a whole block, and the next reads come from the
           buffer. /C:0 turns it off and saves 16 KB of memory
  /H:0     no heartbeat. Use it when videos or sound freeze in a game
           that plays CD music, on a PCI sound card. The board then stops
           the CD music when this PC is silent for the auto stop time
  /DISC    lists the discs on the server and inserts the one you
           choose. /DISC:n inserts disc n, /DISC:0 ejects. Run it
           after MOUNT. SHSUCDX sees the new disc at once.
  /CHECK   only reports what is loaded, as the exit code:
           0 = nothing, 1 = packet driver, 2 = LEADOUT,
           3 = LEADOUT and SHSUCDX or MSCDEX

  Exit codes of LEADOUT: 0 = installed, 1 = wrong command line or no IP
  address, 2 = no packet driver, 3 = no server, 4 = already loaded.

CHANGE THE DISC FROM DOS

  C:\LEADOUT\LEADOUT /DISC

  Shows the discs on the server, 20 per page. Type a number to insert
  that disc, N or P for the next or previous page, E to eject.

MUSIC STOPS WHEN THE PC IS OFF

  When a game starts the music, LEADOUT sends a short message to the
  server every 5 seconds. If the PC crashes or is switched off, the
  server stops the music after the client timeout (15 seconds, set in
  the web UI). Music started from the web UI keeps playing.

NOTES

  - While LEADOUT is loaded, mTCP programs (PING, DHCP) cannot use the
    network, because LEADOUT uses the packet driver. Use them before
    MOUNT.
  - Type PING without a path, and DOS runs the old Windows PING from
    C:\WINDOWS. Use C:\UTILS\mTCP\PING, or put C:\UTILS\mTCP first
    in the PATH line of AUTOEXEC.BAT.
  - LEADOUT cannot be unloaded. Restart the PC to remove it.
  - A disc change from the web UI shows in DOS after 7 seconds without
    a CD access (SHSUCDX checks only then, as with a real drive).
    LEADOUT /DISC shows it at once.
