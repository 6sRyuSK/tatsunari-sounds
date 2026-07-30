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

type stagedItem struct {
	item      model.PlanItem
	bundleDir string // extracted source
	dst       string // final bundle path
}

// Run downloads, verifies, extracts and installs the given items. Per-item
// failures (bad download, checksum mismatch, extract error) are recorded and
// skipped; the rest still install. All staging happens before the single
// apply/elevation so the user is prompted at most once.
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

	var (
		staged     []stagedItem
		result     model.ApplyResult
		moves      []model.Move
		quarantine []string
		refreshAU  bool
	)

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
		moves = append(moves, model.Move{Src: bundleDir, Dst: dst})
		if in.OS == model.OSMacOS {
			quarantine = append(quarantine, dst)
			if it.Format == model.FormatAU {
				refreshAU = true
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
					mv, err := install.SelfInstallMove(in.OS, scope, stagedBin)
					if err != nil {
						result.Errors = append(result.Errors, fmt.Sprintf("self-install: %v", err))
					} else {
						moves = append(moves, mv)
					}
				}
			}
		}
	}

	if len(moves) == 0 {
		return result, nil, nil // nothing staged; only errors (if any)
	}

	plan := model.InstallPlan{Moves: moves, Quarantine: quarantine, RefreshAU: refreshAU}
	applyRes, err := elevate.Apply(scope, plan, stagingRoot)
	if err != nil {
		// Elevation cancelled/failed: report as a whole-batch error alongside
		// any per-item staging errors already collected.
		result.Errors = append(result.Errors, err.Error())
		return result, nil, err
	}
	result.Installed = append(result.Installed, applyRes.Installed...)
	result.Errors = append(result.Errors, applyRes.Errors...)

	installedSet := make(map[string]bool, len(applyRes.Installed))
	for _, d := range applyRes.Installed {
		installedSet[d] = true
	}
	var installed []InstalledItem
	for _, s := range staged {
		if installedSet[s.dst] {
			installed = append(installed, InstalledItem{Item: s.item, Dst: s.dst})
			emit(s.item, PhaseDone, nil)
		}
	}
	return result, installed, nil
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
