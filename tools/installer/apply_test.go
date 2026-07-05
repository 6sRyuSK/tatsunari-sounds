package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// withInstallRoots overrides the install-root allowlist seam for one test and
// restores it afterward. On Linux CI the real roots are empty, so the apply
// orchestration can only be exercised through this seam.
func withInstallRoots(t *testing.T, roots []string) {
	t.Helper()
	orig := installRootsFn
	installRootsFn = func() []string { return roots }
	t.Cleanup(func() { installRootsFn = orig })
}

// writePlan stages a plan.json in dir and returns its path and the result path.
func writePlan(t *testing.T, dir string, plan model.InstallPlan) (planPath, resultPath string) {
	t.Helper()
	planPath = filepath.Join(dir, "plan.json")
	resultPath = filepath.Join(dir, "result.json")
	data, err := json.MarshalIndent(plan, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(planPath, data, 0o600); err != nil {
		t.Fatal(err)
	}
	return planPath, resultPath
}

func readApplyResult(t *testing.T, path string) model.ApplyResult {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read result: %v", err)
	}
	var res model.ApplyResult
	if err := json.Unmarshal(data, &res); err != nil {
		t.Fatalf("parse result: %v", err)
	}
	return res
}

// stageBundle creates a staged bundle directory (a fake .vst3) with one file.
func stageBundle(t *testing.T, dir, name string) string {
	t.Helper()
	bundle := filepath.Join(dir, name)
	if err := os.MkdirAll(bundle, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bundle, "binary"), []byte("payload"), 0o644); err != nil {
		t.Fatal(err)
	}
	return bundle
}

func TestRunApplyHappyPath(t *testing.T) {
	staging := t.TempDir()
	root := t.TempDir()
	withInstallRoots(t, []string{root})

	src := stageBundle(t, staging, "resonance-suppressor.vst3")
	dst := filepath.Join(root, "resonance-suppressor.vst3")
	planPath, resultPath := writePlan(t, staging, model.InstallPlan{
		Moves: []model.Move{{Src: src, Dst: dst}},
	})

	code := runApply([]string{"--plan", planPath, "--result", resultPath})
	if code != 0 {
		t.Fatalf("exit code = %d, want 0", code)
	}

	res := readApplyResult(t, resultPath)
	if len(res.Errors) != 0 {
		t.Fatalf("unexpected errors: %v", res.Errors)
	}
	if len(res.Installed) != 1 || res.Installed[0] != dst {
		t.Fatalf("Installed = %v, want [%s]", res.Installed, dst)
	}
	// The bundle must actually have been copied to the destination.
	if _, err := os.Stat(filepath.Join(dst, "binary")); err != nil {
		t.Fatalf("bundle not installed at dst: %v", err)
	}
}

func TestRunApplyRejectsDestinationOutsideRoots(t *testing.T) {
	staging := t.TempDir()
	root := t.TempDir()
	withInstallRoots(t, []string{root})

	src := stageBundle(t, staging, "evil.vst3")
	// Destination is NOT under the allowed root -> ValidatePlan must reject.
	dst := filepath.Join(t.TempDir(), "elsewhere", "evil.vst3")
	planPath, resultPath := writePlan(t, staging, model.InstallPlan{
		Moves: []model.Move{{Src: src, Dst: dst}},
	})

	code := runApply([]string{"--plan", planPath, "--result", resultPath})
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
	res := readApplyResult(t, resultPath)
	if len(res.Errors) == 0 {
		t.Fatal("expected a rejection error recorded in result")
	}
	if len(res.Installed) != 0 {
		t.Fatalf("nothing should be installed on rejection, got %v", res.Installed)
	}
	// Nothing was written to the destination.
	if _, err := os.Stat(dst); !os.IsNotExist(err) {
		t.Fatalf("rejected destination should not exist: err=%v", err)
	}
}

func TestRunApplyRejectsSourceOutsideStaging(t *testing.T) {
	staging := t.TempDir()
	root := t.TempDir()
	withInstallRoots(t, []string{root})

	// Source lives outside the staging dir (dir holding plan.json).
	src := stageBundle(t, t.TempDir(), "outside.vst3")
	dst := filepath.Join(root, "outside.vst3")
	planPath, resultPath := writePlan(t, staging, model.InstallPlan{
		Moves: []model.Move{{Src: src, Dst: dst}},
	})

	code := runApply([]string{"--plan", planPath, "--result", resultPath})
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
	res := readApplyResult(t, resultPath)
	if len(res.Errors) == 0 {
		t.Fatal("expected a rejection error for out-of-staging source")
	}
}

func TestRunApplyPropagatesApplyErrors(t *testing.T) {
	staging := t.TempDir()
	root := t.TempDir()
	withInstallRoots(t, []string{root})

	// A valid (allowlisted) plan whose source does not exist: passes validation
	// but ApplyPlan fails to copy, so runApply exits 1 with the error recorded.
	src := filepath.Join(staging, "missing.vst3")
	dst := filepath.Join(root, "missing.vst3")
	planPath, resultPath := writePlan(t, staging, model.InstallPlan{
		Moves: []model.Move{{Src: src, Dst: dst}},
	})

	code := runApply([]string{"--plan", planPath, "--result", resultPath})
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
	res := readApplyResult(t, resultPath)
	if len(res.Errors) == 0 {
		t.Fatal("expected an apply error recorded in result")
	}
	if len(res.Installed) != 0 {
		t.Fatalf("nothing installed on apply failure, got %v", res.Installed)
	}
}

func TestRunApplyMissingArgs(t *testing.T) {
	cases := [][]string{
		{},
		{"--plan", "/x/plan.json"},
		{"--result", "/x/result.json"},
	}
	for _, args := range cases {
		if code := runApply(args); code != 2 {
			t.Errorf("runApply(%v) = %d, want 2", args, code)
		}
	}
}

func TestRunApplyUnreadablePlan(t *testing.T) {
	dir := t.TempDir()
	code := runApply([]string{"--plan", filepath.Join(dir, "nope.json"), "--result", filepath.Join(dir, "result.json")})
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
}

func TestRunApplyMalformedPlan(t *testing.T) {
	dir := t.TempDir()
	planPath := filepath.Join(dir, "plan.json")
	if err := os.WriteFile(planPath, []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	code := runApply([]string{"--plan", planPath, "--result", filepath.Join(dir, "result.json")})
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
}
