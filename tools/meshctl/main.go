// Command meshctl discovers a neomesh gateway via mDNS and sends control
// messages to it over the TcpBridge protocol.
//
// Example:
//
//	meshctl set --node a4cf1234abcd0000 --color ff8800 --brightness 128
//	meshctl set --node broadcast --color 000000 --brightness 0   # all off
package main

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/grandcat/zeroconf"
)

// ─── mesh wire protocol (must match include/MeshPacket.h) ──────────────────

const (
	meshVersion = 1
	typeData    = 0x01

	flagBroadcast = 0x01

	broadcastID NodeID = 0xFFFFFFFFFFFFFFFF

	headerSize = 29 // sizeof(MeshPacket), packed

	mdnsService = "_neoworks._tcp"
	mdnsDomain  = "local."
)

type NodeID uint64

// meshPacket mirrors the packed C struct MeshPacket (29 bytes, little-endian).
type meshPacket struct {
	version    uint8
	pType      uint8
	payloadLen uint16
	msgID      uint32
	seq        uint16
	src        NodeID
	dst        NodeID
	ttl        uint8
	flags      uint8
	topicLen   uint8
}

func (p *meshPacket) marshal() []byte {
	b := make([]byte, headerSize)
	b[0] = p.version
	b[1] = p.pType
	binary.LittleEndian.PutUint16(b[2:4], p.payloadLen)
	binary.LittleEndian.PutUint32(b[4:8], p.msgID)
	binary.LittleEndian.PutUint16(b[8:10], p.seq)
	binary.LittleEndian.PutUint64(b[10:18], uint64(p.src))
	binary.LittleEndian.PutUint64(b[18:26], uint64(p.dst))
	b[26] = p.ttl
	b[27] = p.flags
	b[28] = p.topicLen
	return b
}

// colorCmd mirrors the firmware's ColorCmd{r,g,b,brightness} (4 bytes).
type colorCmd struct {
	r, g, b, brightness uint8
}

func (c colorCmd) bytes() []byte { return []byte{c.r, c.g, c.b, c.brightness} }

// ─── mDNS discovery ──────────────────────────────────────────────────────────

type gateway struct {
	addr   string // host:port for the TcpBridge
	nodeID string // gateway's own NodeID, from TXT record (informational)
}

func discoverGateway(timeout time.Duration) (*gateway, error) {
	resolver, err := zeroconf.NewResolver(nil)
	if err != nil {
		return nil, fmt.Errorf("create resolver: %w", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	entries := make(chan *zeroconf.ServiceEntry)
	var found *zeroconf.ServiceEntry

	done := make(chan struct{})
	go func() {
		defer close(done)
		for entry := range entries {
			found = entry
			cancel()
			return
		}
	}()

	if err := resolver.Browse(ctx, mdnsService, mdnsDomain, entries); err != nil {
		return nil, fmt.Errorf("browse %s: %w", mdnsService, err)
	}

	<-ctx.Done()
	<-done

	if found == nil {
		return nil, fmt.Errorf("no neomesh gateway found via mDNS (%s) within %s", mdnsService, timeout)
	}
	if len(found.AddrIPv4) == 0 {
		return nil, fmt.Errorf("gateway %q advertised no IPv4 address", found.HostName)
	}

	gw := &gateway{
		addr: net.JoinHostPort(found.AddrIPv4[0].String(), strconv.Itoa(found.Port)),
	}
	for _, txt := range found.Text {
		if id, ok := strings.CutPrefix(txt, "node_id="); ok {
			gw.nodeID = id
		}
	}
	return gw, nil
}

// ─── command: set ────────────────────────────────────────────────────────────

func sendColor(addr string, dst NodeID, color colorCmd) error {
	const topic = "led/color"
	payload := color.bytes()
	body := append([]byte(topic), payload...)

	var msgIDBytes [4]byte
	if _, err := rand.Read(msgIDBytes[:]); err != nil {
		return fmt.Errorf("generate msgId: %w", err)
	}

	pkt := meshPacket{
		version:    meshVersion,
		pType:      typeData,
		payloadLen: uint16(len(body)),
		msgID:      binary.LittleEndian.Uint32(msgIDBytes[:]),
		dst:        dst,
		ttl:        8,
		topicLen:   uint8(len(topic)),
	}
	if dst == broadcastID {
		pkt.flags |= flagBroadcast
	}

	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return fmt.Errorf("connect to gateway %s: %w", addr, err)
	}
	defer conn.Close()

	frame := append(pkt.marshal(), body...)
	if _, err := conn.Write(frame); err != nil {
		return fmt.Errorf("write to gateway: %w", err)
	}
	return nil
}

// ─── CLI ─────────────────────────────────────────────────────────────────────

func parseNodeID(s string) (NodeID, error) {
	if strings.EqualFold(s, "broadcast") || strings.EqualFold(s, "all") {
		return broadcastID, nil
	}
	s = strings.TrimPrefix(strings.TrimPrefix(s, "0x"), "0X")
	v, err := strconv.ParseUint(s, 16, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid --node %q: expected 16 hex digits or \"broadcast\"", s)
	}
	return NodeID(v), nil
}

func parseColor(s string) (colorCmd, error) {
	b, err := hex.DecodeString(strings.TrimPrefix(s, "#"))
	if err != nil || len(b) != 3 {
		return colorCmd{}, fmt.Errorf("invalid --color %q: expected RRGGBB hex", s)
	}
	return colorCmd{r: b[0], g: b[1], b: b[2]}, nil
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(1)
	}

	switch os.Args[1] {
	case "discover":
		runDiscover(os.Args[2:])
	case "set":
		runSet(os.Args[2:])
	case "-h", "--help", "help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n", os.Args[1])
		usage()
		os.Exit(1)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `meshctl — control a neomesh network

Usage:
  meshctl discover [--timeout 5s]
      Find the active gateway via mDNS and print its address.

  meshctl set --node <id|broadcast> --color RRGGBB [--brightness 0-255] [--addr host:port]
      Set LED color/brightness on a node. Discovers the gateway via mDNS
      unless --addr is given.

Examples:
  meshctl discover
  meshctl set --node a4cf1234abcd0000 --color ff8800 --brightness 128
  meshctl set --node broadcast --color 000000 --brightness 0`)
}

func runDiscover(args []string) {
	fs := flag.NewFlagSet("discover", flag.ExitOnError)
	timeout := fs.Duration("timeout", 5*time.Second, "mDNS discovery timeout")
	fs.Parse(args)

	gw, err := discoverGateway(*timeout)
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	fmt.Printf("gateway: %s\n", gw.addr)
	if gw.nodeID != "" {
		fmt.Printf("node_id: %s\n", gw.nodeID)
	}
}

func runSet(args []string) {
	fs := flag.NewFlagSet("set", flag.ExitOnError)
	addrFlag := fs.String("addr", "", "gateway bridge address host:port (skip mDNS discovery)")
	nodeFlag := fs.String("node", "broadcast", "target NodeID as 16 hex digits, or \"broadcast\"")
	colorFlag := fs.String("color", "ffffff", "color as RRGGBB hex")
	brightnessFlag := fs.Uint("brightness", 128, "brightness 0-255")
	timeout := fs.Duration("timeout", 5*time.Second, "mDNS discovery timeout")
	fs.Parse(args)

	dst, err := parseNodeID(*nodeFlag)
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}

	color, err := parseColor(*colorFlag)
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	if *brightnessFlag > 255 {
		fmt.Fprintln(os.Stderr, "error: --brightness must be 0-255")
		os.Exit(1)
	}
	color.brightness = uint8(*brightnessFlag)

	addr := *addrFlag
	if addr == "" {
		fmt.Fprintln(os.Stderr, "discovering neomesh gateway via mDNS...")
		gw, err := discoverGateway(*timeout)
		if err != nil {
			fmt.Fprintln(os.Stderr, "error:", err)
			os.Exit(1)
		}
		fmt.Fprintf(os.Stderr, "found gateway at %s\n", gw.addr)
		addr = gw.addr
	}

	if err := sendColor(addr, dst, color); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	fmt.Printf("sent: node=%016x color=#%02x%02x%02x brightness=%d\n",
		uint64(dst), color.r, color.g, color.b, color.brightness)
}
