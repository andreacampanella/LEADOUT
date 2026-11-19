# LEADIN protocol, version 2

LEADOUT, the DOS client, reads a CD image that a LEADIN server on the local network holds.
LEADOUT is a TSR that acts as a CD-ROM device driver for SHSUCDX or MSCDEX.

## Transport

- UDP over IPv4. The server and the client both use port 2352.
- The client sends DISCOVER as a broadcast to 255.255.255.255.
- All other messages are unicast.
- Each UDP payload is at most 1036 bytes. This fits in one Ethernet frame, so IP fragmentation does not occur.
- The client answers ARP requests for its own IP address. The client learns the MAC address of the server from the OFFER packet, so the client never sends ARP requests.

## Byte order

- All multi-byte fields in the payload are little-endian.
- The server copies the `seq` field from a request into each reply without a change.

## Header (8 bytes)

| Offset | Size | Field  | Value |
|-------:|-----:|--------|-------|
| 0      | 4    | magic  | `N` `C` `D` `0x01` |
| 4      | 1    | type   | message type |
| 5      | 1    | status | 0 = OK, other values = error |
| 6      | 2    | seq    | request number, set by the client |

## Messages

### 0x01 DISCOVER (client to broadcast)

No body. Each server on the network replies with OFFER. The client uses the first OFFER that it gets.

### 0x81 OFFER (server to client)

| Offset | Size | Field |
|-------:|-----:|-------|
| 8      | 16   | server name, ASCII, padded with zero bytes |
| 24     | 4    | volume size in sectors (the lead-out LBA) |
| 28     | 1    | first track number |
| 29     | 1    | last track number |
| 30     | 1    | number of track entries (N), 0 = no disc |
| 31     | 1    | disc ID (version 0.5): changes each time the server changes the disc |
| 32     | 4    | lead-out address, Red Book format: frame, second, minute, 0 |
| 36     | 6×N  | track entries |
| 36+6×N | 1    | protocol version (version 2 and later). A version 1 server sends nothing here; the client reads 0 and treats it as 1 |

Each track entry has 6 bytes:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0      | 1    | track number |
| 1      | 1    | control and ADR: `0x41` = data track, `0x01` = audio track |
| 2      | 4    | start address, Red Book format: frame, second, minute, 0 |

The Red Book addresses already include the 150-frame (2-second) offset. The client copies them to the MSCDEX IOCTL replies without a change.

### 0x02 READ (client to server)

| Offset | Size | Field |
|-------:|-----:|-------|
| 8      | 4    | first sector (LBA, the same as the HSG sector number) |
| 12     | 1    | number of sectors, 1 to 8 |
| 13     | 2    | part mask (optional): bit `p` set = send part `p` |
| 15     | 1    | disc ID (optional, version 0.5): the disc ID from the last OFFER |

The server replies with two DATA packets for each sector. If the part mask is present, the server sends only the parts whose bits are set. If the request has no part mask, the server sends all parts.

### 0x82 DATA (server to client)

| Offset | Size | Field |
|-------:|-----:|-------|
| 8      | 1    | part number, 0 to 2×count−1 |
| 9      | 1    | number of parts (2×count) |
| 10     | 2    | reserved, 0 |
| 12     | 1024 | data |

Part `p` holds bytes `p×1024` to `p×1024+1023` of the requested data, in cooked (2048-byte) sector format.

If the server cannot read the sectors, it sends one DATA packet with an error `status` and no data:

| Status | Meaning |
|-------:|---------|
| 1 | read error (for example a sector past the end of the disc) |
| 2 | the disc changed: the disc ID in the READ request is not the current disc ID |
| 3 | no disc |

### Audio messages (version 0.4)

The server plays audio tracks on its own audio output. The client sends only control messages. Each request gets one reply. The reply type is the request type + 0x80. `status` = 0 means OK.

| Type | Name | Request body | Reply body |
|-----:|------|--------------|------------|
| 0x03 | PLAY | start LBA (4), number of sectors (4) | none |
| 0x04 | STOP | none | none |
| 0x05 | RESUME | none | none |
| 0x06 | AUDIO STATUS | none | see below |
| 0x07 | VOLUME | left (1), right (1), 0 to 255 | none |
| 0x08 | MEDIA | none | disc ID (1), disc present (1) |

PLAY stops any audio that plays, then plays the sectors from `start` to `start + count − 1`. A count of 0 only stops the audio. Sectors that are not audio play as silence.

STOP follows the MSCDEX rules. If audio plays, STOP pauses it and keeps the position. If audio is paused, STOP clears the pause, and RESUME is then not possible.

RESUME continues a paused play. If no play is paused, the server replies with `status` = 1.

AUDIO STATUS reply body:

| Offset | Size | Field |
|-------:|-----:|-------|
| 8      | 1    | state: 0 = stopped, 1 = playing, 2 = paused |
| 9      | 1    | reserved, 0 |
| 10     | 4    | current position (LBA) |
| 14     | 4    | start of the last play, or the resume position when paused (LBA) |
| 18     | 4    | end of the play (LBA of the first sector after it) |

The server keeps the time itself: the position moves at 75 sectors per second.

### Version 2 messages

| Type | Name | Request body | Reply body |
|-----:|------|--------------|------------|
| 0x09 | HEARTBEAT | none | none |
| 0x0A | LIST | first index (2) | total (2), first index (2), current index (2, 0xFFFF = none), count (1), then `count` entries: length (1) + name (ASCII, at most 63 bytes) |
| 0x0B | SELECT | index (2), 0xFFFF = eject | none; `status` 1 = no such entry or the image cannot be loaded |
| 0x0C | ANNOUNCE | none | none. Sent by the server, as a broadcast, every second. `seq` is 0. |

- LIST returns at most 20 entries. The index is the position in the library sorted by name, case ignored. It changes when files are added or removed, so a client lists again before it selects.
- After SELECT, the disc ID changes, as with a disc change in the web UI.
- ANNOUNCE lets a client find the server when the client's own broadcasts do not reach it. A Wi-Fi access point often drops broadcasts from the cable to its wireless clients, but passes broadcasts from them. A client that gets no OFFER takes the server's address from an ANNOUNCE and sends its DISCOVER to that address directly. A client ignores ANNOUNCE at every other time.

### Client liveness (version 2)

The server records who started the current play: a client (PLAY) or the web UI.

- A play started by a client, playing or paused, needs a message from that client's IP address at least every *client timeout* seconds (default 15, set in the web UI; 0 = never). Every protocol message counts.
- While such a play is active, the client sends HEARTBEAT every 5 seconds, from its timer interrupt, so that a game that does not ask for the audio status still counts as alive.
- On a timeout, the server stops the audio and clears the pause.
- A play started from the web UI is never stopped by the timeout. A play or stop from the web UI replaces a play started by a client.

### Disc change (version 0.5)

The server can change the disc at any time (in the web UI). Then:

1. The server increments the disc ID, and stops any audio.
2. When DOS asks the client if the media changed (IOCTL 9), the client sends MEDIA, at most once per timer tick. If the disc ID differs from the one that the client has, the client sends DISCOVER to the server (unicast) to get the new OFFER, and reports "media changed" to DOS.
3. A READ request with an old disc ID gets status 2. The client reports "invalid disk change" to DOS and checks the disc at the next request.

If no disc is selected, OFFER has 0 tracks, the client reports "no disc" in the device status, and reads fail with "drive not ready".

## Loss and retries

- The client waits about 165 ms for all parts of a READ reply.
- If parts are missing, the client sends the READ again with the same `seq`. The part mask then asks only for the missing parts. The client keeps the parts that it already has.
- A network card can hold only a small number of frames in its receive buffer. For example, an NE2000 holds about 11 full-size frames. The client requests 8 sectors (16 frames) by default. `/B:4` halves that for a card with a small receive ring.
- The client tries 6 times. Then it reports a read error to DOS.
- A lost PLAY reply makes the client send PLAY again. The server then starts the same play again, which the listener does not notice.
- The client ignores replies with a different `seq`.

## Limits

- The server does not keep sessions. It answers each request from any client.
- The client does not run DHCP itself. It takes its address from the command line or from mTCP's configuration file.
