package main

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// installSiegeHost puts SiegeHost into the game folder as a proxy for the game-local Miles DLL:
//   Mss32.dll (original)  -> mss32_orig.dll   (only the first time)
//   SiegeHost-Mss32.dll   -> Mss32.dll        (our proxy; all Miles exports forward to mss32_orig.dll)
// It also removes an old SiegeHost version.dll, which Windows ignores when it applies compatibility
// shims to DSLOA.exe and which would otherwise double-apply patches when it does load.
func installSiegeHost(gameDir, proxyPath string) (string, error) {
	proxy, err := os.ReadFile(proxyPath)
	if err != nil {
		return "", fmt.Errorf("SiegeHost-Mss32.dll missing next to the launcher/server: %v", err)
	}
	if !bytes.Contains(proxy, []byte("SiegeHost")) {
		return "", errors.New("SiegeHost-Mss32.dll is not a SiegeHost build")
	}
	mss := filepath.Join(gameDir, "Mss32.dll")
	orig := filepath.Join(gameDir, "mss32_orig.dll")
	msg := ""
	cur, err := os.ReadFile(mss)
	if err != nil {
		return "", fmt.Errorf("Mss32.dll not found in the game folder (%v)", err)
	}
	isOurs := bytes.Contains(cur, []byte("SiegeHost"))
	if _, err := os.Stat(orig); errors.Is(err, os.ErrNotExist) {
		if isOurs {
			return "", errors.New("Mss32.dll is SiegeHost but mss32_orig.dll is missing - restore the game's original Mss32.dll")
		}
		if err := os.Rename(mss, orig); err != nil {
			return "", fmt.Errorf("could not rename Mss32.dll (is the game running?): %v", err)
		}
		msg += "original Mss32.dll saved as mss32_orig.dll; "
		isOurs, cur = false, nil
	} else if !isOurs {
		// both exist and Mss32.dll is a real Miles DLL (e.g. game re-installed): keep the newer original
		if err := os.Rename(mss, orig); err != nil {
			return "", fmt.Errorf("could not replace old mss32_orig.dll: %v", err)
		}
		msg += "refreshed mss32_orig.dll; "
		cur = nil
	}
	if sha256.Sum256(cur) != sha256.Sum256(proxy) {
		if err := os.WriteFile(mss, proxy, 0o644); err != nil {
			return "", fmt.Errorf("could not install SiegeHost (is the game running?): %v", err)
		}
		msg += "SiegeHost installed; "
	}
	vd := filepath.Join(gameDir, "version.dll")
	if b, err := os.ReadFile(vd); err == nil && bytes.Contains(b, []byte("SiegeHost")) {
		if os.Remove(vd) == nil {
			msg += "removed old SiegeHost version.dll; "
		}
	}
	if msg == "" {
		msg = "SiegeHost up to date"
	}
	return msg, nil
}
