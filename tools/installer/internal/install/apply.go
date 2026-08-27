package install

import (
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// ApplyPlan executes an InstallPlan: it copies each staged bundle to its
// destination (replacing any existing bundle), runs macOS post-install
// housekeeping, and — for a system-scope plan — merges what it actually
// installed into the system receipt. This is the single apply engine, used both
// in-process for per-user installs and by the elevated __apply helper for
// system installs.
//
// The USER receipt is still the unprivileged parent's job (app.WriteReceipt).
// The SYSTEM receipt has to be written here: its directory is root-owned, and
// its contents are only knowable after the moves are attempted, so doing it in
// the parent would need a second elevation prompt.
func ApplyPlan(plan model.InstallPlan) model.ApplyResult {
	var res model.ApplyResult
	var recorded []model.ReceiptRef
	for _, mv := range plan.Moves {
		if err := installBundle(mv.Src, mv.Dst); err != nil {
			res.Errors = append(res.Errors, fmt.Sprintf("%s: %v", mv.Dst, err))
			continue
		}
		res.Installed = append(res.Installed, mv.Dst)
		// Only moves that actually landed are recorded: a receipt claiming an
		// install that failed would hide the plugin from the next run's repair.
		if mv.Receipt != nil {
			ref := *mv.Receipt
			recorded = append(recorded, ref)
		}
		// Strip quarantine right after a successful move (macOS; no-op else).
		if slices.Contains(plan.Quarantine, mv.Dst) {
			_ = stripQuarantine(mv.Dst)
		}
	}
	if plan.RefreshAU && len(res.Installed) > 0 {
		_ = refreshAudioUnits() // best-effort; failure just means a manual rescan
	}
	if plan.ReceiptPath != "" && len(recorded) > 0 {
		if err := mergeReceiptAt(plan.ReceiptPath, plan.Moves, recorded); err != nil {
			res.Errors = append(res.Errors, fmt.Sprintf("receipt %s: %v", plan.ReceiptPath, err))
		}
	}
	return res
}

// mergeReceiptAt reads the receipt at path (missing = empty), records every
// successfully-installed identity into it and writes it back. System receipts
// are world-readable (dir 0755 / file 0644) so any user on the machine can
// reconcile what is installed system-wide; without that a second account sees
// an empty receipt and reinstalls over a working system install.
func mergeReceiptAt(path string, moves []model.Move, recorded []model.ReceiptRef) error {
	r, err := loadReceiptFile(path)
	if err != nil {
		return err
	}
	// Destination path per (identity, format), so the receipt records where each
	// bundle actually went rather than a recomputed guess.
	dstOf := map[string]string{}
	for _, mv := range moves {
		if mv.Receipt == nil {
			continue
		}
		dstOf[EntryKey(mv.Receipt.Slug, mv.Receipt.Variant, mv.Receipt.Scope)+"|"+string(mv.Receipt.Format)] = mv.Dst
	}

	type agg struct {
		ref     model.ReceiptRef
		formats []model.Format
		paths   []string
	}
	order := []string{}
	byKey := map[string]*agg{}
	for _, ref := range recorded {
		key := EntryKey(ref.Slug, ref.Variant, ref.Scope)
		a := byKey[key]
		if a == nil {
			a = &agg{ref: ref}
			byKey[key] = a
			order = append(order, key)
		}
		if ref.Version != "" {
			a.ref.Version = ref.Version
		}
		a.formats = append(a.formats, ref.Format)
		if dst := dstOf[key+"|"+string(ref.Format)]; dst != "" {
			a.paths = append(a.paths, dst)
		}
	}
	for _, key := range order {
		a := byKey[key]
		r.Record(a.ref.Slug, a.ref.Variant, a.ref.Version, a.ref.Scope, a.formats, a.paths)
	}
	return r.SaveTo(path, 0o755, 0o644)
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
	// The receipt is the one non-bundle file the privileged applier writes, so
	// it gets the same allowlist treatment as a destination, plus a fixed
	// filename. Both together mean a tampered plan can at worst overwrite a
	// receipt.json inside a directory the installer already owns — never an
	// arbitrary privileged write.
	if plan.ReceiptPath != "" {
		if !underAny(plan.ReceiptPath, roots) {
			return fmt.Errorf("receipt path %q is outside allowed install roots", plan.ReceiptPath)
		}
		if filepath.Base(plan.ReceiptPath) != receiptFileName {
			return fmt.Errorf("receipt path %q must be named %s", plan.ReceiptPath, receiptFileName)
		}
	}
	return nil
}

// installBundle replaces the destination bundle dir with the staged one.
func installBundle(src, dst string) error {
	return installPath(src, dst)
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
	return writeFileFrom(dst, in, perm)
}

// writeFileFrom creates (truncating any existing file) dst with the given mode
// and streams r into it, closing the file on both success and error. A zero
// mode falls back to 0o644 so an entry with no recorded permissions still lands
// readable.
func writeFileFrom(dst string, r io.Reader, mode fs.FileMode) error {
	if mode == 0 {
		mode = 0o644
	}
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, mode.Perm())
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, r); err != nil {
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
	return slices.ContainsFunc(roots, func(r string) bool { return under(path, r) })
}
