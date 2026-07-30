package updates

import (
	"encoding/json"
	"fmt"
	"strings"
	"time"
)

// ParseLatest decodes latest.json bytes. Envelope errors are fatal; unknown
// fields are ignored. Per-plugin type errors become Issues and that plugin is
// dropped.
func ParseLatest(data []byte) (ParseResult[LatestDocument], error) {
	var out ParseResult[LatestDocument]
	if len(data) > MaxLatestBytes {
		return out, fmt.Errorf("latest.json exceeds %d bytes", MaxLatestBytes)
	}
	if !json.Valid(data) {
		return out, fmt.Errorf("latest.json is not valid UTF-8 JSON")
	}
	if err := rejectDuplicateKeys(data); err != nil {
		return out, fmt.Errorf("latest.json: %w", err)
	}
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return out, fmt.Errorf("latest.json envelope: %w", err)
	}
	schema, err := requireInt(raw, "schema")
	if err != nil {
		return out, err
	}
	if schema != SchemaVersion {
		return out, fmt.Errorf("unsupported schema %d (want %d)", schema, SchemaVersion)
	}
	generated, err := requireString(raw, "generated")
	if err != nil {
		return out, err
	}
	if _, err := time.Parse(time.RFC3339, generated); err != nil {
		return out, fmt.Errorf("generated must be RFC3339: %w", err)
	}
	pluginsRaw, err := requireArray(raw, "plugins")
	if err != nil {
		return out, err
	}
	out.Doc.Schema = schema
	out.Doc.Generated = generated
	for i, pr := range pluginsRaw {
		var p LatestPlugin
		if err := json.Unmarshal(pr, &p); err != nil {
			out.Issues = append(out.Issues, ParseIssue{
				Kind: "plugin", Detail: fmt.Sprintf("plugins[%d]: %v", i, err),
			})
			continue
		}
		if p.Slug == "" || p.Latest == "" {
			out.Issues = append(out.Issues, ParseIssue{
				Kind: "plugin", Slug: p.Slug,
				Detail: "slug and latest are required",
			})
			continue
		}
		if err := validateStableSemVer(p.Latest); err != nil {
			out.Issues = append(out.Issues, ParseIssue{
				Kind: "plugin", Slug: p.Slug, Detail: err.Error(),
			})
			continue
		}
		if p.ChangelogURL != "" {
			if err := CheckHTTPSURL(p.ChangelogURL); err != nil {
				out.Issues = append(out.Issues, ParseIssue{
					Kind: "plugin", Slug: p.Slug, Detail: "changelogUrl: " + err.Error(),
				})
				continue
			}
		}
		out.Doc.Plugins = append(out.Doc.Plugins, p)
	}
	return out, nil
}

// ParseCatalog decodes catalog.json. Envelope errors are fatal. Unknown
// formats/plugins degrade to Issues; forbidden hook keys are rejected at the
// asset/plugin row. reference is never a wire field (plan §3.5).
func ParseCatalog(data []byte) (ParseResult[CatalogDocument], error) {
	var out ParseResult[CatalogDocument]
	if len(data) > MaxCatalogBytes {
		return out, fmt.Errorf("catalog.json exceeds %d bytes", MaxCatalogBytes)
	}
	if !json.Valid(data) {
		return out, fmt.Errorf("catalog.json is not valid UTF-8 JSON")
	}
	if err := rejectDuplicateKeys(data); err != nil {
		return out, fmt.Errorf("catalog.json: %w", err)
	}
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return out, fmt.Errorf("catalog.json envelope: %w", err)
	}
	for _, banned := range []string{
		"postInstallScript", "preInstallCommand", "telemetryUrl", "remoteConfig",
	} {
		if _, ok := raw[banned]; ok {
			return out, fmt.Errorf("forbidden envelope field %q", banned)
		}
	}
	schema, err := requireInt(raw, "schema")
	if err != nil {
		return out, err
	}
	if schema != SchemaVersion {
		return out, fmt.Errorf("unsupported schema %d (want %d)", schema, SchemaVersion)
	}
	generated, err := requireString(raw, "generated")
	if err != nil {
		return out, err
	}
	if _, err := time.Parse(time.RFC3339, generated); err != nil {
		return out, fmt.Errorf("generated must be RFC3339: %w", err)
	}
	out.Doc.Schema = schema
	out.Doc.Generated = generated
	if g, ok := raw["generator"]; ok {
		_ = json.Unmarshal(g, &out.Doc.Generator)
	}
	if m, ok := raw["minClientVersion"]; ok {
		_ = json.Unmarshal(m, &out.Doc.MinClientVersion)
	}
	if t, ok := raw["ttlHint"]; ok {
		_ = json.Unmarshal(t, &out.Doc.TTLHint)
	}

	channelsRaw, err := requireArray(raw, "channels")
	if err != nil {
		return out, err
	}
	for i, cr := range channelsRaw {
		var ch Channel
		if err := json.Unmarshal(cr, &ch); err != nil || ch.ID == "" || ch.Name["en"] == "" {
			out.Issues = append(out.Issues, ParseIssue{
				Kind: "envelope", Detail: fmt.Sprintf("channels[%d] invalid", i),
			})
			continue
		}
		out.Doc.Channels = append(out.Doc.Channels, ch)
	}
	if len(out.Doc.Channels) == 0 {
		return out, fmt.Errorf("channels must contain at least one entry")
	}

	if c, ok := raw["client"]; ok {
		client, issues := parseClient(c)
		out.Issues = append(out.Issues, issues...)
		out.Doc.Client = client
	}
	if n, ok := raw["notice"]; ok {
		var notice Notice
		if err := json.Unmarshal(n, &notice); err == nil && notice.Message["en"] != "" {
			out.Doc.Notice = &notice
		}
	}

	pluginsRaw, err := requireArray(raw, "plugins")
	if err != nil {
		return out, err
	}
	seen := map[string]bool{}
	for i, pr := range pluginsRaw {
		p, issues, ok := parsePlugin(pr, i)
		out.Issues = append(out.Issues, issues...)
		if !ok {
			continue
		}
		key := p.Slug + "\x00" + p.Variant
		if seen[key] {
			out.Issues = append(out.Issues, ParseIssue{
				Kind: "plugin", Slug: p.Slug, Variant: p.Variant,
				Detail: "duplicate (slug, variant)",
			})
			continue
		}
		seen[key] = true
		out.Doc.Plugins = append(out.Doc.Plugins, p)
	}
	return out, nil
}

func parseClient(raw json.RawMessage) (*ClientInfo, []ParseIssue) {
	var issues []ParseIssue
	var c ClientInfo
	if err := json.Unmarshal(raw, &c); err != nil {
		return nil, []ParseIssue{{Kind: "client", Detail: err.Error()}}
	}
	if c.Latest == "" {
		return nil, []ParseIssue{{Kind: "client", Detail: "latest is required"}}
	}
	var assets []ClientAsset
	for i, a := range c.Assets {
		if err := validateClientAsset(a); err != nil {
			issues = append(issues, ParseIssue{
				Kind: "client", Detail: fmt.Sprintf("assets[%d]: %v", i, err),
			})
			continue
		}
		assets = append(assets, a)
	}
	c.Assets = assets
	if len(c.Assets) == 0 {
		issues = append(issues, ParseIssue{Kind: "client", Detail: "no usable assets"})
		return nil, issues
	}
	return &c, issues
}

func parsePlugin(raw json.RawMessage, index int) (Plugin, []ParseIssue, bool) {
	var issues []ParseIssue
	var probe map[string]json.RawMessage
	if err := json.Unmarshal(raw, &probe); err != nil {
		return Plugin{}, []ParseIssue{{
			Kind: "plugin", Detail: fmt.Sprintf("plugins[%d]: %v", index, err),
		}}, false
	}
	for _, banned := range []string{
		"postInstallScript", "preInstallCommand", "absolutePath", "installRoot", "reference",
	} {
		if _, ok := probe[banned]; ok {
			return Plugin{}, []ParseIssue{{
				Kind: "plugin", Detail: fmt.Sprintf("plugins[%d]: forbidden field %q", index, banned),
			}}, false
		}
	}
	var p Plugin
	if err := json.Unmarshal(raw, &p); err != nil {
		return Plugin{}, []ParseIssue{{
			Kind: "plugin", Detail: fmt.Sprintf("plugins[%d]: %v", index, err),
		}}, false
	}
	if p.Slug == "" || p.Variant == "" || p.Category == "" || p.Vendor == "" || p.Latest == "" {
		return Plugin{}, []ParseIssue{{
			Kind: "plugin", Slug: p.Slug, Variant: p.Variant,
			Detail: "slug, variant, category, vendor, latest are required",
		}}, false
	}
	if p.Variant != "stable" && p.Variant != "dev" {
		return Plugin{}, []ParseIssue{{
			Kind: "plugin", Slug: p.Slug, Variant: p.Variant,
			Detail: "variant must be stable or dev",
		}}, false
	}
	if p.Name["en"] == "" {
		return Plugin{}, []ParseIssue{{
			Kind: "plugin", Slug: p.Slug, Variant: p.Variant,
			Detail: "name.en is required",
		}}, false
	}
	if p.PluginIds.ClapID == "" {
		return Plugin{}, []ParseIssue{{
			Kind: "plugin", Slug: p.Slug, Variant: p.Variant,
			Detail: "pluginIds.clapId is required",
		}}, false
	}
	var versions []Version
	seenVer := map[string]bool{}
	for i, v := range p.Versions {
		clean, vIssues, ok := sanitizeVersion(p.Slug, p.Variant, v, i)
		issues = append(issues, vIssues...)
		if !ok {
			continue
		}
		if seenVer[clean.Version] {
			issues = append(issues, ParseIssue{
				Kind: "version", Slug: p.Slug, Variant: p.Variant, Version: clean.Version,
				Detail: "duplicate version",
			})
			continue
		}
		seenVer[clean.Version] = true
		versions = append(versions, clean)
	}
	p.Versions = versions
	if len(p.Versions) == 0 {
		issues = append(issues, ParseIssue{
			Kind: "plugin", Slug: p.Slug, Variant: p.Variant,
			Detail: "no usable versions",
		})
		return Plugin{}, issues, false
	}
	return p, issues, true
}

func sanitizeVersion(slug, variant string, v Version, index int) (Version, []ParseIssue, bool) {
	var issues []ParseIssue
	if v.Version == "" || v.Channel == "" || v.ReleasedAt == "" || v.StateCompatVersion == "" {
		return Version{}, []ParseIssue{{
			Kind: "version", Slug: slug, Variant: variant,
			Detail: fmt.Sprintf("versions[%d]: version/channel/releasedAt/stateCompatVersion required", index),
		}}, false
	}
	if _, err := time.Parse(time.RFC3339, v.ReleasedAt); err != nil {
		return Version{}, []ParseIssue{{
			Kind: "version", Slug: slug, Variant: variant, Version: v.Version,
			Detail: "releasedAt must be RFC3339",
		}}, false
	}
	var assets []Asset
	seen := map[string]bool{}
	for i, a := range v.Assets {
		clean, err := sanitizeAsset(a)
		if err != nil {
			issues = append(issues, ParseIssue{
				Kind: "asset", Slug: slug, Variant: variant, Version: v.Version,
				Detail: fmt.Sprintf("assets[%d]: %v", i, err),
			})
			continue
		}
		key := clean.Format + "\x00" + clean.OS + "\x00" + clean.Arch
		if seen[key] {
			issues = append(issues, ParseIssue{
				Kind: "asset", Slug: slug, Variant: variant, Version: v.Version,
				Detail: "duplicate (format, os, arch)",
			})
			continue
		}
		seen[key] = true
		assets = append(assets, clean)
	}
	v.Assets = assets
	if len(v.Assets) == 0 && !v.Yanked {
		issues = append(issues, ParseIssue{
			Kind: "version", Slug: slug, Variant: variant, Version: v.Version,
			Detail: "no usable assets",
		})
		return Version{}, issues, false
	}
	return v, issues, true
}

func sanitizeAsset(a Asset) (Asset, error) {
	format := strings.ToLower(strings.TrimSpace(a.Format))
	switch format {
	case "vst3", "au", "clap":
		a.Format = format
	default:
		return Asset{}, fmt.Errorf("unknown format %q (skipped)", a.Format)
	}
	osID := strings.ToLower(strings.TrimSpace(a.OS))
	switch osID {
	case "macos", "windows":
		a.OS = osID
	default:
		return Asset{}, fmt.Errorf("unknown os %q", a.OS)
	}
	arch := strings.TrimSpace(a.Arch)
	switch arch {
	case "universal", "x86_64", "arm64":
		a.Arch = arch
	default:
		return Asset{}, fmt.Errorf("unknown arch %q", a.Arch)
	}
	if a.Size < 0 {
		return Asset{}, fmt.Errorf("size must be non-negative")
	}
	if err := validateSHA256(a.SHA256); err != nil {
		return Asset{}, err
	}
	if err := CheckHTTPSURL(a.URL); err != nil {
		return Asset{}, err
	}
	if err := ValidateSubpath(a.Subpath); err != nil {
		return Asset{}, err
	}
	if err := ValidateBundleName(a.BundleName); err != nil {
		return Asset{}, err
	}
	for _, banned := range []string{a.Subpath, a.BundleName} {
		if strings.Contains(banned, "\x00") {
			return Asset{}, fmt.Errorf("nul byte in path component")
		}
	}
	return a, nil
}

func validateClientAsset(a ClientAsset) error {
	osID := strings.ToLower(strings.TrimSpace(a.OS))
	switch osID {
	case "macos", "windows":
	default:
		return fmt.Errorf("unknown os %q", a.OS)
	}
	switch a.Arch {
	case "arm64", "amd64":
	default:
		return fmt.Errorf("unknown arch %q", a.Arch)
	}
	if a.Size < 0 {
		return fmt.Errorf("size must be non-negative")
	}
	if err := validateSHA256(a.SHA256); err != nil {
		return err
	}
	return CheckHTTPSURL(a.URL)
}

func validateSHA256(s string) error {
	if len(s) != SHA256HexLen {
		return fmt.Errorf("sha256 must be %d lowercase hex chars", SHA256HexLen)
	}
	for _, c := range s {
		if (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			return fmt.Errorf("sha256 must be lowercase hex")
		}
	}
	return nil
}

func validateStableSemVer(s string) error {
	s = strings.TrimSpace(s)
	if strings.ContainsAny(s, "-+") {
		return fmt.Errorf("stable latest must not include prerelease/build metadata: %q", s)
	}
	parts := strings.Split(s, ".")
	if len(parts) != 3 {
		return fmt.Errorf("semver must be MAJOR.MINOR.PATCH: %q", s)
	}
	for _, p := range parts {
		if p == "" {
			return fmt.Errorf("invalid semver %q", s)
		}
		for _, c := range p {
			if c < '0' || c > '9' {
				return fmt.Errorf("invalid semver %q", s)
			}
		}
	}
	return nil
}

func requireInt(raw map[string]json.RawMessage, key string) (int, error) {
	v, ok := raw[key]
	if !ok {
		return 0, fmt.Errorf("missing required field %q", key)
	}
	var n int
	if err := json.Unmarshal(v, &n); err != nil {
		return 0, fmt.Errorf("field %q: %w", key, err)
	}
	return n, nil
}

func requireString(raw map[string]json.RawMessage, key string) (string, error) {
	v, ok := raw[key]
	if !ok {
		return "", fmt.Errorf("missing required field %q", key)
	}
	var s string
	if err := json.Unmarshal(v, &s); err != nil || s == "" {
		return "", fmt.Errorf("field %q must be a non-empty string", key)
	}
	return s, nil
}

func requireArray(raw map[string]json.RawMessage, key string) ([]json.RawMessage, error) {
	v, ok := raw[key]
	if !ok {
		return nil, fmt.Errorf("missing required field %q", key)
	}
	var arr []json.RawMessage
	if err := json.Unmarshal(v, &arr); err != nil {
		return nil, fmt.Errorf("field %q must be an array: %w", key, err)
	}
	return arr, nil
}
