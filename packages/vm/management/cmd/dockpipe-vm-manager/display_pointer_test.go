package main

import (
	"reflect"
	"testing"
)

func TestNativePointerAddsTabletWithoutChangingCallerArguments(t *testing.T) {
	original := []string{"-machine", "pc", "-display", "dbus"}
	before := append([]string(nil), original...)
	for _, mode := range []string{"", "auto", "absolute"} {
		result, err := nativePointerArguments(original, mode)
		if err != nil {
			t.Fatal(err)
		}
		expected := append(append([]string(nil), before...), "-device", "qemu-xhci,id=dockpipe-native-pointer-usb", "-device", "usb-tablet,bus=dockpipe-native-pointer-usb.0")
		if !reflect.DeepEqual(result, expected) || !reflect.DeepEqual(original, before) {
			t.Fatalf("incorrect tablet arguments for %q", mode)
		}
	}
}

func TestNativePointerPreservesExistingTabletAndExplicitRelativeMode(t *testing.T) {
	for _, device := range []string{"usb-tablet,bus=existing.0", "virtio-tablet-pci", "virtio-tablet-device"} {
		arguments := []string{"-device", device}
		result, err := nativePointerArguments(arguments, "auto")
		if err != nil || !reflect.DeepEqual(result, arguments) {
			t.Fatalf("existing tablet was changed: %v", err)
		}
		if _, err := nativePointerArguments(arguments, "relative"); err == nil {
			t.Fatal("relative mode silently retained an absolute tablet")
		}
	}
	original := []string{"-machine", "pc"}
	result, err := nativePointerArguments(original, "relative")
	if err != nil || !reflect.DeepEqual(result, original) {
		t.Fatalf("relative mode changed guest hardware: %v", err)
	}
	if _, err := nativePointerArguments(original, "invalid"); err == nil {
		t.Fatal("invalid pointer mode accepted")
	}
	if _, err := nativePointerArguments([]string{"-device", "qemu-xhci,id=dockpipe-native-pointer-usb"}, "auto"); err == nil {
		t.Fatal("reserved controller collision accepted")
	}
}
