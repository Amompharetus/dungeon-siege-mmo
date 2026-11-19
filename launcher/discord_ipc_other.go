//go:build !windows

package main

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"time"
)

type unixPipeConn struct {
	c net.Conn
}

func dialDiscordPipe(idx int, timeout time.Duration) (discordPipe, error) {
	dirs := []string{
		os.Getenv("XDG_RUNTIME_DIR"),
		os.Getenv("TMPDIR"),
		os.Getenv("TMP"),
		os.Getenv("TEMP"),
		"/tmp",
	}
	name := fmt.Sprintf("discord-ipc-%d", idx)
	for _, d := range dirs {
		if d == "" {
			continue
		}
		for _, sub := range []string{"", "app/com.discordapp.Discord", "snap.discord"} {
			p := filepath.Join(d, sub, name)
			c, err := net.DialTimeout("unix", p, timeout)
			if err == nil {
				return &unixPipeConn{c: c}, nil
			}
		}
	}
	return nil, errors.New("discord ipc socket unavailable")
}

func (u *unixPipeConn) WriteAll(b []byte, timeout time.Duration) error {
	_ = u.c.SetWriteDeadline(time.Now().Add(timeout))
	for len(b) > 0 {
		n, err := u.c.Write(b)
		if err != nil {
			return err
		}
		b = b[n:]
	}
	return nil
}

func (u *unixPipeConn) ReadFull(b []byte, timeout time.Duration) error {
	_ = u.c.SetReadDeadline(time.Now().Add(timeout))
	_, err := io.ReadFull(u.c, b)
	return err
}

func (u *unixPipeConn) Close() error {
	if u.c != nil {
		return u.c.Close()
	}
	return nil
}
