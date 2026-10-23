package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

func TestGuestStatusRequiresExactSchemaTypedFieldsAndBoundedValues(t *testing.T) {
	valid := validStatusJSON()
	status, err := decodeGuestStatus(valid)
	if err != nil || !status.SSH || !status.Agent || !status.ApplicationInstalled || status.RepairRunning {
		t.Fatalf("valid guest status failed: %+v %v", status, err)
	}
	changes := map[string]func(map[string]any){
		"missing boolean":          func(value map[string]any) { delete(value, "repair_running") },
		"null boolean":             func(value map[string]any) { value["repair_running"] = nil },
		"string boolean":           func(value map[string]any) { value["ssh"] = "true" },
		"numeric boolean":          func(value map[string]any) { value["agent"] = 1 },
		"schema version":           func(value map[string]any) { value["schema"] = 2 },
		"missing schema":           func(value map[string]any) { delete(value, "schema") },
		"unknown credential field": func(value map[string]any) { value["password"] = "synthetic" },
		"unknown state":            func(value map[string]any) { value["tools_state"] = "anything" },
		"oversized message":        func(value map[string]any) { value["tools_message"] = strings.Repeat("x", 4097) },
		"null string":              func(value map[string]any) { value["machine_name"] = nil },
		"null repair result":       func(value map[string]any) { value["repair_result"] = nil },
		"negative repair result":   func(value map[string]any) { value["repair_result"] = -2 },
	}
	for name, change := range changes {
		t.Run(name, func(t *testing.T) {
			var value map[string]any
			if err := json.Unmarshal(valid, &value); err != nil {
				t.Fatal(err)
			}
			change(value)
			data, err := json.Marshal(value)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := decodeGuestStatus(data); err == nil {
				t.Fatal("invalid guest status accepted")
			}
		})
	}
	for _, data := range [][]byte{
		[]byte(`{}`), append(append([]byte{}, valid...), []byte(`{}`)...),
		bytes.Replace(valid, []byte(`"schema":1`), []byte(`"schema":1,"schema":1`), 1),
		bytes.Repeat([]byte(" "), 64*1024+1), []byte{0xff},
	} {
		if _, err := decodeGuestStatus(data); err == nil {
			t.Fatalf("invalid framing accepted: %q", data[:min(len(data), 80)])
		}
	}
}
