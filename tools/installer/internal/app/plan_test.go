package app

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

func testCatalog() release.Catalog {
	manifest := map[string]string{"resonance-suppressor": "0.2.1", "nam-player": "0.1.0"}
	assets := release.ParsePluginAssets(&release.Release{Tag: "2026.2", Assets: []model.Asset{
		{Name: "resonance-suppressor-v0_2_1-macOS-AU.zip", DownloadURL: "https://x/rs-au"},
		{Name: "resonance-suppressor-v0_2_1-macOS-VST3.zip", DownloadURL: "https://x/rs-vst3"},
		{Name: "resonance-suppressor-v0_2_1-Windows.zip", DownloadURL: "https://x/rs-win"},
		{Name: "nam-player-v0_1_0-Windows.zip", DownloadURL: "https://x/nam-win"},
	}})
	installed := map[string]string{"resonance-suppressor": "0.2.0"} // update available
	return release.Reconcile("2026.2", manifest, assets, nil, installed, nil)
}

func TestBuildPlanItemsMacOS(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	cat := testCatalog()
	items, err := BuildPlanItems(cat, Selection{
		OS:      model.OSMacOS,
		Slugs:   []string{"resonance-suppressor"},
		Formats: []model.Format{model.FormatVST3, model.FormatAU},
		Scope:   model.ScopeUser,
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 {
		t.Fatalf("want 2 items (VST3+AU), got %d", len(items))
	}
	for _, it := range items {
		if it.Action != "update" {
			t.Errorf("expected update action (installed 0.2.0 < 0.2.1), got %q", it.Action)
		}
	}
}

func TestBuildPlanItemsWindowsSkipsAU(t *testing.T) {
	t.Setenv("CommonProgramFiles", `C:\Program Files\Common Files`)
	cat := testCatalog()
	items, err := BuildPlanItems(cat, Selection{
		OS:      model.OSWindows,
		Slugs:   []string{"resonance-suppressor"},
		Formats: []model.Format{model.FormatVST3, model.FormatAU},
		Scope:   model.ScopeSystem,
	})
	if err != nil {
		t.Fatal(err)
	}
	// Only VST3 exists on Windows; AU has no asset and is skipped.
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
		OS:      model.OSWindows,
		Slugs:   []string{"nam-player"},
		Formats: []model.Format{model.FormatVST3},
		Scope:   model.ScopeSystem,
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 1 || items[0].Action != "install" {
		t.Fatalf("expected 1 fresh install item, got %+v", items)
	}
}
