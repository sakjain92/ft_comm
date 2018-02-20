package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"log"
	"os/exec"
	"regexp"
	"strconv"
)

/* Global defines */
var (
	NumHosts    = 4
	NumEPs      = 3
	NumSwitches = 2
)

/* Information about the management part of the node */
type sshInfo struct {
	user string
	ip   string
	port string /* Port forwarding */
}

/* Information about the IPs/Ports uses by hosts/eps to communicate */
type commInfo struct {
	ips  []string
	port string
}

/* All information about a node */
type NodeInfo struct {
	IsHost bool
	Num    int
	ci     commInfo
	si     sshInfo
}

/* Message to send */
type HostMsg struct {
	Msg int /* Message to send */
}

/* Message received */
type EPMsg struct {
	N       *NodeInfo
	SW      int
	Session int
	MsgNum  int
	Msg     int
}

/* Errors detected by Node */
type NodeErr struct {
	Err int
	N   *NodeInfo
	SW  int
}

/* Used as a handle to delete filter entry previous created */
type FilterEntry struct {
	cmdS []string
}

var (
	hostsSSHInfo = []sshInfo{
		sshInfo{user: "root", ip: "localhost", port: "14501"},
		sshInfo{user: "root", ip: "localhost", port: "14502"},
		sshInfo{user: "root", ip: "localhost", port: "14503"},
		sshInfo{user: "root", ip: "localhost", port: "14504"},
	}

	epsSSHInfo = []sshInfo{
		sshInfo{user: "root", ip: "localhost", port: "14601"},
		sshInfo{user: "root", ip: "localhost", port: "14602"},
		sshInfo{user: "root", ip: "localhost", port: "14603"},
	}

	commPort = "14700"

	hostsCommInfo = []commInfo{
		commInfo{ips: []string{"192.168.1.1", "192.168.2.1"}, port: commPort},
		commInfo{ips: []string{"192.168.1.2", "192.168.2.2"}, port: commPort},
		commInfo{ips: []string{"192.168.1.3", "192.168.2.3"}, port: commPort},
		commInfo{ips: []string{"192.168.1.4", "192.168.2.4"}, port: commPort},
	}

	epsCommInfo = []commInfo{
		commInfo{ips: []string{"192.168.1.11", "192.168.2.11"}, port: commPort},
		commInfo{ips: []string{"192.168.1.12", "192.168.2.12"}, port: commPort},
		commInfo{ips: []string{"192.168.1.13", "192.168.2.13"}, port: commPort},
	}

	HostsInfo []NodeInfo
	EPsInfo   []NodeInfo

	FILTER_INCOMING_REJECT = 1
	FILTER_INCOMING_DROP   = 2
	FILTER_OUTGOING_REJECT = 3
	FILTER_OUTGOING_DROP   = 4

	HOST_CONNECT_FAIL      = 1
	HOST_CONNECT_TERMINATE = 2
	EP_CONNECT_TERMINATE   = 4
	EP_HEARTBEAT_FAIL      = 5
	EP_INVALID_MSG         = 6

	codeDir = "/home/saksham/ft_comm/"
	hostElf = codeDir + "host.elf"
	epElf   = codeDir + "ep.elf"
)

/* Intialize and check variables for consistency */
func UtilSetup() {

	log.SetFlags(log.LstdFlags | log.Lshortfile)

	if len(hostsSSHInfo) != NumHosts || len(hostsCommInfo) != NumHosts ||
		len(epsSSHInfo) != NumEPs || len(epsCommInfo) != NumEPs {
		panic("Invalid Configuration")
	}

	for i := 0; i < NumHosts; i++ {

		ni := NodeInfo{IsHost: true, Num: i, ci: hostsCommInfo[i], si: hostsSSHInfo[i]}

		if len(hostsCommInfo[i].ips) != NumSwitches {
			panic("Invalid Configuration")
		}

		HostsInfo = append(HostsInfo, ni)
	}

	for i := 0; i < NumEPs; i++ {

		ni := NodeInfo{IsHost: false, Num: i, ci: epsCommInfo[i], si: epsSSHInfo[i]}

		if len(epsCommInfo[i].ips) != NumSwitches {
			panic("Invalid Configuration")
		}

		EPsInfo = append(EPsInfo, ni)
	}
}

/* Returns a modified command that will run the supplied command remotely */
func (n *NodeInfo) command(cmd ...string) *exec.Cmd {

	s := n.si

	arg := append(
		[]string{
			"-tt",
			fmt.Sprintf("%s@%s", s.user, s.ip),
			fmt.Sprintf("-p"),
			fmt.Sprintf("%s", s.port),
		},
		cmd...,
	)

	fmt.Println(arg)

	return exec.Command("ssh", arg...)
}

/* Resets the filter table for the node */
func (n *NodeInfo) ResetNetFilter() {
	cmdS := []string{
		"iptables",
		"-F",
	}

	cmd := n.command(cmdS...)

	err := cmd.Run()
	if err != nil {
		log.Fatal(err)
	}
}

/* Resets the filter table for the node */
func (n *NodeInfo) KillElfs() {

	killHostElf := []string{
		"killall",
		hostElf,
	}

	killEPElf := []string{
		"killall",
		epElf,
	}

	cmd := n.command(killHostElf...)

	cmd.Run()

	cmd = n.command(killEPElf...)

	cmd.Run()
}

/* Deletes a filter entry previous created */
func (n *NodeInfo) DeleteFilterEntry(fe FilterEntry) {
	cmdS := []string{
		"iptables",
		"-D",
	}

	cmdS = append(cmdS, fe.cmdS...)

	cmd := n.command(cmdS...)

	err := cmd.Run()
	if err != nil {
		log.Fatal(err)
	}
}

/* Drops incoming packets */
func (n *NodeInfo) FilterNodesPackets(otherNode *NodeInfo, sw int, filterType int) FilterEntry {

	var fe FilterEntry

	if filterType != FILTER_INCOMING_DROP && filterType != FILTER_INCOMING_REJECT &&
		filterType != FILTER_OUTGOING_DROP && filterType != FILTER_OUTGOING_REJECT {
		panic("Invalid filter type")
	}

	if sw < 0 || sw >= len(otherNode.ci.ips) {
		panic("Invalid input")
	}

	cmdS := []string{
		"iptables",
		"-A",
	}

	if filterType == FILTER_INCOMING_DROP || filterType == FILTER_INCOMING_REJECT {
		fe.cmdS = []string{
			"INPUT",
			"-p",
			"tcp",
			"--dport",
			n.ci.port,
			"-s",
			otherNode.ci.ips[sw],
			"-j",
		}
	} else {
		fe.cmdS = []string{
			"OUTPUT",
			"-p",
			"tcp",
			"--dport",
			otherNode.ci.port,
			"-s",
			otherNode.ci.ips[sw],
			"-j",
		}
	}

	if filterType == FILTER_INCOMING_DROP || filterType == FILTER_OUTGOING_DROP {
		fe.cmdS = append(fe.cmdS, "DROP")
	} else {
		fe.cmdS = append(fe.cmdS, "REJECT")
	}

	cmdS = append(cmdS, fe.cmdS...)

	cmd := n.command(cmdS...)

	err := cmd.Run()
	if err != nil {
		log.Fatal(err)
	}

	return fe
}

func (n *NodeInfo) RunHost(msgChan <-chan HostMsg, errChan chan<- NodeErr) {

	if !n.IsHost {
		panic("Can't run EP when asked to run Host")
	}

	cmdS := []string{
		hostElf,
		"-i",
	}

	cmd := n.command(cmdS...)

	stdin, err := cmd.StdinPipe()
	if err != nil {
		log.Fatal(err)
	}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		log.Fatal(err)
	}

	err = cmd.Start()
	if err != nil {
		log.Fatal(err)
	}

	/* XXX: Do we monitor stderr also? */

	go func() {
		for {
			m, more := <-msgChan
			if !more {
				stdin.Close()
				return
			}

			s := strconv.Itoa(m.Msg) + "\n"

			_, err = io.Copy(stdin, bytes.NewBufferString(s))
			if err != nil {
				log.Fatal(err)
			}
		}
	}()

	go func() {
		var e error

		r := bufio.NewReader(stdout)

		e = nil
		for e == nil {

			s, e := r.ReadString('\n')
			if e == nil {

				var ne NodeErr

				re := regexp.MustCompile("ERROR_CALLBACK\\((.*?)\\): EP\\((.*?):(.*?)\\)")
				match := re.FindStringSubmatch(s)
				if match == nil {
					/* Some debugging messages */
					continue
				}

				reason, err := strconv.Atoi(match[1])
				if err != nil {
					log.Fatal(err)
				}

				if reason != HOST_CONNECT_FAIL &&
					reason != HOST_CONNECT_TERMINATE {
					panic("Invalid output")
				}

				num, err := strconv.Atoi(match[2])
				if err != nil {
					log.Fatal(err)
				}

				if num < 0 || num >= NumEPs {
					panic("Invalid output")
				}

				sw, err := strconv.Atoi(match[3])
				if err != nil {
					log.Fatal(err)
				}

				if sw < 0 || sw >= NumSwitches {
					panic("Invalid output")
				}

				ne.Err = reason
				ne.SW = sw
				ne.N = &EPsInfo[num]
				errChan <- ne
			} else {
				close(errChan)
				return
			}
		}
	}()

	cmd.Wait()
	/* FIXME: Why error here?
	err = cmd.Wait()
	if err != nil {
		log.Fatal("Host", n.Num, err)
	}
	*/
}

func (n *NodeInfo) RunEP(msgChan chan<- EPMsg, errChan chan<- NodeErr) {

	if n.IsHost {
		panic("Can't run Host when asked to run EP")
	}

	cmdS := []string{
		epElf,
	}

	cmd := n.command(cmdS...)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		log.Fatal(err)
	}

	err = cmd.Start()
	if err != nil {
		log.Fatal(err)
	}

	/* XXX: Do we monitor stderr also? */

	go func() {
		var e error

		r := bufio.NewReader(stdout)
		e = nil
		for e == nil {

			s, e := r.ReadString('\n')
			if e == nil {

				var ne NodeErr

				re := regexp.MustCompile("ERROR_CALLBACK\\((.*?)\\): HOST\\((.*?):(.*?)\\)")
				match := re.FindStringSubmatch(s)
				if match == nil {

					var em EPMsg

					/* We got message */
					re = regexp.MustCompile("Host\\((.*?):(.*?)\\): Session\\((.*?)\\): MsgNum\\((.*?)\\): Msg\\((.*?)\\)")
					match = re.FindStringSubmatch(s)
					if match == nil {
						/* Some error messages */
						continue
					}

					num, err := strconv.Atoi(match[1])
					if err != nil {
						log.Fatal(err)
					}

					if num < 0 || num >= NumHosts {
						panic("Invalid output")
					}

					sw, err := strconv.Atoi(match[2])
					if err != nil {
						log.Fatal(err)
					}

					if sw < 0 || sw >= NumSwitches {
						panic("Invalid output")
					}

					session, err := strconv.Atoi(match[3])
					if err != nil {
						log.Fatal(err)
					}

					msgnum, err := strconv.Atoi(match[4])
					if err != nil {
						log.Fatal(err)
					}

					msg, err := strconv.Atoi(match[5])
					if err != nil {
						log.Fatal(err)
					}

					em.N = &HostsInfo[num]
					em.SW = sw
					em.Session = session
					em.MsgNum = msgnum
					em.Msg = msg
					msgChan <- em

				} else {

					reason, err := strconv.Atoi(match[1])
					if err != nil {
						log.Fatal(err)
					}

					if reason != EP_INVALID_MSG &&
						reason != EP_HEARTBEAT_FAIL &&
						reason != EP_CONNECT_TERMINATE {
						panic("Invalid output")
					}

					num, err := strconv.Atoi(match[2])
					if err != nil {
						log.Fatal(err)
					}

					if num < 0 || num >= NumEPs {
						panic("Invalid output")
					}

					sw, err := strconv.Atoi(match[3])
					if err != nil {
						log.Fatal(err)
					}

					if sw < 0 || sw >= NumSwitches {
						panic("Invalid output")
					}

					ne.Err = reason
					ne.SW = sw
					ne.N = &HostsInfo[num]
					errChan <- ne
				}
			} else {
				close(errChan)
				close(msgChan)
				return
			}
		}
	}()

	cmd.Wait()
}
