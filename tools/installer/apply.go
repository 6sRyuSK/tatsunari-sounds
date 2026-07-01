package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// runApply is the privileged file-mover, re-invoked as
// `__apply --plan <f> --result <f>` under OS elevation (macOS osascript /
// Windows RunAs) or in-process. It reads the plan, re-validates every path
// against the install-root allowlist, applies it, and writes an ApplyResult so
// the unprivileged parent can report outcomes and write the receipt.
func runApply(args []string) int {
	var planPath, resultPath string
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--plan":
			if i+1 < len(args) {
				i++
				planPath = args[i]
			}
		case "--result":
			if i+1 < len(args) {
				i++
				resultPath = args[i]
			}
		}
	}
	if planPath == "" || resultPath == "" {
		fmt.Fprintln(os.Stderr, "__apply: --plan and --result are required")
		return 2
	}

	data, err := os.ReadFile(planPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "__apply: read plan:", err)
		return 1
	}
	var plan model.InstallPlan
	if err := json.Unmarshal(data, &plan); err != nil {
		fmt.Fprintln(os.Stderr, "__apply: parse plan:", err)
		return 1
	}

	// Security gate: destinations must be under a known install root, sources
	// under the staging dir (the directory holding plan.json).
	stagingRoot := filepath.Dir(planPath)
	roots := install.InstallRoots(applyOS())
	if err := install.ValidatePlan(plan, roots, stagingRoot); err != nil {
		writeResult(resultPath, model.ApplyResult{Errors: []string{err.Error()}})
		fmt.Fprintln(os.Stderr, "__apply: rejected plan:", err)
		return 1
	}

	res := install.ApplyPlan(plan)
	if err := writeResult(resultPath, res); err != nil {
		fmt.Fprintln(os.Stderr, "__apply: write result:", err)
		return 1
	}
	if len(res.Errors) > 0 {
		return 1
	}
	return 0
}

func writeResult(path string, res model.ApplyResult) error {
	data, err := json.MarshalIndent(res, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

// applyOS maps the running platform to the model OS used for the install-root
// allowlist. The helper always runs on the target machine.
func applyOS() model.OS {
	switch runtime.GOOS {
	case "darwin":
		return model.OSMacOS
	case "windows":
		return model.OSWindows
	default:
		return ""
	}
}
