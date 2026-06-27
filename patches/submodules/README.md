# Submodule Patch Notes

These patches are kept in the parent repository so the ESP32 ILI9341 firmware can use local submodule fixes without advancing the recorded submodule commits.

Apply them from the repository root before building the ESP32 firmware:

```powershell
git -C refs/emu8950 am ../../patches/submodules/0001-Place-ESP32-OPL-tables-in-external-BSS.patch
git -C refs/tiny386 am ../../patches/submodules/0001-Increase-ESP32-i386-TLB-size.patch
```

Patch purposes:

- `0001-Place-ESP32-OPL-tables-in-external-BSS.patch`
  Moves the large emu8950 lookup tables to ESP32 external BSS. This frees internal DRAM while keeping the upstream emu8950 submodule commit unchanged.

- `0001-Increase-ESP32-i386-TLB-size.patch`
  Raises the ESP32 i386 TLB size from 256 to 1024 entries. This improves TLB hit rate while keeping the extra internal DRAM use bounded.

If a patch fails because it was already applied, leave the submodule as-is. If you need to remove an applied patch before committing submodule changes, reset only that submodule back to the commit recorded by the parent repository.
