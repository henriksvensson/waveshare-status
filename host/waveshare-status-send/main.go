package main

import (
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	msgVersion      uint16 = 0
	msgAuthenticate uint16 = 2
	msgPing         uint16 = 3
	msgServerSync   uint16 = 5
	msgUserRemove   uint16 = 8
	msgUserState    uint16 = 9
)

type config struct {
	port        string
	interval    time.Duration
	wlanIface   string
	hostapdConf string
	mumbleAddr  string
	mumbleName  string
}

type mumbleState struct {
	mu         sync.RWMutex
	ownSession uint32
	users      map[uint32]string
}

type displayStatus struct {
	Service   string   `json:"service"`
	Mode      string   `json:"mode"`
	WiFi      string   `json:"wifi"`
	IP        string   `json:"ip"`
	Users     int      `json:"users"`
	UserNames []string `json:"user_names"`
}

func main() {
	cfg := config{}
	flag.StringVar(&cfg.port, "port", env("PORT", ""), "serial port path")
	flag.DurationVar(&cfg.interval, "interval", envDuration("INTERVAL", 2*time.Second), "status send interval")
	flag.StringVar(&cfg.wlanIface, "wlan", env("WLAN_IFACE", "wlan0"), "wireless interface")
	flag.StringVar(&cfg.hostapdConf, "hostapd-conf", env("HOSTAPD_CONF", "/etc/hostapd/kismet-ap.conf"), "hostapd AP config")
	flag.StringVar(&cfg.mumbleAddr, "mumble", env("MUMBLE_ADDR", "127.0.0.1:64738"), "Mumble server address")
	flag.StringVar(&cfg.mumbleName, "mumble-name", env("MUMBLE_NAME", "status-display"), "temporary Mumble client name")
	once := flag.Bool("once", false, "send one update and exit")
	flag.Parse()

	state := &mumbleState{users: map[uint32]string{}}
	updates := make(chan struct{}, 1)
	go mumbleLoop(cfg, state, updates)

	if *once {
		time.Sleep(1200 * time.Millisecond)
		if err := sendStatus(cfg, state); err != nil {
			log.Fatal(err)
		}
		return
	}

	ticker := time.NewTicker(cfg.interval)
	defer ticker.Stop()

	log.Printf("starting status sender: port=%q wlan=%s mumble=%s", cfg.port, cfg.wlanIface, cfg.mumbleAddr)
	for {
		if err := sendStatus(cfg, state); err != nil {
			log.Printf("send status: %v", err)
		}

		select {
		case <-ticker.C:
		case <-updates:
		}
	}
}

func sendStatus(cfg config, state *mumbleState) error {
	port, err := findPort(cfg.port)
	if err != nil {
		return err
	}
	mode := wifiMode()
	status := displayStatus{
		Service:   "MURMUR",
		Mode:      mode,
		WiFi:      wifiName(cfg, mode),
		IP:        wifiIP(cfg, mode),
		Users:     0,
		UserNames: nil,
	}
	status.Users, status.UserNames = state.snapshot()

	line, err := json.Marshal(status)
	if err != nil {
		return err
	}
	line = append(line, '\n')

	_ = exec.Command("stty", "-F", port, "115200", "raw", "-echo").Run()
	file, err := os.OpenFile(port, os.O_WRONLY, 0)
	if err != nil {
		return err
	}
	defer file.Close()
	_, err = file.Write(line)
	return err
}

func findPort(configured string) (string, error) {
	if configured != "" && isCharDevice(configured) {
		return configured, nil
	}
	patterns := []string{
		"/dev/serial/by-id/*Espressif*",
		"/dev/serial/by-id/*JTAG*",
		"/dev/serial/by-id/*serial*",
	}
	for _, pattern := range patterns {
		matches, _ := filepath.Glob(pattern)
		for _, match := range matches {
			resolved, err := filepath.EvalSymlinks(match)
			if err == nil && isCharDevice(resolved) {
				return resolved, nil
			}
		}
	}
	if isCharDevice("/dev/ttyACM0") {
		return "/dev/ttyACM0", nil
	}
	return "", fmt.Errorf("serial port not found")
}

func isCharDevice(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode()&os.ModeCharDevice != 0
}

func wifiMode() string {
	if exec.Command("rc-service", "hostapd", "status").Run() == nil {
		return "ap"
	}
	return "client"
}

func wifiName(cfg config, mode string) string {
	if mode == "ap" {
		if ssid := readHostapdSSID(cfg.hostapdConf); ssid != "" {
			return ssid
		}
		return "Kismet"
	}
	out, _ := exec.Command("iw", "dev", cfg.wlanIface, "link").Output()
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "SSID:") {
			return strings.TrimSpace(strings.TrimPrefix(line, "SSID:"))
		}
	}
	return ""
}

func readHostapdSSID(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "ssid=") {
			return strings.TrimSpace(strings.TrimPrefix(line, "ssid="))
		}
	}
	return ""
}

func wifiIP(cfg config, mode string) string {
	out, _ := exec.Command("ip", "-4", "-o", "addr", "show", "dev", cfg.wlanIface, "scope", "global").Output()
	fields := strings.Fields(string(out))
	for i, field := range fields {
		if field == "inet" && i+1 < len(fields) {
			return strings.SplitN(fields[i+1], "/", 2)[0]
		}
	}
	if mode == "ap" {
		return "192.168.50.1"
	}
	return ""
}

func (s *mumbleState) snapshot() (int, []string) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	names := make([]string, 0, len(s.users))
	for session, name := range s.users {
		if session == s.ownSession || name == "" {
			continue
		}
		names = append(names, name)
	}
	sort.Strings(names)
	return len(names), names
}

func mumbleLoop(cfg config, state *mumbleState, updates chan<- struct{}) {
	for {
		if err := watchMumble(cfg, state, updates); err != nil {
			log.Printf("mumble watch: %v", err)
		}
		time.Sleep(2 * time.Second)
	}
}

func watchMumble(cfg config, state *mumbleState, updates chan<- struct{}) error {
	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 5 * time.Second}, "tcp", cfg.mumbleAddr, &tls.Config{InsecureSkipVerify: true})
	if err != nil {
		return err
	}
	defer conn.Close()

	if err := writeMessage(conn, msgVersion, encodeVersion()); err != nil {
		return err
	}
	if err := writeMessage(conn, msgAuthenticate, encodeAuthenticate(cfg.mumbleName)); err != nil {
		return err
	}

	for {
		msgType, payload, err := readMessage(conn)
		if err != nil {
			return err
		}
		switch msgType {
		case msgPing:
			_ = writeMessage(conn, msgPing, payload)
		case msgServerSync:
			state.mu.Lock()
			state.ownSession = parseFieldUint32(payload, 1)
			state.mu.Unlock()
			notify(updates)
		case msgUserState:
			session := parseFieldUint32(payload, 1)
			name := parseFieldString(payload, 3)
			if session != 0 && name != "" {
				state.mu.Lock()
				state.users[session] = name
				state.mu.Unlock()
				notify(updates)
			}
		case msgUserRemove:
			session := parseFieldUint32(payload, 1)
			state.mu.Lock()
			delete(state.users, session)
			state.mu.Unlock()
			notify(updates)
		}
	}
}

func notify(updates chan<- struct{}) {
	select {
	case updates <- struct{}{}:
	default:
	}
}

func writeMessage(w io.Writer, msgType uint16, payload []byte) error {
	header := make([]byte, 6)
	binary.BigEndian.PutUint16(header[0:2], msgType)
	binary.BigEndian.PutUint32(header[2:6], uint32(len(payload)))
	if _, err := w.Write(header); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

func readMessage(r io.Reader) (uint16, []byte, error) {
	header := make([]byte, 6)
	if _, err := io.ReadFull(r, header); err != nil {
		return 0, nil, err
	}
	msgType := binary.BigEndian.Uint16(header[0:2])
	size := binary.BigEndian.Uint32(header[2:6])
	if size > 1024*1024 {
		return 0, nil, fmt.Errorf("message too large: %d", size)
	}
	payload := make([]byte, size)
	_, err := io.ReadFull(r, payload)
	return msgType, payload, err
}

func encodeVersion() []byte {
	var out []byte
	out = appendVarintField(out, 1, 0x010204)
	out = appendStringField(out, 2, "waveshare-status")
	out = appendStringField(out, 3, "linux")
	return out
}

func encodeAuthenticate(username string) []byte {
	var out []byte
	out = appendStringField(out, 1, username)
	out = appendVarintField(out, 5, 1)
	return out
}

func appendStringField(out []byte, field int, value string) []byte {
	out = appendVarint(out, uint64(field<<3|2))
	out = appendVarint(out, uint64(len(value)))
	return append(out, value...)
}

func appendVarintField(out []byte, field int, value uint64) []byte {
	out = appendVarint(out, uint64(field<<3|0))
	return appendVarint(out, value)
}

func appendVarint(out []byte, value uint64) []byte {
	for value >= 0x80 {
		out = append(out, byte(value)|0x80)
		value >>= 7
	}
	return append(out, byte(value))
}

func parseFieldUint32(payload []byte, field int) uint32 {
	var found uint32
	parseFields(payload, func(num int, wire int, value []byte) {
		if num == field && wire == 0 {
			v, _ := readVarint(value)
			found = uint32(v)
		}
	})
	return found
}

func parseFieldString(payload []byte, field int) string {
	var found string
	parseFields(payload, func(num int, wire int, value []byte) {
		if num == field && wire == 2 {
			found = string(value)
		}
	})
	return found
}

func parseFields(payload []byte, each func(num int, wire int, value []byte)) {
	for len(payload) > 0 {
		tag, n := readVarint(payload)
		if n <= 0 {
			return
		}
		payload = payload[n:]
		num := int(tag >> 3)
		wire := int(tag & 0x07)
		switch wire {
		case 0:
			_, n = readVarint(payload)
			if n <= 0 || n > len(payload) {
				return
			}
			each(num, wire, payload[:n])
			payload = payload[n:]
		case 1:
			if len(payload) < 8 {
				return
			}
			each(num, wire, payload[:8])
			payload = payload[8:]
		case 2:
			length, n := readVarint(payload)
			if n <= 0 {
				return
			}
			payload = payload[n:]
			if length > uint64(len(payload)) {
				return
			}
			each(num, wire, payload[:length])
			payload = payload[length:]
		case 5:
			if len(payload) < 4 {
				return
			}
			each(num, wire, payload[:4])
			payload = payload[4:]
		default:
			return
		}
	}
}

func readVarint(data []byte) (uint64, int) {
	var value uint64
	for i, b := range data {
		if i == 10 {
			return 0, -1
		}
		value |= uint64(b&0x7f) << (7 * i)
		if b < 0x80 {
			return value, i + 1
		}
	}
	return 0, -1
}

func env(key string, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func envDuration(key string, fallback time.Duration) time.Duration {
	value := os.Getenv(key)
	if value == "" {
		return fallback
	}
	duration, err := time.ParseDuration(value)
	if err == nil {
		return duration
	}
	seconds, err := time.ParseDuration(value + "s")
	if err == nil {
		return seconds
	}
	return fallback
}
