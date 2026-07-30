package install

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// installPath replaces dst with src. Bundle directories use the normal remove+copy
// path. On Windows, replacing the currently-running executable fails with a
// sharing violation, so we stage a .pending sibling and schedule a swap after
// this process exits (plan review P2).
func installPath(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if runtime.GOOS == "windows" && sameFileAsExecutable(dst) {
		return installPendingWindows(src, dst)
	}
	if _, err := os.Lstat(dst); err == nil {
		if err := os.RemoveAll(dst); err != nil {
			return fmt.Errorf("remove old bundle: %w", err)
		}
	}
	return copyTree(src, dst)
}

func sameFileAsExecutable(dst string) bool {
	exe, err := os.Executable()
	if err != nil {
		return false
	}
	exe, err = filepath.EvalSymlinks(exe)
	if err != nil {
		exe, _ = os.Executable()
	}
	dstAbs, err := filepath.Abs(dst)
	if err != nil {
		return false
	}
	exeAbs, err := filepath.Abs(exe)
	if err != nil {
		return false
	}
	return strings.EqualFold(filepath.Clean(dstAbs), filepath.Clean(exeAbs))
}

func installPendingWindows(src, dst string) error {
	pending := dst + ".pending"
	_ = os.Remove(pending)
	if err := copyTree(src, pending); err != nil {
		return fmt.Errorf("stage pending self-install: %w", err)
	}
	// Detached cmd swaps pending → final after a short delay so our process can
	// exit and release the file lock. /c exits when done; start does not wait.
	bat := fmt.Sprintf(
		`@echo off
ping -n 2 127.0.0.1 >nul
move /Y "%s" "%s"
del "%%~f0"
`, pending, dst)
	batPath := dst + ".replace.bat"
	if err := os.WriteFile(batPath, []byte(bat), 0o644); err != nil {
		return err
	}
	cmd := exec.Command("cmd.exe", "/C", "start", "", "/MIN", batPath)
	cmd.Stdout = nil
	cmd.Stderr = nil
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("schedule self-replace: %w", err)
	}
	_ = cmd.Process.Release()
	return nil
}
