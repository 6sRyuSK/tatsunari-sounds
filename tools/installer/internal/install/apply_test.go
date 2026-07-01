package install

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// stageBundle creates a fake bundle dir with a file, returning its path.
func stageBundle(t *testing.T, root, name string) string {
	t.Helper()
	b := filepath.Join(root, name)
	if err := os.MkdirAll(filepath.Join(b, "Contents"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(b, "Contents", "info"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	return b
}

func TestApplyPlanCopiesAndReplaces(t *testing.T) {
	staging := t.TempDir()
	destRoot := t.TempDir()

	src := stageBundle(t, staging, "Thing.vst3")
	dst := filepath.Join(destRoot, "Thing.vst3")

	// Pre-existing stale bundle to prove replacement.
	if err := os.MkdirAll(dst, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dst, "stale"), []byte("old"), 0o644); err != nil {
		t.Fatal(err)
	}

	plan := model.InstallPlan{Moves: []model.Move{{Src: src, Dst: dst}}}
	res := ApplyPlan(plan)
	if len(res.Errors) != 0 {
		t.Fatalf("unexpected errors: %v", res.Errors)
	}
	if len(res.Installed) != 1 || res.Installed[0] != dst {
		t.Fatalf("installed = %v", res.Installed)
	}
	if _, err := os.Stat(filepath.Join(dst, "Contents", "info")); err != nil {
		t.Errorf("new content missing: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dst, "stale")); !os.IsNotExist(err) {
		t.Error("stale file from old bundle should be gone after replace")
	}
}

func TestValidatePlan(t *testing.T) {
	staging := filepath.Clean(t.TempDir())
	root := filepath.Clean(t.TempDir())
	roots := []string{root}

	good := model.InstallPlan{
		Moves:      []model.Move{{Src: filepath.Join(staging, "A.vst3"), Dst: filepath.Join(root, "A.vst3")}},
		Quarantine: []string{filepath.Join(root, "A.vst3")},
	}
	if err := ValidatePlan(good, roots, staging); err != nil {
		t.Errorf("valid plan rejected: %v", err)
	}

	// Destination outside allowed roots.
	badDst := model.InstallPlan{Moves: []model.Move{{Src: filepath.Join(staging, "A.vst3"), Dst: filepath.Join(t.TempDir(), "A.vst3")}}}
	if err := ValidatePlan(badDst, roots, staging); err == nil {
		t.Error("expected rejection: dst outside install roots")
	}

	// Source outside the staging dir (e.g. attacker-supplied absolute path).
	badSrc := model.InstallPlan{Moves: []model.Move{{Src: "/etc/passwd", Dst: filepath.Join(root, "A.vst3")}}}
	if err := ValidatePlan(badSrc, roots, staging); err == nil {
		t.Error("expected rejection: src outside staging root")
	}

	// Quarantine target outside install roots.
	badQ := model.InstallPlan{
		Moves:      good.Moves,
		Quarantine: []string{"/Library/LaunchDaemons/evil"},
	}
	if err := ValidatePlan(badQ, roots, staging); err == nil {
		t.Error("expected rejection: quarantine target outside install roots")
	}
}
