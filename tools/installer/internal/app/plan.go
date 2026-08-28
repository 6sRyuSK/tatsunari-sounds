package app

import (
	"fmt"
	"sort"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/release"
)

// SelectedRow is one TUI/headless selection identity. Empty Scope means "use
// Selection.Scope" (fresh installs). Installed rows carry their own Scope so
// updates target that scope only (plan §5.5).
type SelectedRow struct {
	Slug    string
	Variant model.Variant
	Scope   model.Scope
	Version string // empty → catalog latest for the row's channel/variant
}

// Selection captures a user's choices from the TUI (or headless flags).
type Selection struct {
	OS      model.OS
	Rows    []SelectedRow
	Formats []model.Format
	Scope   model.Scope // default scope for rows with empty Scope
	Channel string      // stable | beta | dev
}

// BuildPlanItems resolves a selection against the discovered catalog into the
// concrete download+destination items shown on the confirm screen. Formats not
// offered for a plugin on the target OS are silently skipped (e.g. AU on
// Windows), so the result contains only actionable items.
//
// PlanItem.Destination is the destination *directory* (the bundle's own
// ".vst3"/".component" name is only known after extraction); apply computes the
// full bundle path at install time.
func BuildPlanItems(cat release.Catalog, sel Selection) ([]model.PlanItem, error) {
	channel := sel.Channel
	if channel == "" {
		channel = "stable"
	}
	wantVariant := model.VariantForChannel(channel)

	type pluginKey struct {
		slug    string
		variant model.Variant
	}
	byKey := make(map[pluginKey]model.Plugin, len(cat.Plugins))
	for _, p := range cat.Plugins {
		v := p.Variant
		if v == "" {
			v = model.VariantStable
		}
		// Prefer a catalog row that still has assets; duplicate (slug,variant)
		// rows differ only by installed scope.
		k := pluginKey{p.Slug, v}
		if prev, ok := byKey[k]; !ok || len(p.Available) >= len(prev.Available) {
			byKey[k] = p
		}
	}

	var items []model.PlanItem
	for _, row := range sel.Rows {
		variant := row.Variant
		if variant == "" {
			variant = wantVariant
		}
		p, ok := byKey[pluginKey{row.Slug, variant}]
		if !ok {
			return nil, fmt.Errorf("unknown plugin %q variant %q (channel %s)", row.Slug, variant, channel)
		}
		scope := row.Scope
		if scope == "" {
			scope = sel.Scope
		}
		version := row.Version
		if version == "" {
			version = p.Version
		}
		for _, f := range sel.Formats {
			key := model.AssetKey{OS: sel.OS, Format: f}
			asset, ok := p.Available[key]
			if !ok {
				continue
			}
			dest, err := install.Destination(sel.OS, f, scope)
			if err != nil {
				return nil, err
			}
			action := "install"
			if p.Installed != "" {
				if down, err := isOlder(version, p.Installed); err == nil && !down && version != p.Installed {
					action = "update"
				} else if p.State == model.StateUpdateAvailable && version == p.Version {
					action = "update"
				} else if version != p.Installed {
					action = "update"
				}
			}
			items = append(items, model.PlanItem{
				Slug:        row.Slug,
				Variant:     variant,
				Name:        p.Name,
				Format:      f,
				Scope:       scope,
				Version:     version,
				Channel:     channel,
				Action:      action,
				Asset:       asset,
				Destination: dest,
			})
		}
	}
	sort.Slice(items, func(i, j int) bool {
		if items[i].Slug != items[j].Slug {
			return items[i].Slug < items[j].Slug
		}
		if items[i].Variant != items[j].Variant {
			return items[i].Variant < items[j].Variant
		}
		if items[i].Scope != items[j].Scope {
			return items[i].Scope < items[j].Scope
		}
		return items[i].Format < items[j].Format
	})
	return items, nil
}

func isOlder(a, b string) (bool, error) {
	av, err := model.ParseSemVer(a)
	if err != nil {
		return false, err
	}
	bv, err := model.ParseSemVer(b)
	if err != nil {
		return false, err
	}
	return model.CompareSemVer(av, bv) < 0, nil
}
