package main

import (
	"bufio"
	"io"
	"os/exec"
	"strings"
)

// flashJob pairs an upload port with the pane its output should land in.
type flashJob struct {
	pane string
	port string
}

// streamPio runs a `pio` subcommand in the project root and forwards each
// output line as a logMsg into the given pane.
func streamPio(pane, kind, root string, args ...string) error {
	cmd := exec.Command("pio", args...)
	cmd.Dir = root

	pipeReader, pipeWriter := io.Pipe()
	cmd.Stdout = pipeWriter
	cmd.Stderr = pipeWriter

	if err := cmd.Start(); err != nil {
		send(logMsg{pane, kind, "could not start pio: " + err.Error(), true})
		return err
	}

	copyDone := make(chan struct{})
	go func() {
		scanner := bufio.NewScanner(pipeReader)
		scanner.Buffer(make([]byte, 0, 64*1024), 1<<20)
		for scanner.Scan() {
			send(logMsg{pane, kind, strings.TrimRight(scanner.Text(), "\r"), false})
		}
		close(copyDone)
	}()

	err := cmd.Wait()
	pipeWriter.Close()
	<-copyDone
	return err
}

// runBuild compiles the firmware for env, mirroring output to both panes.
func runBuild(root, env string) {
	send(logMsg{"both", "build", "pio run -e " + env, false})
	err := streamPio("both", "build", root, "run", "-e", env)
	send(cmdDoneMsg{"build", err})
}

// runFlash uploads env to each job's port in turn (sequential — avoids two pio
// invocations fighting over the shared build directory), routing each upload's
// output into its device pane.
func runFlash(root, env string, jobs []flashJob) {
	for _, job := range jobs {
		send(logMsg{job.pane, "flash", "upload -e " + env + " → " + job.port, false})
		if err := streamPio(job.pane, "flash", root, "run", "-e", env,
			"-t", "upload", "--upload-port", job.port); err != nil {
			send(cmdDoneMsg{"flash", err})
			return
		}
	}
	send(cmdDoneMsg{"flash", nil})
}
