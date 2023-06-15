package server

import (
	"context"
	"net"
	"os"
	"sync"
	"syscall"

	"golang.org/x/sys/unix"
	klog "k8s.io/klog/v2"
)

type ClientHandler interface {
	Handle(context.Context, *ClientConn)
}

type Server struct {
	handler  ClientHandler
	wg       sync.WaitGroup
	closed   bool
	listener *net.UnixListener
	mu       sync.Mutex
}

type ClientConn struct {
	Conn            *net.UnixConn
	PeerCredentials *syscall.Ucred
}

func New(handler ClientHandler) *Server {
	return &Server{
		handler: handler,
		closed:  false,
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
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	oldMask := unix.Umask(0077)
	listener, err := net.ListenUnix("unixpacket", addr)
	unix.Umask(oldMask)
	if err != nil {
		s.mu.Unlock()
		return err
	}
	defer listener.Close()
	s.listener = listener
	s.mu.Unlock()

	klog.Infof("Listening on %s", address)

	for {
		conn, err := listener.AcceptUnix()
		if err != nil {
			s.wg.Wait()
			if s.closed {
				return nil
			} else {
				return err
			}
		}

		s.wg.Add(1)
		go func() {
			s.serveClient(conn)
			s.wg.Done()
		}()
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

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	s.handler.Handle(ctx, &ClientConn{
		Conn:            conn,
		PeerCredentials: cred,
	})
}

func (s *Server) Close() {
	s.mu.Lock()
	s.closed = true
	s.listener.Close()
	s.mu.Unlock()
}
