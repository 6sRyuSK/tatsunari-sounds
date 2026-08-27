package install

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
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

// TestApplyPlanWritesSystemReceipt is the gate for the privileged receipt
// write: a system-scope apply must record what it installed while it still
// holds elevation, because the receipt directory is root-owned and a second
// elevation prompt is not allowed.
func TestApplyPlanWritesSystemReceipt(t *testing.T) {
	stage, dest := t.TempDir(), t.TempDir()
	receipt := filepath.Join(t.TempDir(), receiptFileName)

	vst3 := stageBundle(t, stage, "TN Equalizer.vst3")
	au := stageBundle(t, stage, "TN Equalizer.component")
	ref := func(f model.Format) *model.ReceiptRef {
		return &model.ReceiptRef{
			Slug: "tn-equalizer", Variant: model.VariantStable,
			Version: "0.1.0", Format: f, Scope: model.ScopeSystem,
		}
	}
	plan := model.InstallPlan{
		ReceiptPath: receipt,
		Moves: []model.Move{
			{Src: vst3, Dst: filepath.Join(dest, "TN Equalizer.vst3"), Receipt: ref(model.FormatVST3)},
			{Src: au, Dst: filepath.Join(dest, "TN Equalizer.component"), Receipt: ref(model.FormatAU)},
		},
	}
	res := ApplyPlan(plan)
	if len(res.Errors) != 0 {
		t.Fatalf("unexpected errors: %v", res.Errors)
	}

	r, err := LoadReceiptAt(receipt)
	if err != nil {
		t.Fatal(err)
	}
	key := EntryKey("tn-equalizer", model.VariantStable, model.ScopeSystem)
	e, ok := r.Entries[key]
	if !ok {
		t.Fatalf("no entry for %s; have %v", key, r.Entries)
	}
	if e.Version != "0.1.0" || e.Scope != string(model.ScopeSystem) {
		t.Errorf("entry = %+v", e)
	}
	if len(e.Formats) != 2 {
		t.Errorf("both formats should collapse into one entry, got %v", e.Formats)
	}
	if len(e.Paths) != 2 {
		t.Errorf("entry should record the real destinations, got %v", e.Paths)
	}

	// World-readable: another user must be able to reconcile the system install.
	st, err := os.Stat(receipt)
	if err != nil {
		t.Fatal(err)
	}
	if st.Mode().Perm() != 0o644 {
		t.Errorf("system receipt mode = %v, want 0644", st.Mode().Perm())
	}
}

// A move that failed must not appear in the receipt: a receipt claiming an
// install that never landed hides the plugin from the next run's repair.
func TestApplyPlanReceiptOmitsFailedMoves(t *testing.T) {
	stage, dest := t.TempDir(), t.TempDir()
	receipt := filepath.Join(t.TempDir(), receiptFileName)

	good := stageBundle(t, stage, "Good.vst3")
	plan := model.InstallPlan{
		ReceiptPath: receipt,
		Moves: []model.Move{
			{Src: good, Dst: filepath.Join(dest, "Good.vst3"), Receipt: &model.ReceiptRef{
				Slug: "good", Variant: model.VariantStable, Version: "1.0.0",
				Format: model.FormatVST3, Scope: model.ScopeSystem}},
			{Src: filepath.Join(stage, "does-not-exist.vst3"), Dst: filepath.Join(dest, "Bad.vst3"),
				Receipt: &model.ReceiptRef{
					Slug: "bad", Variant: model.VariantStable, Version: "1.0.0",
					Format: model.FormatVST3, Scope: model.ScopeSystem}},
		},
	}
	res := ApplyPlan(plan)
	if len(res.Errors) == 0 {
		t.Fatal("the missing source must be reported")
	}

	r, err := LoadReceiptAt(receipt)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := r.Entries[EntryKey("good", model.VariantStable, model.ScopeSystem)]; !ok {
		t.Error("the successful move should be recorded")
	}
	if _, ok := r.Entries[EntryKey("bad", model.VariantStable, model.ScopeSystem)]; ok {
		t.Error("the failed move must NOT be recorded")
	}
}

// An existing system receipt must be merged, not clobbered: installing one
// product may not erase the record of every other product on the machine.
func TestApplyPlanReceiptMergesExisting(t *testing.T) {
	stage, dest := t.TempDir(), t.TempDir()
	receipt := filepath.Join(t.TempDir(), receiptFileName)

	prior := &Receipt{Schema: receiptSchema, Entries: map[string]ReceiptEntry{}}
	prior.Record("tn-vocal-tuner", model.VariantStable, "0.1.0", model.ScopeSystem,
		[]model.Format{model.FormatVST3}, []string{"/somewhere/TN Vocal Tuner.vst3"})
	if err := prior.SaveTo(receipt, 0o755, 0o644); err != nil {
		t.Fatal(err)
	}

	b := stageBundle(t, stage, "TN Equalizer.vst3")
	res := ApplyPlan(model.InstallPlan{
		ReceiptPath: receipt,
		Moves: []model.Move{{Src: b, Dst: filepath.Join(dest, "TN Equalizer.vst3"),
			Receipt: &model.ReceiptRef{
				Slug: "tn-equalizer", Variant: model.VariantStable, Version: "0.1.0",
				Format: model.FormatVST3, Scope: model.ScopeSystem}}},
	})
	if len(res.Errors) != 0 {
		t.Fatalf("unexpected errors: %v", res.Errors)
	}

	r, err := LoadReceiptAt(receipt)
	if err != nil {
		t.Fatal(err)
	}
	for _, slug := range []string{"tn-vocal-tuner", "tn-equalizer"} {
		if _, ok := r.Entries[EntryKey(slug, model.VariantStable, model.ScopeSystem)]; !ok {
			t.Errorf("%s missing after merge; have %v", slug, r.Entries)
		}
	}
}

// ValidatePlan is the security gate: the receipt is the one non-bundle file the
// privileged applier writes, so its path gets the same allowlist treatment as a
// destination plus a fixed filename.
func TestValidatePlanReceiptPath(t *testing.T) {
	root := t.TempDir()
	staging := t.TempDir()
	src := stageBundle(t, staging, "X.vst3")
	base := model.InstallPlan{Moves: []model.Move{{Src: src, Dst: filepath.Join(root, "X.vst3")}}}

	ok := base
	ok.ReceiptPath = filepath.Join(root, receiptFileName)
	if err := ValidatePlan(ok, []string{root}, staging); err != nil {
		t.Fatalf("an in-root receipt.json must be accepted: %v", err)
	}

	outside := base
	outside.ReceiptPath = filepath.Join(t.TempDir(), receiptFileName)
	if err := ValidatePlan(outside, []string{root}, staging); err == nil {
		t.Error("a receipt outside the install roots must be rejected")
	}

	renamed := base
	renamed.ReceiptPath = filepath.Join(root, "authorized_keys")
	if err := ValidatePlan(renamed, []string{root}, staging); err == nil {
		t.Error("a receipt path with another filename must be rejected")
	}

	none := base
	if err := ValidatePlan(none, []string{root}, staging); err != nil {
		t.Errorf("an empty receipt path (user scope) must stay valid: %v", err)
	}
}
