# OSIF Port Runbook — netbsd

> Canonical NetBSD backend in `../Jash/c/osif/netbsd/`. Copied from
> `macos/` (BSD libc shape), adapted to NetBSD 11. `beyondbsd/` is legacy
> (blibc freestanding) and NOT used for stock NetBSD.

## 0. The contract

Two files:
- `syscall.c` — L0: Linux-NR → NetBSD libc. No raw traps.
- `osinfo.c` — L1: 8 providers via sysctl/getmntinfo/getifaddrs.
  Any subset may return -1; the shell degrades gracefully.

## 1. Gate 0: it BOOTS (on NetBSD)

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jash
./build/jash -c 'echo ok'
./build/jash -c 'osif check'
```

## 2. Providers (all wired, verify on real NetBSD 11)

1. **SYSID** — uname(3)
2. **PROC** — sysctl CTL_KERN/KERN_PROC2 (no -lkvm)
3. **MEM** — hw.physmem64 + vm.uvmexp2
4. **MOUNTS** — getmntinfo(struct statvfs)
5. **NET** — getifaddrs(AF_LINK) + struct if_data
6. **UPTIME** — kern.boottime
7. **LOADAVG** — getloadavg(3)
(SPAWN is POSIX-common.)

## 3. Known traps

- `struct statvfs` on NetBSD (not `struct statfs`).
- `kinfo_proc2` needs `mib[6]` with `sizeof(kinfo_proc2)` in slot 4.
- `p_vm_rssize`/`p_vm_vsize` are in pages — multiply by page size.
- Never assume `/proc` (optional on NetBSD).
- Linux-only NRs (epoll/inotify/signalfd/futex/arch_prctl) → ENOSYS.
