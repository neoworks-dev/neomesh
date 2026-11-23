package main

import (
	"bufio"
	"context"
	"strconv"
	"strings"
	"time"

	"go.bug.st/serial"
)

const serialBaud = 115200

// listPorts returns likely USB-serial device paths, falling back to every
// reported port if none match the usual naming.
func listPorts() []string {
	ports, err := serial.GetPortsList()
	if err != nil {
		return nil
	}
	var usb []string
	for _, p := range ports {
		if strings.Contains(p, "ttyACM") || strings.Contains(p, "ttyUSB") ||
			strings.Contains(p, "cu.") {
			usb = append(usb, p)
		}
	}
	if len(usb) > 0 {
		return usb
	}
	return ports
}

// startSerialReader streams one port's lines into the program as logMsg values
// until ctx is cancelled. It retries the open for a few seconds so a board that
// is still re-enumerating after a flash reconnects on its own.
func startSerialReader(ctx context.Context, label, portName string) {
	port := openWithRetry(ctx, label, portName)
	if port == nil {
		return
	}
	go func() {
		<-ctx.Done()
		port.Close()
	}()

	send(logMsg{label, "sys", "── monitoring " + portName + " @" + strconv.Itoa(serialBaud) + " ──", false})

	scanner := bufio.NewScanner(port)
	scanner.Buffer(make([]byte, 0, 64*1024), 1<<20)
	for scanner.Scan() {
		send(logMsg{label, "dev", strings.TrimRight(scanner.Text(), "\r"), false})
	}

	select {
	case <-ctx.Done(): // closed on purpose, stay quiet
	default:
		send(logMsg{label, "sys", "── " + portName + " disconnected ──", true})
	}
}

func openWithRetry(ctx context.Context, label, portName string) serial.Port {
	mode := &serial.Mode{BaudRate: serialBaud}
	for attempt := 0; attempt < 25; attempt++ {
		port, err := serial.Open(portName, mode)
		if err == nil {
			return port
		}
		select {
		case <-ctx.Done():
			return nil
		case <-time.After(200 * time.Millisecond):
		}
	}
	send(logMsg{label, "sys", "could not open " + portName, true})
	return nil
}
