package updates

import (
	"bytes"
	"encoding/json"
	"fmt"
)

// rejectDuplicateKeys walks JSON and fails if any object repeats a key.
// encoding/json silently keeps the last value, which violates the wire spec.
func rejectDuplicateKeys(data []byte) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	return walkJSONRejectDup(dec, nil)
}

func walkJSONRejectDup(dec *json.Decoder, path []string) error {
	tok, err := dec.Token()
	if err != nil {
		return err
	}
	switch t := tok.(type) {
	case json.Delim:
		switch t {
		case '{':
			seen := map[string]bool{}
			for dec.More() {
				keyTok, err := dec.Token()
				if err != nil {
					return err
				}
				key, ok := keyTok.(string)
				if !ok {
					return fmt.Errorf("expected object key at %v", path)
				}
				if seen[key] {
					loc := append(append([]string{}, path...), key)
					return fmt.Errorf("duplicate JSON key %q at %v", key, loc)
				}
				seen[key] = true
				if err := walkJSONRejectDup(dec, append(path, key)); err != nil {
					return err
				}
			}
			end, err := dec.Token()
			if err != nil {
				return err
			}
			if end != json.Delim('}') {
				return fmt.Errorf("expected } at %v", path)
			}
		case '[':
			i := 0
			for dec.More() {
				if err := walkJSONRejectDup(dec, append(path, fmt.Sprintf("[%d]", i))); err != nil {
					return err
				}
				i++
			}
			end, err := dec.Token()
			if err != nil {
				return err
			}
			if end != json.Delim(']') {
				return fmt.Errorf("expected ] at %v", path)
			}
		default:
			return fmt.Errorf("unexpected delimiter %v at %v", t, path)
		}
	case string, bool, float64, json.Number, nil:
		return nil
	default:
		return fmt.Errorf("unexpected token %T at %v", tok, path)
	}
	return nil
}
