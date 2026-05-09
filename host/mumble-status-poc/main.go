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
	"sort"
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

type user struct {
	Session uint32 `json:"session"`
	Name    string `json:"name"`
}

type status struct {
	Users     int      `json:"users"`
	UserNames []string `json:"user_names"`
}

func main() {
	addr := flag.String("addr", "kismet:64738", "Mumble TCP address")
	name := flag.String("name", "status-display", "temporary client username")
	watch := flag.Bool("watch", false, "keep running and print updates")
	timeout := flag.Duration("timeout", 8*time.Second, "connection timeout")
	flag.Parse()

	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: *timeout}, "tcp", *addr, &tls.Config{InsecureSkipVerify: true})
	if err != nil {
		log.Fatalf("connect: %v", err)
	}
	defer conn.Close()

	if err := writeMessage(conn, msgVersion, encodeVersion()); err != nil {
		log.Fatalf("send version: %v", err)
	}
	if err := writeMessage(conn, msgAuthenticate, encodeAuthenticate(*name)); err != nil {
		log.Fatalf("send authenticate: %v", err)
	}

	users := map[uint32]user{}
	var ownSession uint32
	deadline := time.Now().Add(*timeout)
	synced := false
	var printAfter time.Time

	for {
		if !*watch {
			_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		}

		msgType, payload, err := readMessage(conn)
		if err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				if !*watch && time.Now().After(deadline) {
					printStatus(users, ownSession)
					return
				}
				continue
			}
			if err == io.EOF {
				printStatus(users, ownSession)
				return
			}
			log.Fatalf("read: %v", err)
		}

		switch msgType {
		case msgPing:
			_ = writeMessage(conn, msgPing, payload)
		case msgServerSync:
			ownSession = parseFieldUint32(payload, 1)
			synced = true
			printAfter = time.Now().Add(1 * time.Second)
			if *watch {
				fmt.Fprintf(os.Stderr, "synced own_session=%d\n", ownSession)
				printStatus(users, ownSession)
			}
		case msgUserState:
			session := parseFieldUint32(payload, 1)
			name := parseFieldString(payload, 3)
			if session != 0 && name != "" {
				users[session] = user{Session: session, Name: name}
				if *watch {
					printStatus(users, ownSession)
				}
			}
		case msgUserRemove:
			session := parseFieldUint32(payload, 1)
			delete(users, session)
			if *watch {
				printStatus(users, ownSession)
			}
		}

		if !*watch && synced && !printAfter.IsZero() && time.Now().After(printAfter) {
			printStatus(users, ownSession)
			return
		}
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
	out = appendVarintField(out, 5, 1) // opus=true
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

func printStatus(users map[uint32]user, ownSession uint32) {
	names := make([]string, 0, len(users))
	for session, user := range users {
		if session == ownSession || user.Name == "" {
			continue
		}
		names = append(names, user.Name)
	}
	sort.Strings(names)
	out, _ := json.Marshal(status{Users: len(names), UserNames: names})
	fmt.Println(string(out))
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
