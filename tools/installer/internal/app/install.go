package app

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/elevate"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/release"
)

// Install phases reported through ProgressFunc.
const (
	PhaseDownload = "download"
	PhaseVerify   = "verify"
	PhaseExtract  = "extract"
	PhaseInstall  = "install"
	PhaseDone     = "done"
	PhaseError    = "error"
)

// ProgressEvent is one step update for one plan item.
type ProgressEvent struct {
	Item  model.PlanItem
	Phase string
	Err   error
}

// ProgressFunc receives progress updates (may be nil).
type ProgressFunc func(ProgressEvent)

// InstalledItem records a plan item that reached its destination, so the caller
// can write the receipt.
type InstalledItem struct {
	Item model.PlanItem
	Dst  string
}

// Installer stages downloads under a private temp dir and applies them under
// the requested scope (one elevation prompt for system scope).
type Installer struct {
	Client      *release.Client
	Checksums   release.Checksums
	OS          model.OS
	SelfInstall bool // copy this executable into the scope's installer bin (plan §5.4)
}

// applyFn is the elevation-aware apply entry point. It is a package variable
// purely as a test seam (same idiom as installRootsFn in apply.go): the real
// elevate.Apply shells out to osascript / RunAs for system scope, which no
// headless test can exercise. Production behaviour is unchanged.
var applyFn = elevate.Apply

type stagedItem struct {
	item      model.PlanItem
	bundleDir string // extracted source
	dst       string // final bundle path
}

// Run downloads, verifies, extracts and installs the given items. Per-item
// failures (bad download, checksum mismatch, extract error) are recorded and
// skipped; the rest still install. All staging happens before the apply step so
// the user is prompted at most once.
//
// scope is the scope the user picked for NEW installs; it is also where this
// executable self-installs. It is NOT necessarily where every item goes: an
// UPDATE keeps the scope of the row it updates (PlanItem.Scope), so one run can
// legitimately mix a user-scope install with a system-scope update. Elevation
// therefore follows each item's own scope, not the picked one — applying a
// system destination unelevated would just fail with a permission error.
// The moves are grouped by scope and applied user-first: only the system group
// crosses the elevation boundary, so there is still at most one prompt.
func (in *Installer) Run(ctx context.Context, items []model.PlanItem, scope model.Scope, progress ProgressFunc) (model.ApplyResult, []InstalledItem, error) {
	emit := func(it model.PlanItem, phase string, err error) {
		if progress != nil {
			progress(ProgressEvent{Item: it, Phase: phase, Err: err})
		}
	}

	stagingRoot, err := os.MkdirTemp("", "tatsunari-install-")
	if err != nil {
		return model.ApplyResult{}, nil, err
	}
	defer os.RemoveAll(stagingRoot)
	_ = os.Chmod(stagingRoot, 0o700)

	dlDir := filepath.Join(stagingRoot, "dl")
	stageDir := filepath.Join(stagingRoot, "stage")

	// One group per destination scope (see the note on Run above).
	type scopeGroup struct {
		moves      []model.Move
		quarantine []string
		refreshAU  bool
	}
	var (
		staged []stagedItem
		result model.ApplyResult
		groups = map[model.Scope]*scopeGroup{}
	)
	groupFor := func(s model.Scope) *scopeGroup {
		if s == "" {
			s = scope
		}
		g := groups[s]
		if g == nil {
			g = &scopeGroup{}
			groups[s] = g
		}
		return g
	}

	for i, it := range items {
		zipPath := filepath.Join(dlDir, it.Asset.Name)

		emit(it, PhaseDownload, nil)
		if err := in.Client.Download(ctx, it.Asset.DownloadURL, zipPath); err != nil {
			result.Errors = append(result.Errors, fmt.Sprintf("%s (%s): download: %v", it.Slug, it.Format, err))
			emit(it, PhaseError, err)
			continue
		}

		emit(it, PhaseVerify, nil)
		if err := in.verify(zipPath, it.Asset.Name); err != nil {
			result.Errors = append(result.Errors, fmt.Sprintf("%s (%s): %v", it.Slug, it.Format, err))
			emit(it, PhaseError, err)
			continue
		}

		emit(it, PhaseExtract, nil)
		itemStage := filepath.Join(stageDir, fmt.Sprintf("%d-%s-%s", i, it.Slug, it.Format))
		bundleDir, err := install.ExtractZip(zipPath, itemStage)
		if err != nil {
			result.Errors = append(result.Errors, fmt.Sprintf("%s (%s): extract: %v", it.Slug, it.Format, err))
			emit(it, PhaseError, err)
			continue
		}

		dst := filepath.Join(it.Destination, filepath.Base(bundleDir))
		staged = append(staged, stagedItem{item: it, bundleDir: bundleDir, dst: dst})
		g := groupFor(it.Scope)
		g.moves = append(g.moves, model.Move{Src: bundleDir, Dst: dst})
		if in.OS == model.OSMacOS {
			g.quarantine = append(g.quarantine, dst)
			if it.Format == model.FormatAU {
				g.refreshAU = true
			}
		}
	}

	if in.SelfInstall {
		exe, err := os.Executable()
		if err != nil {
			result.Errors = append(result.Errors, fmt.Sprintf("self-install: resolve executable: %v", err))
		} else {
			selfDir := filepath.Join(stageDir, "self")
			if err := os.MkdirAll(selfDir, 0o700); err != nil {
				result.Errors = append(result.Errors, fmt.Sprintf("self-install: stage dir: %v", err))
			} else {
				stagedBin := filepath.Join(selfDir, install.InstallerBinaryName(in.OS))
				if err := copyFileSimple(exe, stagedBin); err != nil {
					result.Errors = append(result.Errors, fmt.Sprintf("self-install: stage binary: %v", err))
				} else {
					// Self-install always follows the PICKED scope: it is a new
					// install of this binary, not an update of an existing row.
					mv, err := install.SelfInstallMove(in.OS, scope, stagedBin)
					if err != nil {
						result.Errors = append(result.Errors, fmt.Sprintf("self-install: %v", err))
					} else {
						g := groupFor(scope)
						g.moves = append(g.moves, mv)
					}
				}
			}
		}
	}

	// Apply user scope before system scope. The user group is applied in-process
	// and writes no plan.json, so the two groups cannot collide over the staging
	// dir's plan/result files, and the single elevation prompt (if any) comes
	// last — after the unprivileged work is already done.
	var applyErr error
	installedSet := map[string]bool{}
	for _, s := range []model.Scope{model.ScopeUser, model.ScopeSystem} {
		g := groups[s]
		if g == nil || len(g.moves) == 0 {
			continue
		}
		plan := model.InstallPlan{Moves: g.moves, Quarantine: g.quarantine, RefreshAU: g.refreshAU}
		applyRes, err := applyFn(s, plan, stagingRoot)
		if err != nil {
			// Elevation cancelled/failed for this scope: record it, but keep
			// whatever the other scope already installed.
			result.Errors = append(result.Errors, err.Error())
			if applyErr == nil {
				applyErr = err
			}
			continue
		}
		result.Installed = append(result.Installed, applyRes.Installed...)
		result.Errors = append(result.Errors, applyRes.Errors...)
		for _, d := range applyRes.Installed {
			installedSet[d] = true
		}
	}
	if len(installedSet) == 0 && applyErr == nil && len(staged) == 0 {
		return result, nil, nil // nothing staged; only errors (if any)
	}
	var installed []InstalledItem
	for _, s := range staged {
		if installedSet[s.dst] {
			installed = append(installed, InstalledItem{Item: s.item, Dst: s.dst})
			emit(s.item, PhaseDone, nil)
		}
	}
	// installed is returned even alongside applyErr: a partially-applied run
	// must still be recorded in the receipt, or the next run reinstalls over it.
	return result, installed, applyErr
}

// verify checks the downloaded zip against the release checksums. A missing
// checksum entry is treated as a hard failure (fail closed).
func (in *Installer) verify(zipPath, assetName string) error {
	want, ok := in.Checksums[assetName]
	if !ok {
		return fmt.Errorf("no published checksum for %s", assetName)
	}
	return release.Verify(zipPath, want)
}

// WriteReceipt records the installed items into the receipt for their
// (slug, variant, scope), using PlanItem.Version as the installed version.
func WriteReceipt(installed []InstalledItem) error {
	if len(installed) == 0 {
		return nil
	}
	r, err := install.LoadReceipt()
	if err != nil {
		return err
	}
	type agg struct {
		scope   model.Scope
		variant model.Variant
		version string
		formats []model.Format
		paths   []string
	}
	byKey := map[string]*agg{}
	for _, ii := range installed {
		variant := ii.Item.Variant
		if variant == "" {
			variant = model.VariantStable
		}
		key := install.EntryKey(ii.Item.Slug, variant, ii.Item.Scope)
		a := byKey[key]
		if a == nil {
			a = &agg{scope: ii.Item.Scope, variant: variant, version: ii.Item.Version}
			byKey[key] = a
		}
		a.formats = append(a.formats, ii.Item.Format)
		a.paths = append(a.paths, ii.Dst)
		if ii.Item.Version != "" {
			a.version = ii.Item.Version
		}
	}
	for key, a := range byKey {
		slug, variant, scope, err := install.ParseEntryKey(key)
		if err != nil {
			return err
		}
		r.Record(slug, variant, a.version, scope, a.formats, a.paths)
	}
	return r.Save()
}

func copyFileSimple(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o755)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}
