package main

import (
	"fmt"
	"strings"
)

// The native view uses an ordinary guest HID tablet for absolute coordinates.
// Guests that require a relative device retain explicit capture/release instead.
func nativePointerArguments(arguments []string, mode string) ([]string, error) {
	if mode == "" {
		mode = "auto"
	}
	if mode != "auto" && mode != "absolute" && mode != "relative" {
		return nil, fmt.Errorf("native pointer mode must be auto, absolute or relative")
	}
	hasTablet := false
	for index, argument := range arguments {
		if index+1 >= len(arguments) {
			continue
		}
		device := strings.SplitN(arguments[index+1], ",", 2)[0]
		if argument == "-usbdevice" && device == "tablet" {
			hasTablet = true
		}
		if argument != "-device" {
			continue
		}
		switch device {
		case "usb-tablet", "virtio-tablet-pci", "virtio-tablet-device":
			hasTablet = true
		}
		for _, property := range strings.Split(arguments[index+1], ",")[1:] {
			if property == "id=dockpipe-native-pointer-usb" {
				return nil, fmt.Errorf("native pointer controller identity is reserved")
			}
		}
	}
	if mode == "relative" {
		if hasTablet {
			return nil, fmt.Errorf("relative pointer mode conflicts with the supplied absolute tablet device")
		}
		return append([]string(nil), arguments...), nil
	}
	if hasTablet {
		return append([]string(nil), arguments...), nil
	}
	result := append([]string(nil), arguments...)
	return append(result,
		"-device", "qemu-xhci,id=dockpipe-native-pointer-usb",
		"-device", "usb-tablet,bus=dockpipe-native-pointer-usb.0"), nil
}
