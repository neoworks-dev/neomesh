package main

import (
	"encoding/binary"
	"testing"
)

func TestMarshalLedFrame(t *testing.T) {
	color := colorPreset{"warm", 0xff, 0x8c, 0x1a}
	frame, err := marshalLedFrame(0xA4CF1234ABCD0000, color, 200)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}

	const wantLen = headerSize + len("led/color") + 4
	if len(frame) != wantLen {
		t.Fatalf("len = %d, want %d", len(frame), wantLen)
	}
	if frame[0] != meshVersion || frame[1] != typeData {
		t.Errorf("version/type = %d/%d", frame[0], frame[1])
	}
	if got := binary.LittleEndian.Uint16(frame[2:4]); got != 13 {
		t.Errorf("payloadLen = %d, want 13", got)
	}
	if got := binary.LittleEndian.Uint64(frame[18:26]); got != 0xA4CF1234ABCD0000 {
		t.Errorf("dst = %016X", got)
	}
	if frame[26] != 8 {
		t.Errorf("ttl = %d, want 8", frame[26])
	}
	if frame[27] != 0 { // unicast → no broadcast flag
		t.Errorf("flags = %d, want 0", frame[27])
	}
	if frame[28] != 9 {
		t.Errorf("topicLen = %d, want 9", frame[28])
	}
	if string(frame[headerSize:headerSize+9]) != "led/color" {
		t.Errorf("topic = %q", frame[headerSize:headerSize+9])
	}
	payload := frame[headerSize+9:]
	if payload[0] != 0xff || payload[1] != 0x8c || payload[2] != 0x1a || payload[3] != 200 {
		t.Errorf("payload = % X", payload)
	}
}

func TestMarshalBroadcastFlag(t *testing.T) {
	frame, _ := marshalLedFrame(broadcastID, palette[0], 100)
	if frame[27]&flagBroadcast == 0 {
		t.Errorf("broadcast flag not set: %d", frame[27])
	}
}

func TestParseNodeID(t *testing.T) {
	cases := map[string]struct {
		want uint64
		ok   bool
	}{
		"[MESH] node id: A4CF1234ABCD0000":      {0xA4CF1234ABCD0000, true},
		"[APP] node id: 0011223344556677 extra": {0x0011223344556677, true},
		"some unrelated line":                   {0, false},
		"node id: short":                        {0, false},
	}
	for line, want := range cases {
		got, ok := parseNodeID(line)
		if ok != want.ok || (ok && got != want.want) {
			t.Errorf("parseNodeID(%q) = %016X,%v want %016X,%v", line, got, ok, want.want, want.ok)
		}
	}
}

func TestParseGatewayIP(t *testing.T) {
	addr, ok := parseGatewayIP("[GW]   IP:   192.168.1.50")
	if !ok || addr != "192.168.1.50:7778" {
		t.Errorf("got %q,%v", addr, ok)
	}
	if _, ok := parseGatewayIP("[APP] IP: 1.2.3.4"); ok {
		t.Errorf("non-[GW] line should not match")
	}
	if _, ok := parseGatewayIP("[GW] connecting"); ok {
		t.Errorf("line without IP should not match")
	}
}
