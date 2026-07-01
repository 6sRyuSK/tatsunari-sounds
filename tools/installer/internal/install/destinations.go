// Package install turns a chosen set of (plugin, format, scope) selections into
// concrete on-disk operations: resolving destinations, extracting zips, moving
// bundles into place, macOS quarantine/AU housekeeping and receipt bookkeeping.
package install

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// Destination returns the directory a bundle of (format) is installed into for
// the given os/scope. Bundles (".vst3"/".component") are placed directly under
// this directory. The Windows system path mirrors the existing tools/install.ps1
// convention of a "tatsunari" subfolder.
//
// Env-based Windows roots fall back to conventional literals when the variable
// is absent (e.g. computing a Windows plan while dry-running on another OS).
func Destination(osID model.OS, format model.Format, scope model.Scope) (string, error) {
	switch osID {
	case model.OSWindows:
		if format != model.FormatVST3 {
			return "", fmt.Errorf("windows has no %s format", format)
		}
		switch scope {
		case model.ScopeSystem:
			common := envOr("CommonProgramFiles", `C:\Program Files\Common Files`)
			return filepath.Join(common, "VST3", "tatsunari"), nil
		case model.ScopeUser:
			local := envOr("LOCALAPPDATA", `%LOCALAPPDATA%`)
			return filepath.Join(local, "Programs", "Common", "VST3"), nil
		}
	case model.OSMacOS:
		sub, err := macSubdir(format)
		if err != nil {
			return "", err
		}
		switch scope {
		case model.ScopeSystem:
			return filepath.Join("/Library/Audio/Plug-Ins", sub), nil
		case model.ScopeUser:
			home, err := homeDir()
			if err != nil {
				return "", err
			}
			return filepath.Join(home, "Library", "Audio", "Plug-Ins", sub), nil
		}
	}
	return "", fmt.Errorf("unsupported os %q", osID)
}

func macSubdir(format model.Format) (string, error) {
	switch format {
	case model.FormatVST3:
		return "VST3", nil
	case model.FormatAU:
		return "Components", nil
	}
	return "", fmt.Errorf("unsupported macOS format %q", format)
}

// InstallRoots returns every destination directory the applier is allowed to
// write into for the given OS. The privileged __apply helper validates every
// move destination against this allowlist before acting with elevated rights.
func InstallRoots(osID model.OS) []string {
	var roots []string
	for _, scope := range []model.Scope{model.ScopeSystem, model.ScopeUser} {
		for _, f := range []model.Format{model.FormatVST3, model.FormatAU} {
			if d, err := Destination(osID, f, scope); err == nil {
				roots = append(roots, filepath.Clean(d))
			}
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
	// Prefer $HOME so the resolution is deterministic across OSes (on macOS
	// os.UserHomeDir already returns $HOME; on Windows this only matters for
	// the ConfigDir fallback and for tests).
	if h := os.Getenv("HOME"); h != "" {
		return h, nil
	}
	h, err := os.UserHomeDir()
	if err != nil || h == "" {
		return "", fmt.Errorf("cannot determine home directory: %w", err)
	}
	return h, nil
}
