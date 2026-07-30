// Package install turns a chosen set of (plugin, format, scope) selections into
// concrete on-disk operations: resolving destinations, extracting zips, moving
// bundles into place, macOS quarantine/AU housekeeping and receipt bookkeeping.
package install

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/updates"
)

// DestinationRoot returns the binary-enum root directory for (os, scope, id).
// Manifests never supply this path — only a DestinationRootID + relative subpath.
func DestinationRoot(osID model.OS, scope model.Scope, id model.DestinationRootID) (string, error) {
	switch id {
	case model.RootPlugin:
		return pluginRoot(osID, scope)
	case model.RootInstaller:
		return installerBinRoot(osID, scope)
	case model.RootReceipt:
		return receiptRoot(osID, scope)
	default:
		return "", fmt.Errorf("unknown destination root %q", id)
	}
}

func pluginRoot(osID model.OS, scope model.Scope) (string, error) {
	switch osID {
	case model.OSWindows:
		switch scope {
		case model.ScopeSystem:
			return envOr("CommonProgramFiles", `C:\Program Files\Common Files`), nil
		case model.ScopeUser:
			local := envOr("LOCALAPPDATA", `%LOCALAPPDATA%`)
			return filepath.Join(local, "Programs", "Common"), nil
		}
	case model.OSMacOS:
		switch scope {
		case model.ScopeSystem:
			return "/Library/Audio/Plug-Ins", nil
		case model.ScopeUser:
			home, err := homeDir()
			if err != nil {
				return "", err
			}
			return filepath.Join(home, "Library", "Audio", "Plug-Ins"), nil
		}
	}
	return "", fmt.Errorf("unsupported os %q", osID)
}

func installerBinRoot(osID model.OS, scope model.Scope) (string, error) {
	switch osID {
	case model.OSMacOS:
		base, err := appSupportRoot(osID, scope)
		if err != nil {
			return "", err
		}
		return filepath.Join(base, "bin"), nil
	case model.OSWindows:
		switch scope {
		case model.ScopeSystem:
			pf := envOr("ProgramFiles", `C:\Program Files`)
			return filepath.Join(pf, "tatsunari-sounds"), nil
		case model.ScopeUser:
			local := envOr("LOCALAPPDATA", `%LOCALAPPDATA%`)
			return filepath.Join(local, "tatsunari-sounds", "bin"), nil
		}
	}
	return "", fmt.Errorf("unsupported os %q", osID)
}

func receiptRoot(osID model.OS, scope model.Scope) (string, error) {
	switch scope {
	case model.ScopeUser:
		return ConfigDir()
	case model.ScopeSystem:
		switch osID {
		case model.OSMacOS:
			return "/Library/Application Support/tatsunari-sounds", nil
		case model.OSWindows:
			pd := envOr("ProgramData", `C:\ProgramData`)
			return filepath.Join(pd, "tatsunari-sounds"), nil
		}
	}
	return "", fmt.Errorf("unsupported receipt scope %q os %q", scope, osID)
}

func appSupportRoot(osID model.OS, scope model.Scope) (string, error) {
	switch osID {
	case model.OSMacOS:
		switch scope {
		case model.ScopeSystem:
			return "/Library/Application Support/tatsunari-sounds", nil
		case model.ScopeUser:
			home, err := homeDir()
			if err != nil {
				return "", err
			}
			return filepath.Join(home, "Library", "Application Support", "tatsunari-sounds"), nil
		}
	}
	return "", fmt.Errorf("appSupportRoot only defined for macOS")
}

// DefaultSubpath returns the conventional relative placement for a format when
// the catalog asset does not declare subpath (legacy GitHub zip path).
func DefaultSubpath(osID model.OS, format model.Format) (string, error) {
	switch osID {
	case model.OSMacOS:
		switch format {
		case model.FormatVST3:
			return "VST3", nil
		case model.FormatAU:
			return "Components", nil
		case model.FormatCLAP:
			return "CLAP", nil
		}
	case model.OSWindows:
		switch format {
		case model.FormatVST3:
			// Preserve the existing tools/install.ps1 layout for system installs
			// by using the same relative path under CommonProgramFiles; user
			// installs stay flat under Programs\Common\VST3.
			return "VST3/tatsunari-sounds", nil
		case model.FormatCLAP:
			return "CLAP", nil
		case model.FormatAU:
			return "", fmt.Errorf("windows has no AU format")
		}
	}
	return "", fmt.Errorf("unsupported os/format %s/%s", osID, format)
}

// ResolvePath joins root + validated subpath + optional bundleName.
func ResolvePath(root, subpath, bundleName string) (string, error) {
	if err := updates.ValidateSubpath(subpath); err != nil {
		return "", err
	}
	parts := []string{filepath.Clean(root)}
	for _, seg := range strings.Split(filepath.ToSlash(subpath), "/") {
		parts = append(parts, seg)
	}
	if bundleName != "" {
		if err := updates.ValidateBundleName(bundleName); err != nil {
			return "", err
		}
		parts = append(parts, bundleName)
	}
	return filepath.Join(parts...), nil
}

// Destination returns the directory a bundle of (format) is installed into for
// the given os/scope (without the bundle leaf name). Compat wrapper over
// DestinationRoot + DefaultSubpath for the legacy GitHub zip path.
func Destination(osID model.OS, format model.Format, scope model.Scope) (string, error) {
	root, err := DestinationRoot(osID, scope, model.RootPlugin)
	if err != nil {
		return "", err
	}
	sub, err := DefaultSubpath(osID, format)
	if err != nil {
		return "", err
	}
	// Windows user VST3 historically omits the tatsunari-sounds subfolder.
	if osID == model.OSWindows && format == model.FormatVST3 && scope == model.ScopeUser {
		sub = "VST3"
	}
	return ResolvePath(root, sub, "")
}

// InstallRoots returns every destination directory the applier is allowed to
// write into for the given OS. Includes plugin roots, installer bin dirs, and
// receipt dirs so self-install + system receipt updates stay inside the
// allowlist (invariant 5).
func InstallRoots(osID model.OS) []string {
	seen := map[string]bool{}
	var roots []string
	add := func(p string) {
		p = filepath.Clean(p)
		if p == "" || seen[p] {
			return
		}
		seen[p] = true
		roots = append(roots, p)
	}
	for _, scope := range []model.Scope{model.ScopeSystem, model.ScopeUser} {
		if r, err := DestinationRoot(osID, scope, model.RootPlugin); err == nil {
			add(r)
		}
		if r, err := DestinationRoot(osID, scope, model.RootInstaller); err == nil {
			add(r)
		}
		if r, err := DestinationRoot(osID, scope, model.RootReceipt); err == nil {
			add(r)
		}
	}
	return roots
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func homeDir() (string, error) {
	if h := os.Getenv("HOME"); h != "" {
		return h, nil
	}
	h, err := os.UserHomeDir()
	if err != nil || h == "" {
		return "", fmt.Errorf("cannot determine home directory: %w", err)
	}
	return h, nil
}
