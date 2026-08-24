package install

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// InstallerBinaryName is the on-disk name for the package manager binary
// (plan §11.5). The name deliberately contains "installer", which trips the
// Windows UAC Installer Detection heuristic for UNMANIFESTED executables — the
// Windows build therefore embeds an explicit application manifest declaring
// requestedExecutionLevel=asInvoker (see winres/), which suppresses the
// heuristic entirely. System-scope elevation stays the sole responsibility of
// the __apply boundary. Do not drop that manifest.
func InstallerBinaryName(osID model.OS) string {
	if osID == model.OSWindows {
		return "tatsunari-sounds-installer.exe"
	}
	return "tatsunari-sounds-installer"
}

// InstallerBinaryPath is the canonical permanent path for the installer under scope.
func InstallerBinaryPath(osID model.OS, scope model.Scope) (string, error) {
	root, err := DestinationRoot(osID, scope, model.RootInstaller)
	if err != nil {
		return "", err
	}
	return filepath.Join(root, InstallerBinaryName(osID)), nil
}

// FindInstallerBinary searches user → system → empty (plan §4.3).
// PATH is intentionally not consulted.
func FindInstallerBinary(osID model.OS) (path string, scope model.Scope, ok bool) {
	for _, sc := range []model.Scope{model.ScopeUser, model.ScopeSystem} {
		p, err := InstallerBinaryPath(osID, sc)
		if err != nil {
			continue
		}
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p, sc, true
		}
	}
	return "", "", false
}

// SelfInstallMove builds a Move that copies the currently running executable
// into the canonical installer path for scope. Src must be a real file under
// the staging root (or the caller's verified temp copy) so ValidatePlan passes.
func SelfInstallMove(osID model.OS, scope model.Scope, stagedBinary string) (model.Move, error) {
	dst, err := InstallerBinaryPath(osID, scope)
	if err != nil {
		return model.Move{}, err
	}
	if stagedBinary == "" {
		return model.Move{}, fmt.Errorf("staged installer binary path is empty")
	}
	return model.Move{Src: stagedBinary, Dst: dst}, nil
}

// RunningOS maps runtime.GOOS to model.OS for self-install helpers.
func RunningOS() (model.OS, error) {
	switch runtime.GOOS {
	case "darwin":
		return model.OSMacOS, nil
	case "windows":
		return model.OSWindows, nil
	default:
		return "", fmt.Errorf("self-install unsupported on %s", runtime.GOOS)
	}
}
