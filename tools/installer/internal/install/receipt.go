package install

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"time"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// receiptSchema versions the on-disk format so future changes can migrate.
const receiptSchema = 1

// Receipt records what the installer has placed on this machine, enabling
// update detection and (later) uninstall. It is always written by the
// unprivileged parent process into the per-user config dir, never by the
// elevated helper — so it stays user-owned.
type Receipt struct {
	Schema    int                    `json:"schema"`
	UpdatedAt string                 `json:"updatedAt"`
	Plugins   map[string]ReceiptItem `json:"plugins"` // keyed by slug
}

// ReceiptItem is one installed plugin's state.
type ReceiptItem struct {
	Version string   `json:"version"`
	Formats []string `json:"formats"`
	Scope   string   `json:"scope"`
	Paths   []string `json:"paths"` // installed bundle dirs
}

// ConfigDir returns the per-user config directory for the installer,
// creating nothing. Windows: %AppData%\tatsunari-plugins; macOS:
// ~/Library/Application Support/tatsunari-plugins.
func ConfigDir() (string, error) {
	if dir := os.Getenv("APPDATA"); dir != "" { // Windows
		return filepath.Join(dir, "tatsunari-plugins"), nil
	}
	home, err := homeDir()
	if err != nil {
		return "", err
	}
	// macOS (and a sane default elsewhere).
	return filepath.Join(home, "Library", "Application Support", "tatsunari-plugins"), nil
}

// ReceiptPath is the receipt file location.
func ReceiptPath() (string, error) {
	dir, err := ConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "receipt.json"), nil
}

// LoadReceipt reads the receipt, returning an empty (non-nil) receipt when the
// file does not exist yet.
func LoadReceipt() (*Receipt, error) {
	path, err := ReceiptPath()
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return &Receipt{Schema: receiptSchema, Plugins: map[string]ReceiptItem{}}, nil
	}
	if err != nil {
		return nil, err
	}
	var r Receipt
	if err := json.Unmarshal(data, &r); err != nil {
		return nil, fmt.Errorf("parse receipt %s: %w", path, err)
	}
	if r.Plugins == nil {
		r.Plugins = map[string]ReceiptItem{}
	}
	return &r, nil
}

// InstalledVersions extracts slug -> version for discovery reconciliation.
func (r *Receipt) InstalledVersions() map[string]string {
	out := make(map[string]string, len(r.Plugins))
	for slug, item := range r.Plugins {
		out[slug] = item.Version
	}
	return out
}

// Record merges one plugin's install result into the receipt (in memory).
// Formats/paths for the slug are unioned so installing VST3 then AU keeps both.
func (r *Receipt) Record(slug, version string, scope model.Scope, formats []model.Format, paths []string) {
	if r.Plugins == nil {
		r.Plugins = map[string]ReceiptItem{}
	}
	item := r.Plugins[slug]
	item.Version = version
	item.Scope = string(scope)
	item.Formats = unionStrings(item.Formats, formatStrings(formats))
	item.Paths = unionStrings(item.Paths, paths)
	r.Plugins[slug] = item
}

// Save writes the receipt atomically (temp file + rename), creating the config
// dir if needed. 0700 dir / 0600 file keep it private to the user.
func (r *Receipt) Save() error {
	r.Schema = receiptSchema
	r.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	dir, err := ConfigDir()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	data, err := json.MarshalIndent(r, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(dir, "receipt.json")
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
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
