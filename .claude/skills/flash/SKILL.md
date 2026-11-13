---
name: flash
description: Flash or recover an elotto node (master, slave0..slave8) - node table with IP/MAC/COM, OTA commands, rev v3 builds, boot slots, USB recovery.
---

# Flashing elotto nodes

The root CLAUDE.md keeps the standing rules: build through `build.ps1`, never OTA into a running
session without asking, poll `fw_sha` after every OTA.

## Nodes
| node | IP | MAC | COM | flash contents |
|------|----|-----|-----|----------------|
| master | 192.168.178.100 | 80:f1:b2:d2:e3:1d | COM4 | factory = updater, ota_0/ota_1 = elotto app · IMX219 |
| slave0 | 192.168.178.103 (static lease) | 80:f1:b2:d2:e3:e5 | — | factory = updater, ota_0 = slave app · IMX219 |
| slave1 | 192.168.178.145 (static lease) | e8:f6:0a:e0:ce:a8 | — | factory = updater, ota_0/ota_1 = slave app · IMX219 |
| slave2 | 192.168.178.155 | e8:f6:0a:e0:c7:a1 | — | factory = updater, ota_0 = slave app · IMX219 |
| slave3 | 192.168.178.83 (static lease) | e8:f6:0a:e0:d9:53 | COM10 | factory = updater, ota_0 = slave app · IMX219 |
| slave4 | 192.168.178.172 (static lease) | 80:f1:b2:d6:58:80 | COM11 | factory = updater, ota_0 = slave app · IMX219 · **chip rev v3.2** |
| slave5 | 192.168.178.166 (static lease) | 80:f1:b2:d6:58:81 | COM3 | factory = updater, ota_0 = slave app · IMX219 · **chip rev v3.2** |
| slave6 | 192.168.178.173 (static lease) | 80:f1:b2:d6:5a:1a | COM4 | factory = updater, ota_0 = slave app · IMX219 · **chip rev v3.2** |
| slave7 | 192.168.178.32 (static lease) | e8:f6:0a:e7:fe:13 | COM6 | factory = updater, ota_0 = slave app · IMX219 · **chip rev v3.2** |
| slave8 | 192.168.178.181 (static lease) | e8:f6:0a:e8:15:94 | COM9 | factory = updater, ota_0 = slave app · IMX219 · **chip rev v3.2** |

⚠ The updater starts its web server only if it gets an IP within 30 s of boot — plug LAN before power, else power-cycle.
An image of the wrong revision is refused by the bootloader. Read the revision before flashing a
new board (`esptool chip-id`).
⚠ **COM ports are not stable** — list the ports before an `erase-flash`; a wrong port wipes a
working node.
Addresses are informational: the master finds slaves by UDP broadcast.

## Build for rev v3.x boards
```powershell
# rev v3.x boards (slave4): overlay sdkconfig.defaults.rev3, own build dir and sdkconfig
$s='D:\E-Lotto\elotto_slave'; .\build.ps1 -C $s -B "$s\build_rev3" -D "SDKCONFIG=$s\sdkconfig.rev3" -D "SDKCONFIG_DEFAULTS=$s\sdkconfig.defaults;$s\sdkconfig.defaults.rev3" build
# same pattern with -C ota_firmware for the rev3 updater
```

**Flash over Ethernet, not USB:**

```powershell
curl http://192.168.178.100/update --data-binary @build/elotto.bin                        # master
curl http://192.168.178.103/update --data-binary @../elotto_slave/build/elotto_slave.bin  # slave
curl http://192.168.178.172/update --data-binary @../elotto_slave/build_rev3/elotto_slave.bin  # slave4 (rev v3)
```

~750 KB in ~3 s. The node writes the *inactive* slot, reboots, and marks itself valid only once its
webserver answers, so a failed transfer cannot strand it. `POST /update?slot=0|1` targets a slot;
`POST /boot?slot=N` selects the next boot slot and clears the fail counter — that is how a node is
put back on a known-good image without USB.

- ⚠ **A node that pings but refuses port 80 is not dead** — check `/otainfo` before USB.
  ⚠ An *immediate* close on every endpoint (connect succeeds, no response) is socket-table
  eviction, not a crash. To prove the array is still measuring, watch a slave's `/camlog`
  `tag` advance — the ring is under **`win`**, and `measuring` on `/diag` is the ~2 s window flag,
  not a session flag.
- **USB is only for** a fresh board or a node whose recovery updater is gone:
  `.\build.ps1 -C ota_firmware -p COMx erase-flash`, then `... -p COMx flash` (a rev v3 board:
  `esptool write-flash "@flash_args"` from `ota_firmware/build_rev3`). The updater logs `link up`,
  the PHY id and BMSR on the console; no `link up` = cable/port, not firmware.
