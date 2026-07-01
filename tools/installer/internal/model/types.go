// Package model holds the installer's plain data types plus a few pure helpers
// (semver comparison, install-plan construction). It deliberately depends on
// nothing else so it can be unit-tested headless and imported everywhere.
package model

// OS identifies a target operating system for plugin assets.
type OS string

const (
	OSMacOS   OS = "macOS"
	OSWindows OS = "Windows"
)

// Format identifies a plugin binary format. AU is macOS-only in this repo.
type Format string

const (
	FormatVST3 Format = "VST3"
	FormatAU   Format = "AU"
)

// Scope selects a system-wide (all users, needs OS elevation) or per-user
// (no password) install destination.
type Scope string

const (
	ScopeSystem Scope = "system"
	ScopeUser   Scope = "user"
)

// InstallState describes a plugin relative to what is already installed.
type InstallState int

const (
	// StateNotInstalled: no local receipt / bundle found.
	StateNotInstalled InstallState = iota
	// StateUpToDate: installed version >= latest available.
	StateUpToDate
	// StateUpdateAvailable: installed version < latest available.
	StateUpdateAvailable
	// StateInstalledUnknown: a bundle exists on disk but we have no recorded
	// version (e.g. a manual install), so we cannot tell if it is current.
	StateInstalledUnknown
)

// AssetKey identifies one downloadable per-plugin zip within a release.
type AssetKey struct {
	OS     OS
	Format Format
}

// Asset is one downloadable release asset (a per-plugin zip).
type Asset struct {
	Name        string // e.g. "resonance-suppressor-v0_2_1-macOS-VST3.zip"
	DownloadURL string // https browser_download_url
	Size        int64
}

// Plugin is the reconciled view of one plugin: release metadata joined with
// local install state.
type Plugin struct {
	Slug      string
	Name      string // display name (catalog.json or title-cased slug)
	Category  string
	Reference string
	Version   string // latest available version, dotted (e.g. "0.2.1")

	// Available maps each (os, format) offered by the current release to its
	// downloadable asset. Only entries valid for this platform are surfaced in
	// the UI, but the map itself is platform-agnostic.
	Available map[AssetKey]Asset

	Installed string // installed version, "" if none / unknown
	State     InstallState
}

// HasFormat reports whether the release offers this plugin for the given
// os/format pair.
func (p Plugin) HasFormat(os OS, f Format) bool {
	_, ok := p.Available[AssetKey{OS: os, Format: f}]
	return ok
}

// ApplyResult is written by the privileged __apply helper (and returned by the
// in-process apply) so the unprivileged parent can report outcomes and write
// the receipt.
type ApplyResult struct {
	Installed []string `json:"installed"` // destination bundle dirs written
	Errors    []string `json:"errors"`
}
