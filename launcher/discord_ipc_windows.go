//go:build windows

package main

import (
	"errors"
	"fmt"
	"io"
	"syscall"
	"time"
	"unsafe"
)

var (
	modKernel32          = syscall.NewLazyDLL("kernel32.dll")
	procCreateFileW      = modKernel32.NewProc("CreateFileW")
	procCreateEventW     = modKernel32.NewProc("CreateEventW")
	procWaitSingleObject = modKernel32.NewProc("WaitForSingleObject")
	procGetOverlapped    = modKernel32.NewProc("GetOverlappedResult")
	procCancelIo         = modKernel32.NewProc("CancelIo")
	procReadFile         = modKernel32.NewProc("ReadFile")
	procWriteFile        = modKernel32.NewProc("WriteFile")
	procWaitNamedPipeW   = modKernel32.NewProc("WaitNamedPipeW")
)

const (
	winFileFlagOverlapped = 0x40000000
	winWaitObject0        = 0x00000000
	winWaitTimeout        = 0x00000102
	winErrorPipeBusy      = syscall.Errno(231)
	winErrorIOPending     = syscall.Errno(997)
)

type winPipeConn struct {
	h syscall.Handle
}

func dialDiscordPipe(idx int, timeout time.Duration) (discordPipe, error) {
	path := fmt.Sprintf(`\\.\pipe\discord-ipc-%d`, idx)
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	open := func() (syscall.Handle, error) {
		h, _, callErr := procCreateFileW.Call(
			uintptr(unsafe.Pointer(p)),
			uintptr(syscall.GENERIC_READ|syscall.GENERIC_WRITE),
			0,
			0,
			uintptr(syscall.OPEN_EXISTING),
			uintptr(winFileFlagOverlapped),
			0,
		)
		if syscall.Handle(h) == syscall.InvalidHandle {
			if callErr != nil && callErr != syscall.Errno(0) {
				return syscall.InvalidHandle, callErr
			}
			return syscall.InvalidHandle, errors.New("open pipe failed")
		}
		return syscall.Handle(h), nil
	}
	h, err := open()
	if err == winErrorPipeBusy {
		ms := uint32(timeout / time.Millisecond)
		if ms < 50 {
			ms = 50
		}
		if r, _, _ := procWaitNamedPipeW.Call(uintptr(unsafe.Pointer(p)), uintptr(ms)); r != 0 {
			h, err = open()
		}
	}
	if err != nil {
		return nil, err
	}
	return &winPipeConn{h: h}, nil
}

func (c *winPipeConn) ioWithTimeout(isWrite bool, buf []byte, timeout time.Duration) (int, error) {
	if len(buf) == 0 {
		return 0, nil
	}
	ev, _, err := procCreateEventW.Call(0, 1, 0, 0)
	if ev == 0 {
		return 0, err
	}
	defer syscall.CloseHandle(syscall.Handle(ev))

	var ov syscall.Overlapped
	ov.HEvent = syscall.Handle(ev)

	var done uint32
	var r uintptr
	var callErr error
	if isWrite {
		r, _, callErr = procWriteFile.Call(
			uintptr(c.h),
			uintptr(unsafe.Pointer(&buf[0])),
			uintptr(len(buf)),
			uintptr(unsafe.Pointer(&done)),
			uintptr(unsafe.Pointer(&ov)),
		)
	} else {
		r, _, callErr = procReadFile.Call(
			uintptr(c.h),
			uintptr(unsafe.Pointer(&buf[0])),
			uintptr(len(buf)),
			uintptr(unsafe.Pointer(&done)),
			uintptr(unsafe.Pointer(&ov)),
		)
	}
	if r == 0 {
		if callErr != winErrorIOPending {
			return 0, callErr
		}
		ms := uint32(timeout / time.Millisecond)
		if ms == 0 {
			ms = 1000
		}
		w, _, wErr := procWaitSingleObject.Call(ev, uintptr(ms))
		if w == winWaitTimeout {
			_, _, _ = procCancelIo.Call(uintptr(c.h))
			return 0, errors.New("discord ipc timed out")
		}
		if w != winWaitObject0 {
			_, _, _ = procCancelIo.Call(uintptr(c.h))
			return 0, wErr
		}
		gr, _, gErr := procGetOverlapped.Call(uintptr(c.h), uintptr(unsafe.Pointer(&ov)), uintptr(unsafe.Pointer(&done)), 0)
		if gr == 0 {
			return 0, gErr
		}
	}
	if !isWrite && done == 0 {
		return 0, io.EOF
	}
	return int(done), nil
}

func (c *winPipeConn) WriteAll(b []byte, timeout time.Duration) error {
	for len(b) > 0 {
		n, err := c.ioWithTimeout(true, b, timeout)
		if err != nil {
			return err
		}
		b = b[n:]
	}
	return nil
}

func (c *winPipeConn) ReadFull(b []byte, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	off := 0
	for off < len(b) {
		rem := time.Until(deadline)
		if rem <= 0 {
			return errors.New("discord ipc read timed out")
		}
		n, err := c.ioWithTimeout(false, b[off:], rem)
		if err != nil {
			return err
		}
		off += n
	}
	return nil
}

func (c *winPipeConn) Close() error {
	if c.h != 0 && c.h != syscall.InvalidHandle {
		err := syscall.CloseHandle(c.h)
		c.h = syscall.InvalidHandle
		return err
	}
	return nil
}
