package main

import (
	"log"
	"testing"
)

func TestFaultFree(t *testing.T) {
	var c Config

	log.Println("Running TestFaultFree")

	c.numEPs = NumEPs - 1
	c.numHosts = NumHosts
	c.numMsg = 2

	TestInit(t, c)

	TestStart(t, c)

	done := make([]chan bool, c.numHosts)

	for i := range done {
		done[i] = make(chan bool)
	}

	for i := 0; i < c.numHosts; i++ {
		go func(i int) {
			FaultFreeHost(t, c, &testHosts[i])
			done[i] <- true
		}(i)
	}

	for i := 0; i < c.numHosts; i++ {
		<-done[i]
	}

	log.Println("Ending TestFaultFree")

	TestDeinit(t, c)
}
