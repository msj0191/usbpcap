# Learnings — fix-block-hwid

## 2026-03-31 Task: Initial State Assessment

### adcusb.h (adcusbDriver/include/adcusb.h) — ALREADY COMPLETE
- `ADCUSB_BLOCK_RULE_PATTERN_LEN` = 128 (line 63)
- `ADCUSB_MAX_BLOCK_RULES` = 16 (line 64)
- `ADCUSB_BLOCK_RULE` struct at lines 67-71 (in `#pragma pack(push, 1)` block)
- `ADCUSB_BLOCK_RULE_LIST_RESPONSE` struct at lines 73-79 (separate `#pragma pack(push, 1)` block)
  - Fields: `ULONG count` + `WCHAR patterns[16][128]`
- `IOCTL_ADCUSB_LIST_BLOCK_RULES` at lines 100-101:
  - `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)` ← CORRECT

### adcusbDeviceControl.c — NEEDS Task 2
- `HandleadcusbControlIOCTL()` function handles IOCTLs at lines 15-244
- SpinLock pattern (lines 78, 129): `KeAcquireSpinLock(&g_blockRules.lock, &oldIrql)` / `KeReleaseSpinLock(&g_blockRules.lock, oldIrql)`
- GET_HUB_SYMLINK output pattern (lines 21-52): OutputBufferLength check → RtlCopyMemory → *outLength → return
- PROTECTION_CONTROL is at lines 133-159 (last before `allowCapture` gate at line 162)
- **Insert LIST_BLOCK_RULES handler BEFORE the allowCapture check (line 161)**
- `outLength` parameter is `SIZE_T *` (not ULONG *)

### iocontrol.c — NEEDS Task 3
- `adcusbSendBlockRule()` pattern at lines 134-153: DeviceIoControl, hDevice check, wcsncpy, NULL output
- New function `adcusbListBlockRules(HANDLE hDevice, PADCUSB_BLOCK_RULE_LIST_RESPONSE pResponse)` needs OUTPUT direction:
  - InputBuffer=NULL, InputLen=0, OutputBuffer=pResponse, OutputLen=sizeof(*pResponse)

### cmd.c — NEEDS Tasks 4, 5, 6
- `long_options[]` at lines 1847-1880
- `ARG_BLOCK_HWID=910`, `ARG_ALLOW_HWID=911` at lines 1820-1821
- `block_hwid_str` and `allow_hwid_str` vars at lines 1883-1884
- `--block-hwid`/`--allow-hwid` main block at lines 2051-2098
- "Device not specified" at line 2060 (line 2058-2063)
- CreateFileA error at lines 2073-2079 (single fprintf only)

### git state
- ALL files are UNTRACKED (never `git add`-ed)
- Last commit: `e7cc9f8 fix(vmusb): selective SCSI write blocking via CBW parsing`
- Need to `git add` files before committing

## 2026-03-31 Task 5: CreateFileA Error Diagnostics

- `adcusbCMD/cmd.c` has two `CreateFileA` failure paths that now use `DWORD dwErr` + `switch` for `ERROR_FILE_NOT_FOUND` and `ERROR_ACCESS_DENIED`.
- `filters_initialize()` / `filters_free()` are used only on `ERROR_FILE_NOT_FOUND` to print available devices from `adcusbFilters[i]->device`.
- `build_task_driver.bat fre` completed successfully via `cmd.exe /c build_task_driver.bat fre`.
