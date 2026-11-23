package main

import (
	"bufio"
	"net"
	"sync"
	"time"
)

const attachBacklog = 1000

// attachHub fans the captured log out to any TCP client that connects, so the
// live logs can be watched from outside the full-screen TUI (e.g. `nc
// 127.0.0.1 7700` in another terminal, or an agent attaching to the stream).
// Each new client first receives the recent backlog, then live lines.
type attachHub struct {
	mu      sync.Mutex
	clients map[net.Conn]*bufio.Writer
	backlog []string
}

func newAttachHub() *attachHub {
	return &attachHub{clients: map[net.Conn]*bufio.Writer{}}
}

// listen binds addr and serves clients in the background. It returns the
// resolved address (so ":0" / host:0 yields the actual chosen port).
func (h *attachHub) listen(addr string) (string, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return "", err
	}
	go h.acceptLoop(ln)
	return ln.Addr().String(), nil
}

func (h *attachHub) acceptLoop(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		h.add(conn)
	}
}

func (h *attachHub) add(conn net.Conn) {
	writer := bufio.NewWriter(conn)

	h.mu.Lock()
	writer.WriteString("── attached to meshtest ──\n")
	for _, line := range h.backlog {
		writer.WriteString(line + "\n")
	}
	writer.Flush()
	h.clients[conn] = writer
	h.mu.Unlock()

	// drop the client as soon as it disconnects (we ignore anything it sends)
	go func() {
		buf := make([]byte, 256)
		for {
			if _, err := conn.Read(buf); err != nil {
				h.remove(conn)
				return
			}
		}
	}()
}

func (h *attachHub) remove(conn net.Conn) {
	h.mu.Lock()
	if _, ok := h.clients[conn]; ok {
		delete(h.clients, conn)
		conn.Close()
	}
	h.mu.Unlock()
}

// publish records the line in the backlog and pushes it to every client.
func (h *attachHub) publish(line string) {
	h.mu.Lock()
	defer h.mu.Unlock()

	h.backlog = append(h.backlog, line)
	if len(h.backlog) > attachBacklog {
		h.backlog = h.backlog[len(h.backlog)-attachBacklog:]
	}

	for conn, writer := range h.clients {
		conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
		if _, err := writer.WriteString(line + "\n"); err != nil {
			delete(h.clients, conn)
			conn.Close()
			continue
		}
		if err := writer.Flush(); err != nil {
			delete(h.clients, conn)
			conn.Close()
		}
	}
}

func (h *attachHub) count() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.clients)
}

// ─── package-level access ──────────────────────────────────────────────────

var hub *attachHub

func publishLog(line string) {
	if hub != nil {
		hub.publish(line)
	}
}

func attachCount() int {
	if hub == nil {
		return 0
	}
	return hub.count()
}
