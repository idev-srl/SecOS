# 🛣️ SecOS Development Roadmap

*Last updated: 2025/10/17*

This roadmap outlines the planned features and milestones for the development of **SecOS**, a minimal secure operating system kernel built from scratch.

---

## ✅ Current Status

- [x] Bootloader integration with GRUB
- [x] 64-bit long mode kernel entry
- [x] VGA text mode output
- [x] PS/2 keyboard input handling
- [x] Custom basic shell environment
- [x] ELF loader for user programs
- [x] Basic heap memory allocator

---

## 🔜 In Progress

- [ ] Virtual memory management (paging)
- [ ] Physical memory manager
- [ ] Syscall interface
- [ ] Timer interrupts
- [ ] Basic process scheduler (round-robin)

---

## 🎯 Planned Features

### 🔐 Security & Isolation
- [ ] Process isolation and memory protection
- [ ] User/kernel privilege separation
- [ ] FIPS-compliant crypto module (future phase)

### 🧠 System Components
- [ ] VFS (Virtual File System) layer
- [ ] Basic filesystem driver (e.g., FAT or ext2)
- [ ] Device drivers: serial, mouse, disk

### 🧰 Development & Tooling
- [ ] GDB stub or debug interface
- [ ] QEMU integration with snapshot support
- [ ] Build system improvements (Make → CMake or Ninja)

---

## 📅 Long-Term Vision

- [ ] Multi-process support
- [ ] Networking stack (TCP/IP minimal)
- [ ] Modular services (daemon-style processes)
- [ ] Remote control via web UI over serial/IP
- [ ] Hardened minimal Linux-like shell for embedded systems
- [ ] Support for real-world deployment on industrial hardware

---

## 🤝 How to Contribute

If you'd like to contribute to SecOS, please refer to [CONTRIBUTING.md](CONTRIBUTING.md). Issues and suggestions are welcome!

---



