//go:build !darwin && !windows

package elevate

import "fmt"

// runElevated is unsupported off Windows/macOS. The installer only targets
// those two OSes; this stub exists so the package still builds on Linux CI
// (where only per-user, in-process applies and headless dry-runs are exercised).
func runElevated(self, planPath, resultPath string) error {
	return fmt.Errorf("system-scope elevation is not supported on this platform")
}
