package install

import (
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// ApplyPlan executes an InstallPlan: it copies each staged bundle to its
// destination (replacing any existing bundle) and runs macOS post-install
// housekeeping. This is the single apply engine, used both in-process for
// per-user installs and by the elevated __apply helper for system installs.
//
// It never touches the receipt — the unprivileged parent owns that.
func ApplyPlan(plan model.InstallPlan) model.ApplyResult {
	var res model.ApplyResult
	for _, mv := range plan.Moves {
		if err := installBundle(mv.Src, mv.Dst); err != nil {
			res.Errors = append(res.Errors, fmt.Sprintf("%s: %v", mv.Dst, err))
			continue
		}
		res.Installed = append(res.Installed, mv.Dst)
		// Strip quarantine right after a successful move (macOS; no-op else).
		if contains(plan.Quarantine, mv.Dst) {
			_ = stripQuarantine(mv.Dst)
		}
	}
	if plan.RefreshAU && len(res.Installed) > 0 {
		_ = refreshAudioUnits() // best-effort; failure just means a manual rescan
	}
	return res
}

// ValidatePlan is the security gate the privileged helper runs before acting:
// every destination must live under an allowlisted install root and every
// source under the staging root. This stops a tampered plan.json from turning
// elevation into an arbitrary privileged write.
func ValidatePlan(plan model.InstallPlan, allowedRoots []string, stagingRoot string) error {
	staging := filepath.Clean(stagingRoot)
	roots := make([]string, len(allowedRoots))
	for i, r := range allowedRoots {
		roots[i] = filepath.Clean(r)
	}
	for _, mv := range plan.Moves {
		if !underAny(mv.Dst, roots) {
			return fmt.Errorf("destination %q is outside allowed install roots", mv.Dst)
		}
		if !under(mv.Src, staging) {
			return fmt.Errorf("source %q is outside the staging root", mv.Src)
		}
	}
	for _, q := range plan.Quarantine {
		if !underAny(q, roots) {
			return fmt.Errorf("quarantine target %q is outside allowed install roots", q)
		}
	}
	return nil
}

// installBundle replaces the destination bundle dir with the staged one.
func installBundle(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if _, err := os.Lstat(dst); err == nil {
		if err := os.RemoveAll(dst); err != nil {
			return fmt.Errorf("remove old bundle: %w", err)
		}
	}
	return copyTree(src, dst)
}

// copyTree recursively copies a directory tree, preserving file modes and
// symlinks (macOS bundles contain both). It does not follow symlinks.
func copyTree(src, dst string) error {
	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		info, err := d.Info()
		if err != nil {
			return err
		}
		switch {
		case info.Mode()&os.ModeSymlink != 0:
			link, err := os.Readlink(path)
			if err != nil {
				return err
			}
			_ = os.Remove(target)
			return os.Symlink(link, target)
		case d.IsDir():
			return os.MkdirAll(target, info.Mode().Perm()|0o700)
		default:
			return copyFile(path, target, info.Mode().Perm())
		}
	})
}

func copyFile(src, dst string, perm os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if perm == 0 {
		perm = 0o644
	}
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, perm)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

func under(path, root string) bool {
	p := filepath.Clean(path)
	return p == root || strings.HasPrefix(p, root+string(os.PathSeparator))
}

func underAny(path string, roots []string) bool {
	for _, r := range roots {
		if under(path, r) {
			return true
		}
	}
	return false
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}
