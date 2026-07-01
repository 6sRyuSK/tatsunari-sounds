//go:build !darwin

package install

// stripQuarantine / refreshAudioUnits are macOS-only concepts; on Windows (and
// Linux CI) they are no-ops so the shared apply engine stays platform-neutral.

func stripQuarantine(string) error { return nil }

func refreshAudioUnits() error { return nil }
