package install

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
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

	r.Record("saturator", model.VariantStable, "0.1.3", model.ScopeSystem,
		[]model.Format{model.FormatVST3}, []string{"/Library/Audio/Plug-Ins/VST3/Saturator.vst3"})
	// Add AU later; formats/paths should union, version updates.
	r.Record("saturator", model.VariantStable, "0.1.3", model.ScopeSystem,
		[]model.Format{model.FormatAU}, []string{"/Library/Audio/Plug-Ins/Components/Saturator.component"})
	if err := r.Save(); err != nil {
		t.Fatalf("Save: %v", err)
	}

	got, err := LoadReceipt()
	if err != nil {
		t.Fatalf("LoadReceipt (saved): %v", err)
	}
	key := EntryKey("saturator", model.VariantStable, model.ScopeSystem)
	item, ok := got.Entries[key]
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
	if v := got.InstalledVersions()[EntryKey("saturator", model.VariantStable, model.ScopeSystem)]; v != "0.1.3" {
		t.Errorf("InstalledVersions = %q", v)
	}
}

func TestReceiptV1MigrationAndDualScope(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("APPDATA", dir)
	t.Setenv("HOME", dir)

	legacy := []byte(`{
  "schema": 1,
  "updatedAt": "2026-01-01T00:00:00Z",
  "plugins": {
    "saturator": {
      "version": "0.1.0",
      "formats": ["VST3"],
      "scope": "user",
      "paths": ["/tmp/Saturator.vst3"]
    }
  }
}`)
	path, err := ReceiptPath()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, legacy, 0o600); err != nil {
		t.Fatal(err)
	}
	got, err := LoadReceipt()
	if err != nil {
		t.Fatal(err)
	}
	key := EntryKey("saturator", model.VariantStable, model.ScopeUser)
	if got.Entries[key].Version != "0.1.0" {
		t.Fatalf("migrated entry: %+v", got.Entries)
	}
	got.Record("saturator", model.VariantStable, "0.1.0", model.ScopeSystem,
		[]model.Format{model.FormatVST3}, []string{"/Library/Audio/Plug-Ins/VST3/Saturator.vst3"})
	warns := got.DualScopeWarningSlugs()
	if len(warns) != 1 {
		t.Fatalf("dual-scope warnings = %v", warns)
	}
}
