package main

import (
	"errors"
	"io"
	"net"
	"os"
	"sync"
	"syscall"
	"time"
)

// chunkConnection adapts the relay's opaque message IPC to the byte stream TLS
// needs. One bounded inbound chunk is retained; writes have one frame in flight.
type chunkConnection struct {
	net.Conn
	readBuffer []byte
	writeLock  sync.Mutex
}

func (connection *chunkConnection) Read(output []byte) (int, error) {
	if len(output) == 0 {
		return 0, nil
	}
	if len(connection.readBuffer) == 0 {
		chunk, err := readFrame(connection.Conn, maximumChunk)
		if err != nil {
			return 0, err
		}
		connection.readBuffer = chunk
	}
	count := copy(output, connection.readBuffer)
	connection.readBuffer = connection.readBuffer[count:]
	return count, nil
}

func (connection *chunkConnection) Write(value []byte) (int, error) {
	connection.writeLock.Lock()
	defer connection.writeLock.Unlock()
	written := 0
	for len(value) > 0 {
		count := min(len(value), maximumChunk)
		if err := writeFrame(connection.Conn, value[:count], maximumChunk); err != nil {
			return written, err
		}
		written += count
		value = value[count:]
	}
	return written, nil
}

type pipeConnection struct {
	input interface {
		io.WriteCloser
		SetWriteDeadline(time.Time) error
	}
	output interface {
		io.ReadCloser
		SetReadDeadline(time.Time) error
	}
	once sync.Once
}

type pipeAddress struct{}

func (pipeAddress) Network() string { return "private-pipe" }
func (pipeAddress) String() string  { return "private-pipe" }
func (connection *pipeConnection) Read(value []byte) (int, error) {
	return connection.output.Read(value)
}
func (connection *pipeConnection) Write(value []byte) (int, error) {
	return connection.input.Write(value)
}
func (connection *pipeConnection) LocalAddr() net.Addr  { return pipeAddress{} }
func (connection *pipeConnection) RemoteAddr() net.Addr { return pipeAddress{} }
func (connection *pipeConnection) SetReadDeadline(deadline time.Time) error {
	return connection.output.SetReadDeadline(deadline)
}
func (connection *pipeConnection) SetWriteDeadline(deadline time.Time) error {
	return connection.input.SetWriteDeadline(deadline)
}
func (connection *pipeConnection) SetDeadline(deadline time.Time) error {
	if err := connection.SetReadDeadline(deadline); err != nil {
		return err
	}
	return connection.SetWriteDeadline(deadline)
}
func (connection *pipeConnection) Close() error {
	connection.once.Do(func() {
		connection.input.Close()
		connection.output.Close()
	})
	return nil
}

// Inherited Unix pipes can be blocking descriptors. Adopt a private duplicate
// in nonblocking mode before os.NewFile so Go's poller can interrupt every read
// and write on cancellation; closing an unpollable blocking fd is insufficient.
func adoptPipe(descriptor int, name string) (*os.File, error) {
	return adoptDescriptor(descriptor, name, false)
}

// Only the trusted lease-control descriptor may be a connected Unix stream
// socket, including the socketpair used by the Linux viewer. Media/status remain pipes.
func adoptControl(descriptor int, name string) (*os.File, error) {
	return adoptDescriptor(descriptor, name, true)
}

func connectedUnixStream(descriptor int) bool {
	kind, err := syscall.GetsockoptInt(descriptor, syscall.SOL_SOCKET, syscall.SO_TYPE)
	if err != nil || kind != syscall.SOCK_STREAM {
		return false
	}
	local, err := syscall.Getsockname(descriptor)
	if err != nil {
		return false
	}
	peer, err := syscall.Getpeername(descriptor)
	if err != nil {
		return false
	}
	_, localOK := local.(*syscall.SockaddrUnix)
	_, peerOK := peer.(*syscall.SockaddrUnix)
	// This inherited descriptor is selected by the trusted native parent, not
	// by wire data. Connected AF_UNIX stream identity is sufficient; endpoint
	// path names are not an authentication boundary (nor are FIFO names).
	return localOK && peerOK
}

func adoptDescriptor(descriptor int, name string, allowControlSocket bool) (*os.File, error) {
	duplicate, err := syscall.Dup(descriptor)
	if err != nil {
		return nil, errors.New("cannot adopt private IPC descriptor")
	}
	syscall.CloseOnExec(duplicate)
	var info syscall.Stat_t
	err = syscall.Fstat(duplicate, &info)
	valid := err == nil && info.Mode&syscall.S_IFMT == syscall.S_IFIFO
	if !valid && err == nil && allowControlSocket && info.Mode&syscall.S_IFMT == syscall.S_IFSOCK {
		valid = connectedUnixStream(duplicate)
	}
	if !valid {
		syscall.Close(duplicate)
		return nil, errors.New("private IPC descriptor type rejected")
	}
	syscall.CloseOnExec(descriptor)
	if err := syscall.SetNonblock(duplicate, true); err != nil {
		syscall.Close(duplicate)
		return nil, errors.New("cannot make private IPC cancellable")
	}
	file := os.NewFile(uintptr(duplicate), name)
	if file == nil {
		syscall.Close(duplicate)
		return nil, errors.New("invalid private IPC descriptor")
	}
	return file, nil
}
