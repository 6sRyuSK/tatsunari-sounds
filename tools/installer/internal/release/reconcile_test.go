package release

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

func TestReconcile(t *testing.T) {
	manifest := map[string]string{
		"tn-resonance-suppressor": "0.2.1",
		"nam-player":              "0.1.0",
	}
	assets := ParsePluginAssets(mkRelease(
		"tn-resonance-suppressor-v0_2_1-macOS-AU.zip",
		"tn-resonance-suppressor-v0_2_1-macOS-VST3.zip",
		"tn-resonance-suppressor-v0_2_1-Windows.zip",
		"nam-player-v0_1_0-Windows.zip",
	))
	catalog := map[string]CatalogEntry{
		"tn-resonance-suppressor": {Slug: "tn-resonance-suppressor", Name: "Resonance Suppressor", Category: "EQ"},
		// nam-player intentionally absent -> falls back to title-cased slug
	}
	installed := map[string]string{
		model.EntryKey("tn-resonance-suppressor", model.VariantStable, model.ScopeUser): "0.2.0",
	}

	cat := Reconcile("2026.2", manifest, assets, catalog, installed, nil)
	if cat.Tag != "2026.2" {
		t.Errorf("tag = %q", cat.Tag)
	}
	if len(cat.Plugins) != 2 {
		t.Fatalf("want 2 plugins, got %d", len(cat.Plugins))
	}
	// sorted by slug: nam-player, tn-resonance-suppressor
	if cat.Plugins[0].Slug != "nam-player" || cat.Plugins[1].Slug != "tn-resonance-suppressor" {
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
	if rs.Scope != model.ScopeUser || rs.Installed != "0.2.0" {
		t.Errorf("installed identity lost: scope=%q installed=%q", rs.Scope, rs.Installed)
	}
	if rs.State != model.StateUpdateAvailable {
		t.Errorf("tn-resonance-suppressor state = %v, want UpdateAvailable", rs.State)
	}
	if !rs.HasFormat(model.OSMacOS, model.FormatAU) {
		t.Error("tn-resonance-suppressor should offer macOS AU")
	}
}

func TestReconcileDualScopeRows(t *testing.T) {
	manifest := map[string]string{"tn-resonance-suppressor": "1.0.0"}
	installed := map[string]string{
		model.EntryKey("tn-resonance-suppressor", model.VariantStable, model.ScopeUser):   "0.9.0",
		model.EntryKey("tn-resonance-suppressor", model.VariantStable, model.ScopeSystem): "1.0.0",
	}
	cat := Reconcile("t", manifest, nil, nil, installed, nil)
	if len(cat.Plugins) != 2 {
		t.Fatalf("want 2 rows for dual scope, got %d", len(cat.Plugins))
	}
	var sawUser, sawSystem bool
	for _, p := range cat.Plugins {
		switch p.Scope {
		case model.ScopeUser:
			sawUser = true
			if p.State != model.StateUpdateAvailable {
				t.Errorf("user row state = %v", p.State)
			}
		case model.ScopeSystem:
			sawSystem = true
			if p.State != model.StateUpToDate {
				t.Errorf("system row state = %v", p.State)
			}
		}
	}
	if !sawUser || !sawSystem {
		t.Fatalf("missing scope rows: %+v", cat.Plugins)
	}
}
