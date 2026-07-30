package app

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/release"
)

func testCatalog() release.Catalog {
	manifest := map[string]string{"resonance-suppressor": "0.2.1", "nam-player": "0.1.0"}
	assets := release.ParsePluginAssets(&release.Release{Tag: "2026.2", Assets: []model.Asset{
		{Name: "resonance-suppressor-v0_2_1-macOS-AU.zip", DownloadURL: "https://x/rs-au"},
		{Name: "resonance-suppressor-v0_2_1-macOS-VST3.zip", DownloadURL: "https://x/rs-vst3"},
		{Name: "resonance-suppressor-v0_2_1-Windows.zip", DownloadURL: "https://x/rs-win"},
		{Name: "nam-player-v0_1_0-Windows.zip", DownloadURL: "https://x/nam-win"},
	}})
	installed := map[string]string{
		model.EntryKey("resonance-suppressor", model.VariantStable, model.ScopeUser): "0.2.0",
	}
	return release.Reconcile("2026.2", manifest, assets, nil, installed, nil)
}

func TestBuildPlanItemsMacOS(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	cat := testCatalog()
	items, err := BuildPlanItems(cat, Selection{
		OS: model.OSMacOS,
		Rows: []SelectedRow{{
			Slug: "resonance-suppressor", Variant: model.VariantStable,
			Scope: model.ScopeUser, Version: "0.2.1",
		}},
		Formats: []model.Format{model.FormatVST3, model.FormatAU},
		Scope:   model.ScopeUser,
		Channel: "stable",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 {
		t.Fatalf("want 2 items (VST3+AU), got %d", len(items))
	}
	for _, it := range items {
		if it.Action != "update" {
			t.Errorf("expected update action, got %q", it.Action)
		}
		if it.Version != "0.2.1" || it.Channel != "stable" || it.Variant != model.VariantStable {
			t.Errorf("plan identity not carried: %+v", it)
		}
		if it.Scope != model.ScopeUser {
			t.Errorf("scope = %q, want user (from row)", it.Scope)
		}
	}
}

func TestBuildPlanItemsWindowsSkipsAU(t *testing.T) {
	t.Setenv("CommonProgramFiles", `C:\Program Files\Common Files`)
	cat := testCatalog()
	items, err := BuildPlanItems(cat, Selection{
		OS: model.OSWindows,
		Rows: []SelectedRow{{
			Slug: "resonance-suppressor", Variant: model.VariantStable, Scope: model.ScopeSystem,
		}},
		Formats: []model.Format{model.FormatVST3, model.FormatAU},
		Scope:   model.ScopeSystem,
		Channel: "stable",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 1 {
		t.Fatalf("want 1 item (VST3 only on Windows), got %d", len(items))
	}
	if items[0].Format != model.FormatVST3 {
		t.Errorf("got format %q", items[0].Format)
	}
}

func TestBuildPlanItemsFreshInstall(t *testing.T) {
	t.Setenv("CommonProgramFiles", `C:\Program Files\Common Files`)
	cat := testCatalog()
	items, err := BuildPlanItems(cat, Selection{
		OS: model.OSWindows,
		Rows: []SelectedRow{{
			Slug: "nam-player", Variant: model.VariantStable,
		}},
		Formats: []model.Format{model.FormatVST3},
		Scope:   model.ScopeSystem,
		Channel: "stable",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 1 || items[0].Action != "install" {
		t.Fatalf("expected 1 fresh install item, got %+v", items)
	}
	if items[0].Scope != model.ScopeSystem {
		t.Errorf("fresh install should take Selection.Scope, got %q", items[0].Scope)
	}
}

func TestBuildPlanItemsChannelDevRequiresVariant(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	cat := testCatalog()
	_, err := BuildPlanItems(cat, Selection{
		OS:      model.OSMacOS,
		Rows:    []SelectedRow{{Slug: "resonance-suppressor"}},
		Formats: []model.Format{model.FormatVST3},
		Scope:   model.ScopeUser,
		Channel: "dev",
	})
	if err == nil {
		t.Fatal("dev channel with only stable catalog row should error")
	}
}

func TestBuildPlanItemsPickedVersionCarried(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	cat := testCatalog()
	items, err := BuildPlanItems(cat, Selection{
		OS: model.OSMacOS,
		Rows: []SelectedRow{{
			Slug: "resonance-suppressor", Variant: model.VariantStable,
			Scope: model.ScopeUser, Version: "0.2.0",
		}},
		Formats: []model.Format{model.FormatVST3},
		Scope:   model.ScopeUser,
		Channel: "stable",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 1 || items[0].Version != "0.2.0" {
		t.Fatalf("picked version not on plan: %+v", items)
	}
}
