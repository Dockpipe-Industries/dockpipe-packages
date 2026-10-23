package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"strings"
	"testing"
	"time"
)

func TestClipboardRequestBoundsAndPrivacy(t *testing.T) {
	for _, text := range []string{"", "Unicode 日本語\n✓", strings.Repeat("x", clipboardTextLimit)} {
		encoded := base64.StdEncoding.EncodeToString([]byte(text))
		data, _ := json.Marshal(map[string]any{"schema": 1, "operation": "write", "text_base64": encoded, "expected_sha256": strings.Repeat("0", 64)})
		if _, err := decodeClipboardRequest(data); err != nil {
			t.Fatal("valid bounded UTF8 request rejected")
		}
	}
	invalidText := base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{'x'}, clipboardTextLimit+1))
	cases := []string{
		`{"schema":1,"schema":1,"operation":"read"}`,
		`{"schema":1,"operation":"read","text_base64":""}`,
		`{"schema":1,"operation":"write","text_base64":"/w=="}`,
		`{"schema":1,"operation":"write","text_base64":"AA=="}`,
		`{"schema":1,"operation":"write","text_base64":"eA==\n"}`,
		`{"schema":1,"operation":"write","text_base64":null}`,
		`{"schema":1,"operation":"write","text_base64":"","expected_sha256":null}`,
		`{"schema":1,"operation":"write","text_base64":"","expected_sha256":"invalid"}`,
		`{"schema":1,"operation":"write","text_base64":"` + invalidText + `"}`,
		`{"schema":1,"operation":"show"}`,
	}
	for _, data := range cases {
		_, err := decodeClipboardRequest([]byte(data))
		if err != errClipboard {
			t.Fatal("invalid clipboard request did not produce the fixed safe error")
		}
	}
}

func TestClipboardResponsesRejectUnboundedOrAmbiguousData(t *testing.T) {
	for _, data := range []string{
		`{"schema":1,"success":true,"text_base64":"aGVsbG8="}`,
		`{"schema":1,"success":false,"error":"unsupported-text"}`,
	} {
		if _, err := decodeClipboardResponse([]byte(data), "read"); err != nil {
			t.Fatal(err)
		}
	}
	for _, data := range []string{
		`{"schema":1,"success":true,"success":false,"text_base64":""}`,
		`{"schema":1,"success":true,"text_base64":"/w=="}`,
		`{"schema":1,"success":true,"text_base64":null}`,
		`{"schema":1,"success":false,"error":"private-payload-must-not-appear"}`,
	} {
		if _, err := decodeClipboardResponse([]byte(data), "read"); err != errClipboard {
			t.Fatal("untrusted response escaped fixed private boundary")
		}
	}
	if _, err := decodeClipboardResponse([]byte(strings.Repeat(" ", clipboardWireLimit+1)), "read"); err != errClipboard {
		t.Fatal("oversized response accepted")
	}
}

func TestClipboardInputHonorsCancellationBeforeAnyIdentityOrSSH(t *testing.T) {
	reader, writer := io.Pipe()
	defer reader.Close()
	defer writer.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	began := time.Now()
	if _, err := (manager{}).clipboardExchange(ctx, reader, "fixture"); err != errClipboard {
		t.Fatal("unfinished stdin did not fail safely")
	}
	if time.Since(began) > time.Second {
		t.Fatal("clipboard stdin ignored operation deadline")
	}
}
