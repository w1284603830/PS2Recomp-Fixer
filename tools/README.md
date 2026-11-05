# Tool boundaries

General tools in this directory operate on caller-supplied source, maps, captures or archives. Hardware address
labels in GIF/IO census tools describe PS2 registers. They are not a game's RAM layout. Run tools on copies and
review generated function-map changes before rebuilding a port.

The following fixed-range/counter scripts moved to `examples/dod_runner/tools/`, preserving the relative `diag/`
subdirectory. Their constants describe the reference game's executable and are not generic defaults:

- `fix_function_map.py`, `scan_code_pointers.py`, `scan_jump_tables.py`, `find_missing_function_starts.py`
- `diag/cop2_census.py`, `diag/mmi_census.py`, `diag/find_const.py`, `diag/find_io.py`, `diag/find_io2.py`
- `diag/hle_inventory.py`, `diag/pcsx2_compare_report.py`

These scripts remain historical reference tools, with their original bounds/assumptions. Other games need ELF segment
mapping, executable/data ranges, instruction/ABI guards and measured counters from their own revision. No renamed
script is claimed to implement a general ELF analysis. `tools/diag/ee_dis.py` already reads ELF load segments;
`tools/diag/mips_dis.py` takes an explicit module load base. Library-prefix inventory and capture analyzers remain
shared where their inputs determine the addresses.

For failure triage, capture consumed input and the first differing EE/VU/GIF state, then use
[REGRESSION_TESTING.md](../docs/REGRESSION_TESTING.md). Opcode counts, missing-map counts or an empty smoke runner
cannot establish full-game correctness.
