//go:build windows

package elevate

import (
	"os/exec"
	"strings"
)

// runElevated re-launches the helper with `-Verb RunAs`, triggering a single
// UAC prompt. The elevated child runs hidden and waits; its exit code is
// propagated so a cancelled UAC surfaces as an error. Result is still read from
// result.json by the caller (the elevated child writes it).
func runElevated(self, planPath, resultPath string) error {
	argList := strings.Join([]string{
		psQuote("__apply"),
		psQuote("--plan"), psQuote(planPath),
		psQuote("--result"), psQuote(resultPath),
	}, ",")

	command := "$ErrorActionPreference='Stop';" +
		"$p = Start-Process -FilePath " + psQuote(self) +
		" -ArgumentList " + argList +
		" -Verb RunAs -WindowStyle Hidden -Wait -PassThru;" +
		"exit $p.ExitCode"

	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command)
	return cmd.Run()
}

// psQuote wraps s in a PowerShell single-quoted string, doubling embedded quotes.
func psQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", "''") + "'"
}
