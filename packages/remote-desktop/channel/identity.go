package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"math/big"
	"os"
	"path/filepath"
	"syscall"
	"time"
)

func owned(info os.FileInfo) bool {
	status, ok := info.Sys().(*syscall.Stat_t)
	return ok && status.Uid == uint32(os.Getuid())
}

func privateDirectory(path string) error {
	info, err := os.Lstat(path)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm() != 0700 || !owned(info) {
		return errors.New("identity directory must be a private real directory")
	}
	return nil
}

func privateFile(path string, maximum int64) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm() != 0600 || !owned(info) || info.Size() <= 0 || info.Size() > maximum {
		return nil, errors.New("channel file must be private, regular and bounded")
	}
	// The directory is caller-owned0700 and files are never resolved from wire
	// values. Open+Stat checks also reject replacement with a different inode.
	file, err := os.Open(path)
	if err != nil {
		return nil, errors.New("cannot open channel file")
	}
	defer file.Close()
	actual, err := file.Stat()
	if err != nil || !os.SameFile(info, actual) {
		return nil, errors.New("channel file changed during open")
	}
	data := make([]byte, info.Size())
	if _, err := file.ReadAt(data, 0); err != nil {
		return nil, errors.New("cannot read channel file")
	}
	return data, nil
}

func exclusiveFile(path string, content []byte) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return errors.New("cannot create fresh private identity file")
	}
	writeErr := writeAll(file, content)
	closeErr := file.Close()
	if writeErr != nil || closeErr != nil {
		return errors.New("cannot write private identity file")
	}
	return nil
}

func createIdentity(directory string, now time.Time) (string, error) {
	// Caller supplies a fresh path; never chmod or reuse existing data.
	if err := os.Mkdir(directory, 0700); err != nil {
		return "", errors.New("identity directory must be fresh")
	}
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return "", errors.New("cannot generate channel identity")
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return "", errors.New("cannot generate certificate serial")
	}
	template := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "Dockpipe ephemeral desktop"},
		NotBefore:             now.Add(-time.Minute),
		NotAfter:              now.Add(time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth, x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	certificate, err := x509.CreateCertificate(rand.Reader, template, template, public, private)
	if err != nil {
		return "", errors.New("cannot generate channel certificate")
	}
	privateDER, err := x509.MarshalPKCS8PrivateKey(private)
	if err != nil {
		return "", errors.New("cannot encode channel identity")
	}
	if err := exclusiveFile(filepath.Join(directory, "key.pem"), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: privateDER})); err != nil {
		return "", err
	}
	if err := exclusiveFile(filepath.Join(directory, "cert.pem"), pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certificate})); err != nil {
		return "", err
	}
	sum := sha256.Sum256(certificate)
	return hex.EncodeToString(sum[:]), nil
}

func loadIdentity(directory, expected string) (tls.Certificate, error) {
	if err := privateDirectory(directory); err != nil {
		return tls.Certificate{}, err
	}
	certificate, err := privateFile(filepath.Join(directory, "cert.pem"), maximumJSON)
	if err != nil {
		return tls.Certificate{}, err
	}
	key, err := privateFile(filepath.Join(directory, "key.pem"), maximumJSON)
	if err != nil {
		return tls.Certificate{}, err
	}
	identity, err := tls.X509KeyPair(certificate, key)
	if err != nil || len(identity.Certificate) != 1 {
		return tls.Certificate{}, errors.New("invalid channel identity")
	}
	sum := sha256.Sum256(identity.Certificate[0])
	if !digest(expected) || hex.EncodeToString(sum[:]) != expected {
		return tls.Certificate{}, errors.New("local channel fingerprint mismatch")
	}
	return identity, nil
}

func tlsConfiguration(identity tls.Certificate, peerPin string, now func() time.Time) (*tls.Config, error) {
	if !digest(peerPin) {
		return nil, errors.New("invalid peer fingerprint")
	}
	return &tls.Config{
		MinVersion:             tls.VersionTLS13,
		MaxVersion:             tls.VersionTLS13,
		Certificates:           []tls.Certificate{identity},
		ClientAuth:             tls.RequireAnyClientCert,
		InsecureSkipVerify:     true, // Exact authenticated leaf pin replaces PKI, below.
		SessionTicketsDisabled: true,
		ClientSessionCache:     nil,
		NextProtos:             []string{channelVersion},
		Time:                   now,
		VerifyConnection: func(connection tls.ConnectionState) error {
			if connection.Version != tls.VersionTLS13 || connection.DidResume || connection.NegotiatedProtocol != channelVersion || len(connection.PeerCertificates) != 1 {
				return errors.New("channel TLS policy rejected")
			}
			leaf := connection.PeerCertificates[0]
			sum := sha256.Sum256(leaf.Raw)
			if hex.EncodeToString(sum[:]) != peerPin || now().Before(leaf.NotBefore) || !now().Before(leaf.NotAfter) || leaf.PublicKeyAlgorithm != x509.Ed25519 {
				return errors.New("peer channel identity rejected")
			}
			return nil
		},
	}, nil
}
