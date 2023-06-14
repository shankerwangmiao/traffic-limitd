package server

import (
	"net"
	"os"
	"syscall"

	"golang.org/x/sys/unix"
	klog "k8s.io/klog/v2"
)

type ClientHandler interface {
	Handle(*ClientConn)
}

type Server struct {
	handler ClientHandler
}

type ClientConn struct {
	Conn            *net.UnixConn
	PeerCredentials *syscall.Ucred
}

func New(handler ClientHandler) *Server {
	return &Server{
		handler: handler,
	}
}

func (s *Server) ListenAndServe(address string) error {
	if err := os.RemoveAll(address); err != nil {
		klog.Fatal(err)
		return err
	}
	addr, err := net.ResolveUnixAddr("unixpacket", address)
	if err != nil {
		return err
	}
	oldMask := unix.Umask(0077)
	listener, err := net.ListenUnix("unixpacket", addr)
	unix.Umask(oldMask)
	if err != nil {
		return err
	}
	defer listener.Close()

	klog.Infof("Listening on %s", address)

	for {
		conn, err := listener.AcceptUnix()
		if err != nil {
			return err
		}

		go s.serveClient(conn)
	}
}

func (s *Server) serveClient(conn *net.UnixConn) {
	defer conn.Close()

	f, err := conn.File()
	if err != nil {
		klog.Errorf("Failed to get file descriptor: %v", err)
		return
	}
	cred, err := syscall.GetsockoptUcred(int(f.Fd()), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	if err != nil {
		klog.Errorf("Failed to get peer credentials: %v", err)
		_ = f.Close()
		return
	}
	_ = f.Close()
	s.handler.Handle(&ClientConn{
		Conn:            conn,
		PeerCredentials: cred,
	})
}
