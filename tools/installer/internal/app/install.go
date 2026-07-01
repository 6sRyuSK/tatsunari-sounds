package app

import (
	"context"
	"fmt"
	"os"
	"path/filepath"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/elevate"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
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
	Client    *release.Client
	Checksums release.Checksums
	OS        model.OS
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

// WriteReceipt records the installed items into the user receipt, grouping by
// slug (versions from the plan items). Always called from the unprivileged
// parent so the receipt stays user-owned.
func WriteReceipt(installed []InstalledItem, versionOf map[string]string) error {
	if len(installed) == 0 {
		return nil
	}
	r, err := install.LoadReceipt()
	if err != nil {
		return err
	}
	type agg struct {
		scope   model.Scope
		formats []model.Format
		paths   []string
	}
	bySlug := map[string]*agg{}
	for _, ii := range installed {
		a := bySlug[ii.Item.Slug]
		if a == nil {
			a = &agg{scope: ii.Item.Scope}
			bySlug[ii.Item.Slug] = a
		}
		a.formats = append(a.formats, ii.Item.Format)
		a.paths = append(a.paths, ii.Dst)
	}
	for slug, a := range bySlug {
		r.Record(slug, versionOf[slug], a.scope, a.formats, a.paths)
	}
	return r.Save()
}
