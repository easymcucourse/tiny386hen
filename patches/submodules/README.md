# Submodule Patch Notes

These patches are kept in the parent repository so the ESP32 ILI9341 firmware can use local submodule fixes without advancing the recorded submodule commits.

Apply the non-tiny386 submodule patches from the repository root before building the ESP32 firmware:

```powershell
git -C refs/emu8950 am ../../patches/submodules/0001-Place-ESP32-OPL-tables-in-external-BSS.patch
```

Patch purposes:

- `0001-Place-ESP32-OPL-tables-in-external-BSS.patch`
  Moves the large emu8950 lookup tables to ESP32 external BSS. This frees internal DRAM while keeping the upstream emu8950 submodule commit unchanged.

Tiny386 ESP-specific overrides are kept under `src/tiny386` and compiled directly before the upstream `refs/tiny386` include path. This keeps the tiny386 submodule unchanged while allowing local replacements such as `i386.c`, `pc.c`, and `pc.h`.

If a patch fails because it was already applied, leave the submodule as-is. If you need to remove an applied patch before committing submodule changes, reset only that submodule back to the commit recorded by the parent repository.
