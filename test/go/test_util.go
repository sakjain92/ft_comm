package main

import (
	"log"
	"math/rand"
	"testing"
	"time"
)

/* Configuration of the test */
type Config struct {
	numHosts int
	numEPs   int
	numMsg   int
}

/* EP information for test purpose */
type TEPInfo struct {
	ni      *NodeInfo
	msgChan [][]chan EPMsg /* One channel per host & sw */
	errChan [][]chan NodeErr
}

/* Host information for test purpose */
type THostInfo struct {
	ni      *NodeInfo
	msgChan chan HostMsg
	errChan [][]chan NodeErr /* One channel per ep & sw */
}

var (
	testEPs   []TEPInfo
	testHosts []THostInfo
)

/* Initializes a test - Useful for all */
func TestInit(t *testing.T, c Config) {

	UtilSetup()

	testHosts = make([]THostInfo, c.numHosts)

	for i := 0; i < c.numHosts; i++ {

		var testHost THostInfo

		testHost.ni = &HostsInfo[i]
		testHost.msgChan = make(chan HostMsg, 1000)
		testHost.errChan = make([][]chan NodeErr, c.numEPs)
		for j := range testHost.errChan {
			testHost.errChan[j] = make([]chan NodeErr, NumSwitches)
			for k := range testHost.errChan[j] {
				testHost.errChan[j][k] = make(chan NodeErr, 1000)
			}
		}

		testHosts[i] = testHost

		HostsInfo[i].ResetNetFilter()
		HostsInfo[i].KillElfs()
	}

	testEPs = make([]TEPInfo, c.numEPs)
	for i := 0; i < c.numEPs; i++ {

		var testEP TEPInfo

		testEP.ni = &EPsInfo[i]

		testEP.msgChan = make([][]chan EPMsg, c.numHosts)
		for j := range testEP.msgChan {
			testEP.msgChan[j] = make([]chan EPMsg, NumSwitches)
			for k := range testEP.msgChan[j] {
				testEP.msgChan[j][k] = make(chan EPMsg, 1000)
			}

		}

		testEP.errChan = make([][]chan NodeErr, c.numHosts)
		for j := range testEP.errChan {
			testEP.errChan[j] = make([]chan NodeErr, NumSwitches)
			for k := range testEP.errChan[j] {
				testEP.errChan[j][k] = make(chan NodeErr, 1000)
			}
		}

		testEPs[i] = testEP

		EPsInfo[i].ResetNetFilter()
		EPsInfo[i].KillElfs()
	}
}

func TestStart(t *testing.T, c Config) {

	for i := 0; i < c.numEPs; i++ {
		go runTestEP(t, c, &testEPs[i])
	}

	time.Sleep(2 * time.Second)

	for i := 0; i < c.numHosts; i++ {
		go runTestHost(t, c, &testHosts[i])
	}
}

/* Resets the state */
func TestDeinit(t *testing.T, c Config) {

	time.Sleep(time.Second)

	for i := 0; i < c.numHosts; i++ {
		HostsInfo[i].ResetNetFilter()
		HostsInfo[i].KillElfs()

	}

	for i := 0; i < c.numEPs; i++ {
		EPsInfo[i].ResetNetFilter()
		EPsInfo[i].KillElfs()
	}

}

/* Run and control a host */
func runTestHost(t *testing.T, c Config, testHost *THostInfo) {

	t.Log("Running host", testHost.ni.Num)
	errChan := make(chan NodeErr, 1000)
	go testHost.ni.RunHost(testHost.msgChan, errChan)

	for {
		ne, more := <-errChan
		if !more {
			for i := range testHost.errChan {
				for j := range testHost.errChan[i] {
					close(testHost.errChan[i][j])
				}
			}

			return
		} else {

			if ne.N.Num >= c.numEPs {
				log.Fatal("Unexpected error received on host")
			}

			testHost.errChan[ne.N.Num][ne.SW] <- ne
		}
	}
}

func runTestEP(t *testing.T, c Config, testEP *TEPInfo) {

	t.Log("Running host", testEP.ni.Num)
	errChan := make(chan NodeErr, 1000)
	msgChan := make(chan EPMsg, 1000)

	go testEP.ni.RunEP(msgChan, errChan)

	for {
		select {
		case ne, more := <-errChan:
			if !more {
				for i := range testEP.errChan {
					for j := range testEP.errChan[i] {
						close(testEP.errChan[i][j])
					}
				}

				return
			} else {

				if ne.N.Num >= c.numHosts {
					log.Fatal("Unexpected error received on ep")
				}

				testEP.errChan[ne.N.Num][ne.SW] <- ne
			}

		case msg, more := <-msgChan:
			if !more {
				for i := range testEP.msgChan {
					for j := range testEP.msgChan[i] {
						close(testEP.msgChan[i][j])
					}
				}

				return
			} else {

				if msg.N.Num >= c.numHosts {
					log.Fatal("Unexpected msg received on ep")
				}

				testEP.msgChan[msg.N.Num][msg.SW] <- msg
			}
		}
	}
}

/* Emulates a fault free host and ep */
func FaultFreeHost(t *testing.T, c Config, testHost *THostInfo) {

	if testHost.ni.Num >= c.numHosts {
		log.Fatal("Unknown host running")
	}

	for i := 0; i < c.numMsg; i++ {
		var msg HostMsg

		msg.Msg = rand.Intn(100)

		testHost.msgChan <- msg

		log.Printf("Host %d, Sending %d", testHost.ni.Num, msg.Msg)

		/* All eps should have received message on all switches */
		for j := 0; j < c.numEPs; j++ {
			for k := 0; k < NumSwitches; k++ {

				timer := time.NewTimer(2 * time.Second)

				select {
				case rmsg := <-testEPs[j].msgChan[testHost.ni.Num][k]:
					if rmsg.Msg != msg.Msg {
						log.Fatal("Received wrong message on EP")
					}
				case <-timer.C:
					log.Fatal("Timeout and didn't receive message")
				}
			}
		}

		log.Printf("Host:%d, Successfully sent msg:%d", testHost.ni.Num, i)
	}

	for i := 0; i < c.numEPs; i++ {
		for j := 0; j < NumSwitches; j++ {
			select {
			case <-testEPs[i].errChan[testHost.ni.Num][j]:
				log.Fatal("Shouldn't receive error")
			default:
			}
		}
	}

	close(testHost.msgChan)
}
