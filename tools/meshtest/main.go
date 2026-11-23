// Command meshtest is a terminal UI for testing neomesh on two boards that are
// both connected to this computer over USB. It builds and flashes the firmware
// with PlatformIO, shows each board's serial log in its own pane, and embeds
// the meshctl light-control protocol so colour commands can be fired at either
// board (or broadcast) straight from the UI.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

const (
	maxLines    = 5000
	brightStep  = 16
	chromeLines = 5 // title + setup + control + status + help
)

// focusable fields
const (
	fEnv = iota
	fPortA
	fPortB
	fTarget
	fColor
	fBright
	fCount
)

// ─── messages ──────────────────────────────────────────────────────────────

var program *tea.Program

func send(msg tea.Msg) {
	if program != nil {
		program.Send(msg)
	}
}

type logMsg struct {
	pane  string // "A", "B", or "both"
	kind  string // "dev","build","flash","sys","light"
	text  string
	isErr bool
}

type cmdDoneMsg struct {
	label string
	err   error
}

type gatewayMsg struct {
	addr string
	via  string
}

// ─── model ─────────────────────────────────────────────────────────────────

type pane struct {
	vp viewport.Model
}

// logEvent is one line in the merged, time-ordered log. Both panes are laid out
// from the same event slice so entries with the same timestamp share a row.
type logEvent struct {
	pane   string // "A", "B", or "both"
	styled string
	raw    string
}

type model struct {
	root string

	envs   []string
	envIdx int

	ports []string
	idxA  int // index into ports, -1 = none
	idxB  int

	focus      int
	monitoring bool
	busy       bool
	follow     bool

	readers map[string]context.CancelFunc

	paneA pane
	paneB pane

	events []logEvent // merged, time-ordered log shared by both panes

	// identity sniffed from the serial logs
	nodeA, nodeB         uint64
	nodeAOK, nodeBOK     bool
	gatewayAddr, gateVia string

	// light controls
	targetIdx int // 0 broadcast, 1 device A, 2 device B
	colorIdx  int
	bright    uint8

	attachAddr string

	ready  bool
	status string
}

func newModel(root, gwOverride, attachAddr string) model {
	m := model{
		root:    root,
		envs:    parseEnvs(root),
		ports:   listPorts(),
		readers: map[string]context.CancelFunc{},
		follow:  true,
		idxA:       -1,
		idxB:       -1,
		bright:     128,
		attachAddr: attachAddr,
		status:     "ready — project " + root,
	}
	m.envIdx = preferEnv(m.envs, "led_node")
	if len(m.ports) > 0 {
		m.idxA = 0
	}
	if len(m.ports) > 1 {
		m.idxB = 1
	}
	if gwOverride != "" {
		m.gatewayAddr = gwOverride
		m.gateVia = "flag"
	}
	return m
}

func (m model) Init() tea.Cmd { return nil }

// ─── update ────────────────────────────────────────────────────────────────

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.resize(msg.Width, msg.Height)
		return m, nil

	case logMsg:
		m.handleLog(msg)
		return m, nil

	case gatewayMsg:
		m.gatewayAddr = msg.addr
		m.gateVia = msg.via
		m.status = "gateway: " + msg.addr + " (" + msg.via + ")"
		return m, nil

	case cmdDoneMsg:
		m.busy = false
		if msg.err != nil {
			m.status = msg.label + " failed: " + msg.err.Error()
			m.appendLine("both", "build", "FAILED: "+msg.err.Error(), true)
		} else {
			m.status = msg.label + " done"
		}
		if m.monitoring {
			m.ensureReaders()
		}
		return m, nil

	case tea.KeyMsg:
		return m.handleKey(msg)
	}
	return m, nil
}

func (m *model) resize(width, height int) {
	bodyHeight := height - chromeLines
	if bodyHeight < 3 {
		bodyHeight = 3
	}
	leftTotal := width / 2
	innerA := leftTotal - 2
	innerB := (width - leftTotal) - 2
	if innerA < 4 {
		innerA = 4
	}
	if innerB < 4 {
		innerB = 4
	}
	vpHeight := bodyHeight - 3 // border (2) + title (1)
	if vpHeight < 1 {
		vpHeight = 1
	}

	if !m.ready {
		m.paneA.vp = viewport.New(innerA, vpHeight)
		m.paneB.vp = viewport.New(innerB, vpHeight)
		m.ready = true
	} else {
		m.paneA.vp.Width, m.paneA.vp.Height = innerA, vpHeight
		m.paneB.vp.Width, m.paneB.vp.Height = innerB, vpHeight
	}
	m.refreshContent()
}

func (m model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q":
		m.stopAllReaders()
		return m, tea.Quit
	case "tab":
		m.focus = (m.focus + 1) % fCount
		return m, nil
	case "shift+tab":
		m.focus = (m.focus + fCount - 1) % fCount
		return m, nil
	case "left":
		m.cycleField(-1)
		return m, nil
	case "right":
		m.cycleField(1)
		return m, nil
	case "enter", "g":
		m.sendLight()
		return m, nil
	case "b":
		return m.startBuild()
	case "1":
		return m.startFlash([]string{"A"})
	case "2":
		return m.startFlash([]string{"B"})
	case "f":
		return m.startFlash([]string{"A", "B"})
	case "m":
		m.toggleMonitor()
		return m, nil
	case "d":
		m.status = "discovering gateway via mDNS…"
		go discoverGateway(5 * time.Second)
		return m, nil
	case "r":
		m.rescan()
		return m, nil
	case "c":
		m.clearLogs()
		return m, nil
	case "s":
		m.save()
		return m, nil
	case "up", "down", "pgup", "pgdown", "home", "end":
		m.scroll(msg)
		return m, nil
	}
	return m, nil
}

func (m *model) scroll(msg tea.KeyMsg) {
	if !m.ready {
		return
	}
	m.paneA.vp, _ = m.paneA.vp.Update(msg)
	m.paneB.vp, _ = m.paneB.vp.Update(msg)
	m.follow = m.paneA.vp.AtBottom() && m.paneB.vp.AtBottom()
}

// ─── actions ───────────────────────────────────────────────────────────────

func (m model) startBuild() (tea.Model, tea.Cmd) {
	if m.busy || !m.haveEnv() {
		m.statusIf(!m.haveEnv(), "no env selected")
		return m, nil
	}
	m.busy = true
	m.status = "building " + m.env()
	go runBuild(m.root, m.env())
	return m, nil
}

func (m model) startFlash(labels []string) (tea.Model, tea.Cmd) {
	if m.busy || !m.haveEnv() {
		m.statusIf(!m.haveEnv(), "no env selected")
		return m, nil
	}

	var jobs []flashJob
	seen := map[string]bool{}
	for _, label := range labels {
		port := m.portFor(label)
		if port != "" && !seen[port] {
			jobs = append(jobs, flashJob{pane: label, port: port})
			seen[port] = true
		}
	}
	if len(jobs) == 0 {
		m.status = "no port selected to flash"
		return m, nil
	}

	m.busy = true
	for _, job := range jobs {
		m.stopReader(job.port)
	}
	m.status = "flashing " + strings.Join(labels, "+")
	go runFlash(m.root, m.env(), jobs)
	return m, nil
}

func (m *model) sendLight() {
	if m.gatewayAddr == "" {
		m.status = "gateway unknown — monitor a board so it prints its IP, or press d"
		return
	}
	dst, label, ok := m.lightDst()
	if !ok {
		m.status = "target " + label + " node id not seen yet"
		return
	}
	color := palette[m.colorIdx]
	bright := m.bright
	addr := m.gatewayAddr
	m.status = "sending " + color.name + " → " + label
	go sendLight(addr, dst, color, bright)
}

func (m model) lightDst() (uint64, string, bool) {
	switch m.targetIdx {
	case 1:
		return m.nodeA, "A", m.nodeAOK
	case 2:
		return m.nodeB, "B", m.nodeBOK
	default:
		return broadcastID, "broadcast", true
	}
}

func (m *model) toggleMonitor() {
	if m.monitoring {
		m.stopAllReaders()
		m.monitoring = false
		m.status = "monitor stopped"
		return
	}
	if m.portFor("A") == "" && m.portFor("B") == "" {
		m.status = "no ports selected"
		return
	}
	m.monitoring = true
	m.ensureReaders()
	m.status = "monitoring"
}

func (m *model) rescan() {
	m.ports = listPorts()
	if m.idxA >= len(m.ports) {
		m.idxA = len(m.ports) - 1
	}
	if m.idxB >= len(m.ports) {
		m.idxB = len(m.ports) - 1
	}
	m.status = "rescanned: " + strconv.Itoa(len(m.ports)) + " port(s)"
}

func (m *model) clearLogs() {
	m.events = nil
	if m.ready {
		m.refreshContent()
	}
}

func (m *model) save() {
	var rawA, rawB []string
	for _, e := range m.events {
		if e.pane == "A" || e.pane == "both" {
			rawA = append(rawA, e.raw)
		}
		if e.pane == "B" || e.pane == "both" {
			rawB = append(rawB, e.raw)
		}
	}
	if len(rawA)+len(rawB) == 0 {
		m.status = "nothing to save"
		return
	}
	all := append(append([]string{"== device A =="}, rawA...),
		append([]string{"", "== device B =="}, rawB...)...)
	name := "meshtest-" + time.Now().Format("20060102-150405") + ".log"
	path := filepath.Join(m.root, name)
	if err := os.WriteFile(path, []byte(strings.Join(all, "\n")+"\n"), 0o644); err != nil {
		m.status = "save failed: " + err.Error()
		return
	}
	m.status = "saved " + path
}

func (m *model) cycleField(dir int) {
	switch m.focus {
	case fEnv:
		if len(m.envs) > 0 {
			m.envIdx = wrap(m.envIdx+dir, len(m.envs))
		}
	case fPortA:
		m.idxA = cyclePort(m.idxA, dir, len(m.ports))
		m.refreshReadersIfMonitoring()
	case fPortB:
		m.idxB = cyclePort(m.idxB, dir, len(m.ports))
		m.refreshReadersIfMonitoring()
	case fTarget:
		m.targetIdx = wrap(m.targetIdx+dir, 3)
	case fColor:
		m.colorIdx = wrap(m.colorIdx+dir, len(palette))
	case fBright:
		m.bright = stepBright(m.bright, dir)
	}
}

// ─── log handling + sniffing ───────────────────────────────────────────────

func (m *model) handleLog(msg logMsg) {
	m.appendLine(msg.pane, msg.kind, msg.text, msg.isErr)
	if msg.kind != "dev" {
		return
	}
	m.sniff(msg.pane, msg.text)
}

func (m *model) sniff(pane, text string) {
	if pane == "A" && !m.nodeAOK {
		if id, ok := parseNodeID(text); ok {
			m.nodeA, m.nodeAOK = id, true
		}
	}
	if pane == "B" && !m.nodeBOK {
		if id, ok := parseNodeID(text); ok {
			m.nodeB, m.nodeBOK = id, true
		}
	}
	if addr, ok := parseGatewayIP(text); ok && m.gateVia != "flag" {
		if m.gatewayAddr != addr {
			m.status = "gateway: " + addr + " (log:" + pane + ")"
		}
		m.gatewayAddr = addr
		m.gateVia = "log:" + pane
	}
}

func (m *model) appendLine(target, kind, text string, isErr bool) {
	ts := time.Now().Format("15:04:05.000")
	styled := fmt.Sprintf("%s %s", styleTime.Render(ts),
		styleForKind(kind, isErr).Render(text))
	raw := ts + " " + text

	// fan out to any attached log clients, tagged with the source pane
	publishLog(fmt.Sprintf("%s [%s] %s", ts, target, text))

	m.events = append(m.events, logEvent{pane: target, styled: styled, raw: raw})
	if len(m.events) > maxLines {
		m.events = m.events[len(m.events)-maxLines:]
	}
	if m.ready {
		m.refreshContent()
	}
}

func (m *model) refreshContent() {
	colA, colB := m.columns()
	m.paneA.vp.SetContent(strings.Join(colA, "\n"))
	m.paneB.vp.SetContent(strings.Join(colB, "\n"))
	if m.follow {
		m.paneA.vp.GotoBottom()
		m.paneB.vp.GotoBottom()
	}
}

// columns lays the merged event log into two aligned columns. Every event takes
// the same row in both panes, so same-timestamp entries line up horizontally;
// the pane that did not emit an event gets a blank cell on that row.
func (m *model) columns() (colA, colB []string) {
	colA = make([]string, 0, len(m.events))
	colB = make([]string, 0, len(m.events))
	for _, e := range m.events {
		switch e.pane {
		case "A":
			colA = append(colA, e.styled)
			colB = append(colB, "")
		case "B":
			colA = append(colA, "")
			colB = append(colB, e.styled)
		default:
			colA = append(colA, e.styled)
			colB = append(colB, e.styled)
		}
	}
	return colA, colB
}

// ─── serial reader management ──────────────────────────────────────────────

func (m *model) ensureReaders() {
	for _, d := range []struct{ label, port string }{
		{"A", m.portFor("A")}, {"B", m.portFor("B")},
	} {
		if d.port == "" {
			continue
		}
		if _, running := m.readers[d.port]; running {
			continue
		}
		ctx, cancel := context.WithCancel(context.Background())
		m.readers[d.port] = cancel
		go startSerialReader(ctx, d.label, d.port)
	}
}

func (m *model) stopReader(port string) {
	if cancel, ok := m.readers[port]; ok {
		cancel()
		delete(m.readers, port)
	}
}

func (m *model) stopAllReaders() {
	for port, cancel := range m.readers {
		cancel()
		delete(m.readers, port)
	}
}

func (m *model) refreshReadersIfMonitoring() {
	if !m.monitoring {
		return
	}
	m.stopAllReaders()
	m.ensureReaders()
}

// ─── view ──────────────────────────────────────────────────────────────────

func (m model) View() string {
	if !m.ready {
		return "starting…"
	}
	title := styleTitle.Render("neomesh test harness")

	setupParts := []string{
		m.fieldView(fEnv, "env", m.env()),
		m.fieldView(fPortA, "A", portLabel(m.portFor("A"))),
		m.fieldView(fPortB, "B", portLabel(m.portFor("B"))),
		m.statusBadge(),
	}
	if m.attachAddr != "" {
		setupParts = append(setupParts,
			dim(fmt.Sprintf("⇄ attach %s (%d)", m.attachAddr, attachCount())))
	}
	setup := strings.Join(setupParts, "  ")

	panes := lipgloss.JoinHorizontal(lipgloss.Top,
		renderPane(m.paneTitle("A", m.nodeAOK, m.nodeA), m.paneA.vp, colorA, m.focus == fPortA),
		renderPane(m.paneTitle("B", m.nodeBOK, m.nodeB), m.paneB.vp, colorB, m.focus == fPortB))

	control := m.controlBar()
	status := styleStatus.Render("• " + m.status)
	help := styleHelp.Render(strings.Join([]string{
		key("tab") + " field", key("←/→") + " change", key("⏎/g") + " send light",
		key("b") + " build", key("1/2/f") + " flash", key("m") + " mon",
		key("d") + " discover", key("r") + " rescan", key("c") + " clr",
		key("s") + " save", key("q") + " quit",
	}, "  "))

	return strings.Join([]string{title, setup, panes, control, status, help}, "\n")
}

func (m model) controlBar() string {
	color := palette[m.colorIdx]
	swatch := lipgloss.NewStyle().Background(lipgloss.Color(color.hex())).Render("  ")

	gw := m.gatewayAddr
	if gw == "" {
		gw = "(unknown)"
	} else {
		gw += " " + dim("via "+m.gateVia)
	}

	parts := []string{
		styleLabel.Render("Lights"),
		m.lightField(fTarget, "target", m.targetLabel()),
		m.lightField(fColor, "color", swatch+" "+color.name),
		m.lightField(fBright, "bright", strconv.Itoa(int(m.bright))),
		styleField.Render("gw: " + gw),
	}
	return strings.Join(parts, "  ")
}

func (m model) targetLabel() string {
	switch m.targetIdx {
	case 1:
		return "A " + nodeStr(m.nodeAOK, m.nodeA)
	case 2:
		return "B " + nodeStr(m.nodeBOK, m.nodeB)
	default:
		return "broadcast"
	}
}

func (m model) paneTitle(label string, ok bool, id uint64) string {
	port := portLabel(m.portFor(label))
	return label + "  " + port + "  " + nodeStr(ok, id)
}

func (m model) fieldView(idx int, label, val string) string {
	return styleSelectable(m.focus == idx, " "+label+": "+val+" ")
}

func (m model) lightField(idx int, label, val string) string {
	return styleSelectable(m.focus == idx, " "+label+": "+val+" ")
}

func styleSelectable(focused bool, text string) string {
	if focused {
		return styleFocused.Render(text)
	}
	return styleField.Render(text)
}

func (m model) statusBadge() string {
	switch {
	case m.busy:
		return styleBuild.Render("⏳ busy")
	case m.monitoring:
		return styleFlash.Render("● monitoring")
	default:
		return styleHelp.Render("○ idle")
	}
}

// renderPane wraps a viewport in a titled, coloured border.
func renderPane(title string, vp viewport.Model, accent lipgloss.Color, focused bool) string {
	border := accent
	if focused {
		border = colorFocus
	}
	box := lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).
		BorderForeground(border).Width(vp.Width)
	titleBar := lipgloss.NewStyle().Foreground(accent).Bold(true).
		Render(fitLine(title, vp.Width))
	return box.Render(titleBar + "\n" + vp.View())
}

// ─── accessors / helpers ───────────────────────────────────────────────────

func (m model) haveEnv() bool { return m.env() != "" }

func (m model) env() string {
	if m.envIdx >= 0 && m.envIdx < len(m.envs) {
		return m.envs[m.envIdx]
	}
	return ""
}

func (m model) portFor(label string) string {
	idx := m.idxA
	if label == "B" {
		idx = m.idxB
	}
	if idx >= 0 && idx < len(m.ports) {
		return m.ports[idx]
	}
	return ""
}

func (m *model) statusIf(cond bool, s string) {
	if cond {
		m.status = s
	}
}

func key(s string) string { return styleKey.Render(s) }
func dim(s string) string { return styleHelp.Render(s) }
func wrap(i, n int) int   { return ((i % n) + n) % n }

func portLabel(p string) string {
	if p == "" {
		return "(none)"
	}
	return p
}

func nodeStr(ok bool, id uint64) string {
	if !ok {
		return "(no id)"
	}
	return fmt.Sprintf("%016X", id)
}

func stepBright(b uint8, dir int) uint8 {
	v := int(b) + dir*brightStep
	if v < 0 {
		v = 0
	}
	if v > 255 {
		v = 255
	}
	return uint8(v)
}

func cyclePort(idx, dir, n int) int {
	idx += dir
	if idx < -1 {
		return n - 1
	}
	if idx >= n {
		return -1
	}
	return idx
}

func fitLine(s string, w int) string {
	r := []rune(s)
	if len(r) > w {
		return string(r[:w])
	}
	return s + strings.Repeat(" ", w-len(r))
}

func preferEnv(envs []string, want string) int {
	for i, e := range envs {
		if e == want {
			return i
		}
	}
	return 0
}

var envHeaderRe = regexp.MustCompile(`(?m)^\[env:([^\]]+)\]`)

func parseEnvs(root string) []string {
	data, err := os.ReadFile(filepath.Join(root, "platformio.ini"))
	if err != nil {
		return nil
	}
	var envs []string
	seen := map[string]bool{}
	for _, match := range envHeaderRe.FindAllStringSubmatch(string(data), -1) {
		if !seen[match[1]] {
			envs = append(envs, match[1])
			seen[match[1]] = true
		}
	}
	return envs
}

func findProjectRoot(start string) string {
	dir := start
	for {
		if _, err := os.Stat(filepath.Join(dir, "platformio.ini")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return start
		}
		dir = parent
	}
}

// ─── main ──────────────────────────────────────────────────────────────────

func main() {
	dir := flag.String("C", "", "project root (default: auto-detect platformio.ini)")
	gw := flag.String("gw", "", "gateway bridge address host:port (skip discovery)")
	attach := flag.String("attach", "127.0.0.1:7700", "stream captured logs to TCP clients here (empty to disable)")
	flag.Parse()

	root := *dir
	if root == "" {
		cwd, err := os.Getwd()
		if err != nil {
			fmt.Fprintln(os.Stderr, "error:", err)
			os.Exit(1)
		}
		root = findProjectRoot(cwd)
	}

	attachAddr := ""
	if *attach != "" {
		hub = newAttachHub()
		resolved, err := hub.listen(*attach)
		if err != nil {
			fmt.Fprintln(os.Stderr, "attach listen:", err)
			hub = nil
		} else {
			attachAddr = resolved
		}
	}

	program = tea.NewProgram(newModel(root, *gw, attachAddr), tea.WithAltScreen())
	if _, err := program.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}
