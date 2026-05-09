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
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
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
	pidFile     string
}

type mumbleState struct {
	mu         sync.RWMutex
	ownSession uint32
	users      map[uint32]string
}

type mumbleConn struct {
	conn    *tls.Conn
	writeMu sync.Mutex
}

type protoField struct {
	num   int
	wire  int
	value []byte
}

type protoCursor struct {
	data []byte
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
	flag.StringVar(&cfg.pidFile, "pid-file", env("PID_FILE", "/run/waveshare-status.pid"), "pidfile for refresh signalling")
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
	if err := writePIDFile(cfg.pidFile); err != nil {
		log.Printf("write pidfile: %v", err)
	} else {
		defer os.Remove(cfg.pidFile)
	}

	ticker := time.NewTicker(cfg.interval)
	defer ticker.Stop()
	refresh := make(chan os.Signal, 1)
	signal.Notify(refresh, syscall.SIGUSR1)
	defer signal.Stop(refresh)

	log.Printf("starting status sender: port=%q wlan=%s mumble=%s", cfg.port, cfg.wlanIface, cfg.mumbleAddr)
	for {
		if err := sendStatus(cfg, state); err != nil {
			log.Printf("send status: %v", err)
		}

		select {
		case <-ticker.C:
		case <-updates:
		case <-refresh:
		}
	}
}

func writePIDFile(path string) error {
	if path == "" {
		return nil
	}
	return os.WriteFile(path, []byte(fmt.Sprintf("%d\n", os.Getpid())), 0644)
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

func (s *mumbleState) clear() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.ownSession = 0
	s.users = map[uint32]string{}
}

func (s *mumbleState) setOwnSession(session uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.ownSession = session
}

func (s *mumbleState) setUser(session uint32, name string) {
	if session == 0 || name == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.users[session] = name
}

func (s *mumbleState) removeUser(session uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.users, session)
}

func mumbleLoop(cfg config, state *mumbleState, updates chan<- struct{}) {
	for {
		if err := watchMumble(cfg, state, updates); err != nil {
			log.Printf("mumble watch: %v", err)
			state.clear()
			notify(updates)
		}
		time.Sleep(2 * time.Second)
	}
}

func watchMumble(cfg config, state *mumbleState, updates chan<- struct{}) error {
	client, err := connectMumble(cfg)
	if err != nil {
		return err
	}
	defer client.conn.Close()

	done := make(chan struct{})
	defer close(done)
	go client.sendPings(done)

	for {
		msgType, payload, err := readMessage(client.conn)
		if err != nil {
			return err
		}
		if err := client.handleMessage(msgType, payload, state, updates); err != nil {
			return err
		}
	}
}

func connectMumble(cfg config) (*mumbleConn, error) {
	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 5 * time.Second}, "tcp", cfg.mumbleAddr, &tls.Config{InsecureSkipVerify: true})
	if err != nil {
		return nil, err
	}
	client := &mumbleConn{conn: conn}
	if err := client.writeMessage(msgVersion, encodeVersion()); err != nil {
		conn.Close()
		return nil, err
	}
	if err := client.writeMessage(msgAuthenticate, encodeAuthenticate(cfg.mumbleName)); err != nil {
		conn.Close()
		return nil, err
	}
	return client, nil
}

func (c *mumbleConn) handleMessage(msgType uint16, payload []byte, state *mumbleState, updates chan<- struct{}) error {
	switch msgType {
	case msgPing:
		return c.writeMessage(msgPing, payload)
	case msgServerSync:
		state.setOwnSession(parseFieldUint32(payload, 1))
		notify(updates)
	case msgUserState:
		state.setUser(parseFieldUint32(payload, 1), parseFieldString(payload, 3))
		notify(updates)
	case msgUserRemove:
		state.removeUser(parseFieldUint32(payload, 1))
		notify(updates)
	}
	return nil
}

func (c *mumbleConn) sendPings(done <-chan struct{}) {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-done:
			return
		case <-ticker.C:
			if err := c.writeMessage(msgPing, encodePing()); err != nil {
				return
			}
		}
	}
}

func notify(updates chan<- struct{}) {
	select {
	case updates <- struct{}{}:
	default:
	}
}

func (c *mumbleConn) writeMessage(msgType uint16, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return writeMessage(c.conn, msgType, payload)
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

func encodePing() []byte {
	var out []byte
	out = appendVarintField(out, 1, uint64(time.Now().UnixMilli()))
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
	forEachProtoField(payload, func(item protoField) {
		if item.num == field && item.wire == 0 {
			v, _ := readVarint(item.value)
			found = uint32(v)
		}
	})
	return found
}

func parseFieldString(payload []byte, field int) string {
	var found string
	forEachProtoField(payload, func(item protoField) {
		if item.num == field && item.wire == 2 {
			found = string(item.value)
		}
	})
	return found
}

func forEachProtoField(payload []byte, each func(protoField)) {
	cursor := protoCursor{data: payload}
	for {
		field, ok := cursor.next()
		if !ok {
			return
		}
		each(field)
	}
}

func (c *protoCursor) next() (protoField, bool) {
	tag, ok := c.takeVarint()
	if !ok {
		return protoField{}, false
	}
	wire := int(tag & 0x07)
	value, ok := c.takeValue(wire)
	return protoField{num: int(tag >> 3), wire: wire, value: value}, ok
}

func (c *protoCursor) takeValue(wire int) ([]byte, bool) {
	switch wire {
	case 0:
		return c.takeVarintBytes()
	case 1:
		return c.takeFixed(8)
	case 2:
		length, ok := c.takeVarint()
		if !ok || length > uint64(len(c.data)) {
			return nil, false
		}
		return c.takeFixed(int(length))
	case 5:
		return c.takeFixed(4)
	default:
		return nil, false
	}
}

func (c *protoCursor) takeVarint() (uint64, bool) {
	value, size := readVarint(c.data)
	if size <= 0 {
		return 0, false
	}
	c.data = c.data[size:]
	return value, true
}

func (c *protoCursor) takeVarintBytes() ([]byte, bool) {
	_, size := readVarint(c.data)
	if size <= 0 || size > len(c.data) {
		return nil, false
	}
	return c.takeFixed(size)
}

func (c *protoCursor) takeFixed(size int) ([]byte, bool) {
	if size < 0 || len(c.data) < size {
		return nil, false
	}
	value := c.data[:size]
	c.data = c.data[size:]
	return value, true
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
