// mock9p - host mock of the esp8266-9p namespace (DESIGN section 5).
//
// Purpose: freeze the file/verb design with real 9P clients before
// firmware work (DESIGN phase 1). It serves the same namespace the
// firmware will, over TCP, with mock state. 9P2000, stdlib only.
//
// Run: go run . [-listen :1564]
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	tVersion = 100
	rVersion = 101
	tAuth    = 102
	rAuth    = 103
	tAttach  = 104
	rAttach  = 105
	rError   = 107
	tFlush   = 108
	rFlush   = 109
	tWalk    = 110
	rWalk    = 111
	tOpen    = 112
	rOpen    = 113
	tStat    = 124	// canonical 9P2000 (was 114 = Tcreate, B30)
	rStat    = 125
	tRead    = 116
	rRead    = 117
	tWrite   = 118
	rWrite   = 119
	tClunk   = 120
	rClunk   = 121
	tRemove  = 122
	tWstat   = 124
	nofid    = 0xFFFFFFFF
)

var boot = time.Now()

type state struct {
	mu       sync.Mutex
	mode     string
	lastCtl  string
	lastWifi string
	scan     string
}

var st = &state{mode: "softap", scan: ""}

type node struct {
	name string
	dir  bool
	qid  uint64
	ctl  bool
	get  func() string
}

var nodes = map[string]*node{}

func reg(path string, dir, ctl bool, qid uint64, get func() string) {
	name := path[strings.LastIndex(path, "/")+1:]
	if path == "/" {
		name = "/"
	}
	nodes[path] = &node{name: name, dir: dir, qid: qid, ctl: ctl, get: get}
}

func init() {
	reg("/", true, false, 1, nil)
	reg("/ctl", false, true, 2, statusText)
	reg("/status", false, false, 3, statusText)
	reg("/version", false, false, 4, func() string {
		return "esp9-mock 0.1\nchip ESP8266EX (mock)\nmac 48:3f:da:7e:39:68\nsdk 2.0.0 (mock)\n"
	})
	reg("/wifi", true, false, 5, nil)
	reg("/wifi/ctl", false, true, 6, wifiStatus)
	reg("/wifi/status", false, false, 7, wifiStatus)
	reg("/wifi/scan", false, false, 8, func() string {
		st.mu.Lock()
		defer st.mu.Unlock()
		if st.scan == "" {
			return "# no scan yet; write 'scan' to /wifi/ctl\n"
		}
		return st.scan
	})
	reg("/net", true, false, 9, nil)
	reg("/net/status", false, false, 10, func() string {
		return "ip 192.168.4.1\nnetmask 255.255.255.0\ngateway 192.168.4.1\ndns 192.168.4.1\nmac 48:3f:da:7e:39:68\n"
	})
	reg("/console", false, true, 11, func() string {
		return "esp9 console (mock)\n"
	})
}

func statusText() string {
	st.mu.Lock()
	defer st.mu.Unlock()
	return fmt.Sprintf("mode %s\nssid ESP9-3968\nip 192.168.4.1\nrssi 0\nuptime %d\nheap 52328\nclients 0\nlastctl %s\n",
		st.mode, int(time.Since(boot).Seconds()), st.lastCtl)
}

func wifiStatus() string {
	st.mu.Lock()
	defer st.mu.Unlock()
	return fmt.Sprintf("state softap\nssid ESP9-3968\nip 192.168.4.1\nrssi 0\nchannel 1\nlastwifi %s\n", st.lastWifi)
}

func children(path string) []string {
	var out []string
	for p := range nodes {
		if p == path {
			continue
		}
		parent := p[:strings.LastIndex(p, "/")]
		if parent == "" {
			parent = "/"
		}
		if parent == path {
			out = append(out, nodes[p].name)
		}
	}
	sort.Strings(out)
	return out
}

func dirBytes(path string) []byte {
	var b strings.Builder
	for _, name := range children(path) {
		child := join(path, name)
		b.Write(statBytes(nodes[child], uint32(len(content(child)))))
	}
	return []byte(b.String())
}

func join(dir, name string) string {
	if dir == "/" {
		return "/" + name
	}
	return dir + "/" + name
}

func content(path string) string {
	n := nodes[path]
	if n == nil {
		return ""
	}
	if n.dir {
		return string(dirBytes(path))
	}
	if n.get != nil {
		return n.get()
	}
	return ""
}

func qidBytes(n *node) []byte {
	// Canonical 9P2000 qid wire order: type[1], vers[4], path[8].
	// Was vers,type until B32 (shared srv9p's wrong order, so the
	// self-pair agreed); a real kernel mount checks QTDIR.
	b := make([]byte, 13)
	if n.dir {
		b[0] = 0x80
	}
	binary.LittleEndian.PutUint32(b[1:5], 0)
	binary.LittleEndian.PutUint64(b[5:13], n.qid)
	return b
}

func statBytes(n *node, length uint32) []byte {
	name, uid, gid, muid := n.name, "esp9", "esp9", "esp9"
	size := 2 + 4 + 13 + 4 + 4 + 4 + 8 + 2 + len(name) + 2 + len(uid) + 2 + len(gid) + 2 + len(muid)
	b := make([]byte, 0, size+2)
	put16 := func(v uint16) { b = append(b, byte(v), byte(v>>8)) }
	put32 := func(v uint32) { var t [4]byte; binary.LittleEndian.PutUint32(t[:], v); b = append(b, t[:]...) }
	put64 := func(v uint64) { var t [8]byte; binary.LittleEndian.PutUint64(t[:], v); b = append(b, t[:]...) }
	putS := func(s string) { put16(uint16(len(s))); b = append(b, s...) }
	put16(uint16(size))
	put16(0) // type
	put32(0) // dev
	b = append(b, qidBytes(n)...)
	mode := uint32(0444)
	if n.dir {
		mode = 0x80000000 | 0755
	} else if n.ctl {
		mode = 0666
	}
	put32(mode)
	now := uint32(time.Now().Unix())
	put32(now)
	put32(now)
	if n.dir {
		put64(0)
	} else {
		put64(uint64(length))
	}
	putS(name)
	putS(uid)
	putS(gid)
	putS(muid)
	return b
}

func pathOf(n *node) string {
	for p, x := range nodes {
		if x == n {
			return p
		}
	}
	return "/"
}

type fid struct {
	path string
	open bool
}

type conn struct {
	c     net.Conn
	fids  map[uint32]*fid
	msize uint32
}

func (cn *conn) rerr(tag uint16, msg string) {
	var b strings.Builder
	_ = b
	payload := append([]byte{rError}, byte(tag), byte(tag>>8))
	payload = append(payload, byte(len(msg)), byte(len(msg)>>8))
	payload = append(payload, msg...)
	send(cn.c, payload)
}

func send(w io.Writer, payload []byte) {
	var hdr [4]byte
	binary.LittleEndian.PutUint32(hdr[:], uint32(len(payload)+4))
	w.Write(hdr[:])
	w.Write(payload)
}

func readMsg(r io.Reader) (byte, uint16, []byte, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, 0, nil, err
	}
	size := binary.LittleEndian.Uint32(hdr[:])
	if size < 7 || size > 1<<20 {
		return 0, 0, nil, fmt.Errorf("bad size %d", size)
	}
	buf := make([]byte, size-4)
	if _, err := io.ReadFull(r, buf); err != nil {
		return 0, 0, nil, err
	}
	return buf[0], binary.LittleEndian.Uint16(buf[1:3]), buf[3:], nil
}

type reader struct {
	b []byte
	i int
}

func (r *reader) u8() int     { v := r.b[r.i]; r.i++; return int(v) }
func (r *reader) u16() int    { v := binary.LittleEndian.Uint16(r.b[r.i:]); r.i += 2; return int(v) }
func (r *reader) u32() uint32 { v := binary.LittleEndian.Uint32(r.b[r.i:]); r.i += 4; return v }
func (r *reader) u64() uint64 { v := binary.LittleEndian.Uint64(r.b[r.i:]); r.i += 8; return v }
func (r *reader) str() string { n := r.u16(); s := string(r.b[r.i : r.i+n]); r.i += n; return s }

func applyCtl(path, s string) string {
	st.mu.Lock()
	defer st.mu.Unlock()
	fields := strings.Fields(s)
	if len(fields) == 0 {
		return "empty ctl write"
	}
	if path == "/wifi/ctl" {
		switch fields[0] {
		case "connect":
			if len(fields) != 3 {
				return "usage: connect <ssid> <key>"
			}
			st.lastWifi = "connect " + fields[1]
			return ""
		case "disconnect", "scan":
			st.lastWifi = fields[0]
			if fields[0] == "scan" {
				st.scan = "# mock scan\nExampleAP\t6\t-61\twpa2\n"
			}
			return ""
		}
		return "bad wifi ctl verb"
	}
	switch fields[0] {
	case "reboot", "factoryreset":
		st.lastCtl = fields[0]
		return ""
	case "ap":
		if len(fields) == 2 && (fields[1] == "on" || fields[1] == "off") {
			st.lastCtl = s
			return ""
		}
		return "usage: ap on|off"
	}
	return "bad ctl verb"
}

func (cn *conn) handle(typ byte, tag uint16, body []byte) {
	r := &reader{b: body}
	reply := func(t byte, rest ...byte) { send(cn.c, append([]byte{t, byte(tag), byte(tag >> 8)}, rest...)) }
	_ = reply
	msg := func(t byte, build func(*strings.Builder)) {}
	_ = msg
	switch typ {
	case tVersion:
		msize := r.u32()
		ver := r.str()
		if ver != "9P2000" {
			cn.rerr(tag, "unknown version")
			return
		}
		if msize < cn.msize {
			cn.msize = msize
		}
		p := []byte{rVersion, byte(tag), byte(tag >> 8)}
		var t [4]byte
		binary.LittleEndian.PutUint32(t[:], cn.msize)
		p = append(p, t[:]...)
		p = append(p, 8, 0)
		p = append(p, "9P2000"...)
		send(cn.c, p)
	case tAuth:
		cn.rerr(tag, "no authentication required")
	case tAttach:
		f := r.u32()
		_ = r.u32()
		_ = r.str()
		_ = r.str()
		cn.fids[f] = &fid{path: "/"}
		p := []byte{rAttach, byte(tag), byte(tag >> 8)}
		p = append(p, qidBytes(nodes["/"])...)
		send(cn.c, p)
	case tWalk:
		f := r.u32()
		nf := r.u32()
		nw := r.u16()
		src, ok := cn.fids[f]
		if !ok {
			cn.rerr(tag, "unknown fid")
			return
		}
		path := src.path
		var qids []byte
		walked := 0
		for i := 0; i < nw; i++ {
			name := r.str()
			next := join(path, name)
			if name == ".." {
				next = path[:strings.LastIndex(path, "/")]
				if next == "" {
					next = "/"
				}
			}
			n, exists := nodes[next]
			if !exists {
				break
			}
			path = next
			qids = append(qids, qidBytes(n)...)
			walked++
		}
		if nw == 0 {
			cn.fids[nf] = &fid{path: src.path}
		} else if walked == nw {
			cn.fids[nf] = &fid{path: path}
		} else if walked == 0 {
			cn.rerr(tag, "file does not exist")
			return
		}
		p := []byte{rWalk, byte(tag), byte(tag >> 8), byte(walked), byte(walked >> 8)}
		p = append(p, qids...)
		send(cn.c, p)
	case tOpen:
		f := r.u32()
		_ = r.u8()
		fd, ok := cn.fids[f]
		if !ok {
			cn.rerr(tag, "unknown fid")
			return
		}
		fd.open = true
		p := []byte{rOpen, byte(tag), byte(tag >> 8)}
		p = append(p, qidBytes(nodes[fd.path])...)
		p = append(p, 0, 0, 0, 0)
		send(cn.c, p)
	case tRead:
		f := r.u32()
		off := r.u64()
		count := r.u32()
		fd, ok := cn.fids[f]
		if !ok {
			cn.rerr(tag, "unknown fid")
			return
		}
		data := []byte(content(fd.path))
		var chunk []byte
		if off < uint64(len(data)) {
			end := int(off) + int(count)
			if end > len(data) {
				end = len(data)
			}
			chunk = data[off:end]
		}
		p := []byte{rRead, byte(tag), byte(tag >> 8)}
		var t [4]byte
		binary.LittleEndian.PutUint32(t[:], uint32(len(chunk)))
		p = append(p, t[:]...)
		p = append(p, chunk...)
		send(cn.c, p)
	case tWrite:
		f := r.u32()
		_ = r.u64()
		count := r.u32()
		data := string(r.b[r.i : r.i+int(count)])
		fd, ok := cn.fids[f]
		if !ok {
			cn.rerr(tag, "unknown fid")
			return
		}
		if !nodes[fd.path].ctl {
			cn.rerr(tag, "file is read-only")
			return
		}
		if errText := applyCtl(fd.path, strings.TrimSpace(data)); errText != "" {
			cn.rerr(tag, errText)
			return
		}
		p := []byte{rWrite, byte(tag), byte(tag >> 8)}
		var t [4]byte
		binary.LittleEndian.PutUint32(t[:], count)
		p = append(p, t[:]...)
		send(cn.c, p)
	case tStat:
		f := r.u32()
		fd, ok := cn.fids[f]
		if !ok {
			cn.rerr(tag, "unknown fid")
			return
		}
		sb := statBytes(nodes[fd.path], uint32(len(content(fd.path))))
		p := []byte{rStat, byte(tag), byte(tag >> 8), byte(len(sb)), byte(len(sb) >> 8)}
		p = append(p, sb...)
		send(cn.c, p)
	case tClunk:
		f := r.u32()
		delete(cn.fids, f)
		send(cn.c, []byte{rClunk, byte(tag), byte(tag >> 8)})
	case tFlush:
		send(cn.c, []byte{rFlush, byte(tag), byte(tag >> 8)})
	default:
		cn.rerr(tag, "not implemented")
	}
}

func serve(c net.Conn) {
	defer c.Close()
	cn := &conn{c: c, fids: map[uint32]*fid{}, msize: 2048}
	for {
		typ, tag, body, err := readMsg(c)
		if err != nil {
			return
		}
		cn.handle(typ, tag, body)
	}
}

func main() {
	addr := flag.String("listen", ":1564", "listen address")
	flag.Parse()
	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("mock9p serving DESIGN namespace on %s", *addr)
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		go serve(c)
	}
}
