package i18n

import "testing"

func TestDetect(t *testing.T) {
	cases := []struct {
		env  map[string]string
		want Lang
	}{
		{map[string]string{"TATSUNARI_LANG": "ja"}, JA},
		{map[string]string{"TATSUNARI_LANG": "en"}, EN},
		{map[string]string{"LANG": "ja_JP.UTF-8"}, JA},
		{map[string]string{"LANG": "en_US.UTF-8"}, EN},
		{map[string]string{"LC_ALL": "ja_JP.UTF-8", "LANG": "en_US.UTF-8"}, JA}, // LC_ALL wins
		{map[string]string{}, EN},                                               // nothing set
	}
	for i, c := range cases {
		for _, k := range []string{"TATSUNARI_LANG", "LC_ALL", "LC_MESSAGES", "LANG"} {
			t.Setenv(k, "")
		}
		for k, v := range c.env {
			t.Setenv(k, v)
		}
		if got := Detect(); got != c.want {
			t.Errorf("case %d: Detect() = %v, want %v", i, got, c.want)
		}
	}
}

func TestT(t *testing.T) {
	if got := (Translator{Lang: JA}).T("日本語", "English"); got != "日本語" {
		t.Errorf("JA translate = %q", got)
	}
	if got := (Translator{Lang: EN}).T("日本語", "English"); got != "English" {
		t.Errorf("EN translate = %q", got)
	}
}
