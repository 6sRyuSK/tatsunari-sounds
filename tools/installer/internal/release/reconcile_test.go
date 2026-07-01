package release

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

func TestReconcile(t *testing.T) {
	manifest := map[string]string{
		"resonance-suppressor": "0.2.1",
		"nam-player":           "0.1.0",
	}
	assets := ParsePluginAssets(mkRelease(
		"resonance-suppressor-v0_2_1-macOS-AU.zip",
		"resonance-suppressor-v0_2_1-macOS-VST3.zip",
		"resonance-suppressor-v0_2_1-Windows.zip",
		"nam-player-v0_1_0-Windows.zip",
	))
	catalog := map[string]CatalogEntry{
		"resonance-suppressor": {Slug: "resonance-suppressor", Name: "Resonance Suppressor", Category: "EQ"},
		// nam-player intentionally absent -> falls back to title-cased slug
	}
	installed := map[string]string{
		"resonance-suppressor": "0.2.0", // update available
		// nam-player not installed
	}

	cat := Reconcile("2026.2", manifest, assets, catalog, installed, nil)
	if cat.Tag != "2026.2" {
		t.Errorf("tag = %q", cat.Tag)
	}
	if len(cat.Plugins) != 2 {
		t.Fatalf("want 2 plugins, got %d", len(cat.Plugins))
	}
	// sorted by slug: nam-player, resonance-suppressor
	if cat.Plugins[0].Slug != "nam-player" || cat.Plugins[1].Slug != "resonance-suppressor" {
		t.Fatalf("unexpected order: %s, %s", cat.Plugins[0].Slug, cat.Plugins[1].Slug)
	}
	nam := cat.Plugins[0]
	if nam.Name != "Nam Player" {
		t.Errorf("nam-player fallback name = %q, want Nam Player", nam.Name)
	}
	if nam.State != model.StateNotInstalled {
		t.Errorf("nam-player state = %v, want NotInstalled", nam.State)
	}
	rs := cat.Plugins[1]
	if rs.Name != "Resonance Suppressor" || rs.Category != "EQ" {
		t.Errorf("catalog metadata not applied: %+v", rs)
	}
	if rs.State != model.StateUpdateAvailable {
		t.Errorf("resonance-suppressor state = %v, want UpdateAvailable", rs.State)
	}
	if !rs.HasFormat(model.OSMacOS, model.FormatAU) {
		t.Error("resonance-suppressor should offer macOS AU")
	}
}
