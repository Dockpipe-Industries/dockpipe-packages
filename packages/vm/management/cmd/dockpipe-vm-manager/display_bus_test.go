//go:build linux || darwin || windows

package main

import (
	"encoding/xml"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestPrivateDisplayBusConfigurationIsSelfContainedAndProtected(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "private-bus")
	if err := createPrivateDisplayDirectory(directory); err != nil {
		t.Fatal(err)
	}
	address := privateDisplayBusAddress(filepath.Join(directory, "bus"))
	path, err := writePrivateDisplayBusConfiguration(directory, address)
	if err != nil {
		t.Fatal(err)
	}
	if !privateOwnedPath(path, false, 0600) || filepath.Dir(path) != directory {
		t.Fatal("broker configuration escaped its private boundary")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var configuration struct {
		XMLName xml.Name `xml:"busconfig"`
		Type    string   `xml:"type"`
		Auth    []string `xml:"auth"`
		Listen  []string `xml:"listen"`
		Policy  struct {
			Context string `xml:"context,attr"`
			Entries []struct {
				Attributes []xml.Attr `xml:",any,attr"`
			} `xml:"allow"`
		} `xml:"policy"`
		Other []struct {
			XMLName xml.Name
		} `xml:",any"`
	}
	if err := xml.Unmarshal(data, &configuration); err != nil {
		t.Fatal(err)
	}
	if configuration.Type != "session" || !reflect.DeepEqual(configuration.Auth, []string{"EXTERNAL"}) ||
		!reflect.DeepEqual(configuration.Listen, []string{address}) || configuration.Policy.Context != "default" ||
		len(configuration.Other) != 0 || len(configuration.Policy.Entries) != 3 {
		t.Fatal("broker configuration added external authentication, listeners or activation inputs")
	}
	permissions := map[string]bool{}
	for _, entry := range configuration.Policy.Entries {
		if len(entry.Attributes) != 1 || entry.Attributes[0].Value != "*" {
			t.Fatal("unexpected broker permission")
		}
		permissions[entry.Attributes[0].Name.Local] = true
	}
	if !reflect.DeepEqual(permissions, map[string]bool{"send_destination": true, "receive_sender": true, "own": true}) {
		t.Fatal("private broker policy changed")
	}
	if _, err := writePrivateDisplayBusConfiguration(directory, "unix:path=/unrelated/bus"); err == nil {
		t.Fatal("configuration admitted an unrelated resource address")
	}
}
