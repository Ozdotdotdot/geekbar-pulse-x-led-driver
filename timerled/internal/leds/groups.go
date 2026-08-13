// Package leds mirrors the small subset of include/led_groups.h and the
// SEVEN_SEG_BITS table from src/main.cpp that the daemon needs client-side
// to build "U <i1> <b1> ..." payloads -- everything else (constellations,
// ring, stray stars) is referenced by name and rendered entirely on-device.
// Keep this in sync with led_groups.h/main.cpp if the physical mapping or
// digit segment layout ever changes.
package leds

// The center X icon: four 3-LED arms. See led_groups.h for how this was
// confirmed against led_map.json centroids.
var XIcon = []uint16{16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27}

// Seven-segment digit slots, ordered [top, upperLeft, upperRight, middle,
// lowerLeft, lowerRight, bottom] -- identical to DIGIT_TOP_A/B and
// DIGIT_BOTTOM_A/B in led_groups.h.
var (
	DigitTopA    = [7]uint16{0, 5, 1, 6, 4, 2, 3}
	DigitTopB    = [7]uint16{8, 13, 9, 14, 12, 10, 11}
	DigitBottomA = [7]uint16{40, 45, 41, 46, 44, 42, 43}
	DigitBottomB = [7]uint16{48, 53, 49, 54, 52, 50, 51}
)

// Standard 7-segment truth table, one bit per position in a digit slot
// array (bit 0 = top .. bit 6 = bottom) -- identical to SEVEN_SEG_BITS in
// main.cpp.
var SevenSegBits = [10]uint8{
	119, // 0
	36,  // 1
	93,  // 2
	109, // 3
	46,  // 4
	107, // 5
	123, // 6
	37,  // 7
	127, // 8
	111, // 9
}

// DigitPixels returns the (index, brightness) pairs to light the given
// digit (0-9) in the given slot at the given brightness. Pass digit < 0 to
// get the pairs that turn every segment in the slot off instead (index,0),
// useful for clearing a slot without touching anything else via "U".
func DigitPixels(slot [7]uint16, digit int, brightness uint8) (indices []uint16, brightnesses []uint8) {
	indices = make([]uint16, 0, 7)
	brightnesses = make([]uint8, 0, 7)
	var bits uint8
	if digit >= 0 && digit <= 9 {
		bits = SevenSegBits[digit]
	}
	for i := range 7 {
		indices = append(indices, slot[i])
		if bits&(1<<uint(i)) != 0 {
			brightnesses = append(brightnesses, brightness)
		} else {
			brightnesses = append(brightnesses, 0)
		}
	}
	return indices, brightnesses
}
