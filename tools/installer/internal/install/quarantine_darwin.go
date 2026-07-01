//go:build darwin

package install

import "os/exec"

// stripQuarantine removes com.apple.quarantine recursively so an unsigned,
// freshly-downloaded bundle loads without Gatekeeper blocking it. Best-effort:
// if the attribute isn't present, xattr returns non-zero, which we ignore.
func stripQuarantine(bundle string) error {
	return exec.Command("/usr/bin/xattr", "-dr", "com.apple.quarantine", bundle).Run()
}

// refreshAudioUnits nudges the AU registrar so a newly-installed .component is
// picked up on the next DAW scan.
func refreshAudioUnits() error {
	return exec.Command("/usr/bin/killall", "-9", "AudioComponentRegistrar").Run()
}
