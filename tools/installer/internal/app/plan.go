package app

import (
	"fmt"
	"sort"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

// Selection captures a user's choices from the TUI (or headless flags).
type Selection struct {
	OS      model.OS
	Slugs   []string
	Formats []model.Format
	Scope   model.Scope
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
	bySlug := make(map[string]model.Plugin, len(cat.Plugins))
	for _, p := range cat.Plugins {
		bySlug[p.Slug] = p
	}

	var items []model.PlanItem
	for _, slug := range sel.Slugs {
		p, ok := bySlug[slug]
		if !ok {
			return nil, fmt.Errorf("unknown plugin %q", slug)
		}
		for _, f := range sel.Formats {
			key := model.AssetKey{OS: sel.OS, Format: f}
			asset, ok := p.Available[key]
			if !ok {
				continue // format not offered for this plugin on this OS
			}
			dest, err := install.Destination(sel.OS, f, sel.Scope)
			if err != nil {
				return nil, err
			}
			action := "install"
			if p.State == model.StateUpdateAvailable {
				action = "update"
			}
			items = append(items, model.PlanItem{
				Slug:        slug,
				Name:        p.Name,
				Format:      f,
				Scope:       sel.Scope,
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
		return items[i].Format < items[j].Format
	})
	return items, nil
}
