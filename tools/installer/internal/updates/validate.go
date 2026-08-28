package updates

import (
	"fmt"
	"path"
	"strings"
	"unicode"
)

// ValidateSubpath rejects absolute paths, parent traversal, empty segments,
// Windows drive/UNC forms, and reserved device names. The binary enum root is
// the only install root; subpath may only refine within it (invariant 5).
func ValidateSubpath(subpath string) error {
	if subpath == "" {
		return fmt.Errorf("subpath is empty")
	}
	if strings.Contains(subpath, "\x00") {
		return fmt.Errorf("subpath contains nul")
	}
	// Reject absolute / drive / UNC before cleaning.
	if strings.HasPrefix(subpath, "/") || strings.HasPrefix(subpath, "\\") {
		return fmt.Errorf("subpath must be relative, got %q", subpath)
	}
	if strings.HasPrefix(subpath, "//") || strings.HasPrefix(subpath, `\\`) {
		return fmt.Errorf("subpath must not be UNC, got %q", subpath)
	}
	if len(subpath) >= 2 && unicode.IsLetter(rune(subpath[0])) && subpath[1] == ':' {
		return fmt.Errorf("subpath must not include a drive letter, got %q", subpath)
	}
	normalized := strings.ReplaceAll(subpath, "\\", "/")
	if path.IsAbs(normalized) {
		return fmt.Errorf("subpath must be relative, got %q", subpath)
	}
	cleaned := path.Clean(normalized)
	if cleaned == ".." || strings.HasPrefix(cleaned, "../") {
		return fmt.Errorf("subpath must not escape root, got %q", subpath)
	}
	if cleaned == "." {
		return fmt.Errorf("subpath must not be empty after clean")
	}
	for _, seg := range strings.Split(cleaned, "/") {
		if seg == "" || seg == "." || seg == ".." {
			return fmt.Errorf("subpath has illegal segment in %q", subpath)
		}
		if isReservedWindowsDevice(seg) {
			return fmt.Errorf("subpath uses reserved device name %q", seg)
		}
		if strings.ContainsAny(seg, `<>:"|?*`) {
			return fmt.Errorf("subpath segment %q has illegal characters", seg)
		}
	}
	return nil
}

// ValidateBundleName checks the leaf bundle directory name (e.g. Foo.vst3).
func ValidateBundleName(name string) error {
	if name == "" {
		return fmt.Errorf("bundleName is empty")
	}
	if strings.ContainsAny(name, `/\`) || name == "." || name == ".." {
		return fmt.Errorf("bundleName must be a single path segment, got %q", name)
	}
	if strings.Contains(name, "\x00") {
		return fmt.Errorf("bundleName contains nul")
	}
	if isReservedWindowsDevice(name) {
		return fmt.Errorf("bundleName uses reserved device name %q", name)
	}
	return nil
}

func isReservedWindowsDevice(seg string) bool {
	base := seg
	if i := strings.IndexByte(seg, '.'); i >= 0 {
		base = seg[:i]
	}
	switch strings.ToUpper(base) {
	case "CON", "PRN", "AUX", "NUL",
		"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9":
		return true
	}
	return false
}
