# Kernel 7.4 testing kernel (x86/64 only)

Branch: `kernel-7.4`, based on vanilla upstream main (`origin` = openwrt/openwrt).
It's independent of `kernel-7.3`: no shared history and no 7.3 files. Nobody knows
yet whether 7.3 or 7.4 will be the 2027 LTS, so either branch has to be able to go
upstream on its own. Fixes needed on both get ported to each branch separately.

Don't push until the user has tested it.

Same workflow and sandbox notes as KERNEL-7.3.md on kernel-7.3. Before building:
`make defconfig` (`.config` is a diffconfig), then check that
`grep CONFIG_LINUX_7_4=y .config` matches.

## Steps

1. `kernel: add define for kernel 7.4` (-rc1 tarball from git.kernel.org/torvalds/t).
2. `scripts/kernel_bump.sh -p generic -s v6.18 -t v7.4`, then the same with `-p x86`.
   `-p` is required: without it, the script takes the platform from the cwd name.
3. `x86: enable testing kernel for x86` (`KERNEL_TESTING_PATCHVER:=7.4`).
4. generic backport/pending/hack: remove what's obsolete, manually rebuild, refresh.
5. Kernel symbols (generic, then x86), kmod packaging, build fallout.
6. Each new RC: bump the version and hash, then refresh.

## Porting from kernel-7.3

- `LINUX_7_3` Kconfig conditions become `LINUX_7_4`. `LINUX_x_y` only exists for
  kernel versions a target uses, and it matches one exact version.
- `@ge7.3` / `@lt7.3` file filters stay as they are. They mark the kernel version
  that changed, and they're still true on 7.4.
- main dropped 6.12, so the 7.3 commits that rekeyed 6.12/6.18 conditions are moot,
  and so is the Module.symvers fallback commit.

## Status log

- 2026-11-03: 7.4-rc1 (released 2026-11-01).
  - Backports: all 228 removed. They're all named v6.19..v7.4, and each one is
    checked against upstream by sha or subject, or by content where the shallow
    history hid the commit.
  - Pending removed as upstream: 707, 711-05, 796, 880 (35cbd34a6a05), 895-00.
    Dropped with reasons: pending 812, hack 250, 260, 970 (same reasons as 7.3).
    707 still applied forward with fuzz even though it's upstream. Don't trust
    "applies" for obsolete detection.
  - Took the 7.3 rebuild for 20 patches whose 6.18 version is unchanged on main.
    Rebuilt the rest on 7.4:
    - 151 and 710 (bridge port flags are bitops now)
    - 203 (kallsyms writes a binary blob through .incbin now)
    - 708 (from upstream nf-next v8 instead of the 6.18 downport)
    - 760-09 (DSA tag value 37; uses the upstream teardown)
    - 835-03/05 (leds-netdev)
    - 904 (new dma-buf layout)
    - 201 (gen_init_cpio moved to scripts/)
    - 967-36
    - context-only: 111, 140, 485, 487, 737-02/03, 742, 795-03, hack 710, 721, 902
  - Checked all 36 hunks that needed fuzz during the refresh; all landed in the
    right place.

  - Configs: removed 362 dead symbols (generic 277, x86 85). A symbol only counts
    as dead if no Kconfig in the patched tree, generic/files or any target patch
    defines it. Added 98 new ones with kconfig defaults: arch-independent disabled
    ones go in generic, the rest in x86/64. Then 9 more exposed by the kmods.
    BOOTPARAM_HUNG_TASK_PANIC and BOOTPARAM_SOFTLOCKUP_PANIC are integers now (=0).
    A second kernel_oldconfig run asks nothing new and changes nothing; FRAME_WARN
    always prompts because config-filter strips it.
  - Build fixes:
    - 642 reads the fdb dst through br_fdb_dst_port()
    - r8169 needs kmod-phylink
    - realtek needs kmod-phy-package, amd-xgbe needs kmod-net-selftests
  - Ported from 7.3: the rc kmod version fixes, 7 package build fixes and the kmod
    packaging changes. The krb5 modules stay in main's kmod-crypto-krb5 (camellia
    added there); they don't go into rpcsec the way 7.3 did it.
  - Full x86/64 build OK with the user's config (glibc, gcc 15.3, LTO): ext4 and
    squashfs, BIOS and EFI images, kernel 7.4-rc1. Not boot tested.
  - After pulling new kmod deps: `make defconfig` (it added kmod-phy-package,
    kmod-net-selftests, kmod-phylink).

## MUST-FIX before another target uses 7.4

- YT922x switch series (pending 967-27, 967-40..55, hack 967-39) is dropped. 7.4
  moved the yt921x DSA driver into drivers/net/dsa/motorcomm/ (chip.c, mdio_bus.c,
  ...), so the series needs a port to the new layout, not a rebuild. Only used by
  mediatek/filogic (GL-BE14000). 967-36 (the YT8824 PHY driver) is kept.
- 7.4 removed ATM CLIP/LANE/MPOA and the PCI ATM drivers; only the core, BR2684 and
  PPPoATM are left. Check the ATM kmods.
- From KERNEL-7.3.md, still open on 7.4: pending 497 vs upstream MTD_VIRT_CONCAT,
  739-03 !pcs check, 720-04 NULL priv.
