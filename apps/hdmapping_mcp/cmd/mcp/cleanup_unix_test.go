//go:build darwin || linux

package main

func stopTestCoordinator(pid int) error { return killCoordinator(pid) }
