package install

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

func TestReceiptRoundTrip(t *testing.T) {
	// Force the config dir into a temp location on both platforms.
	dir := t.TempDir()
	t.Setenv("APPDATA", dir) // used first on Windows
	t.Setenv("HOME", dir)    // used on macOS/Linux

	r, err := LoadReceipt()
	if err != nil {
		t.Fatalf("LoadReceipt (empty): %v", err)
	}
	if len(r.Plugins) != 0 {
		t.Fatalf("fresh receipt should be empty, got %d", len(r.Plugins))
	}

	r.Record("saturator", "0.1.3", model.ScopeSystem,
		[]model.Format{model.FormatVST3}, []string{"/Library/Audio/Plug-Ins/VST3/Saturator.vst3"})
	// Add AU later; formats/paths should union, version updates.
	r.Record("saturator", "0.1.3", model.ScopeSystem,
		[]model.Format{model.FormatAU}, []string{"/Library/Audio/Plug-Ins/Components/Saturator.component"})
	if err := r.Save(); err != nil {
		t.Fatalf("Save: %v", err)
	}

	got, err := LoadReceipt()
	if err != nil {
		t.Fatalf("LoadReceipt (saved): %v", err)
	}
	item, ok := got.Plugins["saturator"]
	if !ok {
		t.Fatal("saturator missing after reload")
	}
	if item.Version != "0.1.3" {
		t.Errorf("version = %q", item.Version)
	}
	if len(item.Formats) != 2 {
		t.Errorf("formats should union to 2, got %v", item.Formats)
	}
	if len(item.Paths) != 2 {
		t.Errorf("paths should union to 2, got %v", item.Paths)
	}
	if v := got.InstalledVersions()["saturator"]; v != "0.1.3" {
		t.Errorf("InstalledVersions = %q", v)
	}
}
