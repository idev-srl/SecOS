# Contributing to SecOS

Thank you for your interest in contributing! This document outlines the workflow, coding style, and guidelines to keep the project consistent and secure.

## Table of Contents
1. Code of Conduct
2. Getting Started
3. Build & Run
4. Branching & Commits
5. Coding Style
6. Memory & Security Practices
7. Adding Features
8. Documentation Standards
9. Testing Guidelines (Future)
10. License & Liability

## 1. Code of Conduct
Be respectful, constructive and clear. Avoid introducing third-party code without verifying license compatibility (MIT preferred).

## 2. Getting Started
Clone the repo and ensure required toolchain:
- NASM
- GCC (x86_64, freestanding)
- binutils (ld)
- GRUB utilities (grub-mkrescue)
- xorriso
- QEMU (recommended for testing)

Install on Debian/Ubuntu:
```bash
sudo apt update
sudo apt install nasm gcc binutils grub-common grub-pc-bin xorriso qemu-system-x86
```

## 3. Build & Run
```bash
make            # builds kernel.bin
make iso        # builds bootable myos.iso
make run        # build + iso + run in QEMU
make clean      # remove artifacts
```

## 4. Branching & Commits
- Create topic branches: `feat/vmm-demand-zero`, `fix/heap-coalesce-bounds`, `docs/glossary-update`.
- Keep commits focused; one logical change per commit.
- Use imperative commit messages:
  - `Add demand-zero allocation for user space`
  - `Fix off-by-one in heap split logic`
  - `Document driver syscall audit filtering`

## 5. Coding Style
- C99 (freestanding). Avoid compiler-specific extensions unless guarded.
- Indentation: 4 spaces, no tabs.
- Brace style: K&R (opening brace on same line for functions and control blocks).
- Prefer explicit types (`uint64_t`) over generic (`unsigned long`).
- Avoid magic numbers: define constants or enums.
- Keep functions short; if >150 lines consider refactoring.
- Prefix internal static helpers with a clear domain (e.g., `vmm_`, `pmm_`, `drv_`).

## 6. Memory & Security Practices
- Enforce W^X: never map writable and executable simultaneously.
- Use NX on data and stack regions.
- Keep user/kernel separation: never set USER bit for kernel pages.
- Validate all ELF segment alignment (p_align == 0 or 0x1000).
- Track allocated pages per process for precise unload and accounting.
- Use guard pages for user stacks.

## 7. Adding Features
1. Open an issue describing motivation and design sketch.
2. Discuss any page table, privilege or syscall additions (security impact).
3. Include minimal tests or a shell command to exercise the feature.
4. Update relevant documentation (`README.md`, `MEMORY.md`, `DRIVER_IF.md`).

## 8. Documentation Standards
- Write in English.
- Keep glossary terms short and precise.
- Provide usage examples for shell commands and new APIs.
- Use fenced code blocks for C snippets and shell commands.

## 9. Testing Guidelines (Future)
- Planned: lightweight unit tests for allocator (frame counts, coalescing), ELF loader (segment permissions), driver syscall permission matrix.
- For now, provide a reproducible shell script or sequence of shell commands demonstrating correctness.

## 10. License & Liability
SecOS is distributed under the MIT License (see `LICENSE.md`). iDev srl and contributors provide the software "as is" without warranty of any kind. You are solely responsible for any use, modification, or distribution of this code.

## Pull Request Checklist
- [ ] Builds successfully (`make`)
- [ ] No new warnings (enable `-Wall -Wextra` locally if possible)
- [ ] Documentation updated
- [ ] No mixed Italian/English text
- [ ] Commit messages follow style
- [ ] Security considerations noted (if applicable)

## Contact
For architectural questions or security concerns, open an issue or start a discussion thread.
