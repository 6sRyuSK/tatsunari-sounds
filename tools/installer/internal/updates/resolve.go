package updates

import (
	"fmt"
	"sort"
	"strings"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// ChannelID identifies a release channel.
type ChannelID string

const (
	ChannelStable ChannelID = "stable"
	ChannelBeta   ChannelID = "beta"
	ChannelDev    ChannelID = "dev"
)

// VariantID identifies a coinstallable plugin variant.
type VariantID string

const (
	VariantStable VariantID = "stable"
	VariantDev    VariantID = "dev"
)

// SelectionKey is the install identity (plan §1 / §5.5).
type SelectionKey struct {
	Slug    string
	Variant VariantID
	Scope   model.Scope
}

// VersionChoice is a resolved non-yanked version for one plugin row.
type VersionChoice struct {
	Plugin   Plugin
	Version  Version
	Reason   string // empty unless disabled
	Disabled bool
}

// ResolveOptions controls version listing for the TUI (plan §1.6).
type ResolveOptions struct {
	Channel       ChannelID
	ClientVersion string // installer semver; empty skips minClientVersion checks
	IncludeYanked bool
	PreferLatest  bool // when true, only return the latest usable version
}

// ListVersions returns non-yanked versions for a plugin in newest-first order.
// Rows that require a newer client are returned Disabled with Reason set; other
// rows stay usable (invariant 2).
func ListVersions(p Plugin, opt ResolveOptions) []VersionChoice {
	channel := string(opt.Channel)
	if channel == "" {
		channel = string(ChannelStable)
	}
	type scored struct {
		v  Version
		sv model.SemVer
	}
	var list []scored
	for _, v := range p.Versions {
		if !opt.IncludeYanked && v.Yanked {
			continue
		}
		if v.Channel != channel && !(opt.Channel == ChannelStable && v.Channel == "stable") {
			// For stable channel, only stable versions; for others match exactly.
			if v.Channel != channel {
				continue
			}
		}
		sv, err := model.ParseSemVer(stripPrerelease(v.Version))
		if err != nil {
			continue
		}
		// Stable latest selection excludes prerelease tags on the version string.
		if opt.Channel == ChannelStable && hasPrerelease(v.Version) {
			continue
		}
		list = append(list, scored{v: v, sv: sv})
	}
	sort.Slice(list, func(i, j int) bool {
		return model.CompareSemVer(list[i].sv, list[j].sv) > 0
	})
	out := make([]VersionChoice, 0, len(list))
	for _, s := range list {
		vc := VersionChoice{Plugin: p, Version: s.v}
		if p.MinClientVersion != "" && opt.ClientVersion != "" {
			if cmp, err := compareSemVerStrings(opt.ClientVersion, p.MinClientVersion); err == nil && cmp < 0 {
				vc.Disabled = true
				vc.Reason = fmt.Sprintf("requires client >= %s", p.MinClientVersion)
			}
		}
		out = append(out, vc)
		if opt.PreferLatest && !vc.Disabled {
			return out[:len(out):len(out)]
		}
	}
	return out
}

// LatestUsable returns the newest non-disabled version for the channel, or false.
func LatestUsable(p Plugin, opt ResolveOptions) (VersionChoice, bool) {
	opt.PreferLatest = false
	for _, vc := range ListVersions(p, opt) {
		if !vc.Disabled && len(vc.Version.Assets) > 0 {
			return vc, true
		}
	}
	return VersionChoice{}, false
}

// IsDowngrade reports whether selected is older than installed (semver).
func IsDowngrade(installed, selected string) (bool, error) {
	if installed == "" {
		return false, nil
	}
	a, err := model.ParseSemVer(stripPrerelease(installed))
	if err != nil {
		return false, err
	}
	b, err := model.ParseSemVer(stripPrerelease(selected))
	if err != nil {
		return false, err
	}
	return model.CompareSemVer(b, a) < 0, nil
}

// StateCompatDecreases reports whether selected.stateCompatVersion is lower than
// installedStateCompat (exact downgrade warning signal; not inferred from major).
func StateCompatDecreases(installedStateCompat, selectedStateCompat string) (bool, error) {
	if installedStateCompat == "" || selectedStateCompat == "" {
		return false, nil
	}
	a, err := model.ParseSemVer(stripPrerelease(installedStateCompat))
	if err != nil {
		return false, err
	}
	b, err := model.ParseSemVer(stripPrerelease(selectedStateCompat))
	if err != nil {
		return false, err
	}
	return model.CompareSemVer(b, a) < 0, nil
}

func hasPrerelease(v string) bool {
	return strings.Contains(v, "-")
}

func stripPrerelease(v string) string {
	v = strings.TrimSpace(v)
	v = strings.TrimPrefix(v, "v")
	if i := strings.IndexAny(v, "-+"); i >= 0 {
		return v[:i]
	}
	return v
}

func compareSemVerStrings(a, b string) (int, error) {
	av, err := model.ParseSemVer(stripPrerelease(a))
	if err != nil {
		return 0, err
	}
	bv, err := model.ParseSemVer(stripPrerelease(b))
	if err != nil {
		return 0, err
	}
	return model.CompareSemVer(av, bv), nil
}
