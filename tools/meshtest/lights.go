package main

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"net"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/grandcat/zeroconf"
)

// Mesh wire protocol — must match include/MeshPacket.h.
const (
	meshVersion   = 1
	typeData      = 0x01
	flagBroadcast = 0x01
	bridgePort    = 7778 // MESH_BRIDGE_PORT = MESH_PORT + 1
	headerSize    = 29   // packed sizeof(MeshPacket)

	broadcastID uint64 = 0xFFFFFFFFFFFFFFFF

	mdnsService = "_neoworks._tcp"
	mdnsDomain  = "local."
)

// colorPreset is one selectable colour in the control bar.
type colorPreset struct {
	name    string
	r, g, b uint8
}

var palette = []colorPreset{
	{"red", 0xff, 0x00, 0x00},
	{"green", 0x00, 0xff, 0x00},
	{"blue", 0x00, 0x40, 0xff},
	{"warm", 0xff, 0x8c, 0x1a},
	{"cyan", 0x00, 0xe0, 0xff},
	{"purple", 0xb0, 0x00, 0xff},
	{"white", 0xff, 0xff, 0xff},
	{"off", 0x00, 0x00, 0x00},
}

func (c colorPreset) hex() string {
	return fmt.Sprintf("#%02x%02x%02x", c.r, c.g, c.b)
}

// marshalLedFrame builds the TcpBridge frame: 29-byte header + "led/color"
// topic + {r,g,b,brightness} payload, little-endian to match the packed C
// struct.
func marshalLedFrame(dst uint64, c colorPreset, brightness uint8) ([]byte, error) {
	const topic = "led/color"
	body := append([]byte(topic), c.r, c.g, c.b, brightness)

	var msgID [4]byte
	if _, err := rand.Read(msgID[:]); err != nil {
		return nil, err
	}

	frame := make([]byte, headerSize+len(body))
	frame[0] = meshVersion
	frame[1] = typeData
	binary.LittleEndian.PutUint16(frame[2:4], uint16(len(body)))
	copy(frame[4:8], msgID[:])
	// frame[8:10] seq = 0
	// frame[10:18] src = 0 (gateway fills its own identity in)
	binary.LittleEndian.PutUint64(frame[18:26], dst)
	frame[26] = 8 // ttl
	if dst == broadcastID {
		frame[27] = flagBroadcast
	}
	frame[28] = uint8(len(topic))
	copy(frame[headerSize:], body)
	return frame, nil
}

// sendLight pushes one colour command to the gateway's TcpBridge.
func sendLight(addr string, dst uint64, c colorPreset, brightness uint8) {
	frame, err := marshalLedFrame(dst, c, brightness)
	if err != nil {
		send(logMsg{"both", "light", "encode failed: " + err.Error(), true})
		return
	}

	conn, err := net.DialTimeout("tcp", addr, 4*time.Second)
	if err != nil {
		send(logMsg{"both", "light", "connect " + addr + ": " + err.Error(), true})
		return
	}
	defer conn.Close()

	if _, err := conn.Write(frame); err != nil {
		send(logMsg{"both", "light", "send failed: " + err.Error(), true})
		return
	}
	send(logMsg{"both", "light", fmt.Sprintf("sent %s b=%d → %s via %s",
		c.name, brightness, targetHex(dst), addr), false})
}

func targetHex(dst uint64) string {
	if dst == broadcastID {
		return "broadcast"
	}
	return fmt.Sprintf("%016X", dst)
}

// discoverGateway resolves the gateway's bridge address. It prefers the system
// avahi resolver (avahi-browse) because grandcat/zeroconf cannot bind UDP 5353
// when avahi-daemon already owns it — the usual reason in-process mDNS "doesn't
// work" on a Linux desktop. Falls back to zeroconf where avahi is absent.
func discoverGateway(timeout time.Duration) {
	if addr, ok := discoverViaAvahi(timeout); ok {
		send(gatewayMsg{addr: addr, via: "avahi"})
		return
	}
	if addr, ok := discoverViaZeroconf(timeout); ok {
		send(gatewayMsg{addr: addr, via: "mDNS"})
		return
	}
	send(logMsg{"both", "light",
		"no " + mdnsService + " gateway found — monitor a board for its [GW] IP, or pass -gw host:port",
		true})
}

func discoverViaAvahi(timeout time.Duration) (string, bool) {
	if _, err := exec.LookPath("avahi-browse"); err != nil {
		return "", false
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	// -r resolve, -p parsable, -t terminate, -k no service-type cache
	out, err := exec.CommandContext(ctx, "avahi-browse", "-rptk", mdnsService).Output()
	if err != nil {
		return "", false
	}
	// resolved lines start with '=' and are ';'-separated:
	//   =;iface;IPv4;name;type;domain;host;ADDRESS;PORT;txt
	for _, line := range strings.Split(string(out), "\n") {
		if !strings.HasPrefix(line, "=") {
			continue
		}
		fields := strings.Split(line, ";")
		if len(fields) < 9 || fields[2] != "IPv4" {
			continue
		}
		if net.ParseIP(fields[7]) == nil {
			continue
		}
		return net.JoinHostPort(fields[7], fields[8]), true
	}
	return "", false
}

func discoverViaZeroconf(timeout time.Duration) (string, bool) {
	resolver, err := zeroconf.NewResolver(nil)
	if err != nil {
		return "", false
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	entries := make(chan *zeroconf.ServiceEntry)
	result := make(chan string, 1)
	go func() {
		for entry := range entries {
			if len(entry.AddrIPv4) > 0 {
				result <- net.JoinHostPort(entry.AddrIPv4[0].String(), strconv.Itoa(entry.Port))
				cancel()
				return
			}
		}
	}()

	if err := resolver.Browse(ctx, mdnsService, mdnsDomain, entries); err != nil {
		return "", false
	}
	<-ctx.Done()
	select {
	case addr := <-result:
		return addr, true
	default:
		return "", false
	}
}

// ─── log sniffing — pull identity out of the serial stream ─────────────────

// parseNodeID extracts a 16-hex-digit NodeID following "node id:".
func parseNodeID(line string) (uint64, bool) {
	i := strings.Index(line, "node id:")
	if i < 0 {
		return 0, false
	}
	field := strings.TrimSpace(line[i+len("node id:"):])
	field = strings.Fields(field + " ")[0]
	if len(field) < 16 {
		return 0, false
	}
	v, err := strconv.ParseUint(field[:16], 16, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

// parseGatewayIP extracts the IP from a firmware "[GW]   IP:   1.2.3.4" line.
func parseGatewayIP(line string) (string, bool) {
	i := strings.Index(line, "IP:")
	if i < 0 || !strings.Contains(line, "[GW]") {
		return "", false
	}
	field := strings.TrimSpace(line[i+len("IP:"):])
	field = strings.Fields(field + " ")[0]
	if net.ParseIP(field) == nil {
		return "", false
	}
	return net.JoinHostPort(field, strconv.Itoa(bridgePort)), true
}
