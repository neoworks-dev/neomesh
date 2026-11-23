package main

import "github.com/charmbracelet/lipgloss"

// Accent colours for the two device panes.
const (
	colorA    = lipgloss.Color("45")  // cyan
	colorB    = lipgloss.Color("213") // magenta
	colorFocus = lipgloss.Color("231") // bright white (focused border)
)

var (
	styleA = lipgloss.NewStyle().Foreground(colorA).Bold(true)
	styleB = lipgloss.NewStyle().Foreground(colorB).Bold(true)

	// log-line colours by message kind
	styleDev   = lipgloss.NewStyle().Foreground(lipgloss.Color("252"))
	styleBuild = lipgloss.NewStyle().Foreground(lipgloss.Color("220")) // yellow
	styleFlash = lipgloss.NewStyle().Foreground(lipgloss.Color("82"))  // green
	styleSys   = lipgloss.NewStyle().Foreground(lipgloss.Color("45"))  // cyan
	styleErr   = lipgloss.NewStyle().Foreground(lipgloss.Color("196")).Bold(true)
	styleTime  = lipgloss.NewStyle().Foreground(lipgloss.Color("240"))

	styleTitle = lipgloss.NewStyle().Foreground(lipgloss.Color("231")).
			Background(lipgloss.Color("57")).Bold(true).Padding(0, 1)
	styleField   = lipgloss.NewStyle().Foreground(lipgloss.Color("250"))
	styleFocused = lipgloss.NewStyle().Foreground(lipgloss.Color("231")).
			Background(lipgloss.Color("239")).Bold(true)
	styleStatus = lipgloss.NewStyle().Foreground(lipgloss.Color("250"))
	styleKey    = lipgloss.NewStyle().Foreground(lipgloss.Color("45")).Bold(true)
	styleHelp   = lipgloss.NewStyle().Foreground(lipgloss.Color("244"))
	styleLabel  = lipgloss.NewStyle().Foreground(lipgloss.Color("159")).Bold(true)
)

// styleForKind picks the colour for a log line by its kind.
func styleForKind(kind string, isErr bool) lipgloss.Style {
	if isErr {
		return styleErr
	}
	switch kind {
	case "build":
		return styleBuild
	case "flash":
		return styleFlash
	case "sys", "light":
		return styleSys
	default:
		return styleDev
	}
}
