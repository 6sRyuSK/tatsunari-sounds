# winres — the Windows application manifest

`rsrc_windows_amd64.syso` (in the module root, next to `main.go`) is a COFF
resource object the Go linker picks up automatically for `GOOS=windows
GOARCH=amd64` builds. It embeds the application manifest generated from
`winres.json`.

## Why it must exist

The shipping binary is named `tatsunari-sounds-installer.exe` (plan §11.5).
Windows' UAC **Installer Detection** heuristic forces an elevation prompt on
*unmanifested* executables whose filename contains `install` / `setup` /
`update` / `patch`. Embedding an explicit manifest with
`requestedExecutionLevel=asInvoker` disables that heuristic, so the installer
starts unelevated and system-scope elevation stays the sole responsibility of
the `__apply` boundary.

Deleting the `.syso`, or changing the execution level, reintroduces a UAC
prompt before the user has even picked a scope. `TestWindowsManifest*` in
`internal/…`/`winres_test.go` gates both.

## Regenerating

```bash
cd tools/installer
go run github.com/tc-hib/go-winres@v0.3.3 make --arch amd64
```

That rewrites `rsrc_windows_amd64.syso` from `winres.json`. Commit both.
Only amd64 is generated because that is the only Windows target the release
pipeline builds; add `--arch arm64` (and a matching test case) if that changes.
