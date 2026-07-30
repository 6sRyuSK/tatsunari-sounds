package install

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// receiptSchema versions the on-disk format.
// v1: plugins keyed by slug only (legacy).
// v2: entries keyed by (slug, variant, scope) — plan §5.5.
const (
	receiptSchemaV1 = 1
	receiptSchemaV2 = 2
	receiptSchema   = receiptSchemaV2
)

const receiptFileName = "receipt.json"

// Receipt records what the installer has placed on this machine.
// Schema 2 entries use compound keys; Load* migrates v1 → v2 in memory.
type Receipt struct {
	Schema    int                     `json:"schema"`
	UpdatedAt string                  `json:"updatedAt"`
	Entries   map[string]ReceiptEntry `json:"entries"` // key: slug|variant|scope
	// Plugins is the legacy v1 map; retained only for migration on read.
	Plugins map[string]ReceiptItem `json:"plugins,omitempty"`
}

// ReceiptEntry is one installed (slug, variant, scope) identity.
type ReceiptEntry struct {
	Slug               string            `json:"slug"`
	Variant            string            `json:"variant"`
	Scope              string            `json:"scope"`
	Version            string            `json:"version"`
	StateCompatVersion string            `json:"stateCompatVersion,omitempty"`
	Formats            []string          `json:"formats"`
	Paths              []string          `json:"paths"`
	FormatPaths        map[string]string `json:"formatPaths,omitempty"`
	InstallerVersion   string            `json:"installerVersion,omitempty"`
	InstalledAt        string            `json:"installedAt,omitempty"`
}

// ReceiptItem is the legacy v1 shape (slug-keyed).
type ReceiptItem struct {
	Version string   `json:"version"`
	Formats []string `json:"formats"`
	Scope   string   `json:"scope"`
	Paths   []string `json:"paths"`
}

// EntryKey builds the stable compound key for a receipt row.
func EntryKey(slug string, variant model.Variant, scope model.Scope) string {
	if variant == "" {
		variant = model.VariantStable
	}
	return slug + "|" + string(variant) + "|" + string(scope)
}

// ConfigDir returns the per-user config directory for the installer,
// creating nothing. Windows: %AppData%\tatsunari-sounds; macOS:
// ~/Library/Application Support/tatsunari-sounds.
func ConfigDir() (string, error) {
	if dir := os.Getenv("APPDATA"); dir != "" { // Windows
		return filepath.Join(dir, "tatsunari-sounds"), nil
	}
	home, err := homeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, "Library", "Application Support", "tatsunari-sounds"), nil
}

// ReceiptPath is the per-user receipt file location.
func ReceiptPath() (string, error) {
	dir, err := ConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, receiptFileName), nil
}

// ReceiptPathFor returns the receipt path for a scope on the given OS.
func ReceiptPathFor(osID model.OS, scope model.Scope) (string, error) {
	dir, err := DestinationRoot(osID, scope, model.RootReceipt)
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, receiptFileName), nil
}

// LoadReceipt reads the per-user receipt (compat wrapper).
func LoadReceipt() (*Receipt, error) {
	path, err := ReceiptPath()
	if err != nil {
		return nil, err
	}
	return loadReceiptFile(path)
}

// LoadAllReceipts reads user + system receipts and merges by compound key
// (not by slug). Missing files yield empty contributions.
func LoadAllReceipts(osID model.OS) (*Receipt, error) {
	merged := &Receipt{Schema: receiptSchema, Entries: map[string]ReceiptEntry{}}
	for _, scope := range []model.Scope{model.ScopeUser, model.ScopeSystem} {
		path, err := ReceiptPathFor(osID, scope)
		if err != nil {
			continue
		}
		r, err := loadReceiptFile(path)
		if err != nil {
			return nil, err
		}
		for k, e := range r.Entries {
			merged.Entries[k] = e
		}
	}
	return merged, nil
}

func loadReceiptFile(path string) (*Receipt, error) {
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return &Receipt{Schema: receiptSchema, Entries: map[string]ReceiptEntry{}}, nil
	}
	if err != nil {
		return nil, err
	}
	var r Receipt
	if err := json.Unmarshal(data, &r); err != nil {
		return nil, fmt.Errorf("parse receipt %s: %w", path, err)
	}
	migrateReceipt(&r)
	return &r, nil
}

func migrateReceipt(r *Receipt) {
	if r.Entries == nil {
		r.Entries = map[string]ReceiptEntry{}
	}
	if r.Schema <= receiptSchemaV1 && len(r.Plugins) > 0 {
		for slug, item := range r.Plugins {
			scope := model.Scope(item.Scope)
			if scope == "" {
				scope = model.ScopeUser
			}
			key := EntryKey(slug, model.VariantStable, scope)
			r.Entries[key] = ReceiptEntry{
				Slug:    slug,
				Variant: string(model.VariantStable),
				Scope:   string(scope),
				Version: item.Version,
				Formats: append([]string{}, item.Formats...),
				Paths:   append([]string{}, item.Paths...),
			}
		}
	}
	r.Plugins = nil
	r.Schema = receiptSchema
}

// InstalledVersions extracts slug -> version for legacy discovery reconciliation.
// Prefer InstalledEntries for new code.
func (r *Receipt) InstalledVersions() map[string]string {
	out := make(map[string]string)
	for _, e := range r.Entries {
		// Prefer stable+any scope; last write wins for same slug.
		if e.Variant == string(model.VariantStable) || e.Variant == "" {
			out[e.Slug] = e.Version
		}
	}
	return out
}

// InstalledEntries returns a copy of all compound-key entries.
func (r *Receipt) InstalledEntries() map[string]ReceiptEntry {
	out := make(map[string]ReceiptEntry, len(r.Entries))
	for k, v := range r.Entries {
		out[k] = v
	}
	return out
}

// Record merges one plugin install into the receipt under (slug, variant, scope).
func (r *Receipt) Record(slug string, variant model.Variant, version string, scope model.Scope, formats []model.Format, paths []string) {
	if r.Entries == nil {
		r.Entries = map[string]ReceiptEntry{}
	}
	if variant == "" {
		variant = model.VariantStable
	}
	key := EntryKey(slug, variant, scope)
	item := r.Entries[key]
	item.Slug = slug
	item.Variant = string(variant)
	item.Scope = string(scope)
	item.Version = version
	item.Formats = unionStrings(item.Formats, formatStrings(formats))
	item.Paths = unionStrings(item.Paths, paths)
	item.InstalledAt = time.Now().UTC().Format(time.RFC3339)
	r.Entries[key] = item
}

// Save writes the per-user receipt atomically.
func (r *Receipt) Save() error {
	path, err := ReceiptPath()
	if err != nil {
		return err
	}
	return r.SaveTo(path, 0o700, 0o600)
}

// SaveForScope writes the receipt for the given scope. System receipts use
// world-readable files (0644) so other users can reconcile; dirs 0755.
func (r *Receipt) SaveForScope(osID model.OS, scope model.Scope) error {
	path, err := ReceiptPathFor(osID, scope)
	if err != nil {
		return err
	}
	dirMode, fileMode := os.FileMode(0o700), os.FileMode(0o600)
	if scope == model.ScopeSystem {
		dirMode, fileMode = 0o755, 0o644
	}
	return r.SaveTo(path, dirMode, fileMode)
}

// SaveTo writes this receipt to path with atomic rename.
func (r *Receipt) SaveTo(path string, dirMode, fileMode os.FileMode) error {
	r.Schema = receiptSchema
	r.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	r.Plugins = nil
	if r.Entries == nil {
		r.Entries = map[string]ReceiptEntry{}
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, dirMode); err != nil {
		return err
	}
	data, err := json.MarshalIndent(r, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, fileMode); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// FilterScope returns a receipt containing only entries for scope.
func (r *Receipt) FilterScope(scope model.Scope) *Receipt {
	out := &Receipt{Schema: receiptSchema, Entries: map[string]ReceiptEntry{}}
	for k, e := range r.Entries {
		if e.Scope == string(scope) {
			out.Entries[k] = e
		}
	}
	return out
}

// DualScopeWarningSlugs returns slugs present in both system and user scopes
// for the same variant (hosts see duplicate plugins).
func (r *Receipt) DualScopeWarningSlugs() []string {
	type key struct{ slug, variant string }
	scopes := map[key]map[string]bool{}
	for _, e := range r.Entries {
		k := key{e.Slug, e.Variant}
		if scopes[k] == nil {
			scopes[k] = map[string]bool{}
		}
		scopes[k][e.Scope] = true
	}
	var out []string
	for k, sc := range scopes {
		if sc[string(model.ScopeSystem)] && sc[string(model.ScopeUser)] {
			out = append(out, k.slug+"|"+k.variant)
		}
	}
	sort.Strings(out)
	return out
}

func formatStrings(fs []model.Format) []string {
	out := make([]string, len(fs))
	for i, f := range fs {
		out[i] = string(f)
	}
	return out
}

func unionStrings(a, b []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, s := range append(append([]string{}, a...), b...) {
		if s == "" || seen[s] {
			continue
		}
		seen[s] = true
		out = append(out, s)
	}
	sort.Strings(out)
	return out
}

// ParseEntryKey splits a compound receipt key.
func ParseEntryKey(key string) (slug string, variant model.Variant, scope model.Scope, err error) {
	parts := strings.Split(key, "|")
	if len(parts) != 3 {
		return "", "", "", fmt.Errorf("invalid entry key %q", key)
	}
	return parts[0], model.Variant(parts[1]), model.Scope(parts[2]), nil
}
