# Running SECoS on VMware or a physical PC

SECoS boots via **UEFI**. This guide builds a bootable disk image and runs it on
QEMU (sanity check), **VMware**, or a **physical PC**.

> **What you get:** the kernel boots to the **interactive SecOS shell** on the
> screen (UEFI GOP framebuffer) with PS/2 keyboard input. Type `help`.
>
> **Disks (M21):** SECoS now has an **AHCI/SATA** driver, so a **SATA** disk works
> as `/mnt` on VMware and on physical PCs with SATA. Attach the data disk as
> **SATA** (see §3). NVMe-only machines aren't supported yet (NVMe driver is a
> future milestone); for those, `vls /mnt` is empty but the RAMFS (`rf*`) and
> everything else still work.
>
> **Secure Boot must be OFF** — the loader is not signed for the Microsoft chain.

## 0. Prerequisites (build host)

Debian/Ubuntu/WSL packages: `gnu-efi ovmf mtools gdisk dosfstools qemu-utils`
(plus the normal toolchain). Check: `sgdisk`, `mkfs.fat`, `mcopy`, `qemu-img`.

## 1. Build the bootable image

```bash
make uefi-disk        # -> secos-uefi.img   (raw GPT+ESP boot disk; for USB / QEMU)
make uefi-vmdk        # -> secos-uefi.vmdk  (also builds the .img; for VMware)
make data-vmdk        # -> data.vmdk        (FAT32 DATA disk for /mnt; for VMware)
```

`secos-uefi.*` is the **boot** disk: an EFI System Partition (FAT32) with
`/EFI/BOOT/BOOTX64.EFI` (the loader) + `/kernel.elf`. `data.vmdk` is a separate
**data** disk (a raw FAT32 volume with a `HELLO.TXT`) that SECoS mounts at `/mnt`
through its AHCI driver.

## 2. Sanity-check locally (QEMU + OVMF)

```bash
make run-uefi-disk     # boots secos-uefi.img in QEMU+OVMF, serial on stdio
```
You should see the `secos$` shell prompt on the serial console. Exit QEMU with
`Ctrl-A` then `X`. If this works, the image is good for VMware / hardware.

## 3. VMware (Workstation / Player / Fusion)

1. Copy `secos-uefi.vmdk` to your host (from WSL: it's under your repo dir;
   e.g. `cp secos-uefi.vmdk /mnt/c/Users/<you>/SecOS/`).
2. **Create a new VM** → *I will install the OS later* → Guest OS: *Other / Other
   64-bit*.
3. **Firmware type = UEFI** (VM Settings → Options → Advanced → Firmware type:
   UEFI). **Disable Secure Boot** (uncheck the Secure Boot box).
4. **Remove the default disk** the wizard created, then **Add → Hard Disk →
   SATA → Use an existing virtual disk** → select `secos-uefi.vmdk` (keep format).
   *Use the SATA controller type* (not SCSI/NVMe).
5. **For working `/mnt`: Add a second Hard Disk → SATA → Use an existing virtual
   disk → `data.vmdk`.** SECoS's AHCI driver finds it and mounts it at `/mnt`.
   (The boot disk is skipped automatically — it's a GPT/ESP, not a raw FS.)
6. Memory: 256 MB+ is plenty. **Power on.** The SecOS shell appears; try
   `vls /mnt` and `vcat /mnt/HELLO.TXT`.

Tip — capture output to a file: add a **Serial Port → Output to file** in VM
settings; SECoS also writes the shell to COM1.

> **Disk requirements:** use the **SATA** controller (AHCI). SCSI/NVMe/PVSCSI are
> not supported yet. The data disk must be a **raw FAT32 (or ext2/ext4) volume**
> with no partition table — `make data-vmdk` produces exactly that. A blank VMware
> disk you create yourself will be unformatted and won't mount.

## 4. Physical PC (USB stick)

> This **erases the target USB stick.** Triple-check the device name.

```bash
lsblk                                   # find your USB, e.g. /dev/sdX (NOT a partition)
sudo dd if=secos-uefi.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```
(On Windows, write `secos-uefi.img` with **Rufus** in *DD image* mode, or
**balenaEtcher**.)

Then on the target PC:
1. Enter firmware setup (F2/F10/Del/Esc at power-on).
2. **Disable Secure Boot.** Ensure **UEFI boot** (not Legacy/CSM-only).
3. Boot from the USB stick (boot menu, often F12).
4. The SecOS shell appears on the monitor; use the keyboard (`help`).

For a working `/mnt` on a physical PC: connect a **SATA** drive whose whole
device is a raw FAT32 (or ext2/ext4) volume — no partition table. You can make one
on Linux with `mkfs.fat -F 32 /dev/sdX` (⚠️ erases the drive), then copy files with
`mtools`. SECoS's AHCI driver will mount it at `/mnt`. NVMe-only laptops need the
(future) NVMe driver.

## Troubleshooting

- **Drops to the UEFI shell / "no bootable device":** the firmware didn't find
  `\EFI\BOOT\BOOTX64.EFI`. Re-verify the image (`make run-uefi-disk` works), make
  sure Secure Boot is off, and that you booted the disk in **UEFI** mode.
- **Black screen, no shell:** some firmwares don't give a GOP framebuffer SECoS
  can use. Capture COM1 (VMware: serial-to-file) to confirm it actually booted —
  you should still see the `secos$` banner there.
- **`/mnt` empty / no disk:** the data disk must be on a **SATA** controller
  (AHCI) and be a **raw FAT32/ext2/ext4** volume (no partition table) — use
  `make data-vmdk`. SCSI/NVMe controllers and unformatted/partitioned disks won't
  mount yet. The RAMFS (`rfls`, `rfwrite`, `rfcat`, …) always works.
- **OVMF + a second disk under `-bios OVMF.fd`** can be flaky (non-persistent
  NVRAM). For QEMU testing, the single-disk `make run-uefi-disk` is the reliable
  path; split `OVMF_CODE`/`OVMF_VARS` if you need persistence.

## How it's built (reproducible)

`tools/mkuefidisk.sh` (invoked by `make uefi-disk`) creates the image with no root:
`mkfs.fat` makes a FAT32 ESP, `mmd`/`mcopy` populate it, `sgdisk` writes a GPT with
one `EF00` (ESP) partition at sector 2048, and `dd` places the filesystem there.
`make uefi-vmdk` converts the raw image with `qemu-img`.
