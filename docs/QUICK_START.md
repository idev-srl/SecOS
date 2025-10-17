```
==================================
   Kernel 64-bit con GRUB
==================================
[OK] Sistema pronto!

==================================
   Benvenuto in SecOS Shell!
==================================
Digita 'help' per vedere i comandi disponibili.
# Quick Start - SecOS Kernel

## 🚀 Setup & Build

### 1. Ensure required files exist

You should have these files in the same directory:
```
boot.asm
idt_asm.asm
kernel.c
idt.c
idt.h
timer.c
timer.h
keyboard.c
keyboard.h
shell.c
shell.h
terminal.h
linker.ld
Makefile
```

### 2. Build the kernel

```bash
# Clean previous build artifacts
make clean

# Build and create the ISO image
make iso

# Run in QEMU
make run
```

## 🎮 Use the shell

Once booted you will see:

```
=================================
    Kernel 64-bit with GRUB
=================================

Kernel started in Long Mode (64-bit)!
[OK] Multiboot bootloader detected
[OK] IDT initialization...
[OK] PS/2 keyboard initialization...
[OK] System ready!

=================================
    Welcome to SecOS Shell!
=================================

Type 'help' to list available commands.

secos$
```

### Available commands:

```bash
help      # Command list
info      # System info (includes timer frequency)
uptime    # Show uptime
echo test # Prints "test"
sleep 500 # Waits 500ms
colors    # Shows VGA colors
clear     # Clears screen
reboot    # Reboots
```

## ⏱️ Timer test

Try these commands to test the timer:

```bash
# Show current uptime
uptime

# Wait 1 second
sleep 1000

# Show uptime again (~1 second more)
uptime

# Test with different values
sleep 100   # 100ms
sleep 2000  # 2 seconds
sleep 5000  # 5 seconds
```

The timer is configured at **1000 Hz** (1 tick per millisecond), so you can:
- Measure time with millisecond precision
- Use `sleep` to create pauses
- See system uptime in hours:minutes:seconds

## 🐛 Troubleshooting

### Build error

```bash
# Reinstall required packages
sudo apt install nasm gcc binutils grub-common grub-pc-bin xorriso

# Retry
make clean && make iso
```

### QEMU window not opening (WSL2)

If using WSL2 and QEMU does not show a window:

1. Build in WSL2: `make iso`
2. Use VirtualBox on Windows with the ISO from `\\wsl$\Ubuntu-22.04\...\myos.iso`

### Keyboard not working

- Ensure QEMU window has focus
- Click inside QEMU window
- If using a VM ensure keyboard is captured

## 📝 Important notes

- Keyboard uses **US QWERTY** layout
- Supports **Shift** and **Caps Lock**
- **Backspace** works correctly
- Commands are **case-sensitive**
- Timer raises **1000 interrupts per second** (1ms per tick)
- `sleep` accepts **1 to 10000** milliseconds
- `uptime` shows time in **hours:minutes:seconds** format

## 🎯 Next steps

Now that you have a working kernel with shell and timer, you can:

1. **Cooperative multitasking** - Use timer for scheduling
2. **Heap allocator** - Implement kmalloc/kfree
3. **File system** - Start with a simple RAM FS
4. **Additional drivers** - Serial port for debug, PS/2 mouse
5. **Syscalls** - Create a user/kernel mode interface

Enjoy building your OS! 🚀
Divertiti a sviluppare il tuo OS! 🚀