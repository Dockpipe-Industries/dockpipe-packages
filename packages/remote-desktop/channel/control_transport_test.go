package main

import (
	"context"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func socketPairFixture(t *testing.T, kind int) (int, *os.File) {
	t.Helper()
	descriptors, err := syscall.Socketpair(syscall.AF_UNIX, kind, 0)
	if err != nil {
		t.Fatal(err)
	}
	syscall.CloseOnExec(descriptors[0])
	syscall.CloseOnExec(descriptors[1])
	t.Cleanup(func() { syscall.Close(descriptors[0]) })
	peer := os.NewFile(uintptr(descriptors[1]), "control-parent")
	t.Cleanup(func() { peer.Close() })
	return descriptors[0], peer
}

func TestControlAnonymousSocketpairFramingRenewalAndEOF(t *testing.T) {
	descriptor, parent := socketPairFixture(t, syscall.SOCK_STREAM)
	control, err := adoptControl(descriptor, "control")
	if err != nil {
		t.Fatal(err)
	}
	defer control.Close()
	if _, err := adoptPipe(descriptor, "media"); err == nil {
		t.Fatal("socket admitted to strict pipe path")
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	now := time.Now()
	guard := watchAuthority(ctx, cancel, control, now.Add(100*time.Millisecond), now.Add(time.Minute))
	update, _ := json.Marshal(map[string]any{"type": "lease", "expires_at": now.Add(time.Second)})
	if err := writeFrame(parent, update, 4096); err != nil {
		t.Fatal(err)
	}
	select {
	case <-ctx.Done():
		t.Fatal("renewal over socket failed")
	case <-time.After(150 * time.Millisecond):
	}
	parent.Close()
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("socket EOF did not cancel authority")
	}
	control.Close()
	guard.Wait()
}

func TestControlSocketBlockedReadCancellation(t *testing.T) {
	descriptor, _ := socketPairFixture(t, syscall.SOCK_STREAM)
	control, err := adoptControl(descriptor, "control")
	if err != nil {
		t.Fatal(err)
	}
	result := make(chan error, 1)
	go func() { _, err := readFrame(control, 4096); result <- err }()
	control.Close()
	if waitResult(t, result) == nil {
		t.Fatal("blocked socket read accepted closure")
	}
}

func TestControlRejectsWrongDescriptorTypes(t *testing.T) {
	regular, err := os.CreateTemp(t.TempDir(), "regular")
	if err != nil {
		t.Fatal(err)
	}
	defer regular.Close()
	if file, err := adoptControl(int(regular.Fd()), "regular"); err == nil {
		file.Close()
		t.Fatal("regular file admitted")
	}
	datagram, _ := socketPairFixture(t, syscall.SOCK_DGRAM)
	if file, err := adoptControl(datagram, "datagram"); err == nil {
		file.Close()
		t.Fatal("datagram admitted")
	}
	for _, domain := range []int{syscall.AF_UNIX, syscall.AF_INET} {
		descriptor, err := syscall.Socket(domain, syscall.SOCK_STREAM, 0)
		if err != nil {
			t.Fatal(err)
		}
		file, err := adoptControl(descriptor, "unconnected")
		syscall.Close(descriptor)
		if err == nil {
			file.Close()
			t.Fatal("unconnected/TCP socket admitted")
		}
	}
	directory, err := os.MkdirTemp("/tmp", "dp-control-named-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(directory)
	path := filepath.Join(directory, "named.sock")
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	connected := make(chan *net.UnixConn, 1)
	go func() { value, _ := listener.AcceptUnix(); connected <- value }()
	client, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	server := <-connected
	if server == nil {
		t.Fatal("named Unix fixture failed")
	}
	defer server.Close()
	for _, connection := range []*net.UnixConn{client, server} {
		raw, err := connection.File()
		if err != nil {
			t.Fatal(err)
		}
		adopted, err := adoptControl(int(raw.Fd()), "named")
		raw.Close()
		if err != nil {
			t.Fatal("connected Unix stream rejected", err)
		}
		adopted.Close()
	}
}

func TestControlSocketRejectsPartialAndOversizeLeaseFrames(t *testing.T) {
	for _, payload := range [][]byte{{0, 0, 0, 2, 1}, {0, 0, 16, 1}} {
		descriptor, parent := socketPairFixture(t, syscall.SOCK_STREAM)
		control, err := adoptControl(descriptor, "control")
		if err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithCancel(context.Background())
		now := time.Now()
		guard := watchAuthority(ctx, cancel, control, now.Add(time.Second), now.Add(time.Minute))
		if err := writeAll(parent, payload); err != nil {
			t.Fatal(err)
		}
		parent.Close()
		select {
		case <-ctx.Done():
		case <-time.After(time.Second):
			t.Fatal("invalid socket lease frame retained authority")
		}
		control.Close()
		guard.Wait()
		cancel()
	}
}

func TestControlRejectsConnectedTCP(t *testing.T) {
	listener, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan *net.TCPConn, 1)
	go func() { connection, _ := listener.AcceptTCP(); accepted <- connection }()
	client, err := net.DialTCP("tcp4", nil, listener.Addr().(*net.TCPAddr))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	server := <-accepted
	if server == nil {
		t.Fatal("TCP fixture did not connect")
	}
	defer server.Close()
	for _, connection := range []*net.TCPConn{client, server} {
		raw, err := connection.File()
		if err != nil {
			t.Fatal(err)
		}
		adopted, err := adoptControl(int(raw.Fd()), "TCP")
		raw.Close()
		if err == nil {
			adopted.Close()
			t.Fatal("connected TCP admitted as private control")
		}
	}
}
