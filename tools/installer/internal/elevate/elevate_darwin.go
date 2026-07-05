//go:build darwin

package elevate

import "os/exec"

// runElevated asks osascript to run the helper "with administrator privileges",
// which shows the native macOS auth dialog (Touch ID / password) exactly once.
// The command string is assembled (and its arguments quoted) by the pure,
// unit-tested buildDarwinScript.
func runElevated(self, planPath, resultPath string) error {
	script := buildDarwinScript(self, planPath, resultPath)
	return exec.Command("/usr/bin/osascript", "-e", script).Run()
}
