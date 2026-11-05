# PS2Recomp upstream patch series

`PS2Recomp_upstream_series.txt` is the ordered patch list for upstream `eb8688b`. The existing base patch is retained
unchanged; subsequent deltas contain the later shared timing and capture/API documentation changes. Vendored
PS2Recomp source in this repository already includes the complete series. PCSX2 GS source is vendored separately
and is not part of this series.

In a checkout of PS2Recomp at `eb8688b`, substitute the absolute path to this repository for `<Fixer>`:

```sh
git apply -p1 <Fixer>/patches/PS2Recomp_vs_upstream_eb8688b.diff
git apply -p1 --unidiff-zero <Fixer>/patches/PS2Recomp_host_maintenance_20261103.diff
git apply -p1 --unidiff-zero <Fixer>/patches/PS2Recomp_particle_profiles_20261104.diff
git apply -p1 --unidiff-zero <Fixer>/patches/PS2Recomp_neutral_host_20261105.diff
git apply -p1 --unidiff-zero <Fixer>/patches/PS2Recomp_storage_bounds_20261106.diff
git apply -p1 --unidiff-zero <Fixer>/patches/PS2Recomp_path_utf8_20261106.diff
```

The incremental patch uses zero context to carry only the shared changes, so `--unidiff-zero` is required. Apply it
to the specified base after the first patch, not to an arbitrary port fork. The headless patch test reverses the
series, reapplies it in order and checks that every vendored file is reproduced byte for byte. Future changes must
update the appropriate delta or append a new one to the series and preserve that check.
