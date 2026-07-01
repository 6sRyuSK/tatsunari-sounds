// Package elevate applies an install plan under the requested scope. Per-user
// installs run in-process (no prompt); system installs re-invoke this same
// executable as the hidden `__apply` helper under a single native OS elevation
// prompt (Touch ID / password on macOS, UAC on Windows), then read the result
// back across the privilege boundary via a JSON file.
package elevate

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// Apply runs plan under scope. stagingDir is the 0700, user-owned directory
// that already holds the staged bundles; plan.json and result.json are written
// there. For ScopeUser this is a plain in-process apply. For ScopeSystem it
// elevates once and applies every move under that single prompt.
func Apply(scope model.Scope, plan model.InstallPlan, stagingDir string) (model.ApplyResult, error) {
	if scope == model.ScopeUser {
		return install.ApplyPlan(plan), nil
	}

	self, err := os.Executable()
	if err != nil {
		return model.ApplyResult{}, fmt.Errorf("locate self: %w", err)
	}
	self, _ = filepath.Abs(self)

	planPath := filepath.Join(stagingDir, "plan.json")
	resultPath := filepath.Join(stagingDir, "result.json")
	plan.ResultPath = resultPath
	_ = os.Remove(resultPath) // stale result must not be mistaken for success

	data, err := json.MarshalIndent(plan, "", "  ")
	if err != nil {
		return model.ApplyResult{}, err
	}
	if err := os.WriteFile(planPath, data, 0o600); err != nil {
		return model.ApplyResult{}, err
	}

	runErr := runElevated(self, planPath, resultPath)

	// The result file is the source of truth: if the helper ran it exists even
	// when runElevated reports a benign non-zero (e.g. a partial failure it
	// already recorded). Absence means elevation was cancelled or never ran.
	res, readErr := readResult(resultPath)
	if readErr != nil {
		if runErr != nil {
			return model.ApplyResult{}, fmt.Errorf("elevation failed or was cancelled: %w", runErr)
		}
		return model.ApplyResult{}, fmt.Errorf("no result from privileged helper: %w", readErr)
	}
	return res, nil
}

func readResult(path string) (model.ApplyResult, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return model.ApplyResult{}, err
	}
	var res model.ApplyResult
	if err := json.Unmarshal(data, &res); err != nil {
		return model.ApplyResult{}, err
	}
	return res, nil
}
