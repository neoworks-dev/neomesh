package main

import (
	"bufio"
	"net"
	"strings"
	"testing"
	"time"
)

func TestAttachHubReplaysBacklogThenStreams(t *testing.T) {
	h := newAttachHub()
	h.publish("12:00:00.000 [A] boot line") // backlog before any client connects

	addr, err := h.listen("127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	// give the server a moment to register the client, then push a live line
	time.Sleep(50 * time.Millisecond)
	h.publish("12:00:01.000 [B] live line")

	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	reader := bufio.NewReader(conn)

	var gotBacklog, gotLive bool
	for i := 0; i < 5 && (!gotBacklog || !gotLive); i++ {
		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}
		if strings.Contains(line, "boot line") {
			gotBacklog = true
		}
		if strings.Contains(line, "live line") {
			gotLive = true
		}
	}
	if !gotBacklog {
		t.Error("did not receive backlog line on attach")
	}
	if !gotLive {
		t.Error("did not receive live line after attach")
	}
}

func TestAttachHubDropsDeadClient(t *testing.T) {
	h := newAttachHub()
	addr, err := h.listen("127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	time.Sleep(50 * time.Millisecond)
	if h.count() != 1 {
		t.Fatalf("count = %d, want 1", h.count())
	}
	conn.Close()
	// the read goroutine notices the close and removes the client
	time.Sleep(100 * time.Millisecond)
	if h.count() != 0 {
		t.Errorf("count after close = %d, want 0", h.count())
	}
}
